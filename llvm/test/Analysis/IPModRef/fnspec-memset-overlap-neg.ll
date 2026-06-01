; SOUNDNESS (negative): memset(p,0,16) writes p[0,16); a reload of p[0] overlaps
; and must NOT be folded.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)

define i64 @test(ptr %p, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK:         %r = load i64, ptr %p
; CHECK:         ret i64 %r
entry:
  store i64 %v, ptr %p
  call void @callee(ptr %p)
  %r = load i64, ptr %p
  ret i64 %r
}

define void @callee(ptr %p) {
entry:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 16, i1 false)
  ret void
}
