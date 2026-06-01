; Phase 2: foo calls bar(p), bar writes p[0]. Merging bar's summary into foo
; means foo writes p[0]; a reload of p[1] across foo folds. Today foo bails
; (it contains a call).
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr %p, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK-NOT:     load i64
; CHECK:         ret i64 %v
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %v, ptr %p1
  call void @foo(ptr %p)
  %r = load i64, ptr %p1
  ret i64 %r
}
define void @foo(ptr %p) { call void @bar(ptr %p) ret void }
define void @bar(ptr %p) { store i64 0, ptr %p ret void }
