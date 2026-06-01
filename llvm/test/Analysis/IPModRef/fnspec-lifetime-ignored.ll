; fnspec #1: lifetime/assume intrinsics have no observable effect on caller
; memory and must be ignored (not treated as unknown calls). The callee writes
; only p[0]; a store/reload of p[1] across it should fold.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @llvm.assume(i1)

define i64 @test(ptr %p, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64
; CHECK:         ret i64 %v
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %v, ptr %p1
  call void @callee(ptr %p)
  %r = load i64, ptr %p1
  ret i64 %r
}

; writes p[0] only; the assume must be ignored
define void @callee(ptr %p) {
entry:
  %c = icmp ne ptr %p, null
  call void @llvm.assume(i1 %c)
  store i64 0, ptr %p
  ret void
}
