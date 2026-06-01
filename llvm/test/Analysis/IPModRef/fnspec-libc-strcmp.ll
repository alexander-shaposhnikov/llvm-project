; strcmp is read-only: a store before the call survives it (no Mod), reload folds.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"
declare i32 @strcmp(ptr, ptr)
define i64 @test(ptr noalias %e) {
; CHECK-LABEL: @test(
; CHECK:         call i32 @strcmp
; CHECK:         ret i64 0
  %d = alloca [64 x i8]
  store i64 7, ptr %d
  %a = load i64, ptr %d
  %c = call i32 @strcmp(ptr %d, ptr %e)
  %b = load i64, ptr %d
  %r = sub i64 %a, %b
  ret i64 %r
}
