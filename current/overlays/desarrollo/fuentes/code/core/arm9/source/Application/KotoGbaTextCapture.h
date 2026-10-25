#pragma once
#include "common.h"
enum { KOTO_TEXT_SLOTS=64, KOTO_TEXT_TOTAL_SLOTS=65, KOTO_TEXT_BYTES=240, KOTO_TEXT_SCRATCH=5444,
       KOTO_NATIVE_RENDER_BYTES=3072, KOTO_NATIVE_JP_OFFSET=1536 };
struct KotoTextRecord {
    u32 source,origin,tiles,sequence,context,cursor;
    u8 window,x,y,font,size,speed,ready,reserved;
    u8 bytes[KOTO_TEXT_BYTES];
};
// The byte count stays in u8; only the trailing capture payload grows in R30.
static_assert(KOTO_TEXT_BYTES<=255);
static_assert(sizeof(KotoTextRecord)==272&&__builtin_offsetof(KotoTextRecord,bytes)==32&&
              __builtin_offsetof(KotoTextRecord,size)==28);
extern KotoTextRecord *gKotoTextRecords;
extern volatile u32 gKotoTextGeneration,gKotoTextOverflow,gKotoTextCaptureInstalled;
extern "C" bool kotogba_installTextCapture(u32 gameCode,u8 revision);
extern "C" void kotogba_resetTextCapture();

extern unsigned gKotoTextCapacity;
// Reader-owned masks use the existing CorpusRef stride, without another
// capture allocation. Publication records actual PAGE/SCROLL observations.
enum {KOTO_TEXT_FRAGMENT_MASK_STRIDE=9};
extern u32* gKotoTextFragmentMasks;
extern "C" void kotogba_setupTextCapture(void* data,unsigned size);

extern "C" void* kotogba_textScratch();
extern "C" u32 kotogba_captureCursor(const KotoTextRecord* record,u32 cursor);
extern "C" u32 kotogba_variableSource(u32 origin,unsigned token,u32 context);
extern "C" u32 kotogba_variableOrigin(u32 origin,unsigned token,u32 context);
extern "C" unsigned kotogba_recapNumber(const KotoTextRecord* record);

extern volatile u32 gKotoTextWindowMask;
extern volatile u8 gKotoTextOptionsWindow,gKotoTextArrowSlot;

// Direct-rendered Help System panels have no ordinary guest Window IDs.
enum {KOTO_HELP_MAIN=32,KOTO_HELP_ANSWER,KOTO_HELP_DESCRIPTION,KOTO_HELP_CONTROLS,KOTO_HELP_TITLE,KOTO_SAVE_FAILED};
extern volatile u8 gKotoHelpArrowSlot;
// 1..8: Help controller; 9: separate save-failure controller owns the scene.
extern "C" unsigned kotogba_overlayState();

extern u8 gKotoTextLast[32],gKotoTextAsync[32];

// A constructed message retains its real ordered source pieces separately
// from its capture. Slot64 belongs to the reader's selected piece, never to
// the guest printer observer. Offsets are in the current captured view.
struct KotoCompositionPart {
    u32 origin;
    u16 rawStart,rawEnd,wordCode;
    u8 row,column;
};
static_assert(sizeof(KotoCompositionPart)==12);
extern "C" u32 kotogba_compositionVersion(const KotoTextRecord* record);
extern "C" unsigned kotogba_compositionVisibleBytes(const KotoTextRecord* record);
extern "C" bool kotogba_compositionComplete(const KotoTextRecord* record);
extern "C" unsigned kotogba_compositionCount(const KotoTextRecord* record);
extern "C" bool kotogba_compositionPart(const KotoTextRecord* record,unsigned index,KotoCompositionPart* part);
extern "C" bool kotogba_compositionAuthorize(const KotoTextRecord* record,u32 version,unsigned index,const KotoCompositionPart* part);
