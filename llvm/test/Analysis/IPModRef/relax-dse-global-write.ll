; Relaxation v1 (modeled on gcc modref-dse-1): callee reads p[0] AND writes a
; global @g. A dead store to p[1] before the call should be eliminated: the
; callee reads only p[0] (not p[1]) and its global write cannot alias the
; caller's alloca, so nothing reads the first store before it is overwritten.
;
; Today IPModRef bails (global write). Baseline -O2 keeps the store (callee is
; not argmemonly; arg may be read).
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,dse' -S | FileCheck %s

@g = global i64 0

define i64 @test(i64 %a, i64 %b) {
; CHECK-LABEL: @test(
; CHECK-NOT:     store i64 %a
; CHECK:         call void @callee(
; CHECK:         store i64 %b
; CHECK:         ret
entry:
  %p = alloca [4 x i64]
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %a, ptr %p1
  call void @callee(ptr %p)
  store i64 %b, ptr %p1
  %v = load i64, ptr %p1
  ret i64 %v
}

; reads p[0] only; writes the global @g. nounwind so the pre-call store can't be
; observed on an unwind path.
define void @callee(ptr %p) nounwind {
entry:
  %x = load i64, ptr %p
  store i64 %x, ptr @g
  ret void
}
