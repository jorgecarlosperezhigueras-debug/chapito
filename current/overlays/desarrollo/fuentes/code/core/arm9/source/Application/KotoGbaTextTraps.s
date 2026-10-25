#include "AsmMacros.inc"
.section ".ewram.bss", "aw", %nobits
.balign 8
.global gKotoTextTrapRegisters
gKotoTextTrapRegisters:
    .space 72
.global gKotoTextTrapStack
.global gKotoTextTrapStackGuardLow
gKotoTextTrapStack:
gKotoTextTrapStackGuardLow:
    .space 4
    .space 1024 - 4
.global gKotoTextTrapStackEnd
.global gKotoTextTrapStackGuardHigh
gKotoTextTrapStackEnd:
gKotoTextTrapStackGuardHigh:
    .space 4
    .space 4
.global gKotoTextTrapStackAllocationEnd
gKotoTextTrapStackAllocationEnd:

.section ".text", "ax"
// Keep the bridge outside EWRAM: region 1 is non-executable on entry
// and again after restoring MPU permissions. Share its body to fit VRAM A.
.macro observe kind
    ldr r13,=gKotoTextTrapRegisters
    stmia r13!,{r0-r12,lr}
    mov r1,#\kind
    bl kotoObserveCommon
.endm

arm_func kotoObserveCommon
    mov r4,lr // native continuation; guest r4 is already in the snapshot
    mov r5,r1 // observer kind; callee-saved across native calls
    mrs r0,spsr
    str r0,[r13],#4
    stmia r13,{r13,r14}^
    nop
    mrc p15,0,r0,c5,c0,3
    str r0,[r13,#8]
    bic r1,r0,#0xf0
    orr r1,r1,#0x10
    mcr p15,0,r1,c5,c0,3
    ldr sp,=gKotoTextTrapStackEnd
    push {r4,r5}
    bl vm_enableNestedIrqs
    ldr r0,=gKotoTextTrapRegisters
    ldr r1,[sp,#4]
    bl kotogba_observeText
    bl vm_disableNestedIrqs
    pop {r4,r5}
    ldr r13,=gKotoTextTrapRegisters
    ldr r0,[r13,#68]
    mcr p15,0,r0,c5,c0,3
    ldr r0,[r13,#56]
    msr spsr_cxsf,r0
    mov lr,r4
    ldmia r13,{r0-r12}
    mov r13,lr
    ldr lr,=gKotoTextTrapRegisters
    ldr lr,[lr,#52]
    bx r13

arm_func kotoFlagsReturn
    mrs r13,cpsr
    and r13,r13,#0xf0000000
    ldr lr,=gKotoTextTrapRegisters
    ldr lr,[lr,#56]
    bic lr,lr,#0xf0000000
    orr lr,lr,r13
    msr spsr_cxsf,lr
    ldr lr,=gKotoTextTrapRegisters
    ldr lr,[lr,#52]
    movs pc,lr

arm_func kotogba_textTrap
    observe 0
    mov r12,r2 // displaced 0x4694; no flags changed
    movs pc,lr
arm_func kotogba_expandTrap
    observe 1
    adds r5,r1,#0 // displaced Thumb ADDS, including NZCV
    b kotoFlagsReturn
arm_func kotogba_expandDoneTrap
    observe 7
    adds r0,r4,#0 // displaced Thumb ADDS, including NZCV
    b kotoFlagsReturn
arm_func kotogba_publishTrap
    observe 2
    // Displaced ADD SP,#12 updates the virtual user bank, not SVC SP.
    ldr r13,=gKotoTextTrapRegisters+60
    ldr lr,[r13]
    add lr,lr,#12
    str lr,[r13]
    ldmia r13,{r13}^
    nop
    ldr lr,=gKotoTextTrapRegisters
    ldr lr,[lr,#52]
    movs pc,lr
arm_func kotogba_clearTrap
    observe 3
    ldr r5,=0x02020430
    movs pc,lr
arm_func kotogba_removeTrap
    observe 4
    ldr r1,=0x02020430
    movs pc,lr
arm_func kotogba_rectTrap
    observe 5
    mov r9,r3
    movs pc,lr
arm_func kotogba_windowsTrap
    observe 6
    mov r7,r10
    movs pc,lr
.pool

// R14: preserve source identity through StringCopy into temporary UI buffers.
// Both displaced instructions are Thumb ADDS and must restore guest NZCV.
.macro copy_return
    b kotoFlagsReturn
.endm
arm_func kotogba_copyTrap
    observe 8
    adds r3,r0,#0
    copy_return
arm_func kotogba_copyDoneTrap
    observe 9
    adds r0,r3,#0
    copy_return

// BattleStringExpandPlaceholdersToBuffer has its own source/token namespace.
arm_func kotogba_battleTrap
    observe 10
    mov r9,r0 // displaced high-register MOV preserves guest NZCV
    movs pc,lr
arm_func kotogba_battleVariableTrap
    observe 11
    adds r0,r1,#0
    copy_return
arm_func kotogba_battleDoneTrap
    observe 12
    adds r0,r6,#0
    copy_return

// F7 has a separate eight-slot pointer array, not StringExpand's FD namespace.
arm_func kotogba_dynamicTrap
    observe 13
    adds r2,r0,#0
    b kotoFlagsReturn
arm_func kotogba_dynamicVariableTrap
    observe 14
    cmp r1,#0
    b kotoFlagsReturn
arm_func kotogba_dynamicDoneTrap
    observe 15
    adds r0,r2,#0
    b kotoFlagsReturn

// StringCopyN is bounded and deliberately does not append EOS.
arm_func kotogba_copyNTrap
    observe 16
    adds r4,r0,#0
    b kotoFlagsReturn
arm_func kotogba_copyNDoneTrap
    observe 17
    adds r0,r4,r5
    b kotoFlagsReturn
// STR r7,[SP] acts on the virtual user stack, not the native observer stack.
arm_func kotogba_categoryTrap
    observe 18
    ldr r13,=gKotoTextTrapRegisters+60
    ldr r13,[r13]
    str r7,[r13]
    ldr lr,=gKotoTextTrapRegisters
    ldr lr,[lr,#52]
    movs pc,lr

// Ordered EasyChat constructor bounds. Input words and the completed output
// are witnessed together, not inferred from matching displayed substrings.
arm_func kotogba_easyChatTrap
    observe 19
    adds r4,r0,#0
    b kotoFlagsReturn
arm_func kotogba_easyChatDoneTrap
    observe 20
    adds r0,r4,#0
    b kotoFlagsReturn

// BPRJ1's alternate Help System renderer and its actual buffer lifecycle.
arm_func kotogba_helpTextTrap
    observe 21
    mov r9,r2
    movs pc,lr
arm_func kotogba_helpPublishTrap
    observe 22
    ldr r0,=0x0201c000
    movs pc,lr
arm_func kotogba_helpClearTrap
    observe 23
    ldr r1,=gKotoTextTrapRegisters
    ldr r1,[r1,#60] // displaced MOV r1,SP reads the guest user-bank stack
    movs pc,lr
arm_func kotogba_helpBeginTrap
    observe 24
    ldr r0,=0x0600c000
    movs pc,lr
arm_func kotogba_helpQuestionTrap
    observe 25
    mov r8,r0
    movs pc,lr
arm_func kotogba_helpPcNameTrap
    observe 26
    cmp r0,#1
    b kotoFlagsReturn

// The exceptional save diagnostic queues a distinct character-buffer DMA.
arm_func kotogba_saveFailedPublishTrap
    observe 27
    ldr r0,=0x0201c020
    movs pc,lr

// BPRJ1 GetSpeciesName uses its own byte loop rather than StringCopy.
arm_func kotogba_speciesDoneTrap
    observe 28
    adds r1,r6,r1
    b kotoFlagsReturn
