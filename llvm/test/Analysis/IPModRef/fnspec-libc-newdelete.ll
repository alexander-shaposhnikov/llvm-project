; operator new returns fresh memory (touches no caller arg/global); operator
; delete invalidates only its argument. Both let an unrelated reload fold.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"
declare ptr @_Znwm(i64)
declare void @_ZdlPv(ptr)

define i64 @new_is_fresh() {
; CHECK-LABEL: @new_is_fresh(
; CHECK:         call ptr @_Znwm
; CHECK:         ret i64 0
  %q = alloca i64
  store i64 7, ptr %q
  %a = load i64, ptr %q
  %p = call ptr @_Znwm(i64 32)
  %b = load i64, ptr %q
  %d = sub i64 %a, %b
  ret i64 %d
}

define i64 @delete_only_touches_arg(ptr %p) {
; CHECK-LABEL: @delete_only_touches_arg(
; CHECK:         call void @_ZdlPv
; CHECK:         ret i64 0
  %q = alloca i64
  store i64 7, ptr %q
  %a = load i64, ptr %q
  call void @_ZdlPv(ptr %p)
  %b = load i64, ptr %q
  %d = sub i64 %a, %b
  ret i64 %d
}
