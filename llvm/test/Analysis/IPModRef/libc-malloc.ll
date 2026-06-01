; malloc-like (inaccessible-only): no effect on caller args/globals. The callee's
; summary is just "writes p[0]"; a reload of p[1] folds.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare ptr @ext_alloc(i64) memory(inaccessiblemem: readwrite)

define i64 @test(ptr %p, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64, ptr %p1
; CHECK:         ret i64 %v
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %v, ptr %p1
  call void @callee(ptr %p)
  %r = load i64, ptr %p1
  ret i64 %r
}
define void @callee(ptr %p) {
  %m = call ptr @ext_alloc(i64 8)
  store i64 0, ptr %p
  ret void
}
