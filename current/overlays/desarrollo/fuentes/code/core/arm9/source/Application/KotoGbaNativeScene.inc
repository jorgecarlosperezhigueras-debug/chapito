// Native observations extend the sampled-printer fallback. They are not lexical
// guesses: each label retains the window, coordinates and source of its draw.
struct NativeRoute {u32 owner;u16 page,count;u16 record,reserved;};
static bool CompositionRoute(const NativeRoute& route);
static ReaderFrame CompositionFrame(unsigned parent,unsigned part,int scroll);
[[gnu::section(".ewram.bss")]] static NativeRoute *sNativeRoute,*sNativeBuild;
[[gnu::section(".ewram.bss")]] static u16 sNativeCount,sNativeTotal,sNativeBuildCount,sNativeCursor;
[[gnu::section(".ewram.bss")]] static u32 sNativeVersion,sNativeBuildingVersion,sNativeMain,sNativeBuildingMain;
[[gnu::section(".ewram.bss")]] static char* sNativeRenderText;
[[gnu::section(".ewram.bss")]] static u8* sNativeNormalized;
[[gnu::section(".ewram.text")]] static void InitNativeSceneStorage()
{
 static_assert(2*sizeof(NativeRoute)*(KOTO_TEXT_SLOTS+12)+KOTO_NATIVE_RENDER_BYTES+512<=KOTO_TEXT_SCRATCH);
 static_assert(KOTO_NATIVE_JP_OFFSET+3*KOTO_TEXT_BYTES+KOTO_TEXT_BYTES/14+1<KOTO_NATIVE_RENDER_BYTES);
 u8* area=(u8*)kotogba_textScratch();sNativeRoute=(NativeRoute*)area;
 sNativeBuild=area?sNativeRoute+KOTO_TEXT_SLOTS+12:nullptr;
 sNativeRenderText=area?(char*)(sNativeBuild+KOTO_TEXT_SLOTS+12):nullptr;
 sNativeNormalized=area?(u8*)sNativeRenderText+KOTO_NATIVE_RENDER_BYTES:nullptr;
}
[[gnu::section(".ewram.bss")]] static u16 sNativeRenderHeight;
[[gnu::section(".ewram.bss")]] static u32 sNativeTextCard,sNativeTextSequence,sNativeTextRevision,sNativeTextCursor;
[[gnu::section(".ewram.bss")]] static u32 sNativeRecordCursor[KOTO_TEXT_TOTAL_SLOTS];
[[gnu::section(".ewram.bss")]] static u16 sNativeTextPage,sNativeTextHeight;
[[gnu::section(".ewram.bss")]] static bool sNativeTextOrthography;
[[gnu::section(".ewram.bss")]] static u16 sNativeJapanesePrefixLines;
[[gnu::section(".ewram.bss")]] static u32 sNativePublishedRevision,sNativePublishedSequence,sNativePublishedCursor;
static bool NativePublishedCoordinates()
{return sNativeTextCard==sShown.card&&sNativeTextPage==sShown.page&&sNativeTextRevision==sNativePublishedRevision&&
    sNativeTextSequence==sNativePublishedSequence&&sNativeTextCursor==sNativePublishedCursor;}
[[gnu::section(".ewram.text")]] static bool NativeFrame(const ReaderFrame& f)
{return (f.card&0x80000000u)&&f.page&0x8000;}
[[gnu::section(".ewram.text")]] static unsigned NativeControlSize(u8 command)
{
    // FireRed extended text commands; only their arguments are omitted.
    static const u8 sizes[]={0,1,1,1,3,1,1,0,1,0,0,2,1,1,1,0,2,1,1,1,1,0,0,0,0};
    return command<sizeof(sizes)?sizes[command]:0;
}
[[gnu::section(".ewram.text")]] static unsigned NativeNormalized(const KotoTextRecord& r,u8* out,bool current=false)
{
    unsigned n=0,size=r.size;const u8* input=current?NativeCurrentText(r,size):r.bytes;
    for(unsigned i=0;i<size&&n<512;++i){const u8 c=input[i];
        if(c==0xfc){if(++i==size)break;i+=NativeControlSize(input[i]);continue;}
        if(c==0xf8||c==0xf9){++i;continue;}
        if(c==0xff||c==0xfa||c==0xfb){out[n++]=0xff;break;}
        out[n++]=c;
    }
    return n;
}
[[gnu::section(".ewram.text")]] static NativeRoute NativeResolved(unsigned index)
{
    const auto& r=gKotoTextRecords[index];
    u32 source=r.origin?r.origin:r.source;
    if(source>=0x02200000&&source<0x02400000)source=source-0x02200000+0x08000000;
    if(source<0x08000000||source>=0x0a000000)source=0;
    const bool async=r.speed&&r.speed!=255;
    u32 cursor=r.source+(r.size?r.size-1:0);
    if(async){const u8* printer=(const u8*)(0x02020030+32*r.window);
        if(printer[4]==r.window)cursor=*(const u32*)printer;}
    u32 legacy=0;
    if(const u32 original=NativeOriginalCursor(r,cursor))legacy=LegacyOriginalPage(original);
    u8* bytes=sNativeNormalized;const unsigned n=NativeNormalized(r,bytes,true);u32 hash=2166136261u;
    for(unsigned i=0;i<n;++i)hash=(hash^bytes[i])*16777619u;
    const bool corpus=CorpusResolve(index,source,hash,n);
    // A source-bound R29 document owns the new spelling even for captions
    // which previously selected a static KS5 page. Popup card/ref identities
    // are supplied separately by its exact raw-word table.
    if(CorpusOrthographyRoute(index,source))
        return {0x80000000u|(hash&0x7fffffffu),(u16)(0x8000|index),1,(u16)index,0};
    if(legacy)if(const u8* card=FindStudy(legacy))return {legacy,Card16(card,4),Card16(card,10),(u16)index,0};
    if(corpus){
        const unsigned page=CorpusStaticPage(index);
        if(page!=65535)return {255,(u16)page,1,(u16)index,0};
    }
    // A redraw of the same caption must not reset the reader's chosen page.
    return {0x80000000u|(hash&0x7fffffffu),(u16)(0x8000|index),1,(u16)index,0};
}
[[gnu::section(".ewram.text")]] static ReaderFrame NativeAt(unsigned page,int scroll)
{
    for(unsigned i=0;i<sNativeCount;++i){const auto& r=sNativeRoute[i];
        if(page<r.count){if(CompositionRoute(r))return CompositionFrame(r.record,page,scroll);
            const u8* card=!(r.owner&0x80000000u)?FindStudy(r.owner):nullptr;
            const unsigned preferred=card?PreferredFragment(r.owner,r.count):0;
            ReaderFrame f={r.owner,(u16)(r.page+(page+preferred)%r.count),0,(u8)(r.owner==sKotoGbaActiveCardId?sGameMenu:0),scroll};
            if(NativeFrame(f)&&sGameMenu){f.menu=sGameMenu;if(!OrthographyQuestion(f))f.menu=0;}return f;}
        page-=r.count;
    }
    return {0,0,0,0,scroll};
}
[[gnu::section(".ewram.text")]] static unsigned NativeDecode(const ReaderFrame& f)
{
    const unsigned index=f.page&0x3fff;if(!gKotoTextRecords||index>=gKotoTextCapacity)return 0;
    const auto& record=gKotoTextRecords[index];
    // Publication may be interrupted by a native IRQ. Keep the already
    // composed frame until the observer commits its complete new snapshot.
    if(!record.ready||!record.sequence)return sNativeTextHeight;
    const bool translated=CorpusReady(f);
    if((f.page&0x4000)&&(!translated||!CorpusSentenceProven(index,false))){OrthographyAbort();sNativeRenderText[0]=0;return 0;}
    const u32 revision=(translated?CorpusGeneration():0)+OrthographyGeneration();
    if(sNativeTextCard==f.card&&sNativeTextPage==f.page&&sNativeTextSequence==record.sequence&&sNativeTextRevision==revision&&sNativeTextCursor==record.cursor&&
       (!sNativeTextOrthography||OrthographyActive(f)))return sNativeTextHeight;
    const bool orthography=OrthographyPrepare(f);
    u8* bytes=sNativeNormalized;const unsigned count=NativeNormalized(record,bytes,true);
    CorpusMatchLinks(f,bytes,count);
    unsigned out=0,width=0,lines=1;
    for(unsigned i=0;i<count&&out+5<1536;++i){const u8 ch=bytes[i];if(ch==0xff)break;
        CorpusLinkPosition(i,out);
        if(ch==0xfe){sNativeRenderText[out++]='\n';width=0;++lines;continue;}
        const u32 cp=ch==0xf7?(++i,0x2026u):sNativeCharacters[ch];const unsigned w=cp>255?14:6;
        if(width+w>224){sNativeRenderText[out++]='\n';width=0;++lines;}width+=w;
        if(cp<128)sNativeRenderText[out++]=cp;
        else if(cp<2048){sNativeRenderText[out++]=0xc0|(cp>>6);sNativeRenderText[out++]=0x80|(cp&63);}
        else{sNativeRenderText[out++]=0xe0|(cp>>12);sNativeRenderText[out++]=0x80|((cp>>6)&63);sNativeRenderText[out++]=0x80|(cp&63);}
        CorpusLinkPosition(i+1,out);
    }
    if(orthography){CorpusOrthographyLinks(f);out=0;}
    else if(f.page&0x4000){out=translated?CorpusFullJapanese(f):0;CorpusMatchLinks(f,nullptr,0);}
    sNativeRenderText[out]=0;
    sNativeJapanesePrefixLines=0;
    const unsigned height=translated?CorpusCompose(f,out):orthography?28:14*lines+28;
    sNativeTextCard=f.card;sNativeTextPage=f.page;sNativeTextSequence=record.sequence;sNativeTextRevision=revision;sNativeTextCursor=record.cursor;sNativeTextOrthography=orthography;
    return sNativeTextHeight=OrthographyHeight(f,height,translated);
}
