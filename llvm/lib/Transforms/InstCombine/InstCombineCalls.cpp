//===- InstCombineCalls.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the visitCall, visitInvoke, and visitCallBr functions.
//
//===----------------------------------------------------------------------===//

#include "InstCombineInternal.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FloatingPointMode.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumeBundleQueries.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/IntrinsicsARM.h"
#include "llvm/IR/IntrinsicsHexagon.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/AtomicOrdering.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombineWorklist.h"
#include "llvm/Transforms/InstCombine/InstCombiner.h"
#include "llvm/Transforms/Utils/AssumeBundleBuilder.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SimplifyLibCalls.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace llvm;
using namespace PatternMatch;

#define DEBUG_TYPE "instcombine"

STATISTIC(NumSimplified, "Number of library calls simplified");

static cl::opt<unsigned> GuardWideningWindow(
    "instcombine-guard-widening-window",
    cl::init(3),
    cl::desc("How wide an instruction window to bypass looking for "
             "another guard"));

/// enable preservation of attributes in assume like:
/// call void @llvm.assume(i1 true) [ "nonnull"(i32* %PTR) ]
extern cl::opt<bool> EnableKnowledgeRetention;

/// Return the specified type promoted as it would be to pass though a va_arg
/// area.
static Type *getPromotedType(Type *Ty) {
  if (IntegerType* ITy = dyn_cast<IntegerType>(Ty)) {
    if (ITy->getBitWidth() < 32)
      return Type::getInt32Ty(Ty->getContext());
  }
  return Ty;
}

Instruction *InstCombinerImpl::SimplifyAnyMemTransfer(AnyMemTransferInst *MI) {
  Align DstAlign = getKnownAlignment(MI->getRawDest(), DL, MI, &AC, &DT);
  MaybeAlign CopyDstAlign = MI->getDestAlign();
  if (!CopyDstAlign || *CopyDstAlign < DstAlign) {
    MI->setDestAlignment(DstAlign);
    return MI;
  }

  Align SrcAlign = getKnownAlignment(MI->getRawSource(), DL, MI, &AC, &DT);
  MaybeAlign CopySrcAlign = MI->getSourceAlign();
  if (!CopySrcAlign || *CopySrcAlign < SrcAlign) {
    MI->setSourceAlignment(SrcAlign);
    return MI;
  }

  // If we have a store to a location which is known constant, we can conclude
  // that the store must be storing the constant value (else the memory
  // wouldn't be constant), and this must be a noop.
  if (AA->pointsToConstantMemory(MI->getDest())) {
    // Set the size of the copy to 0, it will be deleted on the next iteration.
    MI->setLength(Constant::getNullValue(MI->getLength()->getType()));
    return MI;
  }

  // If MemCpyInst length is 1/2/4/8 bytes then replace memcpy with
  // load/store.
  ConstantInt *MemOpLength = dyn_cast<ConstantInt>(MI->getLength());
  if (!MemOpLength) return nullptr;

  // Source and destination pointer types are always "i8*" for intrinsic.  See
  // if the size is something we can handle with a single primitive load/store.
  // A single load+store correctly handles overlapping memory in the memmove
  // case.
  uint64_t Size = MemOpLength->getLimitedValue();
  assert(Size && "0-sized memory transferring should be removed already.");

  if (Size > 8 || (Size&(Size-1)))
    return nullptr;  // If not 1/2/4/8 bytes, exit.

  // If it is an atomic and alignment is less than the size then we will
  // introduce the unaligned memory access which will be later transformed
  // into libcall in CodeGen. This is not evident performance gain so disable
  // it now.
  if (isa<AtomicMemTransferInst>(MI))
    if (*CopyDstAlign < Size || *CopySrcAlign < Size)
      return nullptr;

  // Use an integer load+store unless we can find something better.
  unsigned SrcAddrSp =
    cast<PointerType>(MI->getArgOperand(1)->getType())->getAddressSpace();
  unsigned DstAddrSp =
    cast<PointerType>(MI->getArgOperand(0)->getType())->getAddressSpace();

  IntegerType* IntType = IntegerType::get(MI->getContext(), Size<<3);
  Type *NewSrcPtrTy = PointerType::get(IntType, SrcAddrSp);
  Type *NewDstPtrTy = PointerType::get(IntType, DstAddrSp);

  // If the memcpy has metadata describing the members, see if we can get the
  // TBAA tag describing our copy.
  MDNode *CopyMD = nullptr;
  if (MDNode *M = MI->getMetadata(LLVMContext::MD_tbaa)) {
    CopyMD = M;
  } else if (MDNode *M = MI->getMetadata(LLVMContext::MD_tbaa_struct)) {
    if (M->getNumOperands() == 3 && M->getOperand(0) &&
        mdconst::hasa<ConstantInt>(M->getOperand(0)) &&
        mdconst::extract<ConstantInt>(M->getOperand(0))->isZero() &&
        M->getOperand(1) &&
        mdconst::hasa<ConstantInt>(M->getOperand(1)) &&
        mdconst::extract<ConstantInt>(M->getOperand(1))->getValue() ==
        Size &&
        M->getOperand(2) && isa<MDNode>(M->getOperand(2)))
      CopyMD = cast<MDNode>(M->getOperand(2));
  }

  Value *Src = Builder.CreateBitCast(MI->getArgOperand(1), NewSrcPtrTy);
  Value *Dest = Builder.CreateBitCast(MI->getArgOperand(0), NewDstPtrTy);
  LoadInst *L = Builder.CreateLoad(IntType, Src);
  // Alignment from the mem intrinsic will be better, so use it.
  L->setAlignment(*CopySrcAlign);
  if (CopyMD)
    L->setMetadata(LLVMContext::MD_tbaa, CopyMD);
  MDNode *LoopMemParallelMD =
    MI->getMetadata(LLVMContext::MD_mem_parallel_loop_access);
  if (LoopMemParallelMD)
    L->setMetadata(LLVMContext::MD_mem_parallel_loop_access, LoopMemParallelMD);
  MDNode *AccessGroupMD = MI->getMetadata(LLVMContext::MD_access_group);
  if (AccessGroupMD)
    L->setMetadata(LLVMContext::MD_access_group, AccessGroupMD);

  StoreInst *S = Builder.CreateStore(L, Dest);
  // Alignment from the mem intrinsic will be better, so use it.
  S->setAlignment(*CopyDstAlign);
  if (CopyMD)
    S->setMetadata(LLVMContext::MD_tbaa, CopyMD);
  if (LoopMemParallelMD)
    S->setMetadata(LLVMContext::MD_mem_parallel_loop_access, LoopMemParallelMD);
  if (AccessGroupMD)
    S->setMetadata(LLVMContext::MD_access_group, AccessGroupMD);

  if (auto *MT = dyn_cast<MemTransferInst>(MI)) {
    // non-atomics can be volatile
    L->setVolatile(MT->isVolatile());
    S->setVolatile(MT->isVolatile());
  }
  if (isa<AtomicMemTransferInst>(MI)) {
    // atomics have to be unordered
    L->setOrdering(AtomicOrdering::Unordered);
    S->setOrdering(AtomicOrdering::Unordered);
  }

  // Set the size of the copy to 0, it will be deleted on the next iteration.
  MI->setLength(Constant::getNullValue(MemOpLength->getType()));
  return MI;
}

Instruction *InstCombinerImpl::SimplifyAnyMemSet(AnyMemSetInst *MI) {
  const Align KnownAlignment =
      getKnownAlignment(MI->getDest(), DL, MI, &AC, &DT);
  MaybeAlign MemSetAlign = MI->getDestAlign();
  if (!MemSetAlign || *MemSetAlign < KnownAlignment) {
    MI->setDestAlignment(KnownAlignment);
    return MI;
  }

  // If we have a store to a location which is known constant, we can conclude
  // that the store must be storing the constant value (else the memory
  // wouldn't be constant), and this must be a noop.
  if (AA->pointsToConstantMemory(MI->getDest())) {
    // Set the size of the copy to 0, it will be deleted on the next iteration.
    MI->setLength(Constant::getNullValue(MI->getLength()->getType()));
    return MI;
  }

  // Extract the length and alignment and fill if they are constant.
  ConstantInt *LenC = dyn_cast<ConstantInt>(MI->getLength());
  ConstantInt *FillC = dyn_cast<ConstantInt>(MI->getValue());
  if (!LenC || !FillC || !FillC->getType()->isIntegerTy(8))
    return nullptr;
  const uint64_t Len = LenC->getLimitedValue();
  assert(Len && "0-sized memory setting should be removed already.");
  const Align Alignment = assumeAligned(MI->getDestAlignment());

  // If it is an atomic and alignment is less than the size then we will
  // introduce the unaligned memory access which will be later transformed
  // into libcall in CodeGen. This is not evident performance gain so disable
  // it now.
  if (isa<AtomicMemSetInst>(MI))
    if (Alignment < Len)
      return nullptr;

  // memset(s,c,n) -> store s, c (for n=1,2,4,8)
  if (Len <= 8 && isPowerOf2_32((uint32_t)Len)) {
    Type *ITy = IntegerType::get(MI->getContext(), Len*8);  // n=1 -> i8.

    Value *Dest = MI->getDest();
    unsigned DstAddrSp = cast<PointerType>(Dest->getType())->getAddressSpace();
    Type *NewDstPtrTy = PointerType::get(ITy, DstAddrSp);
    Dest = Builder.CreateBitCast(Dest, NewDstPtrTy);

    // Extract the fill value and store.
    uint64_t Fill = FillC->getZExtValue()*0x0101010101010101ULL;
    StoreInst *S = Builder.CreateStore(ConstantInt::get(ITy, Fill), Dest,
                                       MI->isVolatile());
    S->setAlignment(Alignment);
    if (isa<AtomicMemSetInst>(MI))
      S->setOrdering(AtomicOrdering::Unordered);

    // Set the size of the copy to 0, it will be deleted on the next iteration.
    MI->setLength(Constant::getNullValue(LenC->getType()));
    return MI;
  }

  return nullptr;
}

// TODO, Obvious Missing Transforms:
// * Narrow width by halfs excluding zero/undef lanes
Value *InstCombinerImpl::simplifyMaskedLoad(IntrinsicInst &II) {
  Value *LoadPtr = II.getArgOperand(0);
  const Align Alignment =
      cast<ConstantInt>(II.getArgOperand(1))->getAlignValue();

  // If the mask is all ones or undefs, this is a plain vector load of the 1st
  // argument.
  if (maskIsAllOneOrUndef(II.getArgOperand(2)))
    return Builder.CreateAlignedLoad(II.getType(), LoadPtr, Alignment,
                                     "unmaskedload");

  // If we can unconditionally load from this address, replace with a
  // load/select idiom. TODO: use DT for context sensitive query
  if (isDereferenceablePointer(LoadPtr, II.getType(),
                               II.getModule()->getDataLayout(), &II, nullptr)) {
    Value *LI = Builder.CreateAlignedLoad(II.getType(), LoadPtr, Alignment,
                                         "unmaskedload");
    return Builder.CreateSelect(II.getArgOperand(2), LI, II.getArgOperand(3));
  }

  return nullptr;
}

// TODO, Obvious Missing Transforms:
// * Single constant active lane -> store
// * Narrow width by halfs excluding zero/undef lanes
Instruction *InstCombinerImpl::simplifyMaskedStore(IntrinsicInst &II) {
  auto *ConstMask = dyn_cast<Constant>(II.getArgOperand(3));
  if (!ConstMask)
    return nullptr;

  // If the mask is all zeros, this instruction does nothing.
  if (ConstMask->isNullValue())
    return eraseInstFromFunction(II);

  // If the mask is all ones, this is a plain vector store of the 1st argument.
  if (ConstMask->isAllOnesValue()) {
    Value *StorePtr = II.getArgOperand(1);
    Align Alignment = cast<ConstantInt>(II.getArgOperand(2))->getAlignValue();
    return new StoreInst(II.getArgOperand(0), StorePtr, false, Alignment);
  }

  if (isa<ScalableVectorType>(ConstMask->getType()))
    return nullptr;

  // Use masked off lanes to simplify operands via SimplifyDemandedVectorElts
  APInt DemandedElts = possiblyDemandedEltsInMask(ConstMask);
  APInt UndefElts(DemandedElts.getBitWidth(), 0);
  if (Value *V =
          SimplifyDemandedVectorElts(II.getOperand(0), DemandedElts, UndefElts))
    return replaceOperand(II, 0, V);

  return nullptr;
}

// TODO, Obvious Missing Transforms:
// * Single constant active lane load -> load
// * Dereferenceable address & few lanes -> scalarize speculative load/selects
// * Adjacent vector addresses -> masked.load
// * Narrow width by halfs excluding zero/undef lanes
// * Vector splat address w/known mask -> scalar load
// * Vector incrementing address -> vector masked load
Instruction *InstCombinerImpl::simplifyMaskedGather(IntrinsicInst &II) {
  return nullptr;
}

// TODO, Obvious Missing Transforms:
// * Single constant active lane -> store
// * Adjacent vector addresses -> masked.store
// * Narrow store width by halfs excluding zero/undef lanes
// * Vector splat address w/known mask -> scalar store
// * Vector incrementing address -> vector masked store
Instruction *InstCombinerImpl::simplifyMaskedScatter(IntrinsicInst &II) {
  auto *ConstMask = dyn_cast<Constant>(II.getArgOperand(3));
  if (!ConstMask)
    return nullptr;

  // If the mask is all zeros, a scatter does nothing.
  if (ConstMask->isNullValue())
    return eraseInstFromFunction(II);

  if (isa<ScalableVectorType>(ConstMask->getType()))
    return nullptr;

  // Use masked off lanes to simplify operands via SimplifyDemandedVectorElts
  APInt DemandedElts = possiblyDemandedEltsInMask(ConstMask);
  APInt UndefElts(DemandedElts.getBitWidth(), 0);
  if (Value *V =
          SimplifyDemandedVectorElts(II.getOperand(0), DemandedElts, UndefElts))
    return replaceOperand(II, 0, V);
  if (Value *V =
          SimplifyDemandedVectorElts(II.getOperand(1), DemandedElts, UndefElts))
    return replaceOperand(II, 1, V);

  return nullptr;
}

/// This function transforms launder.invariant.group and strip.invariant.group
/// like:
/// launder(launder(%x)) -> launder(%x)       (the result is not the argument)
/// launder(strip(%x)) -> launder(%x)
/// strip(strip(%x)) -> strip(%x)             (the result is not the argument)
/// strip(launder(%x)) -> strip(%x)
/// This is legal because it preserves the most recent information about
/// the presence or absence of invariant.group.
static Instruction *simplifyInvariantGroupIntrinsic(IntrinsicInst &II,
                                                    InstCombinerImpl &IC) {
  auto *Arg = II.getArgOperand(0);
  auto *StrippedArg = Arg->stripPointerCasts();
  auto *StrippedInvariantGroupsArg = Arg->stripPointerCastsAndInvariantGroups();
  if (StrippedArg == StrippedInvariantGroupsArg)
    return nullptr; // No launders/strips to remove.

  Value *Result = nullptr;

  if (II.getIntrinsicID() == Intrinsic::launder_invariant_group)
    Result = IC.Builder.CreateLaunderInvariantGroup(StrippedInvariantGroupsArg);
  else if (II.getIntrinsicID() == Intrinsic::strip_invariant_group)
    Result = IC.Builder.CreateStripInvariantGroup(StrippedInvariantGroupsArg);
  else
    llvm_unreachable(
        "simplifyInvariantGroupIntrinsic only handles launder and strip");
  if (Result->getType()->getPointerAddressSpace() !=
      II.getType()->getPointerAddressSpace())
    Result = IC.Builder.CreateAddrSpaceCast(Result, II.getType());
  if (Result->getType() != II.getType())
    Result = IC.Builder.CreateBitCast(Result, II.getType());

  return cast<Instruction>(Result);
}

static Instruction *foldCttzCtlz(IntrinsicInst &II, InstCombinerImpl &IC) {
  assert((II.getIntrinsicID() == Intrinsic::cttz ||
          II.getIntrinsicID() == Intrinsic::ctlz) &&
         "Expected cttz or ctlz intrinsic");
  bool IsTZ = II.getIntrinsicID() == Intrinsic::cttz;
  Value *Op0 = II.getArgOperand(0);
  Value *X;
  // ctlz(bitreverse(x)) -> cttz(x)
  // cttz(bitreverse(x)) -> ctlz(x)
  if (match(Op0, m_BitReverse(m_Value(X)))) {
    Intrinsic::ID ID = IsTZ ? Intrinsic::ctlz : Intrinsic::cttz;
    Function *F = Intrinsic::getDeclaration(II.getModule(), ID, II.getType());
    return CallInst::Create(F, {X, II.getArgOperand(1)});
  }

  if (IsTZ) {
    // cttz(-x) -> cttz(x)
    if (match(Op0, m_Neg(m_Value(X))))
      return IC.replaceOperand(II, 0, X);

    // cttz(abs(x)) -> cttz(x)
    // cttz(nabs(x)) -> cttz(x)
    Value *Y;
    SelectPatternFlavor SPF = matchSelectPattern(Op0, X, Y).Flavor;
    if (SPF == SPF_ABS || SPF == SPF_NABS)
      return IC.replaceOperand(II, 0, X);

    if (match(Op0, m_Intrinsic<Intrinsic::abs>(m_Value(X))))
      return IC.replaceOperand(II, 0, X);
  }

  KnownBits Known = IC.computeKnownBits(Op0, 0, &II);

  // Create a mask for bits above (ctlz) or below (cttz) the first known one.
  unsigned PossibleZeros = IsTZ ? Known.countMaxTrailingZeros()
                                : Known.countMaxLeadingZeros();
  unsigned DefiniteZeros = IsTZ ? Known.countMinTrailingZeros()
                                : Known.countMinLeadingZeros();

  // If all bits above (ctlz) or below (cttz) the first known one are known
  // zero, this value is constant.
  // FIXME: This should be in InstSimplify because we're replacing an
  // instruction with a constant.
  if (PossibleZeros == DefiniteZeros) {
    auto *C = ConstantInt::get(Op0->getType(), DefiniteZeros);
    return IC.replaceInstUsesWith(II, C);
  }

  // If the input to cttz/ctlz is known to be non-zero,
  // then change the 'ZeroIsUndef' parameter to 'true'
  // because we know the zero behavior can't affect the result.
  if (!Known.One.isNullValue() ||
      isKnownNonZero(Op0, IC.getDataLayout(), 0, &IC.getAssumptionCache(), &II,
                     &IC.getDominatorTree())) {
    if (!match(II.getArgOperand(1), m_One()))
      return IC.replaceOperand(II, 1, IC.Builder.getTrue());
  }

  // Add range metadata since known bits can't completely reflect what we know.
  // TODO: Handle splat vectors.
  auto *IT = dyn_cast<IntegerType>(Op0->getType());
  if (IT && IT->getBitWidth() != 1 && !II.getMetadata(LLVMContext::MD_range)) {
    Metadata *LowAndHigh[] = {
        ConstantAsMetadata::get(ConstantInt::get(IT, DefiniteZeros)),
        ConstantAsMetadata::get(ConstantInt::get(IT, PossibleZeros + 1))};
    II.setMetadata(LLVMContext::MD_range,
                   MDNode::get(II.getContext(), LowAndHigh));
    return &II;
  }

  return nullptr;
}

static Instruction *foldCtpop(IntrinsicInst &II, InstCombinerImpl &IC) {
  assert(II.getIntrinsicID() == Intrinsic::ctpop &&
         "Expected ctpop intrinsic");
  Type *Ty = II.getType();
  unsigned BitWidth = Ty->getScalarSizeInBits();
  Value *Op0 = II.getArgOperand(0);
  Value *X;

  // ctpop(bitreverse(x)) -> ctpop(x)
  // ctpop(bswap(x)) -> ctpop(x)
  if (match(Op0, m_BitReverse(m_Value(X))) || match(Op0, m_BSwap(m_Value(X))))
    return IC.replaceOperand(II, 0, X);

  // ctpop(x | -x) -> bitwidth - cttz(x, false)
  if (Op0->hasOneUse() &&
      match(Op0, m_c_Or(m_Value(X), m_Neg(m_Deferred(X))))) {
    Function *F =
        Intrinsic::getDeclaration(II.getModule(), Intrinsic::cttz, Ty);
    auto *Cttz = IC.Builder.CreateCall(F, {X, IC.Builder.getFalse()});
    auto *Bw = ConstantInt::get(Ty, APInt(BitWidth, BitWidth));
    return IC.replaceInstUsesWith(II, IC.Builder.CreateSub(Bw, Cttz));
  }

  // ctpop(~x & (x - 1)) -> cttz(x, false)
  if (match(Op0,
            m_c_And(m_Not(m_Value(X)), m_Add(m_Deferred(X), m_AllOnes())))) {
    Function *F =
        Intrinsic::getDeclaration(II.getModule(), Intrinsic::cttz, Ty);
    return CallInst::Create(F, {X, IC.Builder.getFalse()});
  }

  // FIXME: Try to simplify vectors of integers.
  auto *IT = dyn_cast<IntegerType>(Ty);
  if (!IT)
    return nullptr;

  KnownBits Known(BitWidth);
  IC.computeKnownBits(Op0, Known, 0, &II);

  unsigned MinCount = Known.countMinPopulation();
  unsigned MaxCount = Known.countMaxPopulation();

  // Add range metadata since known bits can't completely reflect what we know.
  if (IT->getBitWidth() != 1 && !II.getMetadata(LLVMContext::MD_range)) {
    Metadata *LowAndHigh[] = {
        ConstantAsMetadata::get(ConstantInt::get(IT, MinCount)),
        ConstantAsMetadata::get(ConstantInt::get(IT, MaxCount + 1))};
    II.setMetadata(LLVMContext::MD_range,
                   MDNode::get(II.getContext(), LowAndHigh));
    return &II;
  }

  return nullptr;
}

/// Convert a table lookup to shufflevector if the mask is constant.
/// This could benefit tbl1 if the mask is { 7,6,5,4,3,2,1,0 }, in
/// which case we could lower the shufflevector with rev64 instructions
/// as it's actually a byte reverse.
static Value *simplifyNeonTbl1(const IntrinsicInst &II,
                               InstCombiner::BuilderTy &Builder) {
  // Bail out if the mask is not a constant.
  auto *C = dyn_cast<Constant>(II.getArgOperand(1));
  if (!C)
    return nullptr;

  auto *VecTy = cast<FixedVectorType>(II.getType());
  unsigned NumElts = VecTy->getNumElements();

  // Only perform this transformation for <8 x i8> vector types.
  if (!VecTy->getElementType()->isIntegerTy(8) || NumElts != 8)
    return nullptr;

  int Indexes[8];

  for (unsigned I = 0; I < NumElts; ++I) {
    Constant *COp = C->getAggregateElement(I);

    if (!COp || !isa<ConstantInt>(COp))
      return nullptr;

    Indexes[I] = cast<ConstantInt>(COp)->getLimitedValue();

    // Make sure the mask indices are in range.
    if ((unsigned)Indexes[I] >= NumElts)
      return nullptr;
  }

  auto *V1 = II.getArgOperand(0);
  auto *V2 = Constant::getNullValue(V1->getType());
  return Builder.CreateShuffleVector(V1, V2, makeArrayRef(Indexes));
}

// Returns true iff the 2 intrinsics have the same operands, limiting the
// comparison to the first NumOperands.
static bool haveSameOperands(const IntrinsicInst &I, const IntrinsicInst &E,
                             unsigned NumOperands) {
  assert(I.getNumArgOperands() >= NumOperands && "Not enough operands");
  assert(E.getNumArgOperands() >= NumOperands && "Not enough operands");
  for (unsigned i = 0; i < NumOperands; i++)
    if (I.getArgOperand(i) != E.getArgOperand(i))
      return false;
  return true;
}

// Remove trivially empty start/end intrinsic ranges, i.e. a start
// immediately followed by an end (ignoring debuginfo or other
// start/end intrinsics in between). As this handles only the most trivial
// cases, tracking the nesting level is not needed:
//
//   call @llvm.foo.start(i1 0)
//   call @llvm.foo.start(i1 0) ; This one won't be skipped: it will be removed
//   call @llvm.foo.end(i1 0)
//   call @llvm.foo.end(i1 0) ; &I
static bool
removeTriviallyEmptyRange(IntrinsicInst &EndI, InstCombinerImpl &IC,
                          std::function<bool(const IntrinsicInst &)> IsStart) {
  // We start from the end intrinsic and scan backwards, so that InstCombine
  // has already processed (and potentially removed) all the instructions
  // before the end intrinsic.
  BasicBlock::reverse_iterator BI(EndI), BE(EndI.getParent()->rend());
  for (; BI != BE; ++BI) {
    if (auto *I = dyn_cast<IntrinsicInst>(&*BI)) {
      if (isa<DbgInfoIntrinsic>(I) ||
          I->getIntrinsicID() == EndI.getIntrinsicID())
        continue;
      if (IsStart(*I)) {
        if (haveSameOperands(EndI, *I, EndI.getNumArgOperands())) {
          IC.eraseInstFromFunction(*I);
          IC.eraseInstFromFunction(EndI);
          return true;
        }
        // Skip start intrinsics that don't pair with this end intrinsic.
        continue;
      }
    }
    break;
  }

  return false;
}

Instruction *InstCombinerImpl::visitVAEndInst(VAEndInst &I) {
  removeTriviallyEmptyRange(I, *this, [](const IntrinsicInst &I) {
    return I.getIntrinsicID() == Intrinsic::vastart ||
           I.getIntrinsicID() == Intrinsic::vacopy;
  });
  return nullptr;
}

static CallInst *canonicalizeConstantArg0ToArg1(CallInst &Call) {
  assert(Call.getNumArgOperands() > 1 && "Need at least 2 args to swap");
  Value *Arg0 = Call.getArgOperand(0), *Arg1 = Call.getArgOperand(1);
  if (isa<Constant>(Arg0) && !isa<Constant>(Arg1)) {
    Call.setArgOperand(0, Arg1);
    Call.setArgOperand(1, Arg0);
    return &Call;
  }
  return nullptr;
}

/// Creates a result tuple for an overflow intrinsic \p II with a given
/// \p Result and a constant \p Overflow value.
static Instruction *createOverflowTuple(IntrinsicInst *II, Value *Result,
                                        Constant *Overflow) {
  Constant *V[] = {UndefValue::get(Result->getType()), Overflow};
  StructType *ST = cast<StructType>(II->getType());
  Constant *Struct = ConstantStruct::get(ST, V);
  return InsertValueInst::Create(Struct, Result, 0);
}

Instruction *
InstCombinerImpl::foldIntrinsicWithOverflowCommon(IntrinsicInst *II) {
  WithOverflowInst *WO = cast<WithOverflowInst>(II);
  Value *OperationResult = nullptr;
  Constant *OverflowResult = nullptr;
  if (OptimizeOverflowCheck(WO->getBinaryOp(), WO->isSigned(), WO->getLHS(),
                            WO->getRHS(), *WO, OperationResult, OverflowResult))
    return createOverflowTuple(WO, OperationResult, OverflowResult);
  return nullptr;
}

static Optional<bool> getKnownSign(Value *Op, Instruction *CxtI,
                                   const DataLayout &DL, AssumptionCache *AC,
                                   DominatorTree *DT) {
  KnownBits Known = computeKnownBits(Op, DL, 0, AC, CxtI, DT);
  if (Known.isNonNegative())
    return false;
  if (Known.isNegative())
    return true;

  return isImpliedByDomCondition(
      ICmpInst::ICMP_SLT, Op, Constant::getNullValue(Op->getType()), CxtI, DL);
}

/// CallInst simplification. This mostly only handles folding of intrinsic
/// instructions. For normal calls, it allows visitCallBase to do the heavy
/// lifting.
Instruction *InstCombinerImpl::visitCallInst(CallInst &CI) {
  // Don't try to simplify calls without uses. It will not do anything useful,
  // but will result in the following folds being skipped.
  if (!CI.use_empty())
    if (Value *V = SimplifyCall(&CI, SQ.getWithInstruction(&CI)))
      return replaceInstUsesWith(CI, V);

  if (isFreeCall(&CI, &TLI))
    return visitFree(CI);

  // If the caller function is nounwind, mark the call as nounwind, even if the
  // callee isn't.
  if (CI.getFunction()->doesNotThrow() && !CI.doesNotThrow()) {
    CI.setDoesNotThrow();
    return &CI;
  }

  IntrinsicInst *II = dyn_cast<IntrinsicInst>(&CI);
  if (!II) return visitCallBase(CI);

  // For atomic unordered mem intrinsics if len is not a positive or
  // not a multiple of element size then behavior is undefined.
  if (auto *AMI = dyn_cast<AtomicMemIntrinsic>(II))
    if (ConstantInt *NumBytes = dyn_cast<ConstantInt>(AMI->getLength()))
      if (NumBytes->getSExtValue() < 0 ||
          (NumBytes->getZExtValue() % AMI->getElementSizeInBytes() != 0)) {
        CreateNonTerminatorUnreachable(AMI);
        assert(AMI->getType()->isVoidTy() &&
               "non void atomic unordered mem intrinsic");
        return eraseInstFromFunction(*AMI);
      }

  // Intrinsics cannot occur in an invoke or a callbr, so handle them here
  // instead of in visitCallBase.
  if (auto *MI = dyn_cast<AnyMemIntrinsic>(II)) {
    bool Changed = false;

    // memmove/cpy/set of zero bytes is a noop.
    if (Constant *NumBytes = dyn_cast<Constant>(MI->getLength())) {
      if (NumBytes->isNullValue())
        return eraseInstFromFunction(CI);

      if (ConstantInt *CI = dyn_cast<ConstantInt>(NumBytes))
        if (CI->getZExtValue() == 1) {
          // Replace the instruction with just byte operations.  We would
          // transform other cases to loads/stores, but we don't know if
          // alignment is sufficient.
        }
    }

    // No other transformations apply to volatile transfers.
    if (auto *M = dyn_cast<MemIntrinsic>(MI))
      if (M->isVolatile())
        return nullptr;

    // If we have a memmove and the source operation is a constant global,
    // then the source and dest pointers can't alias, so we can change this
    // into a call to memcpy.
    if (auto *MMI = dyn_cast<AnyMemMoveInst>(MI)) {
      if (GlobalVariable *GVSrc = dyn_cast<GlobalVariable>(MMI->getSource()))
        if (GVSrc->isConstant()) {
          Module *M = CI.getModule();
          Intrinsic::ID MemCpyID =
              isa<AtomicMemMoveInst>(MMI)
                  ? Intrinsic::memcpy_element_unordered_atomic
                  : Intrinsic::memcpy;
          Type *Tys[3] = { CI.getArgOperand(0)->getType(),
                           CI.getArgOperand(1)->getType(),
                           CI.getArgOperand(2)->getType() };
          CI.setCalledFunction(Intrinsic::getDeclaration(M, MemCpyID, Tys));
          Changed = true;
        }
    }

    if (AnyMemTransferInst *MTI = dyn_cast<AnyMemTransferInst>(MI)) {
      // memmove(x,x,size) -> noop.
      if (MTI->getSource() == MTI->getDest())
        return eraseInstFromFunction(CI);
    }

    // If we can determine a pointer alignment that is bigger than currently
    // set, update the alignment.
    if (auto *MTI = dyn_cast<AnyMemTransferInst>(MI)) {
      if (Instruction *I = SimplifyAnyMemTransfer(MTI))
        return I;
    } else if (auto *MSI = dyn_cast<AnyMemSetInst>(MI)) {
      if (Instruction *I = SimplifyAnyMemSet(MSI))
        return I;
    }

    if (Changed) return II;
  }

  // For fixed width vector result intrinsics, use the generic demanded vector
  // support.
  if (auto *IIFVTy = dyn_cast<FixedVectorType>(II->getType())) {
    auto VWidth = IIFVTy->getNumElements();
    APInt UndefElts(VWidth, 0);
    APInt AllOnesEltMask(APInt::getAllOnesValue(VWidth));
    if (Value *V = SimplifyDemandedVectorElts(II, AllOnesEltMask, UndefElts)) {
      if (V != II)
        return replaceInstUsesWith(*II, V);
      return II;
    }
  }

  if (II->isCommutative()) {
    if (CallInst *NewCall = canonicalizeConstantArg0ToArg1(CI))
      return NewCall;
  }

  Intrinsic::ID IID = II->getIntrinsicID();
  switch (IID) {
  case Intrinsic::objectsize:
    if (Value *V = lowerObjectSizeCall(II, DL, &TLI, /*MustSucceed=*/false))
      return replaceInstUsesWith(CI, V);
    return nullptr;
  case Intrinsic::abs: {
    Value *IIOperand = II->getArgOperand(0);
    bool IntMinIsPoison = cast<Constant>(II->getArgOperand(1))->isOneValue();

    // abs(-x) -> abs(x)
    // TODO: Copy nsw if it was present on the neg?
    Value *X;
    if (match(IIOperand, m_Neg(m_Value(X))))
      return replaceOperand(*II, 0, X);
    if (match(IIOperand, m_Select(m_Value(), m_Value(X), m_Neg(m_Deferred(X)))))
      return replaceOperand(*II, 0, X);
    if (match(IIOperand, m_Select(m_Value(), m_Neg(m_Value(X)), m_Deferred(X))))
      return replaceOperand(*II, 0, X);

    if (Optional<bool> Sign = getKnownSign(IIOperand, II, DL, &AC, &DT)) {
      // abs(x) -> x if x >= 0
      if (!*Sign)
        return replaceInstUsesWith(*II, IIOperand);

      // abs(x) -> -x if x < 0
      if (IntMinIsPoison)
        return BinaryOperator::CreateNSWNeg(IIOperand);
      return BinaryOperator::CreateNeg(IIOperand);
    }

    // abs (sext X) --> zext (abs X*)
    // Clear the IsIntMin (nsw) bit on the abs to allow narrowing.
    if (match(IIOperand, m_OneUse(m_SExt(m_Value(X))))) {
      Value *NarrowAbs =
          Builder.CreateBinaryIntrinsic(Intrinsic::abs, X, Builder.getFalse());
      return CastInst::Create(Instruction::ZExt, NarrowAbs, II->getType());
    }

    break;
  }
  case Intrinsic::umax:
  case Intrinsic::umin: {
    Value *I0 = II->getArgOperand(0), *I1 = II->getArgOperand(1);
    Value *X, *Y;
    if (match(I0, m_ZExt(m_Value(X))) && match(I1, m_ZExt(m_Value(Y))) &&
        (I0->hasOneUse() || I1->hasOneUse()) && X->getType() == Y->getType()) {
      Value *NarrowMaxMin = Builder.CreateBinaryIntrinsic(IID, X, Y);
      return CastInst::Create(Instruction::ZExt, NarrowMaxMin, II->getType());
    }
    Constant *C;
    if (match(I0, m_ZExt(m_Value(X))) && match(I1, m_Constant(C)) &&
        I0->hasOneUse()) {
      Constant *NarrowC = ConstantExpr::getTrunc(C, X->getType());
      if (ConstantExpr::getZExt(NarrowC, II->getType()) == C) {
        Value *NarrowMaxMin = Builder.CreateBinaryIntrinsic(IID, X, NarrowC);
        return CastInst::Create(Instruction::ZExt, NarrowMaxMin, II->getType());
      }
    }
    // If both operands of unsigned min/max are sign-extended, it is still ok
    // to narrow the operation.
    LLVM_FALLTHROUGH;
  }
  case Intrinsic::smax:
  case Intrinsic::smin: {
    Value *I0 = II->getArgOperand(0), *I1 = II->getArgOperand(1);
    Value *X, *Y;
    if (match(I0, m_SExt(m_Value(X))) && match(I1, m_SExt(m_Value(Y))) &&
        (I0->hasOneUse() || I1->hasOneUse()) && X->getType() == Y->getType()) {
      Value *NarrowMaxMin = Builder.CreateBinaryIntrinsic(IID, X, Y);
      return CastInst::Create(Instruction::SExt, NarrowMaxMin, II->getType());
    }
    Constant *C;
    if (match(I0, m_SExt(m_Value(X))) && match(I1, m_Constant(C)) &&
        I0->hasOneUse()) {
      Constant *NarrowC = ConstantExpr::getTrunc(C, X->getType());
      if (ConstantExpr::getSExt(NarrowC, II->getType()) == C) {
        Value *NarrowMaxMin = Builder.CreateBinaryIntrinsic(IID, X, NarrowC);
        return CastInst::Create(Instruction::SExt, NarrowMaxMin, II->getType());
      }
    }
    break;
  }
  case Intrinsic::bswap: {
    Value *IIOperand = II->getArgOperand(0);
    Value *X = nullptr;

    // bswap(trunc(bswap(x))) -> trunc(lshr(x, c))
    if (match(IIOperand, m_Trunc(m_BSwap(m_Value(X))))) {
      unsigned C = X->getType()->getScalarSizeInBits() -
                   IIOperand->getType()->getScalarSizeInBits();
      Value *CV = ConstantInt::get(X->getType(), C);
      Value *V = Builder.CreateLShr(X, CV);
      return new TruncInst(V, IIOperand->getType());
    }
    break;
  }
  case Intrinsic::masked_load:
    if (Value *SimplifiedMaskedOp = simplifyMaskedLoad(*II))
      return replaceInstUsesWith(CI, SimplifiedMaskedOp);
    break;
  case Intrinsic::masked_store:
    return simplifyMaskedStore(*II);
  case Intrinsic::masked_gather:
    return simplifyMaskedGather(*II);
  case Intrinsic::masked_scatter:
    return simplifyMaskedScatter(*II);
  case Intrinsic::launder_invariant_group:
  case Intrinsic::strip_invariant_group:
    if (auto *SkippedBarrier = simplifyInvariantGroupIntrinsic(*II, *this))
      return replaceInstUsesWith(*II, SkippedBarrier);
    break;
  case Intrinsic::powi:
    if (ConstantInt *Power = dyn_cast<ConstantInt>(II->getArgOperand(1))) {
      // 0 and 1 are handled in instsimplify
      // powi(x, -1) -> 1/x
      if (Power->isMinusOne())
        return BinaryOperator::CreateFDivFMF(ConstantFP::get(CI.getType(), 1.0),
                                             II->getArgOperand(0), II);
      // powi(x, 2) -> x*x
      if (Power->equalsInt(2))
        return BinaryOperator::CreateFMulFMF(II->getArgOperand(0),
                                             II->getArgOperand(0), II);
    }
    break;

  case Intrinsic::cttz:
  case Intrinsic::ctlz:
    if (auto *I = foldCttzCtlz(*II, *this))
      return I;
    break;

  case Intrinsic::ctpop:
    if (auto *I = foldCtpop(*II, *this))
      return I;
    break;

  case Intrinsic::fshl:
  case Intrinsic::fshr: {
    Value *Op0 = II->getArgOperand(0), *Op1 = II->getArgOperand(1);
    Type *Ty = II->getType();
    unsigned BitWidth = Ty->getScalarSizeInBits();
    Constant *ShAmtC;
    if (match(II->getArgOperand(2), m_ImmConstant(ShAmtC)) &&
        !ShAmtC->containsConstantExpression()) {
      // Canonicalize a shift amount constant operand to modulo the bit-width.
      Constant *WidthC = ConstantInt::get(Ty, BitWidth);
      Constant *ModuloC = ConstantExpr::getURem(ShAmtC, WidthC);
      if (ModuloC != ShAmtC)
        return replaceOperand(*II, 2, ModuloC);

      assert(ConstantExpr::getICmp(ICmpInst::ICMP_UGT, WidthC, ShAmtC) ==
                 ConstantInt::getTrue(CmpInst::makeCmpResultType(Ty)) &&
             "Shift amount expected to be modulo bitwidth");

      // Canonicalize funnel shift right by constant to funnel shift left. This
      // is not entirely arbitrary. For historical reasons, the backend may
      // recognize rotate left patterns but miss rotate right patterns.
      if (IID == Intrinsic::fshr) {
        // fshr X, Y, C --> fshl X, Y, (BitWidth - C)
        Constant *LeftShiftC = ConstantExpr::getSub(WidthC, ShAmtC);
        Module *Mod = II->getModule();
        Function *Fshl = Intrinsic::getDeclaration(Mod, Intrinsic::fshl, Ty);
        return CallInst::Create(Fshl, { Op0, Op1, LeftShiftC });
      }
      assert(IID == Intrinsic::fshl &&
             "All funnel shifts by simple constants should go left");

      // fshl(X, 0, C) --> shl X, C
      // fshl(X, undef, C) --> shl X, C
      if (match(Op1, m_ZeroInt()) || match(Op1, m_Undef()))
        return BinaryOperator::CreateShl(Op0, ShAmtC);

      // fshl(0, X, C) --> lshr X, (BW-C)
      // fshl(undef, X, C) --> lshr X, (BW-C)
      if (match(Op0, m_ZeroInt()) || match(Op0, m_Undef()))
        return BinaryOperator::CreateLShr(Op1,
                                          ConstantExpr::getSub(WidthC, ShAmtC));

      // fshl i16 X, X, 8 --> bswap i16 X (reduce to more-specific form)
      if (Op0 == Op1 && BitWidth == 16 && match(ShAmtC, m_SpecificInt(8))) {
        Module *Mod = II->getModule();
        Function *Bswap = Intrinsic::getDeclaration(Mod, Intrinsic::bswap, Ty);
        return CallInst::Create(Bswap, { Op0 });
      }
    }

    // Left or right might be masked.
    if (SimplifyDemandedInstructionBits(*II))
      return &CI;

    // The shift amount (operand 2) of a funnel shift is modulo the bitwidth,
    // so only the low bits of the shift amount are demanded if the bitwidth is
    // a power-of-2.
    if (!isPowerOf2_32(BitWidth))
      break;
    APInt Op2Demanded = APInt::getLowBitsSet(BitWidth, Log2_32_Ceil(BitWidth));
    KnownBits Op2Known(BitWidth);
    if (SimplifyDemandedBits(II, 2, Op2Demanded, Op2Known))
      return &CI;
    break;
  }
  case Intrinsic::uadd_with_overflow:
  case Intrinsic::sadd_with_overflow: {
    if (Instruction *I = foldIntrinsicWithOverflowCommon(II))
      return I;

    // Given 2 constant operands whose sum does not overflow:
    // uaddo (X +nuw C0), C1 -> uaddo X, C0 + C1
    // saddo (X +nsw C0), C1 -> saddo X, C0 + C1
    Value *X;
    const APInt *C0, *C1;
    Value *Arg0 = II->getArgOperand(0);
    Value *Arg1 = II->getArgOperand(1);
    bool IsSigned = IID == Intrinsic::sadd_with_overflow;
    bool HasNWAdd = IsSigned ? match(Arg0, m_NSWAdd(m_Value(X), m_APInt(C0)))
                             : match(Arg0, m_NUWAdd(m_Value(X), m_APInt(C0)));
    if (HasNWAdd && match(Arg1, m_APInt(C1))) {
      bool Overflow;
      APInt NewC =
          IsSigned ? C1->sadd_ov(*C0, Overflow) : C1->uadd_ov(*C0, Overflow);
      if (!Overflow)
        return replaceInstUsesWith(
            *II, Builder.CreateBinaryIntrinsic(
                     IID, X, ConstantInt::get(Arg1->getType(), NewC)));
    }
    break;
  }

  case Intrinsic::umul_with_overflow:
  case Intrinsic::smul_with_overflow:
  case Intrinsic::usub_with_overflow:
    if (Instruction *I = foldIntrinsicWithOverflowCommon(II))
      return I;
    break;

  case Intrinsic::ssub_with_overflow: {
    if (Instruction *I = foldIntrinsicWithOverflowCommon(II))
      return I;

    Constant *C;
    Value *Arg0 = II->getArgOperand(0);
    Value *Arg1 = II->getArgOperand(1);
    // Given a constant C that is not the minimum signed value
    // for an integer of a given bit width:
    //
    // ssubo X, C -> saddo X, -C
    if (match(Arg1, m_Constant(C)) && C->isNotMinSignedValue()) {
      Value *NegVal = ConstantExpr::getNeg(C);
      // Build a saddo call that is equivalent to the discovered
      // ssubo call.
      return replaceInstUsesWith(
          *II, Builder.CreateBinaryIntrinsic(Intrinsic::sadd_with_overflow,
                                             Arg0, NegVal));
    }

    break;
  }

  case Intrinsic::uadd_sat:
  case Intrinsic::sadd_sat:
  case Intrinsic::usub_sat:
  case Intrinsic::ssub_sat: {
    SaturatingInst *SI = cast<SaturatingInst>(II);
    Type *Ty = SI->getType();
    Value *Arg0 = SI->getLHS();
    Value *Arg1 = SI->getRHS();

    // Make use of known overflow information.
    OverflowResult OR = computeOverflow(SI->getBinaryOp(), SI->isSigned(),
                                        Arg0, Arg1, SI);
    switch (OR) {
      case OverflowResult::MayOverflow:
        break;
      case OverflowResult::NeverOverflows:
        if (SI->isSigned())
          return BinaryOperator::CreateNSW(SI->getBinaryOp(), Arg0, Arg1);
        else
          return BinaryOperator::CreateNUW(SI->getBinaryOp(), Arg0, Arg1);
      case OverflowResult::AlwaysOverflowsLow: {
        unsigned BitWidth = Ty->getScalarSizeInBits();
        APInt Min = APSInt::getMinValue(BitWidth, !SI->isSigned());
        return replaceInstUsesWith(*SI, ConstantInt::get(Ty, Min));
      }
      case OverflowResult::AlwaysOverflowsHigh: {
        unsigned BitWidth = Ty->getScalarSizeInBits();
        APInt Max = APSInt::getMaxValue(BitWidth, !SI->isSigned());
        return replaceInstUsesWith(*SI, ConstantInt::get(Ty, Max));
      }
    }

    // ssub.sat(X, C) -> sadd.sat(X, -C) if C != MIN
    Constant *C;
    if (IID == Intrinsic::ssub_sat && match(Arg1, m_Constant(C)) &&
        C->isNotMinSignedValue()) {
      Value *NegVal = ConstantExpr::getNeg(C);
      return replaceInstUsesWith(
          *II, Builder.CreateBinaryIntrinsic(
              Intrinsic::sadd_sat, Arg0, NegVal));
    }

    // sat(sat(X + Val2) + Val) -> sat(X + (Val+Val2))
    // sat(sat(X - Val2) - Val) -> sat(X - (Val+Val2))
    // if Val and Val2 have the same sign
    if (auto *Other = dyn_cast<IntrinsicInst>(Arg0)) {
      Value *X;
      const APInt *Val, *Val2;
      APInt NewVal;
      bool IsUnsigned =
          IID == Intrinsic::uadd_sat || IID == Intrinsic::usub_sat;
      if (Other->getIntrinsicID() == IID &&
          match(Arg1, m_APInt(Val)) &&
          match(Other->getArgOperand(0), m_Value(X)) &&
          match(Other->getArgOperand(1), m_APInt(Val2))) {
        if (IsUnsigned)
          NewVal = Val->uadd_sat(*Val2);
        else if (Val->isNonNegative() == Val2->isNonNegative()) {
          bool Overflow;
          NewVal = Val->sadd_ov(*Val2, Overflow);
          if (Overflow) {
            // Both adds together may add more than SignedMaxValue
            // without saturating the final result.
            break;
          }
        } else {
          // Cannot fold saturated addition with different signs.
          break;
        }

        return replaceInstUsesWith(
            *II, Builder.CreateBinaryIntrinsic(
                     IID, X, ConstantInt::get(II->getType(), NewVal)));
      }
    }
    break;
  }

  case Intrinsic::minnum:
  case Intrinsic::maxnum:
  case Intrinsic::minimum:
  case Intrinsic::maximum: {
    Value *Arg0 = II->getArgOperand(0);
    Value *Arg1 = II->getArgOperand(1);
    Value *X, *Y;
    if (match(Arg0, m_FNeg(m_Value(X))) && match(Arg1, m_FNeg(m_Value(Y))) &&
        (Arg0->hasOneUse() || Arg1->hasOneUse())) {
      // If both operands are negated, invert the call and negate the result:
      // min(-X, -Y) --> -(max(X, Y))
      // max(-X, -Y) --> -(min(X, Y))
      Intrinsic::ID NewIID;
      switch (IID) {
      case Intrinsic::maxnum:
        NewIID = Intrinsic::minnum;
        break;
      case Intrinsic::minnum:
        NewIID = Intrinsic::maxnum;
        break;
      case Intrinsic::maximum:
        NewIID = Intrinsic::minimum;
        break;
      case Intrinsic::minimum:
        NewIID = Intrinsic::maximum;
        break;
      default:
        llvm_unreachable("unexpected intrinsic ID");
      }
      Value *NewCall = Builder.CreateBinaryIntrinsic(NewIID, X, Y, II);
      Instruction *FNeg = UnaryOperator::CreateFNeg(NewCall);
      FNeg->copyIRFlags(II);
      return FNeg;
    }

    // m(m(X, C2), C1) -> m(X, C)
    const APFloat *C1, *C2;
    if (auto *M = dyn_cast<IntrinsicInst>(Arg0)) {
      if (M->getIntrinsicID() == IID && match(Arg1, m_APFloat(C1)) &&
          ((match(M->getArgOperand(0), m_Value(X)) &&
            match(M->getArgOperand(1), m_APFloat(C2))) ||
           (match(M->getArgOperand(1), m_Value(X)) &&
            match(M->getArgOperand(0), m_APFloat(C2))))) {
        APFloat Res(0.0);
        switch (IID) {
        case Intrinsic::maxnum:
          Res = maxnum(*C1, *C2);
          break;
        case Intrinsic::minnum:
          Res = minnum(*C1, *C2);
          break;
        case Intrinsic::maximum:
          Res = maximum(*C1, *C2);
          break;
        case Intrinsic::minimum:
          Res = minimum(*C1, *C2);
          break;
        default:
          llvm_unreachable("unexpected intrinsic ID");
        }
        Instruction *NewCall = Builder.CreateBinaryIntrinsic(
            IID, X, ConstantFP::get(Arg0->getType(), Res), II);
        // TODO: Conservatively intersecting FMF. If Res == C2, the transform
        //       was a simplification (so Arg0 and its original flags could
        //       propagate?)
        NewCall->andIRFlags(M);
        return replaceInstUsesWith(*II, NewCall);
      }
    }

    Value *ExtSrc0;
    Value *ExtSrc1;

    // minnum (fpext x), (fpext y) -> minnum x, y
    // maxnum (fpext x), (fpext y) -> maxnum x, y
    if (match(II->getArgOperand(0), m_OneUse(m_FPExt(m_Value(ExtSrc0)))) &&
        match(II->getArgOperand(1), m_OneUse(m_FPExt(m_Value(ExtSrc1)))) &&
        ExtSrc0->getType() == ExtSrc1->getType()) {
      Function *F = Intrinsic::getDeclaration(
          II->getModule(), II->getIntrinsicID(), {ExtSrc0->getType()});
      CallInst *NewCall = Builder.CreateCall(F, { ExtSrc0, ExtSrc1 });
      NewCall->copyFastMathFlags(II);
      NewCall->takeName(II);
      return new FPExtInst(NewCall, II->getType());
    }

    break;
  }
  case Intrinsic::fmuladd: {
    // Canonicalize fast fmuladd to the separate fmul + fadd.
    if (II->isFast()) {
      BuilderTy::FastMathFlagGuard Guard(Builder);
      Builder.setFastMathFlags(II->getFastMathFlags());
      Value *Mul = Builder.CreateFMul(II->getArgOperand(0),
                                      II->getArgOperand(1));
      Value *Add = Builder.CreateFAdd(Mul, II->getArgOperand(2));
      Add->takeName(II);
      return replaceInstUsesWith(*II, Add);
    }

    // Try to simplify the underlying FMul.
    if (Value *V = SimplifyFMulInst(II->getArgOperand(0), II->getArgOperand(1),
                                    II->getFastMathFlags(),
                                    SQ.getWithInstruction(II))) {
      auto *FAdd = BinaryOperator::CreateFAdd(V, II->getArgOperand(2));
      FAdd->copyFastMathFlags(II);
      return FAdd;
    }

    LLVM_FALLTHROUGH;
  }
  case Intrinsic::fma: {
    // fma fneg(x), fneg(y), z -> fma x, y, z
    Value *Src0 = II->getArgOperand(0);
    Value *Src1 = II->getArgOperand(1);
    Value *X, *Y;
    if (match(Src0, m_FNeg(m_Value(X))) && match(Src1, m_FNeg(m_Value(Y)))) {
      replaceOperand(*II, 0, X);
      replaceOperand(*II, 1, Y);
      return II;
    }

    // fma fabs(x), fabs(x), z -> fma x, x, z
    if (match(Src0, m_FAbs(m_Value(X))) &&
        match(Src1, m_FAbs(m_Specific(X)))) {
      replaceOperand(*II, 0, X);
      replaceOperand(*II, 1, X);
      return II;
    }

    // Try to simplify the underlying FMul. We can only apply simplifications
    // that do not require rounding.
    if (Value *V = SimplifyFMAFMul(II->getArgOperand(0), II->getArgOperand(1),
                                   II->getFastMathFlags(),
                                   SQ.getWithInstruction(II))) {
      auto *FAdd = BinaryOperator::CreateFAdd(V, II->getArgOperand(2));
      FAdd->copyFastMathFlags(II);
      return FAdd;
    }

    // fma x, y, 0 -> fmul x, y
    // This is always valid for -0.0, but requires nsz for +0.0 as
    // -0.0 + 0.0 = 0.0, which would not be the same as the fmul on its own.
    if (match(II->getArgOperand(2), m_NegZeroFP()) ||
        (match(II->getArgOperand(2), m_PosZeroFP()) &&
         II->getFastMathFlags().noSignedZeros()))
      return BinaryOperator::CreateFMulFMF(Src0, Src1, II);

    break;
  }
  case Intrinsic::copysign: {
    Value *Mag = II->getArgOperand(0), *Sign = II->getArgOperand(1);
    if (SignBitMustBeZero(Sign, &TLI)) {
      // If we know that the sign argument is positive, reduce to FABS:
      // copysign Mag, +Sign --> fabs Mag
      Value *Fabs = Builder.CreateUnaryIntrinsic(Intrinsic::fabs, Mag, II);
      return replaceInstUsesWith(*II, Fabs);
    }
    // TODO: There should be a ValueTracking sibling like SignBitMustBeOne.
    const APFloat *C;
    if (match(Sign, m_APFloat(C)) && C->isNegative()) {
      // If we know that the sign argument is negative, reduce to FNABS:
      // copysign Mag, -Sign --> fneg (fabs Mag)
      Value *Fabs = Builder.CreateUnaryIntrinsic(Intrinsic::fabs, Mag, II);
      return replaceInstUsesWith(*II, Builder.CreateFNegFMF(Fabs, II));
    }

    // Propagate sign argument through nested calls:
    // copysign Mag, (copysign ?, X) --> copysign Mag, X
    Value *X;
    if (match(Sign, m_Intrinsic<Intrinsic::copysign>(m_Value(), m_Value(X))))
      return replaceOperand(*II, 1, X);

    // Peek through changes of magnitude's sign-bit. This call rewrites those:
    // copysign (fabs X), Sign --> copysign X, Sign
    // copysign (fneg X), Sign --> copysign X, Sign
    if (match(Mag, m_FAbs(m_Value(X))) || match(Mag, m_FNeg(m_Value(X))))
      return replaceOperand(*II, 0, X);

    break;
  }
  case Intrinsic::fabs: {
    Value *Cond, *TVal, *FVal;
    if (match(II->getArgOperand(0),
              m_Select(m_Value(Cond), m_Value(TVal), m_Value(FVal)))) {
      // fabs (select Cond, TrueC, FalseC) --> select Cond, AbsT, AbsF
      if (isa<Constant>(TVal) && isa<Constant>(FVal)) {
        CallInst *AbsT = Builder.CreateCall(II->getCalledFunction(), {TVal});
        CallInst *AbsF = Builder.CreateCall(II->getCalledFunction(), {FVal});
        return SelectInst::Create(Cond, AbsT, AbsF);
      }
      // fabs (select Cond, -FVal, FVal) --> fabs FVal
      if (match(TVal, m_FNeg(m_Specific(FVal))))
        return replaceOperand(*II, 0, FVal);
      // fabs (select Cond, TVal, -TVal) --> fabs TVal
      if (match(FVal, m_FNeg(m_Specific(TVal))))
        return replaceOperand(*II, 0, TVal);
    }

    LLVM_FALLTHROUGH;
  }
  case Intrinsic::ceil:
  case Intrinsic::floor:
  case Intrinsic::round:
  case Intrinsic::roundeven:
  case Intrinsic::nearbyint:
  case Intrinsic::rint:
  case Intrinsic::trunc: {
    Value *ExtSrc;
    if (match(II->getArgOperand(0), m_OneUse(m_FPExt(m_Value(ExtSrc))))) {
      // Narrow the call: intrinsic (fpext x) -> fpext (intrinsic x)
      Value *NarrowII = Builder.CreateUnaryIntrinsic(IID, ExtSrc, II);
      return new FPExtInst(NarrowII, II->getType());
    }
    break;
  }
  case Intrinsic::cos:
  case Intrinsic::amdgcn_cos: {
    Value *X;
    Value *Src = II->getArgOperand(0);
    if (match(Src, m_FNeg(m_Value(X))) || match(Src, m_FAbs(m_Value(X)))) {
      // cos(-x) -> cos(x)
      // cos(fabs(x)) -> cos(x)
      return replaceOperand(*II, 0, X);
    }
    break;
  }
  case Intrinsic::sin: {
    Value *X;
    if (match(II->getArgOperand(0), m_OneUse(m_FNeg(m_Value(X))))) {
      // sin(-x) --> -sin(x)
      Value *NewSin = Builder.CreateUnaryIntrinsic(Intrinsic::sin, X, II);
      Instruction *FNeg = UnaryOperator::CreateFNeg(NewSin);
      FNeg->copyFastMathFlags(II);
      return FNeg;
    }
    break;
  }

  case Intrinsic::arm_neon_vtbl1:
  case Intrinsic::aarch64_neon_tbl1:
    if (Value *V = simplifyNeonTbl1(*II, Builder))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::arm_neon_vmulls:
  case Intrinsic::arm_neon_vmullu:
  case Intrinsic::aarch64_neon_smull:
  case Intrinsic::aarch64_neon_umull: {
    Value *Arg0 = II->getArgOperand(0);
    Value *Arg1 = II->getArgOperand(1);

    // Handle mul by zero first:
    if (isa<ConstantAggregateZero>(Arg0) || isa<ConstantAggregateZero>(Arg1)) {
      return replaceInstUsesWith(CI, ConstantAggregateZero::get(II->getType()));
    }

    // Check for constant LHS & RHS - in this case we just simplify.
    bool Zext = (IID == Intrinsic::arm_neon_vmullu ||
                 IID == Intrinsic::aarch64_neon_umull);
    VectorType *NewVT = cast<VectorType>(II->getType());
    if (Constant *CV0 = dyn_cast<Constant>(Arg0)) {
      if (Constant *CV1 = dyn_cast<Constant>(Arg1)) {
        CV0 = ConstantExpr::getIntegerCast(CV0, NewVT, /*isSigned=*/!Zext);
        CV1 = ConstantExpr::getIntegerCast(CV1, NewVT, /*isSigned=*/!Zext);

        return replaceInstUsesWith(CI, ConstantExpr::getMul(CV0, CV1));
      }

      // Couldn't simplify - canonicalize constant to the RHS.
      std::swap(Arg0, Arg1);
    }

    // Handle mul by one:
    if (Constant *CV1 = dyn_cast<Constant>(Arg1))
      if (ConstantInt *Splat =
              dyn_cast_or_null<ConstantInt>(CV1->getSplatValue()))
        if (Splat->isOne())
          return CastInst::CreateIntegerCast(Arg0, II->getType(),
                                             /*isSigned=*/!Zext);

    break;
  }
  case Intrinsic::arm_neon_aesd:
  case Intrinsic::arm_neon_aese:
  case Intrinsic::aarch64_crypto_aesd:
  case Intrinsic::aarch64_crypto_aese: {
    Value *DataArg = II->getArgOperand(0);
    Value *KeyArg  = II->getArgOperand(1);

    // Try to use the builtin XOR in AESE and AESD to eliminate a prior XOR
    Value *Data, *Key;
    if (match(KeyArg, m_ZeroInt()) &&
        match(DataArg, m_Xor(m_Value(Data), m_Value(Key)))) {
      replaceOperand(*II, 0, Data);
      replaceOperand(*II, 1, Key);
      return II;
    }
    break;
  }
  case Intrinsic::hexagon_V6_vandvrt:
  case Intrinsic::hexagon_V6_vandvrt_128B: {
    // Simplify Q -> V -> Q conversion.
    if (auto Op0 = dyn_cast<IntrinsicInst>(II->getArgOperand(0))) {
      Intrinsic::ID ID0 = Op0->getIntrinsicID();
      if (ID0 != Intrinsic::hexagon_V6_vandqrt &&
          ID0 != Intrinsic::hexagon_V6_vandqrt_128B)
        break;
      Value *Bytes = Op0->getArgOperand(1), *Mask = II->getArgOperand(1);
      uint64_t Bytes1 = computeKnownBits(Bytes, 0, Op0).One.getZExtValue();
      uint64_t Mask1 = computeKnownBits(Mask, 0, II).One.getZExtValue();
      // Check if every byte has common bits in Bytes and Mask.
      uint64_t C = Bytes1 & Mask1;
      if ((C & 0xFF) && (C & 0xFF00) && (C & 0xFF0000) && (C & 0xFF000000))
        return replaceInstUsesWith(*II, Op0->getArgOperand(0));
    }
    break;
  }
  case Intrinsic::stackrestore: {
    // If the save is right next to the restore, remove the restore.  This can
    // happen when variable allocas are DCE'd.
    if (IntrinsicInst *SS = dyn_cast<IntrinsicInst>(II->getArgOperand(0))) {
      if (SS->getIntrinsicID() == Intrinsic::stacksave) {
        // Skip over debug info.
        if (SS->getNextNonDebugInstruction() == II) {
          return eraseInstFromFunction(CI);
        }
      }
    }

    // Scan down this block to see if there is another stack restore in the
    // same block without an intervening call/alloca.
    BasicBlock::iterator BI(II);
    Instruction *TI = II->getParent()->getTerminator();
    bool CannotRemove = false;
    for (++BI; &*BI != TI; ++BI) {
      if (isa<AllocaInst>(BI)) {
        CannotRemove = true;
        break;
      }
      if (CallInst *BCI = dyn_cast<CallInst>(BI)) {
        if (auto *II2 = dyn_cast<IntrinsicInst>(BCI)) {
          // If there is a stackrestore below this one, remove this one.
          if (II2->getIntrinsicID() == Intrinsic::stackrestore)
            return eraseInstFromFunction(CI);

          // Bail if we cross over an intrinsic with side effects, such as
          // llvm.stacksave, or llvm.read_register.
          if (II2->mayHaveSideEffects()) {
            CannotRemove = true;
            break;
          }
        } else {
          // If we found a non-intrinsic call, we can't remove the stack
          // restore.
          CannotRemove = true;
          break;
        }
      }
    }

    // If the stack restore is in a return, resume, or unwind block and if there
    // are no allocas or calls between the restore and the return, nuke the
    // restore.
    if (!CannotRemove && (isa<ReturnInst>(TI) || isa<ResumeInst>(TI)))
      return eraseInstFromFunction(CI);
    break;
  }
  case Intrinsic::lifetime_end:
    // Asan needs to poison memory to detect invalid access which is possible
    // even for empty lifetime range.
    if (II->getFunction()->hasFnAttribute(Attribute::SanitizeAddress) ||
        II->getFunction()->hasFnAttribute(Attribute::SanitizeMemory) ||
        II->getFunction()->hasFnAttribute(Attribute::SanitizeHWAddress))
      break;

    if (removeTriviallyEmptyRange(*II, *this, [](const IntrinsicInst &I) {
          return I.getIntrinsicID() == Intrinsic::lifetime_start;
        }))
      return nullptr;
    break;
  case Intrinsic::assume: {
    Value *IIOperand = II->getArgOperand(0);
    SmallVector<OperandBundleDef, 4> OpBundles;
    II->getOperandBundlesAsDefs(OpBundles);

    /// This will remove the boolean Condition from the assume given as
    /// argument and remove the assume if it becomes useless.
    /// always returns nullptr for use as a return values.
    auto RemoveConditionFromAssume = [&](Instruction *Assume) -> Instruction * {
      assert(isa<IntrinsicInst>(Assume));
      if (isAssumeWithEmptyBundle(*cast<IntrinsicInst>(II)))
        return eraseInstFromFunction(CI);
      replaceUse(II->getOperandUse(0), ConstantInt::getTrue(II->getContext()));
      return nullptr;
    };
    // Remove an assume if it is followed by an identical assume.
    // TODO: Do we need this? Unless there are conflicting assumptions, the
    // computeKnownBits(IIOperand) below here eliminates redundant assumes.
    Instruction *Next = II->getNextNonDebugInstruction();
    if (match(Next, m_Intrinsic<Intrinsic::assume>(m_Specific(IIOperand))))
      return RemoveConditionFromAssume(Next);

    // Canonicalize assume(a && b) -> assume(a); assume(b);
    // Note: New assumption intrinsics created here are registered by
    // the InstCombineIRInserter object.
    FunctionType *AssumeIntrinsicTy = II->getFunctionType();
    Value *AssumeIntrinsic = II->getCalledOperand();
    Value *A, *B;
    if (match(IIOperand, m_LogicalAnd(m_Value(A), m_Value(B)))) {
      Builder.CreateCall(AssumeIntrinsicTy, AssumeIntrinsic, A, OpBundles,
                         II->getName());
      Builder.CreateCall(AssumeIntrinsicTy, AssumeIntrinsic, B, II->getName());
      return eraseInstFromFunction(*II);
    }
    // assume(!(a || b)) -> assume(!a); assume(!b);
    if (match(IIOperand, m_Not(m_LogicalOr(m_Value(A), m_Value(B))))) {
      Builder.CreateCall(AssumeIntrinsicTy, AssumeIntrinsic,
                         Builder.CreateNot(A), OpBundles, II->getName());
      Builder.CreateCall(AssumeIntrinsicTy, AssumeIntrinsic,
                         Builder.CreateNot(B), II->getName());
      return eraseInstFromFunction(*II);
    }

    // assume( (load addr) != null ) -> add 'nonnull' metadata to load
    // (if assume is valid at the load)
    CmpInst::Predicate Pred;
    Instruction *LHS;
    if (match(IIOperand, m_ICmp(Pred, m_Instruction(LHS), m_Zero())) &&
        Pred == ICmpInst::ICMP_NE && LHS->getOpcode() == Instruction::Load &&
        LHS->getType()->isPointerTy() &&
        isValidAssumeForContext(II, LHS, &DT)) {
      MDNode *MD = MDNode::get(II->getContext(), None);
      LHS->setMetadata(LLVMContext::MD_nonnull, MD);
      return RemoveConditionFromAssume(II);

      // TODO: apply nonnull return attributes to calls and invokes
      // TODO: apply range metadata for range check patterns?
    }

    // Convert nonnull assume like:
    // %A = icmp ne i32* %PTR, null
    // call void @llvm.assume(i1 %A)
    // into
    // call void @llvm.assume(i1 true) [ "nonnull"(i32* %PTR) ]
    if (EnableKnowledgeRetention &&
        match(IIOperand, m_Cmp(Pred, m_Value(A), m_Zero())) &&
        Pred == CmpInst::ICMP_NE && A->getType()->isPointerTy()) {
      if (IntrinsicInst *Replacement = buildAssumeFromKnowledge(
              {RetainedKnowledge{Attribute::NonNull, 0, A}}, Next, &AC, &DT)) {

        Replacement->insertBefore(Next);
        AC.registerAssumption(Replacement);
        return RemoveConditionFromAssume(II);
      }
    }

    // Convert alignment assume like:
    // %B = ptrtoint i32* %A to i64
    // %C = and i64 %B, Constant
    // %D = icmp eq i64 %C, 0
    // call void @llvm.assume(i1 %D)
    // into
    // call void @llvm.assume(i1 true) [ "align"(i32* [[A]], i64  Constant + 1)]
    uint64_t AlignMask;
    if (EnableKnowledgeRetention &&
        match(IIOperand,
              m_Cmp(Pred, m_And(m_Value(A), m_ConstantInt(AlignMask)),
                    m_Zero())) &&
        Pred == CmpInst::ICMP_EQ) {
      if (isPowerOf2_64(AlignMask + 1)) {
        uint64_t Offset = 0;
        match(A, m_Add(m_Value(A), m_ConstantInt(Offset)));
        if (match(A, m_PtrToInt(m_Value(A)))) {
          /// Note: this doesn't preserve the offset information but merges
          /// offset and alignment.
          /// TODO: we can generate a GEP instead of merging the alignment with
          /// the offset.
          RetainedKnowledge RK{Attribute::Alignment,
                               (unsigned)MinAlign(Offset, AlignMask + 1), A};
          if (IntrinsicInst *Replacement =
                  buildAssumeFromKnowledge(RK, Next, &AC, &DT)) {

            Replacement->insertAfter(II);
            AC.registerAssumption(Replacement);
          }
          return RemoveConditionFromAssume(II);
        }
      }
    }

    /// Canonicalize Knowledge in operand bundles.
    if (EnableKnowledgeRetention && II->hasOperandBundles()) {
      for (unsigned Idx = 0; Idx < II->getNumOperandBundles(); Idx++) {
        auto &BOI = II->bundle_op_info_begin()[Idx];
        RetainedKnowledge RK = llvm::getKnowledgeFromBundle(*II, BOI);
        if (BOI.End - BOI.Begin > 2)
          continue; // Prevent reducing knowledge in an align with offset since
                    // extracting a RetainedKnowledge form them looses offset
                    // information
        RetainedKnowledge CanonRK = llvm::simplifyRetainedKnowledge(
            II, RK, &getAssumptionCache(), &getDominatorTree());
        if (CanonRK == RK)
          continue;
        if (!CanonRK) {
          if (BOI.End - BOI.Begin > 0) {
            Worklist.pushValue(II->op_begin()[BOI.Begin]);
            Value::dropDroppableUse(II->op_begin()[BOI.Begin]);
          }
          continue;
        }
        assert(RK.AttrKind == CanonRK.AttrKind);
        if (BOI.End - BOI.Begin > 0)
          II->op_begin()[BOI.Begin].set(CanonRK.WasOn);
        if (BOI.End - BOI.Begin > 1)
          II->op_begin()[BOI.Begin + 1].set(ConstantInt::get(
              Type::getInt64Ty(II->getContext()), CanonRK.ArgValue));
        if (RK.WasOn)
          Worklist.pushValue(RK.WasOn);
        return II;
      }
    }

    // If there is a dominating assume with the same condition as this one,
    // then this one is redundant, and should be removed.
    KnownBits Known(1);
    computeKnownBits(IIOperand, Known, 0, II);
    if (Known.isAllOnes() && isAssumeWithEmptyBundle(*II))
      return eraseInstFromFunction(*II);

    // Update the cache of affected values for this assumption (we might be
    // here because we just simplified the condition).
    AC.updateAffectedValues(II);
    break;
  }
  case Intrinsic::experimental_gc_statepoint: {
    GCStatepointInst &GCSP = *cast<GCStatepointInst>(II);
    SmallPtrSet<Value *, 32> LiveGcValues;
    for (const GCRelocateInst *Reloc : GCSP.getGCRelocates()) {
      GCRelocateInst &GCR = *const_cast<GCRelocateInst *>(Reloc);

      // Remove the relocation if unused.
      if (GCR.use_empty()) {
        eraseInstFromFunction(GCR);
        continue;
      }

      Value *DerivedPtr = GCR.getDerivedPtr();
      Value *BasePtr = GCR.getBasePtr();

      // Undef is undef, even after relocation.
      if (isa<UndefValue>(DerivedPtr) || isa<UndefValue>(BasePtr)) {
        replaceInstUsesWith(GCR, UndefValue::get(GCR.getType()));
        eraseInstFromFunction(GCR);
        continue;
      }

      if (auto *PT = dyn_cast<PointerType>(GCR.getType())) {
        // The relocation of null will be null for most any collector.
        // TODO: provide a hook for this in GCStrategy.  There might be some
        // weird collector this property does not hold for.
        if (isa<ConstantPointerNull>(DerivedPtr)) {
          // Use null-pointer of gc_relocate's type to replace it.
          replaceInstUsesWith(GCR, ConstantPointerNull::get(PT));
          eraseInstFromFunction(GCR);
          continue;
        }

        // isKnownNonNull -> nonnull attribute
        if (!GCR.hasRetAttr(Attribute::NonNull) &&
            isKnownNonZero(DerivedPtr, DL, 0, &AC, II, &DT)) {
          GCR.addAttribute(AttributeList::ReturnIndex, Attribute::NonNull);
          // We discovered new fact, re-check users.
          Worklist.pushUsersToWorkList(GCR);
        }
      }

      // If we have two copies of the same pointer in the statepoint argument
      // list, canonicalize to one.  This may let us common gc.relocates.
      if (GCR.getBasePtr() == GCR.getDerivedPtr() &&
          GCR.getBasePtrIndex() != GCR.getDerivedPtrIndex()) {
        auto *OpIntTy = GCR.getOperand(2)->getType();
        GCR.setOperand(2, ConstantInt::get(OpIntTy, GCR.getBasePtrIndex()));
      }

      // TODO: bitcast(relocate(p)) -> relocate(bitcast(p))
      // Canonicalize on the type from the uses to the defs

      // TODO: relocate((gep p, C, C2, ...)) -> gep(relocate(p), C, C2, ...)
      LiveGcValues.insert(BasePtr);
      LiveGcValues.insert(DerivedPtr);
    }
    Optional<OperandBundleUse> Bundle =
        GCSP.getOperandBundle(LLVMContext::OB_gc_live);
    unsigned NumOfGCLives = LiveGcValues.size();
    if (!Bundle.hasValue() || NumOfGCLives == Bundle->Inputs.size())
      break;
    // We can reduce the size of gc live bundle.
    DenseMap<Value *, unsigned> Val2Idx;
    std::vector<Value *> NewLiveGc;
    for (unsigned I = 0, E = Bundle->Inputs.size(); I < E; ++I) {
      Value *V = Bundle->Inputs[I];
      if (Val2Idx.count(V))
        continue;
      if (LiveGcValues.count(V)) {
        Val2Idx[V] = NewLiveGc.size();
        NewLiveGc.push_back(V);
      } else
        Val2Idx[V] = NumOfGCLives;
    }
    // Update all gc.relocates
    for (const GCRelocateInst *Reloc : GCSP.getGCRelocates()) {
      GCRelocateInst &GCR = *const_cast<GCRelocateInst *>(Reloc);
      Value *BasePtr = GCR.getBasePtr();
      assert(Val2Idx.count(BasePtr) && Val2Idx[BasePtr] != NumOfGCLives &&
             "Missed live gc for base pointer");
      auto *OpIntTy1 = GCR.getOperand(1)->getType();
      GCR.setOperand(1, ConstantInt::get(OpIntTy1, Val2Idx[BasePtr]));
      Value *DerivedPtr = GCR.getDerivedPtr();
      assert(Val2Idx.count(DerivedPtr) && Val2Idx[DerivedPtr] != NumOfGCLives &&
             "Missed live gc for derived pointer");
      auto *OpIntTy2 = GCR.getOperand(2)->getType();
      GCR.setOperand(2, ConstantInt::get(OpIntTy2, Val2Idx[DerivedPtr]));
    }
    // Create new statepoint instruction.
    OperandBundleDef NewBundle("gc-live", NewLiveGc);
    if (isa<CallInst>(II))
      return CallInst::CreateWithReplacedBundle(cast<CallInst>(II), NewBundle);
    else
      return InvokeInst::CreateWithReplacedBundle(cast<InvokeInst>(II),
                                                  NewBundle);
    break;
  }
  case Intrinsic::experimental_guard: {
    // Is this guard followed by another guard?  We scan forward over a small
    // fixed window of instructions to handle common cases with conditions
    // computed between guards.
    Instruction *NextInst = II->getNextNonDebugInstruction();
    for (unsigned i = 0; i < GuardWideningWindow; i++) {
      // Note: Using context-free form to avoid compile time blow up
      if (!isSafeToSpeculativelyExecute(NextInst))
        break;
      NextInst = NextInst->getNextNonDebugInstruction();
    }
    Value *NextCond = nullptr;
    if (match(NextInst,
              m_Intrinsic<Intrinsic::experimental_guard>(m_Value(NextCond)))) {
      Value *CurrCond = II->getArgOperand(0);

      // Remove a guard that it is immediately preceded by an identical guard.
      // Otherwise canonicalize guard(a); guard(b) -> guard(a & b).
      if (CurrCond != NextCond) {
        Instruction *MoveI = II->getNextNonDebugInstruction();
        while (MoveI != NextInst) {
          auto *Temp = MoveI;
          MoveI = MoveI->getNextNonDebugInstruction();
          Temp->moveBefore(II);
        }
        replaceOperand(*II, 0, Builder.CreateAnd(CurrCond, NextCond));
      }
      eraseInstFromFunction(*NextInst);
      return II;
    }
    break;
  }
  case Intrinsic::experimental_vector_insert: {
    Value *Vec = II->getArgOperand(0);
    Value *SubVec = II->getArgOperand(1);
    Value *Idx = II->getArgOperand(2);
    auto *DstTy = dyn_cast<FixedVectorType>(II->getType());
    auto *VecTy = dyn_cast<FixedVectorType>(Vec->getType());
    auto *SubVecTy = dyn_cast<FixedVectorType>(SubVec->getType());

    // Only canonicalize if the destination vector, Vec, and SubVec are all
    // fixed vectors.
    if (DstTy && VecTy && SubVecTy) {
      unsigned DstNumElts = DstTy->getNumElements();
      unsigned VecNumElts = VecTy->getNumElements();
      unsigned SubVecNumElts = SubVecTy->getNumElements();
      unsigned IdxN = cast<ConstantInt>(Idx)->getZExtValue();

      // The result of this call is undefined if IdxN is not a constant multiple
      // of the SubVec's minimum vector length OR the insertion overruns Vec.
      if (IdxN % SubVecNumElts != 0 || IdxN + SubVecNumElts > VecNumElts) {
        replaceInstUsesWith(CI, UndefValue::get(CI.getType()));
        return eraseInstFromFunction(CI);
      }

      // An insert that entirely overwrites Vec with SubVec is a nop.
      if (VecNumElts == SubVecNumElts) {
        replaceInstUsesWith(CI, SubVec);
        return eraseInstFromFunction(CI);
      }

      // Widen SubVec into a vector of the same width as Vec, since
      // shufflevector requires the two input vectors to be the same width.
      // Elements beyond the bounds of SubVec within the widened vector are
      // undefined.
      SmallVector<int, 8> WidenMask;
      unsigned i;
      for (i = 0; i != SubVecNumElts; ++i)
        WidenMask.push_back(i);
      for (; i != VecNumElts; ++i)
        WidenMask.push_back(UndefMaskElem);

      Value *WidenShuffle = Builder.CreateShuffleVector(SubVec, WidenMask);

      SmallVector<int, 8> Mask;
      for (unsigned i = 0; i != IdxN; ++i)
        Mask.push_back(i);
      for (unsigned i = DstNumElts; i != DstNumElts + SubVecNumElts; ++i)
        Mask.push_back(i);
      for (unsigned i = IdxN + SubVecNumElts; i != DstNumElts; ++i)
        Mask.push_back(i);

      Value *Shuffle = Builder.CreateShuffleVector(Vec, WidenShuffle, Mask);
      replaceInstUsesWith(CI, Shuffle);
      return eraseInstFromFunction(CI);
    }
    break;
  }
  case Intrinsic::experimental_vector_extract: {
    Value *Vec = II->getArgOperand(0);
    Value *Idx = II->getArgOperand(1);

    auto *DstTy = dyn_cast<FixedVectorType>(II->getType());
    auto *VecTy = dyn_cast<FixedVectorType>(Vec->getType());

    // Only canonicalize if the the destination vector and Vec are fixed
    // vectors.
    if (DstTy && VecTy) {
      unsigned DstNumElts = DstTy->getNumElements();
      unsigned VecNumElts = VecTy->getNumElements();
      unsigned IdxN = cast<ConstantInt>(Idx)->getZExtValue();

      // The result of this call is undefined if IdxN is not a constant multiple
      // of the result type's minimum vector length OR the extraction overruns
      // Vec.
      if (IdxN % DstNumElts != 0 || IdxN + DstNumElts > VecNumElts) {
        replaceInstUsesWith(CI, UndefValue::get(CI.getType()));
        return eraseInstFromFunction(CI);
      }

      // Extracting the entirety of Vec is a nop.
      if (VecNumElts == DstNumElts) {
        replaceInstUsesWith(CI, Vec);
        return eraseInstFromFunction(CI);
      }

      SmallVector<int, 8> Mask;
      for (unsigned i = 0; i != DstNumElts; ++i)
        Mask.push_back(IdxN + i);

      Value *Shuffle =
          Builder.CreateShuffleVector(Vec, UndefValue::get(VecTy), Mask);
      replaceInstUsesWith(CI, Shuffle);
      return eraseInstFromFunction(CI);
    }
    break;
  }
  default: {
    // Handle target specific intrinsics
    Optional<Instruction *> V = targetInstCombineIntrinsic(*II);
    if (V.hasValue())
      return V.getValue();
    break;
  }
  }
  return visitCallBase(*II);
}

// Fence instruction simplification
Instruction *InstCombinerImpl::visitFenceInst(FenceInst &FI) {
  // Remove identical consecutive fences.
  Instruction *Next = FI.getNextNonDebugInstruction();
  if (auto *NFI = dyn_cast<FenceInst>(Next))
    if (FI.isIdenticalTo(NFI))
      return eraseInstFromFunction(FI);
  return nullptr;
}

// InvokeInst simplification
Instruction *InstCombinerImpl::visitInvokeInst(InvokeInst &II) {
  return visitCallBase(II);
}

// CallBrInst simplification
Instruction *InstCombinerImpl::visitCallBrInst(CallBrInst &CBI) {
  return visitCallBase(CBI);
}

/// If this cast does not affect the value passed through the varargs area, we
/// can eliminate the use of the cast.
static bool isSafeToEliminateVarargsCast(const CallBase &Call,
                                         const DataLayout &DL,
                                         const CastInst *const CI,
                                         const int ix) {
  if (!CI->isLosslessCast())
    return false;

  // If this is a GC intrinsic, avoid munging types.  We need types for
  // statepoint reconstruction in SelectionDAG.
  // TODO: This is probably something which should be expanded to all
  // intrinsics since the entire point of intrinsics is that
  // they are understandable by the optimizer.
  if (isa<GCStatepointInst>(Call) || isa<GCRelocateInst>(Call) ||
      isa<GCResultInst>(Call))
    return false;

  // The size of ByVal or InAlloca arguments is derived from the type, so we
  // can't change to a type with a different size.  If the size were
  // passed explicitly we could avoid this check.
  if (!Call.isPassPointeeByValueArgument(ix))
    return true;

  Type* SrcTy =
            cast<PointerType>(CI->getOperand(0)->getType())->getElementType();
  Type *DstTy = Call.isByValArgument(ix)
                    ? Call.getParamByValType(ix)
                    : cast<PointerType>(CI->getType())->getElementType();
  if (!SrcTy->isSized() || !DstTy->isSized())
    return false;
  if (DL.getTypeAllocSize(SrcTy) != DL.getTypeAllocSize(DstTy))
    return false;
  return true;
}

Instruction *InstCombinerImpl::tryOptimizeCall(CallInst *CI) {
  if (!CI->getCalledFunction()) return nullptr;

  auto InstCombineRAUW = [this](Instruction *From, Value *With) {
    replaceInstUsesWith(*From, With);
  };
  auto InstCombineErase = [this](Instruction *I) {
    eraseInstFromFunction(*I);
  };
  LibCallSimplifier Simplifier(DL, &TLI, ORE, BFI, PSI, InstCombineRAUW,
                               InstCombineErase);
  if (Value *With = Simplifier.optimizeCall(CI, Builder)) {
    ++NumSimplified;
    return CI->use_empty() ? CI : replaceInstUsesWith(*CI, With);
  }

  return nullptr;
}

static IntrinsicInst *findInitTrampolineFromAlloca(Value *TrampMem) {
  // Strip off at most one level of pointer casts, looking for an alloca.  This
  // is good enough in practice and simpler than handling any number of casts.
  Value *Underlying = TrampMem->stripPointerCasts();
  if (Underlying != TrampMem &&
      (!Underlying->hasOneUse() || Underlying->user_back() != TrampMem))
    return nullptr;
  if (!isa<AllocaInst>(Underlying))
    return nullptr;

  IntrinsicInst *InitTrampoline = nullptr;
  for (User *U : TrampMem->users()) {
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(U);
    if (!II)
      return nullptr;
    if (II->getIntrinsicID() == Intrinsic::init_trampoline) {
      if (InitTrampoline)
        // More than one init_trampoline writes to this value.  Give up.
        return nullptr;
      InitTrampoline = II;
      continue;
    }
    if (II->getIntrinsicID() == Intrinsic::adjust_trampoline)
      // Allow any number of calls to adjust.trampoline.
      continue;
    return nullptr;
  }

  // No call to init.trampoline found.
  if (!InitTrampoline)
    return nullptr;

  // Check that the alloca is being used in the expected way.
  if (InitTrampoline->getOperand(0) != TrampMem)
    return nullptr;

  return InitTrampoline;
}

static IntrinsicInst *findInitTrampolineFromBB(IntrinsicInst *AdjustTramp,
                                               Value *TrampMem) {
  // Visit all the previous instructions in the basic block, and try to find a
  // init.trampoline which has a direct path to the adjust.trampoline.
  for (BasicBlock::iterator I = AdjustTramp->getIterator(),
                            E = AdjustTramp->getParent()->begin();
       I != E;) {
    Instruction *Inst = &*--I;
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(I))
      if (II->getIntrinsicID() == Intrinsic::init_trampoline &&
          II->getOperand(0) == TrampMem)
        return II;
    if (Inst->mayWriteToMemory())
      return nullptr;
  }
  return nullptr;
}

// Given a call to llvm.adjust.trampoline, find and return the corresponding
// call to llvm.init.trampoline if the call to the trampoline can be optimized
// to a direct call to a function.  Otherwise return NULL.
static IntrinsicInst *findInitTrampoline(Value *Callee) {
  Callee = Callee->stripPointerCasts();
  IntrinsicInst *AdjustTramp = dyn_cast<IntrinsicInst>(Callee);
  if (!AdjustTramp ||
      AdjustTramp->getIntrinsicID() != Intrinsic::adjust_trampoline)
    return nullptr;

  Value *TrampMem = AdjustTramp->getOperand(0);

  if (IntrinsicInst *IT = findInitTrampolineFromAlloca(TrampMem))
    return IT;
  if (IntrinsicInst *IT = findInitTrampolineFromBB(AdjustTramp, TrampMem))
    return IT;
  return nullptr;
}

Instruction *InstCombinerImpl::tryCombinePtrAuthCall(CallBase &Call) {
  Value *Callee = Call.getCalledOperand();
  auto *IPC = dyn_cast<IntToPtrInst>(Callee);
  if (!IPC || !IPC->isNoopCast(DL))
    return nullptr;

  IntrinsicInst *II = dyn_cast<IntrinsicInst>(IPC->getOperand(0));
  if (!II)
    return nullptr;

  auto PtrAuthBundleOrNone = Call.getOperandBundle(LLVMContext::OB_ptrauth);
  assert(Call.getNumOperandBundles() <= 1 &&
         "unimplemented support for ptrauth and other bundle");

  Value *NewCallee = nullptr;
  SmallVector<OperandBundleDef, 1> NewBundles;
  switch (II->getIntrinsicID()) {
  default:
    return nullptr;

  // call(ptrauth_resign(p)), ["ptrauth"()] ->  call p, ["ptrauth"()]
  // assuming the call bundle and the sign operands match.
  case Intrinsic::ptrauth_resign: {
    if (!PtrAuthBundleOrNone)
      return nullptr;
    auto PtrAuthBundle = *PtrAuthBundleOrNone;
    if (II->getOperand(3) != PtrAuthBundle.Inputs[0] ||
        II->getOperand(4) != PtrAuthBundle.Inputs[1])
      return nullptr;

    Value *NewBundleOps[] = {II->getOperand(1), II->getOperand(2)};
    NewBundles.emplace_back("ptrauth", NewBundleOps);
    NewCallee = II->getOperand(0);
    break;
  }

  // call(ptrauth_sign(p)), ["ptrauth"()] ->  call p
  // assuming the call bundle and the sign operands match.
  case Intrinsic::ptrauth_sign: {
    if (!PtrAuthBundleOrNone)
      return nullptr;
    auto PtrAuthBundle = *PtrAuthBundleOrNone;
    if (II->getOperand(1) != PtrAuthBundle.Inputs[0] ||
        II->getOperand(2) != PtrAuthBundle.Inputs[1])
      return nullptr;
    NewCallee = II->getOperand(0);
    break;
  }

  // call(ptrauth_auth(p)) ->  call p, ["ptrauth"()]
  case Intrinsic::ptrauth_auth: {
    if (PtrAuthBundleOrNone)
      return nullptr;
    Value *NewBundleOps[] = {II->getOperand(1), II->getOperand(2)};
    NewBundles.emplace_back("ptrauth", NewBundleOps);
    NewCallee = II->getOperand(0);
    break;
  }
  }

  if (!NewCallee)
    return nullptr;

  NewCallee = Builder.CreateBitOrPointerCast(NewCallee, Callee->getType());
  CallBase *NewCall = nullptr;
  if (auto *CI = dyn_cast<CallInst>(&Call)) {
    NewCall = CallInst::Create(CI, NewBundles);
  } else {
    auto *IKI = cast<InvokeInst>(&Call);
    NewCall = InvokeInst::Create(IKI, NewBundles);
  }
  NewCall->setCalledOperand(NewCallee);
  return NewCall;
}

static void annotateAnyAllocSite(CallBase &Call, const TargetLibraryInfo *TLI) {
  unsigned NumArgs = Call.getNumArgOperands();
  ConstantInt *Op0C = dyn_cast<ConstantInt>(Call.getOperand(0));
  ConstantInt *Op1C =
      (NumArgs == 1) ? nullptr : dyn_cast<ConstantInt>(Call.getOperand(1));
  // Bail out if the allocation size is zero (or an invalid alignment of zero
  // with aligned_alloc).
  if ((Op0C && Op0C->isNullValue()) || (Op1C && Op1C->isNullValue()))
    return;

  if (isMallocLikeFn(&Call, TLI) && Op0C) {
    if (isOpNewLikeFn(&Call, TLI))
      Call.addAttribute(AttributeList::ReturnIndex,
                        Attribute::getWithDereferenceableBytes(
                            Call.getContext(), Op0C->getZExtValue()));
    else
      Call.addAttribute(AttributeList::ReturnIndex,
                        Attribute::getWithDereferenceableOrNullBytes(
                            Call.getContext(), Op0C->getZExtValue()));
  } else if (isAlignedAllocLikeFn(&Call, TLI) && Op1C) {
    Call.addAttribute(AttributeList::ReturnIndex,
                      Attribute::getWithDereferenceableOrNullBytes(
                          Call.getContext(), Op1C->getZExtValue()));
    // Add alignment attribute if alignment is a power of two constant.
    if (Op0C && Op0C->getValue().ult(llvm::Value::MaximumAlignment)) {
      uint64_t AlignmentVal = Op0C->getZExtValue();
      if (llvm::isPowerOf2_64(AlignmentVal))
        Call.addAttribute(AttributeList::ReturnIndex,
                          Attribute::getWithAlignment(Call.getContext(),
                                                      Align(AlignmentVal)));
    }
  } else if (isReallocLikeFn(&Call, TLI) && Op1C) {
    Call.addAttribute(AttributeList::ReturnIndex,
                      Attribute::getWithDereferenceableOrNullBytes(
                          Call.getContext(), Op1C->getZExtValue()));
  } else if (isCallocLikeFn(&Call, TLI) && Op0C && Op1C) {
    bool Overflow;
    const APInt &N = Op0C->getValue();
    APInt Size = N.umul_ov(Op1C->getValue(), Overflow);
    if (!Overflow)
      Call.addAttribute(AttributeList::ReturnIndex,
                        Attribute::getWithDereferenceableOrNullBytes(
                            Call.getContext(), Size.getZExtValue()));
  } else if (isStrdupLikeFn(&Call, TLI)) {
    uint64_t Len = GetStringLength(Call.getOperand(0));
    if (Len) {
      // strdup
      if (NumArgs == 1)
        Call.addAttribute(AttributeList::ReturnIndex,
                          Attribute::getWithDereferenceableOrNullBytes(
                              Call.getContext(), Len));
      // strndup
      else if (NumArgs == 2 && Op1C)
        Call.addAttribute(
            AttributeList::ReturnIndex,
            Attribute::getWithDereferenceableOrNullBytes(
                Call.getContext(), std::min(Len, Op1C->getZExtValue() + 1)));
    }
  }
}

/// Improvements for call, callbr and invoke instructions.
Instruction *InstCombinerImpl::visitCallBase(CallBase &Call) {
  if (isAllocationFn(&Call, &TLI))
    annotateAnyAllocSite(Call, &TLI);

  bool Changed = false;

  // Mark any parameters that are known to be non-null with the nonnull
  // attribute.  This is helpful for inlining calls to functions with null
  // checks on their arguments.
  SmallVector<unsigned, 4> ArgNos;
  unsigned ArgNo = 0;

  for (Value *V : Call.args()) {
    if (V->getType()->isPointerTy() &&
        !Call.paramHasAttr(ArgNo, Attribute::NonNull) &&
        isKnownNonZero(V, DL, 0, &AC, &Call, &DT))
      ArgNos.push_back(ArgNo);
    ArgNo++;
  }

  assert(ArgNo == Call.arg_size() && "sanity check");

  if (!ArgNos.empty()) {
    AttributeList AS = Call.getAttributes();
    LLVMContext &Ctx = Call.getContext();
    AS = AS.addParamAttribute(Ctx, ArgNos,
                              Attribute::get(Ctx, Attribute::NonNull));
    Call.setAttributes(AS);
    Changed = true;
  }

  // If the callee is a pointer to a function, attempt to move any casts to the
  // arguments of the call/callbr/invoke.
  Value *Callee = Call.getCalledOperand();
  if (!isa<Function>(Callee) && transformConstExprCastCall(Call))
    return nullptr;

  if (Function *CalleeF = dyn_cast<Function>(Callee)) {
    // Remove the convergent attr on calls when the callee is not convergent.
    if (Call.isConvergent() && !CalleeF->isConvergent() &&
        !CalleeF->isIntrinsic()) {
      LLVM_DEBUG(dbgs() << "Removing convergent attr from instr " << Call
                        << "\n");
      Call.setNotConvergent();
      return &Call;
    }

    // If the call and callee calling conventions don't match, this call must
    // be unreachable, as the call is undefined.
    if (CalleeF->getCallingConv() != Call.getCallingConv() &&
        // Only do this for calls to a function with a body.  A prototype may
        // not actually end up matching the implementation's calling conv for a
        // variety of reasons (e.g. it may be written in assembly).
        !CalleeF->isDeclaration()) {
      Instruction *OldCall = &Call;
      CreateNonTerminatorUnreachable(OldCall);
      // If OldCall does not return void then replaceInstUsesWith undef.
      // This allows ValueHandlers and custom metadata to adjust itself.
      if (!OldCall->getType()->isVoidTy())
        replaceInstUsesWith(*OldCall, UndefValue::get(OldCall->getType()));
      if (isa<CallInst>(OldCall))
        return eraseInstFromFunction(*OldCall);

      // We cannot remove an invoke or a callbr, because it would change thexi
      // CFG, just change the callee to a null pointer.
      cast<CallBase>(OldCall)->setCalledFunction(
          CalleeF->getFunctionType(),
          Constant::getNullValue(CalleeF->getType()));
      return nullptr;
    }
  }

  if ((isa<ConstantPointerNull>(Callee) &&
       !NullPointerIsDefined(Call.getFunction())) ||
      isa<UndefValue>(Callee)) {
    // If Call does not return void then replaceInstUsesWith undef.
    // This allows ValueHandlers and custom metadata to adjust itself.
    if (!Call.getType()->isVoidTy())
      replaceInstUsesWith(Call, UndefValue::get(Call.getType()));

    if (Call.isTerminator()) {
      // Can't remove an invoke or callbr because we cannot change the CFG.
      return nullptr;
    }

    // This instruction is not reachable, just remove it.
    CreateNonTerminatorUnreachable(&Call);
    return eraseInstFromFunction(Call);
  }

  if (IntrinsicInst *II = findInitTrampoline(Callee))
    return transformCallThroughTrampoline(Call, *II);

  // Combine calls involving pointer authentication
  if (Instruction *NewCall = tryCombinePtrAuthCall(Call))
    return NewCall;


  PointerType *PTy = cast<PointerType>(Callee->getType());
  FunctionType *FTy = cast<FunctionType>(PTy->getElementType());
  if (FTy->isVarArg()) {
    int ix = FTy->getNumParams();
    // See if we can optimize any arguments passed through the varargs area of
    // the call.
    for (auto I = Call.arg_begin() + FTy->getNumParams(), E = Call.arg_end();
         I != E; ++I, ++ix) {
      CastInst *CI = dyn_cast<CastInst>(*I);
      if (CI && isSafeToEliminateVarargsCast(Call, DL, CI, ix)) {
        replaceUse(*I, CI->getOperand(0));

        // Update the byval type to match the argument type.
        if (Call.isByValArgument(ix)) {
          Call.removeParamAttr(ix, Attribute::ByVal);
          Call.addParamAttr(
              ix, Attribute::getWithByValType(
                      Call.getContext(),
                      CI->getOperand(0)->getType()->getPointerElementType()));
        }
        Changed = true;
      }
    }
  }

  if (isa<InlineAsm>(Callee) && !Call.doesNotThrow()) {
    // Inline asm calls cannot throw - mark them 'nounwind'.
    Call.setDoesNotThrow();
    Changed = true;
  }

  // Try to optimize the call if possible, we require DataLayout for most of
  // this.  None of these calls are seen as possibly dead so go ahead and
  // delete the instruction now.
  if (CallInst *CI = dyn_cast<CallInst>(&Call)) {
    Instruction *I = tryOptimizeCall(CI);
    // If we changed something return the result, etc. Otherwise let
    // the fallthrough check.
    if (I) return eraseInstFromFunction(*I);
  }

  if (!Call.use_empty() && !Call.isMustTailCall())
    if (Value *ReturnedArg = Call.getReturnedArgOperand()) {
      Type *CallTy = Call.getType();
      Type *RetArgTy = ReturnedArg->getType();
      if (RetArgTy->canLosslesslyBitCastTo(CallTy))
        return replaceInstUsesWith(
            Call, Builder.CreateBitOrPointerCast(ReturnedArg, CallTy));
    }

  if (isAllocLikeFn(&Call, &TLI))
    return visitAllocSite(Call);

  return Changed ? &Call : nullptr;
}

/// If the callee is a constexpr cast of a function, attempt to move the cast to
/// the arguments of the call/callbr/invoke.
bool InstCombinerImpl::transformConstExprCastCall(CallBase &Call) {
  auto *Callee =
      dyn_cast<Function>(Call.getCalledOperand()->stripPointerCasts());
  if (!Callee)
    return false;

  // If this is a call to a thunk function, don't remove the cast. Thunks are
  // used to transparently forward all incoming parameters and outgoing return
  // values, so it's important to leave the cast in place.
  if (Callee->hasFnAttribute("thunk"))
    return false;

  // If this is a musttail call, the callee's prototype must match the caller's
  // prototype with the exception of pointee types. The code below doesn't
  // implement that, so we can't do this transform.
  // TODO: Do the transform if it only requires adding pointer casts.
  if (Call.isMustTailCall())
    return false;

  Instruction *Caller = &Call;
  const AttributeList &CallerPAL = Call.getAttributes();

  // Okay, this is a cast from a function to a different type.  Unless doing so
  // would cause a type conversion of one of our arguments, change this call to
  // be a direct call with arguments casted to the appropriate types.
  FunctionType *FT = Callee->getFunctionType();
  Type *OldRetTy = Caller->getType();
  Type *NewRetTy = FT->getReturnType();

  // Check to see if we are changing the return type...
  if (OldRetTy != NewRetTy) {

    if (NewRetTy->isStructTy())
      return false; // TODO: Handle multiple return values.

    if (!CastInst::isBitOrNoopPointerCastable(NewRetTy, OldRetTy, DL)) {
      if (Callee->isDeclaration())
        return false;   // Cannot transform this return value.

      if (!Caller->use_empty() &&
          // void -> non-void is handled specially
          !NewRetTy->isVoidTy())
        return false;   // Cannot transform this return value.
    }

    if (!CallerPAL.isEmpty() && !Caller->use_empty()) {
      AttrBuilder RAttrs(CallerPAL, AttributeList::ReturnIndex);
      if (RAttrs.overlaps(AttributeFuncs::typeIncompatible(NewRetTy)))
        return false;   // Attribute not compatible with transformed value.
    }

    // If the callbase is an invoke/callbr instruction, and the return value is
    // used by a PHI node in a successor, we cannot change the return type of
    // the call because there is no place to put the cast instruction (without
    // breaking the critical edge).  Bail out in this case.
    if (!Caller->use_empty()) {
      if (InvokeInst *II = dyn_cast<InvokeInst>(Caller))
        for (User *U : II->users())
          if (PHINode *PN = dyn_cast<PHINode>(U))
            if (PN->getParent() == II->getNormalDest() ||
                PN->getParent() == II->getUnwindDest())
              return false;
      // FIXME: Be conservative for callbr to avoid a quadratic search.
      if (isa<CallBrInst>(Caller))
        return false;
    }
  }

  unsigned NumActualArgs = Call.arg_size();
  unsigned NumCommonArgs = std::min(FT->getNumParams(), NumActualArgs);

  // Prevent us turning:
  // declare void @takes_i32_inalloca(i32* inalloca)
  //  call void bitcast (void (i32*)* @takes_i32_inalloca to void (i32)*)(i32 0)
  //
  // into:
  //  call void @takes_i32_inalloca(i32* null)
  //
  //  Similarly, avoid folding away bitcasts of byval calls.
  if (Callee->getAttributes().hasAttrSomewhere(Attribute::InAlloca) ||
      Callee->getAttributes().hasAttrSomewhere(Attribute::Preallocated) ||
      Callee->getAttributes().hasAttrSomewhere(Attribute::ByVal))
    return false;

  auto AI = Call.arg_begin();
  for (unsigned i = 0, e = NumCommonArgs; i != e; ++i, ++AI) {
    Type *ParamTy = FT->getParamType(i);
    Type *ActTy = (*AI)->getType();

    if (!CastInst::isBitOrNoopPointerCastable(ActTy, ParamTy, DL))
      return false;   // Cannot transform this parameter value.

    if (AttrBuilder(CallerPAL.getParamAttributes(i))
            .overlaps(AttributeFuncs::typeIncompatible(ParamTy)))
      return false;   // Attribute not compatible with transformed value.

    if (Call.isInAllocaArgument(i))
      return false;   // Cannot transform to and from inalloca.

    if (CallerPAL.hasParamAttribute(i, Attribute::SwiftError))
      return false;

    // If the parameter is passed as a byval argument, then we have to have a
    // sized type and the sized type has to have the same size as the old type.
    if (ParamTy != ActTy && CallerPAL.hasParamAttribute(i, Attribute::ByVal)) {
      PointerType *ParamPTy = dyn_cast<PointerType>(ParamTy);
      if (!ParamPTy || !ParamPTy->getElementType()->isSized())
        return false;

      Type *CurElTy = Call.getParamByValType(i);
      if (DL.getTypeAllocSize(CurElTy) !=
          DL.getTypeAllocSize(ParamPTy->getElementType()))
        return false;
    }
  }

  if (Callee->isDeclaration()) {
    // Do not delete arguments unless we have a function body.
    if (FT->getNumParams() < NumActualArgs && !FT->isVarArg())
      return false;

    // If the callee is just a declaration, don't change the varargsness of the
    // call.  We don't want to introduce a varargs call where one doesn't
    // already exist.
    PointerType *APTy = cast<PointerType>(Call.getCalledOperand()->getType());
    if (FT->isVarArg()!=cast<FunctionType>(APTy->getElementType())->isVarArg())
      return false;

    // If both the callee and the cast type are varargs, we still have to make
    // sure the number of fixed parameters are the same or we have the same
    // ABI issues as if we introduce a varargs call.
    if (FT->isVarArg() &&
        cast<FunctionType>(APTy->getElementType())->isVarArg() &&
        FT->getNumParams() !=
        cast<FunctionType>(APTy->getElementType())->getNumParams())
      return false;
  }

  if (FT->getNumParams() < NumActualArgs && FT->isVarArg() &&
      !CallerPAL.isEmpty()) {
    // In this case we have more arguments than the new function type, but we
    // won't be dropping them.  Check that these extra arguments have attributes
    // that are compatible with being a vararg call argument.
    unsigned SRetIdx;
    if (CallerPAL.hasAttrSomewhere(Attribute::StructRet, &SRetIdx) &&
        SRetIdx > FT->getNumParams())
      return false;
  }

  // Okay, we decided that this is a safe thing to do: go ahead and start
  // inserting cast instructions as necessary.
  SmallVector<Value *, 8> Args;
  SmallVector<AttributeSet, 8> ArgAttrs;
  Args.reserve(NumActualArgs);
  ArgAttrs.reserve(NumActualArgs);

  // Get any return attributes.
  AttrBuilder RAttrs(CallerPAL, AttributeList::ReturnIndex);

  // If the return value is not being used, the type may not be compatible
  // with the existing attributes.  Wipe out any problematic attributes.
  RAttrs.remove(AttributeFuncs::typeIncompatible(NewRetTy));

  LLVMContext &Ctx = Call.getContext();
  AI = Call.arg_begin();
  for (unsigned i = 0; i != NumCommonArgs; ++i, ++AI) {
    Type *ParamTy = FT->getParamType(i);

    Value *NewArg = *AI;
    if ((*AI)->getType() != ParamTy)
      NewArg = Builder.CreateBitOrPointerCast(*AI, ParamTy);
    Args.push_back(NewArg);

    // Add any parameter attributes.
    if (CallerPAL.hasParamAttribute(i, Attribute::ByVal)) {
      AttrBuilder AB(CallerPAL.getParamAttributes(i));
      AB.addByValAttr(NewArg->getType()->getPointerElementType());
      ArgAttrs.push_back(AttributeSet::get(Ctx, AB));
    } else
      ArgAttrs.push_back(CallerPAL.getParamAttributes(i));
  }

  // If the function takes more arguments than the call was taking, add them
  // now.
  for (unsigned i = NumCommonArgs; i != FT->getNumParams(); ++i) {
    Args.push_back(Constant::getNullValue(FT->getParamType(i)));
    ArgAttrs.push_back(AttributeSet());
  }

  // If we are removing arguments to the function, emit an obnoxious warning.
  if (FT->getNumParams() < NumActualArgs) {
    // TODO: if (!FT->isVarArg()) this call may be unreachable. PR14722
    if (FT->isVarArg()) {
      // Add all of the arguments in their promoted form to the arg list.
      for (unsigned i = FT->getNumParams(); i != NumActualArgs; ++i, ++AI) {
        Type *PTy = getPromotedType((*AI)->getType());
        Value *NewArg = *AI;
        if (PTy != (*AI)->getType()) {
          // Must promote to pass through va_arg area!
          Instruction::CastOps opcode =
            CastInst::getCastOpcode(*AI, false, PTy, false);
          NewArg = Builder.CreateCast(opcode, *AI, PTy);
        }
        Args.push_back(NewArg);

        // Add any parameter attributes.
        ArgAttrs.push_back(CallerPAL.getParamAttributes(i));
      }
    }
  }

  AttributeSet FnAttrs = CallerPAL.getFnAttributes();

  if (NewRetTy->isVoidTy())
    Caller->setName("");   // Void type should not have a name.

  assert((ArgAttrs.size() == FT->getNumParams() || FT->isVarArg()) &&
         "missing argument attributes");
  AttributeList NewCallerPAL = AttributeList::get(
      Ctx, FnAttrs, AttributeSet::get(Ctx, RAttrs), ArgAttrs);

  SmallVector<OperandBundleDef, 1> OpBundles;
  Call.getOperandBundlesAsDefs(OpBundles);

  CallBase *NewCall;
  if (InvokeInst *II = dyn_cast<InvokeInst>(Caller)) {
    NewCall = Builder.CreateInvoke(Callee, II->getNormalDest(),
                                   II->getUnwindDest(), Args, OpBundles);
  } else if (CallBrInst *CBI = dyn_cast<CallBrInst>(Caller)) {
    NewCall = Builder.CreateCallBr(Callee, CBI->getDefaultDest(),
                                   CBI->getIndirectDests(), Args, OpBundles);
  } else {
    NewCall = Builder.CreateCall(Callee, Args, OpBundles);
    cast<CallInst>(NewCall)->setTailCallKind(
        cast<CallInst>(Caller)->getTailCallKind());
  }
  NewCall->takeName(Caller);
  NewCall->setCallingConv(Call.getCallingConv());
  NewCall->setAttributes(NewCallerPAL);

  // Preserve prof metadata if any.
  NewCall->copyMetadata(*Caller, {LLVMContext::MD_prof});

  // Insert a cast of the return type as necessary.
  Instruction *NC = NewCall;
  Value *NV = NC;
  if (OldRetTy != NV->getType() && !Caller->use_empty()) {
    if (!NV->getType()->isVoidTy()) {
      NV = NC = CastInst::CreateBitOrPointerCast(NC, OldRetTy);
      NC->setDebugLoc(Caller->getDebugLoc());

      // If this is an invoke/callbr instruction, we should insert it after the
      // first non-phi instruction in the normal successor block.
      if (InvokeInst *II = dyn_cast<InvokeInst>(Caller)) {
        BasicBlock::iterator I = II->getNormalDest()->getFirstInsertionPt();
        InsertNewInstBefore(NC, *I);
      } else if (CallBrInst *CBI = dyn_cast<CallBrInst>(Caller)) {
        BasicBlock::iterator I = CBI->getDefaultDest()->getFirstInsertionPt();
        InsertNewInstBefore(NC, *I);
      } else {
        // Otherwise, it's a call, just insert cast right after the call.
        InsertNewInstBefore(NC, *Caller);
      }
      Worklist.pushUsersToWorkList(*Caller);
    } else {
      NV = UndefValue::get(Caller->getType());
    }
  }

  if (!Caller->use_empty())
    replaceInstUsesWith(*Caller, NV);
  else if (Caller->hasValueHandle()) {
    if (OldRetTy == NV->getType())
      ValueHandleBase::ValueIsRAUWd(Caller, NV);
    else
      // We cannot call ValueIsRAUWd with a different type, and the
      // actual tracked value will disappear.
      ValueHandleBase::ValueIsDeleted(Caller);
  }

  eraseInstFromFunction(*Caller);
  return true;
}

/// Turn a call to a function created by init_trampoline / adjust_trampoline
/// intrinsic pair into a direct call to the underlying function.
Instruction *
InstCombinerImpl::transformCallThroughTrampoline(CallBase &Call,
                                                 IntrinsicInst &Tramp) {
  Value *Callee = Call.getCalledOperand();
  Type *CalleeTy = Callee->getType();
  FunctionType *FTy = Call.getFunctionType();
  AttributeList Attrs = Call.getAttributes();

  // If the call already has the 'nest' attribute somewhere then give up -
  // otherwise 'nest' would occur twice after splicing in the chain.
  if (Attrs.hasAttrSomewhere(Attribute::Nest))
    return nullptr;

  Function *NestF = cast<Function>(Tramp.getArgOperand(1)->stripPointerCasts());
  FunctionType *NestFTy = NestF->getFunctionType();

  AttributeList NestAttrs = NestF->getAttributes();
  if (!NestAttrs.isEmpty()) {
    unsigned NestArgNo = 0;
    Type *NestTy = nullptr;
    AttributeSet NestAttr;

    // Look for a parameter marked with the 'nest' attribute.
    for (FunctionType::param_iterator I = NestFTy->param_begin(),
                                      E = NestFTy->param_end();
         I != E; ++NestArgNo, ++I) {
      AttributeSet AS = NestAttrs.getParamAttributes(NestArgNo);
      if (AS.hasAttribute(Attribute::Nest)) {
        // Record the parameter type and any other attributes.
        NestTy = *I;
        NestAttr = AS;
        break;
      }
    }

    if (NestTy) {
      std::vector<Value*> NewArgs;
      std::vector<AttributeSet> NewArgAttrs;
      NewArgs.reserve(Call.arg_size() + 1);
      NewArgAttrs.reserve(Call.arg_size());

      // Insert the nest argument into the call argument list, which may
      // mean appending it.  Likewise for attributes.

      {
        unsigned ArgNo = 0;
        auto I = Call.arg_begin(), E = Call.arg_end();
        do {
          if (ArgNo == NestArgNo) {
            // Add the chain argument and attributes.
            Value *NestVal = Tramp.getArgOperand(2);
            if (NestVal->getType() != NestTy)
              NestVal = Builder.CreateBitCast(NestVal, NestTy, "nest");
            NewArgs.push_back(NestVal);
            NewArgAttrs.push_back(NestAttr);
          }

          if (I == E)
            break;

          // Add the original argument and attributes.
          NewArgs.push_back(*I);
          NewArgAttrs.push_back(Attrs.getParamAttributes(ArgNo));

          ++ArgNo;
          ++I;
        } while (true);
      }

      // The trampoline may have been bitcast to a bogus type (FTy).
      // Handle this by synthesizing a new function type, equal to FTy
      // with the chain parameter inserted.

      std::vector<Type*> NewTypes;
      NewTypes.reserve(FTy->getNumParams()+1);

      // Insert the chain's type into the list of parameter types, which may
      // mean appending it.
      {
        unsigned ArgNo = 0;
        FunctionType::param_iterator I = FTy->param_begin(),
          E = FTy->param_end();

        do {
          if (ArgNo == NestArgNo)
            // Add the chain's type.
            NewTypes.push_back(NestTy);

          if (I == E)
            break;

          // Add the original type.
          NewTypes.push_back(*I);

          ++ArgNo;
          ++I;
        } while (true);
      }

      // Replace the trampoline call with a direct call.  Let the generic
      // code sort out any function type mismatches.
      FunctionType *NewFTy = FunctionType::get(FTy->getReturnType(), NewTypes,
                                                FTy->isVarArg());
      Constant *NewCallee =
        NestF->getType() == PointerType::getUnqual(NewFTy) ?
        NestF : ConstantExpr::getBitCast(NestF,
                                         PointerType::getUnqual(NewFTy));
      AttributeList NewPAL =
          AttributeList::get(FTy->getContext(), Attrs.getFnAttributes(),
                             Attrs.getRetAttributes(), NewArgAttrs);

      SmallVector<OperandBundleDef, 1> OpBundles;
      Call.getOperandBundlesAsDefs(OpBundles);

      Instruction *NewCaller;
      if (InvokeInst *II = dyn_cast<InvokeInst>(&Call)) {
        NewCaller = InvokeInst::Create(NewFTy, NewCallee,
                                       II->getNormalDest(), II->getUnwindDest(),
                                       NewArgs, OpBundles);
        cast<InvokeInst>(NewCaller)->setCallingConv(II->getCallingConv());
        cast<InvokeInst>(NewCaller)->setAttributes(NewPAL);
      } else if (CallBrInst *CBI = dyn_cast<CallBrInst>(&Call)) {
        NewCaller =
            CallBrInst::Create(NewFTy, NewCallee, CBI->getDefaultDest(),
                               CBI->getIndirectDests(), NewArgs, OpBundles);
        cast<CallBrInst>(NewCaller)->setCallingConv(CBI->getCallingConv());
        cast<CallBrInst>(NewCaller)->setAttributes(NewPAL);
      } else {
        NewCaller = CallInst::Create(NewFTy, NewCallee, NewArgs, OpBundles);
        cast<CallInst>(NewCaller)->setTailCallKind(
            cast<CallInst>(Call).getTailCallKind());
        cast<CallInst>(NewCaller)->setCallingConv(
            cast<CallInst>(Call).getCallingConv());
        cast<CallInst>(NewCaller)->setAttributes(NewPAL);
      }
      NewCaller->setDebugLoc(Call.getDebugLoc());

      return NewCaller;
    }
  }

  // Replace the trampoline call with a direct call.  Since there is no 'nest'
  // parameter, there is no need to adjust the argument list.  Let the generic
  // code sort out any function type mismatches.
  Constant *NewCallee = ConstantExpr::getBitCast(NestF, CalleeTy);
  Call.setCalledFunction(FTy, NewCallee);
  return &Call;
}
