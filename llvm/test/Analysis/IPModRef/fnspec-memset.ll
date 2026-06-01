; fnspec #1: a callee whose only effect is memset(p,0,16) writes p[0,16). A
; store/reload of p[2] (bytes [16,24)) across the call should fold. Today the
; callee bails (the memset intrinsic is a call -> AccessesUnknown).
; Modeled on gcc ipa/modref-2.c (memset records an argument write range).
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)

define i64 @test(ptr %p, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64
; CHECK:         ret i64 %v
entry:
  %p2 = getelementptr inbounds i64, ptr %p, i64 2
  store i64 %v, ptr %p2
  call void @callee(ptr %p)
  %r = load i64, ptr %p2
  ret i64 %r
}

define void @callee(ptr %p) {
entry:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 16, i1 false)
  ret void
}
