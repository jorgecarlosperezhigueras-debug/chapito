// BPRJ1 keeps its GPU register buffer at 03000000. Write-only hardware
// window/scroll registers cannot be used as a reliable selection source.
[[gnu::always_inline]] static inline u16 NativeGpu(unsigned offset)
{return *(volatile const u16*)(0x03000000+offset);}
#include "KotoGbaHelpReader.inc"
[[gnu::section(".vram.native_selection")]] static bool NativePosition(unsigned window,int& x,int& y)
{
    if(!WindowExists(window))return false;
    volatile const u8* w=(volatile const u8*)(0x02020430+12*window);const unsigned bg=w[0];
    const unsigned control=NativeGpu(8+bg*2);
    const int sx=NativeGpu(16+bg*4)&511,sy=NativeGpu(18+bg*4)&511;
    const unsigned size=control>>14;
    const int wide=(size==1||size==3)?512:256,high=(size>=2)?512:256;
    x=(w[1]*8-sx)&(wide-1);y=(w[2]*8-sy)&(high-1);
    if(x>=240)x-=wide;if(y>=160)y-=high;
    return true;
}
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]] static bool NativeTileVisible(unsigned window,unsigned px,unsigned py)
{
    const u8* w=(const u8*)(0x02020430+12*window);
    if(px>=w[3]*8||py>=w[4]*8)return false;
    // A window may begin outside the viewport while later text tiles are on
    // screen. Conversely, owned tiles outside the viewport prove nothing.
    const unsigned control=NativeGpu(8+w[0]*2),size=control>>14;
    const unsigned wide=(size==1||size==3)?64:32,high=size>=2?64:32;
    const unsigned bg=w[0];
    const unsigned sx=(w[1]*8+(px&~7u)-(NativeGpu(16+bg*4)&511))&(wide*8-1);
    const unsigned sy=(w[2]*8+(py&~7u)-(NativeGpu(18+bg*4)&511))&(high*8-1);
    // Wrapped coordinates in the final seven pixels represent a partially
    // visible tile on the left/top edge. Keep the comparison unsigned instead
    // of retaining signed normalized coordinates across the ownership check.
    if((sx>=240&&sx<=wide*8-8)||(sy>=160&&sy<=high*8-8))return false;
    const unsigned tx=(w[1]+px/8)&(wide-1),ty=(w[2]+py/8)&(high-1);
    const unsigned block=(ty/32)*(wide/32)+tx/32;
    const unsigned map=0x06000000+((control>>8)&31)*2048+block*2048;
    const unsigned tile=*(volatile const u16*)(map+2*((ty%32)*32+tx%32))&1023;
    const unsigned expected=(*(const u16*)(w+6)+(py/8)*w[3]+px/8)&1023;
    return tile==expected;
}
// 0: no readable unit; 1: its initial tile may own text; 2: blank prefix.
// Existing selection callers deliberately use this as a boolean predicate.
[[gnu::target("thumb"),gnu::section(".ewram.text"),gnu::noinline]] static unsigned NativeReadableCaption(const KotoTextRecord& r)
{
    unsigned readable=1;
    for(unsigned at=0;at<r.size;++at){const u8 c=r.bytes[at];
        if(c==0xff||c==0xfa||c==0xfb)break;
        if(c==0xfc){if(++at>=r.size)return false;const unsigned n=NativeControlSize(r.bytes[at]);
            if(n>=r.size-at)return false;at+=n;continue;}
        if(c==0xf8){if(++at>=r.size)return false;continue;}
        if(c==0xf9)return at+1<r.size?readable:0; // an extended glyph, not a control
        if(c==0xfd||c==0xf7)return at+1<r.size?readable:0; // a bound name is readable text
        if(c&&c!=0xfe)return readable;
        readable=2;
    }
    return false;
}
[[gnu::target("thumb"),gnu::section(".ewram.text"),gnu::noinline]] static unsigned NativeOrdinaryWidth(unsigned font,unsigned glyph)
{
    if(font<2)return 8;if(font==3||!glyph)return 10;
    if(font!=4)return NativeHelpWidth(font,glyph);
    u8 width=0;NativeRomByte(0x081e2724+glyph,width);return width<=16?width:0;
}
[[gnu::target("thumb"),gnu::section(".ewram.text"),gnu::noinline]] static bool NativeBlankPrefixVisible(const KotoTextRecord& r)
{
    // BPRJ1's FontInfo at 083dea40: ordinary fonts 2/3 add one pixel;
    // fonts 0..3 advance rows by 14, fonts 4/5 by 12. These differ from
    // the direct Help renderer. Per-printer custom spacing is not captured.
    unsigned px=r.x,py=r.y,font=r.font;
    const unsigned spacing=(font==2||font==3),line=font<4?14:12;
    const u8* source=r.bytes;const u8* end=source+r.size;const u8* resume=nullptr;
    while(source<end){unsigned c=*source++;
        if(c==0xff){if(!resume)break;source=resume;end=r.bytes+r.size;resume=nullptr;continue;}
        if(c==0xfa||c==0xfb)break;
        if(c==0xfe){px=r.x;py=(py+line)&255;continue;}
        if(c==0xfc){if(source>=end)break;const unsigned command=*source++,n=NativeControlSize(command);
            if(unsigned(end-source)<n)break;
            if(command==6&&n)font=source[0]&15;
            if(command==13&&n)px=(r.x+source[0])&255;
            if(command==14&&n)py=(r.y+source[0])&255;
            source+=n;continue;}
        if(c==0xfd||c==0xf7){if(source>=end||resume)break;
            const unsigned token=*source+++(c==0xf7?768:0);
            const u32 address=kotogba_variableSource(r.origin,token,r.context);
            if(address<0x02000000||address>=0x02400000)return false;
            // BindVariable owns a bounded 64-byte immutable snapshot. Never
            // use current guest name/counter RAM or skip its drawn glyphs.
            resume=source;source=(const u8*)address;end=source+64;continue;}
        if(c==0xf8){if(source>=end)break;u8 width=0;
            NativeRomByte(0x081c9dd8+4*(*source++)+2,width);px=(px+width+spacing)&255;continue;}
        if(c==0xf9){if(source>=end)break;c=0x100+*source++;}
        const unsigned width=NativeOrdinaryWidth(font,c),height=font==1?16:12;
        if(c)for(unsigned row=py;row<py+height;row=(row&~7u)+8)
            for(unsigned col=px;col<px+width;col=(col&~7u)+8)
                if(NativeTileVisible(r.window,col,row))return true;
        px=(px+width+spacing)&255;
    }
    return false;
}
[[gnu::section(".ewram.text"),gnu::noinline]] static bool NativeVisible(unsigned index,int& x,int& y)
{
    if(index>=KOTO_TEXT_SLOTS||index>=gKotoTextCapacity)return false;
    const auto& r=gKotoTextRecords[index];if(!r.sequence||!r.ready||!r.size||r.size>KOTO_TEXT_BYTES)return false;
    const unsigned readable=NativeReadableCaption(r);if(!readable)return false;
    if(r.bytes[0]==0xff)return false; // empty name slots have no readable page
    // Help keeps ordinary windows allocated while replacing their entire
    // background. Their old tile IDs cannot authorize captions in this scene.
    if(r.window>=KOTO_HELP_MAIN){
        if(r.size<=2&&r.bytes[0]==0xef)return false;
        return NativeHelpVisible(r,x,y);
    }
    if(kotogba_overlayState())return false;
    volatile const u8* w=(volatile const u8*)(0x02020430+12*r.window);
    if(r.tiles!=*(volatile const u32*)(w+8)||!WindowExists(r.window))return false;
    const unsigned display=NativeGpu(0);
    if((display&128)||!(display&(1u<<(8+w[0]))))return false;
    const u32 source=r.origin?r.origin:r.source;
    if(source>=0x083db232&&source<=0x083db2be&&(r.window==1||r.window==2)){
        // The naming UI keeps both alphabets in coincident, opaque windows.
        // Only the front background is visible, though both tilemaps exist.
        const unsigned other=r.window==1?2:1;
        const u8* front=(const u8*)(0x02020430+12*other);
        if(WindowExists(other)&&!memcmp((const void*)(w+1),front+1,4)&&
            (NativeGpu(0)&(1u<<(8+front[0])))&&
            (NativeGpu(8+front[0]*2)&3)<(NativeGpu(8+w[0]*2)&3))return false;
    }
    // Arrow captions are selection state, never vocabulary pages.
    if(r.size<=2&&r.bytes[0]==0xef)return false;
    // The tilemap is the visibility authority. Window IDs do not encode
    // stacking order, and battle windows may live on a scrolled map page.
    // The first owned tile proves readable text only when the caption starts
    // with a glyph. Spaces and an empty first row may be visible while every
    // letter is covered. Preserve glyph-bearing FD/F7/F9 atoms after them.
    if(readable==2){if(!NativeBlankPrefixVisible(r))return false;}
    else if(!NativeTileVisible(r.window,r.x,r.y)){
        // A popup can hide the first tile while another part of this caption
        // remains visible. Check its text rows, bounded by the source/window.
        unsigned line=0,column=0;bool found=false;
        for(unsigned ch=0;ch<r.size&&!found;++ch){const u8 c=r.bytes[ch];
            if(c==0xff||c==0xfa||c==0xfb)break;if(c==0xfe){++line;column=0;continue;}
            // Control operands can contain delimiters or glyph-valued bytes.
            // They must never fabricate extra visible columns.
            if(c==0xfc){if(++ch>=r.size)break;ch+=NativeControlSize(r.bytes[ch]);continue;}
            if(c==0xf8||c==0xf9||c==0xfd){++ch;continue;}
            if(c==0xf7)++ch;
            const unsigned px=r.x+8*column++,py=r.y+13*line;
            // Spaces advance the retained layout but cannot authorize a page.
            if(c)found=NativeTileVisible(r.window,px,py);
        }
        if(!found)return false;
    }
    const unsigned bg=w[0],size=NativeGpu(8+bg*2)>>14;
    const int wide=(size==1||size==3)?512:256,high=size>=2?512:256;
    x=(w[1]*8+r.x-(NativeGpu(16+bg*4)&511))&(wide-1);
    y=(w[2]*8+r.y-(NativeGpu(18+bg*4)&511))&(high-1);
    if(x>=240)x-=wide;if(y>=160)y-=high;
    return true;
}
// A packed point avoids three address-taken outputs on the bounded IRQ stack.
[[gnu::always_inline]] static inline u32 NativeSelectionPoint(int x,int y,unsigned window)
{return x>=0&&x<240&&y>=0&&y<160?0x80000000u|(window<<16)|(unsigned(y)<<8)|unsigned(x):0;}
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]] static u32 NativeMenuSelection()
{
    const unsigned display=NativeGpu(0);
    // Both generic menus and scrolling lists publish an actual arrow draw.
    if(gKotoTextArrowSlot<KOTO_TEXT_SLOTS&&gKotoTextArrowSlot<gKotoTextCapacity){const auto& r=gKotoTextRecords[gKotoTextArrowSlot];int xx,yy;
        const u8* w=(const u8*)(0x02020430+12*r.window);
        // An arrow has one glyph: validate its exact owning tile here rather
        // than nesting the full caption/row scanner inside cursor selection.
        if(r.ready&&r.sequence&&r.size&&r.size<=2&&r.bytes[0]==0xef&&
            r.tiles==*(const u32*)(w+8)&&WindowExists(r.window)&&
            !(display&128)&&(display&(1u<<(8+w[0])))&&NativeTileVisible(r.window,r.x,r.y)){
            const unsigned bg=w[0],size=NativeGpu(8+bg*2)>>14;
            const int wide=(size==1||size==3)?512:256,high=size>=2?512:256;
            xx=(w[1]*8+r.x-(NativeGpu(16+bg*4)&511))&(wide-1);
            yy=(w[2]*8+r.y-(NativeGpu(18+bg*4)&511))&(high-1);
            if(xx>=240)xx-=wide;if(yy>=160)yy-=high;
            if(xx>=0&&xx<236&&yy>=0&&yy<160)return NativeSelectionPoint(xx+4,yy,r.window);}}
    // Generic menus include start, yes/no, rival names and item actions.
    volatile const u8* m=(volatile const u8*)0x0203ad5c;
    if(m[5]<32&&m[2]<=m[4]&&m[4]<32&&m[8]>0&&m[8]<40){
        if(WindowExists(m[5])){
            const unsigned columns=m[9]?m[9]:1;
            const unsigned px=m[0]+(m[2]%columns)*m[7],py=m[1]+(m[2]/columns)*m[8];
            const u8* w=(const u8*)(0x02020430+12*m[5]);
            if(!(display&128)&&(display&(1u<<(8+w[0])))&&NativeTileVisible(m[5],px,py)){
                const unsigned bg=w[0],size=NativeGpu(8+bg*2)>>14;
                const int wide=(size==1||size==3)?512:256,high=size>=2?512:256;
                int x=(w[1]*8+px+4-(NativeGpu(16+bg*4)&511))&(wide-1);
                int y=(w[2]*8+py-(NativeGpu(18+bg*4)&511))&(high-1);
                if(x>=240)x-=wide;if(y>=160)y-=high;
                return NativeSelectionPoint(x,y,m[5]);}}
    }
    return false;
}
// A brightness window can be the menu cursor even when no arrow is drawn.
// Accept only one complete, visible text window enclosed by the contrasting
// rectangle. A fade, partial overlap or coincident windows proves no selection.
struct NativeHighlightWork {volatile u32 display,blend,h,v,result,index,point;int x,y;};
// The highlight selector finishes before caption scanning starts. Reuse the
// same 36 bytes for the resolved route and output coordinates; normalization
// owns only the preceding 512 bytes and neither observer nor painter uses this
// idle tail. No record/route capacity or permanent allocation is added.
struct NativeRefreshWork {NativeRoute route;u32 unused[4];int x,y;};
static_assert(sizeof(NativeRefreshWork)==sizeof(NativeHighlightWork));
static_assert(2*sizeof(NativeRoute)*(KOTO_TEXT_SLOTS+12)+KOTO_NATIVE_RENDER_BYTES+512+sizeof(NativeHighlightWork)<=KOTO_TEXT_SCRATCH);
static_assert((2*sizeof(NativeRoute)*(KOTO_TEXT_SLOTS+12)+KOTO_NATIVE_RENDER_BYTES+512)%alignof(NativeHighlightWork)==0);
[[gnu::target("arm"),gnu::optimize("Os"),gnu::section(".ewram.text"),gnu::noinline]] static u32 NativeHighlightSelection()
{
    // This idle scene-building tail follows the normalized bytes in the same
    // existing scratch allocation. It cannot overlap rendered text or routes.
    if(!sNativeNormalized)return NativeMenuSelection();
    auto& work=*(NativeHighlightWork*)(sNativeNormalized+512);
    work.display=NativeGpu(0);work.blend=NativeGpu(0x50);
    if(!(work.display&0x2000)||(work.display&128)||(work.blend&0xc0)<0x80||
       unsigned((NativeGpu(0x54)&31)-1)>=16||(NativeGpu(0x48)&32)||!(NativeGpu(0x4a)&32))return NativeMenuSelection();
    work.h=NativeGpu(0x40);work.v=NativeGpu(0x44);
    if((work.h>>8)>=(work.h&255)||(work.h&255)>240||(work.v>>8)>=(work.v&255)||(work.v&255)>160)return NativeMenuSelection();
    work.result=0;
    for(work.index=0;work.index<KOTO_TEXT_SLOTS&&work.index<gKotoTextCapacity;++work.index){const auto& r=gKotoTextRecords[work.index];
        if(!r.ready||!r.sequence||!r.size||r.size>KOTO_TEXT_BYTES||!NativeReadableCaption(r)||r.bytes[0]==0xff||(r.size<=2&&r.bytes[0]==0xef))continue;
        const u8* w=(const u8*)(0x02020430+12*r.window);
        if(!WindowExists(r.window)||r.tiles!=*(const u32*)(w+8)||!(work.display&(1u<<(8+w[0])))||
           !(work.blend&NativeGpu(0x48)&NativeGpu(0x4a)&(1u<<w[0])))continue;
        if(!NativePosition(r.window,work.x,work.y)||work.x<int(work.h>>8)||work.y<int(work.v>>8)||
           work.x+w[3]*8>(work.h&255)||work.y+w[4]*8>(work.v&255))continue;
        // Full window containment and the tile bounds prove an on-screen point.
        work.point=0x80000000u|(r.window<<16)|((work.y+r.y)<<8)|(work.x+r.x);
        if(!NativeTileVisible(r.window,r.x,r.y))continue;
        if(work.result&&((work.point^work.result)&0xff0000))return NativeMenuSelection();
        if(!work.result||(work.point&65535)<(work.result&65535))work.result=work.point;
    }
    return work.result?work.result:NativeMenuSelection();
}
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]] static u32 NativeSelection()
{
    if(kotogba_overlayState())return NativeHelpSelection();
    // Options use the hardware highlight window, not the stale generic cursor.
    if(gKotoTextOptionsWindow<32){int xx,yy;
        if(NativePosition(gKotoTextOptionsWindow,xx,yy)){
            const unsigned top=NativeGpu(0x44)>>8;
            const u8* w=(const u8*)(0x02020430+12*gKotoTextOptionsWindow);
            const unsigned display=NativeGpu(0);
            if(!(display&128)&&(display&(1u<<(8+w[0])))&&top>=58&&top<=136&&
                int(top)>=yy&&NativeTileVisible(gKotoTextOptionsWindow,8,top-yy)){
                return NativeSelectionPoint(xx+8,top,gKotoTextOptionsWindow);}}
    }
    // Battle cursors are tile pairs 1/2 in BG0's actual action/move grid.
    const unsigned cnt=NativeGpu(8);
    const unsigned map=0x06000000+((cnt>>8)&31)*2048;
    const int sx=NativeGpu(16)&511,sy=NativeGpu(18)&511;
    const unsigned display=NativeGpu(0);
    const bool battle=!(display&128)&&(display&256)&&(sy==160||sy==320)&&*(volatile const u8*)(0x02020430+12*2+2)==35; // inactive off-screen battle pages stay excluded
    if(battle){
        for(unsigned kind=0;kind<2;++kind)for(unsigned cell=0;cell<4;++cell){
            const unsigned tx=kind?1+8*(cell&1):18+6*(cell&1),ty=kind?55+(cell&2):35+(cell&2);
            const unsigned index=((ty&31)*32+tx)+((ty>>5)*1024);
            const u16 a=*(volatile const u16*)(map+2*index),b=*(volatile const u16*)(map+2*(index+32));
            if((a&1023)==1&&(b&1023)==2){const unsigned x=(tx*8-sx+8)&511,y=(ty*8-sy)&511;
                if(x<240&&y<160)return NativeSelectionPoint(x,y,255);}
        }
    }
    return NativeHighlightSelection();
}

[[gnu::target("thumb"),gnu::noinline]] static void SentenceRequestCorpus(unsigned slot)
{
    const ReaderFrame f={0x80000000u,(u16)(0x8000|slot),0,0,0};CorpusPrepare(f);
}
struct [[gnu::may_alias]] SentenceRouteWork {u32 mask;u16 slot,group;};
[[gnu::target("thumb"),gnu::noinline]] static void SentenceAddRoute()
{
    // The native refresh scratch is idle once its route scan has finished.
    // Keep full-sentence dispatch out of the visibility scanner's stack frame.
    if(!sNativeBuildCount||sNativeBuildCount>=KOTO_TEXT_SLOTS+12||!gKotoTextFragmentMasks)return;
    auto& w=*(SentenceRouteWork*)(sNativeNormalized+512);w.slot=sNativeBuild[0].record;
    if(w.slot>=gKotoTextCapacity)return;
    w.mask=gKotoTextFragmentMasks[w.slot*KOTO_TEXT_FRAGMENT_MASK_STRIDE];
    if(!w.mask||!(w.mask&(w.mask-1)))return;
    w.group=SentenceLegacyGroupFor(gKotoTextRecords[w.slot].origin);
    if(w.group!=65535){
        if((w.mask&SentenceLegacyMask(w.group))!=SentenceLegacyMask(w.group))return;
        CorpusResolve(w.slot,gKotoTextRecords[w.slot].origin,0,0);
    }
    SentenceRequestCorpus(w.slot);if(!CorpusSentenceProven(w.slot,w.group!=65535))return;
    for(unsigned j=sNativeBuildCount;j>1;--j)sNativeBuild[j]=sNativeBuild[j-1];
    const bool normalized=CorpusOrthographyRoute(w.slot,gKotoTextRecords[w.slot].origin);
    const bool legacy=w.group!=65535&&!normalized;
    const u32 owner=legacy?0x40000000u|w.group:0x80000000u|(sNativeBuild[0].owner&0x3fffffffu);
    sNativeBuild[1]={owner,(u16)(legacy?w.slot:0xc000|w.slot),1,w.slot,(u16)gKotoTextRecords[w.slot].sequence};++sNativeBuildCount;
}

[[gnu::always_inline]] static inline bool SentenceRouteTag(const NativeRoute& r)
{return (r.owner&0xc0000000u)==0x40000000u||((r.owner&0x80000000u)&&(r.page&0x4000));}
[[gnu::target("thumb"),gnu::noinline]] static bool SentenceRetainedRoute()
{
    if(sNativeCount<2||!SentenceRouteTag(sNativeRoute[1]))return false;
    const auto& full=sNativeRoute[1];if(full.record>=gKotoTextCapacity)return false;
    const auto& r=gKotoTextRecords[full.record];
    return r.ready&&r.sequence&&(u16)r.sequence==full.reserved&&r.origin&&sCorpusRefs[full.record].seen;
}
[[gnu::target("thumb"),gnu::noinline]] static void SentenceCarryRoute()
{
    if(!sNativeBuildCount||sNativeBuildCount>=KOTO_TEXT_SLOTS+12||!SentenceRetainedRoute())return;
    if(sNativeRoute[0].owner!=sNativeBuild[0].owner||sNativeRoute[0].page!=sNativeBuild[0].page||sNativeRoute[0].record!=sNativeBuild[0].record)return;
    for(unsigned j=sNativeBuildCount;j>1;--j)sNativeBuild[j]=sNativeBuild[j-1];
    sNativeBuild[1]=sNativeRoute[1];++sNativeBuildCount;
}
// Full-source proof/SD preparation runs after the visibility scanner returns,
// so the scanner's bounded stack frame is never retained through that work.
[[gnu::target("thumb"),gnu::noinline]] static void SentenceRefresh()
{
    if(!sNativeCount||sNativeBuildingVersion||sModalKind||SentenceRetainedRoute())return;
    sNativeBuildCount=sNativeCount;memcpy(sNativeBuild,sNativeRoute,sNativeCount*sizeof(NativeRoute));
    if(sNativeBuildCount>1&&SentenceRouteTag(sNativeBuild[1])){
        for(unsigned j=1;j+1<sNativeBuildCount;++j)sNativeBuild[j]=sNativeBuild[j+1];--sNativeBuildCount;
    }
    SentenceAddRoute();
    if(sNativeBuildCount==sNativeCount&&!memcmp(sNativeBuild,sNativeRoute,sNativeCount*sizeof(NativeRoute)))return;
    memcpy(sNativeRoute,sNativeBuild,sNativeBuildCount*sizeof(NativeRoute));sNativeCount=sNativeBuildCount;sNativeTotal=0;
    for(unsigned i=0;i<sNativeCount;++i)sNativeTotal+=sNativeRoute[i].count;
    if(sNativeTotal>255)sNativeTotal=255;
    if(sStudyPage[0]>=sNativeTotal){sStudyPage[0]=0;sScroll=0;}
    SceneChanged();
}

[[gnu::target("thumb"),gnu::section(".ewram.text"),gnu::noinline]] static bool NativeQuestionText(const KotoTextRecord& r)
{
    if(r.size>sizeof(r.bytes))return false;
    for(unsigned i=0;i<r.size;++i){const u8 c=r.bytes[i];
        if(c==0xff||c==0xfa||c==0xfb)break;
        if(c==0xfc){if(++i>=r.size)break;i+=NativeControlSize(r.bytes[i]);continue;}
        if(c==0xf7||c==0xf8||c==0xf9||c==0xfd){++i;continue;}
        if(sNativeCharacters[c]=='?'||sNativeCharacters[c]==0xff1f)return true;
    }
    return false;
}
[[gnu::target("thumb"),gnu::noinline]] static bool NativeQuestion(const KotoTextRecord& r,unsigned selectedWindow)
{
    // A persistent question caption does not prove a question/answer scene.
    // Only the established question menu may promote its primary question.
    return sGameMenu&&selectedWindow<32&&r.window!=selectedWindow&&r.window==sPrimaryWindow&&NativeQuestionText(r);
}
// These source IDs are the reviewed, literal button-help captions in BPRJ1.
// Do not infer this role from an F8/F9 byte: those controls also occur in
// informative descriptions and Pokémon metadata. Keep the complete captions
// available, but after informative text and selected entries.
struct NativeControlHintRole {u32 source;u16 offset,bytes;};
static const NativeControlHintRole sNativeControlHints[]={
    {0x3d8dcd,0,16},
    {0x3d8de0,16,5},
    {0x3d8de8,21,11},
    {0x3d8df4,32,17},
    {0x3d8e08,49,14},
    {0x3d8e18,63,6},
    {0x3d8e36,69,13},
    {0x3d8f4b,82,19},
    {0x3d8f64,101,26},
    {0x3d8f98,127,7},
    {0x3d8f9f,134,15},
    {0x3d8fae,149,6},
    {0x3d8fb4,155,15},
    {0x3db1e8,170,20},
    {0x3db324,190,6},
    {0x3db32a,196,6},
    {0x3db330,202,6},
    {0x3db336,208,7},
    {0x3db33d,215,7},
    {0x3db344,222,6},
    {0x3db34a,228,7},
    {0x3dbb99,235,12},
    {0x3dbba5,247,11},
    {0x3dbbb0,258,7},
    {0x3dbbb7,265,13},
    {0x3dbbc4,278,13},
    {0x3dbbd1,291,7},
    {0x3dbbd8,298,13},
    {0x3dbd17,311,21},
    {0x3dce9e,332,33},
    {0x3de2be,365,13},
};
static const u8 sNativeControlHintBytes[]={
    0x00,0x00,0x00,0xf8,0x0a,0x04,0x27,0x48,0x00,0xf8,0x00,0x09,0x50,0x13,0x02,0xff,0xf8,0x00,0x12,0x38,
    0xff,0xf8,0x00,0x12,0x38,0x00,0xf8,0x01,0x23,0x45,0x29,0xff,0xf8,0x0a,0x04,0x27,0x48,0x00,0xf8,0x00,
    0x12,0x38,0x00,0xf8,0x01,0x05,0x2c,0x29,0xff,0xf8,0x0a,0x04,0x27,0x48,0x00,0xf8,0x00,0xf8,0x01,0x05,
    0x2c,0x29,0xff,0xf8,0x00,0x05,0x2c,0x29,0xff,0xf8,0x0a,0x04,0x27,0x48,0x00,0xf8,0x00,0x09,0x50,0x13,
    0x02,0xff,0xf8,0x0a,0x04,0x27,0x48,0x00,0xf8,0x00,0x09,0x50,0x13,0x02,0x00,0xf8,0x01,0x23,0x45,0x29,
    0xff,0xf8,0x0b,0x04,0x27,0x48,0xf9,0x04,0x9e,0xae,0x8d,0x07,0x28,0x06,0x04,0x00,0xf8,0x00,0x20,0x29,
    0x00,0xf8,0x01,0x23,0x45,0x29,0xff,0xf8,0x04,0x15,0x07,0x3b,0x04,0xff,0xf8,0x00,0x12,0x38,0x19,0x94,
    0xae,0x60,0x00,0xf8,0x01,0x23,0x45,0x29,0xff,0xf8,0x00,0x12,0x38,0x1d,0xff,0xf8,0x00,0x23,0x45,0x29,
    0x00,0xf8,0x01,0x1f,0x04,0x19,0x94,0xae,0x60,0xff,0xf8,0x0c,0x02,0x45,0x03,0x00,0xf8,0x00,0x09,0x50,
    0x13,0x02,0x00,0xf8,0x01,0x23,0x45,0x29,0x00,0xff,0xf8,0x00,0x87,0x52,0x95,0xff,0xf8,0x00,0x05,0x2c,
    0x29,0xff,0xf8,0x00,0x23,0x45,0x29,0xff,0xf8,0x00,0x07,0x28,0x06,0x04,0xff,0xf8,0x00,0x09,0x50,0x13,
    0x02,0xff,0xf8,0x0c,0x02,0x45,0x03,0xff,0xf8,0x0a,0x0e,0x2e,0x10,0x08,0xff,0xf8,0x09,0x07,0x28,0x06,
    0x04,0xf8,0x00,0x24,0x22,0x29,0xff,0x00,0x00,0x00,0x00,0x00,0xf8,0x00,0x24,0x22,0x29,0xff,0xf8,0x0b,
    0x07,0x28,0x06,0x04,0xff,0xf8,0x08,0x07,0x28,0x06,0x04,0xf8,0x00,0x08,0x2c,0x0c,0x08,0xff,0xf8,0x0a,
    0x0e,0x2e,0x10,0x08,0xf8,0x00,0x02,0x2a,0x06,0x04,0xff,0xf8,0x0a,0x0e,0x2e,0x10,0x08,0xff,0xf8,0x0a,
    0x0e,0x2e,0x10,0x08,0xf8,0x00,0x2c,0x0d,0x2a,0x29,0xff,0xf8,0x0a,0x04,0x27,0x48,0x00,0xf8,0x0b,0x07,
    0x28,0x06,0x04,0x00,0xf8,0x00,0xf8,0x01,0x05,0x2c,0x29,0xff,0xf8,0x0b,0x24,0x08,0x19,0x0e,0x12,0x22,
    0x02,0x00,0xf8,0x07,0xbc,0xbf,0xce,0x00,0xf8,0x00,0x78,0xae,0x79,0x5d,0x64,0xa0,0x9d,0x00,0xf8,0x01,
    0x24,0x22,0x29,0x00,0xff,0xf8,0x00,0x09,0x50,0x13,0x02,0x00,0xf8,0x01,0x24,0x22,0x29,0xff,
};
[[gnu::target("thumb"),gnu::section(".ewram.text"),gnu::noinline]] static bool NativeControlHint(const KotoTextRecord& r)
{
    u32 source=r.origin?r.origin:r.source;
    if(source<0x08000000u||source>=0x0a000000u)return false;
    source-=0x08000000u;
    unsigned lo=0,hi=sizeof(sNativeControlHints)/sizeof(sNativeControlHints[0]);
    while(lo<hi){const unsigned mid=(lo+hi)/2;if(sNativeControlHints[mid].source<source)lo=mid+1;else hi=mid;}
    if(lo==sizeof(sNativeControlHints)/sizeof(sNativeControlHints[0])||sNativeControlHints[lo].source!=source)return false;
    const auto& role=sNativeControlHints[lo];
    return r.size==role.bytes&&!memcmp(r.bytes,sNativeControlHintBytes+role.offset,role.bytes);
}
[[gnu::target("thumb"),gnu::section(".ewram.text"),gnu::noinline]] static unsigned NativeReadingRank(const KotoTextRecord& r,int x,int y)
{
    const unsigned xx=x<0?0:x,yy=y<0?0:y;
    // The recap uses an instantaneous printer in its own description window.
    // The live JP playback state and the actual published window must agree;
    // an address in the recap corpus alone cannot promote stale/offscreen text.
    const unsigned recap=*(volatile const u8*)0x0203ad72;
    if((recap==2||recap==3)&&r.window==*(volatile const u8*)0x0203ad78&&
       r.origin>=0x083dbf66&&r.origin<=0x083dc887)return 0;
    if(r.window==KOTO_HELP_DESCRIPTION)return 256; // after the selected entry
    if(r.window==KOTO_HELP_CONTROLS||r.window==KOTO_HELP_TITLE)return 49152+yy*64+xx/4;
    // 64 units per row retains the reading order of the short help captions;
    // their 49152+ rank stays after every normal on-screen text coordinate.
    return NativeControlHint(r)?49152+yy*64+xx/4:512+yy*256+xx;
}
// Stable insertion also reorders the sampled primary route once its actual
// screen coordinates have been observed. Window IDs/publication order are
// not a reading order.
[[gnu::target("thumb"),gnu::noinline]] static void NativeInsert(const NativeRoute& route)
{
    unsigned pos=0;
    while(pos<sNativeBuildCount&&(sNativeBuild[pos].owner!=route.owner||sNativeBuild[pos].page!=route.page))++pos;
    if(pos<sNativeBuildCount){for(unsigned i=pos;i+1<sNativeBuildCount;++i)sNativeBuild[i]=sNativeBuild[i+1];--sNativeBuildCount;}
    if(sNativeBuildCount>=KOTO_TEXT_SLOTS+12)return;
    pos=sNativeBuildCount;while(pos&&sNativeBuild[pos-1].reserved>route.reserved){sNativeBuild[pos]=sNativeBuild[pos-1];--pos;}
    sNativeBuild[pos]=route;++sNativeBuildCount;
}
// Geometry alone also matches ordinary two-choice action menus. Require a
// live, separate answer window and a visible primary question before folding
// answers or promoting a legacy card. Captures, not a retained printer pointer,
// prove that this question still exists on screen.
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]] static bool NativeQuestionContext()
{
    if(!sGameMenu||!gKotoTextRecords||sPrimaryWindow>=32||!WindowExists(sPrimaryWindow))return false;
    const unsigned answers=*(volatile const u8*)0x0203ad61;
    if(answers==sPrimaryWindow||!WindowExists(answers))return false;
    for(unsigned i=0;i<KOTO_TEXT_SLOTS&&i<gKotoTextCapacity;++i){const auto& r=gKotoTextRecords[i];
        if(r.window!=sPrimaryWindow||!r.ready||!r.sequence||!r.size||r.size>KOTO_TEXT_BYTES||!NativeQuestion(r,answers))continue;
        int x,y;if(NativeVisible(i,x,y))return true;
    }
    return false;
}
[[gnu::section(".ewram.text")]] static void NativeRefresh(unsigned start)
{
    if(!gKotoTextRecords||!gKotoTextGeneration||!sNativeNormalized||sModalKind)return;
    const u32 selection=NativeSelection();const bool selected=selection!=0;
    auto& work=*(NativeRefreshWork*)(sNativeNormalized+512);
    const int selectedX=selection&255,selectedY=(selection>>8)&255;
    const unsigned selectedWindow=(selection>>16)&255;
    const bool questionMenu=NativeQuestionContext();
    const unsigned help=kotogba_overlayState();
    const u32 main=help?0:sKotoGbaActiveCardId,signature=(main?main:sLookup.hash)^(selected?((selectedWindow<<24)^(selectedX<<12)^selectedY):0)^(questionMenu?0x40000000u:0)^(help<<28);
    const u32 version=gKotoTextGeneration+CorpusGeneration()+OrthographyGeneration();
    // Tile ownership/scroll may change without another text draw. Recheck a
    // settled scene every four reader frames (previously32), while immediate
    // capture/selection changes still invalidate it on the same service call.
    // Repeated scans must not consume the slice ahead of an unfinished frame.
    if(!sNativeBuildingVersion&&sNativeVersion==version&&sNativeMain==signature&&
       (sRender.busy||!sHasShown||(sReaderTick&3)))return;
    if(sNativeBuildingVersion!=version||sNativeBuildingMain!=signature){
        sNativeCursor=0;sNativeBuildCount=0;sNativeBuildingVersion=version;sNativeBuildingMain=signature;
        if(const u8* card=FindStudy(main))sNativeBuild[sNativeBuildCount++]={main,Card16(card,4),Card16(card,10),65535,(u16)65535};
        if(questionMenu&&sNativeBuildCount)sNativeBuild[0].reserved=0;
    }
    while(sNativeCursor<KOTO_TEXT_SLOTS&&sNativeCursor<gKotoTextCapacity){
        if(SliceExpired(start))return;
        const unsigned i=sNativeCursor++;const auto& r=gKotoTextRecords[i];
        if(!NativeVisible(i,work.x,work.y))continue;
        // The legacy question cards already append their validated answers.
        // A new corpus/native question has no such static layout: preserve
        // its answer captures as pages instead of silently dropping them.
        if(questionMenu&&FindStudy(main)&&r.window==*(volatile const u8*)0x0203ad61)continue;
        const bool async=r.speed&&r.speed!=255;
        // CurrentPage proves the current printer's visible primary window.
        // Keep that dialogue ahead of static captions above it, for both
        // legacy and native routes. A live menu selection retains its role;
        // neither a stale async slot nor a hidden caption can claim priority.
        const bool dialogue=!selected&&async&&r.window<32&&r.window==sPrimaryWindow&&gKotoTextAsync[r.window]==i;
        if(async&&r.window==sPrimaryWindow&&main){
            // The primary card already owns these pages, but it still needs
            // the current printer's frozen variable context and invalidation.
            if(gKotoTextAsync[r.window]==i)for(unsigned j=0;j<sNativeBuildCount;++j)
                if(sNativeBuild[j].owner==main){work.route=CorpusOrthographyRoute(i,r.origin?r.origin:r.source)?NativeResolved(i):sNativeBuild[j];work.route.record=i;
                    work.route.reserved=(questionMenu||dialogue)?0:NativeReadingRank(r,work.x,work.y);
                    if(work.route.owner!=main){for(unsigned k=j;k+1<sNativeBuildCount;++k)sNativeBuild[k]=sNativeBuild[k+1];--sNativeBuildCount;}
                    NativeInsert(work.route);break;}
            continue;
        }
        // FireRed draws its four action labels as one string. The verified
        // action cursor selects one label; retain the complete grid as well.
        if((r.origin?r.origin:r.source)==0x083c0011&&selected&&selectedWindow==255){
            const unsigned cell=(selectedX>=work.x+40?1:0)|(selectedY>=work.y+12?2:0);
            const u16 page=sNativeBattlePages[cell];
            bool duplicate=false;for(unsigned j=0;j<sNativeBuildCount;++j)
                if(sNativeBuild[j].owner==255&&sNativeBuild[j].page==page)duplicate=true;
            if(!duplicate){for(unsigned j=sNativeBuildCount;j;--j)sNativeBuild[j]=sNativeBuild[j-1];
                sNativeBuild[0]={255,page,1,(u16)i,0};++sNativeBuildCount;}
        }
        work.route=NativeResolved(i);auto& route=work.route;
        // On the naming screen the question is the reading anchor; keyboard
        // hints and letters remain available on later pages.
        const u32 source=r.origin?r.origin:r.source;
        if((route.owner&0x80000000u)&&CorpusOrthographyRoute(i,source))
            for(unsigned j=0;j<sNativeBuildCount;++j)if(sNativeBuild[j].owner==main&&sNativeBuild[j].record==65535&&
               LegacyOriginalPage(r.cursor?r.cursor:source)==main){
                for(unsigned k=j;k+1<sNativeBuildCount;++k)sNativeBuild[k]=sNativeBuild[k+1];--sNativeBuildCount;break;}
        const bool nameQuestion=((source==0x083db1b7||source==0x083db1dc||route.owner==8)&&r.window==sPrimaryWindow&&NativeQuestionText(r))||
            (selected&&questionMenu&&NativeQuestion(r,selectedWindow));
        const unsigned rank=(nameQuestion||dialogue)?0:(selected&&(selectedWindow==255||selectedWindow==r.window)&&work.y>=selectedY-3&&work.y<=selectedY+3&&work.x>=selectedX-2)?work.x+1:NativeReadingRank(r,work.x,work.y);
        route.reserved=rank>65535?65535:rank;
        NativeInsert(route);
    }
    CompositionAddRoutes();
    SentenceCarryRoute();
    bool changed=sNativeCount!=sNativeBuildCount||(!main&&sNativeMain!=signature),anchorChanged=false;unsigned total=0;
    for(unsigned i=0;i<sNativeBuildCount;++i){const auto& a=sNativeRoute[i];const auto& b=sNativeBuild[i];
        if(a.owner!=b.owner||a.page!=b.page||a.count!=b.count||a.record!=b.record)changed=true;total+=b.count;
        if(b.record<gKotoTextCapacity){const auto& record=gKotoTextRecords[b.record];const u32 cursor=(b.owner&0x80000000u)?record.cursor:record.context^record.origin;
            if(sNativeRecordCursor[b.record]!=cursor){changed=true;if(!i)anchorChanged=true;sNativeRecordCursor[b.record]=cursor;}}}
    if(changed){
        bool firstChanged=anchorChanged||(sNativeCount&&sNativeBuildCount&&(sNativeRoute[0].owner!=sNativeBuild[0].owner||sNativeRoute[0].page!=sNativeBuild[0].page));
        memcpy(sNativeRoute,sNativeBuild,sNativeBuildCount*sizeof(NativeRoute));sNativeCount=sNativeBuildCount;sNativeTotal=total>255?255:total;
        if(firstChanged&&!sStudyTab){sStudyPage[0]=0;sScroll=0;}
        SceneChanged();
    }
    sNativeVersion=version;sNativeMain=signature;sNativeCursor=0;sNativeBuildingVersion=0;
}
[[gnu::section(".ewram.text")]] static u32 NativeHint()
{
    if(!gKotoTextRecords||sLookup.size>KOTO_TEXT_BYTES)return 0;
    const auto* r=NativePointerRecord(sLookup.ptr,sPrimaryWindow,true);
    if(!r)r=NativePointerRecord(sLookup.ptr,sPrimaryWindow,false);
    if(r&&r->cursor)return LegacyOriginalPage(r->cursor);
    return 0;
}
