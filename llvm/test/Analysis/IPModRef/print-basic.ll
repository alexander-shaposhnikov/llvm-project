; Analysis dump: foo writes arg0 at [2,4) and reads arg1 at [4,6).
;
; RUN: opt < %s -disable-output -passes='print<ip-modref>' 2>&1 | FileCheck %s

; CHECK:      IPModRef summary for 'foo' (AccessesUnknown=0, Globals=NoModRef, Other=NoModRef)
; CHECK-NEXT:   arg 0: Mod RangesKnown=1
; CHECK-NEXT:     [2, 4) Mod
; CHECK-NEXT:   arg 1: Ref RangesKnown=1
; CHECK-NEXT:     [4, 6) Ref

define void @foo(ptr %p, ptr %q) {
entry:
  %p1 = getelementptr inbounds i16, ptr %p, i64 1
  %q2 = getelementptr inbounds i16, ptr %q, i64 2
  %t = load i16, ptr %q2
  store i16 %t, ptr %p1
  ret void
}
