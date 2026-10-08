.section ".itcm", "ax"

#include "AsmMacros.inc"
#include "VirtualMachine/VMDtcmDefs.inc"

.macro kotogba_detector_body exitLabel
    ldr r10,= gKotoGbaRuntimeEnabled
    ldr r10, [r10]
    cmp r10, #0
        beq \exitLabel

    // The first 2 MiB can appear either as original GBA addresses or through
    // GBARunner3's 0x02200000 linear DS mapping.
    cmp r8, #0x08000000
    ldrlo r10,= 0x023A9DCD
    ldrhs r10,= 0x081A9DCD
    subs r11, r8, r10
        bmi \exitLabel
    cmp r11, #0xED
        bhi \exitLabel

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
        bne \exitLabel
98:
    ldr r10,= gKotoGbaPendingCardId
    str r12, [r10]
.endm

/// @brief Hook used by MemoryLoad8.s, where one extra BL fits safely.
/// @param r8 Address being read; r10-r12 may be trashed by caller contract.
arm_func kotogba_detectIntroTextFromLoad8
    kotogba_detector_body 99f
99:
    bx lr

/// @brief Size-neutral trampoline for MemoryLoadRom.s.
/// Replaces its existing first LDR so the fixed 0x390-0x400 ITCM slot does not grow.
arm_func kotogba_detectIntroTextFromFixedRomLoad8
    kotogba_detector_body 97f
97:
    // Execute the instruction displaced from memu_load8Rom.
    ldr r11,= memu_adjustedRomBlockToCacheBlockAddress
    ldr r11, [r11]
    b kotogba_load8RomAfterDetector

.pool
