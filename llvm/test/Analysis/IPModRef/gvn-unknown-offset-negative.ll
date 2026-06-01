; SOUNDNESS (negative): foo writes arg0 at a variable index, so the write range
; is unknown. IPModRef must keep the argument's Overall=Mod and not narrow; the
; reload must NOT be folded.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i16 @test(ptr %p, i64 %i) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK:         %v = load i16, ptr %p2
; CHECK:         ret i16 %v
entry:
  %p2 = getelementptr inbounds i16, ptr %p, i64 2
  store i16 1, ptr %p2
  call void @foo(ptr %p, i64 %i)
  %v = load i16, ptr %p2
  ret i16 %v
}

; Writes arg0 at a runtime index - range unknown.
define void @foo(ptr %p, i64 %i) {
entry:
  %pi = getelementptr inbounds i16, ptr %p, i64 %i
  store i16 0, ptr %pi
  ret void
}
