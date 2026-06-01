; Relaxation v1 (direct global): callee writes p[0] AND a global @g. A store/
; reload of p[1] across the call should fold: p[1] is disjoint from the arg
; write range [0,8), and the global write cannot alias the caller's alloca.
;
; Today IPModRef bails (AccessesUnknown) because the callee touches a global.
; Baseline -O2 also cannot fold (callee is not argmemonly, arg is write-only).
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

@g = global i64 0

define i64 @test(i64 %val) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64
; CHECK:         ret i64 %val
entry:
  %p = alloca [4 x i64]
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %val, ptr %p1
  call void @callee(ptr %p)
  %v = load i64, ptr %p1
  ret i64 %v
}

; writes p[0] (bytes [0,8)) and the global @g
define void @callee(ptr %p) {
entry:
  store i64 7, ptr %p
  store i64 5, ptr @g
  ret void
}
