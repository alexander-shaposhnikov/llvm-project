; A variable-offset access into arg a (a[i]) should mark only arg a imprecise,
; not poison the whole summary -- so a precise write to b[5] stays precise and a
; reload of b[6] (noalias) folds. Today a[i] -> AccessesUnknown -> no fold.
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr noalias %a, ptr noalias %b, i64 %i, i64 %v) {
; CHECK-LABEL: @test(
; CHECK:         call void @callee(
; CHECK-NOT:     load i64, ptr %b6
; CHECK:         ret i64 %v
entry:
  %b6 = getelementptr inbounds i64, ptr %b, i64 6
  store i64 %v, ptr %b6
  call void @callee(ptr %a, ptr %b, i64 %i)
  %r = load i64, ptr %b6
  ret i64 %r
}
define void @callee(ptr %a, ptr %b, i64 %i) {
  %ai = getelementptr inbounds i64, ptr %a, i64 %i
  store i64 0, ptr %ai               ; a[i] -> arg a imprecise
  %b5 = getelementptr inbounds i64, ptr %b, i64 5
  store i64 1, ptr %b5               ; b[5] precise
  ret void
}
