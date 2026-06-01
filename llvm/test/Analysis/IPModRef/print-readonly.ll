; Analysis dump: a read-only function. Both args are Ref-only.
;
; RUN: opt < %s -disable-output -passes='print<ip-modref>' 2>&1 | FileCheck %s

; CHECK:      IPModRef summary for 'foo' (AccessesUnknown=0, Globals=NoModRef, Other=NoModRef)
; CHECK-NEXT:   arg 0: Ref RangesKnown=1
; CHECK-NEXT:     [0, 4) Ref
; CHECK-NEXT:   arg 1: Ref RangesKnown=1
; CHECK-NEXT:     [0, 4) Ref

define i32 @foo(ptr %p, ptr %q) {
entry:
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %s = add i32 %a, %b
  ret i32 %s
}
