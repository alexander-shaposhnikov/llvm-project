; SOUNDNESS (negative): memset(p,0,n) with a variable length -> the write range
; is unknown, so the reload of p[2] must NOT be folded.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)

define i64 @test(ptr %p, i64 %v, i64 %n) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK:         %r = load i64, ptr %p2
; CHECK:         ret i64 %r
entry:
  %p2 = getelementptr inbounds i64, ptr %p, i64 2
  store i64 %v, ptr %p2
  call void @callee(ptr %p, i64 %n)
  %r = load i64, ptr %p2
  ret i64 %r
}

define void @callee(ptr %p, i64 %n) {
entry:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 %n, i1 false)
  ret void
}
