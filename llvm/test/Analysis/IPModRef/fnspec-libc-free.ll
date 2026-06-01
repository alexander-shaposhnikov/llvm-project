; libc fnspec: free(p) modifies (invalidates) only *p. A store to an unrelated
; alloca survives the call, so its reload folds. The bare declaration is
; otherwise opaque.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @free(ptr)

define i64 @test(ptr %p) {
; CHECK-LABEL: @test(
; CHECK:         call void @free(
; CHECK:         ret i64 0
entry:
  %q = alloca i64
  store i64 7, ptr %q
  %a = load i64, ptr %q
  call void @free(ptr %p)
  %b = load i64, ptr %q
  %diff = sub i64 %a, %b
  ret i64 %diff
}
