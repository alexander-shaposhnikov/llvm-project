; fnspec #1: callee does memcpy(d, s, 16) -> writes d[0,16), reads s[0,16).
; A store/reload of d[2] (bytes [16,24)) across the call should fold.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define i64 @test(ptr %d, ptr %s, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64, ptr %d2
; CHECK:         ret i64 %v
entry:
  %d2 = getelementptr inbounds i64, ptr %d, i64 2
  store i64 %v, ptr %d2
  call void @callee(ptr %d, ptr %s)
  %r = load i64, ptr %d2
  ret i64 %r
}

define void @callee(ptr %d, ptr %s) {
entry:
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 16, i1 false)
  ret void
}
