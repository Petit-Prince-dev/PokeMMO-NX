// One eight-byte stub per function a virtual library is asked for but does not implement: movz w16, #slot ; b linuxVirtualStubCommon.
// x16 is the intra-procedure-call scratch register: the caller's arguments, x30 and stack stay untouched until the common code.
    .text
    .balign 16
    .global linuxVirtualStubTable
    .type linuxVirtualStubTable, %function
linuxVirtualStubTable:
    .set stub_slot, 0
    .rept 1024
    mov w16, #stub_slot
    b linuxVirtualStubCommon
    .set stub_slot, stub_slot + 1
    .endr
    .size linuxVirtualStubTable, . - linuxVirtualStubTable

// linuxVirtualStubCalled(slot) returns 0 (NULL / false / no id), which is the stub's result for its caller. A frame is built so that
// the original link register survives the call.
    .type linuxVirtualStubCommon, %function
linuxVirtualStubCommon:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov w0, w16
    bl linuxVirtualStubCalled
    ldp x29, x30, [sp], #16
    ret
    .size linuxVirtualStubCommon, . - linuxVirtualStubCommon
