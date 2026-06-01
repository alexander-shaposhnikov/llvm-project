; Relaxation v1: callee reads p[0] AND writes a global @g (no writes to the arg).
; A value stored at p[1] is still present after the call (callee neither reads
; nor writes p[1], and its global write cannot alias the alloca), so the reload
; folds and the difference is 0.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

@g = global i64 0

define i64 @test(i64 %val) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64
; CHECK:         ret i64 0
entry:
  %p = alloca [4 x i64]
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %val, ptr %p1
  %a = load i64, ptr %p1
  call void @callee(ptr %p)
  %b = load i64, ptr %p1
  %d = sub i64 %a, %b
  ret i64 %d
}

; reads p[0]; writes the global @g
define void @callee(ptr %p) {
entry:
  %x = load i64, ptr %p
  store i64 %x, ptr @g
  ret void
}
