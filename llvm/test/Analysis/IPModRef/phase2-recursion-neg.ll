; SOUNDNESS (negative): self-recursive callee -> cycle guard -> AccessesUnknown,
; so the reload of p[1] must NOT be folded.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr %p, i64 %v, i64 %n) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK:         %r = load i64, ptr %p1
; CHECK:         ret i64 %r
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %v, ptr %p1
  call void @foo(ptr %p, i64 %n)
  %r = load i64, ptr %p1
  ret i64 %r
}
define void @foo(ptr %p, i64 %n) {
entry:
  %c = icmp ne i64 %n, 0
  br i1 %c, label %rec, label %end
rec:
  %n1 = sub i64 %n, 1
  call void @foo(ptr %p, i64 %n1)
  br label %end
end:
  store i64 0, ptr %p
  ret void
}
