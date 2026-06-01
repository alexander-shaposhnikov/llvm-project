; Redundant load elimination across a call. Two loads of p[2] straddle a call
; that writes only p[1] (disjoint). The second load is redundant and is CSE'd
; to the first.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i16 @test(ptr %p, ptr %q) {
; The second load is CSE'd to the first, so %a - %b folds to 0 and both loads
; become dead. (Without IPModRef the call is assumed to clobber p[2], the loads
; differ, and nothing folds.)
;
; CHECK-LABEL: @test(
; CHECK-NOT:     load
; CHECK:         ret i16 0
entry:
  %p2 = getelementptr inbounds i16, ptr %p, i64 2
  %a = load i16, ptr %p2
  call void @foo(ptr %p, ptr %q)
  %b = load i16, ptr %p2
  %d = sub i16 %a, %b
  ret i16 %d
}

; Writes p[1] only (bytes [2,4)), disjoint from p[2] (bytes [4,6)).
define void @foo(ptr %p, ptr %q) {
entry:
  %p1 = getelementptr inbounds i16, ptr %p, i64 1
  store i16 7, ptr %p1
  ret void
}
