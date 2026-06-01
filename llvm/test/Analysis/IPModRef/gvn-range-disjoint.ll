; Headline IPA-modref case (from modref.pdf "tracking access ranges"):
;   foo writes only p[1] and reads only q[2]. A store to p[2] before the call
;   and a reload of p[2] after the call can be forwarded, because foo's write
;   range [2,4) is disjoint from p[2] = bytes [4,6).
;
; Without IPModRef the call is assumed to clobber all of *p and the reload is
; not folded. With IPModRef the reload folds to the stored constant 1.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

define i16 @test(ptr %p, ptr %q) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK-NOT:     load i16
; CHECK:         ret i16 1
entry:
  %p2 = getelementptr inbounds i16, ptr %p, i64 2
  store i16 1, ptr %p2
  call void @foo(ptr %p, ptr %q)
  %v = load i16, ptr %p2
  ret i16 %v
}

define void @foo(ptr %p, ptr %q) {
entry:
  %p1 = getelementptr inbounds i16, ptr %p, i64 1
  %q2 = getelementptr inbounds i16, ptr %q, i64 2
  %t = load i16, ptr %q2
  store i16 %t, ptr %p1
  ret void
}
