//=== ScalarEvolutionExpanderTest.cpp - ScalarEvolutionExpander unit tests ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

namespace llvm {

using namespace PatternMatch;

// We use this fixture to ensure that we clean up ScalarEvolution before
// deleting the PassManager.
class ScalarEvolutionExpanderTest : public testing::Test {
protected:
  LLVMContext Context;
  Module M;
  TargetLibraryInfoImpl TLII;
  TargetLibraryInfo TLI;

  std::unique_ptr<AssumptionCache> AC;
  std::unique_ptr<DominatorTree> DT;
  std::unique_ptr<LoopInfo> LI;

  ScalarEvolutionExpanderTest()
      : M("", Context), TLII(M.getTargetTriple()), TLI(TLII) {}

  ScalarEvolution buildSE(Function &F) {
    AC.reset(new AssumptionCache(F));
    DT.reset(new DominatorTree(F));
    LI.reset(new LoopInfo(*DT));
    return ScalarEvolution(F, TLI, *AC, *DT, *LI);
  }

  void runWithSE(
      Module &M, StringRef FuncName,
      function_ref<void(Function &F, LoopInfo &LI, ScalarEvolution &SE)> Test) {
    auto *F = M.getFunction(FuncName);
    ASSERT_NE(F, nullptr) << "Could not find " << FuncName;
    ScalarEvolution SE = buildSE(*F);
    Test(*F, *LI, SE);
  }
};

static Instruction &GetInstByName(Function &F, StringRef Name) {
  for (auto &I : instructions(F))
    if (I.getName() == Name)
      return I;
  llvm_unreachable("Could not find instructions!");
}

TEST_F(ScalarEvolutionExpanderTest, ExpandPtrTypeSCEV) {
  // It is to test the fix for PR30213. It exercises the branch in scev
  // expansion when the value in ValueOffsetPair is a ptr and the offset
  // is not divisible by the elem type size of value.
  auto *I8Ty = Type::getInt8Ty(Context);
  auto *PtrTy = PointerType::get(Context, 0);
  auto *I32Ty = Type::getInt32Ty(Context);
  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(Context), std::vector<Type *>(), false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "f", M);
  BasicBlock *EntryBB = BasicBlock::Create(Context, "entry", F);
  BasicBlock *LoopBB = BasicBlock::Create(Context, "loop", F);
  BasicBlock *ExitBB = BasicBlock::Create(Context, "exit", F);
  UncondBrInst::Create(LoopBB, EntryBB);
  ReturnInst::Create(Context, nullptr, ExitBB);

  // loop:                            ; preds = %loop, %entry
  //   %alloca = alloca i32
  //   %gep0 = getelementptr i32, ptr %alloca, i32 1
  //   %gep1 = getelementptr i8, ptr %gep0, i32 1
  //   %gep2 = getelementptr i8, ptr undef, i32 1
  //   %cmp = icmp ult ptr undef, %gep0
  //   %select = select i1 %cmp, ptr %gep1, ptr %gep2
  //   br i1 undef, label %loop, label %exit

  const DataLayout &DL = F->getDataLayout();
  CondBrInst *Br = CondBrInst::Create(
      PoisonValue::get(Type::getInt1Ty(Context)), LoopBB, ExitBB, LoopBB);
  AllocaInst *Alloca = new AllocaInst(I32Ty, DL.getAllocaAddrSpace(), "alloca",
                                      Br->getIterator());
  ConstantInt *Ci32 = ConstantInt::get(Context, APInt(32, 1));
  UndefValue *UndefPtr = UndefValue::get(PtrTy);
  GetElementPtrInst *Gep0 =
      GetElementPtrInst::Create(I32Ty, Alloca, Ci32, "gep0", Br->getIterator());
  GetElementPtrInst *Gep1 =
      GetElementPtrInst::Create(I8Ty, Gep0, Ci32, "gep1", Br->getIterator());
  GetElementPtrInst *Gep2 = GetElementPtrInst::Create(
      I8Ty, UndefPtr, Ci32, "gep2", Br->getIterator());
  CmpInst *Cmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_ULT,
                                 UndefPtr, Gep0, "cmp", Br->getIterator());
  SelectInst *Select =
      SelectInst::Create(Cmp, Gep1, Gep2, "select", Br->getIterator());

  ScalarEvolution SE = buildSE(*F);
  const SCEV *S = SE.getSCEV(Select);
  EXPECT_TRUE(isa<SCEVUnknown>(S));
}

// Make sure that SCEV doesn't introduce illegal ptrtoint/inttoptr instructions
TEST_F(ScalarEvolutionExpanderTest, SCEVZeroExtendExprNonIntegral) {
  /*
   * Create the following code:
   * func(ptr addrspace(10) %arg)
   * top:
   *  br label %L.ph
   * L.ph:
   *  %gepbase = getelementptr ptr addrspace(10) %arg, i64 1
   *  br label %L
   * L:
   *  %phi = phi i64 [i64 0, %L.ph], [ %add, %L2 ]
   *  %add = add i64 %phi2, 1
   *  br i1 undef, label %post, label %L2
   * post:
   *  #= %gep = getelementptr ptr addrspace(10) %gepbase, i64 %add =#
   *  ret void
   *
   * We will create the appropriate SCEV expression for %gep and expand it,
   * then check that no inttoptr/ptrtoint instructions got inserted.
   */

  // Create a module with non-integral pointers in it's datalayout
  Module NIM("nonintegral", Context);
  std::string DataLayout = M.getDataLayoutStr();
  if (!DataLayout.empty())
    DataLayout += "-";
  DataLayout += "ni:10";
  NIM.setDataLayout(DataLayout);

  Type *T_int1 = Type::getInt1Ty(Context);
  Type *T_int64 = Type::getInt64Ty(Context);
  Type *T_pint64 = PointerType::get(Context, 10);

  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(Context), {T_pint64}, false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "foo", NIM);

  Argument *Arg = &*F->arg_begin();

  BasicBlock *Top = BasicBlock::Create(Context, "top", F);
  BasicBlock *LPh = BasicBlock::Create(Context, "L.ph", F);
  BasicBlock *L = BasicBlock::Create(Context, "L", F);
  BasicBlock *Post = BasicBlock::Create(Context, "post", F);

  IRBuilder<> Builder(Top);
  Builder.CreateBr(LPh);

  Builder.SetInsertPoint(LPh);
  Value *GepBase =
      Builder.CreateGEP(T_int64, Arg, ConstantInt::get(T_int64, 1));
  Builder.CreateBr(L);

  Builder.SetInsertPoint(L);
  PHINode *Phi = Builder.CreatePHI(T_int64, 2);
  Value *Add = Builder.CreateAdd(Phi, ConstantInt::get(T_int64, 1), "add");
  Builder.CreateCondBr(PoisonValue::get(T_int1), L, Post);
  Phi->addIncoming(ConstantInt::get(T_int64, 0), LPh);
  Phi->addIncoming(Add, L);

  Builder.SetInsertPoint(Post);
  Instruction *Ret = Builder.CreateRetVoid();

  ScalarEvolution SE = buildSE(*F);
  const SCEV *AddRec =
      SE.getAddRecExpr(SE.getUnknown(GepBase), SE.getConstant(T_int64, 1),
                       LI->getLoopFor(L), SCEV::FlagNUW);

  SCEVExpander Exp(SE, "expander");
  Exp.disableCanonicalMode();
  Exp.expandCodeFor(AddRec, T_pint64, Ret);

  // Make sure none of the instructions inserted were inttoptr/ptrtoint.
  // The verifier will check this.
  EXPECT_FALSE(verifyFunction(*F, &errs()));
}

// Check that we can correctly identify the points at which the SCEV of the
// AddRec can be expanded.
TEST_F(ScalarEvolutionExpanderTest, SCEVExpanderIsSafeToExpandAt) {
  /*
   * Create the following code:
   * func(ptr addrspace(10) %arg)
   * top:
   *  br label %L.ph
   * L.ph:
   *  br label %L
   * L:
   *  %phi = phi i64 [i64 0, %L.ph], [ %add, %L2 ]
   *  %add = add i64 %phi2, 1
   *  %cond = icmp slt i64 %add, 1000; then becomes 2000.
   *  br i1 %cond, label %post, label %L2
   * post:
   *  ret void
   *
   */

  // Create a module with non-integral pointers in it's datalayout
  Module NIM("nonintegral", Context);
  std::string DataLayout = M.getDataLayoutStr();
  if (!DataLayout.empty())
    DataLayout += "-";
  DataLayout += "ni:10";
  NIM.setDataLayout(DataLayout);

  Type *T_int64 = Type::getInt64Ty(Context);
  Type *T_pint64 = PointerType::get(Context, 10);

  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(Context), {T_pint64}, false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "foo", NIM);

  BasicBlock *Top = BasicBlock::Create(Context, "top", F);
  BasicBlock *LPh = BasicBlock::Create(Context, "L.ph", F);
  BasicBlock *L = BasicBlock::Create(Context, "L", F);
  BasicBlock *Post = BasicBlock::Create(Context, "post", F);

  IRBuilder<> Builder(Top);
  Builder.CreateBr(LPh);

  Builder.SetInsertPoint(LPh);
  Builder.CreateBr(L);

  Builder.SetInsertPoint(L);
  PHINode *Phi = Builder.CreatePHI(T_int64, 2);
  auto *Add = cast<Instruction>(
      Builder.CreateAdd(Phi, ConstantInt::get(T_int64, 1), "add"));
  auto *Limit = ConstantInt::get(T_int64, 1000);
  auto *Cond = cast<Instruction>(
      Builder.CreateICmp(ICmpInst::ICMP_SLT, Add, Limit, "cond"));
  Builder.CreateCondBr(Cond, L, Post);
  Phi->addIncoming(ConstantInt::get(T_int64, 0), LPh);
  Phi->addIncoming(Add, L);

  Builder.SetInsertPoint(Post);
  Instruction *Ret = Builder.CreateRetVoid();

  ScalarEvolution SE = buildSE(*F);
  SCEVExpander Exp(SE, "expander");
  const SCEV *S = SE.getSCEV(Phi);
  EXPECT_TRUE(isa<SCEVAddRecExpr>(S));
  const SCEVAddRecExpr *AR = cast<SCEVAddRecExpr>(S);
  EXPECT_TRUE(AR->isAffine());
  EXPECT_FALSE(Exp.isSafeToExpandAt(AR, Top->getTerminator()));
  EXPECT_FALSE(Exp.isSafeToExpandAt(AR, LPh->getTerminator()));
  EXPECT_TRUE(Exp.isSafeToExpandAt(AR, L->getTerminator()));
  EXPECT_TRUE(Exp.isSafeToExpandAt(AR, Post->getTerminator()));

  EXPECT_TRUE(LI->getLoopFor(L)->isLCSSAForm(*DT));
  Exp.expandCodeFor(SE.getSCEV(Add), nullptr, Ret);
  EXPECT_TRUE(LI->getLoopFor(L)->isLCSSAForm(*DT));
}

// Check that SCEV expander does not use the nuw instruction
// for expansion.
TEST_F(ScalarEvolutionExpanderTest, SCEVExpanderNUW) {
  /*
   * Create the following code:
   * func(i64 %a)
   * entry:
   *   br false, label %exit, label %body
   * body:
   *  %s1 = add i64 %a, -1
   *  br label %exit
   * exit:
   *  %s = add nuw i64 %a, -1
   *  ret %s
   */

  // Create a module.
  Module M("SCEVExpanderNUW", Context);

  Type *T_int64 = Type::getInt64Ty(Context);

  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(Context), {T_int64}, false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "func", M);
  Argument *Arg = &*F->arg_begin();
  ConstantInt *C = ConstantInt::get(Context, APInt(64, -1));

  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  BasicBlock *Body = BasicBlock::Create(Context, "body", F);
  BasicBlock *Exit = BasicBlock::Create(Context, "exit", F);

  IRBuilder<> Builder(Entry);
  ConstantInt *Cond = ConstantInt::get(Context, APInt(1, 0));
  Builder.CreateCondBr(Cond, Exit, Body);

  Builder.SetInsertPoint(Body);
  auto *S1 = cast<Instruction>(Builder.CreateAdd(Arg, C, "add"));
  Builder.CreateBr(Exit);

  Builder.SetInsertPoint(Exit);
  auto *S2 = cast<Instruction>(Builder.CreateAdd(Arg, C, "add"));
  S2->setHasNoUnsignedWrap(true);
  auto *R = cast<Instruction>(Builder.CreateRetVoid());

  ScalarEvolution SE = buildSE(*F);
  const SCEV *S = SE.getSCEV(S1);
  EXPECT_TRUE(isa<SCEVAddExpr>(S));
  SCEVExpander Exp(SE, "expander");
  auto *I = cast<Instruction>(Exp.expandCodeFor(S, nullptr, R));
  EXPECT_FALSE(I->hasNoUnsignedWrap());
}

// Check that SCEV expander does not use the nsw instruction
// for expansion.
TEST_F(ScalarEvolutionExpanderTest, SCEVExpanderNSW) {
  /*
   * Create the following code:
   * func(i64 %a)
   * entry:
   *   br false, label %exit, label %body
   * body:
   *  %s1 = add i64 %a, -1
   *  br label %exit
   * exit:
   *  %s = add nsw i64 %a, -1
   *  ret %s
   */

  // Create a module.
  Module M("SCEVExpanderNSW", Context);

  Type *T_int64 = Type::getInt64Ty(Context);

  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(Context), {T_int64}, false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "func", M);
  Argument *Arg = &*F->arg_begin();
  ConstantInt *C = ConstantInt::get(Context, APInt(64, -1));

  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  BasicBlock *Body = BasicBlock::Create(Context, "body", F);
  BasicBlock *Exit = BasicBlock::Create(Context, "exit", F);

  IRBuilder<> Builder(Entry);
  ConstantInt *Cond = ConstantInt::get(Context, APInt(1, 0));
  Builder.CreateCondBr(Cond, Exit, Body);

  Builder.SetInsertPoint(Body);
  auto *S1 = cast<Instruction>(Builder.CreateAdd(Arg, C, "add"));
  Builder.CreateBr(Exit);

  Builder.SetInsertPoint(Exit);
  auto *S2 = cast<Instruction>(Builder.CreateAdd(Arg, C, "add"));
  S2->setHasNoSignedWrap(true);
  auto *R = cast<Instruction>(Builder.CreateRetVoid());

  ScalarEvolution SE = buildSE(*F);
  const SCEV *S = SE.getSCEV(S1);
  EXPECT_TRUE(isa<SCEVAddExpr>(S));
  SCEVExpander Exp(SE, "expander");
  auto *I = cast<Instruction>(Exp.expandCodeFor(S, nullptr, R));
  EXPECT_FALSE(I->hasNoSignedWrap());
}

// Check that SCEV does not save the SCEV -> V
// mapping of SCEV differ from V in NUW flag.
TEST_F(ScalarEvolutionExpanderTest, SCEVCacheNUW) {
  /*
   * Create the following code:
   * func(i64 %a)
   * entry:
   *  %s1 = add i64 %a, -1
   *  %s2 = add nuw i64 %a, -1
   *  br label %exit
   * exit:
   *  ret %s
   */

  // Create a module.
  Module M("SCEVCacheNUW", Context);

  Type *T_int64 = Type::getInt64Ty(Context);

  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(Context), {T_int64}, false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "func", M);
  Argument *Arg = &*F->arg_begin();
  ConstantInt *C = ConstantInt::get(Context, APInt(64, -1));

  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  BasicBlock *Exit = BasicBlock::Create(Context, "exit", F);

  IRBuilder<> Builder(Entry);
  auto *S1 = cast<Instruction>(Builder.CreateAdd(Arg, C, "add"));
  auto *S2 = cast<Instruction>(Builder.CreateAdd(Arg, C, "add"));
  S2->setHasNoUnsignedWrap(true);
  Builder.CreateBr(Exit);

  Builder.SetInsertPoint(Exit);
  auto *R = cast<Instruction>(Builder.CreateRetVoid());

  ScalarEvolution SE = buildSE(*F);
  // Get S2 first to move it to cache.
  const SCEV *SC2 = SE.getSCEV(S2);
  EXPECT_TRUE(isa<SCEVAddExpr>(SC2));
  // Now get S1.
  const SCEV *SC1 = SE.getSCEV(S1);
  EXPECT_TRUE(isa<SCEVAddExpr>(SC1));
  // Expand for S1, it should use S1 not S2 in spite S2
  // first in the cache.
  SCEVExpander Exp(SE, "expander");
  auto *I = cast<Instruction>(Exp.expandCodeFor(SC1, nullptr, R));
  EXPECT_FALSE(I->hasNoUnsignedWrap());
}

// Check that SCEV does not save the SCEV -> V
// mapping of SCEV differ from V in NSW flag.
TEST_F(ScalarEvolutionExpanderTest, SCEVCacheNSW) {
  /*
   * Create the following code:
   * func(i64 %a)
   * entry:
   *  %s1 = add i64 %a, -1
   *  %s2 = add nsw i64 %a, -1
   *  br label %exit
   * exit:
   *  ret %s
   */

  // Create a module.
  Module M("SCEVCacheNUW", Context);

  Type *T_int64 = Type::getInt64Ty(Context);

  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(Context), {T_int64}, false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "func", M);
  Argument *Arg = &*F->arg_begin();
  ConstantInt *C = ConstantInt::get(Context, APInt(64, -1));

  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  BasicBlock *Exit = BasicBlock::Create(Context, "exit", F);

  IRBuilder<> Builder(Entry);
  auto *S1 = cast<Instruction>(Builder.CreateAdd(Arg, C, "add"));
  auto *S2 = cast<Instruction>(Builder.CreateAdd(Arg, C, "add"));
  S2->setHasNoSignedWrap(true);
  Builder.CreateBr(Exit);

  Builder.SetInsertPoint(Exit);
  auto *R = cast<Instruction>(Builder.CreateRetVoid());

  ScalarEvolution SE = buildSE(*F);
  // Get S2 first to move it to cache.
  const SCEV *SC2 = SE.getSCEV(S2);
  EXPECT_TRUE(isa<SCEVAddExpr>(SC2));
  // Now get S1.
  const SCEV *SC1 = SE.getSCEV(S1);
  EXPECT_TRUE(isa<SCEVAddExpr>(SC1));
  // Expand for S1, it should use S1 not S2 in spite S2
  // first in the cache.
  SCEVExpander Exp(SE, "expander");
  auto *I = cast<Instruction>(Exp.expandCodeFor(SC1, nullptr, R));
  EXPECT_FALSE(I->hasNoSignedWrap());
}

TEST_F(ScalarEvolutionExpanderTest, SCEVExpandInsertCanonicalIV) {
  LLVMContext C;
  SMDiagnostic Err;

  // Expand the addrec produced by GetAddRec into a loop without a canonical IV.
  // SCEVExpander will insert one.
  auto TestNoCanonicalIV =
      [&](std::function<const SCEV *(ScalarEvolution & SE, Loop * L)>
              GetAddRec) {
        std::unique_ptr<Module> M = parseAssemblyString(
            "define i32 @test(i32 %limit) { "
            "entry: "
            "  br label %loop "
            "loop: "
            "  %i = phi i32 [ 1, %entry ], [ %i.inc, %loop ] "
            "  %i.inc = add nsw i32 %i, 1 "
            "  %cont = icmp slt i32 %i.inc, %limit "
            "  br i1 %cont, label %loop, label %exit "
            "exit: "
            "  ret i32 %i.inc "
            "}",
            Err, C);

        assert(M && "Could not parse module?");
        assert(!verifyModule(*M) && "Must have been well formed!");

        runWithSE(
            *M, "test", [&](Function &F, LoopInfo &LI, ScalarEvolution &SE) {
              auto &I = GetInstByName(F, "i");
              auto *Loop = LI.getLoopFor(I.getParent());
              EXPECT_FALSE(Loop->getCanonicalInductionVariable());

              auto *AR = GetAddRec(SE, Loop);
              unsigned ExpectedCanonicalIVWidth =
                  SE.getTypeSizeInBits(AR->getType());

              SCEVExpander Exp(SE, "expander");
              auto *InsertAt = I.getNextNode();
              Exp.expandCodeFor(AR, nullptr, InsertAt);
              PHINode *CanonicalIV = Loop->getCanonicalInductionVariable();
              unsigned CanonicalIVBitWidth =
                  cast<IntegerType>(CanonicalIV->getType())->getBitWidth();
              EXPECT_EQ(CanonicalIVBitWidth, ExpectedCanonicalIVWidth);
            });
      };

  // Expand the addrec produced by GetAddRec into a loop with a canonical IV
  // which is narrower than addrec type.
  // SCEVExpander will insert a canonical IV of a wider type to expand the
  // addrec.
  auto TestNarrowCanonicalIV = [&](std::function<const SCEV *(
                                       ScalarEvolution & SE, Loop * L)>
                                       GetAddRec) {
    std::unique_ptr<Module> M = parseAssemblyString(
        "define i32 @test(i32 %limit) { "
        "entry: "
        "  br label %loop "
        "loop: "
        "  %i = phi i32 [ 1, %entry ], [ %i.inc, %loop ] "
        "  %canonical.iv = phi i8 [ 0, %entry ], [ %canonical.iv.inc, %loop ] "
        "  %i.inc = add nsw i32 %i, 1 "
        "  %canonical.iv.inc = add i8 %canonical.iv, 1 "
        "  %cont = icmp slt i32 %i.inc, %limit "
        "  br i1 %cont, label %loop, label %exit "
        "exit: "
        "  ret i32 %i.inc "
        "}",
        Err, C);

    assert(M && "Could not parse module?");
    assert(!verifyModule(*M) && "Must have been well formed!");

    runWithSE(*M, "test", [&](Function &F, LoopInfo &LI, ScalarEvolution &SE) {
      auto &I = GetInstByName(F, "i");

      auto *LoopHeaderBB = I.getParent();
      auto *Loop = LI.getLoopFor(LoopHeaderBB);
      PHINode *CanonicalIV = Loop->getCanonicalInductionVariable();
      EXPECT_EQ(CanonicalIV, &GetInstByName(F, "canonical.iv"));

      auto *AR = GetAddRec(SE, Loop);

      unsigned ExpectedCanonicalIVWidth = SE.getTypeSizeInBits(AR->getType());
      unsigned CanonicalIVBitWidth =
          cast<IntegerType>(CanonicalIV->getType())->getBitWidth();
      EXPECT_LT(CanonicalIVBitWidth, ExpectedCanonicalIVWidth);

      SCEVExpander Exp(SE, "expander");
      auto *InsertAt = I.getNextNode();
      Exp.expandCodeFor(AR, nullptr, InsertAt);

      // Loop over all of the PHI nodes, looking for the new canonical indvar.
      PHINode *NewCanonicalIV = nullptr;
      for (BasicBlock::iterator i = LoopHeaderBB->begin(); isa<PHINode>(i);
           ++i) {
        PHINode *PN = cast<PHINode>(i);
        if (PN == &I || PN == CanonicalIV)
          continue;
        // We expect that the only PHI added is the new canonical IV
        EXPECT_FALSE(NewCanonicalIV);
        NewCanonicalIV = PN;
      }

      // Check that NewCanonicalIV is a canonical IV, i.e {0,+,1}
      BasicBlock *Incoming = nullptr, *Backedge = nullptr;
      EXPECT_TRUE(Loop->getIncomingAndBackEdge(Incoming, Backedge));
      auto *Start = NewCanonicalIV->getIncomingValueForBlock(Incoming);
      EXPECT_TRUE(isa<ConstantInt>(Start));
      EXPECT_TRUE(dyn_cast<ConstantInt>(Start)->isZero());
      auto *Next = NewCanonicalIV->getIncomingValueForBlock(Backedge);
      EXPECT_TRUE(isa<BinaryOperator>(Next));
      auto *NextBinOp = dyn_cast<BinaryOperator>(Next);
      EXPECT_EQ(NextBinOp->getOpcode(), Instruction::Add);
      EXPECT_EQ(NextBinOp->getOperand(0), NewCanonicalIV);
      auto *Step = NextBinOp->getOperand(1);
      EXPECT_TRUE(isa<ConstantInt>(Step));
      EXPECT_TRUE(dyn_cast<ConstantInt>(Step)->isOne());

      unsigned NewCanonicalIVBitWidth =
          cast<IntegerType>(NewCanonicalIV->getType())->getBitWidth();
      EXPECT_EQ(NewCanonicalIVBitWidth, ExpectedCanonicalIVWidth);
    });
  };

  // Expand the addrec produced by GetAddRec into a loop with a canonical IV
  // of addrec width.
  // To expand the addrec SCEVExpander should use the existing canonical IV.
  auto TestMatchingCanonicalIV =
      [&](std::function<const SCEV *(ScalarEvolution & SE, Loop * L)> GetAddRec,
          unsigned ARBitWidth) {
        auto ARBitWidthTypeStr = "i" + std::to_string(ARBitWidth);
        std::unique_ptr<Module> M = parseAssemblyString(
            "define i32 @test(i32 %limit) { "
            "entry: "
            "  br label %loop "
            "loop: "
            "  %i = phi i32 [ 1, %entry ], [ %i.inc, %loop ] "
            "  %canonical.iv = phi " +
                ARBitWidthTypeStr +
                " [ 0, %entry ], [ %canonical.iv.inc, %loop ] "
                "  %i.inc = add nsw i32 %i, 1 "
                "  %canonical.iv.inc = add " +
                ARBitWidthTypeStr +
                " %canonical.iv, 1 "
                "  %cont = icmp slt i32 %i.inc, %limit "
                "  br i1 %cont, label %loop, label %exit "
                "exit: "
                "  ret i32 %i.inc "
                "}",
            Err, C);

        assert(M && "Could not parse module?");
        assert(!verifyModule(*M) && "Must have been well formed!");

        runWithSE(
            *M, "test", [&](Function &F, LoopInfo &LI, ScalarEvolution &SE) {
              auto &I = GetInstByName(F, "i");
              auto &CanonicalIV = GetInstByName(F, "canonical.iv");

              auto *LoopHeaderBB = I.getParent();
              auto *Loop = LI.getLoopFor(LoopHeaderBB);
              EXPECT_EQ(&CanonicalIV, Loop->getCanonicalInductionVariable());
              unsigned CanonicalIVBitWidth =
                  cast<IntegerType>(CanonicalIV.getType())->getBitWidth();

              auto *AR = GetAddRec(SE, Loop);
              EXPECT_EQ(ARBitWidth, SE.getTypeSizeInBits(AR->getType()));
              EXPECT_EQ(CanonicalIVBitWidth, ARBitWidth);

              SCEVExpander Exp(SE, "expander");
              auto *InsertAt = I.getNextNode();
              Exp.expandCodeFor(AR, nullptr, InsertAt);

              // Loop over all of the PHI nodes, looking if a new canonical
              // indvar was introduced.
              PHINode *NewCanonicalIV = nullptr;
              for (BasicBlock::iterator i = LoopHeaderBB->begin();
                   isa<PHINode>(i); ++i) {
                PHINode *PN = cast<PHINode>(i);
                if (PN == &I || PN == &CanonicalIV)
                  continue;
                NewCanonicalIV = PN;
              }
              EXPECT_FALSE(NewCanonicalIV);
            });
      };

  unsigned ARBitWidth = 16;
  Type *ARType = IntegerType::get(C, ARBitWidth);

  // Expand {5,+,1}
  auto GetAR2 = [&](ScalarEvolution &SE, Loop *L) -> const SCEV * {
    return SE.getAddRecExpr(SE.getConstant(APInt(ARBitWidth, 5)),
                            SE.getOne(ARType), L, SCEV::FlagNone);
  };
  TestNoCanonicalIV(GetAR2);
  TestNarrowCanonicalIV(GetAR2);
  TestMatchingCanonicalIV(GetAR2, ARBitWidth);
}

TEST_F(ScalarEvolutionExpanderTest, SCEVExpanderShlNSW) {

  auto checkOneCase = [this](std::string &&str) {
    LLVMContext C;
    SMDiagnostic Err;
    std::unique_ptr<Module> M = parseAssemblyString(str, Err, C);

    assert(M && "Could not parse module?");
    assert(!verifyModule(*M) && "Must have been well formed!");

    Function *F = M->getFunction("f");
    ASSERT_NE(F, nullptr) << "Could not find function 'f'";

    BasicBlock &Entry = F->getEntryBlock();
    LoadInst *Load = cast<LoadInst>(&Entry.front());
    BinaryOperator *And = cast<BinaryOperator>(*Load->user_begin());

    ScalarEvolution SE = buildSE(*F);
    const SCEV *AndSCEV = SE.getSCEV(And);
    EXPECT_TRUE(isa<SCEVMulExpr>(AndSCEV));
    EXPECT_TRUE(cast<SCEVMulExpr>(AndSCEV)->hasNoSignedWrap());

    SCEVExpander Exp(SE, "expander");
    auto *I = cast<Instruction>(Exp.expandCodeFor(AndSCEV, nullptr, And));
    EXPECT_EQ(I->getOpcode(), Instruction::And);
  };

  checkOneCase("define void @f(ptr %arrayidx) { "
               "  %1 = load i16, ptr %arrayidx "
               "  %2 = and i16 %1, -32768 "
               "  ret void "
               "} ");

  checkOneCase("define void @f(ptr %arrayidx) { "
               "  %1 = load i8, ptr %arrayidx "
               "  %2 = and i8 %1, -128 "
               "  ret void "
               "} ");
}

// Test expansion of nested addrecs in CanonicalMode.
// Expanding nested addrecs in canonical mode requiers a canonical IV of a
// type wider than the type of the addrec itself. Currently, SCEVExpander
// just falls back to literal mode for nested addrecs.
TEST_F(ScalarEvolutionExpanderTest, SCEVExpandNonAffineAddRec) {
  LLVMContext C;
  SMDiagnostic Err;

  // Expand the addrec produced by GetAddRec into a loop without a canonical IV.
  auto TestNoCanonicalIV =
      [&](std::function<const SCEVAddRecExpr *(ScalarEvolution & SE, Loop * L)>
              GetAddRec) {
        std::unique_ptr<Module> M = parseAssemblyString(
            "define i32 @test(i32 %limit) { "
            "entry: "
            "  br label %loop "
            "loop: "
            "  %i = phi i32 [ 1, %entry ], [ %i.inc, %loop ] "
            "  %i.inc = add nsw i32 %i, 1 "
            "  %cont = icmp slt i32 %i.inc, %limit "
            "  br i1 %cont, label %loop, label %exit "
            "exit: "
            "  ret i32 %i.inc "
            "}",
            Err, C);

        assert(M && "Could not parse module?");
        assert(!verifyModule(*M) && "Must have been well formed!");

        runWithSE(*M, "test",
                  [&](Function &F, LoopInfo &LI, ScalarEvolution &SE) {
                    auto &I = GetInstByName(F, "i");
                    auto *Loop = LI.getLoopFor(I.getParent());
                    EXPECT_FALSE(Loop->getCanonicalInductionVariable());

                    auto *AR = GetAddRec(SE, Loop);
                    EXPECT_FALSE(AR->isAffine());

                    SCEVExpander Exp(SE, "expander");
                    auto *InsertAt = I.getNextNode();
                    Value *V = Exp.expandCodeFor(AR, nullptr, InsertAt);
                    const SCEV *ExpandedAR = SE.getSCEV(V);
                    // Check that the expansion happened literally.
                    EXPECT_EQ(AR, ExpandedAR);
                  });
      };

  // Expand the addrec produced by GetAddRec into a loop with a canonical IV
  // which is narrower than addrec type.
  auto TestNarrowCanonicalIV = [&](std::function<const SCEVAddRecExpr *(
                                       ScalarEvolution & SE, Loop * L)>
                                       GetAddRec) {
    std::unique_ptr<Module> M = parseAssemblyString(
        "define i32 @test(i32 %limit) { "
        "entry: "
        "  br label %loop "
        "loop: "
        "  %i = phi i32 [ 1, %entry ], [ %i.inc, %loop ] "
        "  %canonical.iv = phi i8 [ 0, %entry ], [ %canonical.iv.inc, %loop ] "
        "  %i.inc = add nsw i32 %i, 1 "
        "  %canonical.iv.inc = add i8 %canonical.iv, 1 "
        "  %cont = icmp slt i32 %i.inc, %limit "
        "  br i1 %cont, label %loop, label %exit "
        "exit: "
        "  ret i32 %i.inc "
        "}",
        Err, C);

    assert(M && "Could not parse module?");
    assert(!verifyModule(*M) && "Must have been well formed!");

    runWithSE(*M, "test", [&](Function &F, LoopInfo &LI, ScalarEvolution &SE) {
      auto &I = GetInstByName(F, "i");

      auto *LoopHeaderBB = I.getParent();
      auto *Loop = LI.getLoopFor(LoopHeaderBB);
      PHINode *CanonicalIV = Loop->getCanonicalInductionVariable();
      EXPECT_EQ(CanonicalIV, &GetInstByName(F, "canonical.iv"));

      auto *AR = GetAddRec(SE, Loop);
      EXPECT_FALSE(AR->isAffine());

      unsigned ExpectedCanonicalIVWidth = SE.getTypeSizeInBits(AR->getType());
      unsigned CanonicalIVBitWidth =
          cast<IntegerType>(CanonicalIV->getType())->getBitWidth();
      EXPECT_LT(CanonicalIVBitWidth, ExpectedCanonicalIVWidth);

      SCEVExpander Exp(SE, "expander");
      auto *InsertAt = I.getNextNode();
      Value *V = Exp.expandCodeFor(AR, nullptr, InsertAt);
      const SCEV *ExpandedAR = SE.getSCEV(V);
      // Check that the expansion happened literally.
      EXPECT_EQ(AR, ExpandedAR);
    });
  };

  // Expand the addrec produced by GetAddRec into a loop with a canonical IV
  // of addrec width.
  auto TestMatchingCanonicalIV =
      [&](std::function<const SCEVAddRecExpr *(ScalarEvolution & SE, Loop * L)>
              GetAddRec,
          unsigned ARBitWidth) {
        auto ARBitWidthTypeStr = "i" + std::to_string(ARBitWidth);
        std::unique_ptr<Module> M = parseAssemblyString(
            "define i32 @test(i32 %limit) { "
            "entry: "
            "  br label %loop "
            "loop: "
            "  %i = phi i32 [ 1, %entry ], [ %i.inc, %loop ] "
            "  %canonical.iv = phi " +
                ARBitWidthTypeStr +
                " [ 0, %entry ], [ %canonical.iv.inc, %loop ] "
                "  %i.inc = add nsw i32 %i, 1 "
                "  %canonical.iv.inc = add " +
                ARBitWidthTypeStr +
                " %canonical.iv, 1 "
                "  %cont = icmp slt i32 %i.inc, %limit "
                "  br i1 %cont, label %loop, label %exit "
                "exit: "
                "  ret i32 %i.inc "
                "}",
            Err, C);

        assert(M && "Could not parse module?");
        assert(!verifyModule(*M) && "Must have been well formed!");

        runWithSE(
            *M, "test", [&](Function &F, LoopInfo &LI, ScalarEvolution &SE) {
              auto &I = GetInstByName(F, "i");
              auto &CanonicalIV = GetInstByName(F, "canonical.iv");

              auto *LoopHeaderBB = I.getParent();
              auto *Loop = LI.getLoopFor(LoopHeaderBB);
              EXPECT_EQ(&CanonicalIV, Loop->getCanonicalInductionVariable());
              unsigned CanonicalIVBitWidth =
                  cast<IntegerType>(CanonicalIV.getType())->getBitWidth();

              auto *AR = GetAddRec(SE, Loop);
              EXPECT_FALSE(AR->isAffine());
              EXPECT_EQ(ARBitWidth, SE.getTypeSizeInBits(AR->getType()));
              EXPECT_EQ(CanonicalIVBitWidth, ARBitWidth);

              SCEVExpander Exp(SE, "expander");
              auto *InsertAt = I.getNextNode();
              Value *V = Exp.expandCodeFor(AR, nullptr, InsertAt);
              const SCEV *ExpandedAR = SE.getSCEV(V);
              // Check that the expansion happened literally.
              EXPECT_EQ(AR, ExpandedAR);
            });
      };

  unsigned ARBitWidth = 16;
  Type *ARType = IntegerType::get(C, ARBitWidth);

  // Expand {5,+,1,+,1}
  auto GetAR3 = [&](ScalarEvolution &SE, Loop *L) -> const SCEVAddRecExpr * {
    SmallVector<SCEVUse, 3> Ops = {SE.getConstant(APInt(ARBitWidth, 5)),
                                   SE.getOne(ARType), SE.getOne(ARType)};
    return cast<SCEVAddRecExpr>(SE.getAddRecExpr(Ops, L, SCEV::FlagNone));
  };
  TestNoCanonicalIV(GetAR3);
  TestNarrowCanonicalIV(GetAR3);
  TestMatchingCanonicalIV(GetAR3, ARBitWidth);

  // Expand {5,+,1,+,1,+,1}
  auto GetAR4 = [&](ScalarEvolution &SE, Loop *L) -> const SCEVAddRecExpr * {
    SmallVector<SCEVUse, 4> Ops = {SE.getConstant(APInt(ARBitWidth, 5)),
                                   SE.getOne(ARType), SE.getOne(ARType),
                                   SE.getOne(ARType)};
    return cast<SCEVAddRecExpr>(SE.getAddRecExpr(Ops, L, SCEV::FlagNone));
  };
  TestNoCanonicalIV(GetAR4);
  TestNarrowCanonicalIV(GetAR4);
  TestMatchingCanonicalIV(GetAR4, ARBitWidth);

  // Expand {5,+,1,+,1,+,1,+,1}
  auto GetAR5 = [&](ScalarEvolution &SE, Loop *L) -> const SCEVAddRecExpr * {
    SmallVector<SCEVUse, 5> Ops = {SE.getConstant(APInt(ARBitWidth, 5)),
                                   SE.getOne(ARType), SE.getOne(ARType),
                                   SE.getOne(ARType), SE.getOne(ARType)};
    return cast<SCEVAddRecExpr>(SE.getAddRecExpr(Ops, L, SCEV::FlagNone));
  };
  TestNoCanonicalIV(GetAR5);
  TestNarrowCanonicalIV(GetAR5);
  TestMatchingCanonicalIV(GetAR5, ARBitWidth);
}

TEST_F(ScalarEvolutionExpanderTest, ExpandNonIntegralPtrWithNullBase) {
  LLVMContext C;
  SMDiagnostic Err;

  std::unique_ptr<Module> M =
      parseAssemblyString("target datalayout = "
                          "\"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:"
                          "128-n8:16:32:64-S128-ni:1-p2:32:8:8:32-ni:2\""
                          "define ptr addrspace(1) @test(i64 %offset) { "
                          "  %ptr = getelementptr inbounds float, ptr "
                          "addrspace(1) null, i64 %offset"
                          "  ret ptr addrspace(1) %ptr"
                          "}",
                          Err, C);

  assert(M && "Could not parse module?");
  assert(!verifyModule(*M) && "Must have been well formed!");

  runWithSE(*M, "test", [&](Function &F, LoopInfo &LI, ScalarEvolution &SE) {
    auto &I = GetInstByName(F, "ptr");
    auto PtrPlus1 =
        SE.getAddExpr(SE.getSCEV(&I), SE.getConstant(I.getType(), 1));
    SCEVExpander Exp(SE, "expander");

    Value *V = Exp.expandCodeFor(PtrPlus1, I.getType(), &I);
    I.replaceAllUsesWith(V);

    // Check that the expander created:
    // define ptr addrspace(1) @test(i64 %off) {
    //   %1 = shl i64 %offset, 2
    //   %2 = add nuw nsw i64 %1, 1
    //   %uglygep = getelementptr i8, ptr addrspace(1) null, i64 %2
    //   %ptr = getelementptr inbounds float, ptr addrspace(1) null, i64 %off
    //   ret ptr addrspace(1) %uglygep
    // }

    Value *Offset = &*F.arg_begin();
    auto *GEP = dyn_cast<GetElementPtrInst>(V);
    EXPECT_TRUE(GEP);
    EXPECT_TRUE(cast<Constant>(GEP->getPointerOperand())->isNullValue());
    EXPECT_EQ(GEP->getNumOperands(), 2U);
    EXPECT_TRUE(match(
        GEP->getOperand(1),
        m_Add(m_Shl(m_Specific(Offset), m_SpecificInt(2)), m_SpecificInt(1))));
    EXPECT_EQ(cast<PointerType>(GEP->getPointerOperand()->getType())
                  ->getAddressSpace(),
              cast<PointerType>(I.getType())->getAddressSpace());
    EXPECT_FALSE(verifyFunction(F, &errs()));
  });
}

TEST_F(ScalarEvolutionExpanderTest, GEPFlags) {
  LLVMContext C;
  SMDiagnostic Err;
  StringRef ModStr = R"(
  define void @f(ptr %p, i64 %x) {
    %gep_inbounds = getelementptr inbounds i8, ptr %p, i64 %x
    ret void
  })";
  std::unique_ptr<Module> M = parseAssemblyString(ModStr, Err, C);

  assert(M && "Could not parse module?");
  assert(!verifyModule(*M) && "Must have been well formed!");

  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr) << "Could not find function 'f'";
  BasicBlock &Entry = F->getEntryBlock();
  auto *GEP = cast<GetElementPtrInst>(&Entry.front());

  ScalarEvolution SE = buildSE(*F);
  const SCEV *Ptr = SE.getSCEV(F->getArg(0));
  const SCEV *X = SE.getSCEV(F->getArg(1));
  const SCEV *PtrX = SE.getAddExpr(Ptr, X);

  SCEVExpander Exp(SE, "expander");
  auto *I = cast<Instruction>(
      Exp.expandCodeFor(PtrX, nullptr, Entry.getTerminator()));
  // Check that the GEP is reused, but the inbounds flag cleared. We don't
  // know that the newly introduced use is inbounds.
  EXPECT_EQ(I, GEP);
  EXPECT_EQ(GEP->getNoWrapFlags(), GEPNoWrapFlags::none());
}

// Test that InsertBinop scans existing instructions in the block.
TEST_F(ScalarEvolutionExpanderTest, InsertBinopReuseShlWithMatchingFlags) {
  LLVMContext C;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString("define void @f(i64 %n) { "
                                                  "  ret void "
                                                  "}",
                                                  Err, C);

  assert(M && "Could not parse module?");
  assert(!verifyModule(*M) && "Must have been well formed!");

  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  ScalarEvolution SE = buildSE(*F);

  auto *I64Ty = Type::getInt64Ty(C);
  const SCEV *N = SE.getSCEV(F->getArg(0));
  const SCEV *Four = SE.getConstant(I64Ty, 4);
  const SCEV *Mul = SE.getMulExpr(N, Four, SCEV::FlagNUW | SCEV::FlagNSW);
  const SCEV *Expr1 = SE.getAddExpr(Mul, SE.getConstant(I64Ty, 8));
  const SCEV *Expr2 = SE.getAddExpr(Mul, SE.getConstant(I64Ty, 16));

  // Expand with separate expanders so the InsertedExpressions cache doesn't
  // apply.
  auto *InsertBefore = F->getEntryBlock().getTerminator();
  SCEVExpander Exp1(SE, "expander");
  Value *V1 = Exp1.expandCodeFor(Expr1, nullptr, InsertBefore);
  SCEVExpander Exp2(SE, "expander");
  Value *V2 = Exp2.expandCodeFor(Expr2, nullptr, InsertBefore);

  // Both expansions produce different values (different constants added).
  EXPECT_NE(V1, V2);

  // Both should share the same shl sub-expression via pattern match.
  Value *Shl1 = nullptr, *Shl2 = nullptr;
  Value *Arg = F->getArg(0);
  EXPECT_TRUE(match(V1, m_Add(m_Value(Shl1), m_SpecificInt(8))));
  EXPECT_TRUE(match(V2, m_Add(m_Value(Shl2), m_SpecificInt(16))));
  EXPECT_EQ(Shl1, Shl2) << "Expected the shl to be reused";

  // Verify the shared shl has the expected form and flags.
  auto *ShlInst = cast<Instruction>(Shl1);
  EXPECT_TRUE(match(ShlInst, m_Shl(m_Specific(Arg), m_SpecificInt(2))));
  EXPECT_TRUE(ShlInst->hasNoUnsignedWrap());
  EXPECT_TRUE(ShlInst->hasNoSignedWrap());
}

// Test recognizing urems that are truncated, scaled and/or part of a larger
// sum.
TEST_F(ScalarEvolutionExpanderTest, MatchAffineURem) {
  LLVMContext C;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    define void @f(i64 %x, i64 %y, i64 %z, i32 %d, i8 %a, i8 %b, i1 %c) {
    entry:
      br label %loop
    loop:
      %iv = phi i64 [ %x, %entry ], [ %iv.next, %loop ]
      %iv.next = add i64 %iv, 1
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    }
  )",
                                                  Err, C);
  ASSERT_TRUE(M && !verifyModule(*M, &errs()));

  runWithSE(*M, "f", [&](Function &F, LoopInfo &LI, ScalarEvolution &SE) {
    Type *I64Ty = Type::getInt64Ty(C);
    Type *I32Ty = Type::getInt32Ty(C);
    Type *I8Ty = Type::getInt8Ty(C);
    const SCEV *X = SE.getSCEV(F.getArg(0));
    const SCEV *Y = SE.getSCEV(F.getArg(1));
    const SCEV *Z = SE.getSCEV(F.getArg(2));
    const SCEV *D = SE.getZeroExtendExpr(SE.getSCEV(F.getArg(3)), I64Ty);
    const SCEV *A = SE.getSCEV(F.getArg(4));
    const SCEV *B = SE.getSCEV(F.getArg(5));
    const SCEV *IV = SE.getSCEV(&GetInstByName(F, "iv"));
    auto Const = [&](int64_t V, Type *Ty) {
      return SE.getConstant(Ty, V, /*isSigned=*/true);
    };
    auto Scaled = [&](int64_t Scale, const SCEV *S) {
      return SE.getMulExpr(Const(Scale, S->getType()), S);
    };

    // Returns true if S is matched as
    // Offset + Scale * trunc_or_noop(LHS urem RHS).
    auto Matches = [&](const SCEV *S, const SCEV *LHS, const SCEV *RHS,
                       int64_t Scale, const SCEV *Offset) {
      auto URem = SCEVExpander::matchAffineURem(cast<SCEVAddExpr>(S), SE);
      return URem && URem->LHS == LHS && URem->RHS == RHS &&
             URem->Scale.getSExtValue() == Scale && URem->Offset == Offset;
    };
    auto MatchesAny = [&](const SCEV *S) {
      const auto *Add = dyn_cast<SCEVAddExpr>(S);
      return Add && SCEVExpander::matchAffineURem(Add, SE);
    };
    const SCEV *Zero = Const(0, I64Ty);
    const SCEV *XRemY = SE.getURemExpr(X, Y);

    // The urem itself, also with a constant LHS, which sorts before the
    // multiple of the divisor in the add.
    EXPECT_TRUE(Matches(XRemY, X, Y, 1, Zero));
    const SCEV *C100 = Const(100, I64Ty);
    EXPECT_TRUE(Matches(SE.getURemExpr(C100, Y), C100, Y, 1, Zero));

    // Scaled and negated urems.
    EXPECT_TRUE(Matches(Scaled(256, XRemY), X, Y, 256, Zero));
    EXPECT_TRUE(Matches(Scaled(-256, XRemY), X, Y, -256, Zero));
    EXPECT_TRUE(Matches(SE.getNegativeSCEV(XRemY), X, Y, -1, Zero));
    EXPECT_TRUE(Matches(Scaled(INT64_MIN, XRemY), X, Y, INT64_MIN, Zero));

    // Divisors that are products. Their constant factor is folded into the
    // scale.
    const SCEV *C6 = Const(6, I64Ty);
    EXPECT_TRUE(Matches(Scaled(64, SE.getURemExpr(X, C6)), X, C6, 64, Zero));
    const SCEV *Y3 = Scaled(3, Y);
    EXPECT_TRUE(Matches(Scaled(-5, SE.getURemExpr(X, Y3)), X, Y3, -5, Zero));
    const SCEV *YZ = SE.getMulExpr(Y, Z);
    EXPECT_TRUE(Matches(Scaled(4, SE.getURemExpr(X, YZ)), X, YZ, 4, Zero));

    // Urems that are part of a larger sum. The multiple of the LHS may be
    // folded into another operand or a constant.
    EXPECT_TRUE(Matches(SE.getMinusSCEV(Z, Scaled(256, SE.getURemExpr(IV, D))),
                        IV, D, -256, Z));
    const SCEV *XPlus1 = SE.getAddExpr(X, Const(1, I64Ty));
    EXPECT_TRUE(
        Matches(SE.getMinusSCEV(SE.getURemExpr(XPlus1, Y), Const(1, I64Ty)),
                XPlus1, Y, 1, Const(-1, I64Ty)));
    const SCEV *C100DivY = SE.getMulExpr(SE.getUDivExpr(C100, Y), Y);
    EXPECT_TRUE(Matches(SE.getMinusSCEV(Const(5, I64Ty), C100DivY), C100, Y, 1,
                        Const(-95, I64Ty)));

    // Truncated urems, where the truncate is not pushed into the multiple of
    // the divisor (Y) or pushed into it (D and constants), and with scaling
    // before and after truncating.
    const SCEV *Zero32 = Const(0, I32Ty);
    EXPECT_TRUE(
        Matches(SE.getTruncateExpr(Scaled(4, SE.getURemExpr(IV, Y)), I32Ty), IV,
                Y, 4, Zero32));
    EXPECT_TRUE(Matches(
        SE.getMinusSCEV(Const(2048, I32Ty),
                        SE.getTruncateExpr(SE.getURemExpr(IV, Y), I32Ty)),
        IV, Y, -1, Const(2048, I32Ty)));
    EXPECT_TRUE(
        Matches(Scaled(4, SE.getTruncateExpr(SE.getURemExpr(IV, D), I32Ty)), IV,
                D, 4, Zero32));
    EXPECT_TRUE(Matches(SE.getTruncateExpr(SE.getURemExpr(IV, C6), I32Ty), IV,
                        C6, 1, Zero32));

    // Narrow urems. The scale is not recovered if its product with the
    // constant factor of the divisor wraps.
    const SCEV *Zero8 = Const(0, I8Ty);
    EXPECT_TRUE(Matches(Scaled(-128, SE.getURemExpr(A, B)), A, B, -128, Zero8));
    const SCEV *C60 = Const(60, I8Ty);
    EXPECT_TRUE(Matches(Scaled(2, SE.getURemExpr(A, C60)), A, C60, 2, Zero8));
    EXPECT_FALSE(MatchesAny(Scaled(2, SE.getURemExpr(A, Const(100, I8Ty)))));

    // Do not match if the multiple of the LHS does not cancel out, for
    // non-constant scales, or for constant power-of-2 divisors.
    const SCEV *XDivY = SE.getMulExpr(SE.getUDivExpr(X, Y), Y);
    EXPECT_FALSE(MatchesAny(SE.getMinusSCEV(Z, XDivY)));
    EXPECT_FALSE(MatchesAny(SE.getMinusSCEV(X, Scaled(2, XDivY))));
    EXPECT_FALSE(MatchesAny(SE.getMulExpr(Z, XRemY)));
    const SCEV *C8 = Const(8, I64Ty);
    const SCEV *XDiv8 = SE.getMulExpr(SE.getUDivExpr(X, C8), C8);
    EXPECT_FALSE(MatchesAny(SE.getAddExpr(Z, SE.getMinusSCEV(X, XDiv8))));
  });
}

// Test that urems that are truncated, scaled and/or part of a larger sum are
// expanded as urem, instead of udiv, mul and sub.
TEST_F(ScalarEvolutionExpanderTest, ExpandAffineURem) {
  LLVMContext C;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    define void @f(i64 %x, i64 %y, i64 %z, i32 %a, i32 %b,
                   i64 range(i64 1, 100) %nz) {
    entry:
      %rem = urem i64 %x, %y
      br label %exit
    exit:
      ret void
    }
  )",
                                                  Err, C);
  ASSERT_TRUE(M && !verifyModule(*M, &errs()));

  Function *F = M->getFunction("f");
  ScalarEvolution SE = buildSE(*F);
  Type *I64Ty = Type::getInt64Ty(C);
  Type *I32Ty = Type::getInt32Ty(C);
  Argument *XV = F->getArg(0), *YV = F->getArg(1), *ZV = F->getArg(2);
  Argument *AV = F->getArg(3), *BV = F->getArg(4), *NZV = F->getArg(5);
  const SCEV *X = SE.getSCEV(XV);
  const SCEV *Y = SE.getSCEV(YV);
  const SCEV *Z = SE.getSCEV(ZV);
  Instruction *Rem = &GetInstByName(*F, "rem");
  EXPECT_EQ(SE.getSCEV(Rem), SE.getURemExpr(X, Y));
  Instruction *InsertPt = F->back().getTerminator();
  SCEVExpander Exp(SE, "expander");

  // z - 256 * (x urem y) reuses %rem.
  const SCEV *ScaledRem =
      SE.getMulExpr(SE.getConstant(I64Ty, 256), SE.getURemExpr(X, Y));
  Value *V =
      Exp.expandCodeFor(SE.getMinusSCEV(Z, ScaledRem), nullptr, InsertPt);
  EXPECT_TRUE(match(
      V, m_Sub(m_Specific(ZV), m_Shl(m_Specific(Rem), m_SpecificInt(8)))));

  // (x urem y) + (z urem y) contains two urems.
  V = Exp.expandCodeFor(
      SE.getAddExpr(SE.getURemExpr(X, Y), SE.getURemExpr(Z, Y)), nullptr,
      InsertPt);
  EXPECT_TRUE(match(
      V, m_c_Add(m_Specific(Rem), m_URem(m_Specific(ZV), m_Specific(YV)))));

  // z - ((z + 1) /u y) * y is ((z + 1) urem y) - 1.
  const SCEV *ZPlus1 = SE.getAddExpr(Z, SE.getOne(I64Ty));
  V = Exp.expandCodeFor(
      SE.getMinusSCEV(Z, SE.getMulExpr(SE.getUDivExpr(ZPlus1, Y), Y)), nullptr,
      InsertPt);
  EXPECT_TRUE(
      match(V, m_Add(m_URem(m_Add(m_Specific(ZV), m_One()), m_Specific(YV)),
                     m_AllOnes())));

  // 4 * trunc(zext(a) urem zext(b)), with the truncate pushed into the
  // operands of the add and mul representing the urem.
  const SCEV *WideRem =
      SE.getURemExpr(SE.getZeroExtendExpr(SE.getSCEV(AV), I64Ty),
                     SE.getZeroExtendExpr(SE.getSCEV(BV), I64Ty));
  ASSERT_TRUE(isa<SCEVAddExpr>(SE.getTruncateExpr(WideRem, I32Ty)));
  V = Exp.expandCodeFor(SE.getMulExpr(SE.getConstant(I32Ty, 4),
                                      SE.getTruncateExpr(WideRem, I32Ty)),
                        nullptr, InsertPt);
  EXPECT_TRUE(match(
      V, m_Shl(m_Trunc(m_URem(m_ZExt(m_Specific(AV)), m_ZExt(m_Specific(BV)))),
               m_SpecificInt(2))));

  for (Instruction &I : instructions(*F))
    EXPECT_NE(I.getOpcode(), Instruction::UDiv);

  // In SafeUDivMode, a urem is only expanded as such if its divisor cannot be
  // zero or poison. Otherwise it is expanded with a guarded udiv. %y may be
  // zero, but cannot be poison as it is the divisor of %rem. %nz cannot be
  // zero, but may be poison.
  SCEVExpander SafeExp(SE, "expander", /*PreserveLCSSA=*/true,
                       /*SafeUDivMode=*/true);
  V = SafeExp.expandCodeFor(SE.getURemExpr(Z, SE.getConstant(I64Ty, 6)),
                            nullptr, InsertPt);
  EXPECT_TRUE(match(V, m_URem(m_Specific(ZV), m_SpecificInt(6))));
  V = SafeExp.expandCodeFor(SE.getURemExpr(Z, Y), nullptr, InsertPt);
  EXPECT_TRUE(match(
      V, m_Sub(m_Specific(ZV),
               m_c_Mul(m_UDiv(m_Specific(ZV), m_UMax(m_Specific(YV), m_One())),
                       m_Specific(YV)))));
  V = SafeExp.expandCodeFor(SE.getURemExpr(Z, SE.getSCEV(NZV)), nullptr,
                            InsertPt);
  EXPECT_TRUE(
      match(V, m_Sub(m_Specific(ZV),
                     m_c_Mul(m_UDiv(m_Specific(ZV),
                                    m_UMax(m_Freeze(m_Specific(NZV)), m_One())),
                             m_Specific(NZV)))));
  EXPECT_FALSE(verifyFunction(*F, &errs()));
}

// Test that a urem with a divisor known to be non-zero is hoisted out of loops
// like the udiv it replaces.
TEST_F(ScalarEvolutionExpanderTest, ExpandAffineURemHoist) {
  LLVMContext C;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(R"(
    define void @f(i64 %x, i64 range(i64 1, 100) %y, i64 %n) {
    entry:
      br label %loop
    loop:
      %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
      %iv.next = add i64 %iv, 1
      %c = icmp ult i64 %iv.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    }
  )",
                                                  Err, C);
  ASSERT_TRUE(M && !verifyModule(*M, &errs()));

  Function *F = M->getFunction("f");
  ScalarEvolution SE = buildSE(*F);
  Argument *XV = F->getArg(0), *YV = F->getArg(1);
  const SCEV *ScaledRem =
      SE.getMulExpr(SE.getConstant(XV->getType(), 256),
                    SE.getURemExpr(SE.getSCEV(XV), SE.getSCEV(YV)));
  SCEVExpander Exp(SE, "expander");
  Value *V = Exp.expandCodeFor(
      ScaledRem, nullptr, GetInstByName(*F, "iv").getParent()->getTerminator());
  Value *Rem;
  ASSERT_TRUE(match(V, m_Shl(m_Value(Rem), m_SpecificInt(8))));
  EXPECT_TRUE(match(Rem, m_URem(m_Specific(XV), m_Specific(YV))));
  EXPECT_EQ(cast<Instruction>(Rem)->getParent(), &F->getEntryBlock());
  EXPECT_FALSE(verifyFunction(*F, &errs()));
}

} // end namespace llvm
