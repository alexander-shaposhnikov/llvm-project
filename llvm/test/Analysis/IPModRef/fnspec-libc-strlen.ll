; libc fnspec: strlen reads only its argument (Ref, nocapture) and writes
; nothing. A store to an unrelated alloca survives the call, so its reload
; folds. A bare declaration with no attributes would otherwise be opaque.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i64 @strlen(ptr)

define i64 @test(ptr %s) {
; CHECK-LABEL: @test(
; CHECK:         call i64 @strlen(
; CHECK:         ret i64 0
entry:
  %q = alloca i64
  store i64 7, ptr %q
  %a = load i64, ptr %q
  %len = call i64 @strlen(ptr %s)
  %b = load i64, ptr %q
  %diff = sub i64 %a, %b
  ret i64 %diff
}
