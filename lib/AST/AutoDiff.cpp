//===-------- AutoDiff.cpp - Routines for USR generation ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/AutoDiff.h"
#include "swift/AST/Types.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"

using namespace swift;

SILAutoDiffIndices::SILAutoDiffIndices(
    unsigned source, ArrayRef<unsigned> parameters) : source(source) {
  if (parameters.empty())
    return;

  auto max = *std::max_element(parameters.begin(), parameters.end());
  this->parameters.resize(max + 1);
  int last = -1;
  for (auto paramIdx : parameters) {
    assert((int)paramIdx > last && "Parameter indices must be ascending");
    last = paramIdx;
    this->parameters.set(paramIdx);
  }
}

bool SILAutoDiffIndices::operator==(
    const SILAutoDiffIndices &other) const {
  if (source != other.source)
    return false;

  // The parameters are the same when they have exactly the same set bit
  // indices, even if they have different sizes.
  llvm::SmallBitVector buffer(std::max(parameters.size(),
                                       other.parameters.size()));
  buffer ^= parameters;
  buffer ^= other.parameters;
  return buffer.none();
}

AutoDiffAssociatedFunctionKind::
AutoDiffAssociatedFunctionKind(StringRef string) {
  Optional<innerty> result =
      llvm::StringSwitch<Optional<innerty>>(string)
         .Case("jvp", JVP).Case("vjp", VJP);
  assert(result && "Invalid string");
  rawValue = *result;
}

Differentiability::Differentiability(AutoDiffMode mode,
                                     bool wrtSelf,
                                     llvm::SmallBitVector parameterIndices,
                                     llvm::SmallBitVector resultIndices)
    : mode(mode), wrtSelf(wrtSelf), parameterIndices(parameterIndices),
      resultIndices(resultIndices) {
}

Differentiability::Differentiability(AutoDiffMode mode,
                                     AnyFunctionType *type)
    : mode(mode), wrtSelf(type->getExtInfo().hasSelfParam()),
      // For now, we assume exactly one result until we figure out how to
      // model result selection.
      resultIndices(1) {
  // If function has self, it must be a curried method type.
  if (wrtSelf) {
    auto methodTy = type->getResult()->castTo<AnyFunctionType>();
    parameterIndices = llvm::SmallBitVector(methodTy->getNumParams());
  } else {
    parameterIndices = llvm::SmallBitVector(type->getNumParams());
  }
  parameterIndices.set();
  resultIndices.set();
}

unsigned autodiff::getOffsetForAutoDiffAssociatedFunction(
    unsigned order, AutoDiffAssociatedFunctionKind kind) {
  return (order - 1) * 2 + kind.rawValue;
}

/// Computes the number of parameters in uncurried `functionType`. For example,
///   computeUncurriedNumParams( "(A) -> (B) -> R" ) = 2
static unsigned computeUncurriedNumParams(const AnyFunctionType *functionType) {
  unsigned numParams = 0;
  while (functionType) {
    numParams += functionType->getNumParams();
    functionType = functionType->getResult()->getAs<AnyFunctionType>();
  }
  return numParams;
}

/// Unwraps `unwrapLevel` of curry levels in `functionType`. For example,
///   unwrapCurried( "(A) -> (B) -> R", 1 ) = "(B) -> R"
static const AnyFunctionType *unwrapCurried(
    const AnyFunctionType *functionType, unsigned unwrapLevel) {
  while (unwrapLevel > 0) {
    functionType = functionType->getResult()->castTo<AnyFunctionType>();
    --unwrapLevel;
  }
  return functionType;
}

/// Allocates and initializes an empty `AutoDiffParameterIndices` for the
/// given `functionType`.
AutoDiffParameterIndices
*AutoDiffParameterIndices::create(ASTContext &C,
                                  AnyFunctionType *functionType) {
  // TODO(SR-9290): Note that the AutoDiffParameterIndices' destructor never
  // gets called, which causes a small memory leak in the case that the
  // SmallBitVector decides to allocate some heap space.
  void *mem = C.Allocate(sizeof(AutoDiffParameterIndices),
                         alignof(AutoDiffParameterIndices));
  return ::new (mem)
      AutoDiffParameterIndices(computeUncurriedNumParams(functionType));
}

/// Pushes all of `functionType`'s parameters into `paramTypes` in the same
/// order in which they appear in the bitvector. If `functionType` is curried,
/// includes parameters from all parameter groups. For example, if
///
///   functionType = (A, B) -> (C, D, E) -> R
///
/// then pushes {C, D, E, A, B} to `paramTypes`.
///
void AutoDiffParameterIndices::getAllParamTypesInBitOrder(
    const AnyFunctionType *functionType, SmallVectorImpl<Type> &paramTypes) {
  // Get function types for each parameter group.
  SmallVector<const AnyFunctionType*, 2> functionTypes;
  while (functionType) {
    functionTypes.push_back(functionType);
    functionType = functionType->getResult()->getAs<AnyFunctionType>();
  }

  // Flatten the param types into the result vector.
  for (unsigned i : reversed(range(0, functionTypes.size())))
    for (auto param : functionTypes[i]->getParams())
      paramTypes.push_back(param.getPlainType());
}

/// Given a `functionType`, a `groupIndex` that indexes a parameter group in
/// that function type, and given a vector of `paramIndicesInGroup` that index
/// parameters within that group, adds the indexed parameters to the set. For
/// example, if
///
///   functionType = (A, B) -> (C, D, E) -> R
///   groupIndex = 1
///   paramIndicesInGroup = {0, 2}
///
/// then adds "C" and "E" to the set.
///
void AutoDiffParameterIndices::setParamsInGroup(
    const AnyFunctionType *functionType, unsigned groupIndex,
    const SmallVectorImpl<unsigned> &paramIndicesInGroup) {
  const AnyFunctionType *groupFunctionType =
      unwrapCurried(functionType, groupIndex);
  unsigned firstBitForGroup = computeUncurriedNumParams(
      groupFunctionType->getResult()->getAs<AnyFunctionType>());
  for (unsigned paramIndexInGroup : paramIndicesInGroup)
    indices.set(firstBitForGroup + paramIndexInGroup);
}

/// Given a `functionType` and a `groupIndex` that indexes a parameter group
/// in that function type, adds all the parameters from that group to the set.
/// For example, if
///
///   functionType = (A, B) -> (C, D, E) -> R
///   groupIndex = 0
///
/// then adds "A" and "B" to the set.
///
void AutoDiffParameterIndices::setAllParamsInGroup(
    const AnyFunctionType *functionType, unsigned groupIndex) {
  const AnyFunctionType *groupFunctionType =
      unwrapCurried(functionType, groupIndex);
  unsigned firstBitForGroup = computeUncurriedNumParams(
      groupFunctionType->getResult()->getAs<AnyFunctionType>());
  for (unsigned paramIndexInGroup : range(groupFunctionType->getNumParams()))
    indices.set(firstBitForGroup + paramIndexInGroup);
}

/// Pushes all of the parameters that are in the set to `paramTypes`, in the
/// same order in which they appear in the AST function signature. For
/// example, if
///
///   functionType = (A, B) -> (C, D, E) -> R
///   and if "A", "D", and "E" are in the set
///
/// then pushes {A, D, E} to `paramTypes`.
///
void AutoDiffParameterIndices::getSubsetParamTypesInParamGroupOrder(
    const AnyFunctionType *functionType,
    SmallVectorImpl<Type> &paramTypes) const {
  // Get function types for each parameter group.
  SmallVector<const AnyFunctionType*, 2> functionTypes;
  while (functionType) {
    functionTypes.push_back(functionType);
    functionType = functionType->getResult()->getAs<AnyFunctionType>();
  }

  // Compute the first bit index for each param group.
  SmallVector<unsigned, 8> paramGroupFirstBitIndexes(functionTypes.size());
  unsigned currentBitIndex = 0;
  for (unsigned paramGroupIndex : reversed(range(0, functionTypes.size()))) {
    paramGroupFirstBitIndexes[paramGroupIndex] = currentBitIndex;
    currentBitIndex += functionTypes[paramGroupIndex]->getNumParams();
  }

  // Flatten the param types into the result vector.
  for (unsigned paramGroupIndex : range(functionTypes.size())) {
    const AnyFunctionType *functionType = functionTypes[paramGroupIndex];
    unsigned paramGroupFirstBitIndex =
        paramGroupFirstBitIndexes[paramGroupIndex];
    for (unsigned paramIndex : range(functionType->getNumParams()))
      if (indices[paramGroupFirstBitIndex + paramIndex])
        paramTypes.push_back(
            functionType->getParams()[paramIndex].getPlainType());
  }
}
