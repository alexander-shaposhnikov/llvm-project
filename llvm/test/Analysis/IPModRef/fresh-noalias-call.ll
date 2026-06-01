; A callee that stores through a noalias allocation result (fresh memory) and
; writes p[0] should get a usable summary (just writes p[0]) -- the fresh-memory
; store has no caller-visible effect. A reload of p[1] then folds.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare noalias ptr @myalloc(i64) memory(inaccessiblemem: readwrite)

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
  %m = call noalias ptr @myalloc(i64 16)
  store i64 7, ptr %m         ; fresh memory -> no caller-visible effect
  store i64 0, ptr %p         ; writes p[0]
  ret void
}
