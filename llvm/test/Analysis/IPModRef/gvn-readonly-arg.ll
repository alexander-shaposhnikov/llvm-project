; foo only reads its arguments (no stores at all). A store before the call and
; a reload after it can be forwarded: a read-only callee never clobbers.
; This is subsumed by 'readonly' attrs, but IPModRef derives it from the body
; with no attributes present.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i32 @test(ptr %p, ptr %q) {
; CHECK-LABEL: @test(
; CHECK:         call i32 @foo(
; CHECK-NOT:     load i32, ptr %p
; CHECK:         ret i32 7
entry:
  store i32 7, ptr %p
  %c = call i32 @foo(ptr %p, ptr %q)
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @foo(ptr %p, ptr %q) {
entry:
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %s = add i32 %a, %b
  ret i32 %s
}
