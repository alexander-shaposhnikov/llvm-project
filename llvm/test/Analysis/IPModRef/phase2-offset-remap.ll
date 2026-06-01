; Phase 2 remap with offset: foo calls bar(p+1); bar writes its arg[0], so foo
; writes p[1]. A reload of p[2] folds (disjoint). Exercises ExtraOffset remap.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr %p, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK-NOT:     load i64
; CHECK:         ret i64 %v
entry:
  %p2 = getelementptr inbounds i64, ptr %p, i64 2
  store i64 %v, ptr %p2
  call void @foo(ptr %p)
  %r = load i64, ptr %p2
  ret i64 %r
}
define void @foo(ptr %p) { %q = getelementptr inbounds i64, ptr %p, i64 1
  call void @bar(ptr %q) ret void }
define void @bar(ptr %p) { store i64 0, ptr %p ret void }
