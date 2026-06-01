; A callee that calls an argmem-read-only function (like strlen) on q and writes
; p[0] should still get a usable summary (reads q, writes p[0]) instead of
; bailing on the call. A reload of p[1] (noalias) then folds.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @ext_read(ptr) memory(argmem: read)

define i64 @test(ptr noalias %p, ptr noalias %q, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64, ptr %p1
; CHECK:         ret i64 %v
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 %v, ptr %p1
  call void @callee(ptr %p, ptr %q)
  %r = load i64, ptr %p1
  ret i64 %r
}
define void @callee(ptr %p, ptr %q) {
  call void @ext_read(ptr %q)
  store i64 0, ptr %p
  ret void
}
