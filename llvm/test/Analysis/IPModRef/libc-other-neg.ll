; SOUNDNESS (negative): a callee that calls a function which may write arbitrary
; ("other") memory must bail -- the reload of p[1] is NOT folded.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @ext_any(ptr) memory(readwrite)

define i64 @test(ptr %p, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK:         %r = load i64, ptr %p1
; CHECK:         ret i64 %r
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %v, ptr %p1
  call void @callee(ptr %p)
  %r = load i64, ptr %p1
  ret i64 %r
}
define void @callee(ptr %p) {
  call void @ext_any(ptr %p)
  store i64 0, ptr %p
  ret void
}
