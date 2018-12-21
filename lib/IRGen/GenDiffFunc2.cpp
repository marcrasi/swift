//===--- GenDiffFunc2.cpp - Swift IR Generation For @autodiff functions --===//
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

#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Pattern.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "llvm/IR/DerivedTypes.h"

#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "Explosion.h"
#include "LoadableTypeInfo.h"
#include "StructLayout.h"

namespace swift {
namespace irgen {

class DifferentiableFunctionTypeInfo final : public LoadableTypeInfo {
private:
  SmallVector<ElementLayout, 3> elementLayouts;

  void forEachElementTypeInfo(
      std::function<void(const LoadableTypeInfo *)> callback) const {
    for (auto &layout : elementLayouts)
      callback(cast<LoadableTypeInfo>(&layout.getType()));
  }

  void forEachElementTypeInfo(
      std::function<void(const LoadableTypeInfo *)> normalCase,
      std::function<void(const LoadableTypeInfo *)> undefCase) const {
    normalCase(cast<LoadableTypeInfo>(&elementLayouts[0].getType()));
    for (unsigned index : range((elementLayouts.size() - 1) / 2)) {
      undefCase(cast<LoadableTypeInfo>(
          &elementLayouts[1 + 2 * index].getType()));
      normalCase(cast<LoadableTypeInfo>(
          &elementLayouts[2 + 2 * index].getType()));
    }
  }

public:
  DifferentiableFunctionTypeInfo(llvm::Type *type, Size size,
                                 const SpareBitVector &spareBits,
                                 Alignment align, IsPOD_t pod,
                                 IsFixedSize_t alwaysFixedSize,
                                 ArrayRef<ElementLayout> elementLayouts)
      : LoadableTypeInfo(type, size, spareBits, align, pod, alwaysFixedSize),
        elementLayouts(elementLayouts.begin(), elementLayouts.end()) {
    assert(elementLayouts.size() >= 3);
    assert(elementLayouts.size() % 2 == 1);
  }

  void getSchema(ExplosionSchema &schema) const {
    forEachElementTypeInfo(
        [&](const LoadableTypeInfo *element) {
        });

    forEachElementTypeInfo(
        [&](const LoadableTypeInfo *element) {
          element->getSchema(schema);
        });
  }

  void assignWithCopy(IRGenFunction &IGF, Address dest, Address src, SILType T,
                      bool isOutlined) const override {
    // TODO: callOutlinedCopy ?

    for (auto &layout : elementLayouts) {
      Address destField = layout.projectAddress(IGF, dest, /*offsets*/None);
      Address srcField = layout.projectAddress(IGF, src, /*offsets*/None);
      layout.getTypeInfo().assignWithTake
    }

   
  }

  unsigned getExplosionSize() const {
    unsigned result = 0;
    forEachElementTypeInfo(
        [&](const LoadableTypeInfo *element) {
          result += element->getExplosionSize();
        });
    return result;
  }

  /// Load an explosion of values from an address as if copy-initializing
  /// a set of registers.
  void loadAsCopy(IRGenFunction &IGF, Address addr,
                  Explosion &explosion) const {
    forEachElementTypeInfo(
        /*normalCase*/ [&](const LoadableTypeInfo *element) {
          element->loadAsCopy(IGF, addr, explosion);
        },
        /*undefCase*/ [&](const LoadableTypeInfo *element) {
          element->loadAsTake(IGF, addr, explosion);
        });
  }

  /// Load an explosion of values from an address as if
  /// take-initializing a set of registers.
  void loadAsTake(IRGenFunction &IGF, Address addr,
                  Explosion &explosion) const {
    forEachElementTypeInfo(
        [&](const LoadableTypeInfo *element) {
          element->loadAsTake(IGF, addr, explosion);
        });
  }

  /// Assign a set of exploded values into an address.  The values are
  /// consumed out of the explosion.
  void assign(IRGenFunction &IGF, Explosion &explosion, Address addr,
                      bool isOutlined) const {
    forEachElementTypeInfo(
        /*normalCase*/ [&](const LoadableTypeInfo *element) {
          element->assign(IGF, explosion, addr, isOutlined);
        },
        /*undefCase*/ [&](const LoadableTypeInfo *element) {
          element->initialize(IGF, explosion, addr, isOutlined);
        });
  }

  /// Initialize an address by consuming values out of an explosion.
  void initialize(IRGenFunction &IGF, Explosion &explosion,
                          Address addr, bool isOutlined) const {
    forEachElementTypeInfo(
        [&](const LoadableTypeInfo *element) {
          element->loadAsTake(IGF, addr, explosion);
          element->initialize(IGF, explosion, addr, isOutlined);
        });
  }

  /// Consume a bunch of values which have exploded at one explosion
  /// level and produce them at another.
  ///
  /// Essentially, this is like take-initializing the new explosion.
  void reexplode(IRGenFunction &IGF, Explosion &sourceExplosion,
                         Explosion &targetExplosion) const {
    forEachElementTypeInfo(
        [&](const LoadableTypeInfo *element) {
          element->reexplode(IGF, sourceExplosion, targetExplosion);
        });
  }

  /// Shift values from the source explosion to the target explosion
  /// as if by copy-initialization.
  void copy(IRGenFunction &IGF, Explosion &sourceExplosion,
                    Explosion &targetExplosion, Atomicity atomicity) const {
    forEachElementTypeInfo(
        /*normalCase*/ [&](const LoadableTypeInfo *element) {
          element->copy(IGF, sourceExplosion, targetExplosion, atomicity);
        },
        /*undefCase*/ [&](const LoadableTypeInfo *element) {
          element->reexplode(IGF, sourceExplosion, targetExplosion);
        });
  }

  /// Release reference counts or other resources owned by the explosion.
  void consume(IRGenFunction &IGF, Explosion &explosion,
                       Atomicity atomicity) const {
    forEachElementTypeInfo(
        /*normalCase*/ [&](const LoadableTypeInfo *element) {
          element->consume(IGF, explosion, atomicity);
        },
        /*undefCase*/ [&](const LoadableTypeInfo *element) {
          explosion.claim(element->getExplosionSize());
        });
  }

  /// Fix the lifetime of the source explosion by creating opaque calls to
  /// swift_fixLifetime for all reference types in the explosion.
  void fixLifetime(IRGenFunction &IGF, Explosion &explosion) const {
    forEachElementTypeInfo(
        /*normalCase*/ [&](const LoadableTypeInfo *element) {
          element->fixLifetime(IGF, explosion);
        },
        /*undefCase*/ [&](const LoadableTypeInfo *element) {
          explosion.claim(element->getExplosionSize());
        });
  }

  /// Pack the source explosion into an enum payload.
  void packIntoEnumPayload(IRGenFunction &IGF,
                                   EnumPayload &payload,
                                   Explosion &sourceExplosion,
                                   unsigned offset) const {
    llvm_unreachable("unimplemented");
  }

  /// Unpack an enum payload containing a valid value of the type into the
  /// destination explosion.
  void unpackFromEnumPayload(IRGenFunction &IGF,
                                     const EnumPayload &payload,
                                     Explosion &targetExplosion,
                                     unsigned offset) const {
    llvm_unreachable("unimplemented");
  }

  /// Add this type to the given aggregate lowering.
  void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                                Size offset) const {
    llvm_unreachable("unimplemented");
  }
};

const TypeInfo *TypeConverter::convertDifferentiableFunctionType(
    SILFunctionType *T) {
  assert(T->isDifferentiable());
  auto extInfo = T->getExtInfo();
  auto nondiffExtInfo = extInfo.withDifferentiable(false);
  auto origTy = T->getWithExtInfo(nondiffExtInfo);
  // TODO: Use the parameter indices and diff order in the @autodiff
  // function type.
  auto jvpTy = origTy->getAutoDiffAssociatedFunctionType(
      SmallBitVector(T->getNumParameters(), true), /*resultIndex*/ 0,
      /*differentiationOrder*/ 1, AutoDiffAssociatedFunctionKind::JVP,
      IGM.getSILModule(), LookUpConformanceInModule(IGM.getSwiftModule()));
  auto vjpTy = origTy->getAutoDiffAssociatedFunctionType(
      SmallBitVector(T->getNumParameters(), true), /*resultIndex*/ 0,
      /*differentiationOrder*/ 1, AutoDiffAssociatedFunctionKind::VJP,
      IGM.getSILModule(), LookUpConformanceInModule(IGM.getSwiftModule()));

  SmallVector<const TypeInfo *, 3> elementTypeInfos;
  elementTypeInfos.push_back(convertFunctionType(origTy));
  elementTypeInfos.push_back(convertFunctionType(jvpTy));
  elementTypeInfos.push_back(convertFunctionType(vjpTy));

  StructLayout layout(IGM, /*decl*/ nullptr, LayoutKind::NonHeapObject,
                      LayoutStrategy::Universal, elementTypeInfos);

  return new DifferentiableFunctionTypeInfo(
      layout.getType(), layout.getSize(), std::move(layout.getSpareBits()),
      layout.getAlignment(), layout.isPOD(), layout.isAlwaysFixedSize(),
      layout.getElements());
}

} // end namespace irgen
} // end namespace swift
