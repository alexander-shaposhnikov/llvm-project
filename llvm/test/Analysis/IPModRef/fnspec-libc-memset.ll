; libc fnspec: `call @memset(d, 0, 16)` writes only d[0,16); the reload at
; d[20] folds.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @memset(ptr, i32, i64)

define i64 @test() {
; CHECK-LABEL: @test(
; CHECK:         call ptr @memset(
; CHECK:         ret i64 0
entry:
  %d = alloca [64 x i8]
  %p20 = getelementptr inbounds i8, ptr %d, i64 20
  store i64 7, ptr %p20
  %a = load i64, ptr %p20
  %c = call ptr @memset(ptr %d, i32 0, i64 16)
  %b = load i64, ptr %p20
  %diff = sub i64 %a, %b
  ret i64 %diff
}
