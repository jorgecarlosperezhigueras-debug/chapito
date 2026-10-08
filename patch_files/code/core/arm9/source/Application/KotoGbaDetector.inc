// kotoGba V0.5-A detector hooks.
// MemoryLoadRom occupies an exact fixed ITCM slot, so its hook must replace
// an existing instruction rather than add bytes.
.macro kotogba_detectIntroText
    bl kotogba_detectIntroTextFromLoad8
.endm

.macro kotogba_detectIntroTextFixedRom
    b kotogba_detectIntroTextFromFixedRomLoad8
.endm
