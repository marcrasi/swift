//===--- GenTuple.cpp - Swift IR Generation For Tuple Types ---------------===//
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
//  This file implements IR generation for tuple types in Swift.  This
//  includes creating the IR type as well as emitting the primitive access
//  operations.
//
//  It is assumed in several places in IR-generation that the
//  explosion schema of a tuple type is always equal to the appended
//  explosion schemas of the component types.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Pattern.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "llvm/IR/DerivedTypes.h"

#include "GenHeap.h"
#include "GenRecord.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "Explosion.h"
#include "IndirectTypeInfo.h"
#include "NonFixedTypeInfo.h"

#include "GenDiffFunc.h"

#pragma clang diagnostic ignored "-Winconsistent-missing-override"

using namespace swift;
using namespace irgen;

namespace {
  class DiffFuncFieldInfo : public RecordField<DiffFuncFieldInfo> {
  public:
    DiffFuncFieldInfo(unsigned index, const TypeInfo &type)
      : RecordField(type), Index(index), Name(name)
    {}

    /// The field index.
    const unsigned Index;

    StringRef getFieldName() const {
      return Name;
    }

    SILType getType(IRGenModule& IGM, SILType t) const {
      auto extInfo = fnTy->getExtInfo();
      auto nondiffExtInfo = extInfo.withDifferentiable(false);
      auto origTy = fnTy->getWithExtInfo(nondiffExtInfo);
      if (Index == 0)
        return SILType::getPrimitiveObjectType(origTy);
      unsigned differentiationOrder = (Index - 1) / 2 + 1;
      auto kind = (Index - 1) % 2 == 0 ? AutoDiffAssociatedFunctionKind::JVP
                                       : AutoDiffAssociatedFunctionKind::VJP;
      auto assocTy = origTy->getAutoDiffAssociatedFunctionType(
          SmallBitVector(origTy->getNumParameters(), true), /*resultIndex*/ 0,
          differentiationOrder, kind, IGM.getSILModule(),
          LookUpConformanceInModule(IGM.getSwiftModule()));
      return SILType::getPrimitiveObjectType(assocTy);
    }
  };

  template <class Impl, class Base>
  class TupleTypeInfoBase
      : public RecordTypeInfo<TupleTypeInfoBase, LoadableTypeInfo, DiffFuncFieldInfo> {
    using super = RecordTypeInfo<Impl, Base, DiffFuncFieldInfo>;

  public:

    LoadableTupleTypeInfo(ArrayRef<DiffFuncFieldInfo> fields,
                          unsigned explosionSize,
                          llvm::Type *ty,
                          Size size, SpareBitVector &&spareBits,
                          Alignment align, IsPOD_t isPOD,
                          IsFixedSize_t alwaysFixedSize)
      : super(fields, explosionSize,
                          ty, size, std::move(spareBits), align, isPOD,
                          alwaysFixedSize)
      {}

    void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                          Size offset) const override {
      for (auto &field : getFields()) {
        auto fieldOffset = offset + field.getFixedByteOffset();
        cast<LoadableTypeInfo>(field.getTypeInfo())
          .addToAggLowering(IGM, lowering, fieldOffset);
      }
    }

    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF) const {
      return None;
    }
    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF, SILType T) const {
      return None;
    }

    /// Given a full tuple explosion, project out a single element.
    void projectElementFromExplosion(IRGenFunction &IGF,
                                     Explosion &tuple,
                                     unsigned fieldNo,
                                     Explosion &out) const {
      const DiffFuncFieldInfo &field = asImpl().getFields()[fieldNo];

      // If the field requires no storage, there's nothing to do.
      if (field.isEmpty())
        return IGF.emitFakeExplosion(field.getTypeInfo(), out);
  
      // Otherwise, project from the base.
      auto fieldRange = field.getProjectionRange();
      ArrayRef<llvm::Value *> element = tuple.getRange(fieldRange.first,
                                                       fieldRange.second);
      out.add(element);
    }

    /// Given the address of a tuple, project out the address of a
    /// single element.
    Address projectFieldAddress(IRGenFunction &IGF,
                                Address addr,
                                SILType T,
                                const DiffFuncFieldInfo &field) const {
      return asImpl().projectElementAddress(IGF, addr, T, field.Index);
    }

    /// Given the address of a tuple, project out the address of a
    /// single element.
    Address projectElementAddress(IRGenFunction &IGF,
                                  Address tuple,
                                  SILType T,
                                  unsigned fieldNo) const {
      const DiffFuncFieldInfo &field = asImpl().getFields()[fieldNo];
      if (field.isEmpty())
        return field.getTypeInfo().getUndefAddress();

      auto offsets = asImpl().getNonFixedOffsets(IGF, T);
      return field.projectAddress(IGF, tuple, offsets);
    }

    /// Return the statically-known offset of the given element.
    Optional<Size> getFixedElementOffset(IRGenModule &IGM,
                                         unsigned fieldNo) const {
      const DiffFuncFieldInfo &field = asImpl().getFields()[fieldNo];
      switch (field.getKind()) {
      case ElementLayout::Kind::Empty:
      case ElementLayout::Kind::Fixed:
        return field.getFixedByteOffset();
      case ElementLayout::Kind::InitialNonFixedSize:
        return Size(0);
      case ElementLayout::Kind::NonFixed:
        return None;
      }
      llvm_unreachable("bad element layout kind");
    }

    Optional<unsigned> getElementStructIndex(IRGenModule &IGM,
                                             unsigned fieldNo) const {
      const DiffFuncFieldInfo &field = asImpl().getFields()[fieldNo];
      if (field.isEmpty())
        return None;
      return field.getStructIndex();
    }

    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address src, SILType T,
                              bool isOutlined) const override {
      llvm_unreachable("unexploded tuple as argument?");
    }
    
    void verify(IRGenTypeVerifierFunction &IGF,
                llvm::Value *metadata,
                SILType tupleType) const override {
      auto fields = asImpl().getFields();
      for (unsigned i : indices(fields)) {
        const DiffFuncFieldInfo &field = fields[i];
        switch (field.getKind()) {
        case ElementLayout::Kind::Fixed: {
          // Check that the fixed layout matches the layout in the tuple
          // metadata.
          auto fixedOffset = field.getFixedByteOffset();
          
          auto runtimeOffset = loadTupleOffsetFromMetadata(IGF, metadata, i);

          IGF.verifyValues(metadata, runtimeOffset,
                     IGF.IGM.getSize(fixedOffset),
                     llvm::Twine("offset of tuple element ") + llvm::Twine(i));
          break;
        }
        
        case ElementLayout::Kind::Empty:
        case ElementLayout::Kind::InitialNonFixedSize:
        case ElementLayout::Kind::NonFixed:
          continue;
        }
      }
    }
  };

  class TupleTypeBuilder :
      public RecordTypeBuilder<TupleTypeBuilder, DiffFuncFieldInfo,
                               unsigned> {

    SILFunctionType *origFnTy;

  public:
    TupleTypeBuilder(IRGenModule &IGM, SILFunctionType *fnTy)
      : RecordTypeBuilder(IGM) {
      assert(fnTy->isDifferentiable());
      auto extInfo = fnTy->getExtInfo();
      auto nondiffExtInfo = extInfo.withDifferentiable(false);
      origFnTy = fnTy->getWithExtInfo(nondiffExtInfo);
    }

    FixedTupleTypeInfo *createFixed(ArrayRef<DiffFuncFieldInfo> fields,
                                    StructLayout &&layout) {
      llvm_unreachable("@autodiff functions are always loadable");
    }

    LoadableTupleTypeInfo *createLoadable(ArrayRef<DiffFuncFieldInfo> fields,
                                          StructLayout &&layout,
                                          unsigned explosionSize) {
      return LoadableTupleTypeInfo::create(fields, explosionSize,
                                           layout.getType(), layout.getSize(),
                                           std::move(layout.getSpareBits()),
                                           layout.getAlignment(),
                                           layout.isPOD(),
                                           layout.isAlwaysFixedSize());
    }

    NonFixedTupleTypeInfo *createNonFixed(ArrayRef<DiffFuncFieldInfo> fields,
                                     FieldsAreABIAccessible_t fieldsAccessible,
                                          StructLayout &&layout) {
      llvm_unreachable("@autodiff functions are always loadable");
    }

    DiffFuncFieldInfo getFieldInfo(unsigned index,
                                const TupleTypeElt &field, // TODO!!WOOHOO!
                                const TypeInfo &fieldTI) {
      return DiffFuncFieldInfo(index, name, fieldTI);
    }

    SILType getType(unsigned index) {
      if (Index == 0)
        return SILType::getPrimitiveObjectType(origTy);
      unsigned differentiationOrder = (Index - 1) / 2 + 1;
      auto kind = (Index - 1) % 2 == 0 ? AutoDiffAssociatedFunctionKind::JVP
                                       : AutoDiffAssociatedFunctionKind::VJP;
      auto assocTy = origTy->getAutoDiffAssociatedFunctionType(
          SmallBitVector(origTy->getNumParameters(), true), /*resultIndex*/ 0,
          differentiationOrder, kind, IGM.getSILModule(),
          LookUpConformanceInModule(IGM.getSwiftModule()));
      return SILType::getPrimitiveObjectType(assocTy);

      // We know we're working with a lowered type here.
      return SILType::getPrimitiveObjectType(CanType(field.getType()));
    }

    StructLayout performLayout(ArrayRef<const TypeInfo *> fieldTypes) {
      return StructLayout(IGM, /*decl=*/nullptr, LayoutKind::NonHeapObject,
                          LayoutStrategy::Universal, fieldTypes);
    }
  };
} // end anonymous namespace

const TypeInfo *TypeConverter::convertDifferentiableFunctionType(
    SILFunctionType *type) {
  assert(type->isDifferentiable());
  TupleTypeBuilder builder(IGM, type);
  SmallVector<unsigned, 3> fields;
  for (unsigned i : range(3)) // TODO: Correct range size based on differentiability!
    fields.push_back(i);
  return builder.layout(fields);
}
