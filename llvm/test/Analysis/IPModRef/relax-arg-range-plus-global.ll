; Relaxation v1: callee writes p[1] (a sub-object range) AND a global @g.
; A store/reload of p[2] across the call should fold: p[2] is disjoint from the
; arg write range [8,16), and the global write cannot alias the alloca. This is
; the access-range win (c1-style) but in a callee that also touches a global.
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
  %p2 = getelementptr inbounds i64, ptr %p, i64 2
  store i64 %val, ptr %p2
  call void @callee(ptr %p)
  %v = load i64, ptr %p2
  ret i64 %v
}

; writes p[1] (bytes [8,16)) and the global @g
define void @callee(ptr %p) {
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 1, ptr %p1
  store i64 2, ptr @g
  ret void
}
