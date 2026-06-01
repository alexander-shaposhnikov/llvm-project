; Negative / soundness: same shape but %a is NOT noalias. Now the unknown
; (loaded) pointer access CAN alias %a, so it must poison %a — the reload at
; %a[16] is NOT folded (the call may modify it). This guards that the per-arg
; relaxation only fires for noalias args.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i64 @test(ptr %a, ptr %pp) {
; CHECK-LABEL: @test(
; CHECK:         call void @f(
; CHECK:         load i64
; CHECK:         ret i64 %d
entry:
  %p16 = getelementptr inbounds i8, ptr %a, i64 16
  store i64 7, ptr %p16
  %x = load i64, ptr %p16
  call void @f(ptr %a, ptr %pp)
  %y = load i64, ptr %p16
  %d = sub i64 %x, %y
  ret i64 %d
}

define void @f(ptr %a, ptr %pp) {
entry:
  store i64 1, ptr %a
  %q = load ptr, ptr %pp
  store i64 42, ptr %q
  ret void
}
