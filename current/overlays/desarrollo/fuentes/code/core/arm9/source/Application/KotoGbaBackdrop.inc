// R37: retain the published parent of each study overlay. The reader uses
// palette entries 0..15, so lossless nibble packing keeps two parents in the
// unused 64 rows of the two VRAM surfaces plus 2336 bytes of private RAM.
// Capture is lazy: the first popup keeps its original parent bank intact.
// Only a later paint that would overwrite that bank has to pack its pixels.
struct ReaderBackdrop {
    ReaderFrame frame;
    u32 revision, sequence, cursor;
    u16 rows;
    u8 bank;
    bool active, retained, ready, shown;
};
[[gnu::section(".ewram.bss")]] static ReaderBackdrop sReaderBackdrops[2];
[[gnu::section(".ewram.bss")]] alignas(4) static u32 sReaderBackdropTail[584];
[[gnu::section(".ewram.bss")]] static u16 sReaderRestoreRow;
[[gnu::section(".ewram.bss")]] static u8 sReaderRestoreLayer, sReaderRestoreBank;
[[gnu::section(".ewram.bss")]] static bool sReaderRestoring;
[[gnu::section(".ewram.bss")]] static bool sReaderRestoreKanjiHeader;
[[gnu::section(".ewram.bss")]] volatile u32 gReaderFastCloses, gReaderRestoredCloses;

static unsigned ReaderBackdropTop(unsigned layer){return layer?28:56;}
static unsigned ReaderBackdropRows(unsigned layer){return layer?156:128;}
static unsigned ReaderBackdropWidth(unsigned layer){return layer?240:256;}
[[gnu::always_inline]] static inline volatile u32* ReaderBackdropWord(unsigned layer,unsigned word)
{
    if(layer&&word>=4096)return sReaderBackdropTail+word-4096;
    return (volatile u32*)(Surface(layer)+192*128)+word;
}
static void ReaderBackdropRemember(unsigned layer)
{
    ReaderBackdrop& b=sReaderBackdrops[layer];
    b.frame=sShown;b.revision=sNativePublishedRevision;b.sequence=sNativePublishedSequence;b.cursor=sNativePublishedCursor;
    b.rows=0;b.bank=sFront;b.active=true;b.retained=true;b.ready=false;b.shown=sHasShown;
}
// One row is a bounded unit. Word access is required for the VRAM tails.
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]]
static void ReaderBackdropPackRow(unsigned layer)
{
    ReaderBackdrop& b=sReaderBackdrops[layer];
    const unsigned words=ReaderBackdropWidth(layer)/8;
    const volatile u32* src=(const volatile u32*)(Surface(b.bank)+(ReaderBackdropTop(layer)+b.rows)*128+(layer?4:0));
    unsigned out=b.rows*words;
    for(unsigned i=0;i<words;++i){
        u32 a=*src++,c=*src++;
        a&=0x0f0f0f0fu;a=(a|(a>>4))&0x00ff00ffu;a=(a|(a>>8))&0xffffu;
        c&=0x0f0f0f0fu;c=(c|(c>>4))&0x00ff00ffu;c=(c|(c>>8))&0xffffu;
        *ReaderBackdropWord(layer,out++)=a|(c<<16);
    }
    if(++b.rows==ReaderBackdropRows(layer))b.ready=true;
}
static bool ReaderBackdropProtect(unsigned bank,unsigned start)
{
    for(unsigned layer=0;layer<2;++layer){ReaderBackdrop& b=sReaderBackdrops[layer];
        if(!b.active||!b.retained||b.bank!=bank)continue;
        while(!b.ready){if(SliceExpired(start))return false;ReaderBackdropPackRow(layer);}
        b.retained=false;
    }
    return true;
}
static void ReaderBackdropPublish(unsigned layer,unsigned bank)
{
    const ReaderBackdrop& b=sReaderBackdrops[layer];
    REG_BG3CNT_SUB=0x4080|(bank?0x0400:0);sFront=bank;sShown=b.frame;sHasShown=b.shown;
    sNativePublishedRevision=b.revision;sNativePublishedSequence=b.sequence;sNativePublishedCursor=b.cursor;
    sBufferFrames[bank]=sShown;sBufferValid[bank]=!sModalKind;
    sRender.busy=false;sRender.text=nullptr;sGlyph.active=false;
    sShownModalKind=sModalKind;sShownModalStatus=sModalStatus;sShownSaving=CSaving();
    sModalViewReady=sModalKind!=0;sReaderRestoring=false;
    sReaderBackdrops[layer].active=false;++gReaderPresented;
}
static bool ReaderBackdropRestore(unsigned layer)
{
    ReaderBackdrop& b=sReaderBackdrops[layer];
    // X may dismiss both layers while the kanji parent is still restoring.
    // Its upper chrome lies above the vocabulary rectangle; recover that
    // strip from the kanji parent too, rather than copying stale kanji pixels.
    sReaderRestoreKanjiHeader=!layer&&sReaderRestoring&&
        (sReaderRestoreLayer==1||sReaderRestoreKanjiHeader);
    sReaderRestoring=false;
    if(!b.active)return false;
    sRender.busy=false;sRender.text=nullptr;sGlyph.active=false;
    if(b.retained){ReaderBackdropPublish(layer,b.bank);++gReaderFastCloses;return true;}
    if(!b.ready){b.active=false;return false;}
    sReaderRestoreLayer=layer;sReaderRestoreBank=sFront^1;sReaderRestoreRow=0;sReaderRestoring=true;
    sHasShown=false;sBufferValid[sReaderRestoreBank]=false;return true;
}
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]]
static void ReaderBackdropUnpackRow(unsigned layer,unsigned row)
{
    const unsigned words=ReaderBackdropWidth(layer)/8;
    volatile u32* dst=(volatile u32*)(Surface(sReaderRestoreBank)+(ReaderBackdropTop(layer)+row)*128+(layer?4:0));
    unsigned in=row*words;
    for(unsigned i=0;i<words;++i){
        const u32 value=*ReaderBackdropWord(layer,in++);
        u32 a=value&0xffffu,c=value>>16;
        a=(a|(a<<8))&0x00ff00ffu;a=(a|(a<<4))&0x0f0f0f0fu;
        c=(c|(c<<8))&0x00ff00ffu;c=(c|(c<<4))&0x0f0f0f0fu;
        *dst++=a;*dst++=c;
    }
}
static void ReaderBackdropSlice(unsigned start)
{
    if(!sReaderRestoring)return;
    const unsigned layer=sReaderRestoreLayer,top=ReaderBackdropTop(layer),bottom=top+ReaderBackdropRows(layer);
    if(!ReaderBackdropProtect(sReaderRestoreBank,start))return;
    while(sReaderRestoreRow<192){
        if(SliceExpired(start))return;
        const unsigned y=sReaderRestoreRow++;
        if(y>=top&&y<bottom){
            if(layer){
                mem_copy32((const void*)(Surface(sFront)+y*128),(void*)(Surface(sReaderRestoreBank)+y*128),8);
                mem_copy32((const void*)(Surface(sFront)+y*128+124),(void*)(Surface(sReaderRestoreBank)+y*128+124),8);
            }
            ReaderBackdropUnpackRow(layer,y-top);
        }else if(sReaderRestoreKanjiHeader&&y>=28&&y<56){
            mem_copy32((const void*)(Surface(sFront)+y*128),(void*)(Surface(sReaderRestoreBank)+y*128),8);
            mem_copy32((const void*)(Surface(sFront)+y*128+124),(void*)(Surface(sReaderRestoreBank)+y*128+124),8);
            ReaderBackdropUnpackRow(1,y-28);
        }else mem_copy32((const void*)(Surface(sFront)+y*128),(void*)(Surface(sReaderRestoreBank)+y*128),256);
    }
    if(Scanline()+2>=gKotoGbaReaderWindowEnd)return;
    ReaderBackdropPublish(layer,sReaderRestoreBank);++gReaderRestoredCloses;
}
