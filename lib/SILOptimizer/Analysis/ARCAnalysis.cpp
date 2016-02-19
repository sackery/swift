//===--- ARCAnalysis.cpp - SIL ARC Analysis -------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-arc-analysis"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Projection.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "swift/SILOptimizer/Utils/Local.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                             Decrement Analysis
//===----------------------------------------------------------------------===//

bool swift::mayDecrementRefCount(SILInstruction *User,
                                 SILValue Ptr, AliasAnalysis *AA) {
  // First do a basic check, mainly based on the type of instruction.
  // Reading the RC is as "bad" as releasing.
  if (!User->mayReleaseOrReadRefCount())
    return false;

  // Ok, this instruction may have ref counts. If it is an apply, attempt to
  // prove that the callee is unable to affect Ptr.
  if (auto *AI = dyn_cast<ApplyInst>(User))
    return AA->canApplyDecrementRefCount(AI, Ptr);
  if (auto *TAI = dyn_cast<TryApplyInst>(User))
    return AA->canApplyDecrementRefCount(TAI, Ptr);
  if (auto *BI = dyn_cast<BuiltinInst>(User))
    return AA->canBuiltinDecrementRefCount(BI, Ptr);

  // We cannot conservatively prove that this instruction cannot decrement the
  // ref count of Ptr. So assume that it does.
  return true;
}

bool swift::mayCheckRefCount(SILInstruction *User) {
  return isa<IsUniqueInst>(User) || isa<IsUniqueOrPinnedInst>(User);
}

//===----------------------------------------------------------------------===//
//                                Use Analysis
//===----------------------------------------------------------------------===//

/// Returns true if a builtin apply cannot use reference counted values.
///
/// The main case that this handles here are builtins that via read none imply
/// that they cannot read globals and at the same time do not take any
/// non-trivial types via the arguments. The reason why we care about taking
/// non-trivial types as arguments is that we want to be careful in the face of
/// intrinsics that may be equivalent to bitcast and inttoptr operations.
static bool canApplyOfBuiltinUseNonTrivialValues(BuiltinInst *BInst) {
  SILModule &Mod = BInst->getModule();

  auto &II = BInst->getIntrinsicInfo();
  if (II.ID != llvm::Intrinsic::not_intrinsic) {
    if (II.hasAttribute(llvm::Attribute::ReadNone)) {
      for (auto &Op : BInst->getAllOperands()) {
        if (!Op.get()->getType().isTrivial(Mod)) {
          return false;
        }
      }
    }

    return true;
  }

  auto &BI = BInst->getBuiltinInfo();
  if (BI.isReadNone()) {
    for (auto &Op : BInst->getAllOperands()) {
      if (!Op.get()->getType().isTrivial(Mod)) {
        return false;
      }
    }
  }

  return true;
}

/// Returns true if Inst is a function that we know never uses ref count values.
bool swift::canNeverUseValues(SILInstruction *Inst) {
  switch (Inst->getKind()) {
  // These instructions do not use other values.
  case ValueKind::FunctionRefInst:
  case ValueKind::IntegerLiteralInst:
  case ValueKind::FloatLiteralInst:
  case ValueKind::StringLiteralInst:
  case ValueKind::AllocStackInst:
  case ValueKind::AllocRefInst:
  case ValueKind::AllocRefDynamicInst:
  case ValueKind::AllocBoxInst:
  case ValueKind::MetatypeInst:
  case ValueKind::WitnessMethodInst:
    return true;

  // DeallocStackInst do not use reference counted values.
  case ValueKind::DeallocStackInst:
    return true;

  // Debug values do not use referenced counted values in a manner we care
  // about.
  case ValueKind::DebugValueInst:
  case ValueKind::DebugValueAddrInst:
    return true;

  // Casts do not use pointers in a manner that we care about since we strip
  // them during our analysis. The reason for this is if the cast is not dead
  // then there must be some other use after the cast that we will protect if a
  // release is not in between the cast and the use.
  case ValueKind::UpcastInst:
  case ValueKind::AddressToPointerInst:
  case ValueKind::PointerToAddressInst:
  case ValueKind::UncheckedRefCastInst:
  case ValueKind::UncheckedRefCastAddrInst:
  case ValueKind::UncheckedAddrCastInst:
  case ValueKind::RefToRawPointerInst:
  case ValueKind::RawPointerToRefInst:
  case ValueKind::UnconditionalCheckedCastInst:
  case ValueKind::UncheckedBitwiseCastInst:
    return true;

  // If we have a trivial bit cast between trivial types, it is not something
  // that can use ref count ops in a way we care about. We do need to be careful
  // with uses with ref count inputs. In such a case, we assume conservatively
  // that the bit cast could use it.
  //
  // The reason why this is different from the ref bitcast is b/c the use of a
  // ref bit cast is still a ref typed value implying that our ARC dataflow will
  // properly handle its users. A conversion of a reference count value to a
  // trivial value though could be used as a trivial value in ways that ARC
  // dataflow will not understand implying we need to treat it as a use to be
  // safe.
  case ValueKind::UncheckedTrivialBitCastInst: {
    SILValue Op = cast<UncheckedTrivialBitCastInst>(Inst)->getOperand();
    return Op->getType().isTrivial(Inst->getModule());
  }

  // Typed GEPs do not use pointers. The user of the typed GEP may but we will
  // catch that via the dataflow.
  case ValueKind::StructExtractInst:
  case ValueKind::TupleExtractInst:
  case ValueKind::StructElementAddrInst:
  case ValueKind::TupleElementAddrInst:
  case ValueKind::UncheckedTakeEnumDataAddrInst:
  case ValueKind::RefElementAddrInst:
  case ValueKind::UncheckedEnumDataInst:
  case ValueKind::IndexAddrInst:
  case ValueKind::IndexRawPointerInst:
      return true;

  // Aggregate formation by themselves do not create new uses since it is their
  // users that would create the appropriate uses.
  case ValueKind::EnumInst:
  case ValueKind::StructInst:
  case ValueKind::TupleInst:
    return true;

  // Only uses non reference counted values.
  case ValueKind::CondFailInst:
    return true;

  case ValueKind::BuiltinInst: {
    auto *BI = cast<BuiltinInst>(Inst);

    // Certain builtin function refs we know can never use non-trivial values.
    return canApplyOfBuiltinUseNonTrivialValues(BI);
  }
  // We do not care about branch inst, since if the branch inst's argument is
  // dead, LLVM will clean it up.
  case ValueKind::BranchInst:
  case ValueKind::CondBranchInst:
    return true;
  default:
    return false;
  }
}

static bool doOperandsAlias(ArrayRef<Operand> Ops, SILValue Ptr,
                            AliasAnalysis *AA) {
  // If any are not no alias, we have a use.
  return std::any_of(Ops.begin(), Ops.end(),
                     [&AA, &Ptr](const Operand &Op) -> bool {
                       return !AA->isNoAlias(Ptr, Op.get());
                     });
}

static bool canTerminatorUseValue(TermInst *TI, SILValue Ptr,
                                  AliasAnalysis *AA) {
  if (auto *BI = dyn_cast<BranchInst>(TI)) {
    return doOperandsAlias(BI->getAllOperands(), Ptr, AA);
  }

  if (auto *CBI = dyn_cast<CondBranchInst>(TI)) {
    bool First = doOperandsAlias(CBI->getTrueOperands(), Ptr, AA);
    bool Second = doOperandsAlias(CBI->getFalseOperands(), Ptr, AA);
    return First || Second;
  }

  if (auto *SWEI = dyn_cast<SwitchEnumInst>(TI)) {
    return doOperandsAlias(SWEI->getAllOperands(), Ptr, AA);
  }

  if (auto *SWVI = dyn_cast<SwitchValueInst>(TI)) {
    return doOperandsAlias(SWVI->getAllOperands(), Ptr, AA);
  }

  auto *CCBI = dyn_cast<CheckedCastBranchInst>(TI);
  // If we don't have this last case, be conservative and assume that we can use
  // the value.
  if (!CCBI)
    return true;

  // Otherwise, look at the operands.
  return doOperandsAlias(CCBI->getAllOperands(), Ptr, AA);
}

bool swift::mayUseValue(SILInstruction *User, SILValue Ptr,
                        AliasAnalysis *AA) {
  // If Inst is an instruction that we know can never use values with reference
  // semantics, return true.
  if (canNeverUseValues(User))
    return false;

  // If the user is a load or a store and we can prove that it does not access
  // the object then return true.
  // Notice that we need to check all of the values of the object.
  if (isa<StoreInst>(User)) {
    if (AA->mayWriteToMemory(User, Ptr))
      return true;
    return false;
  }

  if (isa<LoadInst>(User) ) {
    if (AA->mayReadFromMemory(User, Ptr))
      return true;
    return false;
  }

  // If we have a terminator instruction, see if it can use ptr. This currently
  // means that we first show that TI cannot indirectly use Ptr and then use
  // alias analysis on the arguments.
  if (auto *TI = dyn_cast<TermInst>(User))
    return canTerminatorUseValue(TI, Ptr, AA);

  // TODO: If we add in alias analysis support here for apply inst, we will need
  // to check that the pointer does not escape.

  // Otherwise, assume that Inst can use Target.
  return true;
}

//===----------------------------------------------------------------------===//
//                             Must Use Analysis
//===----------------------------------------------------------------------===//

/// Returns true if User must use Ptr.
///
/// In terms of ARC this means that if we do not remove User, all releases post
/// dominated by User are known safe.
bool swift::mustUseValue(SILInstruction *User, SILValue Ptr,
                         AliasAnalysis *AA) {
  // Right now just pattern match applies.
  auto *AI = dyn_cast<ApplyInst>(User);
  if (!AI)
    return false;

  // If any of AI's arguments must alias Ptr, return true.
  for (SILValue Arg : AI->getArguments())
    if (AA->isMustAlias(Arg, Ptr))
      return true;
  return false;
}

/// Returns true if User must use Ptr in a guaranteed way.
///
/// This means that assuming that everything is conservative, we can ignore the
/// ref count effects of User on Ptr since we will only remove things over
/// guaranteed parameters if we are known safe in both directions.
bool swift::mustGuaranteedUseValue(SILInstruction *User, SILValue Ptr,
                                   AliasAnalysis *AA) {
  // Right now just pattern match applies.
  auto *AI = dyn_cast<ApplyInst>(User);
  if (!AI)
    return false;

  // For now just look for guaranteed self.
  //
  // TODO: Expand this to handle *any* guaranteed parameter.
  if (!AI->hasGuaranteedSelfArgument())
    return false;

  // Return true if Ptr alias's self.
  return AA->isMustAlias(AI->getSelfArgument(), Ptr);
}

//===----------------------------------------------------------------------===//
// Utility Methods for determining use, decrement of values in a contiguous
// instruction range in one BB.
//===----------------------------------------------------------------------===//

/// If \p Op has arc uses in the instruction range [Start, End), return the
/// first such instruction. Otherwise return None. We assume that
/// Start and End are both in the same basic block.
Optional<SILBasicBlock::iterator>
swift::
valueHasARCUsesInInstructionRange(SILValue Op,
                                  SILBasicBlock::iterator Start,
                                  SILBasicBlock::iterator End,
                                  AliasAnalysis *AA) {
  assert(Start->getParent() == End->getParent() &&
         "Start and End should be in the same basic block");

  // If Start == End, then we have an empty range, return false.
  if (Start == End)
    return None;

  // Otherwise, until Start != End.
  while (Start != End) {
    // Check if Start can use Op in an ARC relevant way. If so, return true.
    if (mayUseValue(&*Start, Op, AA))
      return Start;

    // Otherwise, increment our iterator.
    ++Start;
  }

  // If all such instructions cannot use Op, return false.
  return None;
}

/// If \p Op has arc uses in the instruction range (Start, End], return the
/// first such instruction. Otherwise return None. We assume that Start and End
/// are both in the same basic block.
Optional<SILBasicBlock::iterator>
swift::valueHasARCUsesInReverseInstructionRange(SILValue Op,
                                                SILBasicBlock::iterator Start,
                                                SILBasicBlock::iterator End,
                                                AliasAnalysis *AA) {
  assert(Start->getParent() == End->getParent() &&
         "Start and End should be in the same basic block");
  assert(End != End->getParent()->end() &&
         "End should be mapped to an actual instruction");

  // If Start == End, then we have an empty range, return false.
  if (Start == End)
    return None;

  // Otherwise, until End == Start.
  while (Start != End) {
    // Check if Start can use Op in an ARC relevant way. If so, return true.
    if (mayUseValue(&*End, Op, AA))
      return End;

    // Otherwise, decrement our iterator.
    --End;
  }

  // If all such instructions cannot use Op, return false.
  return None;
}

/// If \p Op has instructions in the instruction range (Start, End] which may
/// decrement it, return the first such instruction. Returns None
/// if no such instruction exists. We assume that Start and End are both in the
/// same basic block.
Optional<SILBasicBlock::iterator>
swift::
valueHasARCDecrementOrCheckInInstructionRange(SILValue Op,
                                              SILBasicBlock::iterator Start,
                                              SILBasicBlock::iterator End,
                                              AliasAnalysis *AA) {
  assert(Start->getParent() == End->getParent() &&
         "Start and End should be in the same basic block");

  // If Start == End, then we have an empty range, return nothing.
  if (Start == End)
    return None;

  // Otherwise, until Start != End.
  while (Start != End) {
    // Check if Start can decrement or check Op's ref count. If so, return
    // Start. Ref count checks do not have side effects, but are barriers for
    // retains.
    if (mayDecrementRefCount(&*Start, Op, AA) || mayCheckRefCount(&*Start))
      return Start;
    // Otherwise, increment our iterator.
    ++Start;
  }

  // If all such instructions cannot decrement Op, return nothing.
  return None;
}

bool
swift::
mayGuaranteedUseValue(SILInstruction *User, SILValue Ptr, AliasAnalysis *AA) {
  // Only full apply sites can require a guaranteed lifetime. If we don't have
  // one, bail.
  if (!isa<FullApplySite>(User))
    return false;

  FullApplySite FAS(User);

  // Ok, we have a full apply site. If the apply has no arguments, we don't need
  // to worry about any guaranteed parameters.
  if (!FAS.getNumArguments())
    return false;

  // Ok, we have an apply site with arguments. Look at the function type and
  // iterate through the function parameters. If any of the parameters are
  // guaranteed, attempt to prove that the passed in parameter cannot alias
  // Ptr. If we fail, return true.
  CanSILFunctionType FType = FAS.getSubstCalleeType();
  auto Params = FType->getParameters();
  for (unsigned i : indices(Params)) {    
    if (!Params[i].isGuaranteed())
      continue;
    SILValue Op = FAS.getArgument(i);
    if (!AA->isNoAlias(Op, Ptr))
      return true;
  }

  // Ok, we were able to prove that all arguments to the apply that were
  // guaranteed do not alias Ptr. Return false.
  return false;
}

//===----------------------------------------------------------------------===//
//           Utilities for recognizing trap BBs that are ARC inert
//===----------------------------------------------------------------------===//


//===----------------------------------------------------------------------===//
//                          Owned Argument Utilities
//===----------------------------------------------------------------------===//
ConsumedReturnValueToEpilogueRetainMatcher::
ConsumedReturnValueToEpilogueRetainMatcher(RCIdentityFunctionInfo *RCFI,
                                           AliasAnalysis *AA,
                                           SILFunction *F,
                                           ExitKind Kind)
    : F(F), RCFI(RCFI), AA(AA), Kind(Kind) {
  recompute();
}

void ConsumedReturnValueToEpilogueRetainMatcher::recompute() {
  EpilogueRetainInsts.clear();

  // Find the return BB of F. If we fail, then bail.
  SILFunction::iterator BB;
  switch (Kind) {
  case ExitKind::Return:
    BB = F->findReturnBB();
    break;
  case ExitKind::Throw:
    BB = F->findThrowBB();
    break;
  }

  if (BB == F->end()) {
    HasBlock = false;
    return;
  }
  HasBlock = true;
  findMatchingRetains(&*BB);
}


void
ConsumedReturnValueToEpilogueRetainMatcher::
findMatchingRetains(SILBasicBlock *BB) {
  // Iterate over the instructions post-order and find retains associated with
  // return value.
  SILValue RV = SILValue();
  for (auto II = BB->rbegin(), IE = BB->rend(); II != IE; ++II) {
    if (ReturnInst *RI = dyn_cast<ReturnInst>(&*II)) {
      RV = RI->getOperand();
      break;
    }
  }

  // Somehow, we managed not to find a return value.
  if (!RV)
    return;

  // OK. we've found the return value, now iterate on the CFG to find all the
  // post-dominating retains.
  constexpr unsigned WorkListMaxSize = 8;
  RV = RCFI->getRCIdentityRoot(RV);
  llvm::DenseSet<SILBasicBlock *> RetainFrees;
  llvm::SmallVector<SILBasicBlock *, 4> WorkList;
  llvm::DenseSet<SILBasicBlock *> HandledBBs;
  WorkList.push_back(BB);
  HandledBBs.insert(BB);
  while (!WorkList.empty()) {
    auto *CBB = WorkList.pop_back_val();
    RetainKindValue Kind = findMatchingRetainsInner(CBB, RV);

    // Too many blocks ?.
    if (WorkList.size() > WorkListMaxSize) {
      EpilogueRetainInsts.clear();
      return;
    }

    // There is a MayDecrement instruction.
    if (Kind.first == FindRetainKind::Blocked)
      return;
  
    if (Kind.first == FindRetainKind::None) {
      RetainFrees.insert(CBB);

      // We can not find a retain in a block with no predecessors.
      if (CBB->getPreds().begin() == CBB->getPreds().end()) {
        EpilogueRetainInsts.clear();
        return;
      }

      // Check the predecessors.
      for (auto X : CBB->getPreds()){
        if (HandledBBs.find(X) != HandledBBs.end())
          continue;
        WorkList.push_back(X);
        HandledBBs.insert(X);
      }
    }

    // We've found a retain on this path.
    if (Kind.first == FindRetainKind::Found) 
      EpilogueRetainInsts.push_back(Kind.second);
  }

  // For every block with retain, we need to check the transistive
  // closure of its successors are retain-free.
  for (auto &I : EpilogueRetainInsts) {
    auto *CBB = I->getParent();
    for (auto &Succ : CBB->getSuccessors()) {
      if (RetainFrees.find(Succ) != RetainFrees.end())
        continue;
      EpilogueRetainInsts.clear();
      return;
    }
  }
  for (auto CBB : RetainFrees) {
    for (auto &Succ : CBB->getSuccessors()) {
      if (RetainFrees.find(Succ) != RetainFrees.end())
        continue;
      EpilogueRetainInsts.clear();
      return;
    }
  }

  // At this point, we've either failed to find any epilogue retains or
  // all the post-dominating epilogue retains.
}

ConsumedReturnValueToEpilogueRetainMatcher::RetainKindValue
ConsumedReturnValueToEpilogueRetainMatcher::
findMatchingRetainsInner(SILBasicBlock *BB, SILValue V) {
  for (auto II = BB->rbegin(), IE = BB->rend(); II != IE; ++II) {
    // If we do not have a retain_value or strong_retain...
    if (!isa<RetainValueInst>(*II) && !isa<StrongRetainInst>(*II)) {
      // we can ignore it if it can not decrement the reference count of the
      // return value.
      if (!mayDecrementRefCount(&*II, V, AA))
        continue;

      // Otherwise, we need to stop computing since we do not want to create
      // lifetime gap.
      return std::make_pair(FindRetainKind::Blocked, nullptr);
    }

    // Ok, we have a retain_value or strong_retain. Grab Target and find the
    // RC identity root of its operand.
    SILInstruction *Target = &*II;
    SILValue RetainValue = RCFI->getRCIdentityRoot(Target->getOperand(0));
    SILValue ReturnValue = RCFI->getRCIdentityRoot(V);

    // Is this the epilogue retain we are looking for ?.
    // We break here as we do not know whether this is a part of the epilogue
    // retain for the @own return value.
    if (RetainValue != ReturnValue)
      continue;

    return std::make_pair(FindRetainKind::Found, &*II);
  }

  // Did not find retain in this block.
  return std::make_pair(FindRetainKind::None, nullptr);
} 

ConsumedArgToEpilogueReleaseMatcher::ConsumedArgToEpilogueReleaseMatcher(
    RCIdentityFunctionInfo *RCFI, SILFunction *F, ExitKind Kind)
    : F(F), RCFI(RCFI), Kind(Kind) {
  recompute();
}

void ConsumedArgToEpilogueReleaseMatcher::recompute() {
  ArgInstMap.clear();

  // Find the return BB of F. If we fail, then bail.
  SILFunction::iterator BB;
  switch (Kind) {
  case ExitKind::Return:
    BB = F->findReturnBB();
    break;
  case ExitKind::Throw:
    BB = F->findThrowBB();
    break;
  }

  if (BB == F->end()) {
    HasBlock = false;
    return;
  }
  HasBlock = true;
  findMatchingReleases(&*BB);
}

bool
ConsumedArgToEpilogueReleaseMatcher::
isRedundantRelease(ReleaseList Insts, SILValue Base, SILValue Derived) {
  // We use projection path to analyze the relation.
  auto POp = ProjectionPath::getProjectionPath(Base, Derived);
  // We can not build a projection path from the base to the derived, bail out.
  // and return true so that we can stop the epilogue walking sequence.
  if (!POp.hasValue())
    return true;

  for (auto &R : Insts) {
    SILValue ROp = R->getOperand(0);
    auto PROp = ProjectionPath::getProjectionPath(Base, ROp); 
    if (!PROp.hasValue())
      return true;
    // If Op is a part of ROp or Rop is a part of Op. then we have seen
    // a redundant release.
    if (!PROp.getValue().hasNonEmptySymmetricDifference(POp.getValue()))
      return true;
  }
  return false;
}

bool
ConsumedArgToEpilogueReleaseMatcher::
releaseAllNonTrivials(ReleaseList Insts, SILValue Base) {
  // Reason about whether all parts are released.
  SILModule *Mod = &(*Insts.begin())->getModule();

  // These are the list of SILValues that are actually released.
  ProjectionPathSet Paths;
  for (auto &I : Insts) {
    auto PP = ProjectionPath::getProjectionPath(Base, I->getOperand(0));
    if (!PP)
      return false;
    Paths.insert(PP.getValue());
  } 

  // Is there an uncovered non-trivial type.
  return !ProjectionPath::hasUncoveredNonTrivials(Base->getType(), Mod, Paths);
}

void ConsumedArgToEpilogueReleaseMatcher::findMatchingReleases(
    SILBasicBlock *BB) {
  // Iterate over the instructions post-order and find releases associated with
  // each arguments.
  //
  // Break on these conditions.
  //
  // 1. An instruction that can use ref count values.
  //
  // 2. A release that can not be mapped to any @owned argument.
  //
  // 3. A release that is mapped to an argument which already has a release
  // that overlaps with this release.
  for (auto II = std::next(BB->rbegin()), IE = BB->rend(); II != IE; ++II) {
    // If we do not have a release_value or strong_release...
    if (!isa<ReleaseValueInst>(*II) && !isa<StrongReleaseInst>(*II)) {
      // And the object cannot use values in a manner that will keep the object
      // alive, continue. We may be able to find additional releases.
      if (canNeverUseValues(&*II))
        continue;

      // Otherwise, we need to stop computing since we do not want to reduce the
      // lifetime of objects.
      break;
    }

    // Ok, we have a release_value or strong_release. Grab Target and find the
    // RC identity root of its operand.
    SILInstruction *Target = &*II;
    SILValue OrigOp = Target->getOperand(0);
    SILValue Op = RCFI->getRCIdentityRoot(OrigOp);

    // Check whether this is a SILArgument.
    auto *Arg = dyn_cast<SILArgument>(Op);
    // If this is not a SILArgument, maybe it is a part of a SILArgument.
    // This is possible after we expand release instructions in SILLowerAgg pass.
    if (!Arg) { 
      Arg = dyn_cast<SILArgument>(stripValueProjections(OrigOp));
    }

    // If Op is not a consumed argument, we must break since this is not an Op
    // that is a part of a return sequence. We are being conservative here since
    // we could make this more general by allowing for intervening non-arg
    // releases in the sense that we do not allow for race conditions in between
    // destructors.
    if (!Arg || !Arg->isFunctionArg() ||
        !Arg->hasConvention(SILArgumentConvention::Direct_Owned))
      break;

    // Ok, we have a release on a SILArgument that is direct owned. Attempt to
    // put it into our arc opts map. If we already have it, we have exited the
    // return value sequence so break. Otherwise, continue looking for more arc
    // operations.
    auto Iter = ArgInstMap.find(Arg);
    if (Iter == ArgInstMap.end()) {
      ArgInstMap[Arg].push_back(Target);
      continue;
    }

    // We've already seen at least part of this base. Check to see whether we
    // are seeing a redundant release.
    //
    // If we are seeing a redundant release we have exited the return value
    // sequence, so break.
    if (isRedundantRelease(Iter->second, Arg, OrigOp)) 
      break;
    
    // We've seen part of this base, but this is a part we've have not seen.
    // Record it. 
    Iter->second.push_back(Target);
  }

  // If we can not find a releases for all parts with reference semantics
  // that means we did not find all releases for the base.
  llvm::DenseSet<SILArgument *> ArgToRemove;
  for (auto &Arg : ArgInstMap) {
    // If an argument has a single release and it is rc-identical to the
    // SILArgument. Then we do not need to use projection to check for whether
    // all non-trivial fields are covered. This is a short-cut to avoid
    // projection for cost as well as accuracy. Projection currently does not
    // support single incoming argument as rc-identity does whereas rc-identity
    // does.
    if (Arg.second.size() == 1) {
      SILInstruction *I = *Arg.second.begin();
      SILValue RV = I->getOperand(0);
      if (Arg.first == RCFI->getRCIdentityRoot(RV))
        continue;
    }
    if (!releaseAllNonTrivials(Arg.second, Arg.first))
      ArgToRemove.insert(Arg.first);
  }

  for (auto &X : ArgToRemove) 
    ArgInstMap.erase(ArgInstMap.find(X));
}

//===----------------------------------------------------------------------===//
//                    Code for Determining Final Releases
//===----------------------------------------------------------------------===//

// Propagate liveness backwards from an initial set of blocks in our
// LiveIn set.
static void propagateLiveness(llvm::SmallPtrSetImpl<SILBasicBlock *> &LiveIn,
                              SILBasicBlock *DefBB) {
  // First populate a worklist of predecessors.
  llvm::SmallVector<SILBasicBlock *, 64> Worklist;
  for (auto *BB : LiveIn)
    for (auto Pred : BB->getPreds())
      Worklist.push_back(Pred);

  // Now propagate liveness backwards until we hit the alloc_box.
  while (!Worklist.empty()) {
    auto *BB = Worklist.pop_back_val();

    // If it's already in the set, then we've already queued and/or
    // processed the predecessors.
    if (BB == DefBB || !LiveIn.insert(BB).second)
      continue;

    for (auto Pred : BB->getPreds())
      Worklist.push_back(Pred);
  }
}

// Is any successor of BB in the LiveIn set?
static bool successorHasLiveIn(SILBasicBlock *BB,
                               llvm::SmallPtrSetImpl<SILBasicBlock *> &LiveIn) {
  for (auto &Succ : BB->getSuccessors())
    if (LiveIn.count(Succ))
      return true;

  return false;
}

// Walk backwards in BB looking for the last use of a given
// value, and add it to the set of release points.
static bool addLastUse(SILValue V, SILBasicBlock *BB,
                       ReleaseTracker &Tracker) {
  for (auto I = BB->rbegin(); I != BB->rend(); ++I) {
    for (auto &Op : I->getAllOperands())
      if (Op.get() == V) {
        Tracker.trackLastRelease(&*I);
        return true;
      }
  }

  llvm_unreachable("BB is expected to have a use of a closure");
  return false;
}

/// TODO: Refactor this code so the decision on whether or not to accept an
/// instruction.
bool swift::getFinalReleasesForValue(SILValue V, ReleaseTracker &Tracker) {
  llvm::SmallPtrSet<SILBasicBlock *, 16> LiveIn;
  llvm::SmallPtrSet<SILBasicBlock *, 16> UseBlocks;

  // First attempt to get the BB where this value resides.
  auto *DefBB = V->getParentBB();
  if (!DefBB)
    return false;

  bool seenRelease = false;
  SILInstruction *OneRelease = nullptr;

  // We'll treat this like a liveness problem where the value is the def. Each
  // block that has a use of the value has the value live-in unless it is the
  // block with the value.
  for (auto *UI : V->getUses()) {
    auto *User = UI->getUser();
    auto *BB = User->getParent();

    if (!Tracker.isUserAcceptable(User))
      return false;
    Tracker.trackUser(User);

    if (BB != DefBB)
      LiveIn.insert(BB);

    // Also keep track of the blocks with uses.
    UseBlocks.insert(BB);

    // Try to speed up the trivial case of single release/dealloc.
    if (isa<StrongReleaseInst>(User) || isa<DeallocBoxInst>(User)) {
      if (!seenRelease)
        OneRelease = User;
      else
        OneRelease = nullptr;

      seenRelease = true;
    }
  }

  // Only a single release/dealloc? We're done!
  if (OneRelease) {
    Tracker.trackLastRelease(OneRelease);
    return true;
  }

  propagateLiveness(LiveIn, DefBB);

  // Now examine each block we saw a use in. If it has no successors
  // that are in LiveIn, then the last use in the block is the final
  // release/dealloc.
  for (auto *BB : UseBlocks)
    if (!successorHasLiveIn(BB, LiveIn))
      if (!addLastUse(V, BB, Tracker))
        return false;

  return true;
}

//===----------------------------------------------------------------------===//
//                            Leaking BB Analysis
//===----------------------------------------------------------------------===//

static bool ignorableApplyInstInUnreachableBlock(const ApplyInst *AI) {
  const auto *Fn = AI->getCalleeFunction();
  if (!Fn)
    return false;

  return Fn->hasSemanticsAttr("arc.programtermination_point");
}

static bool ignorableBuiltinInstInUnreachableBlock(const BuiltinInst *BI) {
  const BuiltinInfo &BInfo = BI->getBuiltinInfo();
  if (BInfo.ID == BuiltinValueKind::CondUnreachable)
    return true;

  const IntrinsicInfo &IInfo = BI->getIntrinsicInfo();
  if (IInfo.ID == llvm::Intrinsic::trap)
    return true;

  return false;
}

/// Match a call to a trap BB with no ARC relevant side effects.
bool swift::isARCInertTrapBB(const SILBasicBlock *BB) {
  // Do a quick check at the beginning to make sure that our terminator is
  // actually an unreachable. This ensures that in many cases this function will
  // exit early and quickly.
  auto II = BB->rbegin();
  if (!isa<UnreachableInst>(*II))
    return false;

  auto IE = BB->rend();
  while (II != IE) {
    // Ignore any instructions without side effects.
    if (!II->mayHaveSideEffects()) {
      ++II;
      continue;
    }

    // Ignore cond fail.
    if (isa<CondFailInst>(*II)) {
      ++II;
      continue;
    }

    // Check for apply insts that we can ignore.
    if (auto *AI = dyn_cast<ApplyInst>(&*II)) {
      if (ignorableApplyInstInUnreachableBlock(AI)) {
        ++II;
        continue;
      }
    }

    // Check for builtins that we can ignore.
    if (auto *BI = dyn_cast<BuiltinInst>(&*II)) {
      if (ignorableBuiltinInstInUnreachableBlock(BI)) {
        ++II;
        continue;
      }
    }

    // If we can't ignore the instruction, return false.
    return false;
  }

  // Otherwise, we have an unreachable and every instruction is inert from an
  // ARC perspective in an unreachable BB.
  return true;
}
