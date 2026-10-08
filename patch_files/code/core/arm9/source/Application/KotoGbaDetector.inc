// Keep the fixed GBARunner3 ITCM slots tiny.
// The full detector lives in KotoGbaDetector.s after the fixed linker ranges.
.macro kotogba_detectIntroText
    bl kotogba_detectIntroTextFromLoad8
.endm
