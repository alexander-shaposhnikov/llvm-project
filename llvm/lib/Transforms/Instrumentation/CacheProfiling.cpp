//===- CacheProfiling.cpp - inline L1 cache simulation -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Instrument every scalar load/store with an inlined direct-mapped L1 cache
// model. Hit/miss counters live in thread-local globals supplied by the
// compiler-rt runtime; they are aggregated and dumped to a cache profile
// file at process exit.
//
// MVP scope: a single pair of global counters (l1_hits / l1_misses). No
// per-source-location attribution yet.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/CacheProfiling.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

#define DEBUG_TYPE "cache-profile"

// Geometry of the simulated L1. Must agree with the runtime's
// __llvm_cache_l1_tags layout in compiler-rt/lib/profile/InstrProfilingCache.c.
static constexpr unsigned kLineLog2 = 6;     // 64-byte lines
static constexpr unsigned kNumSets  = 512;   // 32 KB direct-mapped

static cl::opt<bool> ClInstrumentLoads(
    "cache-profile-loads", cl::init(true),
    cl::desc("Instrument load instructions"), cl::Hidden);
static cl::opt<bool> ClInstrumentStores(
    "cache-profile-stores", cl::init(true),
    cl::desc("Instrument store instructions"), cl::Hidden);
static cl::opt<bool> ClInstrumentAtomics(
    "cache-profile-atomics", cl::init(false),
    cl::desc("Instrument atomic load/store/rmw accesses"), cl::Hidden);

STATISTIC(NumInstrumentedLoads,  "Number of instrumented loads");
STATISTIC(NumInstrumentedStores, "Number of instrumented stores");
STATISTIC(NumSkippedAtomic,      "Number of atomic accesses skipped");
STATISTIC(NumSkippedNosanitize,  "Number of accesses skipped (nosanitize)");

namespace {

// Externs declared into each instrumented module. Names match the runtime.
//
// The pass references L1Tags + HitsTLS directly from IR (hot path); MissesTLS
// is owned exclusively by the runtime (the IR only calls into Miss, which
// updates both the tag array and the miss counter). Keeping the miss path
// out of IR keeps the hot path's icache footprint small and lets the optimizer
// treat the cold path as a single call instruction.
struct RuntimeRefs {
  GlobalVariable *L1Tags  = nullptr;   // thread_local [kNumSets x i64]
  GlobalVariable *HitsTLS = nullptr;   // thread_local i64
  FunctionCallee  ArmThread;           // void __llvm_cache_arm_thread()
  FunctionCallee  Miss;                // void __llvm_cache_miss(i64 tag, i64 idx)

  Type *I64Ty    = nullptr;
  Type *TagArrTy = nullptr;
};

class CacheProfiler {
public:
  bool runOnModule(Module &M);

private:
  void declareRuntime(Module &M);
  bool runOnFunction(Function &F);
  bool isOwnRuntimeName(StringRef Name) const;
  bool shouldInstrument(const Instruction &I);
  void emitFastPath(IRBuilder<> &IRB, Value *Addr, Instruction *InsertBefore);

  RuntimeRefs RT;
};

bool CacheProfiler::isOwnRuntimeName(StringRef Name) const {
  // The runtime functions in InstrProfilingCache.c, plus the data symbols
  // we reference. We must never instrument these — recursion / chicken-egg.
  if (Name.starts_with("__llvm_cache_")) return true;
  return false;
}

bool CacheProfiler::shouldInstrument(const Instruction &I) {
  if (I.hasMetadata(LLVMContext::MD_nosanitize)) {
    ++NumSkippedNosanitize;
    return false;
  }
  if (auto *LI = dyn_cast<LoadInst>(&I)) {
    if (LI->isAtomic()) {
      if (!ClInstrumentAtomics) { ++NumSkippedAtomic; return false; }
    }
    return ClInstrumentLoads;
  }
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    if (SI->isAtomic()) {
      if (!ClInstrumentAtomics) { ++NumSkippedAtomic; return false; }
    }
    return ClInstrumentStores;
  }
  return false;
}

void CacheProfiler::declareRuntime(Module &M) {
  LLVMContext &Ctx = M.getContext();
  RT.I64Ty    = Type::getInt64Ty(Ctx);
  RT.TagArrTy = ArrayType::get(RT.I64Ty, kNumSets);

  auto declareTLSGlobal = [&](StringRef Name, Type *Ty) -> GlobalVariable * {
    if (GlobalVariable *G = M.getGlobalVariable(Name, /*AllowInternal=*/true))
      return G;
    auto *G = new GlobalVariable(
        M, Ty, /*isConstant=*/false, GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr, Name,
        /*InsertBefore=*/nullptr,
        GlobalValue::GeneralDynamicTLSModel);
    return G;
  };

  RT.L1Tags  = declareTLSGlobal("__llvm_cache_l1_tags",  RT.TagArrTy);
  RT.HitsTLS = declareTLSGlobal("__llvm_cache_hits_tls", RT.I64Ty);

  RT.ArmThread = M.getOrInsertFunction(
      "__llvm_cache_arm_thread",
      FunctionType::get(Type::getVoidTy(Ctx), {}, /*isVarArg=*/false));

  // Marked nounwind/cold so the optimizer treats the miss block as cold and
  // straightens the hit path. The runtime is what actually installs the new
  // tag and bumps the miss counter.
  AttributeList MissAttrs;
  MissAttrs = MissAttrs.addFnAttribute(Ctx, Attribute::NoUnwind);
  MissAttrs = MissAttrs.addFnAttribute(Ctx, Attribute::Cold);
  RT.Miss = M.getOrInsertFunction(
      "__llvm_cache_miss", MissAttrs,
      Type::getVoidTy(Ctx), RT.I64Ty, RT.I64Ty);
}

// Inserts before `InsertBefore`, in its current basic block:
//
//   tag  = (i64)addr >> kLineLog2
//   idx  = tag & (kNumSets-1)
//   slot = __llvm_cache_l1_tags[idx]
//   if (slot == tag) [likely]
//       ++__llvm_cache_hits_tls;     // 3 IR insts on the hot path
//   else
//       __llvm_cache_miss(tag, idx); // single cold call; runtime updates
//                                    // the tag slot + miss counter.
//   <original load/store at InsertBefore>
void CacheProfiler::emitFastPath(IRBuilder<> &IRB, Value *Addr,
                                 Instruction *InsertBefore) {
  Value *AddrInt = IRB.CreatePtrToInt(Addr, RT.I64Ty, "cp.addr");
  Value *Tag     = IRB.CreateLShr(AddrInt, IRB.getInt64(kLineLog2), "cp.tag");
  Value *Idx     = IRB.CreateAnd(Tag, IRB.getInt64(kNumSets - 1), "cp.idx");

  Value *SlotPtr = IRB.CreateInBoundsGEP(
      RT.TagArrTy, RT.L1Tags,
      {IRB.getInt64(0), Idx}, "cp.slotptr");
  Value *Slot = IRB.CreateLoad(RT.I64Ty, SlotPtr, "cp.slot");
  Value *Hit  = IRB.CreateICmpEQ(Slot, Tag, "cp.hit");

  // Strongly-weighted branch — hit is the common case for cache-friendly
  // code. The branch_weights metadata is what tells codegen to lay out the
  // hit path as fall-through and the call as the cold off-path.
  MDBuilder MDB(IRB.getContext());
  MDNode *Weights = MDB.createBranchWeights(/*TrueWeight=*/1000,
                                            /*FalseWeight=*/1);

  Instruction *ThenTerm = nullptr;
  Instruction *ElseTerm = nullptr;
  SplitBlockAndInsertIfThenElse(Hit, InsertBefore, &ThenTerm, &ElseTerm,
                                Weights);

  // Hit: ++__llvm_cache_hits_tls (load/add/store on TLS i64; no atomic — TLS).
  {
    IRBuilder<> B(ThenTerm);
    Value *Cur = B.CreateLoad(RT.I64Ty, RT.HitsTLS, "cp.h");
    B.CreateStore(B.CreateAdd(Cur, B.getInt64(1), "cp.h.inc"), RT.HitsTLS);
  }
  // Miss: single call into compiler-rt. No inlined tag store or counter
  // bump — those live in the runtime.
  {
    IRBuilder<> B(ElseTerm);
    CallInst *CI = B.CreateCall(RT.Miss, {Tag, Idx});
    CI->setTailCall(false);  // we still need the original load/store after
  }
}

bool CacheProfiler::runOnFunction(Function &F) {
  if (F.isDeclaration() || F.empty()) return false;
  if (isOwnRuntimeName(F.getName())) return false;
  if (F.hasFnAttribute(Attribute::Naked)) return false;
  if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation))
    return false;

  SmallVector<std::pair<Instruction *, Value *>, 16> Targets;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (!shouldInstrument(I)) continue;
      Value *Addr = nullptr;
      if (auto *LI = dyn_cast<LoadInst>(&I))
        Addr = LI->getPointerOperand();
      else if (auto *SI = dyn_cast<StoreInst>(&I))
        Addr = SI->getPointerOperand();
      if (!Addr || Addr->isSwiftError()) continue;
      Targets.emplace_back(&I, Addr);
    }
  }
  if (Targets.empty()) return false;

  // Arm the thread-exit flusher once per function entry. Idempotent; the
  // runtime fast-paths the already-armed case with one pthread_getspecific.
  {
    IRBuilder<> IRB(&*F.getEntryBlock().getFirstInsertionPt());
    IRB.CreateCall(RT.ArmThread);
  }

  for (auto &T : Targets) {
    IRBuilder<> IRB(T.first);
    emitFastPath(IRB, T.second, T.first);
    if (isa<LoadInst>(T.first))
      ++NumInstrumentedLoads;
    else
      ++NumInstrumentedStores;
  }
  return true;
}

bool CacheProfiler::runOnModule(Module &M) {
  declareRuntime(M);
  bool Changed = false;
  for (Function &F : M)
    Changed |= runOnFunction(F);
  return Changed;
}

} // namespace

PreservedAnalyses CacheProfilingPass::run(Module &M,
                                          ModuleAnalysisManager &AM) {
  CacheProfiler P;
  if (P.runOnModule(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
