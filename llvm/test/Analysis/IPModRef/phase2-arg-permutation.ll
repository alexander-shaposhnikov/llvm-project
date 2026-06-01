; Phase 2 arg remap: foo(a,b) calls bar(b); bar writes its arg. So foo writes
; only b. A store/reload through noalias a folds (a untouched).
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr noalias %a, ptr noalias %b, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK-NOT:     load i64, ptr %a
; CHECK:         ret i64 %v
entry:
  store i64 %v, ptr %a
  call void @foo(ptr %a, ptr %b)
  %r = load i64, ptr %a
  ret i64 %r
}
define void @foo(ptr %a, ptr %b) { call void @bar(ptr %b) ret void }
define void @bar(ptr %p) { store i64 0, ptr %p ret void }
