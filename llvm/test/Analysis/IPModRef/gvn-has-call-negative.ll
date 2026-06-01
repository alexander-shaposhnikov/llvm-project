; SOUNDNESS (negative, Phase 1 limitation): foo writes only p[1] (disjoint from
; p[2]) but also contains a call to an unknown function. In Phase 1 IPModRef
; cannot characterize callee effects, so it sets AccessesUnknown and contributes
; nothing; the reload is NOT folded. Phase 2 (callee-summary merging) will fold
; this when @bar is itself analyzable.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

declare void @bar()

define i16 @test(ptr %p) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK:         %v = load i16, ptr %p2
; CHECK:         ret i16 %v
entry:
  %p2 = getelementptr inbounds i16, ptr %p, i64 2
  store i16 1, ptr %p2
  call void @foo(ptr %p)
  %v = load i16, ptr %p2
  ret i16 %v
}

define void @foo(ptr %p) {
entry:
  %p1 = getelementptr inbounds i16, ptr %p, i64 1
  store i16 0, ptr %p1
  call void @bar()
  ret void
}
