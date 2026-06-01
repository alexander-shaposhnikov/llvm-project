; Analysis dump: a function that writes a global directly is NOT a bail —
; its arg ranges stay precise and the global is tracked separately (Globals=Mod).
; An access through a truly-unknown (loaded) pointer no longer poisons the whole
; summary (AccessesUnknown stays 0): it poisons only the non-noalias arg(s) and
; records an Other=Mod residue (idea #1, per-arg unknown handling).
;
; RUN: opt < %s -disable-output -passes='print<ip-modref>' 2>&1 | FileCheck %s

@g = global i64 0

; CHECK: IPModRef summary for 'global_and_arg' (AccessesUnknown=0, Globals=Mod, Other=NoModRef)
; CHECK-NEXT:   arg 0: Mod RangesKnown=1
; CHECK-NEXT:     [8, 16) Mod
define void @global_and_arg(ptr %p) {
entry:
  %p1 = getelementptr inbounds i64, ptr %p, i64 1
  store i64 0, ptr %p1
  store i64 3, ptr @g
  ret void
}

; The non-noalias arg %pp is read (load) then poisoned by the unknown store; the
; residue is recorded as Other=Mod, but AccessesUnknown stays 0.
; CHECK: IPModRef summary for 'unknown_ptr' (AccessesUnknown=0, Globals=NoModRef, Other=Mod)
; CHECK-NEXT:   arg 0: ModRef RangesKnown=0
define void @unknown_ptr(ptr %pp) {
entry:
  %q = load ptr, ptr %pp     ; loaded pointer of unknown provenance
  store i64 1, ptr %q        ; store through it -> per-arg poison + Other=Mod
  ret void
}
