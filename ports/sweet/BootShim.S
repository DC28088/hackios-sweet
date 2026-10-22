/* ARM64 image header and Sweet UEFI/XNUP relocation, based on Mu BootShim (APSL kernel repository). */
.text
.global _start
_start:
 adr x1, _Payload
 b _Run
 .quad 0x9fc00000
 .quad 0x00300000
 .quad 0
 .quad 0
 .quad 0
 .quad 0
 .ascii "ARM\x64"
 .long 0
_Run:
 mov x4, x1
 ldr x5, =0x9fc00000
 ldr x6, =0x00300000
 cmp x4, x5
 b.eq _Same
_CopyFD:
 ldp x2, x3, [x4], #16
 stp x2, x3, [x5], #16
 subs x6, x6, #16
 b.ne _CopyFD
 b _XNUP
_Same:
 add x4, x4, x6
_XNUP:
 ldr w2, [x4]
 ldr w3, =0x50554e58
 cmp w2, w3
 b.ne _Enter
 ldr w6, [x4, #4]
 ldr x7, =0x03fff000
 cmp x6, x7
 b.hi _Enter
 add x6, x6, #23
 and x6, x6, #-16
 ldr x5, =0xf0000000
_CopyPayload:
 ldp x2, x3, [x4], #16
 stp x2, x3, [x5], #16
 subs x6, x6, #16
 b.ne _CopyPayload
 dsb sy
_Enter:
 ldr x5, =0x9fc00000
 br x5
.ltorg
.balign 16
_Payload:
