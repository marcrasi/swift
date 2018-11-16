//===--- AutoDiff.h - Swift Automatic Differentiation ---------------------===//
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
//
//  SWIFT_ENABLE_TENSORFLOW
//  This file defines AST support for automatic differentiation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_AUTODIFF_H
#define SWIFT_AST_AUTODIFF_H

#include "ASTContext.h"
#include "llvm/ADT/SmallBitVector.h"

namespace swift {

enum class AutoDiffMode {
  Forward, Reverse
};

struct AutoDiffIndexParameter {
  SourceLoc loc;
  unsigned index;
};

class AutoDiffParameter {
public:
  enum class Kind { Index, Self };

private:
  SourceLoc Loc;
  Kind Kind;
  union Value {
    struct { unsigned Index; }; // Index
    struct {};                  // Self
    Value(unsigned index) : Index(index) {}
    Value() {}
  } V;

public:
  AutoDiffParameter(SourceLoc loc, enum Kind kind, Value value)
    : Loc(loc), Kind(kind), V(value) {}

  static AutoDiffParameter getIndexParameter(SourceLoc loc, unsigned index) {
    return { loc, Kind::Index, index };
  }

  static AutoDiffParameter getSelfParameter(SourceLoc loc) {
    return { loc, Kind::Self, {} };
  }

  unsigned getIndex() const {
    assert(Kind == Kind::Index);
    return V.Index;
  }

  enum Kind getKind() const {
    return Kind;
  }

  SourceLoc getLoc() const {
    return Loc;
  }

  bool isEqual(const AutoDiffParameter &other) const {
    if (getKind() == other.getKind() && getKind() == Kind::Index)
      return getIndex() == other.getIndex();
    return getKind() == other.getKind() && getKind() == Kind::Self;
  }
};

class AnyFunctionType;
class Type;

/// Identifies a subset of a function's parameters.
///
/// Works with AST-level function decls and types. Requires further lowering to
/// work with SIL-level functions and types. (In particular, tuples must be
/// exploded).
class AutoDiffParameterIndices {
  /// Bits corresponding to parameters in the set are "on", and bits
  /// corresponding to parameters not in the set are "off".
  ///
  /// When the function is curried, bits corresponding to all parameter groups
  /// are flattened together in reverse order. For example,
  ///
  ///   Function type: (A, B) -> (C, D) -> R
  ///   Bits: [C][D][A][B]
  ///
  ///   Function type: (Self) -> (A, B) -> R
  ///   Bits: [A][B][Self]
  ///
  llvm::SmallBitVector indices;

  AutoDiffParameterIndices(unsigned indicesSize) : indices(indicesSize) {}

public:
  /// Allocates and initializes an empty `AutoDiffParameterIndices` for the
  /// given `functionType`.
  static AutoDiffParameterIndices *create(ASTContext &C,
                                          AnyFunctionType *functionType);

  /// Pushes all of `functionType`'s parameters into `paramTypes` in the same
  /// order in which they appear in the bitvector. If `functionType` is curried,
  /// includes parameters from all parameter groups. For example, if
  ///
  ///   functionType = (A, B) -> (C, D, E) -> R
  ///
  /// then pushes {C, D, E, A, B} to `paramTypes`.
  ///
  static void getAllParamTypesInBitOrder(const AnyFunctionType *functionType,
                                         SmallVectorImpl<Type> &paramTypes);

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
  void setParamsInGroup(const AnyFunctionType *functionType, unsigned groupIndex,
                        const SmallVectorImpl<unsigned> &paramIndicesInGroup);

  /// Given a `functionType` and a `groupIndex` that indexes a parameter group
  /// in that function type, adds all the parameters from that group to the set.
  /// For example, if
  ///
  ///   functionType = (A, B) -> (C, D, E) -> R
  ///   groupIndex = 0
  ///
  /// then adds "A" and "B" to the set.
  ///
  void setAllParamsInGroup(const AnyFunctionType *functionType, unsigned groupIndex);

  /// Pushes all of the parameters that are in the set to `paramTypes`, in the
  /// same order in which they appear in the AST function signature. For
  /// example, if
  ///
  ///   functionType = (A, B) -> (C, D, E) -> R
  ///   and if "A", "D", and "E" are in the set
  ///
  /// then pushes {A, D, E} to `paramTypes`.
  ///
  void getSubsetParamTypesInParamGroupOrder(
      const AnyFunctionType *functionType,
      SmallVectorImpl<Type> &paramTypes) const;

  const llvm::SmallBitVector &getIndices() const { return indices; }
};

/// Differentiability of a function specifies the differentiation mode,
/// parameter indices at which the function is differentiable with respect to,
/// and indices of results which can be differentiated.
class Differentiability {
private:
  // The differentiation mode.
  AutoDiffMode mode;
  // Differentiable with respect to `self`, applicable to methods only.
  bool wrtSelf;
  // Indices of parameters that are differentiable with respect to.
  llvm::SmallBitVector parameterIndices;
  // Indices of results that are differentiable.
  llvm::SmallBitVector resultIndices;

public:
  Differentiability(AutoDiffMode mode,
                    bool wrtSelf,
                    llvm::SmallBitVector parameterIndices,
                    llvm::SmallBitVector resultIndices);

  Differentiability(AutoDiffMode mode, AnyFunctionType *type);

  AutoDiffMode getMode() const {
    return mode;
  }

  bool isWithRespectToSelf() const {
    return wrtSelf;
  }

  const llvm::SmallBitVector &getParameterIndices() const {
    return parameterIndices;
  }

  const llvm::SmallBitVector &getResultIndices() const {
    return resultIndices;
  }
};

/// SIL-level automatic differentiation indices. Consists of a source index,
/// i.e. index of the dependent result to differentiate from, and parameter
/// indices, i.e. index of independent parameters to differentiate with
/// respect to.
struct SILAutoDiffIndices {
  /// The index of the dependent result to differentiate from.
  unsigned source;
  /// Indices of independent parameters to differentiate with respect to.
  llvm::SmallBitVector parameters;

  /// Creates a set of AD indices from the given source index and a bit vector
  /// representing parameter indices.
  /*implicit*/ SILAutoDiffIndices(unsigned source,
                                  llvm::SmallBitVector parameters)
      : source(source), parameters(parameters) {}

  /// Creates a set of AD indices from the given source index and an array of
  /// parameter indices. Elements in `parameters` must be acending integers.
  /*implicit*/ SILAutoDiffIndices(unsigned source,
                                  ArrayRef<unsigned> parameters);

  bool operator==(const SILAutoDiffIndices &other) const;

  /// Queries whether the function's parameter with index `parameterIndex` is
  /// one of the parameters to differentiate with respect to.
  bool isWrtParameter(unsigned parameterIndex) const {
    return parameterIndex < parameters.size() &&
           parameters.test(parameterIndex);
  }

  void print(llvm::raw_ostream &s = llvm::outs()) const {
    s << "(source=" << source << " parameters=(";
    interleave(parameters.set_bits(),
               [&s](unsigned p) { s << p; }, [&s]{ s << ' '; });
    s << "))";
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &s,
                                     const SILAutoDiffIndices &indices) {
  indices.print(s);
  return s;
}

/// Flags to define the semantics and the type signature of a gradient function.
enum class SILGradientFlags : unsigned {
  /// The gradient function is seedable, i.e. able to take a back-propagated
  /// adjoint value as the last parameter.
  Seedable = 1 << 0,
  
  /// The gradient function is preserving the result of the original function.
  PreservingResult = 1 << 1,
  
  /// The adjoint computation is "delayed". We say that the adjoint computation
  /// is delayed when when it's returned as a thunk.
  Delayed = 1 << 2
};
using SILGradientOptions = OptionSet<SILGradientFlags>;
static inline SILGradientOptions operator|(SILGradientFlags lhs,
                                           SILGradientFlags rhs) {
  return SILGradientOptions(unsigned(lhs) | unsigned(rhs));
}

/// SIL-level automatic differentiation configuration.
struct SILAutoDiffConfig {
  SILAutoDiffIndices indices;
  SILGradientOptions options;

  /*implicit*/
  SILAutoDiffConfig(const SILAutoDiffIndices &indices,
                    SILGradientOptions options)
    : indices(indices), options(options) {}

  /*implicit*/
  SILAutoDiffConfig(const SILAutoDiffIndices &indices,
                    bool seedable, bool preservingResult)
    : SILAutoDiffConfig(indices, getCanonicalGradientOptions()) {}

  unsigned getSourceIndex() const {
    return indices.source;
  }

  llvm::SmallBitVector getParameterIndices() const {
    return indices.parameters;
  }

  bool isSeedable() const {
    return options.contains(SILGradientFlags::Seedable);
  }

  bool isPreservingResult() const {
    return options.contains(SILGradientFlags::PreservingResult);
  }

  bool isDelayed() const {
    return options.contains(SILGradientFlags::Delayed);
  }

  // FIXME: The master configuration should have all three gradient options
  // enabled, that is, the canonical gradient should return a delayed gradient
  // function. We need to handle this here as well as within the
  // differentiation pass.
  static SILGradientOptions getCanonicalGradientOptions() {
    return SILGradientFlags::Seedable | SILGradientFlags::PreservingResult;
  }

  /// Returns the "master" configuration, which all variants with the same
  /// parameter indices can derive from.
  static SILAutoDiffConfig getMaster(const SILAutoDiffIndices &indices) {
    return {
      indices,
      getCanonicalGradientOptions()
    };
  }

  SILAutoDiffConfig getWithCanonicalOptions() const {
    return getMaster(indices);
  }

  bool isMaster() const {
    return options.toRaw() == getCanonicalGradientOptions().toRaw();
  }

  bool operator==(const SILAutoDiffConfig &other) const {
    return indices == other.indices &&
           options.toRaw() == other.options.toRaw();
  }
};

/// The kind of an associated function in the `autodiff_function` and
/// `autodiff_function_extract` instructions in SIL.
struct AutoDiffAssociatedFunctionKind {
  enum innerty : uint8_t {
     // The vector-Jacobian products operator.
     JVP = 0,
     // The Jacobian-vector products operator.
     VJP = 1
  } rawValue;

  AutoDiffAssociatedFunctionKind() = default;
  AutoDiffAssociatedFunctionKind(innerty rawValue) : rawValue(rawValue) {}
  explicit AutoDiffAssociatedFunctionKind(StringRef string);
  operator innerty() const { return rawValue; }
};

/// Automatic differentiation utility namespace.
namespace autodiff {

/// Returns the offset for an associated function at a specific differentiation
/// order.
/// This is used for both ordering in the `autodiff_function` instruction and
/// ABI layout.
///
///                Order 1       Order 2     ...
/// |----------| |-----|-----| |-----|-----| ...
/// | Original | | JVP | VJP | | JVP | VJP | ...
/// |----------| |-----|-----| |-----|-----| ...
unsigned
getOffsetForAutoDiffAssociatedFunction(unsigned order,
                                       AutoDiffAssociatedFunctionKind kind);

} // end namespace autodiff

class BuiltinFloatType;
class NominalTypeDecl;
class StructDecl;
class TupleType;
class EnumDecl;

/// A type that represents the tangent space of a differentiable type.
class TangentSpace {
public:
  /// A tangent space kind.
  enum class Kind {
    /// `Builtin.FP<...>`.
    BuiltinRealScalar,
    /// A type that conforms to `FloatingPoint`.
    RealScalar,
    /// A type that conforms to `VectorNumeric` where the associated
    /// `ScalarElement` conforms to `FloatingPoint`.
    RealVector,
    /// A product of tangent spaces as a struct.
    ProductStruct,
    /// A product of tangent spaces as a tuple.
    ProductTuple,
    /// A sum of tangent spaces.
    Sum
  };

private:
  Kind kind;
  union Value {
    // BuiltinRealScalar
    BuiltinFloatType *builtinFPType;
    // RealScalar or RealVector
    NominalTypeDecl *realNominalType;
    // ProductStruct
    StructDecl *structDecl;
    // ProductTuple
    TupleType *tupleType;
    // Sum
    EnumDecl *enumDecl;

    Value(BuiltinFloatType *builtinFP) : builtinFPType(builtinFP) {}
    Value(NominalTypeDecl *nominal) : realNominalType(nominal) {}
    Value(StructDecl *structDecl) : structDecl(structDecl) {}
    Value(TupleType *tupleType) : tupleType(tupleType) {}
    Value(EnumDecl *enumDecl) : enumDecl(enumDecl) {}
  } value;

  TangentSpace(Kind kind, Value value)
      : kind(kind), value(value) {}

public:
  TangentSpace() = delete;

  static TangentSpace
  getBuiltinRealScalarSpace(BuiltinFloatType *builtinFP) {
    return {Kind::BuiltinRealScalar, builtinFP};
  }
  static TangentSpace getRealScalarSpace(NominalTypeDecl *typeDecl) {
    return {Kind::RealScalar, typeDecl};
  }
  static TangentSpace getRealVectorSpace(NominalTypeDecl *typeDecl) {
    return {Kind::RealVector, typeDecl};
  }
  static TangentSpace getProductStruct(StructDecl *structDecl) {
    return {Kind::ProductStruct, structDecl};
  }
  static TangentSpace getProductTuple(TupleType *tupleTy) {
    return {Kind::ProductTuple, tupleTy};
  }
  static TangentSpace getSum(EnumDecl *enumDecl) {
    return {Kind::Sum, enumDecl};
  }

  bool isBuiltinRealScalarSpace() const {
    return kind == Kind::BuiltinRealScalar;
  }
  bool isRealScalarSpace() const { return kind == Kind::RealScalar; }
  bool isRealVectorSpace() const { return kind == Kind::RealVector; }
  bool isProductStruct() const { return kind == Kind::ProductStruct; }
  bool isProductTuple() const { return kind == Kind::ProductTuple; }

  Kind getKind() const { return kind; }
  BuiltinFloatType *getBuiltinRealScalarSpace() const {
    assert(kind == Kind::BuiltinRealScalar);
    return value.builtinFPType;
  }
  NominalTypeDecl *getRealScalarSpace() const {
    assert(kind == Kind::RealScalar);
    return value.realNominalType;
  }
  NominalTypeDecl *getRealVectorSpace() const {
    assert(kind == Kind::RealVector);
    return value.realNominalType;
  }
  NominalTypeDecl *getRealScalarOrVectorSpace() const {
    assert(kind == Kind::RealScalar || kind == Kind::RealVector);
    return value.realNominalType;
  }
  StructDecl *getProductStruct() const {
    assert(kind == Kind::ProductStruct);
    return value.structDecl;
  }
  TupleType *getProductTuple() const {
    assert(kind == Kind::ProductTuple);
    return value.tupleType;
  }
  EnumDecl *getSum() const {
    assert(kind == Kind::Sum);
    return value.enumDecl;
  }
};

} // end namespace swift

namespace llvm {

using swift::SILAutoDiffIndices;
using swift::SILAutoDiffConfig;
using swift::SILGradientFlags;
using swift::OptionSet;

template<typename T> struct DenseMapInfo;

template<> struct DenseMapInfo<SILAutoDiffIndices> {
  static SILAutoDiffIndices getEmptyKey() {
    return { DenseMapInfo<unsigned>::getEmptyKey(), SmallBitVector() };
  }

  static SILAutoDiffIndices getTombstoneKey() {
    return { DenseMapInfo<unsigned>::getTombstoneKey(),
             SmallBitVector(sizeof(intptr_t), true) };
  }

  static unsigned getHashValue(const SILAutoDiffIndices &Val) {
    auto params = Val.parameters.set_bits();
    unsigned combinedHash =
      hash_combine(~1U, DenseMapInfo<unsigned>::getHashValue(Val.source),
                   hash_combine_range(params.begin(), params.end()));
    return combinedHash;
  }

  static bool isEqual(const SILAutoDiffIndices &LHS,
                      const SILAutoDiffIndices &RHS) {
    return LHS == RHS;
  }
};

template<> struct DenseMapInfo<SILAutoDiffConfig> {
  static SILAutoDiffConfig getEmptyKey() {
    return { DenseMapInfo<SILAutoDiffIndices>::getEmptyKey(), None };
  }

  static SILAutoDiffConfig getTombstoneKey() {
    return {
      DenseMapInfo<SILAutoDiffIndices>::getTombstoneKey(),
      SILGradientFlags::Delayed
    };
  }

  static unsigned getHashValue(const SILAutoDiffConfig &Val) {
    return hash_combine(
      DenseMapInfo<SILAutoDiffIndices>::getHashValue(Val.indices),
      DenseMapInfo<unsigned>::getHashValue(Val.options.toRaw())
    );
  }

  static bool isEqual(const SILAutoDiffConfig &LHS,
                      const SILAutoDiffConfig &RHS) {
    return DenseMapInfo<SILAutoDiffIndices>
             ::isEqual(LHS.indices, RHS.indices) &&
           LHS.options.toRaw() == RHS.options.toRaw();
  }
};

} // end namespace llvm

#endif // SWIFT_AST_AUTODIFF_H
