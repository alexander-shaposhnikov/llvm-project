; foo writes only through its second argument. The call passes noalias p and q,
; so a store/reload through p is not clobbered by foo's write through q.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i32 @test(ptr noalias %p, ptr noalias %q) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK-NOT:     load i32, ptr %p
; CHECK:         ret i32 42
entry:
  store i32 42, ptr %p
  call void @foo(ptr %p, ptr %q)
  %v = load i32, ptr %p
  ret i32 %v
}

; Writes only *q (arg 1). Does not touch arg 0.
define void @foo(ptr %p, ptr %q) {
entry:
  store i32 99, ptr %q
  ret void
}
