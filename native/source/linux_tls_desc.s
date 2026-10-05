// TLS descriptor resolver for variables in the static TLS area: the descriptor is {this function, offset};
// x0 holds the descriptor address and receives the offset from the thread pointer. Only x0 may change.
    .text
    .balign 16
    .global linuxTlsDescResolver
    .type linuxTlsDescResolver, %function
linuxTlsDescResolver:
    ldr x0, [x0, #8]
    ret
    .size linuxTlsDescResolver, . - linuxTlsDescResolver
