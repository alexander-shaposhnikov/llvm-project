; Per-arg unknown (idea #1, sound noalias slice): callee @f writes its noalias
; arg %a[0,8) precisely AND performs an access through an unknown (loaded)
; pointer. The unknown access cannot touch the *noalias* arg %a (it is not
; derived from %a), so %a's access ranges stay precise: a caller store at
; %a[16] survives the call and the reload folds to 0. Previously the unknown
; access poisoned the whole summary and the reload was kept.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr noalias %a, ptr %pp) {
; CHECK-LABEL: @test(
; CHECK:         call void @f(
; CHECK:         ret i64 0
entry:
  %p16 = getelementptr inbounds i8, ptr %a, i64 16
  store i64 7, ptr %p16
  %x = load i64, ptr %p16
  call void @f(ptr %a, ptr %pp)
  %y = load i64, ptr %p16
  %d = sub i64 %x, %y
  ret i64 %d
}

define void @f(ptr noalias %a, ptr %pp) {
entry:
  store i64 1, ptr %a            ; precise: writes %a[0,8)
  %q = load ptr, ptr %pp         ; %q is an unknown (loaded) pointer
  store i64 42, ptr %q           ; access through unknown pointer
  ret void
}
