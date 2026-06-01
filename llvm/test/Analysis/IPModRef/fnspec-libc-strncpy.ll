; strncpy(d,s,16) writes only d[0,16); a store at d[20] survives the call.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"
declare ptr @strncpy(ptr, ptr, i64)
define i64 @test(ptr noalias %s) {
; CHECK-LABEL: @test(
; CHECK:         call ptr @strncpy
; CHECK:         ret i64 0
  %d = alloca [64 x i8]
  %p20 = getelementptr inbounds i8, ptr %d, i64 20
  store i64 7, ptr %p20
  %a = load i64, ptr %p20
  %c = call ptr @strncpy(ptr %d, ptr %s, i64 16)
  %b = load i64, ptr %p20
  %r = sub i64 %a, %b
  ret i64 %r
}
