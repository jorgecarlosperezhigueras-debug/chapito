// kotoGba V0.5-A detector for Pocket Monsters FireRed JP Rev.1.
// I001-I005 begin inside one compact ROM window. When the emulated game reads
// the first byte of one of those strings, publish a numeric card id to DTCM.
// Rendering is deferred to VBlank so the hot memory-load path stays minimal.
.macro kotogba_detectIntroText
    ldr r10,= gKotoGbaRuntimeEnabled
    ldr r10, [r10]
    cmp r10, #0
        beq 99f

    // The first 2 MiB can appear either as original GBA addresses or through
    // GBARunner3's 0x02200000 linear DS mapping.
    cmp r8, #0x08000000
    ldrlo r10,= 0x023A9DCD
    ldrhs r10,= 0x081A9DCD
    subs r11, r8, r10
        bmi 99f
    cmp r11, #0xED
        bhi 99f

    cmp r11, #0x00
    moveq r12, #1
        beq 98f
    cmp r11, #0x4A
    moveq r12, #2
        beq 98f
    cmp r11, #0x7D
    moveq r12, #3
        beq 98f
    cmp r11, #0xD1
    moveq r12, #4
        beq 98f
    cmp r11, #0xED
    moveq r12, #5
        bne 99f
98:
    ldr r10,= gKotoGbaPendingCardId
    str r12, [r10]
99:
.endm
