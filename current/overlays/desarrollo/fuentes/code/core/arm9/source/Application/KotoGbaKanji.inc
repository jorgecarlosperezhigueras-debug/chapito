// D20 continuous documents. Metadata/glyphs load at startup; pixels stream.
#include "KotoGbaSupplementalGlyphsR29.generated.h"
#include "KotoGbaSupplementalGlyphsR32.generated.h"
#include "KotoGbaSupplementalGlyphsHitoA.generated.h"
// The unchanged K20 has at most 188 links per document. Reserve 192 without
// changing any document, touch rectangle, navigation state or pixel surface.
enum { KHitCapacity=192 };
struct KDiskHit {u16 x1,y1,x2,y2,page,reserved;};
struct KHit {u16 x1,y1,x2,y2,page;};
static_assert(sizeof(KDiskHit)==12&&sizeof(KHit)==10);
// K20 keeps its original 32-byte descriptors. Only the bounded hit count is
// narrowed in RAM, after validating it; no document or metadata is discarded.
struct KDiskDoc {u32 offset,pixels;u16 height,hits;u8 kind,flags;u16 glyph;char title[16];};
// Aligned u32 disk byte offsets need at most 23 sector bits. Keep the disk
// format intact. Two 23-bit sector numbers and the validated two-bit kind
// occupy six bytes, retaining every field without another RAM allocation.
struct [[gnu::packed]] KDoc {u32 offset:23,pixels:23,kind:2;u16 height;u8 hits,flags;u16 glyph;char title[16];};
static_assert(sizeof(KDiskDoc)==32&&sizeof(KDoc)==28&&KHitCapacity<=255);
struct KIndex {u16 glyph,page;u8 pixels[56];};
struct KHistory {u16 page,scroll;};
// Cache only the metadata for the two existing VRAM surfaces. No additional
// framebuffer or unbounded allocation is needed, and game RAM is untouched.
struct alignas(4) KBuffer {u16 palette[256];KHit hits[KHitCapacity];u16 page,scroll;bool valid;u8 saveState;};
[[gnu::section(".ewram.bss")]] static KBuffer sKBuffers[2];
[[gnu::section(".ewram.bss")]] alignas(32) static u8 sKIo[4096];
[[gnu::section(".ewram.bss")]] static DWORD sKClusters[2048];
[[gnu::section(".ewram.bss")]] static KIndex sKIndex[2400];
[[gnu::section(".ewram.bss")]] static KDoc sKDocs[3200];
[[gnu::section(".ewram.bss")]] static KHit sKHits[KHitCapacity];
[[gnu::section(".ewram.bss")]] static u16 sKPalette[256];
[[gnu::section(".ewram.bss")]] static KHistory sKHistory[32];
[[gnu::section(".ewram.bss")]] static FsWaitToken sKToken;
[[gnu::section(".ewram.bss")]] volatile u32 gKanjiLoadStatus;
[[gnu::section(".ewram.bss")]] static u32 sKPages,sKSize,sKDataBase,sKReadSerial,sKSerial;
[[gnu::section(".ewram.bss")]] static u16 sKCount,sKPage,sKHome,sKShown,sKHistoryCount,sKChunk,sKCopied,sKClusterSize,sKMetaDoc;
[[gnu::section(".ewram.bss")]] static u16 sKScroll,sKWantedScroll,sKShownScroll,sKRow,sKPixelPos,sKMetaPos,sKSkip;
[[gnu::section(".ewram.bss")]] static u16 sKTapX,sKTapY;
[[gnu::section(".ewram.bss")]] static u16 sKTapPage,sKTapScroll,sKCachePos;
[[gnu::section(".ewram.bss")]] static u16 sKThumbY,sKThumbH;
[[gnu::section(".ewram.bss")]] static int sKDragY;
[[gnu::section(".ewram.bss")]] static u8 sKDevice,sKBack,sKPhase,sKChrome;
[[gnu::section(".ewram.bss")]] static bool sKOpen,sKPending,sKLoading,sKHaveChunk,sKRepaint,sKTracking,sKMoved;
// Error bit and current-document membership share the existing one-byte state.
// Repainting must not rescan 2400 entries after a complete list mutation.
[[gnu::section(".ewram.bss")]] static u8 sKSaveFlags; // bit 0: failed; bit 1: saved
[[gnu::section(".ewram.bss")]] static bool sKHaveShown,sKReuse,sKReuseChrome;
[[gnu::section(".ewram.bss")]] static bool sKScreenHit,sKPressValid,sKDragging;
[[gnu::section(".ewram.bss")]] static u8 sKSaveState;

#include "KotoGbaCollections.inc"

extern "C" [[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline,gnu::optimize("Os")]] void kotogba_loadKanji()
{
    gKanjiLoadStatus=1;sKOpen=false;sKPending=false;sKLoading=false;sKMetaDoc=65535;sKHaveShown=false;
    sKBuffers[0].valid=sKBuffers[1].valid=false;sKPressValid=false;
    FIL f{};UINT got=0;
    kotogba_bootStep(40,0);
    if(f_open(&f,"/_gba/kotogba/kanji/KANJI.K20",FA_READ)!=FR_OK)return;
    gKanjiLoadStatus=2;sKSize=f_size(&f);
    bool ok=f_read(&f,sKIo,512,&got)==FR_OK&&got==512;
    const u32* h=(const u32*)sKIo;u32 desc=h[4],glyph=h[5];
    if(!ok||memcmp(sKIo,"KOTOKJ20",8)||h[2]!=sKSize){f_close(&f);return;}
    sKPages=h[3];sKCount=h[6];sKHome=h[7];
    ok=sKPages&&sKPages<=3200&&sKCount<=2400&&sKHome<sKPages&&desc==512&&!(glyph&511)&&glyph>=desc+sKPages*32&&glyph+sKCount*60<=sKSize;
    for(unsigned base=0;ok&&base<sKPages;){
        unsigned count=sKPages-base;if(count>sizeof(sKIo)/sizeof(KDiskDoc))count=sizeof(sKIo)/sizeof(KDiskDoc);
        ok=BootRead(f,sKIo,count*sizeof(KDiskDoc),41);
        const KDiskDoc* disk=(const KDiskDoc*)sKIo;
        for(unsigned i=0;ok&&i<count;++i){const KDiskDoc& d=disk[i];
            ok=d.height>=102&&d.hits<=KHitCapacity&&d.kind<=3&&d.title[15]==0&&!(d.offset&511)&&!(d.pixels&511)&&d.offset<=d.pixels&&512+d.hits*sizeof(KDiskHit)<=d.pixels-d.offset&&d.pixels<=sKSize&&d.height*256u<=sKSize-d.pixels;
            if(ok){KDoc& out=sKDocs[base+i];out.offset=d.offset>>9;out.pixels=d.pixels>>9;out.height=d.height;out.hits=(u8)d.hits;out.kind=d.kind;out.flags=d.flags;out.glyph=d.glyph;memcpy(out.title,d.title,sizeof(out.title));}
        }
        base+=count;
    }
    if(ok)ok=f_lseek(&f,glyph)==FR_OK&&BootRead(f,sKIndex,sKCount*60,41);
    for(unsigned i=0;ok&&i<sKCount;++i)ok=sKIndex[i].page<sKPages&&(!i||sKIndex[i-1].glyph<sKIndex[i].glyph);
    gKanjiLoadStatus=3;
    kotogba_bootStep(42,0);
    if(ok){sKClusters[0]=sizeof(sKClusters)/sizeof(*sKClusters);f.cltbl=sKClusters;ok=f_lseek(&f,CREATE_LINKMAP)==FR_OK;
        if(f.obj.fs){sKDevice=f.obj.fs->pdrv;sKDataBase=f.obj.fs->database;sKClusterSize=f.obj.fs->csize;}
        else ok=false;
        ok=ok&&sKDevice<=1&&sKClusterSize&&!(sKClusterSize&(sKClusterSize-1));
    }
    f_close(&f);if(ok)gKanjiLoadStatus=10;
}
static unsigned KFindGlyph(u32 cp)
{unsigned lo=0,hi=gKanjiLoadStatus==10?sKCount:0;while(lo<hi){unsigned m=(lo+hi)/2;if(sKIndex[m].glyph<cp)lo=m+1;else hi=m;}return lo;}
static const u8* KGlyphPixels(u32 cp)
{if(const u8* pixels=HitoASupplementalGlyphPixels(cp))return pixels;
 if(const u8* pixels=R32SupplementalGlyphPixels(cp))return pixels;
 if(const u8* pixels=R29SupplementalGlyphPixels(cp))return pixels;
 unsigned i=KFindGlyph(cp);return i<sKCount&&gKanjiLoadStatus==10&&sKIndex[i].glyph==cp?sKIndex[i].pixels:nullptr;}
static void KView()
{
    sGlyph.active=false;
    const bool repaint=sKRepaint;
    // Reuse only pixels from a fully published view of this same document.
    // The front buffer stays immutable until the next atomic publication.
    const int delta=(int)sKWantedScroll-(int)sKShownScroll;
    sKReuse=sKHaveShown&&sKShown==sKPage&&sKMetaDoc==sKPage&&delta>-102&&delta<102;
    const u8 nextSaveState=sKSaveFlags&1?3:sKSaveFlags&2?(CSaving()?1:2):0;
    sKReuseChrome=sKReuse&&!sKRepaint&&sKSaveState==nextSaveState;
    ++sKSerial;sKLoading=true;sKHaveChunk=false;sKPhase=0;sKRow=0;sKPixelPos=0;sKMetaPos=0;sKChrome=0;sKRepaint=false;
    sKScroll=sKWantedScroll;sKBack=sFront^1;sRender.busy=false;sHasShown=false;sRender.text=nullptr;sRender.back=sKBack;
    sBufferValid[0]=sBufferValid[1]=false;
    sKSaveState=nextSaveState;
    KBuffer& cached=sKBuffers[sKBack];
    sKScreenHit=cached.valid&&cached.page==sKPage&&cached.scroll==sKScroll;
    cached.valid=false;sKCachePos=0;
    if(sKScreenHit){sKReuseChrome=!repaint&&cached.saveState==nextSaveState;if(sKReuseChrome)sKRow=192;}
}
static void KGo(u16 page,bool push,u16 scroll=0)
{
    if(gKanjiLoadStatus!=10||page>=sKPages)return;
    if(!sKOpen)ReaderBackdropRemember(1);
    ScrollMotionCancel();
    if(push&&sKOpen){if(sKHistoryCount==32){memmove(sKHistory,sKHistory+1,31*sizeof(KHistory));--sKHistoryCount;}sKHistory[sKHistoryCount++]={sKPage,sKWantedScroll};}
    if(!sKOpen)sKHistoryCount=0;
    sKOpen=true;sKPage=page;sKWantedScroll=scroll;sKTracking=false;sKSaveFlags=CIsKanjiSaved(page)?2:0;sKDragY=-1;sTapTracking=false;sDragY=-1;
    const unsigned max=sKDocs[page].height-102;if(sKWantedScroll>max)sKWantedScroll=max;KView();
}
static bool KOpenGlyph(u32 cp)
{if(gKanjiLoadStatus!=10)return false;
 unsigned i=KFindGlyph(cp);
 if(i<sKCount&&sKIndex[i].glyph==cp){KGo(sKIndex[i].page,false);return true;}
 // Hito B supplements reuse the already loaded descriptor table. Their exact
 // Unicode glyph is authoritative; no alias, Heisig number or extra glyph
 // cache is needed. Historical pages and collection ordinals stay unchanged.
 for(unsigned page=0;page<sKPages;++page)
     if((sKDocs[page].flags&0x80)&&sKDocs[page].kind==1&&sKDocs[page].glyph==cp){KGo(page,false);return true;}
 return false;}
static bool COpenKanji(unsigned index)
{if(index>=sSavedKanjiCount||sSavedKanjis[index]>=sKPages)return false;KGo(sSavedKanjis[index],false);return true;}
static void KClose()
{ScrollMotionCancel();sKOpen=false;sKLoading=false;sKHaveChunk=false;sKHistoryCount=0;sKTracking=false;sKHaveShown=false;sKBuffers[0].valid=sKBuffers[1].valid=false;sRender.busy=false;sHasShown=false;sTapTracking=false;sDragY=-1;sBufferValid[0]=sBufferValid[1]=false;ReaderBackdropRestore(1);}
static void KBack()
{if(sKHistoryCount){const KHistory h=sKHistory[--sKHistoryCount];KGo(h.page,false,h.scroll);}else KClose();}
static bool KRequest(u32 pos,unsigned count)
{
    if(sKPending)return false;
    const u32 logical=pos/512,within=logical&(sKClusterSize-1);u32 cluster=logical/sKClusterSize;const DWORD* t=sKClusters+1;
    const DWORD* end=sKClusters+sizeof(sKClusters)/sizeof(*sKClusters)-1;
    while(t<end&&*t&&cluster>=*t){cluster-=*t;t+=2;}if(t>=end||!*t){KClose();return false;}
    if(count>8)count=8;if(count>sKClusterSize-within)count=sKClusterSize-within;
    const u32 sector=sKDataBase+(t[1]+cluster-2)*sKClusterSize+within;
    if(!fs_tryRead(sKDevice==0?FS_DEVICE_DLDI:FS_DEVICE_DSI_SD,sKIo,sector,count,&sKToken))return false;
    sKChunk=count*512;sKReadSerial=sKSerial;sKPending=true;return true;
}
// Metadata reads may split a 12-byte disk hit across two sectors. Keep the
// streaming position in disk bytes and omit only each reserved sixth u16.
// All five useful fields stay exact; both caches retain all 192 rectangles.
[[gnu::target("arm"),gnu::noinline]] static void KCopyHitBytes(unsigned position,const u8* source,unsigned bytes)
{
    typedef u16 HitWord [[gnu::may_alias]];
    const unsigned hit=position/sizeof(KDiskHit);
    unsigned field=(position-hit*sizeof(KDiskHit))/2;
    HitWord* out=(HitWord*)((u8*)sKHits+hit*sizeof(KHit))+field;
    for(unsigned i=0;i<bytes;i+=2){
        if(field<5)*out++=source[i]|(source[i+1]<<8);
        if(++field==6)field=0;
    }
}
static void KSlice(unsigned start)
{
    if(sKPending){if(!fs_pollRead(&sKToken))return;sKPending=false;if(sKReadSerial==sKSerial&&sKLoading){sKHaveChunk=true;sKCopied=sKSkip;}}
    if(!sKOpen)return;
    if(!sKLoading){if(sKRepaint||sKWantedScroll!=sKShownScroll)KView();else return;}
    const KDoc& d=sKDocs[sKPage];sRender.back=sKBack;
    if(!ReaderBackdropProtect(sKBack,start))return;
    // Glyph rows are now separate units; the scanline/audio budget still
    // bounds the work, while this cap must allow a complete small burst.
    for(unsigned units=0;units<512;++units){
        if(SliceExpired(start))return;
        if(sGlyph.active){GlyphRow();continue;}
        if(sKPhase==0){
            if(sKRow<192){const unsigned y=sKRow++;
                if(sKScreenHit&&y==52){sKRow=154;continue;}
                const bool body=y>=52&&y<154;
                if(!sKScreenHit&&body){
                    // Every body pixel is replaced in phase 3. Preserve only
                    // the outside margins instead of copying/clearing it twice.
                    mem_copy32((const void*)(Surface(sFront)+y*128),(void*)(Surface(sKBack)+y*128),8);
                    mem_copy32((const void*)(Surface(sFront)+y*128+124),(void*)(Surface(sKBack)+y*128+124),8);
                }else if(!sKScreenHit||y<28||y>=184)mem_copy32((const void*)(Surface(sFront)+y*128),(void*)(Surface(sKBack)+y*128),256);
                if(!sKReuseChrome&&!body&&y>=28&&y<184){FillSpan(y,8,240,y<52?Ink:Paper);Pixel(8,y,Teal);Pixel(247,y,Teal);
                    if(d.kind){
                        if(y>=166&&y<176)SaveControlRow(226,y,y-166,Red,sKSaveState);
                    }
                }
                continue;
            }
            if(sKScreenHit){const KBuffer& cached=sKBuffers[sKBack];memcpy(sKPalette,cached.palette,512);memcpy(sKHits,cached.hits,d.hits*sizeof(KHit));sKMetaDoc=sKPage;sKPixelPos=102*256;sKPhase=3;}
            else sKPhase=sKMetaDoc==sKPage?3:1;
            continue;
        }
        if(sKPhase==1){
            if(!sKHaveChunk){sKSkip=0;KRequest(d.offset<<9,(512+d.hits*sizeof(KDiskHit)+511)/512);return;}
            // A split metadata read replaces the palette before all hits arrive.
            // Cancelling here must not reuse the old document's metadata tag.
            sKMetaDoc=65535;
            memcpy(sKPalette,sKIo,512);
            unsigned n=sKChunk-512;if(n>d.hits*sizeof(KDiskHit))n=d.hits*sizeof(KDiskHit);
            if(n)KCopyHitBytes(0,sKIo+512,n);
            sKMetaPos=n;sKHaveChunk=false;sKPhase=n==d.hits*sizeof(KDiskHit)?3:2;
            if(sKPhase==3)sKMetaDoc=sKPage;
            continue;
        }
        if(sKPhase==2){
            if(!sKHaveChunk){sKSkip=0;KRequest((d.offset<<9)+512+sKMetaPos,(d.hits*sizeof(KDiskHit)-sKMetaPos+511)/512);return;}
            unsigned n=d.hits*sizeof(KDiskHit)-sKMetaPos;if(n>sKChunk)n=sKChunk;KCopyHitBytes(sKMetaPos,sKIo,n);sKMetaPos+=n;sKHaveChunk=false;
            if(sKMetaPos==d.hits*sizeof(KDiskHit)){sKMetaDoc=sKPage;sKPhase=3;}continue;
        }
        if(sKPhase==3){
            if(sKPixelPos==102*256){
                if(sKReuseChrome){sKPhase=5;sKRow=52;if(d.height>102){sKThumbH=102*102/d.height;if(sKThumbH<8)sKThumbH=8;sKThumbY=52+sKScroll*(102-sKThumbH)/(d.height-102);}else{sKThumbH=102;sKThumbY=52;}continue;}
                sKPhase=4;sRender.clipTop=28;sRender.clipBottom=184;sRender.text=nullptr;continue;}
            const unsigned viewRow=sKPixelPos/256;
            const int previousRow=(int)sKScroll+(int)viewRow-(int)sKShownScroll;
            if(sKReuse&&previousRow>=0&&previousRow<102){
                mem_copy32((const void*)(Surface(sFront)+(52+previousRow)*128+4),
                           (void*)(Surface(sKBack)+(52+viewRow)*128+4),240);
                sKHaveChunk=false;sKPixelPos+=256;continue;
            }
            if(!sKHaveChunk){const unsigned row=sKScroll+viewRow;sKSkip=(row&1)*256;
                unsigned rows=102-viewRow;
                if(sKReuse&&previousRow<0&&(unsigned)(-previousRow)<rows)rows=-previousRow;
                KRequest((d.pixels<<9)+(row&~1u)*256,(rows*256+sKSkip+511)/512);return;}
            if(sKCopied<sKChunk&&sKPixelPos<102*256){mem_copy32(sKIo+sKCopied+8,(void*)(Surface(sKBack)+(52+sKPixelPos/256)*128+4),240);sKCopied+=256;sKPixelPos+=256;continue;}
            sKHaveChunk=false;continue;
        }
        if(sKPhase==4){
            if(sRender.text&&*sRender.text){u32 cp=DecodeUtf8(sRender.text);Glyph(sRender.x,sRender.y,cp,sRender.color);sRender.x+=cp>255?14:6;continue;}
            sRender.text=nullptr;
            switch(sKChrome++){
            case 0:SetText(14,35,"< Volver",Paper);continue;
            case 1:SetText(78,35,d.title,Paper);continue;
            case 2:SetText(230,35,"x",Paper);continue;
            case 3:if(!d.kind)SetText(32,164,"Arrastra para leer",Muted);continue;
            case 4:if(sKSaveState==3)SetText(14,166,"Error al guardar",Red);continue;
            default:sKPhase=5;sKRow=52;if(d.height>102){sKThumbH=102*102/d.height;if(sKThumbH<8)sKThumbH=8;sKThumbY=52+sKScroll*(102-sKThumbH)/(d.height-102);}else{sKThumbH=102;sKThumbY=52;}continue;
            }
        }
        if(sKPhase==5){if(sKScreenHit)sKRow=154;if(sKRow<154){const unsigned y=sKRow++;const u8 color=y>=sKThumbY&&y<sKThumbY+sKThumbH?Teal:Line;Pixel(244,y,color);Pixel(245,y,color);continue;}sKPhase=6;}
        if(sKPhase==6){
            KBuffer& cached=sKBuffers[sKBack];const unsigned total=512+d.hits*sizeof(KHit);
            if(!sKScreenHit&&sKCachePos<total){
                unsigned n=total-sKCachePos;if(n>128)n=128;
                if(sKCachePos<512)memcpy((u8*)cached.palette+sKCachePos,(u8*)sKPalette+sKCachePos,n);
                else memcpy((u8*)cached.hits+sKCachePos-512,(u8*)sKHits+sKCachePos-512,n);
                sKCachePos+=n;continue;
            }
            cached.page=sKPage;cached.scroll=sKScroll;cached.saveState=sKSaveState;sKPhase=7;
        }
        // Leave margin for the atomic palette/base publication in VBlank.
        if(Scanline()+2>=gKotoGbaReaderWindowEnd){++gReaderBudgetYields;return;}
        for(unsigned i=16;i<256;++i)((volatile u16*)0x05000400)[i]=sKPalette[i];
        REG_BG3CNT_SUB=0x4080|(sKBack?0x0400:0);sFront=sKBack;sKShown=sKPage;sKShownScroll=sKScroll;sKLoading=false;sKHaveShown=true;sKBuffers[sKBack].valid=true;++gReaderPresented;return;
    }
    ++gReaderBudgetYields;
}
static unsigned KHitAt(unsigned x,unsigned docY)
{
    unsigned target=65535,best=19;bool tied=false;
    for(unsigned i=0;i<sKDocs[sKTapPage].hits;++i){const KHit& h=sKHits[i];
        if(h.page>=sKPages||(int)docY<(int)h.y1-3||docY>=h.y2+3u||(int)x<(int)h.x1-3||x>=h.x2+3u)continue;
        if(x>=h.x1&&x<h.x2&&docY>=h.y1&&docY<h.y2)return h.page;
        const int dx=x<h.x1?h.x1-x:x>=h.x2?x-h.x2+1:0;
        const int dy=docY<h.y1?h.y1-docY:docY>=h.y2?docY-h.y2+1:0;
        const unsigned distance=dx*dx+dy*dy;
        if(distance<best){best=distance;target=h.page;tied=false;}
        else if(distance==best&&target!=h.page)tied=true;
    }
    return tied?65535:target;
}
static void KInput(u32 input,u32 pressed)
{
    if(pressed&2){ScrollMotionCancel();sKTracking=false;sKDragging=false;sKDragY=-1;unsigned n=sKWantedScroll+80,max=sKDocs[sKPage].height-102;sKWantedScroll=n>max?max:n;}
    if(input&4){unsigned x=(input>>8)&255,y=(input>>16)&255;
        // Dismiss the top kanji layer on the initial outside contact. The
        // reader has consumed this pen-down edge, so its vocabulary parent
        // remains open and cannot receive the rest of the same contact.
        if((pressed&4)&&(x<8||x>=248||y<28||y>=184)){KClose();return;}
        if(pressed&4){ScrollMotionCancel();sKTracking=true;sKMoved=false;sKDragging=false;sKTapX=x;sKTapY=y;
            sKPressValid=sKHaveShown&&sKShown==sKPage&&sKMetaDoc==sKShown;
            sKTapPage=sKShown;sKTapScroll=sKShownScroll;
            sKDragY=x>=8&&x<248&&y>=52&&y<154?(int)y:-1;
            if(sKDragY>=0){
                if(sKPressValid){
                    if(sKLoading&&sKScroll!=sKShownScroll){
                        // Keep the published image and hit metadata. An old
                        // SD request still drains, but its serial is stale.
                        ++sKSerial;sKLoading=false;sKHaveChunk=false;
                        sKBuffers[sKBack].valid=false;sGlyph.active=false;sRender.text=nullptr;
                    }
                    sKWantedScroll=sKShownScroll;
                }
                const bool rail=x>=244&&x<248;
                ScrollMotionStart(ScrollKanji,sKWantedScroll,y,rail);
                if(rail){sKDragging=sKMoved=true;sKWantedScroll=ScrollRail(y,52,102,sKDocs[sKPage].height-102);}
            }
        }
        if(sKTracking){const int dx=(int)x-sKTapX,dy=(int)y-sKTapY;
            if(dx>8||dx<-8||dy>6||dy<-6)sKMoved=true;
            if(sKDragY>=0&&(dy>6||dy<-6))sKDragging=true;
            if(sKDragging){
                const int max=sKDocs[sKPage].height-102;
                sKWantedScroll=sScrollMotion.rail?ScrollRail(y,52,102,max):ScrollMotionDrag(y,max);
            }
        }
        return;
    }
    sKDragY=-1;if(!sKTracking){sKWantedScroll=ScrollMotionTick(ScrollKanji,sKWantedScroll,sKDocs[sKPage].height-102);return;}
    sKTracking=false;ScrollMotionRelease(sKDragging);if(sKMoved)return;
    const unsigned x=sKTapX,y=sKTapY;
    // Dismiss only the kanji layer. Its vocabulary parent keeps its scroll,
    // selected entry and save status, so another kanji can be opened directly.
    if(x<8||x>=248||y<28||y>=184){KClose();return;}
    if(y<52){if(x<74)KBack();else if(x>=218)KClose();return;}
    if(y>=154){if(y>=162&&y<180&&x>=220&&x<244&&sKDocs[sKPage].kind){if(!CToggleKanji(sKPage))sKSaveFlags|=1;sKRepaint=true;}return;}
    if(!sKPressValid||sKPage!=sKTapPage||sKMetaDoc!=sKTapPage||x>=244)return;
    const unsigned target=KHitAt(x,y-52+sKTapScroll);
    if(target<sKPages){sKWantedScroll=sKTapScroll;KGo(target,true);}
}
