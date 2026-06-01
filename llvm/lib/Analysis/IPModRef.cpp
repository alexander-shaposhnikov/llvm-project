//===- IPModRef.cpp - Interprocedural Mod/Ref analysis --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See IPModRef.h for an overview. This file implements Phase 1: an
// intraprocedural per-argument access-range summary for leaf functions, plus
// the AA hook that narrows getModRefInfo(CallBase, MemoryLocation) using it.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/IPModRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "ip-modref"

// Why a function's summary became unusable (AccessesUnknown) — run with -stats
// to see which bail reason dominates and target it. (Counts are per
// summarization; since summaries are computed on demand, the same function is
// counted once per query, so read these as a relative distribution.)
STATISTIC(NumArgImprecise, "Arg reached via variable offset / PHI / select (kept, whole-arg)");
STATISTIC(NumFreshCall, "noalias/allocation call result treated as fresh local");
STATISTIC(NumUnkLoad, "Bail: pointer loaded from memory");
STATISTIC(NumUnkPhiSelect, "Bail: pointer from PHI/select with unknown input");
STATISTIC(NumUnkCall, "Bail: pointer from call result");
STATISTIC(NumUnkOther, "Bail: pointer from inttoptr / other");
STATISTIC(NumBailVarArg, "Bail: variadic function");
STATISTIC(NumBailOtherInst, "Bail: other memory-touching instruction");
STATISTIC(NumBailIndirect, "Bail: indirect/unknown callee");
STATISTIC(NumBailDecl, "Bail: call to a declaration (no body)");
STATISTIC(NumBailRecursion, "Bail: recursive/SCC call");
STATISTIC(NumBailDepth, "Bail: call past max merge depth");
STATISTIC(NumBailCalleeOpaque, "Bail: defined callee summary unusable");
STATISTIC(NumBailDerefinable, "Bail: callee body may be derefined (linkonce_odr/weak/available_externally)");
STATISTIC(NumMergedCallees, "Callee summaries merged (Phase 2)");
STATISTIC(NumArgMemCalls, "Argmem-only calls bounded via attributes");
STATISTIC(NumLibFuncFnspec, "Calls characterized via libc fnspec");

/// Maximum number of access ranges tracked per argument before we collapse to
/// just the overall Mod/Ref (mirrors GCC's param-modref-max-accesses).
static cl::opt<unsigned> MaxRangesPerArg(
    "ip-modref-max-ranges", cl::Hidden, cl::init(16),
    cl::desc("Max access ranges tracked per argument by IPModRef"));

/// Max call-graph depth IPModRef recurses to merge callee summaries (Phase 2).
static cl::opt<unsigned> MaxMergeDepth(
    "ip-modref-max-merge-depth", cl::Hidden, cl::init(4),
    cl::desc("Max depth IPModRef recurses to merge callee summaries"));

/// Use the libc fnspec table (memcpy/strlen/...). Lets us A/B its contribution.
static cl::opt<bool> EnableFnspec(
    "ip-modref-fnspec", cl::Hidden, cl::init(true),
    cl::desc("Characterize known libc calls via IPModRef's fnspec table"));

// UNSOUND upper-bound measurement only. Pretends accesses through an unknown
// (loaded/opaque) intra-function pointer do not touch any argument or global --
// i.e. simulates perfect points-to disambiguation of the "loaded pointer" bail
// bucket. Use ONLY to measure the ceiling of recovering that bucket; never in
// production (it asserts disjointness we cannot prove without a points-to pass).
static cl::opt<bool> AssumeUnknownDisjoint(
    "ip-modref-assume-unknown-disjoint", cl::Hidden, cl::init(false),
    cl::desc("UNSOUND: assume unknown-pointer accesses miss args/globals "
             "(ceiling measurement for the loaded-pointer bail bucket)"));

// Sound per-argument handling of unknown-pointer accesses (idea #1): poison only
// non-noalias args + an "other memory" residue, instead of a global poison.
// Off => old global-poison behavior (lets us A/B the noalias recovery).
static cl::opt<bool> EnablePerArgUnknown(
    "ip-modref-per-arg-unknown", cl::Hidden, cl::init(true),
    cl::desc("Poison only non-noalias args (not the whole summary) on an "
             "access through an unknown pointer"));

// UNSOUND ceiling-measurement knob (off): treat EVERY call result as fresh
// local memory (not just noalias-returning ones). Estimates the upper bound on
// what return-value provenance could recover (the NumUnkCall bucket).
static cl::opt<bool> AssumeCallResultFresh(
    "ip-modref-assume-callresult-fresh", cl::Hidden, cl::init(false),
    cl::desc("UNSOUND: treat all call results as fresh (ceiling for the "
             "call-result bail bucket)"));

// A derefinable definition (linkonce_odr/weak_odr/weak/linkonce/common/
// available_externally -- i.e. GlobalValue::mayBeDerefined) may be replaced at
// link time by a different copy of the symbol whose LLVM-level memory behavior
// differs (different inlining, sanitizers, flags). Trusting *this* TU's body to
// prove a call accesses *less* than it might is therefore unsound -- precisely
// why FunctionAttrs gates all of its memory/arg inference on hasExactDefinition()
// and GCC's IPA-modref gates on cgraph node availability (AVAIL_INTERPOSABLE).
// Default off (sound): treat a derefinable callee as opaque, exactly like a
// declaration. Set true ONLY to A/B how much of the win rested on derefinable
// bodies, or in a whole-program/LTO context where the prevailing definition is
// already resolved.
static cl::opt<bool> TrustDerefinableBodies(
    "ip-modref-trust-derefinable-bodies", cl::Hidden, cl::init(false),
    cl::desc("UNSOUND outside LTO: trust linkonce_odr/weak_odr/"
             "available_externally callee bodies for interprocedural "
             "refinement (measure-only; default off matches FunctionAttrs)"));

static StringRef mrStr(ModRefInfo MR) {
  switch (MR) {
  case ModRefInfo::NoModRef:
    return "NoModRef";
  case ModRefInfo::Ref:
    return "Ref";
  case ModRefInfo::Mod:
    return "Mod";
  case ModRefInfo::ModRef:
    return "ModRef";
  }
  llvm_unreachable("unhandled ModRefInfo");
}

/// Record one memory access (Ptr + ExtraOffset, Size, MR) into the summary of
/// \p F. ExtraOffset (default 0) lets a merged callee range be shifted by the
/// callee's in-arg offset; Phase-1 callers pass 0.
static void recordAccess(const DataLayout &DL, const Function &F,
                         IPModRefResult::FunctionSummary &S, const Value *Ptr,
                         LocationSize Size, ModRefInfo MR,
                         int64_t ExtraOffset = 0) {
  unsigned BW = DL.getIndexTypeSizeInBits(Ptr->getType());
  APInt Off(BW, 0);
  const Value *Base =
      Ptr->stripAndAccumulateConstantOffsets(DL, Off, /*AllowNonInbounds=*/true);

  if (const auto *Arg = dyn_cast<Argument>(Base)) {
    if (Arg->getParent() == &F && Arg->getType()->isPointerTy()) {
      IPModRefResult::ArgInfo &AI = S.Args[Arg->getArgNo()];
      AI.Overall |= MR;
      if (!AI.RangesKnown)
        return;
      // Non-constant or oversized offset, or too many ranges: collapse.
      if (!Off.isSignedIntN(64) || AI.Ranges.size() >= MaxRangesPerArg) {
        AI.RangesKnown = false;
        AI.Ranges.clear();
        return;
      }
      AI.Ranges.emplace_back(Off.getSExtValue() + ExtraOffset, Size, MR);
      return;
    }
    // Argument of some other function leaking in: be conservative.
    S.AccessesUnknown = true;
    ++NumUnkOther;
    return;
  }

  // A local alloca is invisible to callers (Phase 1 has no calls, so it cannot
  // be observed after return).
  if (const auto *AInst = dyn_cast<AllocaInst>(Base))
    if (AInst->getFunction() == &F)
      return;

  // A directly-accessed global variable is a distinct, identified object. Track
  // it separately from arguments so we stay precise about the arguments; a
  // query only conflicts with it if the queried location may alias the global.
  if (const auto *GV = dyn_cast<GlobalValue>(Base)) {
    S.GlobalsMR |= MR;
    S.Globals.insert(GV);
    return;
  }

  // The base wasn't an argument-at-constant-offset / local / global. Dig through
  // variable-offset GEPs, PHIs and selects to the underlying objects: if they
  // all resolve to arguments (at unknown offset -> whole-arg), locals or
  // globals, we stay usable instead of bailing. Only a genuinely opaque object
  // (a loaded pointer, a call result, ...) forces AccessesUnknown.
  SmallVector<const Value *, 4> Objs;
  getUnderlyingObjects(Ptr, Objs);
  for (const Value *O : Objs) {
    if (const auto *A = dyn_cast<Argument>(O);
        A && A->getParent() == &F && A->getType()->isPointerTy()) {
      IPModRefResult::ArgInfo &AI = S.Args[A->getArgNo()];
      AI.Overall |= MR;
      AI.RangesKnown = false;
      AI.Ranges.clear();
      ++NumArgImprecise;
      continue;
    }
    if (const auto *AInst = dyn_cast<AllocaInst>(O);
        AInst && AInst->getFunction() == &F)
      continue;
    // A noalias call result (malloc/new/...) is fresh memory, distinct from the
    // caller's arguments and globals, so accessing it has no caller-visible
    // effect -- treat it like a local (mirrors GCC's MODREF_LOCAL_MEMORY).
    if (const auto *CB = dyn_cast<CallBase>(O);
        CB && (CB->returnDoesNotAlias() || AssumeCallResultFresh)) {
      ++NumFreshCall;
      continue;
    }
    if (const auto *GV = dyn_cast<GlobalValue>(O)) {
      S.GlobalsMR |= MR;
      S.Globals.insert(GV);
      continue;
    }
    // Access through a genuinely unknown (loaded/opaque) pointer. By the
    // noalias contract it cannot alias a noalias argument it is not derived
    // from (and it isn't -- its base is this opaque object), so poison only the
    // *non-noalias* pointer args and record the rest as an "other memory"
    // residue (idea #1: per-arg instead of a global poison). The
    // ceiling-measurement knob suppresses even this.
    if (AssumeUnknownDisjoint) {
      // ceiling knob: suppress the poison entirely (unsound, measurement only).
    } else if (!EnablePerArgUnknown) {
      S.AccessesUnknown = true; // old behavior: global poison.
    } else {
      for (const Argument &FA : F.args()) {
        if (!FA.getType()->isPointerTy() || FA.hasNoAliasAttr())
          continue;
        IPModRefResult::ArgInfo &PAI = S.Args[FA.getArgNo()];
        PAI.Overall |= MR;
        PAI.RangesKnown = false;
        PAI.Ranges.clear();
      }
      S.OtherMR |= MR;
    }
    if (isa<LoadInst>(O))
      ++NumUnkLoad;
    else if (isa<PHINode>(O) || isa<SelectInst>(O))
      ++NumUnkPhiSelect;
    else if (isa<CallBase>(O))
      ++NumUnkCall;
    else
      ++NumUnkOther;
  }
}

/// Bound a call that only touches argument memory (and/or caller-invisible
/// inaccessible heap) by recording each pointer argument as a whole-arg access,
/// instead of bailing. Covers argmemonly externals/libc (free, strlen, strcpy,
/// ...) we can't merge a body for.
static void recordArgMemCall(const DataLayout &DL, const Function &F,
                             IPModRefResult::FunctionSummary &S,
                             const CallBase &CB) {
  ModRefInfo ArgMR = CB.getMemoryEffects().getModRef(IRMemLocation::ArgMem);
  if (ArgMR == ModRefInfo::NoModRef)
    return; // only touches inaccessible heap (e.g. malloc): no caller effect.
  for (unsigned I = 0, E = CB.arg_size(); I != E; ++I) {
    const Value *A = CB.getArgOperand(I);
    if (!A->getType()->isPointerTy())
      continue;
    if (CB.paramHasAttr(I, Attribute::ReadNone))
      continue;
    ModRefInfo MR = ArgMR;
    if (CB.paramHasAttr(I, Attribute::ReadOnly))
      MR = ModRefInfo::Ref;
    else if (CB.paramHasAttr(I, Attribute::WriteOnly))
      MR = ModRefInfo::Mod;
    // Whole-arg, unknown extent.
    recordAccess(DL, F, S, A, LocationSize::beforeOrAfterPointer(), MR);
  }
}

/// One access a libc fnspec attributes to a pointer argument of the call:
/// \p ArgNo accessed at parameter-relative offset 0 for \p Size bytes, \p MR.
namespace {
struct FnspecAccess {
  unsigned ArgNo;
  LocationSize Size;
  ModRefInfo MR;
};
} // namespace

/// Hand-written summaries for the highest-volume libc functions (GCC's
/// "fnspec"). These have fixed, standard-defined semantics and no IR body even
/// under LTO, so a table is the only way to characterize them -- and it is more
/// precise than the argmem-attribute fallback: it knows which argument is read
/// vs written and, when the size argument is a constant, the *exact* byte range
/// (so e.g. a store past memcpy's length survives). Sound because
/// TargetLibraryInfo confirms the call's name + signature and honors
/// nobuiltin/user-redefinition. Returns true if \p CB is a recognized library
/// call (filling \p Out with its accesses; empty Out = recognized but touches
/// no argument/global memory, e.g. malloc).
static bool getLibFuncFnspec(const TargetLibraryInfo &TLI, const CallBase &CB,
                             SmallVectorImpl<FnspecAccess> &Out) {
  LibFunc LF;
  if (!TLI.getLibFunc(CB, LF))
    return false;
  // Byte count for an access whose size is given by argument \p I (exact when
  // that argument is a constant, otherwise unknown -> whole-arg).
  auto SizeFromArg = [&](unsigned I) -> LocationSize {
    if (I < CB.arg_size())
      if (const auto *C = dyn_cast<ConstantInt>(CB.getArgOperand(I)))
        return LocationSize::precise(C->getZExtValue());
    return LocationSize::afterPointer();
  };
  switch (LF) {
  case LibFunc_memcpy:
  case LibFunc_memmove:
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Mod});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_memset:
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Mod});
    return true;
  case LibFunc_memcmp:
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Ref});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_strlen:
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Ref});
    return true;
  case LibFunc_malloc:
  case LibFunc_calloc:
    // No pointer arguments and the result is fresh (noalias) memory: no
    // argument/global access to record.
    return true;
  case LibFunc_free:
    // free invalidates the pointee's contents: model as a (whole-arg) write.
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Mod});
    return true;

  // --- C++ operator new / new[] : fresh (noalias) result, no arg memory ---
  case LibFunc_Znwj:
  case LibFunc_Znwm:
  case LibFunc_Znaj:
  case LibFunc_Znam:
  case LibFunc_ZnwjRKSt9nothrow_t:
  case LibFunc_ZnwmRKSt9nothrow_t:
  case LibFunc_ZnajRKSt9nothrow_t:
  case LibFunc_ZnamRKSt9nothrow_t:
    return true;

  // --- C++ operator delete / delete[] : invalidates the pointee (arg0) ---
  case LibFunc_ZdlPv:
  case LibFunc_ZdaPv:
  case LibFunc_ZdlPvj:
  case LibFunc_ZdlPvm:
  case LibFunc_ZdaPvj:
  case LibFunc_ZdaPvm:
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Mod});
    return true;

  // --- size-carrying mem*/str* : exact ranges from the size argument ---
  case LibFunc_mempcpy: // (dst, src, n)
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Mod});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_memchr: // (s, c, n)
  case LibFunc_memrchr:
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_bcmp: // (a, b, n)
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Ref});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_bcopy: // (src, dst, n) -- note BSD arg order
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Ref});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Mod});
    return true;
  case LibFunc_bzero: // (s, n)
    Out.push_back({0, SizeFromArg(1), ModRefInfo::Mod});
    return true;
  case LibFunc_strncmp: // (a, b, n)
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Ref});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_strncpy: // (dst, src, n)
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Mod});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_strnlen: // (s, n)
    Out.push_back({0, SizeFromArg(1), ModRefInfo::Ref});
    return true;
  case LibFunc_strndup: // (s, n) -- fresh result
    Out.push_back({0, SizeFromArg(1), ModRefInfo::Ref});
    return true;

  // --- NUL-terminated str* : whole-arg, but precise Mod-vs-Ref kind ---
  case LibFunc_strchr: // (s, c)
  case LibFunc_strrchr:
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Ref});
    return true;
  case LibFunc_strcmp: // (a, b)
  case LibFunc_strcasecmp:
  case LibFunc_strstr:
  case LibFunc_strpbrk:
  case LibFunc_strspn:
  case LibFunc_strcspn:
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Ref});
    Out.push_back({1, LocationSize::afterPointer(), ModRefInfo::Ref});
    return true;
  case LibFunc_strcpy: // (dst, src)
  case LibFunc_stpcpy:
  case LibFunc_strcat:
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Mod});
    Out.push_back({1, LocationSize::afterPointer(), ModRefInfo::Ref});
    return true;
  case LibFunc_strdup: // (s) -- fresh result
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Ref});
    return true;
  case LibFunc_strncat: // (dst, src, n) -- dst appended (whole), src read [0,n)
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Mod});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;

  // --- fortified __*_chk : size arg is shifted by the dstlen argument ---
  case LibFunc_memcpy_chk: // (dst, src, n, dstlen)
  case LibFunc_memmove_chk:
  case LibFunc_mempcpy_chk:
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Mod});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_memset_chk: // (dst, c, n, dstlen)
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Mod});
    return true;
  case LibFunc_strcpy_chk: // (dst, src, dstlen) -- no copy-size arg
  case LibFunc_stpcpy_chk:
  case LibFunc_strcat_chk:
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Mod});
    Out.push_back({1, LocationSize::afterPointer(), ModRefInfo::Ref});
    return true;
  case LibFunc_strncpy_chk: // (dst, src, n, dstlen)
    Out.push_back({0, SizeFromArg(2), ModRefInfo::Mod});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;
  case LibFunc_strncat_chk: // (dst, src, n, dstlen)
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Mod});
    Out.push_back({1, SizeFromArg(2), ModRefInfo::Ref});
    return true;

  // --- posix_memalign(&p, align, size) : writes the out-pointer *arg0 ---
  case LibFunc_posix_memalign:
    Out.push_back({0, LocationSize::afterPointer(), ModRefInfo::Mod});
    return true;

  default:
    return false;
  }
}

/// Merge a callee summary \p GSum into the caller \p F's summary \p S, remapping
/// the callee's per-argument access ranges through the actual call arguments.
static void mergeCallee(const DataLayout &DL, const Function &F,
                        IPModRefResult::FunctionSummary &S, const CallBase &CB,
                        const IPModRefResult::FunctionSummary &GSum) {
  // Callee touches memory we couldn't place -> so does the caller via the call.
  if (!GSum.Usable || GSum.AccessesUnknown) {
    S.AccessesUnknown = true;
    return;
  }
  // The callee's direct-global accesses are the caller's too.
  S.GlobalsMR |= GSum.GlobalsMR;
  for (const GlobalValue *GV : GSum.Globals)
    S.Globals.insert(GV);

  unsigned NumArgs = CB.arg_size();
  for (unsigned J = 0, E = GSum.Args.size(); J != E && J < NumArgs; ++J) {
    const IPModRefResult::ArgInfo &GA = GSum.Args[J];
    if (GA.Overall == ModRefInfo::NoModRef)
      continue;
    const Value *Actual = CB.getArgOperand(J);
    if (!Actual->getType()->isPointerTy())
      continue;
    // Each callee access through its arg J at [Offset, Offset+Size) maps to the
    // actual argument shifted by Offset; recordAccess re-classifies the actual
    // base (caller arg range / global / local / unknown).
    if (GA.RangesKnown) {
      for (const IPModRefResult::ArgAccess &RA : GA.Ranges)
        recordAccess(DL, F, S, Actual, RA.Size, RA.MR, RA.Offset);
    } else {
      recordAccess(DL, F, S, Actual, LocationSize::beforeOrAfterPointer(),
                   GA.Overall, 0);
    }
  }
}

IPModRefResult::FunctionSummary
IPModRefResult::computeSummary(const Function &F) const {
  SmallPtrSet<const Function *, 8> OnPath;
  DenseMap<const Function *, FunctionSummary> Memo;
  return computeSummaryImpl(F, 0, OnPath, Memo);
}

IPModRefResult::FunctionSummary IPModRefResult::computeSummaryImpl(
    const Function &F, unsigned Depth,
    SmallPtrSetImpl<const Function *> &OnPath,
    DenseMap<const Function *, FunctionSummary> &Memo) const {
  if (auto It = Memo.find(&F); It != Memo.end())
    return It->second;

  FunctionSummary S;

  // Only summarize definitions; queries about declarations fall back to ModRef.
  if (F.isDeclaration())
    return S;

  // A derefinable definition's body may be replaced at link time by a copy with
  // different memory behavior, so trusting it to prove a call accesses *less*
  // would be unsound (see TrustDerefinableBodies). Treat it as opaque -- the
  // single chokepoint both the AA-query site and the recursive callee-merge
  // flow through, so this guards both at once.
  if (!TrustDerefinableBodies && !F.hasExactDefinition())
    return S;

  OnPath.insert(&F);
  S.Usable = true;
  S.Args.resize(F.arg_size());

  // For recognizing libc calls (memcpy/strlen/...) and applying their fnspec.
  TargetLibraryInfo TLI(TLImpl, &F);

  // Variadic callees may access their variadic arguments in ways we cannot
  // attribute to a fixed parameter; don't narrow for them.
  if (F.isVarArg()) {
    S.AccessesUnknown = true;
    ++NumBailVarArg;
  }

  for (const Instruction &I : instructions(F)) {
    // Loads, stores, atomics, va_arg: a concrete (Ptr, Size) location.
    if (std::optional<MemoryLocation> Loc = MemoryLocation::getOrNone(&I)) {
      bool R = I.mayReadFromMemory();
      bool W = I.mayWriteToMemory();
      ModRefInfo MR = (R && W) ? ModRefInfo::ModRef
                               : (W ? ModRefInfo::Mod : ModRefInfo::Ref);
      // Normalize scalable (and unknown) sizes to "unknown bytes".
      LocationSize Sz = Loc->Size;
      if (Sz.hasValue() && Sz.getValue().isScalable())
        Sz = LocationSize::beforeOrAfterPointer();
      recordAccess(DL, F, S, Loc->Ptr, Sz, MR);
      continue;
    }

    // Calls. Most callees need interprocedural propagation, but a few have
    // well-known effects we can model directly (GCC's "fnspec").
    if (const auto *CB = dyn_cast<CallBase>(&I)) {
      // No caller-visible memory effect.
      if (CB->doesNotAccessMemory() || CB->onlyAccessesInaccessibleMemory())
        continue;
      if (const auto *II = dyn_cast<IntrinsicInst>(&I)) {
        // memset/memcpy/memmove: model as argument access ranges. recordAccess
        // classifies the dest/src base (argument range, global, local, or
        // unknown) just like a plain store/load would.
        if (const auto *MI = dyn_cast<MemIntrinsic>(II)) {
          const auto *Len = dyn_cast<ConstantInt>(MI->getLength());
          LocationSize Sz = Len ? LocationSize::precise(Len->getZExtValue())
                                : LocationSize::beforeOrAfterPointer();
          recordAccess(DL, F, S, MI->getRawDest(), Sz, ModRefInfo::Mod);
          if (const auto *MTI = dyn_cast<MemTransferInst>(MI))
            recordAccess(DL, F, S, MTI->getRawSource(), Sz, ModRefInfo::Ref);
          continue;
        }
        // Intrinsics with no observable effect on caller-visible memory.
        switch (II->getIntrinsicID()) {
        case Intrinsic::assume:
        case Intrinsic::donothing:
        case Intrinsic::sideeffect:
        case Intrinsic::experimental_noalias_scope_decl:
          continue;
        default:
          break;
        }
      }
      // Known libc functions get a precise hand-written summary (GCC fnspec),
      // even as declarations -- more precise than the argmem fallback and not
      // reachable by Phase-2 merge (no IR body). Remap each modeled access
      // through the actual argument (recordAccess classifies its base).
      SmallVector<FnspecAccess, 2> FA;
      if (EnableFnspec && getLibFuncFnspec(TLI, *CB, FA)) {
        for (const FnspecAccess &A : FA)
          recordAccess(DL, F, S, CB->getArgOperand(A.ArgNo), A.Size, A.MR);
        ++NumLibFuncFnspec;
        continue;
      }
      // Interprocedural propagation: merge a defined callee's summary by
      // remapping its arg ranges (Phase 2).
      const Function *G = CB->getCalledFunction();
      bool Handled = false;
      if (G && !G->isDeclaration() && Depth < MaxMergeDepth &&
          !OnPath.contains(G)) {
        FunctionSummary GSum = computeSummaryImpl(*G, Depth + 1, OnPath, Memo);
        if (GSum.Usable && !GSum.AccessesUnknown) {
          mergeCallee(DL, F, S, *CB, GSum);
          ++NumMergedCallees;
          Handled = true;
        }
      }
      // Fallback: an argmem-only (and/or inaccessible) call -- e.g. an
      // argmemonly external/libc we can't merge -- is bounded to its pointer
      // arguments rather than poisoning the whole summary.
      if (!Handled) {
        if (CB->onlyAccessesArgMemory() ||
            CB->onlyAccessesInaccessibleMemOrArgMem()) {
          recordArgMemCall(DL, F, S, *CB);
          ++NumArgMemCalls;
        } else {
          S.AccessesUnknown = true;
          if (!G)
            ++NumBailIndirect;
          else if (G->isDeclaration())
            ++NumBailDecl;
          else if (!TrustDerefinableBodies && !G->hasExactDefinition())
            ++NumBailDerefinable;
          else if (OnPath.contains(G))
            ++NumBailRecursion;
          else if (Depth >= MaxMergeDepth)
            ++NumBailDepth;
          else
            ++NumBailCalleeOpaque;
        }
      }
      continue;
    }

    // Any other memory-touching instruction (fence, etc.): be conservative.
    if (I.mayReadOrWriteMemory()) {
      S.AccessesUnknown = true;
      ++NumBailOtherInst;
    }
  }

  OnPath.erase(&F);
  Memo[&F] = S;
  return S;
}

IPModRefResult IPModRefResult::analyzeModule(Module &M) {
  // The result is stateless: summaries are computed on demand from current IR.
  return IPModRefResult(M.getDataLayout(), M.getTargetTriple());
}

/// Do byte ranges [Delta, Delta+LocSize) and the access range \p RA overlap?
static bool rangesOverlap(int64_t Delta, uint64_t LocSize,
                          const IPModRefResult::ArgAccess &RA) {
  if (!RA.Size.hasValue())
    return true; // unknown range size: assume overlap (conservative).
  int64_t AStart = Delta;
  int64_t AEnd = Delta + static_cast<int64_t>(LocSize);
  int64_t BStart = RA.Offset;
  int64_t BEnd = RA.Offset + static_cast<int64_t>(RA.Size.getValue().getFixedValue());
  return AStart < BEnd && BStart < AEnd;
}

ModRefInfo IPModRefResult::getModRefInfo(const CallBase *Call,
                                         const MemoryLocation &Loc,
                                         AAQueryInfo &AAQI) {
  const Function *F = Call->getCalledFunction();
  if (!F || !Loc.Ptr)
    return AAResultBase::getModRefInfo(Call, Loc, AAQI);

  // Summaries are computed on demand from the callee's *current* body, so this
  // is always sound w.r.t. function mutation/deletion (no caching).
  FunctionSummary Summary = computeSummary(*F);
  FunctionSummary LibBacking;
  const FunctionSummary *S = &Summary;

  // A recognized libc callee (often a declaration -> no usable body summary)
  // gets its fnspec applied directly to *this call*, so the call-site constant
  // size argument yields exact per-argument ranges (e.g. memcpy(d, s, 16)).
  if (!Summary.Usable && EnableFnspec) {
    SmallVector<FnspecAccess, 2> FA;
    TargetLibraryInfo TLI(TLImpl, F);
    if (getLibFuncFnspec(TLI, *Call, FA)) {
      LibBacking.Usable = true;
      LibBacking.Args.resize(Call->arg_size());
      for (const FnspecAccess &A : FA) {
        if (A.ArgNo >= LibBacking.Args.size())
          continue;
        ArgInfo &AI = LibBacking.Args[A.ArgNo];
        AI.Overall |= A.MR;
        AI.Ranges.emplace_back(/*Offset=*/0, A.Size, A.MR); // param-relative
      }
      S = &LibBacking;
    }
  }

  if (!S->Usable || S->AccessesUnknown)
    return AAResultBase::getModRefInfo(Call, Loc, AAQI);

  unsigned LocBW = DL.getIndexTypeSizeInBits(Loc.Ptr->getType());
  APInt LocOff(LocBW, 0);
  const Value *LocBase =
      Loc.Ptr->stripAndAccumulateConstantOffsets(DL, LocOff, true);

  ModRefInfo Result = ModRefInfo::NoModRef;
  unsigned NumArgs = Call->arg_size();
  for (unsigned I = 0; I < NumArgs && I < S->Args.size(); ++I) {
    const ArgInfo &AI = S->Args[I];
    if (AI.Overall == ModRefInfo::NoModRef)
      continue;

    const Value *Arg = Call->getArgOperand(I);
    if (!Arg->getType()->isPointerTy())
      continue;

    unsigned ArgBW = DL.getIndexTypeSizeInBits(Arg->getType());
    APInt ArgOff(ArgBW, 0);
    const Value *ArgBase =
        Arg->stripAndAccumulateConstantOffsets(DL, ArgOff, true);

    if (ArgBase == LocBase && ArgBW == LocBW) {
      // Loc lies at a known constant offset Delta into the argument pointer.
      APInt DeltaAP = LocOff - ArgOff;
      if (!AI.RangesKnown || !Loc.Size.hasValue() ||
          Loc.Size.getValue().isScalable() || !DeltaAP.isSignedIntN(64)) {
        Result |= AI.Overall;
      } else {
        int64_t Delta = DeltaAP.getSExtValue();
        uint64_t LocSize = Loc.Size.getValue().getFixedValue();
        for (const ArgAccess &RA : AI.Ranges)
          if (rangesOverlap(Delta, LocSize, RA))
            Result |= RA.MR;
      }
    } else {
      // Cannot place Loc relative to this argument. The argument's object is
      // the only way the call can touch Loc through this parameter; if they
      // cannot alias, this argument contributes nothing.
      AliasResult AR = AAQI.AAR.alias(MemoryLocation::getBeforeOrAfter(Arg), Loc,
                                      AAQI, Call);
      if (AR != AliasResult::NoAlias)
        Result |= AI.Overall;
    }

    if (isModAndRefSet(Result))
      break;
  }

  // Account for directly-accessed globals: they conflict with Loc only if Loc
  // may alias one of them (e.g. the caller passed the global's address, or Loc
  // is that global). A fresh local / distinct object excludes them.
  if (!isModAndRefSet(Result) && S->GlobalsMR != ModRefInfo::NoModRef) {
    for (const GlobalValue *GV : S->Globals) {
      if (AAQI.AAR.alias(MemoryLocation::getBeforeOrAfter(GV), Loc, AAQI,
                         Call) != AliasResult::NoAlias) {
        Result |= S->GlobalsMR;
        break;
      }
    }
  }

  // The unknown-pointer residue may hit Loc unless Loc lies exactly within one
  // of the call's noalias arguments (which such a pointer, not derived from
  // that argument, provably cannot alias).
  if (!isModAndRefSet(Result) && S->OtherMR != ModRefInfo::NoModRef) {
    bool LocInNoaliasArg = false;
    for (unsigned I = 0; I < NumArgs; ++I) {
      if (!Call->paramHasAttr(I, Attribute::NoAlias))
        continue;
      const Value *Arg = Call->getArgOperand(I);
      if (!Arg->getType()->isPointerTy())
        continue;
      unsigned ArgBW = DL.getIndexTypeSizeInBits(Arg->getType());
      APInt ArgOff(ArgBW, 0);
      const Value *ArgBase =
          Arg->stripAndAccumulateConstantOffsets(DL, ArgOff, true);
      if (ArgBase == LocBase && ArgBW == LocBW) {
        LocInNoaliasArg = true;
        break;
      }
    }
    if (!LocInNoaliasArg)
      Result |= S->OtherMR;
  }
  return Result;
}

void IPModRefResult::print(raw_ostream &OS, const Module &M) const {
  for (const Function &Fn : M) {
    FunctionSummary S = computeSummary(Fn);
    if (!S.Usable)
      continue;
    const Function *F = &Fn;
    OS << "IPModRef summary for '" << F->getName()
       << "' (AccessesUnknown=" << S.AccessesUnknown
       << ", Globals=" << mrStr(S.GlobalsMR)
       << ", Other=" << mrStr(S.OtherMR) << ")\n";
    for (unsigned I = 0, E = S.Args.size(); I != E; ++I) {
      const ArgInfo &AI = S.Args[I];
      if (AI.Overall == ModRefInfo::NoModRef && AI.Ranges.empty())
        continue;
      OS << "  arg " << I << ": " << mrStr(AI.Overall)
         << " RangesKnown=" << AI.RangesKnown << "\n";
      for (const ArgAccess &RA : AI.Ranges) {
        OS << "    [" << RA.Offset << ", ";
        if (RA.Size.hasValue())
          OS << (RA.Offset +
                 static_cast<int64_t>(RA.Size.getValue().getFixedValue()));
        else
          OS << "?";
        OS << ") " << mrStr(RA.MR) << "\n";
      }
    }
  }
}

bool IPModRefResult::invalidate(Module &, const PreservedAnalyses &PA,
                                ModuleAnalysisManager::Invalidator &) {
  // Like GlobalsAA, this result is treated as stateless: it remains valid
  // unless explicitly invalidated. (Required by the AAManager proxy, which
  // verifies cached AA results are not invalidatable.)
  auto PAC = PA.getChecker<IPModRef>();
  return !PAC.preservedWhenStateless();
}

AnalysisKey IPModRef::Key;

IPModRefResult IPModRef::run(Module &M, ModuleAnalysisManager &) {
  return IPModRefResult::analyzeModule(M);
}

PreservedAnalyses IPModRefPrinterPass::run(Module &M,
                                           ModuleAnalysisManager &AM) {
  AM.getResult<IPModRef>(M).print(OS, M);
  return PreservedAnalyses::all();
}
