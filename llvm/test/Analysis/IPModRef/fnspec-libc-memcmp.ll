; libc fnspec: memcmp is read-only. Even though it *reads* p[0,16) (overlapping
; the stored location), it does not modify it, so the reload at p[0] folds. This
; checks the Ref-only (no Mod) modelling. A bare declaration would be opaque.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @memcmp(ptr, ptr, i64)

define i64 @test(ptr noalias %q) {
; CHECK-LABEL: @test(
; CHECK:         call i32 @memcmp(
; CHECK:         ret i64 0
entry:
  %p = alloca [64 x i8]
  store i64 7, ptr %p
  %a = load i64, ptr %p
  %c = call i32 @memcmp(ptr %p, ptr %q, i64 16)
  %b = load i64, ptr %p
  %diff = sub i64 %a, %b
  ret i64 %diff
}
