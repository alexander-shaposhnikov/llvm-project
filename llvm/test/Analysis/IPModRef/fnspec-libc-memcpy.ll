; libc fnspec: a direct `call @memcpy(d, s, 16)` (bare declaration, no memory
; attributes) writes only d[0,16). A store at d[20] is therefore not clobbered,
; so its reload folds and the difference is 0. Without the analysis the bare
; declaration is opaque and the reload survives.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s
; RUN: opt < %s -aa-pipeline=basic-aa -passes='gvn' -S \
; RUN:   | FileCheck %s --check-prefix=NOFLAG

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare ptr @memcpy(ptr, ptr, i64)

define i64 @test(ptr noalias %s) {
; CHECK-LABEL: @test(
; CHECK:         call ptr @memcpy(
; CHECK:         ret i64 0
;
; NOFLAG-LABEL: @test(
; NOFLAG:         load i64
; NOFLAG:         ret i64 %diff
entry:
  %d = alloca [64 x i8]
  %p20 = getelementptr inbounds i8, ptr %d, i64 20
  store i64 7, ptr %p20
  %a = load i64, ptr %p20
  %c = call ptr @memcpy(ptr %d, ptr %s, i64 16)
  %b = load i64, ptr %p20
  %diff = sub i64 %a, %b
  ret i64 %diff
}
