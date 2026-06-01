//===- IPModRef.h - Interprocedural Mod/Ref analysis ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// Interprocedural mod/ref analysis (a port of GCC's IPA-modref pass).
///
/// For each defined function this analysis records, per pointer argument, the
/// set of memory *access ranges* (byte offset + size, with Mod/Ref kind) that
/// the function performs through that argument. This is finer-grained than the
/// existing function/argument memory attributes: it lets the alias oracle prove
/// that a call does not touch a particular sub-object of an argument even when
/// the function does access (a different part of) that argument.
///
/// The result plugs into the AA framework and refines
/// getModRefInfo(CallBase, MemoryLocation).
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_IPMODREF_H
#define LLVM_ANALYSIS_IPMODREF_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ModRef.h"

namespace llvm {

class Function;
class GlobalValue;
class Module;
class raw_ostream;

/// Interprocedural mod/ref result: per-function summaries plus the AA hooks
/// that consume them.
class IPModRefResult : public AAResultBase {
public:
  /// A single access range relative to an argument's pointer value.
  struct ArgAccess {
    int64_t Offset;    ///< Bytes from the argument pointer (may be negative).
    LocationSize Size; ///< Bytes accessed; may be unknown.
    ModRefInfo MR;     ///< Ref / Mod / ModRef for this range.

    ArgAccess(int64_t Offset, LocationSize Size, ModRefInfo MR)
        : Offset(Offset), Size(Size), MR(MR) {}
  };

  /// Per-argument access information.
  struct ArgInfo {
    /// Union of MR over all accesses through this argument (including the
    /// unknown-offset residue). Meaningful even when !RangesKnown.
    ModRefInfo Overall = ModRefInfo::NoModRef;
    /// When true, \p Ranges exactly describes the accesses through this arg:
    /// memory outside the ranges is not accessed. When false only \p Overall
    /// is meaningful.
    bool RangesKnown = true;
    SmallVector<ArgAccess, 4> Ranges;
  };

  /// Summary for one function.
  struct FunctionSummary {
    /// The function has an analyzable body (is a definition).
    bool Usable = false;
    /// The function accesses memory through a pointer we could not attribute to
    /// an argument or to a specific global variable (a loaded/unknown pointer,
    /// or an uncharacterized call). When set, the summary cannot narrow at all:
    /// the unknown pointer might alias an argument, so the per-argument ranges
    /// are not a complete characterization.
    bool AccessesUnknown = false;
    /// Mod/ref of accesses through an unknown (loaded/opaque) intra-function
    /// pointer. Such a pointer cannot alias a \c noalias argument it is not
    /// derived from, so these poison only the non-noalias arguments; this
    /// residue conservatively may hit anything else (globals / other memory),
    /// so a query includes it unless \c Loc lies exactly within a noalias arg.
    ModRefInfo OtherMR = ModRefInfo::NoModRef;
    /// Mod/ref of *directly* accessed global variables (base is a GlobalValue),
    /// tracked separately from arguments. A query only includes this when the
    /// queried location may alias one of \p Globals.
    ModRefInfo GlobalsMR = ModRefInfo::NoModRef;
    SmallPtrSet<const GlobalValue *, 4> Globals;
    /// Indexed by Argument number.
    SmallVector<ArgInfo, 4> Args;
  };

  IPModRefResult(const DataLayout &DL, const Triple &T) : DL(DL), TLImpl(T) {}
  IPModRefResult(IPModRefResult &&Arg)
      : AAResultBase(std::move(Arg)), DL(Arg.DL),
        TLImpl(std::move(Arg.TLImpl)) {}

  LLVM_ABI static IPModRefResult analyzeModule(Module &M);

  /// Compute the access-range summary for \p F from its *current* body.
  ///
  /// Summaries are computed on demand and never cached across the pass
  /// pipeline. This is deliberately stateless: it makes the result sound by
  /// construction in the presence of function mutation/deletion (no stale
  /// summaries, no Function* lifetime hazards). Returns a non-Usable summary
  /// for declarations and varargs/unknown-access functions.
  LLVM_ABI FunctionSummary computeSummary(const Function &F) const;

  LLVM_ABI void print(raw_ostream &OS, const Module &M) const;

  LLVM_ABI bool invalidate(Module &M, const PreservedAnalyses &PA,
                           ModuleAnalysisManager::Invalidator &);

  //------------------------------------------------
  // AliasAnalysis API
  //
  using AAResultBase::getModRefInfo;
  LLVM_ABI ModRefInfo getModRefInfo(const CallBase *Call,
                                    const MemoryLocation &Loc,
                                    AAQueryInfo &AAQI);

private:
  const DataLayout &DL;
  /// Per-module library-function info, used to recognize libc calls (memcpy,
  /// strlen, ...) and give them a precise hand-written summary ("fnspec") even
  /// for declarations. Stored by value so the result stays self-contained at
  /// AA-query time (preserves the on-demand/stateless model).
  TargetLibraryInfoImpl TLImpl;

  /// Recursive worker for computeSummary: merges callee summaries (Phase 2).
  /// OnPath is the current DFS stack (cycle guard); Memo caches per-function
  /// summaries within one traversal. Bounded by Depth.
  FunctionSummary
  computeSummaryImpl(const Function &F, unsigned Depth,
                     SmallPtrSetImpl<const Function *> &OnPath,
                     DenseMap<const Function *, FunctionSummary> &Memo) const;
};

/// Module analysis producing an \c IPModRefResult.
class IPModRef : public AnalysisInfoMixin<IPModRef> {
  friend AnalysisInfoMixin<IPModRef>;
  LLVM_ABI static AnalysisKey Key;

public:
  using Result = IPModRefResult;
  LLVM_ABI IPModRefResult run(Module &M, ModuleAnalysisManager &AM);
};

/// Printer pass for \c IPModRef (\c print<ip-modref>).
class IPModRefPrinterPass : public PassInfoMixin<IPModRefPrinterPass> {
  raw_ostream &OS;

public:
  explicit IPModRefPrinterPass(raw_ostream &OS) : OS(OS) {}
  LLVM_ABI PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_ANALYSIS_IPMODREF_H
