; SOUNDNESS (negative): foo writes a global (memory not attributable to any
; argument). In Phase 1, IPModRef sets AccessesUnknown and contributes nothing,
; so the reload is NOT folded by IPModRef. (GlobalsAA could refine this, but it
; is not in this pipeline.)
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

@g = global i16 0

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

; Writes p[1] (disjoint from p[2]) but also writes a global.
define void @foo(ptr %p) {
entry:
  %p1 = getelementptr inbounds i16, ptr %p, i64 1
  store i16 0, ptr %p1
  store i16 3, ptr @g
  ret void
}
