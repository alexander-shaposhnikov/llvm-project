; SOUNDNESS (negative): merged callee writes p[1], which overlaps the reload of
; p[1] -> must NOT fold.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

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
define void @foo(ptr %p) { call void @bar(ptr %p) ret void }
define void @bar(ptr %p) { %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 0, ptr %p1 ret void }
