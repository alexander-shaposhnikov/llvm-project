; SOUNDNESS (negative): foo writes p[2] (bytes [4,6)), which overlaps the
; reloaded location p[2]. The reload must NOT be folded - the call really does
; clobber it.
;
; RUN: opt < %s -aa-pipeline=basic-aa,ip-modref-aa \
; RUN:   -passes='require<ip-modref-aa>,gvn' -S | FileCheck %s

define i16 @test(ptr %p, ptr %q) {
; CHECK-LABEL: @test(
; CHECK:         call void @foo(
; CHECK:         %v = load i16, ptr %p2
; CHECK:         ret i16 %v
entry:
  %p2 = getelementptr inbounds i16, ptr %p, i64 2
  store i16 1, ptr %p2
  call void @foo(ptr %p, ptr %q)
  %v = load i16, ptr %p2
  ret i16 %v
}

; Writes arg0 at index 2 (bytes [4,6)) - overlaps p[2].
define void @foo(ptr %p, ptr %q) {
entry:
  %p2 = getelementptr inbounds i16, ptr %p, i64 2
  store i16 0, ptr %p2
  ret void
}
