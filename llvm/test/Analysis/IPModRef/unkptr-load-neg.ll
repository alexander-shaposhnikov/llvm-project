; SOUNDNESS (negative): a store through a pointer LOADED from memory is a
; genuinely unknown object -> AccessesUnknown -> the reload of b[6] must NOT fold.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr noalias %a, ptr noalias %b, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK:         %r = load i64, ptr %b6
; CHECK:         ret i64 %r
entry:
  %b6 = getelementptr inbounds i64, ptr %b, i64 6
  store i64 %v, ptr %b6
  call void @callee(ptr %a, ptr %b)
  %r = load i64, ptr %b6
  ret i64 %r
}
define void @callee(ptr %a, ptr %b) {
  %p = load ptr, ptr %a              ; loaded pointer -> unknown
  store i64 0, ptr %p
  %b5 = getelementptr inbounds i64, ptr %b, i64 5
  store i64 1, ptr %b5
  ret void
}
