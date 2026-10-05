// One eight-byte stub per import without an adapter: movz w16, #index ; b linuxTrapCommon.
// x16 is the intra-procedure-call scratch register: a caller cannot rely on it across a call, so
// the caller's x0-x7, x30 and stack stay untouched until linuxTrapCommon forwards them.
    .text
    .balign 16
    .global linuxTrapStubs
    .type linuxTrapStubs, %function
linuxTrapStubs:
    .set trap_index, 0
    .rept 512
    mov w16, #trap_index
    b linuxTrapCommon
    .set trap_index, trap_index + 1
    .endr
    .size linuxTrapStubs, . - linuxTrapStubs

// linuxTrapReport(index, caller) returns the refusal value in x0 (or never returns). A frame is built
// so the original link register survives the call and the stub returns to the client's caller.
    .type linuxTrapCommon, %function
linuxTrapCommon:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov w0, w16
    mov x1, x30
    bl linuxTrapReport
    ldp x29, x30, [sp], #16
    ret
    .size linuxTrapCommon, . - linuxTrapCommon
