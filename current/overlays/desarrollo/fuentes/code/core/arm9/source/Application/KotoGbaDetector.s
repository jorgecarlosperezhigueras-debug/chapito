.section ".itcm", "ax"

#include "AsmMacros.inc"
#include "VirtualMachine/VMDtcmDefs.inc"
#include "MemoryEmulator/RomDefs.h"

.macro kotogba_detector_body exitLabel
    ldr r10,= gKotoGbaRuntimeEnabled
    ldr r10, [r10]
    cmp r10, #0
        beq \exitLabel
    push {r8,lr}
    cmp r8, #0x08000000
    sublo r8, r8, #ROM_LINEAR_DS_ADDRESS
    subhs r8, r8, #ROM_LINEAR_GBA_ADDRESS
    ldr r10,= gKotoGbaRecognizers
    ldr r11,= gKotoGbaRecognizersEnd
    ldr r9, [r10]
    cmp r8, r9
        blo 96f
    ldr r9, [r11, #-8]
    cmp r8, r9
        bhi 96f
95:
    cmp r10, r11
        bhs 96f
    sub r12, r11, r10
    mov r12, r12, lsr #4
    add r12, r10, r12, lsl #3
    ldr r9, [r12]
    cmp r8, r9
        addhi r10, r12, #8
        movlo r11, r12
        bne 95b
    ldr r9, [r12, #4]
    ldr r10,= gKotoGbaPendingCardId
    str r9, [r10]
96:
    pop {r8,lr}
.endm

/// @brief Size-neutral linear-memory trampoline; both jumps preserve caller LR.
/// @param r8 Address being read; r10-r12 may be trashed by caller contract.
arm_func kotogba_detectIntroTextFromLoad8
    kotogba_detector_body 99f
99:
    // Execute the CMP displaced from memu_load8Ewram, including its flags.
    cmp r8, #ROM_LINEAR_END_DS_ADDRESS
    b kotogba_load8EwramAfterDetector

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
