; A store through select(a,b) resolves (via getUnderlyingObjects) to {a,b}, so
; only a and b are marked imprecise; a precise write to c[5] stays precise and a
; reload of c[6] (noalias) folds. Today the select -> AccessesUnknown -> no fold.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr noalias %a, ptr noalias %b, ptr noalias %c, i1 %cond, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64, ptr %c6
; CHECK:         ret i64 %v
entry:
  %c6 = getelementptr inbounds i64, ptr %c, i64 6
  store i64 %v, ptr %c6
  call void @callee(ptr %a, ptr %b, ptr %c, i1 %cond)
  %r = load i64, ptr %c6
  ret i64 %r
}
define void @callee(ptr %a, ptr %b, ptr %c, i1 %cond) {
  %p = select i1 %cond, ptr %a, ptr %b
  store i64 0, ptr %p                ; a or b -> both imprecise
  %c5 = getelementptr inbounds i64, ptr %c, i64 5
  store i64 1, ptr %c5               ; c[5] precise
  ret void
}
