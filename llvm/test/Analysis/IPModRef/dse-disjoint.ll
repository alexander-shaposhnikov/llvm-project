; Dead store elimination across a call. The first store to p[2] is overwritten
; by the second store, and the intervening call only writes p[1] (disjoint) and
; reads q - it neither reads nor writes p[2]. So the first store is dead.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,dse' -S | FileCheck %s

define void @test(ptr noalias %p, ptr noalias %q) {
; CHECK-LABEL: @test(
; CHECK-NOT:     store i16 1
; CHECK:         call void @foo(
; CHECK:         store i16 2
; CHECK:         ret void
entry:
  %p2 = getelementptr inbounds i16, ptr %p, i64 2
  store i16 1, ptr %p2
  call void @foo(ptr %p, ptr %q)
  store i16 2, ptr %p2
  ret void
}

; Writes p[1] only; reads q[0]. nounwind so the pre-call store can't be
; observed on an unwind path (isolates the test to AA precision).
define void @foo(ptr %p, ptr %q) nounwind {
entry:
  %p1 = getelementptr inbounds i16, ptr %p, i64 1
  %t = load i16, ptr %q
  store i16 %t, ptr %p1
  ret void
}
