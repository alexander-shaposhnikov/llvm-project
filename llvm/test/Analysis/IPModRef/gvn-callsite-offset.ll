; Call-site offset arithmetic: foo writes arg0 at byte offset 2. The call passes
; (p+2 bytes) as arg0, so the real write is at p bytes [4,6). A store/reload of
; p bytes [8,10) is disjoint and must fold.
;
; This exercises stripAndAccumulateConstantOffsets delta handling.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i16 @test(ptr %p) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK-NOT:     load i16
; CHECK:         ret i16 5
entry:
  %p4 = getelementptr inbounds i8, ptr %p, i64 8
  store i16 5, ptr %p4
  %argp = getelementptr inbounds i8, ptr %p, i64 2
  call void @foo(ptr %argp)
  %v = load i16, ptr %p4
  ret i16 %v
}

; Writes arg0 at byte offset 2 only.
define void @foo(ptr %p) {
entry:
  %p1 = getelementptr inbounds i16, ptr %p, i64 1
  store i16 0, ptr %p1
  ret void
}
