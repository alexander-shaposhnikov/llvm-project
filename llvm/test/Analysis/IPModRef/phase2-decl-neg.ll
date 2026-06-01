; SOUNDNESS (negative): callee calls an external declaration -> AccessesUnknown,
; reload NOT folded.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @ext(ptr)
define i64 @test(ptr %p, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK:         %r = load i64, ptr %p1
; CHECK:         ret i64 %r
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %v, ptr %p1
  call void @foo(ptr %p)
  %r = load i64, ptr %p1
  ret i64 %r
}
define void @foo(ptr %p) { call void @ext(ptr %p) ret void }
