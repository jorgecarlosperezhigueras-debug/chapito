#include "common.h"
#include <libtwl/gfx/gfx.h>
#include <libtwl/gfx/gfxBackground.h>
#include <libtwl/ipc/ipcFifo.h>
#include <libtwl/ipc/ipcFifoSystem.h>
#include "cp15.h"
#include "MemCopy.h"
#include "KotoGbaRuntimeService.h"
#include "KotoGbaBootDiag.h"
#include "KotoGbaLatinFont6x10.h"
#include "Fat/ff.h"
#include "Fat/FsIpc.h"
#include "KotoGbaPages.generated.h"
#include "KotoGbaTextCapture.h"
#include "SdCache/SdCache.h"
#include "KotoGbaNativeLabels.generated.h"
#include "KotoGbaPatterns.generated.h"
#include "KotoGbaTapMap.generated.h"
#include "KotoGbaLinguisticApi.h"
#include <string.h>

#pragma GCC optimize ("Os", "no-ipa-ra") // Thumb BX epilogues may clobber r1.
#pragma GCC target ("thumb") // ARMv4T build: explicit BX returns under CP15.L4.

extern volatile u32 gKanjiLoadStatus;
static bool KOpenGlyph(u32 cp);
static void InitNativeSceneStorage();
static const u8* NativeCurrentText(const KotoTextRecord& record,unsigned& size);
static const u8* KGlyphPixels(u32 cp);
static bool ToggleCurrentWord();
static unsigned CListCount(unsigned tab);
static const char* CListTitle(unsigned tab,unsigned index);
static u32 CListGlyph(unsigned tab,unsigned index);
static void COpenWord(unsigned index);
static bool COpenKanji(unsigned index);
static bool CSaving();
static void CBeginWordLabel(unsigned index);
static bool CWordLabelUnit();
static const char* CWordLabel(bool spanish);
static u32 DecodeUtf8(const char*& text);
static bool AudioPending();
static unsigned Scanline();
extern "C" unsigned gKotoGbaReaderWindowEnd;
static unsigned CRowHeight(unsigned tab){return tab==1?38:22;}

#define KOTOGBA_SCREEN_WIDTH  256
#define KOTOGBA_SCREEN_HEIGHT 192
#define KOTOGBA_GLYPH_WIDTH   6
#define KOTOGBA_GLYPH_HEIGHT  10
#define KOTOGBA_LINE_HEIGHT   14
#define KOTOGBA_COLOR_BG      (0x8000u | 2u | (2u << 5) | (3u << 10))
#define KOTOGBA_COLOR_PANEL   (0x8000u | 5u | (5u << 5) | (7u << 10))
#define KOTOGBA_COLOR_WHITE   0xFFFFu
#define KOTOGBA_COLOR_MUTED   (0x8000u | 20u | (20u << 5) | (20u << 10))
#define KOTOGBA_COLOR_RED     (0x8000u | 31u)

[[gnu::section(".ewram.bss")]] KotoGbaRuntimeService gKotoGbaRuntimeService;

[[gnu::section(".dtcm")]]
volatile u32 gKotoGbaRuntimeEnabled = 0;
[[gnu::section(".dtcm")]]
volatile u32 gKotoGbaPendingCardId = 0;
[[gnu::section(".dtcm")]]
static u32 sKotoGbaActiveCardId = 0;

// ARM7 publishes one atomic input word in its own aligned cache line.
// X/Y are not GBA keys; touch therefore never consumes game controls.
[[gnu::section(".ewram.bss")]] alignas(32) static volatile u32 sStudyInput[8];
[[gnu::section(".dtcm")]] static u32 sPreviousInput = 0;
// ARM7 retains the latest pen-down even if it ended while ARM9 was yielding.
[[gnu::section(".ewram.bss")]] static u32 sReaderTouchSequence = 0;
[[gnu::section(".dtcm")]] static u8 sStudyTab = 0;
[[gnu::section(".dtcm")]] static u8 sStudyPage[3] = {};
[[gnu::section(".dtcm")]] static u8 sGameMenu=0;
[[gnu::section(".dtcm")]] static int sScroll=0, sDragY=-1;
[[gnu::section(".dtcm")]] static bool sTapTracking=false, sTapMoved=false;
[[gnu::section(".dtcm")]] static u8 sTapTab=0, sModalKind=0, sModalStatus=0;
[[gnu::section(".dtcm")]] static u16 sTapX=0,sTapY=0,sModalPage=0,sModalFirst=0,sModalCount=0,sModalOrdinal=0;
[[gnu::section(".dtcm")]] static u16 sModalBaseY=0;
[[gnu::section(".dtcm")]] static u16 sModalLeft=12,sModalWidth=232,sModalTop=26,sModalBottom=168;
[[gnu::section(".dtcm")]] static int sModalScroll=0;
[[gnu::section(".ewram.bss")]] static u32 sModalCard=0,sShownModalCard=0;
[[gnu::section(".ewram.bss")]] static int sModalHeight=0;
[[gnu::section(".ewram.bss")]] static int sDisplayedModalScroll=0;
[[gnu::section(".ewram.bss")]] static int sTapModalScroll=0;
[[gnu::section(".dtcm")]] static bool sModalViewReady=false;
[[gnu::section(".dtcm")]] static bool sTapModalReady=false;
[[gnu::section(".ewram.bss")]] static bool sShownSaving=false;
[[gnu::section(".dtcm")]] static u16 sReaderTick=0;
[[gnu::section(".dtcm")]] static const char* sModalBody=nullptr;
#include "KotoGbaScroll.inc"
// V0.5F: bounded external content, loaded once before entering the VM.
// Allow the existing dictionary pilot without replacing validated D23 content.
// R31 retains all64 physical captures and adds one reserved, proven component
// view plus room for their secondary routes. The linker enforces64KiB heap;
// the cache budget is measured after all runtime storage has been reserved.
[[gnu::section(".ewram.bss")]] alignas(4) static u8 sStudyData[201*1024+1284];
// Popup widths are bounded by 248 pixels. Measure validated lines once, before
// entering the game, instead of decoding whole glossary entries inside an IRQ.
[[gnu::section(".ewram.bss")]] static u8* sStudyLineWidths;
static_assert(sizeof(sKotoTapAnchors)/sizeof(sKotoTapAnchors[0])<255,
              "Legacy anchor links reserve 255 as their empty sentinel");
[[gnu::section(".ewram.bss")]] static u8 *sLegacyAnchorHeads,*sLegacyAnchorNext;
[[gnu::section(".ewram.bss")]] static u32 sTouchOffset=0;
struct SavedWord { u16 card, ordinal; };
static u32 InlineMemoryKey(u32 card,u16 ordinal);
static u32 CFrozenIdentity(u16 slot);
extern volatile u32 gCollectionsLoadStatus;
[[gnu::section(".ewram.bss")]] alignas(4) static SavedWord sSavedWords[512];
[[gnu::section(".ewram.bss")]] static u16 sSavedCount=0;
[[gnu::section(".ewram.bss")]] static u32 sSavedGeneration=0;
[[gnu::section(".ewram.bss")]] static u8 sSavedSlot=0;
[[gnu::section(".dtcm")]] static bool sStudyReady = false;
[[gnu::section(".dtcm")]] volatile u32 gStudyLoadStatus = 0;
[[gnu::always_inline]] static inline u32 Read32(u32 off) { return *(const u32*)(sStudyData+off); }
[[gnu::always_inline]] static inline u16 Read16(u32 off) { return sStudyData[off] | (sStudyData[off+1] << 8); }
[[gnu::section(".ewram.bss")]] static const u8* sCachedStudy;
[[gnu::section(".ewram.bss")]] static u32 sCachedStudyId;
static const u8* FindStudy(u32 id)
{
    if (!sStudyReady||!id||(id&0x80000000u)) return nullptr;
    if(sCachedStudyId==id)return sCachedStudy;
    // Released packages use dense IDs. Avoid two full 256-card scans every
    // frame when primary dialogue and a secondary page alternate lookups.
    if(id<=Read32(16)){
        const u8* direct=sStudyData+Read32(32)+16*(id-1);
        if(*(const u32*)direct==id){sCachedStudyId=id;return sCachedStudy=direct;}
    }
    const u8* end=sStudyData+Read32(32)+16*Read32(16);
    for(const u8* card=sStudyData+Read32(32);card<end;card+=16) {
        if(*(const u32*)card==id) {sCachedStudyId=id;return sCachedStudy=card;}
    }
    sCachedStudyId=id;sCachedStudy=nullptr;return nullptr;
}
[[gnu::always_inline]] static inline u16 Card16(const u8* card, u32 off) { return card[off] | (card[off+1]<<8); }
struct SavedHeader { char magic[8];u32 generation;u16 count,reserved;u32 hash; };
static constexpr char kSavedMagic[8]={'K','O','T','O','W','D','1','\0'};
static u32 SavedHash(const SavedHeader& h,const SavedWord* words)
{
    u32 v=2166136261u;const u8* p=(const u8*)&h;
    for(unsigned i=0;i<offsetof(SavedHeader,hash);++i)v=(v^p[i])*16777619u;
    p=(const u8*)words;for(unsigned i=0;i<h.count*sizeof(SavedWord);++i)v=(v^p[i])*16777619u;
    return v;
}
// FatFs is ARMv5 ARM code: keep its caller ARM while L4 is enabled.
// This ARMv4T bridge itself returns to Thumb with BX.
[[gnu::target("arm"),gnu::noinline]] static bool ReadSavedSlot(unsigned slot,SavedHeader& h,SavedWord* words)
{
    FIL f;UINT got=0;const char* path=slot?"/_gba/kotogba/saved-words-b.bin":"/_gba/kotogba/saved-words-a.bin";
    if(f_open(&f,path,FA_READ)!=FR_OK)return false;
    const bool ok=f_size(&f)>=sizeof(SavedHeader) && f_size(&f)<=sizeof(SavedHeader)+128*sizeof(SavedWord) &&
        f_read(&f,&h,sizeof(SavedHeader),&got)==FR_OK && got==sizeof(SavedHeader) &&
        h.count<=128 && f_read(&f,words,h.count*sizeof(SavedWord),&got)==FR_OK && got==h.count*sizeof(SavedWord);
    f_close(&f);
    return ok && !memcmp(h.magic,kSavedMagic,sizeof(kSavedMagic)) && h.hash==SavedHash(h,words);
}
static void LoadSavedWords()
{
    SavedHeader a{},b{};SavedWord wa[128]{},wb[128]{};
    const bool va=ReadSavedSlot(0,a,wa),vb=ReadSavedSlot(1,b,wb);
    if(!va && !vb){sSavedCount=0;sSavedGeneration=0;sSavedSlot=0;return;}
    const bool takeB=vb && (!va || (s32)(b.generation-a.generation)>0);
    const SavedHeader& h=takeB?b:a;SavedWord* src=takeB?wb:wa;
    memcpy(sSavedWords,src,h.count*sizeof(SavedWord));sSavedCount=h.count;
    sSavedGeneration=h.generation;sSavedSlot=takeB?1:0;
}
#include "KotoGbaMemory.inc"
static bool IsWordSaved(u32 card,u16 ordinal)
{
    if(!sSavedCount)return false;
    const u32 key=(card<<16)|ordinal;
    // Ephemeral frozen modals share a virtual reference, but their identities differ.
    if((card!=0x7ffe||ordinal!=65535)&&sSavedCheckCount==sSavedCount&&sSavedCheckKey==key)return sSavedCheckResult;
    const u32 identity=MemoryWordKey(card,ordinal);
    sSavedCheckKey=key;sSavedCheckCount=sSavedCount;sSavedCheckResult=false;
    for(unsigned i=0;i<sSavedCount;++i)if((sSavedWords[i].card==card && sSavedWords[i].ordinal==ordinal)||(identity&&sSavedMemory[i]==identity)){sSavedCheckResult=true;break;}
    return sSavedCheckResult;
}
// FatFs is ARMv5 code with POP PC returns. Under CP15.L4 every direct
// caller must remain ARM; only this ARMv4T bridge returns to Thumb via BX.
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]]
static bool BootRead(FIL& file,void* destination,u32 bytes,u32 stage)
{
    u8* out=(u8*)destination;
    for(u32 offset=0;offset<bytes;){
        const UINT count=bytes-offset>4096?4096:bytes-offset;UINT got=0;
        kotogba_bootStep(stage,offset);
        const FRESULT result=f_read(&file,out+offset,count,&got);
        if(result!=FR_OK||got!=count){kotogba_bootStep(47,(result<<24)|got);return false;}
        offset+=got;
    }
    kotogba_bootStep(stage,bytes);return true;
}
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]]
static u32 LoadStudyFile()
{
    FIL file;kotogba_bootStep(32,0);
    const FRESULT opened=f_open(&file,"/_gba/kotogba/packages/BPRJ_01.ks5",FA_READ);
    if(opened!=FR_OK){gStudyLoadStatus=12;kotogba_bootStep(47,opened);return 0;}
    gStudyLoadStatus=2;const u32 size=f_size(&file);
    bool ok=size>=52&&size<=sizeof(sStudyData);
    if(!ok)gStudyLoadStatus=size>sizeof(sStudyData)?11:13;
    if(ok&&!BootRead(file,sStudyData,size,33)){gStudyLoadStatus=14;ok=false;}
    const FRESULT closed=f_close(&file);
    if(closed!=FR_OK){gStudyLoadStatus=15;kotogba_bootStep(47,closed);ok=false;}
    return ok?size:0;
}
extern "C" [[gnu::target("thumb"),gnu::section(".ewram.text"),gnu::optimize("Os"),gnu::noinline]] void kotogba_loadStudy()
{
    kotogba_setupTextCapture(nullptr,0);
    sStudyLineWidths=nullptr;sLegacyAnchorHeads=nullptr;sLegacyAnchorNext=nullptr;
    sTouchOffset=0; sStudyReady=false; sCachedStudy=nullptr; sCachedStudyId=0xffffffff; gStudyLoadStatus=1;
    const u32 size=LoadStudyFile();
    if(!size)return;
    kotogba_bootStep(34,size);
    if(memcmp(sStudyData,"KOTOST5",8) || Read32(8)!=size) return;
    gStudyLoadStatus=3;
    u32 hash=2166136261u;
    for(u32 i=52;i<size;++i) hash=(hash^sStudyData[i])*16777619u;
    if(hash!=Read32(12)) return;
    gStudyLoadStatus=4;
    u32 nc=Read32(16), np=Read32(20), nl=Read32(24), ng=Read32(28), nr=Read32(48);
    u32 co=Read32(32), po=Read32(36), lo=Read32(40), go=Read32(44);
    if(nc>256 || np>2048 || nl>16384 || ng>1024 || co!=52 || nr>32768 ||
       po!=co+nc*16 || lo!=po+np*8 || go<lo+nl*8+nr*2 || go>size || ng*58>size-go) return;
    gStudyLoadStatus=5;
    for(u32 i=0;i<nc;++i) for(u32 t=0;t<3;++t)
    { u32 start=Read16(co+i*16+4+t*2), count=Read16(co+i*16+10+t*2);
      if(!count || count>255 || start+count>np) return; }
    gStudyLoadStatus=6;
    for(u32 i=0;i<np;++i)
      if(Read32(po+i*8)+Read16(po+i*8+4)>nr || Read16(po+i*8+4)>512) return;
    gStudyLoadStatus=7;
    for(u32 i=0;i<nl;++i)
    { u32 o=lo+i*8, st=Read32(o+4);
      if(sStudyData[o]>242 || sStudyData[o+1]>2 || Read16(o+2)>4096 ||
         st<lo+nl*8+nr*2 || st>=go || !memchr(sStudyData+st,0,go-st)) return; }
    for(u32 i=0;i<nr;++i) if(Read16(lo+nl*8+i*2)>=nl) return;
    gStudyLoadStatus=8;
    for(u32 i=1;i<ng;++i) if(Read16(go+(i-1)*58)>=Read16(go+i*58)) return;
    const u32 tail=go+ng*58;
    if(tail==size)return; // D17 needs its matching touch index.
    if(tail<size){
        if(size-tail<4)return;
        const u32 groups=Read16(tail),links=Read16(tail+2),data=tail+4+groups*6;
        if(data+links*5!=size)return;
        for(u32 i=0;i<groups;++i){u32 p=tail+4+i*6;if(Read16(p)>=np || Read16(p+2)+Read16(p+4)>links)return;}
        sTouchOffset=tail;
    }
    // R15 reserves room for provenance and links by using the existing exact
    // on-demand measurement path. Contents, glyphs and hit geometry are kept;
    // this optional one-byte-per-line cache is not needed during scrolling.
    sStudyLineWidths=nullptr;
    const unsigned anchorCount=sizeof(sKotoTapAnchors)/sizeof(sKotoTapAnchors[0]),anchorOffset=(size+1)&~1u;
    if(anchorOffset<=sizeof(sStudyData)&&256+anchorCount<=sizeof(sStudyData)-anchorOffset){
        sLegacyAnchorHeads=sStudyData+anchorOffset;sLegacyAnchorNext=sLegacyAnchorHeads+256;
        for(unsigned i=0;i<256;++i)sLegacyAnchorHeads[i]=255;
        for(unsigned i=anchorCount;i;){--i;const unsigned card=sKotoTapAnchors[i].card;sLegacyAnchorNext[i]=sLegacyAnchorHeads[card];sLegacyAnchorHeads[card]=i;}
    }
    const unsigned captureOffset=(anchorOffset+256+anchorCount+7)&~7u;
    if(captureOffset<sizeof(sStudyData))kotogba_setupTextCapture(sStudyData+captureOffset,sizeof(sStudyData)-captureOffset);
    InitNativeSceneStorage();
    sStudyReady=true; gStudyLoadStatus=10;kotogba_bootStep(35,0);LoadSavedWords();
}

static constexpr u16 Rgb(int r, int g, int b)
{
    return 0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
}
static constexpr u16 kPaper = Rgb(248, 244, 231);
static constexpr u16 kInk = Rgb(32, 48, 64);
static constexpr u16 kTeal = Rgb(20, 97, 177);
static constexpr u16 kMuted = Rgb(88, 104, 112);
static constexpr u16 kLine = Rgb(208, 216, 200);
static constexpr u16 kTab = Rgb(232, 232, 216);

// D11: two 8-bit indexed 256x256 surfaces fit entirely in VRAM C.
// Only the hidden surface is written. A completed image is published with a
// single BG3 base change during VBlank. No display blanking or DMA is used.
// Keep render state outside the small emulator IRQ stack.
struct ReaderFrame {
    u32 card;
    u16 page;
    u8 tab;
    u8 menu;
    int scroll;
};
struct ReaderRender {
    ReaderFrame target;
    const u8* card;
    bool busy, incremental;
    u8 back, stage;
    u16 row, endRow, line, chrome, total, thumbY, thumbH;
    int clipTop, clipBottom, delta, x, y, originX;
    u32 firstLine, lineCount;
    const char* text;
    u8 color;
    char number[8];
    u8 saveState;
    bool modalIncremental;
    int modalDelta;
};
[[gnu::section(".ewram.bss")]] static ReaderRender sRender;
struct ReaderGlyph {const u8* pixels;int x,y,row,bottom,width;u8 color;bool japanese,active,reduced;};
[[gnu::section(".ewram.bss")]] static ReaderGlyph sGlyph;
[[gnu::section(".ewram.bss")]] static ReaderFrame sShown;
[[gnu::section(".ewram.bss")]] static ReaderFrame sTapFrame;
// One retained printer per game window. Selection always owns the first page;
// other verified, uncovered windows remain reachable without sharing card IDs.
struct SceneWindow {u32 ptr,card;};
[[gnu::section(".ewram.bss")]] static SceneWindow sSceneWindows[32];
[[gnu::section(".ewram.bss")]] static u8 sPrimaryWindow,sOtherWindow,sCaptureIndex,sOtherCount;
[[gnu::section(".ewram.bss")]] static ReaderFrame sBufferFrames[2];
[[gnu::section(".ewram.bss")]] static bool sBufferValid[2];
[[gnu::section(".ewram.bss")]] static u16 sLatinMasks[64][3];
[[gnu::section(".ewram.bss")]] static u16 sJapanesePairs[2][16];
[[gnu::section(".ewram.bss")]] static u16 sJapaneseMasks[16];
[[gnu::section(".ewram.bss")]] static u8 sLatinIndex[256];
[[gnu::section(".dtcm")]] static bool sHasShown=false;
[[gnu::section(".dtcm")]] static u8 sFront=0;
[[gnu::section(".dtcm")]] static u8 sShownModalKind=0;
[[gnu::section(".dtcm")]] static u8 sShownModalStatus=0;
[[gnu::section(".dtcm")]] static u16 sShownModalPage=0,sShownModalFirst=0,sShownModalCount=0,sShownModalOrdinal=0;
[[gnu::section(".dtcm")]] static int sShownModalScroll=0;
[[gnu::section(".ewram.bss")]] volatile u32 gReaderPresented, gReaderAudioYields, gReaderBudgetYields;

// Palette zero remains unused (transparent). All written pixels are opaque.
enum : u8 { Paper=1, Ink, Teal, Muted, Line, Tab, White, Red, InkSoft, InkMedium, TealSoft, TealMedium, LightGreen, Green, GrammarBlue };
static u16 Blend(u16 bg,u16 fg,unsigned a) {
    unsigned out=0x8000;
    for(unsigned shift=0;shift<15;shift+=5)
        out|=(((((bg>>shift)&31)*(3-a))+(((fg>>shift)&31)*a)+1)/3)<<shift;
    return out;
}
static volatile u16* Surface(unsigned bank) {return (volatile u16*)(0x06200000+bank*0x10000);}
static void InitRenderer()
{
    const u16 colors[]={0,kPaper,kInk,kTeal,kMuted,kLine,kTab,0xffff,Rgb(204,65,72),
        Blend(kPaper,kInk,1),Blend(kPaper,kInk,2),Blend(kPaper,kTeal,1),Blend(kPaper,kTeal,2),Rgb(181,229,181),Rgb(40,145,65),Rgb(45,90,160)};
    for(unsigned i=0;i<sizeof(colors)/2;++i) ((volatile u16*)0x05000400)[i]=colors[i];
    for(unsigned r=0;r<6;++r)for(unsigned g=0;g<6;++g)for(unsigned b=0;b<6;++b)
        ((volatile u16*)0x05000400)[16+r*36+g*6+b]=Rgb(r*51,g*51,b*51);
    for(auto& v:sLatinIndex) v=0xff;
    for(unsigned i=0;i<kKotoGbaLatinGlyphCount;++i)
        if(kKotoGbaLatinGlyphs[i].codepoint<256) sLatinIndex[kKotoGbaLatinGlyphs[i].codepoint]=i;
    // Spanish explanatory quotes use the compact Latin quote glyph.
    sLatinIndex[0xab]=sLatinIndex[(unsigned)'"'];
    sLatinIndex[0xbb]=sLatinIndex[(unsigned)'"'];
    for(unsigned bits=0;bits<64;++bits)for(unsigned pair=0;pair<3;++pair) {
        unsigned b=(bits>>(4-pair*2))&3;
        sLatinMasks[bits][pair]=((b&2)?255:0)|((b&1)?0xff00:0);
    }
    for(unsigned i=0;i<16;++i) {
        unsigned a=i>>2,b=i&3;
        sJapaneseMasks[i]=(a?255:0)|(b?0xff00:0);
        for(unsigned c=0;c<2;++c) {
            const u8 shades[]={0,(u8)(c?TealSoft:InkSoft),(u8)(c?TealMedium:InkMedium),(u8)(c?Teal:Ink)};
            sJapanesePairs[c][i]=shades[a]|(shades[b]<<8);
        }
    }
    sBufferValid[0]=sBufferValid[1]=false;
    sFront=0;sHasShown=false;sRender.busy=false;
    gReaderPresented=0;gReaderAudioYields=0;gReaderBudgetYields=0;
    // Called before the VM starts. The first frame is completed synchronously
    // below before selecting indexed mode; normal updates never change mode.
}
static void Pixel(int x,int y,u8 color)
{
    volatile u16* p=Surface(sRender.back)+y*128+(x>>1);
    unsigned shift=(x&1)*8;
    *p=(*p & ~(255u<<shift)) | (color<<shift);
}
[[gnu::always_inline]] static inline const u8* ReaderGlyphPixels(u32 cp)
{
    unsigned lo=0,hi=sStudyReady?Read32(28):0;
    const u32 go=sStudyReady?Read32(44):0;
    while(lo<hi){const unsigned mid=(lo+hi)/2;if(Read16(go+mid*58)<cp)lo=mid+1;else hi=mid;}
    return sStudyReady&&lo<Read32(28)&&Read16(go+lo*58)==cp?sStudyData+go+lo*58+2:KGlyphPixels(cp);
}
static void Glyph(int x,int y,u32 cp,u8 color)
{
    if(cp==0x00ab||cp==0x00bb)cp='"';
    sGlyph.active=false;sGlyph.reduced=false;
    if(cp==0x14c)cp='O';else if(cp==0x14d)cp='o';else if(cp==0x2018||cp==0x2019)cp='\'';
    if(cp==' ' || x>=256 || x<0 || y>=sRender.clipBottom || y+16<=sRender.clipTop) return;
    if(cp>255) {
        const u8* pixels=ReaderGlyphPixels(cp);
        // Legacy packages contain only the small ruby glyphs they used. New
        // dictionary readings also need katakana, long vowels and rare kana.
        // Preserve an existing clean ruby bitmap; otherwise reduce the exact
        // Unicode kana bitmap during the same bounded row painter.
        if(!pixels&&cp>=0xe000&&cp<0xe0c0){
            pixels=ReaderGlyphPixels(0x3040+cp-0xe000);
            if(pixels){
                sGlyph={pixels,x,y,y<sRender.clipTop?sRender.clipTop-y:0,y+10>sRender.clipBottom?sRender.clipBottom-y:10,x+8>256?256-x:8,color,true,true,true};
                sGlyph.active=sGlyph.row<sGlyph.bottom;return;
            }
        }
        if(pixels) {
            sGlyph={pixels,x,y,y<sRender.clipTop?sRender.clipTop-y:0,y+16>sRender.clipBottom?sRender.clipBottom-y:16,x+14>256?256-x:14,color,true,true};
            sGlyph.active=sGlyph.row<sGlyph.bottom;
            return;
        }
        cp='?';
    }
    unsigned idx=sLatinIndex[cp];
    if(idx==0xff) idx=sLatinIndex[(unsigned)'?'];
    if(idx==0xff)return;
    sGlyph={kKotoGbaLatinGlyphs[idx].rows,x,y,y<sRender.clipTop?sRender.clipTop-y:0,y+10>sRender.clipBottom?sRender.clipBottom-y:10,6,color,false,true};
    sGlyph.active=sGlyph.row<sGlyph.bottom;
}
// Small bursts avoid dispatcher overhead, while every row still checks audio
// and the shared time limit. An unfinished glyph survives the next IRQ.
[[gnu::noinline]] static void GlyphRow()
{
    if(!sGlyph.active)return;
    const int x=sGlyph.x,y=sGlyph.y,width=sGlyph.width;const bool japanese=sGlyph.japanese;const u8 color=sGlyph.color;const u8* pixels=sGlyph.pixels;
    for(unsigned work=0;work<4&&sGlyph.active;++work){const int row=sGlyph.row++;
    if(japanese){
        if(sGlyph.reduced){
            const u8 shades[]={0,(u8)(color==Teal?TealSoft:InkSoft),(u8)(color==Teal?TealMedium:InkMedium),color};
            for(int col=0;col<width;++col){unsigned alpha=0;
                // Disjoint source bins cover every pixel once. Sampling only
                // their top-left pixel can erase a thin long-vowel stroke.
                for(int yy=row*16/10;yy<(row+1)*16/10;++yy)
                    for(int xx=col*14/8;xx<(col+1)*14/8;++xx){const unsigned i=yy*14+xx,a=(pixels[i/4]>>(6-2*(i%4)))&3;
                        if(a>alpha)alpha=a;}
                if(alpha)Pixel(x+col,y+row,shades[alpha]);}
        }else if(!(x&1)&&width==14){
            const u16* values=sJapanesePairs[color==Teal];volatile u16* dst=Surface(sRender.back)+(y+row)*128+x/2;
            for(int pair=0;pair<7;++pair){const unsigned i=row*14+pair*2,code=(pixels[i/4]>>(4-2*(i%4)))&15;const u16 mask=sJapaneseMasks[code];if(mask)dst[pair]=(dst[pair]&~mask)|values[code];}
        }else{
            const u8 shades[]={0,(u8)(color==Teal?TealSoft:InkSoft),(u8)(color==Teal?TealMedium:InkMedium),color};
            for(int col=0;col<width;++col){const unsigned i=row*14+col,a=(pixels[i/4]>>(6-2*(i%4)))&3;if(a)Pixel(x+col,y+row,shades[a]);}
        }
    }else if(!(x&1)&&x<=250){
        const u16 c=color*0x101u;volatile u16* dst=Surface(sRender.back)+(y+row)*128+x/2;const u16* m=sLatinMasks[pixels[row]&63];
        if(m[0])dst[0]=(dst[0]&~m[0])|(c&m[0]);
        if(m[1])dst[1]=(dst[1]&~m[1])|(c&m[1]);
        if(m[2])dst[2]=(dst[2]&~m[2])|(c&m[2]);
    }else{const u8 bits=pixels[row];for(int col=0;col<6&&x+col<256;++col)if(bits&(1u<<(5-col)))Pixel(x+col,y+row,color);}
    sGlyph.active=sGlyph.row<sGlyph.bottom;
    if(AudioPending()||Scanline()>=gKotoGbaReaderWindowEnd)break;
    }
}
static u32 DecodeUtf8(const char*& text)
{
    const u8 first=(u8)*text++;
    if(first<0x80)return first;
    unsigned n;u32 v,minimum;
    if(first>=0xc2 && first<=0xdf){n=1;v=first&31;minimum=0x80;}
    else if(first>=0xe0 && first<=0xef){n=2;v=first&15;minimum=0x800;}
    else if(first>=0xf0 && first<=0xf4){n=3;v=first&7;minimum=0x10000;}
    else return '?';
    for(unsigned i=0;i<n;++i){u8 b=*text;if((b&0xc0)!=0x80)return '?';++text;v=(v<<6)|(b&63);}
    return v<minimum || v>0x10ffff || (v>=0xd800 && v<=0xdfff)?'?':v;
}
static void SetText(int x,int y,const char* text,u8 color)
{ sRender.x=x;sRender.originX=x;sRender.y=y;sRender.text=text;sRender.color=color; }
static void FillSpan(unsigned y,unsigned x,unsigned width,u8 color)
{
    volatile u16* dst=Surface(sRender.back)+y*128+(x>>1);
    const u16 pair=color*0x101u;
    for(unsigned i=0;i<width/2;++i)dst[i]=pair;
}
static bool SameModalContent(){return sModalKind==sShownModalKind && sModalStatus==sShownModalStatus && sModalPage==sShownModalPage && sModalFirst==sShownModalFirst && sModalCount==sShownModalCount && sModalOrdinal==sShownModalOrdinal && sModalCard==sShownModalCard && CSaving()==sShownSaving;}
// sShownModalScroll is the destination of the current/abandoned paint.
// Only the published offset proves that a popup is already on screen.
static bool SameModalShown(){return SameModalContent()&&
    (!sModalKind||(sModalViewReady&&sModalScroll==sDisplayedModalScroll));}
static bool ModalSaved(){return IsWordSaved(sModalCard,sModalOrdinal);}
static bool ModalVocabulary(){return sModalKind==1||sModalKind==4;}
static unsigned ModalSaveLeft(){return sModalLeft+sModalWidth-28;}
static unsigned ModalContentTop(){return sModalTop+(ModalVocabulary()||sModalKind==3?20:2);}
static unsigned ModalContentBottom(){return sModalBottom-(ModalVocabulary()?20:2);}
static unsigned ModalRailLeft(){return sModalLeft+sModalWidth-6;}
static int ModalScrollLimit();
static bool ModalRailHit(unsigned x,unsigned y)
{return ModalScrollLimit()>0&&x>=ModalRailLeft()-2&&x<sModalLeft+sModalWidth&&y>=ModalContentTop()&&y<ModalContentBottom();}
static int ModalRailScroll(int y)
{return ScrollRail(y,ModalContentTop(),ModalContentBottom()-ModalContentTop(),ModalScrollLimit());}
static bool InlineOpenReference(unsigned index);
static void InlineClose();
static unsigned InlineModalHeight();
static bool InlineModalRenderUnit();
static u32 InlineModalGlyphAt(unsigned x,unsigned y);
static void InlineSavedCancel();
static void InlineSavedService();
// Small 12 x 10 confirmation mark, drawn one row per render unit.
static void SavedTickRow(unsigned x,unsigned y,unsigned row,u8 color)
{
    static constexpr u16 rows[]={0x000,0xc00,0xe00,0x700,0x383,0x1c7,0x0ee,0x07c,0x038,0x010};
    for(unsigned col=0;col<12;++col)if(rows[row]&(1u<<col))Pixel(x+col,y,color);
}
// Disk, pending dots and confirmation share a 12 x 10 pixel box.
static void SaveControlRow(unsigned x,unsigned y,unsigned row,u8 color,u8 state)
{
    if(state==2){SavedTickRow(x,y,row,color);return;}
    static constexpr u16 disk[]={0x7ff,0xf81,0xf81,0xf81,0xfff,0xc03,0xc03,0xc03,0xc03,0xfff};
    const u16 bits=state==1?((row==4||row==5)?0x333:0):disk[row];
    for(unsigned col=0;col<12;++col)if(bits&(1u<<col))Pixel(x+col,y,color);
}
static unsigned PreferredFragment(u32 id,unsigned count)
{return count>=3&&id>=2&&id<=7?(id-2)%3:0;}
static bool SliceExpired(unsigned start);
static void SceneChanged();
static u32 NativeOriginalCursor(const KotoTextRecord& r,u32 cursor);
static u32 LegacyOriginalPage(u32 cursor);
static bool CorpusResolve(unsigned slot,u32 source,u32 hash,unsigned size);
static unsigned CorpusStaticPage(unsigned slot);
static bool CorpusReady(const ReaderFrame& f);
static u32 CorpusGeneration();
static void CorpusMatchLinks(const ReaderFrame& f,const u8* text,unsigned size);
static void CorpusLinkPosition(unsigned raw,unsigned utf8);
static bool CorpusTouch(unsigned x,unsigned y,u32& card,u16& ref);
static bool CorpusHasLink(unsigned offset);
[[gnu::always_inline]] static inline bool NativeRomByte(u32 ptr,u8& value)
{
    if(ptr>=0x02200000&&ptr<0x02400000){value=*(const u8*)ptr;return true;}
    if(ptr<0x08000000||ptr>=0x0a000000)return false;
    const u32 off=ptr-0x08000000;
    if(off<0x200000){value=*(const u8*)(0x02200000+off);return true;}
    const u8* block=(const u8*)sdc_romBlockToCacheBlock[off>>SDC_BLOCK_SHIFT];
    if(!block)return false;value=block[off&SDC_BLOCK_MASK];return true;
}

static unsigned CorpusCompose(const ReaderFrame& f,unsigned japaneseLength);
static unsigned CorpusFullJapanese(const ReaderFrame& f);
static bool CorpusComplete(unsigned slot);
static const char* CorpusSentenceTranslation(unsigned slot);
static unsigned SentenceLegacyGroupFor(u32 source);
static unsigned SentenceLegacyMask(unsigned group);
static bool IsSentenceLegacyFull(const ReaderFrame& f);
static unsigned SentenceLegacyLineCount(const ReaderFrame& f);
static unsigned SentenceLegacyHeight(const ReaderFrame& f);
static void SentenceLegacyReadLine(const ReaderFrame& f,unsigned line);
static bool SentenceLegacyTouch(unsigned x,unsigned y,u32& card,u16& ref);
static unsigned LegacyShift(const ReaderFrame& f,unsigned line);
static const char* LegacyText(const ReaderFrame& f,unsigned line,const char* original);
static unsigned LegacyPrepare(const ReaderFrame& f);
static unsigned CorpusValidateGrammar(unsigned tail,unsigned bytes);
static bool GrammarRequest(const ReaderFrame& f);
static unsigned GrammarPrepare(const ReaderFrame& f,unsigned height);
static unsigned GrammarHeight(const ReaderFrame& f,unsigned height);
static bool GrammarRenderUnit();
static bool SentenceRetainedRoute();
static bool GrammarTouch(unsigned x,unsigned y,u32& card,u16& ref);
static void GrammarOpen(unsigned index);
static void GrammarBeginFrame(const ReaderFrame& f);
static void GrammarReleaseFrame();
static void GrammarClose();
static bool GrammarLegacyAllowed(u32 card,u16 ref);
static bool GrammarAnchor(unsigned rawStart,unsigned rawEnd,u32& card,u16& ref);
static bool CompositionGrammarAuthorized(unsigned slot,u32 origin);
static bool CompositionGrammarComplete(unsigned slot);
static void CompositionPaintBegin(const ReaderFrame& frame);
static void CompositionPaintPublish();
static void LinguisticPaintPublish();
static bool CompositionContinueRender(const ReaderFrame& frame);
static u32 CompositionPublishedLiteralCurrent(const ReaderFrame& frame);
static void CompositionResolveReference(unsigned slot,u32 key,bool hash);
static void CompositionAddRoutes();
static unsigned CompositionTargetPage();
static bool NativeVisible(unsigned index,int& x,int& y);
static u32 NativeSelection();
static bool ExpandedPointer(u32 ptr);
static bool NativeTileVisible(unsigned window,unsigned px,unsigned py);
static bool NativeQuestionContext();
static inline bool WindowExists(unsigned id);
#include "KotoGbaOrthographyApi.h"
#include "KotoGbaNativeScene.inc"
// A published complete-sentence route is the proof; never infer availability
// from the number of text captures or expose a future fragment.
static bool SentenceNotice()
{return !sStudyTab&&!sStudyPage[0]&&SentenceRetainedRoute();}
static bool QuestionFrame(const ReaderFrame& f)
{return f.menu&&f.menu==sGameMenu&&!f.tab&&f.card==sKotoGbaActiveCardId&&FindStudy(f.card)&&f.page==Card16(FindStudy(f.card),4);}
static unsigned QuestionExtraHeight(const ReaderFrame& f)
{return QuestionFrame(f)?Read16(Read32(36)+8*sGameMenuPages[(f.menu>>4)-1]+6)+12:0;}
static unsigned PageCount(const u8* card,unsigned tab)
{
    if(tab)return 1;
    if(sNativeCount)return sNativeTotal;
    if(!card)return 1;
    unsigned count=Card16(card,10);
    if(!sOtherCount)return count;
    for(unsigned i=0;i<32;++i){const u32 id=sSceneWindows[i].card;
        if(i==sPrimaryWindow||!id||id==sKotoGbaActiveCardId)continue;
        bool duplicate=false;for(unsigned j=0;j<i;++j)if(j!=sPrimaryWindow&&sSceneWindows[j].card==id)duplicate=true;
        if(!duplicate)if(const u8* extra=FindStudy(id))count+=Card16(extra,10);
    }
    return count>255?255:count;
}
static ReaderFrame TranslationFrame(unsigned page,int scroll)
{
    if(sNativeCount)return NativeAt(page,scroll);
    const u8* card=FindStudy(sKotoGbaActiveCardId);
    ReaderFrame result={sKotoGbaActiveCardId,0,0,sGameMenu,scroll};
    if(!card)return result;
    const unsigned count=Card16(card,10);
    if(page<count){result.page=Card16(card,4)+(page+PreferredFragment(result.card,count))%count;return result;}
    page-=count;
    for(unsigned i=0;i<32;++i){const u32 id=sSceneWindows[i].card;
        if(i==sPrimaryWindow||!id||id==sKotoGbaActiveCardId)continue;
        bool duplicate=false;for(unsigned j=0;j<i;++j)if(j!=sPrimaryWindow&&sSceneWindows[j].card==id)duplicate=true;
        if(duplicate)continue;
        const u8* extra=FindStudy(id);if(!extra)continue;
        const unsigned n=Card16(extra,10);
        if(page<n){result.card=id;result.page=Card16(extra,4)+(page+PreferredFragment(id,n))%n;return result;}
        page-=n;
    }
    if(sGameMenu)result.page=sGameMenuPages[(sGameMenu>>4)-1];
    return result;
}
static unsigned PageIndex(const u8* card,unsigned tab,unsigned page,unsigned menu)
{return tab?Card16(card,4+tab*2)+page:page;}
static bool SamePage(const ReaderFrame&,const ReaderFrame&);
[[gnu::target("thumb"),gnu::noinline]] static u16 TouchReference(u16 x,u16 y)
{
    const u8* card=FindStudy(sShown.card);
    if((sRender.busy&&!SamePage(sRender.target,sShown))||!sTouchOffset||!card||!sHasShown||sShown.tab!=0||y<56||y>=173)return 0xffff;
    const u16 page=sShown.page;
    unsigned lo=0,hi=Read16(sTouchOffset);
    while(lo<hi){unsigned m=(lo+hi)/2;if(Read16(sTouchOffset+4+m*6)<page)lo=m+1;else hi=m;}
    if(lo==Read16(sTouchOffset))return 0xffff;
    const u32 group=sTouchOffset+4+lo*6;
    if(Read16(group)!=page)return 0xffff;
    const u32 data=sTouchOffset+4+Read16(sTouchOffset)*6;
    const u32 pg=Read32(36)+page*8,first=Read32(pg),refs=Read32(40)+Read32(24)*8;
    const unsigned n=Read16(group+4),begin=Read16(group+2);
    for(unsigned i=0;i<n;++i){
        const u32 p=data+(begin+i)*5;
        const unsigned line=sStudyData[p+2];
        if(line>=Read16(pg+4))continue;
        const u32 l=Read32(40)+Read16(refs+2*(first+line))*8;
        const int yy=56+Read16(l+2)+LegacyShift(sShown,line)-sShown.scroll;
        const int ruby=((const u8*)sStudyData)[Read32(l+4)]>=0x80?12:0;
        if(y>=yy-ruby && y<yy+16 && x+2>=sStudyData[p+3] && x<sStudyData[p+4]+2)return Read16(p);
    }
    return 0xffff;
}
[[gnu::target("thumb"),gnu::noinline]] static void OpenTouchReference(u16 ref,u32 cardId)
{
    if(cardId==65535){InlineOpenReference(ref);return;}
    if(!cardId&&(ref&0xfff8)==0x8000){GrammarOpen(ref&7);return;}
    if(!GrammarLegacyAllowed(cardId,ref))return;
    // R25 contextual vocabulary may reuse a small popup from catalog card255.
    // The package audit reserves bit14 only for non-grammar references; all
    // pre-existing vocabulary references use pages below32 and keep their IDs.
    if((ref&0xc000)==0x4000){cardId=255;ref&=0x3fff;}
    const bool grammar=ref&0x8000;
    const u8* card=FindStudy(cardId);if(!card)return;
    const unsigned page=(ref>>9)&63,first=ref&511;
    const u32 pg=Read32(36)+8*(Card16(card,grammar?8:6)+page),start=Read32(pg);
    const unsigned total=Read16(pg+4),refs=Read32(40)+Read32(24)*8;
    unsigned count=0,width=112,titleY=0;
    for(unsigned i=first;i<total&&count<128;++i){
        const unsigned line=Read16(refs+2*(start+i));const u32 l=Read32(40)+line*8;
        const unsigned style=sStudyData[l+1],yy=Read16(l+2);
        if(!count)titleY=yy+(style==2?12:0);
        else if(grammar?style!=1:(style!=1&&yy>titleY))break;
        unsigned w=6+sStudyData[l];
        if(sStudyLineWidths)w=sStudyLineWidths[line];
        else{const char* text=(const char*)sStudyData+Read32(l+4);while(*text&&w<248){const u32 cp=DecodeUtf8(text);w+=(cp>=0xe000&&cp<0xe100)?8:cp>255?14:6;}}
        if(w>width)width=w;
        ++count;
    }
    sModalCard=cardId;sModalKind=grammar?2:1;sModalStatus=0;sModalBody=nullptr;sModalPage=page;
    sModalFirst=first;sModalCount=count;sModalWidth=((width+7)/8)*8;
    if(sModalWidth>248)sModalWidth=248;
    sModalOrdinal=0x8000|ref;sModalScroll=0;
    // Preserve existing saved-word identifiers when the glossary range agrees.
    const unsigned anchors=sizeof(sKotoTapAnchors)/sizeof(sKotoTapAnchors[0]);
    if(!grammar)for(unsigned i=sLegacyAnchorHeads?(cardId<256?sLegacyAnchorHeads[cardId]:255):0;i!=255;i=sLegacyAnchorHeads?sLegacyAnchorNext[i]:i+1<anchors?i+1:255){
        const auto& a=sKotoTapAnchors[i];
        unsigned ap=0,af=a.firstLine;if(a.card>=133&&a.card<=135){ap=af;af=0;}
        if(a.card==cardId&&ap==page&&af==first){sModalOrdinal=i;break;}
    }
}

static u32 ModalGlyphAt(u16 x,u16 y)
{
    if(!ModalVocabulary()||!sHasShown||!sModalViewReady||!sTapModalReady||y<ModalContentTop()||y>=sModalBottom-18)return 0;
    if(sModalKind==4)return InlineModalGlyphAt(x,y);
    const u8* card=FindStudy(sModalCard);if(!card)return 0;
    const u32 pg=Read32(36)+8*(Card16(card,6)+sModalPage),first=Read32(pg)+sModalFirst,refs=Read32(40)+Read32(24)*8;
    for(unsigned i=0;i<sModalCount;++i){const u32 l=Read32(40)+8*Read16(refs+2*(first+i));
        const int yy=sModalTop+26+(int)Read16(l+2)-sModalBaseY-sTapModalScroll;
        if(y<yy||y>=yy+16)continue;
        unsigned xx=sModalLeft-2+sStudyData[l];const char* text=(const char*)sStudyData+Read32(l+4);
        while(*text){const u32 cp=DecodeUtf8(text);const unsigned width=cp>255?14:6;if(x>=xx&&x<xx+width)return cp;xx+=width;}
    }
    return 0;
}
static void FillRow(volatile u32* p,u8 color)
{
    const u32 c=color*0x01010101u;
    // Aligned word writes; DS VRAM does not support byte stores.
    for(unsigned n=0;n<8;++n) {
        p[0]=c;p[1]=c;p[2]=c;p[3]=c;p[4]=c;p[5]=c;p[6]=c;p[7]=c;p+=8;
    }
}
static_assert(__builtin_offsetof(ReaderFrame,page)==4&&__builtin_offsetof(ReaderFrame,tab)==6&&
              __builtin_offsetof(ReaderFrame,menu)==7&&__builtin_offsetof(ReaderFrame,scroll)==8);
[[gnu::noinline]] static bool SamePage(const ReaderFrame& a,const ReaderFrame& b)
{return !memcmp(&a,&b,8);}
#include "KotoGbaBackdrop.inc"
static ReaderFrame DesiredFrame()
{return sStudyTab?ReaderFrame{sKotoGbaActiveCardId,sStudyPage[sStudyTab],sStudyTab,sGameMenu,sScroll}:TranslationFrame(sStudyPage[0],sScroll);}
static void RenderPageNumber(unsigned total)
{
    const unsigned n=sStudyPage[0]+1;unsigned pos=0;
    if(n>=100)sRender.number[pos++]='0'+n/100;if(n>=10)sRender.number[pos++]='0'+n/10%10;sRender.number[pos++]='0'+n%10;sRender.number[pos++]='/';
    if(total>=100)sRender.number[pos++]='0'+total/100;if(total>=10)sRender.number[pos++]='0'+total/10%10;sRender.number[pos++]='0'+total%10;sRender.number[pos]=0;
}
static void StartRender(const ReaderFrame& frame)
{
    if(sModalKind&&!sReaderBackdrops[0].active&&sHasShown)ReaderBackdropRemember(0);
    CompositionPaintBegin(frame);
    GrammarBeginFrame(frame);
    sGlyph.active=false;
    // For another scroll position of the same popup, its outside background
    // is already complete. Copy it instead of rebuilding the whole reader.
    const bool reuseBackground=sModalKind&&sHasShown&&SamePage(frame,sShown)&&frame.scroll==sShown.scroll;
    const bool reuseModal=reuseBackground&&sModalViewReady&&SameModalContent();
    if(!SameModalContent())sModalViewReady=false;
    sShownModalKind=sModalKind;sShownModalStatus=sModalStatus;sShownModalPage=sModalPage;sShownModalFirst=sModalFirst;sShownModalCount=sModalCount;sShownModalOrdinal=sModalOrdinal;sShownModalScroll=sModalScroll;sShownModalCard=sModalCard;sShownSaving=CSaving();
    sRender.target=frame;sRender.card=FindStudy(frame.card);sRender.back=sFront^1;sRender.busy=true;sRender.stage=0;sRender.row=0;sRender.endRow=192;sRender.line=0;sRender.chrome=0;sRender.text=nullptr;
    sRender.saveState=sModalStatus==2?3:ModalSaved()?(CSaving()?1:2):0;
    sRender.delta=frame.scroll-sShown.scroll;sRender.incremental=!sModalKind&&sHasShown&&sBufferValid[sFront]&&SamePage(frame,sShown);
    sRender.clipTop=56;sRender.clipBottom=173;
    if(reuseBackground)sRender.stage=6;
    sRender.modalDelta=sModalScroll-sDisplayedModalScroll;
        const int modalVisible=ModalContentBottom()-ModalContentTop();
    sRender.modalIncremental=reuseModal&&sRender.modalDelta>-modalVisible&&sRender.modalDelta<modalVisible;
    if(sRender.incremental){if(sBufferValid[sRender.back]&&SamePage(frame,sBufferFrames[sRender.back])){sRender.row=56;sRender.endRow=173;}
        if(sRender.delta>0&&sRender.delta<117)sRender.clipTop=173-sRender.delta;else if(sRender.delta<=0&&sRender.delta>-117)sRender.clipBottom=56-sRender.delta;}
    sBufferValid[sRender.back]=false;sRender.firstLine=0;sRender.lineCount=0;sRender.thumbH=0;sRender.number[0]=0;
    int height=0;
    if(reuseBackground)height=0; // The published parent remains frozen under its popup.
    else if(IsSentenceLegacyFull(frame)){height=SentenceLegacyHeight(frame);sRender.lineCount=SentenceLegacyLineCount(frame);
        RenderPageNumber(sNativeTotal);
    }
    else if(NativeFrame(frame)){height=sNativeRenderHeight=NativeDecode(frame);
        RenderPageNumber(sNativeTotal);
    }
    else if(frame.tab){sRender.lineCount=CListCount(frame.tab);height=sRender.lineCount*CRowHeight(frame.tab);
        const int first=(int)frame.scroll+sRender.clipTop-60-(frame.tab==1?34:16);
        if(first>=0)sRender.line=first/CRowHeight(frame.tab)+1;
        if(sRender.line>sRender.lineCount)sRender.line=sRender.lineCount;}
    else if(sRender.card){u32 pg=Read32(36)+8*PageIndex(sRender.card,0,frame.page,frame.menu);sRender.firstLine=Read32(pg);sRender.lineCount=Read16(pg+4);height=LegacyPrepare(frame);
        if(QuestionFrame(frame)){sRender.lineCount+=Read16(Read32(36)+8*sGameMenuPages[(frame.menu>>4)-1]+4);height+=QuestionExtraHeight(frame);}
        const u8* primary=FindStudy(sKotoGbaActiveCardId);
        RenderPageNumber(sNativeCount?sNativeTotal:primary?PageCount(primary,0):1);}
    // A contextual popup points only to the published grammar snapshot. Keep
    // it intact even if an unrelated pending save requests another paint.
    if(!sModalKind)height=GrammarPrepare(frame,height);
    else if(!reuseBackground)height=GrammarHeight(frame,height);
    if(height>117){int h=117*117/height;if(h<8)h=8;sRender.thumbH=h;sRender.thumbY=56+frame.scroll*(117-h)/(height-117);}
    if(sModalKind){const u8* card=FindStudy(sModalCard);sModalBaseY=0;sModalHeight=30;
        if(sModalKind==3)sModalHeight=sModalCount+20;
        if(sModalKind==4)sModalHeight=InlineModalHeight();
        if(card&&sModalCount){u32 pg=Read32(36)+8*(Card16(card,sModalKind==2?8:6)+sModalPage),refs=Read32(40)+Read32(24)*8,first=Read32(pg)+sModalFirst;
            u32 l=Read32(40)+8*Read16(refs+2*first);sModalBaseY=Read16(l+2);l=Read32(40)+8*Read16(refs+2*(first+sModalCount-1));sModalHeight=Read16(l+2)-sModalBaseY+(ModalVocabulary()?16:24);}
        unsigned panel=sModalHeight+(ModalVocabulary()?48:16);if(panel>128)panel=128;const unsigned minimum=ModalVocabulary()?80:70;if(panel<minimum)panel=minimum;
        // Vocabulary uses the measured width from OpenTouchReference. Keep
        // the controls inside that panel when its width changes.
        if(!ModalVocabulary())sModalWidth=240;
        sModalLeft=(256-sModalWidth)/2;sModalTop=(240-panel)/2;sModalBottom=sModalTop+panel;}
}
static void BaseRow()
{
    unsigned y=sRender.row++;
    volatile u16* dst=Surface(sRender.back)+y*128;
    if(sRender.incremental) {
        int srcY=y;
        if(y>=56 && y<173)srcY+=sRender.delta;
        if(y>=56 && y<173 && (srcY<56 || srcY>=173)) FillRow((volatile u32*)dst,Paper);
        else mem_copy32((const void*)(Surface(sFront)+srcY*128),(void*)dst,256);
    } else {
        FillRow((volatile u32*)dst,y<27?Ink:Paper);
        if(sStudyReady && y>=29 && y<53) {
            for(unsigned tab=0;tab<3;++tab) {
                u8 color=sRender.target.tab==tab?Teal:Tab;
                unsigned x=tab*85,end=x+84;
                if(x&1){Pixel(x++,y,color);}
                for(;x+1<end;x+=2) dst[x/2]=color*0x101u;
                if(x<end)Pixel(x,y,color);
            }
        }
        if(sStudyReady && y==174) for(unsigned x=4;x<124;++x)dst[x]=Line*0x101u;
    }
    if(sStudyReady && y>=56 && y<173) {
        u8 color=sRender.thumbH?Line:Paper;
        if(sRender.thumbH && y>=sRender.thumbY && y<sRender.thumbY+sRender.thumbH)color=Teal;
        dst[126]=color*0x101u;
    }
}
// One unit is at most one row, glyph, or content record. Progress lives in
// sRender, so yielding never keeps a C stack across frames.
[[gnu::noinline]] static void RenderUnit()
{
    if(sGlyph.active){GlyphRow();return;}
    if(sRender.text) {
        if(!*sRender.text){sRender.text=nullptr;return;}
        if(sRender.y>=sRender.clipBottom){sRender.text=nullptr;return;}
        // Skip clipped text in bounded byte bursts, rather than decoding and
        // dispatching a glyph that cannot touch the visible strip.
        if(sRender.y+16<=sRender.clipTop){
            const int step=sModalKind==4&&sRender.stage==8?(sRender.line==2?12:18):
                (sModalKind==3||sRender.stage==5||(sRender.stage==2&&sRender.target.tab==1))?18:14;
            for(unsigned n=0;n<96&&*sRender.text&&sRender.y+16<=sRender.clipTop;++n)
                if(*sRender.text++=='\n'){sRender.y+=step;sRender.x=sRender.originX;}
            return;
        }
        if(*sRender.text=='\n'){++sRender.text;sRender.y+=sModalKind==4&&sRender.stage==8?(sRender.line==2?12:18):
            (sModalKind==3||sRender.stage==5||(sRender.stage==2&&sRender.target.tab==1))?18:14;sRender.x=sRender.originX;return;}
        const char* begin=sRender.text;u32 cp=DecodeUtf8(sRender.text);
        if(sModalKind==4&&sRender.stage==8&&sRender.line==2&&cp>=0x3040&&cp<=0x30ff)cp=0xe000+cp-0x3040;
        const u8 color=sRender.stage==2&&NativeFrame(sRender.target)&&CorpusHasLink(begin-sNativeRenderText)?Teal:sRender.color;
        Glyph(sRender.x,sRender.y,cp,color);
        sRender.x+=(cp>=0xe000 && cp<0xe100)?8:cp>255?14:6;return;
    }
    if(sRender.stage==6) {
        if(sRender.row<192){const int y=sRender.row++;
            mem_copy32((const void*)(Surface(sFront)+y*128),(void*)(Surface(sRender.back)+y*128),256);
            const int top=ModalContentTop(),bottom=ModalContentBottom();
            if(sRender.modalIncremental&&y>=top&&y<bottom){
                const int sourceY=y+sRender.modalDelta;
                if(sourceY>=top&&sourceY<bottom){
                    const volatile u16* src=Surface(sFront)+sourceY*128+(sModalLeft+2)/2;
                    volatile u16* dst=Surface(sRender.back)+y*128+(sModalLeft+2)/2;
                    dst[0]=src[0];
                    mem_copy32((const void*)(src+1),(void*)(dst+1),sModalWidth-8);
                    dst[(sModalWidth-6)/2]=src[(sModalWidth-6)/2];
                }else FillSpan(y,sModalLeft+2,sModalWidth-4,Paper);
            }
            return;
        }
        sRender.stage=7;sRender.row=sModalTop;return;
    }
    if(sRender.stage==7) {
        // Incremental scrolling already copied the exact published panel in
        // stage6. Its frame, title and save controls are unchanged; repaint
        // only the newly exposed body strip. Content/save changes never use
        // this path because they invalidate SameModalContent/reuseModal.
        if(!sRender.modalIncremental&&sRender.row<sModalBottom){int y=sRender.row++;
            const bool reusedBody=sRender.modalIncremental&&y>=ModalContentTop()&&y<sModalBottom-(ModalVocabulary()?20:2);
            if(!reusedBody)FillSpan(y,sModalLeft,sModalWidth,(y==sModalTop||y==sModalBottom-1||(ModalVocabulary()&&y<sModalTop+18))?Teal:Paper);
            if(ModalVocabulary()){
                if(y>=sModalBottom-14&&y<sModalBottom-4)SaveControlRow(ModalSaveLeft()+6,y,y-(sModalBottom-14),Green,sRender.saveState);
            }
            if(y>sModalTop&&y<sModalBottom-1){Pixel(sModalLeft,y,Teal);Pixel(sModalLeft+1,y,Teal);Pixel(sModalLeft+sModalWidth-2,y,Teal);Pixel(sModalLeft+sModalWidth-1,y,Teal);}
            return;
        }
        sRender.stage=8;sRender.chrome=sRender.modalIncremental?2:0;
        sRender.line=sRender.modalIncremental&&(sModalKind==3||sModalKind==4)?1:0;
        sRender.clipTop=sModalTop+2;sRender.clipBottom=sModalBottom-2;
        if(const u8* card=FindStudy(sModalCard)){const u32 pg=Read32(36)+8*(Card16(card,sModalKind==2?8:6)+sModalPage);sRender.firstLine=Read32(pg)+sModalFirst;sRender.lineCount=sModalCount;}return;
    }
    if(sRender.stage==8) {
        if(sRender.text)return;
        if(sRender.chrome==0){++sRender.chrome;if(ModalVocabulary()||sModalKind==3)SetText(sModalLeft+sModalWidth-18,sModalTop+4,"x",sModalKind==3?GrammarBlue:Paper);return;}
        if(sRender.chrome==1){++sRender.chrome;if(ModalVocabulary()&&sRender.saveState==3)SetText(sModalLeft+8,sModalBottom-14,"Error",Red);return;}
        sRender.clipTop=ModalContentTop();sRender.clipBottom=ModalContentBottom();
        if(sRender.modalIncremental){
            if(sRender.modalDelta>=0)sRender.clipTop=sRender.clipBottom-sRender.modalDelta;
            else sRender.clipBottom=sRender.clipTop-sRender.modalDelta;
        }
        if(sModalKind==4){if(InlineModalRenderUnit())sRender.stage=9;return;}
        if(sModalKind==3){
            if(!sRender.line++){
                sRender.clipTop=sModalTop+2;sRender.clipBottom=sModalTop+20;
                SetText(sModalLeft+8,sModalTop+4,"Gramática",GrammarBlue);return;}
            if(sRender.line==2&&sModalBody){sRender.clipTop=ModalContentTop();sRender.clipBottom=sModalBottom-2;
                if(sRender.modalIncremental){if(sRender.modalDelta>=0)sRender.clipTop=sRender.clipBottom-sRender.modalDelta;else sRender.clipBottom=sRender.clipTop-sRender.modalDelta;}
                SetText(sModalLeft+8,sModalTop+26-sShownModalScroll,sModalBody,Ink);return;}
            sRender.stage=9;return;
        }
        if(sRender.line<sRender.lineCount){
            u32 refs=Read32(40)+Read32(24)*8;
            u32 l=Read32(40)+8*Read16(refs+2*(sRender.firstLine+sRender.line++));
            int y=sModalTop+(sModalKind==1?26:8)+(int)Read16(l+2)-sModalBaseY-sShownModalScroll;
            if(y+16>sRender.clipTop && y<sRender.clipBottom)SetText(sModalLeft-2+(int)sStudyData[l],y,(const char*)sStudyData+Read32(l+4),sStudyData[l+1]==0?Teal:Ink);
            return;
        }
        sRender.stage=9;return;
    }
    if(sRender.stage==9){
        // Text already reserves eight pixels at the right edge. The four
        // pixel rail lives in that gutter, below the title and above Save.
        // Repaint it after both full and strip paints: stage6 may have copied
        // an old thumb with the body. Reuse finished parent-render fields and
        // draw one bounded row per unit without adding persistent state.
        if(sRender.chrome==2){
            const int limit=ModalScrollLimit();
            if(!limit){sRender.stage=4;return;}
            const int top=ModalContentTop(),visible=ModalContentBottom()-top;
            int height=visible*visible/(visible+limit);if(height<8)height=8;
            sRender.thumbH=height;sRender.thumbY=top+ScrollClamp(sShownModalScroll,limit)*(visible-height)/limit;
            sRender.row=top;sRender.chrome=3;
        }
        if(sRender.row<ModalContentBottom()){
            const unsigned y=sRender.row++;
            FillSpan(y,ModalRailLeft(),4,y>=sRender.thumbY&&y<sRender.thumbY+sRender.thumbH?Teal:Line);
            return;
        }
        sRender.stage=4;return;
    }
    if(sRender.stage==0) {
        if(sRender.row<sRender.endRow){BaseRow();return;}
        sRender.stage=sRender.incremental?2:1;sRender.chrome=0;return;
    }
    if(sRender.stage==1) {
        sRender.clipTop=0;sRender.clipBottom=192;
        switch(sRender.chrome++) {
        case 0:SetText(10,8,"kotoGBA B",Paper);return;
        case 1:SetText(108,8,SentenceNotice()?"Oración completa: pág.2 >":CompositionTargetPage()!=65535?((sRender.target.page&0xbfff)==(0x8000|KOTO_TEXT_SLOTS)?"Mensaje completo <":"Partes del mensaje >"):gKanjiLoadStatus==10?"Todas las fichas >":"Copia KANJI.K20",Paper);return;
        case 2:SetText(6,36,"Traducción",sRender.target.tab==0?Paper:Ink);return;
        case 3:SetText(86,36,"Mi vocabulario",sRender.target.tab==1?Paper:Ink);return;
        case 4:SetText(180,36,"Mis kanjis",sRender.target.tab==2?Paper:Ink);return;
        default:sRender.stage=2;sRender.chrome=0;sRender.clipTop=56;sRender.clipBottom=173;return;
        }
    }
    if(sRender.stage==2) {
        if(sRender.target.tab){
            if(gCollectionsLoadStatus>=3&&gCollectionsLoadStatus<=6){
                if(!sRender.chrome++)SetText(12,76,"No se pudieron cargar\nlas colecciones.\nConserva tus archivos de guardado.",Red);
                else{sRender.stage=sRender.incremental?4:3;sRender.chrome=0;}return;}
            if(!sRender.lineCount&&!sRender.chrome++){SetText(12,76,sRender.target.tab==1?"Tu vocabulario está vacío.\nToca una palabra en Traducción\ny toca el disquete verde.":"Todavía no has guardado kanjis.\nAbre una ficha y toca\nel disquete rojo.",Muted);return;}
            if(sRender.line<sRender.lineCount){const unsigned index=sRender.line;const int y=60+index*CRowHeight(sRender.target.tab)-sRender.target.scroll;
                if(y>=sRender.clipBottom){sRender.line=sRender.lineCount;sRender.chrome=0;return;}
                if(y+(sRender.target.tab==1?34:16)<=sRender.clipTop){++sRender.line;sRender.chrome=0;return;}
                if(sRender.target.tab==1){
                    if(sRender.chrome==0){CBeginWordLabel(index);sRender.chrome=1;return;}
                    if(sRender.chrome==1){if(CWordLabelUnit())sRender.chrome=2;return;}
                    if(sRender.chrome==2){sRender.chrome=3;SetText(12,y,CWordLabel(false),Teal);return;}
                    if(sRender.chrome==3){sRender.chrome=4;SetText(130,y,CWordLabel(true),Ink);return;}
                    sRender.chrome=0;++sRender.line;return;
                }
                ++sRender.line;u32 glyph=CListGlyph(sRender.target.tab,index);if(glyph)Glyph(12,y,glyph,Teal);SetText(glyph?34:12,y,CListTitle(sRender.target.tab,index),Teal);return;}
            sRender.stage=sRender.incremental?4:3;sRender.chrome=0;return;
        }
        if(IsSentenceLegacyFull(sRender.target)){if(sRender.line<sRender.lineCount){SentenceLegacyReadLine(sRender.target,sRender.line++);return;}sRender.stage=5;sRender.chrome=0;return;}
        if(NativeFrame(sRender.target)){
            if(!sRender.chrome){sRender.chrome=1;OrthographyRenderBegin();SetText(10,56-sRender.target.scroll,sNativeRenderText,Ink);return;}
            if(OrthographyRenderUnit()){sRender.stage=5;sRender.chrome=0;}return;}
        if(!sRender.card&&!sRender.chrome++){SetText(12,76,gKotoGbaBootMode==1?"Juego sin estudio.\nReinicia y pulsa A para activarlo.":gKotoGbaBootMode?"Estudio no cargado.\nRevisa los archivos de kotoGBA.":sStudyReady?"Este texto aún no tiene ayuda.\nTus colecciones siguen accesibles.":"Actualiza el paquete de lectura.",Muted);return;}
        if(sRender.line<sRender.lineCount) {
            const u32 refs=Read32(40)+Read32(24)*8;
            const unsigned local=sRender.line++;
            unsigned first=sRender.firstLine,shift=0;
            unsigned line=local;
            if(QuestionFrame(sRender.target)){
                const unsigned count=Read16(Read32(36)+8*sRender.target.page+4);
                if(local>=count){const u32 menu=Read32(36)+8*sGameMenuPages[(sRender.target.menu>>4)-1];
                    first=Read32(menu);line-=count;shift=LegacyPrepare(sRender.target)+12;}
            }
            const u32 l=Read32(40)+8*Read16(refs+2*(first+line));
            const int y=56+(int)Read16(l+2)+(shift?shift:LegacyShift(sRender.target,local))-sRender.target.scroll;
            if(y+16>sRender.clipTop && y<sRender.clipBottom)
                SetText(sStudyData[l],y,shift?(const char*)sStudyData+Read32(l+4):LegacyText(sRender.target,local,(const char*)sStudyData+Read32(l+4)),sStudyData[l+1]==0?Teal:Ink);
            return;
        }
        if(QuestionFrame(sRender.target)&&sRender.chrome++==0){
            const unsigned kind=sRender.target.menu>>4,choice=sRender.target.menu&15;
            SetText(2,56+LegacyPrepare(sRender.target)+12+sGameMenuChoiceY[kind-1][choice]-sRender.target.scroll+3,">",Teal);return;
        }
        if(sRender.card&&sRender.target.tab==0&&sRender.target.menu&&sRender.target.page==sGameMenuPages[(sRender.target.menu>>4)-1]&&sRender.chrome++==0){
            const unsigned kind=sRender.target.menu>>4,choice=sRender.target.menu&15;
            int y=56+sGameMenuChoiceY[kind-1][choice]-sRender.target.scroll;
            SetText(2,y+3,">",Teal);return;
        }
        sRender.stage=5;sRender.chrome=0;return;
    }
    if(sRender.stage==5){if(!GrammarRenderUnit())return;sRender.stage=sRender.incremental?4:3;sRender.chrome=0;return;}
    if(sRender.stage==3) {
        sRender.clipTop=0;sRender.clipBottom=192;
        switch(sRender.chrome++) {
        case 0:SetText(8,180,"X vocabulario",Muted);return;
        case 1:SetText(114,180,sRender.number,Ink);return;
        case 2:SetText(94,180,"<",Teal);return;
        case 3:SetText(150,180,sRender.target.tab?"Y deslizar >":"Y página >",Teal);return;
        default:if(sModalKind){sRender.stage=7;sRender.row=sModalTop;}else sRender.stage=4;return;
        }
    }
}
static void PresentFrame()
{
    // Indexed BG3, 256x256; bitmap base in 16-KiB units (0 or 4).
    REG_BG3CNT_SUB=0x4080 | (sRender.back?0x0400:0);
    sFront=sRender.back;sShown=sRender.target;
    if(NativeFrame(sShown)){sNativePublishedRevision=sNativeTextRevision;sNativePublishedSequence=sNativeTextSequence;sNativePublishedCursor=sNativeTextCursor;}
    sBufferFrames[sFront]=sShown;sBufferValid[sFront]=!sModalKind;sHasShown=true;sRender.busy=false;
    if(!sModalKind)sReaderBackdrops[0].active=sReaderBackdrops[1].active=false;
    CompositionPaintPublish();
    LinguisticPaintPublish();
    sDisplayedModalScroll=sShownModalScroll;sModalViewReady=sModalKind!=0;
    GrammarReleaseFrame();
    ++gReaderPresented;
}
static unsigned Scanline(){return *(volatile u16*)0x04000006;}
static bool AudioPending(){return (*(volatile u32*)0x04000214 & (1u<<16))!=0;}
extern "C" { [[gnu::section(".dtcm")]] unsigned gKotoGbaReaderWindowEnd=0; }
extern "C" { [[gnu::section(".dtcm")]] unsigned gKotoGbaReaderInputDone=0; }
static bool SliceExpired(unsigned start)
{
    unsigned now=Scanline();
    unsigned elapsed=now>=start?now-start:now+263-start;
    // Do not clear IF here: the engine must service the pending audio request.
    if(AudioPending()){++gReaderAudioYields;return true;}
    // Parent restoration is a bounded memory copy with no SD or linguistic
    // work. It may use more of VBlank, still yielding at every row for audio
    // and always before the bridge's hard scanline deadline.
    if(elapsed>=(sReaderRestoring?48u:24u) || now<192 || now>=gKotoGbaReaderWindowEnd){++gReaderBudgetYields;return true;}
    return false;
}
#include "KotoGbaCorpus.inc"
#include "KotoGbaLinguistic.inc"
#include "KotoGbaLegacyVariables.inc"
#include "KotoGbaSentenceLegacy.inc"
#include "KotoGbaGrammar.inc"
#include "KotoGbaOrthography.inc"
#include "KotoGbaInlineCards.inc"
#include "KotoGbaCompositionReader.inc"

// Full-screen collection shares the bounded render window.
#include "KotoGbaKanji.inc"

static void RenderSlice(unsigned start)
{
    if(sReaderRestoring){ReaderBackdropSlice(start);return;}
    if(SliceExpired(start))return;
    ReaderFrame desired=DesiredFrame();
    if(InlineHoldReader())return;
    // An identical, already visible literal keeps its positioned touch map.
    // Rebuilding that map on every clock redraw would invalidate a press
    // even though neither the visible caption nor its document changed.
    if(!sModalKind&&!sRender.busy&&CompositionPublishedLiteralCurrent(desired)){
        sCompositionRender.deferred=false;return;
    }
    if(sCompositionRender.deferred&&!sRender.busy)SceneChanged();
    if(sRender.busy&&sCompositionRender.local&&!CompositionLocalPaintCurrent(desired)){
        sCompositionRender.local=false;SceneChanged();return;
    }
    if(!sModalKind&&!sRender.busy&&!CompositionFrameVisible(desired))return;
    if(!sModalKind&&!CompositionContinueRender(desired)){
        const bool grammarChanged=GrammarRequest(desired);
        CorpusPrepare(desired);
        // A pending frozen variable/publication proof may become ready without
        // changing the page, cursor or SD generation. Rebuild the same page.
        if(grammarChanged)SceneChanged();
        OrthographyRequestFrame(desired);
        // A new projection invalidates the composed native layout. Rebuild
        // before painting or publishing its newly positioned reading groups.
        if(NativeFrame(desired)&&sNativeTextPage==0xffff){sHasShown=false;sRender.busy=false;}
    }
    if(!sRender.busy) {
        if(sHasShown && SamePage(desired,sShown) && desired.scroll==sShown.scroll && SameModalShown())return;
        StartRender(desired);
    } else if(!SamePage(desired,sRender.target)||!SameModalContent()) StartRender(desired);
    // Continuous drags coalesce without restarting an unfinished frame.
    // Publish progress, then catch up with the latest finger position.
    if(!ReaderBackdropProtect(sRender.back,start))return;
    for(unsigned units=0;units<512;++units) {
        if(SliceExpired(start))return;
        if(sRender.stage==4){
            if(!sModalKind&&(sCompositionRender.local?!CompositionLocalPaintCurrent(sRender.target):!CompositionFrameVisible(sRender.target))){
                sCompositionRender.local=false;SceneChanged();return;
            }
            PresentFrame();return;
        }
        RenderUnit();
    }
    ++gReaderBudgetYields;
}
static void DrawInitialCard(u32 id)
{
    InitRenderer();StartRender({id,0,0,0,0});
    while(sRender.stage!=4)RenderUnit();
    PresentFrame();
}

// R8: a full bounded fragment, not a 32-byte prefix, is the cache identity.
// All loops resume inside the original shared audio/scanline budget.
struct PageLookup {
    u32 ptr,start,end,page,hash,lower,upper;
    u16 key,size,check,pi,si,starPattern,starText,starBase,best;
    u8 stage,ambiguous;
    u8 text[512];
};
[[gnu::section(".ewram.bss")]] static PageLookup sLookup,sOtherLookup;
static bool ExpandedPointer(u32 ptr){return (ptr>=0x02000000&&ptr<0x02040000)||(ptr>=0x03000000&&ptr<0x03008000);}
static bool TextEnd(u8 c);
// Read only bytes already owned by the game's cache. Recognition never brings
// ROM blocks in or changes the VM's cache from its IRQ window.
// An expanded name changes byte offsets. Count actual page/scroll boundaries
// in the expanded buffer, then find the same boundary in the original string.
// The completed printer cursor remains valid after active becomes zero.
static u32 NativeOriginalCursorUncached(const KotoTextRecord& r,u32 cursor)
{
    const u32 origin=r.origin?r.origin:r.source;
    if(origin<0x08000000||origin>=0x0a000000)return 0;
    if(cursor>=0x08000000&&cursor<0x0a000000)return cursor;
    if(cursor>=0x02200000&&cursor<0x02400000)return cursor-0x02200000+0x08000000;
    if(!ExpandedPointer(cursor)||cursor<r.source||cursor-r.source>1024)return origin;
    u32 end=cursor;if(end>r.source&&TextEnd(*(const u8*)(end-1)))--end;
    unsigned boundary=0;
    for(u32 p=r.source;p<end;++p){const u8 ch=*(const u8*)p;
        if(ch==0xfc){if(++p>=end)break;p+=NativeControlSize(*(const u8*)p);}
        else if(ch==0xf8||ch==0xf9||ch==0xfd||ch==0xf7)++p;
        else if(ch==0xfa||ch==0xfb)++boundary;
        else if(ch==0xff)break;
    }
    for(unsigned i=0;i<1024;++i){u8 ch;if(!NativeRomByte(origin+i,ch))return 0;
        if(ch==0xfc){u8 command;if(!NativeRomByte(origin+ ++i,command))return 0;i+=NativeControlSize(command);}
        else if(ch==0xf8||ch==0xf9||ch==0xfd||ch==0xf7)++i;
        else if(ch==0xfa||ch==0xfb||ch==0xff){if(!boundary||ch==0xff)return origin+i;--boundary;}
    }
    return 0;
}
extern "C" [[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]] u32 kotogba_captureCursor(const KotoTextRecord* record,u32 cursor)
{return record?NativeOriginalCursorUncached(*record,cursor):0;}
// Once a complete fragment has been checked, typewriter cursor advances and
// idle frames reuse its source mapping. Long conversations cannot starve paint.
[[gnu::section(".ewram.bss")]] static u32 sOriginalRecord,sOriginalSequence,sOriginalCursor,sOriginalHash,sOriginalGeneration,sOriginalResult;
static u32 NativeOriginalCursor(const KotoTextRecord& r,u32 cursor)
{
    if(r.cursor)return r.cursor;
    if(!ExpandedPointer(cursor))return NativeOriginalCursorUncached(r,cursor);
    const bool primary=r.window==sPrimaryWindow&&sLookup.stage==4&&cursor>=sLookup.start&&cursor<=sLookup.start+sLookup.size;
    const u32 key=primary?sLookup.start:cursor,hash=primary?sLookup.hash:0;
    if(sOriginalRecord==(u32)&r&&sOriginalSequence==r.sequence&&sOriginalCursor==key&&sOriginalHash==hash&&sOriginalGeneration==gKotoTextGeneration)return sOriginalResult;
    sOriginalRecord=(u32)&r;sOriginalSequence=r.sequence;sOriginalCursor=key;sOriginalHash=hash;sOriginalGeneration=gKotoTextGeneration;
    return sOriginalResult=NativeOriginalCursorUncached(r,cursor);
}
static u32 LegacyOriginalPage(u32 cursor)
{
    if(cursor<0x08000000)return 0;const u32 off=cursor-0x08000000;
    unsigned lo=0,hi=sizeof(sPageKeys)/sizeof(sPageKeys[0]);
    while(lo<hi){const unsigned mid=(lo+hi)/2;if(sPageKeys[mid].start<off)lo=mid+1;else hi=mid;}
    // Only identical aliases overlap in the released table. The final one is
    // its established canonical ID (e.g. the potion label).
    if(lo){const auto& k=sPageKeys[lo-1];if(off<=k.start+k.length)return k.id;}
    return 0;
}
static const KotoTextRecord* NativePointerRecord(u32 ptr,unsigned window,bool async)
{
    if(!gKotoTextRecords||window>=32)return nullptr;
    const unsigned index=async?gKotoTextAsync[window]:gKotoTextLast[window];
    if(index>=KOTO_TEXT_SLOTS||index>=gKotoTextCapacity)return nullptr;const auto& r=gKotoTextRecords[index];
    const unsigned limit=async?1024:r.size&&r.bytes[r.size-1]==0xff?r.size:1024;
    return r.sequence&&r.ready&&r.window==window&&r.tiles==*(volatile const u32*)(0x02020438+12*window)&&ptr>=r.source&&ptr-r.source<=limit?&r:nullptr;
}
static u32 TextBufferBegin(u32 ptr)
{
    if(const auto* r=NativePointerRecord(ptr,sPrimaryWindow,true))return r->source;
    if(const auto* r=NativePointerRecord(ptr,sPrimaryWindow,false))return r->source;
    return ptr>=0x03000000?0x03000000:ptr>=0x02021c88&&ptr<0x02022088?0x02021c88:0x02000000;
}
static u32 TextBufferEnd(u32 ptr){return ptr>=0x03000000?0x03008000:ptr>=0x02021c88&&ptr<0x02022088?0x02022088:0x02040000;}
static const u8* NativeCurrentText(const KotoTextRecord& r,unsigned& size)
{
    // Published captions are immutable snapshots. The game may reuse the
    // expansion buffer for another window while this caption stays visible.
    size=r.size;
    if(kotogba_compositionVersion(&r)){
        const unsigned visible=kotogba_compositionVisibleBytes(&r);if(visible<size)size=visible;
    }
    return r.bytes;
}
static bool TextEnd(u8 c){return c==0xfa||c==0xfb||c==0xff;}
[[gnu::always_inline]] static inline bool WindowExists(unsigned id)
{
    if(id>=32)return false;
    volatile const u8* w=(volatile const u8*)(0x02020430+12*id);
    const u32 tiles=*(volatile const u32*)(w+8);
    return w[0]<4&&w[3]&&w[4]&&w[1]+w[3]<=64&&w[2]+w[4]<=64&&tiles>=0x02000000&&tiles<0x02040000;
}
static bool WindowCovered(unsigned id,unsigned front)
{
    if(id==front||!WindowExists(front))return false;
    volatile const u8* a=(volatile const u8*)(0x02020430+12*id);
    volatile const u8* b=(volatile const u8*)(0x02020430+12*front);
    return a[0]==b[0]&&b[1]<=a[1]&&b[2]<=a[2]&&b[1]+b[3]>=a[1]+a[3]&&b[2]+b[4]>=a[2]+a[4];
}
// BPRJ Rev.1 layout checked against captured RAM; no inference from a cursor
// alone after its owning window has been removed.
static u8 VisibleGameMenu()
{
    volatile const u8* m=(volatile const u8*)0x0203ad5c;
    if(m[5]==0||m[5]>=32||m[6]!=2||m[2]>m[4]||m[4]>4||!WindowExists(m[5]))return 0;
    volatile const u8* w=(volatile const u8*)(0x02020430+12*m[5]);
    if(w[0]!=0)return 0;
    unsigned kind=w[3]==9&&w[4]==4&&m[4]==1?1:
        w[3]==6&&w[4]==4&&m[4]==1?2:w[3]==12&&w[4]==10&&m[4]==4?3:0;
    return kind?(kind<<4)|m[2]:0;
}
static void BeginLookup(PageLookup& state,u32 ptr)
{
    state.ptr=ptr;state.key=0;state.page=0;state.size=0;state.best=0;state.ambiguous=0;state.hash=2166136261u;
    if(ptr>=0x08000000&&ptr<0x09000000){state.stage=3;return;}
    if(!ExpandedPointer(ptr)){state.stage=4;return;}
    state.lower=TextBufferBegin(ptr);state.upper=TextBufferEnd(ptr);state.end=ptr;
    if(ptr>state.lower&&TextEnd(*(volatile const u8*)(ptr-1)))--state.end;
    state.start=state.end;state.stage=1;
}
static u32 Lookup(PageLookup& state,u32 ptr,unsigned sliceStart)
{
    // Direct ROM cursors already have the sorted, exact source-range index.
    // Reuse the same binary lookup as captured text instead of walking every
    // legacy key again after each typewriter advance. Its strict start/end
    // bounds and last-alias winner are identical to the former linear pass.
    if(!ExpandedPointer(ptr)){
        state.ptr=ptr;state.size=0;state.hash=2166136261u;
        state.page=ptr>=0x08000000&&ptr<0x09000000?LegacyOriginalPage(ptr):0;
        state.stage=4;return state.page;
    }
    // A moving typewriter cursor within the same complete snapshot does not
    // force the recognizer to start over on every letter.
    if(!state.stage||ptr!=state.ptr){
        if(state.stage>=3&&state.size&&ExpandedPointer(state.ptr)&&ExpandedPointer(ptr)&&ptr>state.start&&ptr<=state.start+state.size)state.ptr=ptr;
        else BeginLookup(state,ptr);
    }
    if(state.stage==4&&ExpandedPointer(ptr)){state.check=0;state.stage=5;}
    while(state.stage!=4){
        if(SliceExpired(sliceStart))return 0xffffffff;
        if(state.stage==1){
            if(state.start<=state.lower||state.end-state.start>=511||TextEnd(*(volatile const u8*)(state.start-1)))state.stage=2;
            else --state.start;
        }else if(state.stage==2){
            if(state.size>=512||state.start+state.size>=state.upper){state.page=0;state.stage=4;continue;}
            const u8 c=*(volatile const u8*)(state.start+state.size);
            state.text[state.size++]=c;state.hash=(state.hash^c)*16777619u;
            if(TextEnd(c))state.stage=3;
        }else if(state.stage==5){
            if(state.check==state.size){state.stage=4;continue;}
            const unsigned i=state.check++;
            if(state.text[i]!=*(volatile const u8*)(state.start+i))BeginLookup(state,ptr);
        }else if(state.stage==3){
            if(state.key==sizeof(sTextPatterns)/sizeof(sTextPatterns[0])){if(state.ambiguous)state.page=0;state.stage=4;continue;}
            const auto& p=sTextPatterns[state.key];
            // Exact fragments use their complete fingerprint as a fast filter;
            // matching bytes are still verified, so hashes do not establish ID.
            if((!p.variable&&(p.length!=state.size||p.hash!=state.hash))||
               (p.variable&&(p.literals<4||p.literals>state.size))){++state.key;continue;}
            state.pi=0;state.si=0;state.starPattern=65535;state.starText=0;state.stage=6;
        }else{
            const auto& p=sTextPatterns[state.key];
            const u8* pattern=sTextPatternBytes+p.offset;
            bool fail=false,success=false;
            if(state.pi==p.length)success=state.si==state.size;
            if(state.pi<p.length&&pattern[state.pi]==0xfd&&state.pi+1<p.length){
                state.pi+=2;state.starPattern=state.pi;state.starText=state.starBase=state.si;continue;
            }
            if(!success&&state.pi<p.length&&state.si<state.size&&pattern[state.pi]==state.text[state.si]){++state.pi;++state.si;continue;}
            if(!success&&state.starPattern!=65535&&state.starText+1<state.size&&
               !TextEnd(state.text[state.starText])&&state.starText-state.starBase<64){
                state.si=++state.starText;state.pi=state.starPattern;continue;
            }
            fail=!success;
            if(!fail){
                if(p.literals>state.best){state.best=p.literals;state.page=p.canonical;state.ambiguous=0;}
                else if(p.literals==state.best&&state.page!=p.canonical)state.ambiguous=1;
            }
            ++state.key;state.stage=3;
        }
    }
    return state.page;
}
static void SceneChanged()
{
    // A popup owns the published parent until it closes. New corpus/name
    // generations are still recorded and will be rebuilt after CloseWindows.
    if(sModalKind||InlineHoldReader())return;
    if(CompositionDeferSceneChange())return;
    sCompositionRender.local=false;sCompositionRender.deferred=false;
    ScrollMotionCancel();
    sRender.text=nullptr;sGlyph.active=false;
    GrammarReleaseFrame();OrthographyAbort();
    sHasShown=false;sBufferValid[0]=sBufferValid[1]=false;sRender.busy=false;
}
[[gnu::section(".ewram.bss")]] static u32 sLinguisticShownGeneration;
static void LinguisticPaintPublish()
{
    // A completed paint already consumed this verified generation. Do not
    // discard it for a redundant full repaint on the first idle service.
    if(!sModalKind&&sCompositionRender.linguistic==LinguisticGeneration())
        sLinguisticShownGeneration=sCompositionRender.linguistic;
}
// Publish a verified sidecar only between paints. The async reader never
// enters the UI, and this deferred comparison also runs from audio resumes.
static void LinguisticReaderService(unsigned start)
{
    LinguisticSlice(start);
    if(!SliceExpired(start))InlineSavedService();
    const u32 generation=LinguisticGeneration();
    if(generation!=sLinguisticShownGeneration&&!sModalKind&&!sRender.busy&&!InlineHoldReader()){
        sLinguisticShownGeneration=generation;sNativeVersion=0;SceneChanged();
    }
}
#include "KotoGbaNativeRefresh.inc"
static u32 CurrentPage(unsigned sliceStart)
{
    // The Help System covers the game with its own renderer and tilemap.
    // Its captured panels own the reader until the game restores its display.
    // In particular, the tutorial resource is still allocated underneath it.
    if(kotogba_overlayState()){
        sGameMenu=0;
        if(sPrimaryWindow!=255||sOtherCount){
            sPrimaryWindow=255;
            for(auto& window:sSceneWindows)window={0,0};
            sCaptureIndex=0;sOtherCount=0;sOtherLookup.stage=0;SceneChanged();
        }
        sLookup.ptr=0;sLookup.hash=0;sLookup.stage=4;return 0;
    }
    volatile const u8* temp=(volatile const u8*)0x02020010;
    volatile const u8* active=(volatile const u8*)0x02020030;
    sGameMenu=VisibleGameMenu();
    const bool activeValid=active[27]&&WindowExists(active[4]);
    volatile const u8* chosen=activeValid?active:temp;
    bool valid=activeValid||WindowExists(temp[4]);
    if(!valid){chosen=active;valid=WindowExists(active[4]);}
    unsigned primary=valid?chosen[4]:255;
    u32 ptr=valid?*(volatile const u32*)chosen:0;
    if(gKotoTextGeneration){
        const bool async=chosen==active&&active[27];
        // A retained printer pointer is provenance, not proof of visibility.
        // A cleared or covered caption must stop owning a translation until
        // its next publication. Keep its observation record for that redraw.
        const auto* observed=NativePointerRecord(ptr,primary,async);int px,py;
        if(!observed||!NativeVisible(observed-gKotoTextRecords,px,py)||(chosen==active&&WindowCovered(primary,temp[4]))){
            const auto* fallback=NativePointerRecord(*(volatile const u32*)temp,temp[4],false);
            if(WindowExists(temp[4])&&fallback&&NativeVisible(fallback-gKotoTextRecords,px,py)){chosen=temp;primary=temp[4];ptr=*(volatile const u32*)temp;}
            else{primary=255;ptr=0;}
        }
        // The template is a copy made at AddTextPrinter; it never advances.
        // A completed asynchronous printer still points at the visible final
        // fragment. Choose its latest published window before that template.
        u32 latest=0;
        for(unsigned window=0;window<32;++window){
            const unsigned slot=gKotoTextAsync[window];if(slot>=gKotoTextCapacity)continue;
            const auto& record=gKotoTextRecords[slot];
            volatile const u8* printer=(volatile const u8*)(0x02020030+32*window);
            const u32 cursor=*(volatile const u32*)printer;int x,y;
            if(record.speed&&record.speed!=255&&record.sequence>latest&&printer[4]==window&&cursor>=record.source&&cursor-record.source<=1024&&NativeVisible(slot,x,y)){
                latest=record.sequence;chosen=printer;primary=window;ptr=cursor;
            }
        }
    }
    if(sPrimaryWindow!=primary){sPrimaryWindow=primary;for(auto& window:sSceneWindows)window={0,0};sCaptureIndex=0;sOtherCount=0;sOtherLookup.stage=0;SceneChanged();}
    // The tutorial's verified resource owns its text, not its A/B caption.
    volatile const u8* w=(volatile const u8*)0x02020448;
    const u32 res=*(volatile const u32*)0x0203b07c;
    u32 result;
    if(w[0]==0&&w[1]==1&&w[2]==4&&w[3]==28&&w[4]==15&&res>=0x02000000&&res<0x02018000&&!*(volatile const u32*)res){
        const u16 page=*(volatile const u16*)(res+18);result=page<3?9+page:Lookup(sLookup,ptr,sliceStart);
    }else{
        const auto* captured=NativePointerRecord(ptr,primary,true);
        if(!captured)captured=NativePointerRecord(ptr,primary,false);
        if(captured){result=LegacyOriginalPage(captured->cursor);sLookup.hash=CorpusHash(captured->bytes,captured->size);sLookup.stage=4;}
        else result=Lookup(sLookup,ptr,sliceStart);
    }
    if(result==0xffffffff)return result;
    if(!result)result=NativeHint();
    if((sGameMenu>>4)==1&&result!=8)sGameMenu=0;
    if((sGameMenu>>4)==3&&result!=26&&result!=27)sGameMenu=0;
    if(sGameMenu&&!NativeQuestionContext())sGameMenu=0;

    return result;
}

// Capture is resumable and runs after input is accepted. It cannot repeatedly
// consume the beginning of every audio slice ahead of the primary lookup.
static void CaptureOtherWindows(unsigned sliceStart)
{
    if(sRender.busy||!sHasShown||sModalKind||kotogba_overlayState())return;
    volatile const u8* temp=(volatile const u8*)0x02020010;
    while(sCaptureIndex<32){
        if(SliceExpired(sliceStart))return;
        const unsigned i=sCaptureIndex++;
        volatile const u8* printer=(volatile const u8*)(0x02020030+32*i);
        u32 other=printer[4]==i?*(volatile const u32*)printer:0;
        if(temp[4]==i&&(!printer[27]||printer[4]!=i))other=*(volatile const u32*)temp;
        if(i==sPrimaryWindow||!WindowExists(i)||WindowCovered(i,sPrimaryWindow)||(!ExpandedPointer(other)&&(other<0x08000000||other>=0x09000000)))other=0;
        if(sSceneWindows[i].ptr!=other){if(sSceneWindows[i].card){--sOtherCount;SceneChanged();}sSceneWindows[i]={other,0};}
    }
    if(SliceExpired(sliceStart))return;
    if(!sSceneWindows[sOtherWindow].ptr){
        for(unsigned i=0;i<32;++i){sOtherWindow=(sOtherWindow+1)%32;if(sSceneWindows[sOtherWindow].ptr)break;}
    }
    if(sSceneWindows[sOtherWindow].ptr){
        const u32 card=Lookup(sOtherLookup,sSceneWindows[sOtherWindow].ptr,sliceStart);
        if(card==0xffffffff)return;
        if(card!=sSceneWindows[sOtherWindow].card){if(sSceneWindows[sOtherWindow].card)--sOtherCount;if(card)++sOtherCount;sSceneWindows[sOtherWindow].card=card;SceneChanged();}
        sOtherWindow=(sOtherWindow+1)%32;sOtherLookup.stage=0;
    }
    sCaptureIndex=0;
}

extern "C" [[gnu::target("arm")]] void kotogba_initializeReader(u32 gameCode, u8 revision, bool packageInstalled)

{
    const u32 bprj = (u32)'B' | ((u32)'P' << 8) | ((u32)'R' << 16) | ((u32)'J' << 24);
    gKotoGbaPendingCardId = 0;
    sKotoGbaActiveCardId = 0;
    sStudyTab = 0;
    sGameMenu = 0;
    ScrollMotionCancel();
    sCompositionView={};
    sCompositionPending={};
    sCompositionPainting={};sCompositionPublished={};
    sCompositionRender={};
    CompositionVisibilityBegin();
    sNativeCount=sNativeTotal=sNativeCursor=sNativeBuildCount=0;sNativeVersion=sNativeBuildingVersion=0;sNativeMain=sNativeBuildingMain=0;
    sPrimaryWindow=255;sOtherWindow=0;sCaptureIndex=0;sOtherCount=0;
    for(auto& window:sSceneWindows)window={0,0};
    sOtherLookup.stage=0;
    for (auto& page : sStudyPage) page = 0;
    sPreviousInput = 0;
    sLookup.stage=0;
    for (auto& word : sStudyInput) word = 0;
    dc_flushRange((void*)sStudyInput, sizeof(sStudyInput));
    // ARM7 channel 18 installs the shared input address; no response is sent.
    ipc_sendWordDirect(IPC_FIFO_MSG(18, ((u32)sStudyInput) >> 2));
    gKotoGbaRuntimeEnabled = (gameCode == bprj && revision == 1 && packageInstalled) ? 1u : 0u;
    DrawInitialCard(gKotoGbaRuntimeEnabled ? 0 : 0xFFFF);
}

extern "C" [[gnu::target("arm")]] void kotogba_readerBiosError()
{
    gKotoGbaRuntimeEnabled = 0;
    DrawInitialCard(0xFFFE);
}

// This boot gate can be tested independently of the game VM. Emergency mode
// touches no linguistic files and installs no text-capture hooks.
extern "C" [[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]]
bool kotogba_prepareLearning(bool installed,bool bypass)
{
    gKotoGbaBootMode=bypass?1:0;
    if(bypass){kotogba_bootStep(48,0);return false;}
    if(!installed)return false;
    kotogba_loadStudy();
    if(gStudyLoadStatus!=10){gKotoGbaBootMode=2;kotogba_bootStep(49,gStudyLoadStatus);return false;}
    kotogba_loadCorpus();
    if(gCorpusLoadStatus!=10){gKotoGbaBootMode=3;kotogba_bootStep(49,gCorpusLoadStatus);return false;}
    kotogba_loadLinguistic();
    if(!LinguisticAvailable()){gKotoGbaBootMode=4;kotogba_bootStep(49,0x2900);return false;}
    kotogba_loadKanji();
    kotogba_bootStep(43,0);kotogba_loadMemory();
    kotogba_bootStep(44,0);kotogba_loadCollections();
    kotogba_bootStep(46,0);return true;
}

static int ScrollLimit(const u8* study)
{
    const ReaderFrame frame=TranslationFrame(sStudyPage[0],sScroll);
    int height=sStudyTab?CListCount(sStudyTab)*CRowHeight(sStudyTab):IsSentenceLegacyFull(frame)?SentenceLegacyHeight(frame):NativeFrame(frame)?sNativeRenderHeight:study?(sLegacyTextCard==frame.card&&sLegacyTextPage==frame.page?sLegacyTextHeight:Read16(Read32(36)+8*frame.page+6)):0;
    if(!sStudyTab){height+=QuestionExtraHeight(frame);height=GrammarHeight(frame,height);}
    return height>117?height-117:0;
}
static int ModalScrollLimit(){const int height=sModalKind==3?sModalCount:sModalHeight;
    const int visible=sModalBottom-sModalTop-(sModalKind==3?28:ModalVocabulary()?48:12);
    return height>visible?height-visible:0;}
static void DiscardReaderHiddenPaint()
{
    // A fresh gesture starts from the visible image. An older unfinished
    // destination must not publish after the finger has cancelled it.
    sBufferValid[sRender.back]=false;sRender.busy=false;sRender.text=nullptr;sGlyph.active=false;
    GrammarReleaseFrame();
}
static void CloseWindows(){ScrollMotionCancel();GrammarClose();InlineClose();InlineSavedCancel();GrammarReleaseFrame();sModalKind=0;sModalStatus=0;sModalBody=nullptr;sModalViewReady=false;sTapTracking=false;sDragY=-1;sRender.busy=false;sRender.text=nullptr;sGlyph.active=false;sReaderBackdrops[1].active=false;
    if(!ReaderBackdropRestore(0)){sShownModalKind=0;sShownModalStatus=0;sShownSaving=CSaving();}}
// Release the input handler's locals before scene recognition and layout.
// Both paths still use the same audio/scanline budget and existing IRQ stack.
[[gnu::target("arm"),gnu::noinline]] static void ReaderService(unsigned start)
{
    if(sReaderRestoring){ReaderBackdropSlice(start);return;}
    CSlice(start);if(SliceExpired(start))return;
    CorpusSlice(start);if(SliceExpired(start))return;
    LinguisticReaderService(start);if(SliceExpired(start))return;
    // Finish a scroll of the unchanged, published document before scanning
    // every game window again. Capture/document generations invalidate this
    // shortcut immediately; the next settled frame resumes scene discovery.
    const bool scrollPaint=sHasShown&&sScroll!=sShown.scroll&&CompositionContinueRender(DesiredFrame());
    const bool localPaint=sCompositionRender.local&&CompositionContinueRender(DesiredFrame());
    if(!sTapTracking&&!scrollPaint&&!localPaint){NativeRefresh(start);SentenceRefresh();CaptureOtherWindows(start);}
    RenderSlice(start);
}
extern "C" [[gnu::target("arm")]] void kotogba_readerUpdate()
{
    if(!gKotoGbaRuntimeEnabled)return;
    const unsigned start=Scanline();if(start<192||start>=gKotoGbaReaderWindowEnd||AudioPending())return;
    CompositionVisibilityBegin();
    dc_invalidateRange((void*)sStudyInput,sizeof(sStudyInput));
    const u32 current=sStudyInput[0],edge=sStudyInput[1],sequence=edge>>24;
    const bool freshTouch=sequence!=sReaderTouchSequence;
    const u32 input=freshTouch?(current&3)|(edge&0x00fffffcu):current;
    const u32 pressed=((input&~sPreviousInput)&7)|(freshTouch?4u:0u);
    if(pressed)sCompositionPending.ready=false;
    if(pressed&1){gKotoGbaReaderInputDone=1;sReaderTouchSequence=sequence;if(sKOpen)KClose();CloseWindows();sStudyTab=1;sScroll=0;sPreviousInput=input;++sReaderTick;RenderSlice(start);return;}
    if(sKOpen){gKotoGbaReaderInputDone=1;++sReaderTick;sReaderTouchSequence=sequence;sPreviousInput=input;KInput(input,pressed);if(sKOpen){if(!sKTracking||sKMoved)KSlice(start);}else RenderSlice(start);return;}
    // Keep any new pen-down latched until the exact parent is visible. A
    // second brief tap during restoration must not disappear or click through.
    if(sReaderRestoring){gKotoGbaReaderInputDone=1;++sReaderTick;ReaderBackdropSlice(start);return;}
    const u8 oldMenu=sGameMenu;const u32 pending=(sModalKind||(input&7)||sTapTracking)?sKotoGbaActiveCardId:CurrentPage(start);if(pending==0xffffffff||AudioPending())return;
    ++sReaderTick;sPreviousInput=input;sReaderTouchSequence=sequence;
    gKotoGbaReaderInputDone=1;
    if(pending!=sKotoGbaActiveCardId){sKotoGbaActiveCardId=pending;if(!sModalKind&&sStudyTab==0){ScrollMotionCancel();sScroll=0;sDragY=-1;sStudyPage[0]=0;}}
    const u8* study=FindStudy(sKotoGbaActiveCardId);
    if(!study&&sNativeCount)study=FindStudy(255);
    if(study&&sStudyPage[0]>=PageCount(study,0)){sStudyPage[0]=0;if(sStudyTab==0)sScroll=0;}
    if(oldMenu!=sGameMenu&&sStudyTab==0&&!sModalKind){ScrollMotionCancel();sStudyPage[0]=0;sScroll=0;}
    if(pressed&2){ScrollMotionCancel();sTapTracking=false;sDragY=-1;if(sModalKind){int next=sModalScroll+40,max=ModalScrollLimit();sModalScroll=next>max?max:next;}
        else if(sStudyTab){int next=sScroll+80,max=ScrollLimit(study);sScroll=next>max?max:next;}
        else if(study){sStudyPage[0]=(sStudyPage[0]+1)%PageCount(study,0);sScroll=0;}}
    if(input&4){const unsigned x=(input>>8)&255,y=(input>>16)&255;
        // The outside belongs to dismissal, not to a drag or the page below.
        // Commit on pen-down so small pen drift cannot cancel the close.
        // CloseWindows clears tracking; the already consumed input edge keeps
        // this contact from activating a parent tab/link after the popup goes.
        if((pressed&4)&&sModalKind&&(x<sModalLeft||x>=sModalLeft+sModalWidth||y<sModalTop||y>=sModalBottom)){
            CloseWindows();RenderSlice(start);return;
        }
        if(pressed&4){ScrollMotionCancel();InlineSavedCancel();sTapTracking=true;sTapMoved=false;sTapX=x;sTapY=y;sTapTab=sStudyTab;sTapFrame=sShown;sDragY=-1;
            sTapModalScroll=sDisplayedModalScroll;sTapModalReady=ModalVocabulary()&&sHasShown&&sModalViewReady;
            if(!sModalKind&&y>=29&&y<54){sStudyTab=x<85?0:x<170?1:2;sScroll=0;sTapTracking=false;}
            else if(sModalKind?x>=sModalLeft&&x<sModalLeft+sModalWidth&&y>=ModalContentTop()&&y<sModalBottom-(ModalVocabulary()?18:2):y>=56&&y<173){
                sDragY=y;
                const int pendingScroll=sModalKind?sModalScroll:sScroll;
                if(sModalKind){if(sModalViewReady&&SameModalContent()){
                    if(sRender.busy&&sShownModalScroll!=sDisplayedModalScroll)DiscardReaderHiddenPaint();
                    sModalScroll=sDisplayedModalScroll;}}
                else if(sHasShown&&SamePage(DesiredFrame(),sShown)){
                    if(sRender.busy&&sRender.target.scroll!=sShown.scroll)DiscardReaderHiddenPaint();
                    sScroll=sShown.scroll;}
                const bool rail=sModalKind?ModalRailHit(x,y):x>=248;
                ScrollMotionStart(sModalKind?ScrollModal:ScrollReader,sModalKind?sModalScroll:sScroll,y,rail,pendingScroll);
                if(rail){sTapMoved=true;if(sModalKind)sModalScroll=ModalRailScroll(y);else sScroll=ScrollRail(y,56,117,ScrollLimit(study));}
            }
        }
        if(sTapTracking){const int dx=(int)x-sTapX,dy=(int)y-sTapY;if(dx*dx+dy*dy>36)sTapMoved=true;}
        if(sDragY>=0&&sTapMoved){int& value=sModalKind?sModalScroll:sScroll;const int max=sModalKind?ModalScrollLimit():ScrollLimit(study);
            value=sScrollMotion.rail?(sModalKind?ModalRailScroll(y):ScrollRail(y,56,117,max)):ScrollMotionDrag(y,max);sDragY=y;}
    }else{
        sDragY=-1;
        if(sTapTracking){sTapTracking=false;ScrollMotionRelease(sTapMoved);if(!sTapMoved){const unsigned x=sTapX,y=sTapY;
            if(sModalKind){if(x<sModalLeft||x>=sModalLeft+sModalWidth||y<sModalTop||y>=sModalBottom)CloseWindows();
                else if((ModalVocabulary()||sModalKind==3)&&y<sModalTop+18&&x>=sModalLeft+sModalWidth-28)CloseWindows();
                else if(ModalVocabulary()&&y>=sModalBottom-18&&y<sModalBottom-2&&x>=ModalSaveLeft()&&x<ModalSaveLeft()+24){sModalStatus=ToggleCurrentWord()?1:2;sShownModalStatus=255;sRender.busy=false;}
                else if(ModalVocabulary()){if(const u32 cp=ModalGlyphAt(x,y)){if(KOpenGlyph(cp)){KSlice(start);return;}}}
            }else if(y<27&&x>=108){if(SentenceNotice()){sStudyPage[0]=1;sScroll=0;}
                else{const unsigned target=CompositionTargetPage();
                    if(target!=65535){ScrollMotionCancel();sStudyPage[0]=target;sScroll=0;}
                    else if(gKanjiLoadStatus==10){KGo(sKHome,false);KSlice(start);return;}}}
            else if(y>=56&&y<173){const bool published=sHasShown&&sTapTab==sStudyTab&&SamePage(sTapFrame,sShown)&&sTapFrame.scroll==sShown.scroll&&
                (!NativeFrame(sShown)||NativePublishedCoordinates());
                if(sStudyTab==0&&published){u32 card=sShown.card;u16 ref=TouchReference(x,y);if(ref==0xffff)GrammarTouch(x,y,card,ref);if(ref==0xffff&&IsSentenceLegacyFull(sShown))SentenceLegacyTouch(x,y,card,ref);if(ref==0xffff&&NativeFrame(sShown)){if(OrthographyActive(sShown))OrthographyTouch(x,y,card,ref);else CorpusTouch(x,y,card,ref);}
                if(ref!=0xffff)OpenTouchReference(ref,card);
                else if(CompositionTouch(x,y)){}
                else if(NativeFrame(sShown)&&OrthographyActive(sShown)){const u32 cp=OrthographyGlyphAt(x,y);if(cp&&KOpenGlyph(cp)){KSlice(start);return;}}
                }
                else if(sStudyTab&&published){const int rowY=(int)y-60+sTapFrame.scroll;if(rowY>=0){const unsigned index=rowY/CRowHeight(sStudyTab);if(sStudyTab==1)COpenWord(index);else if(COpenKanji(index)){KSlice(start);return;}}}}
            else if(!sStudyTab&&study&&y>=174){if(x>=85&&x<115){sStudyPage[0]=(sStudyPage[0]+PageCount(study,0)-1)%PageCount(study,0);sScroll=0;}else if(x>=150){sStudyPage[0]=(sStudyPage[0]+1)%PageCount(study,0);sScroll=0;}}
        }}else if(sModalKind)sModalScroll=ScrollMotionTick(ScrollModal,sModalScroll,ModalScrollLimit());
        // A canonical-card request temporarily releases the parent's grammar
        // snapshot. Keep its scroll until that exact parent is published again;
        // an unpinned or unpublished height is not a new visible scroll limit.
        else if(sHasShown&&!InlineHoldReader())sScroll=ScrollMotionTick(ScrollReader,sScroll,ScrollLimit(study));
    }
    // A stationary pen owns the published page; finish its tap before a new
    // scene or background repaint can replace the hit-test coordinates.
    if(sTapTracking&&!sTapMoved)return;
    if(!SliceExpired(start))CompositionAutoOpen();
    if(!SliceExpired(start))ReaderService(start);
}

// Audio retries continue the same frame. Recognition/input may need another
// attempt if they yielded, but completed work must not be repeated per refill.
extern "C" [[gnu::target("arm")]] void kotogba_readerResume()
{
    if(!gKotoGbaRuntimeEnabled)return;
    if(!gKotoGbaReaderInputDone){kotogba_readerUpdate();return;}
    const unsigned start=Scanline();if(start<192||start>=gKotoGbaReaderWindowEnd||AudioPending())return;
    if(sReaderRestoring){ReaderBackdropSlice(start);return;}
    CompositionVisibilityBegin();
    // Preserve the published coordinates until a stationary pen releases,
    // including reader retries after an audio refill on the same frame.
    if(sTapTracking&&!sTapMoved)return;
    if(sKOpen&&sKTracking&&!sKMoved)return;
    CompositionAutoOpen();if(SliceExpired(start))return;
    CSlice(start);if(SliceExpired(start))return;
    CorpusSlice(start);if(SliceExpired(start))return;
    LinguisticReaderService(start);if(SliceExpired(start))return;
    if(sKOpen)KSlice(start);else RenderSlice(start);
}

// Interaction may use a larger, still bounded part of VBlank. The bridge
// selects one deadline per display frame; audio retries share that deadline.
// Idle recognition keeps its original eight-line budget, and sound always
// preempts drawing through SliceExpired/GlyphRow before the next small unit.
extern "C" [[gnu::target("arm"),gnu::section(".ewram.text")]] unsigned kotogba_readerBudgetLines()
{
    if(!gKotoGbaRuntimeEnabled)return 8;
    if(sReaderRestoring)return 48;
    if(sKOpen)return sKLoading||!sKHaveShown||(sKShown==sKPage&&
        ((sKTracking&&sKMoved)||sKWantedScroll!=sKShownScroll))?24:8;
    if(sTapTracking&&sTapMoved)return 24;
    if(sModalKind)return !sModalViewReady||sModalScroll!=sDisplayedModalScroll?24:8;
    return sRender.busy||!sHasShown||LinguisticReaderPending()||sScroll!=sShown.scroll?24:8;
}
