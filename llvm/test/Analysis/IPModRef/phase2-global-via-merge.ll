; Phase 2 + globals: bar writes p[0] AND a global; foo calls bar(p). With an
; alloca caller, a reload of p[1] folds (p[1] disjoint p[0]; global != alloca).
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

@g = global i64 0
define i64 @test(i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK-NOT:     load i64
; CHECK:         ret i64 %v
entry:
  %p = alloca [4 x i64]
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %v, ptr %p1
  call void @foo(ptr %p)
  %r = load i64, ptr %p1
  ret i64 %r
}
define void @foo(ptr %p) { call void @bar(ptr %p) ret void }
define void @bar(ptr %p) { store i64 0, ptr %p store i64 1, ptr @g ret void }
