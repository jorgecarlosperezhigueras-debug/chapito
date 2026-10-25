// A component is a secondary reading page, never a fabricated game capture.
// Keep all64 physical observations; slot64 is a single reader-owned projection
// of an exact, currently published constructor witness.
enum {COMPOSITION_SLOT=KOTO_TEXT_SLOTS,COMPOSITION_ROUTE_PAGE=0xbfc0};
struct CompositionView {
    KotoCompositionPart part;
    u32 version,parentSequence,sequence;
    u16 parent,index;
    bool ready;
};
[[gnu::section(".ewram.bss")]] static CompositionView sCompositionView;
[[gnu::section(".ewram.bss")]] static KotoCompositionPart sCompositionPartWork;
[[gnu::section(".ewram.bss")]] static u32 sCompositionRouteSeen[2];
struct CompositionPending {KotoCompositionPart part;u32 version,parentSequence,owner;u16 page,parent,index;bool ready;};
[[gnu::section(".ewram.bss")]] static CompositionPending sCompositionPending;
struct CompositionVisibilityCache {u32 generation;u8 window;bool valid,visible;};
[[gnu::section(".ewram.bss")]] static CompositionVisibilityCache sCompositionVisibility;
struct CompositionPaintProof {KotoCompositionPart first;u32 version,parentSequence;u16 parent,count,prefixLines;bool ready;};
[[gnu::section(".ewram.bss")]] static CompositionPaintProof sCompositionPainting,sCompositionPublished;
struct CompositionRenderStamp {
    // Global corpus generation for nonlocal paints; the last source-reference
    // sequence authorized by its writer against the retained local document.
    // These modes are exclusive, so the witness needs no additional RAM.
    u32 text,corpusOrValidatedSequence,linguistic;
    bool ready,local,deferred,orthography,literal;
    u32 document,selection,main,key;
    KotoTextRecord capture;
};
[[gnu::section(".ewram.bss")]] static CompositionRenderStamp sCompositionRender;
[[gnu::target("thumb"),gnu::noinline]] static bool CompositionLiteral(const KotoTextRecord& r,bool translated,bool orthography)
{
    if(!r.size||r.size>KOTO_TEXT_BYTES||r.bytes[r.size-1]!=0xff||kotogba_compositionVersion(&r))return false;
    for(unsigned i=0;i+1<r.size;++i)if(r.bytes[i]>=0xf7)return false;
    if(translated){const auto& ref=sCorpusRefs[&r-gKotoTextRecords];
        if(ref.hash||r.origin!=r.source||r.source<0x08000000u||r.source>=0x0a000000u||
           ref.key!=r.source-0x08000000u||ref.entry.start!=ref.key||ref.entry.end-ref.entry.start!=r.size||
           Corpus32(sCorpusDocument+16)!=r.size||Corpus32(sCorpusDocument+24)||
           memcmp(sCorpusDocument+32,r.bytes,r.size))return false;
    }else if(r.origin||r.context||orthography||!ExpandedPointer(r.source))return false;
    if(orthography){unsigned bytes;const u8* raw=LinguisticRaw(bytes);
        if(!raw||bytes!=r.size||memcmp(raw,r.bytes,bytes)||LinguisticFragmentCount()>1)return false;}
    return true;
}
[[gnu::target("thumb"),gnu::noinline]] static void CompositionPaintBegin(const ReaderFrame& frame)
{
    // Assign scalar fields directly; the retained record must never become
    // a large aggregate temporary on the bounded IRQ stack.
    auto& stamp=sCompositionRender;stamp.text=gKotoTextGeneration;stamp.corpusOrValidatedSequence=CorpusGeneration();stamp.linguistic=LinguisticGeneration();
    stamp.ready=!frame.tab&&NativeFrame(frame)&&((frame.page&0x3fff)!=COMPOSITION_SLOT||sCompositionView.ready);
    stamp.local=stamp.deferred=stamp.orthography=stamp.literal=false;
    // A clock/counter can redraw unrelated labels every game frame. Retain
    // an ordinary immutable caption's paint only with its own authority and
    // the exact document it consumed. Index-sector generations are not a
    // replacement of that document. Dependent notes and async/composed text
    // keep the original global-generation contract below.
    const unsigned slot=frame.page&0x3fff;
    if(!sModalKind&&sCompositionRender.ready&&!(frame.page&0x4000)&&
       slot<KOTO_TEXT_SLOTS&&slot<gKotoTextCapacity&&!LinguisticNoteCount()){
        const auto& r=gKotoTextRecords[slot];const auto& ref=sCorpusRefs[slot];
        const bool translated=CorpusReady(frame);
        if(r.ready&&r.sequence&&r.window<32&&(!r.speed||r.speed==255)&&
           (translated||(ref.ready==2&&ref.sequence==r.sequence))){
            memcpy(&sCompositionRender.capture,&r,sizeof(r));
            sCompositionRender.document=translated?sCorpusDocIndex:0xffffffffu;
            sCompositionRender.orthography=OrthographyCaptureCurrent(frame,r);
            sCompositionRender.literal=CompositionLiteral(r,translated,sCompositionRender.orthography);
            sCompositionRender.key=ref.key;sCompositionRender.corpusOrValidatedSequence=r.sequence;
            sCompositionRender.selection=NativeSelection();
            sCompositionRender.main=sKotoGbaActiveCardId;sCompositionRender.local=true;
        }
    }
    if(sModalKind){sCompositionPainting=sCompositionPublished;return;}
    auto& proof=sCompositionPainting;proof.ready=false;
    if(frame.tab||!NativeFrame(frame)||(frame.page&0x4000))return;
    const unsigned parent=frame.page&0x3fff;if(parent>=KOTO_TEXT_SLOTS||parent>=gKotoTextCapacity)return;
    const auto* record=gKotoTextRecords+parent;const u32 version=kotogba_compositionVersion(record);
    const unsigned count=kotogba_compositionCount(record);
    if(!version||!count||!kotogba_compositionPart(record,0,&proof.first))return;
    proof.version=version;proof.parentSequence=record->sequence;proof.parent=parent;proof.count=count;proof.ready=true;
}
static void CompositionPaintPublish(){if(!sModalKind)sCompositionPainting.prefixLines=sNativeJapanesePrefixLines;sCompositionPublished=sCompositionPainting;}
[[gnu::target("thumb"),gnu::noinline]] static bool CompositionFrameVisible(const ReaderFrame& frame)
{
    const unsigned slot=frame.page&0x3fff;
    if(!NativeFrame(frame)||slot>=KOTO_TEXT_SLOTS)return true;
    int x,y;return NativeVisible(slot,x,y);
}
[[gnu::target("thumb"),gnu::optimize("Oz"),gnu::noinline]] static u32 CompositionRetainedCurrent(const ReaderFrame& frame,bool published)
{
    const auto& p=sCompositionRender;const unsigned slot=frame.page&0x3fff;
    if(!p.local||!SameModalContent()||sModalKind||
       slot>=KOTO_TEXT_SLOTS||slot>=gKotoTextCapacity||p.main!=sKotoGbaActiveCardId||
       p.linguistic!=LinguisticGeneration()||LinguisticNoteCount())return false;
    if(published){
        // The exact capture and layout which reached BG3 remain the witness.
        // No sequence, record, corpus reference or coordinate is rewritten.
        if(!p.literal||!sHasShown||!SamePage(frame,sShown)||frame.scroll!=sShown.scroll||
           !NativePublishedCoordinates()||p.capture.sequence!=sNativePublishedSequence||
           p.capture.cursor!=sNativePublishedCursor)return false;
    }else if(!sRender.busy||!SamePage(frame,sRender.target))return false;
    const auto& r=gKotoTextRecords[slot];const auto& ref=sCorpusRefs[slot];
    const bool repeated=r.sequence!=p.capture.sequence;
    if(repeated){
        // A new observation may prove exactly the already painted literal.
        // Keep the original document/orthography witness; never rewrite the
        // game's capture, CorpusRef or published coordinates to manufacture it.
        if(!p.literal||!r.sequence||kotogba_compositionVersion(&r))return false;
    }
    // Sequence is validated separately. Both modes must compare every other
    // header byte before trusting the retained payload length.
    if(memcmp(&p.capture,&r,12)||memcmp((const u8*)&p.capture+16,(const u8*)&r+16,16))return false;
    if(memcmp(p.capture.bytes,r.bytes,r.size)||!CompositionFrameVisible(frame)||p.selection!=NativeSelection())return false;
    // A newly active dialogue must regain its priority before another paint.
    for(unsigned window=0;window<32;++window){const unsigned i=gKotoTextAsync[window];
        if(i<gKotoTextCapacity){const auto& a=gKotoTextRecords[i];
            if(a.ready&&a.sequence&&a.speed&&a.speed!=255)return false;}}
    // A stationary touch may span another identical recapture while native
    // discovery is paused. Only the reference sequence authorized by its
    // actual writer can carry the published authority across that pause.
    if(ref.sequence!=p.corpusOrValidatedSequence)return false;
    if(ref.key!=p.key)return false;
    if(p.document!=0xffffffffu){
        if(sCorpusDocIndex!=p.document)return false;
        if(repeated){
            // A scan of a recaptured literal can start an index lookup while
            // its already verified document remains loaded. That pending
            // lookup neither revokes nor replaces the published witness.
            // A revoked old reference, a completed miss or a different
            // document still fails; no reference field is rewritten here.
            if(ref.hash)return false;
            if(ref.ready==1){if(ref.index!=p.document)return false;}
            else if(ref.ready||!published||ref.sequence==p.capture.sequence||ref.index!=0xffffffffu)return false;
        }
        else if(!CorpusReady(frame))return false;
    }else if(ref.ready!=2||CorpusReady(frame))return false;
    if(p.orthography&&(!sOrthography.pinned||!OrthographyCaptureCurrent(frame,p.capture)))return false;
    // Return the validated current capture, without committing any witness.
    return r.sequence;
}
[[gnu::always_inline]] static inline bool CompositionLocalPaintCurrent(const ReaderFrame& frame)
{return CompositionRetainedCurrent(frame,false);}
static u32 CompositionPublishedLiteralCurrent(const ReaderFrame& frame)
{return CompositionRetainedCurrent(frame,true);}
[[gnu::always_inline]] static inline void CompositionResolveReference(unsigned slot,u32 key,bool hash)
{
    // CorpusResolve calls this immediately before replacing this exact
    // reference. Prove the new capture against the still-authorized old
    // reference and retained publication, then commit without a budget yield.
    // Rendering and input only consult the pure predicate above.
    auto& p=sCompositionRender;
    if(!sRender.busy&&!hash&&p.document!=0xffffffffu&&key==p.key&&
       slot==(sShown.page&0x3fff)){
        const u32 sequence=CompositionPublishedLiteralCurrent(sShown);
        if(sequence)p.corpusOrValidatedSequence=sequence;
    }
}
static bool CompositionDeferSceneChange()
{
    if(!sCompositionRender.local)return false;
    // Resolving a newly selected constructor part can itself change scene.
    // That nested change must abort normally instead of entering this guard.
    sCompositionRender.local=false;const ReaderFrame desired=DesiredFrame();sCompositionRender.local=true;
    // Other routes can change while this exact page remains on screen.
    // A completed image must pass the full published-authority checks.
    if(!CompositionRetainedCurrent(desired,!sRender.busy))return false;
    sCompositionRender.deferred=true;return true;
}
// DesiredFrame revalidates a constructed page's exact part and window. The
// same generation stamp also protects ordinary native pages: an unfinished
// paint must not repeat document/grammar/layout preparation before each
// eight-line slice. A new capture or document re-enters preparation.
static bool CompositionContinueRender(const ReaderFrame& frame)
{
    if(sCompositionRender.local)return CompositionLocalPaintCurrent(frame);
    const unsigned slot=frame.page&0x3fff;
    return sRender.busy&&sCompositionRender.ready&&SamePage(frame,sRender.target)&&SameModalContent()&&
        NativeFrame(frame)&&slot<gKotoTextCapacity&&gKotoTextRecords[slot].ready&&gKotoTextRecords[slot].sequence&&
        sCompositionRender.text==gKotoTextGeneration&&
        sCompositionRender.corpusOrValidatedSequence==CorpusGeneration()&&sCompositionRender.linguistic==LinguisticGeneration();
}
[[gnu::target("thumb"),gnu::noinline]] static bool CompositionPaintCurrent(unsigned parent,u32 version)
{
    const auto& proof=sCompositionPublished;
    if(!proof.ready||proof.parent!=parent||proof.version!=version||
       proof.parentSequence!=gKotoTextRecords[parent].sequence||
       proof.count!=kotogba_compositionCount(gKotoTextRecords+parent))return false;
    KotoCompositionPart first;
    return kotogba_compositionPart(gKotoTextRecords+parent,0,&first)&&!memcmp(&first,&proof.first,sizeof(first));
}
// The guest is paused throughout the reader IRQ and its audio-resume slices.
// Clear at every reader entry; never repeat a whole tilemap scan per glyph.
static void CompositionVisibilityBegin(){sCompositionVisibility.valid=false;}
static bool CompositionRoute(const NativeRoute& route)
{return route.page==COMPOSITION_ROUTE_PAGE&&(route.owner&0xffff0000u)==0xc0310000u;}
[[gnu::target("arm"),gnu::optimize("O2"),gnu::noinline]] static bool CompositionWholeWindowVisible(unsigned window)
{
    if(window>=32||!WindowExists(window))return false;
    const u8* w=(const u8*)(0x02020430+12*window);
    const volatile u16* gpu=(const volatile u16*)0x03000000;const unsigned bg=w[0];
    if(bg>=4)return false;
    const unsigned control=gpu[4+bg],size=control>>14,wide=(size==1||size==3)?64:32,high=size>=2?64:32;
    const unsigned sx=(w[1]*8-(gpu[8+bg*2]&511))&(wide*8-1),sy=(w[2]*8-(gpu[9+bg*2]&511))&(high*8-1);
    if(sx+w[3]*8>240||sy+w[4]*8>160)return false;
    const unsigned base=0x06000000+((control>>8)&31)*2048;
    unsigned expected=*(const u16*)(w+6);
    for(unsigned row=0;row<w[4];++row){
        const unsigned y=(w[2]+row)&(high-1),blockRow=(y>>5)*(wide>>5);
        unsigned x=w[1]&(wide-1),remaining=w[3];
        while(remaining){
            unsigned run=32-(x&31);if(run>remaining)run=remaining;
            const volatile u16* tile=(const volatile u16*)(base+(blockRow+(x>>5))*2048+2*((y&31)*32+(x&31)));
            for(unsigned i=0;i<run;++i)if((tile[i]&1023)!=(expected++&1023))return false;
            remaining-=run;x=(x+run)&(wide-1);
        }
    }
    return true;
}
[[gnu::target("thumb"),gnu::noinline]] static bool CompositionWindowVisible(unsigned window)
{
    auto& cache=sCompositionVisibility;
    if(!cache.valid||cache.window!=window||cache.generation!=gKotoTextGeneration){
        cache.window=window;cache.generation=gKotoTextGeneration;
        cache.visible=CompositionWholeWindowVisible(window);cache.valid=true;
    }
    return cache.visible;
}
[[gnu::target("thumb"),gnu::noinline]] static bool CompositionPartVisible(unsigned parent,const KotoCompositionPart& part)
{
    if(parent>=KOTO_TEXT_SLOTS||parent>=gKotoTextCapacity)return false;
    const auto& record=gKotoTextRecords[parent];int x,y;
    if(!NativeVisible(parent,x,y)||part.rawStart>=part.rawEnd||part.rawEnd>record.size||
       part.rawEnd-part.rawStart>=KOTO_TEXT_BYTES||part.origin<0x08000000u||part.origin>=0x0a000000u)return false;
    // Require every tile of the actual published window. This deliberately
    // suppresses component pages if even another part of its window is
    // covered; approximate glyph widths must not grant hidden context.
    return CompositionWindowVisible(record.window);
}
[[gnu::target("thumb"),gnu::noinline]] static unsigned CompositionVisibleCount(unsigned parent)
{
    if(parent>=KOTO_TEXT_SLOTS||parent>=gKotoTextCapacity)return 0;
    const auto* record=gKotoTextRecords+parent;const unsigned count=kotogba_compositionCount(record);unsigned result=0;
    for(unsigned i=0;i<count;++i)if(kotogba_compositionPart(record,i,&sCompositionPartWork)&&
        CompositionPartVisible(parent,sCompositionPartWork))++result;
    return result;
}
[[gnu::target("thumb"),gnu::noinline]] static bool CompositionGrammarAuthorized(unsigned slot,u32 origin)
{
    if(slot!=COMPOSITION_SLOT||gKotoTextCapacity<=COMPOSITION_SLOT||!sCompositionView.ready||
       sCompositionView.parent>=KOTO_TEXT_SLOTS)return false;
    const auto& parent=gKotoTextRecords[sCompositionView.parent];const auto& record=gKotoTextRecords[COMPOSITION_SLOT];
    const auto& part=sCompositionView.part;const unsigned bytes=part.rawEnd-part.rawStart;
    if(part.rawStart>=part.rawEnd||part.rawEnd>parent.size||bytes>=KOTO_TEXT_BYTES)return false;
    return parent.ready&&parent.sequence==sCompositionView.parentSequence&&record.ready&&
        record.sequence==sCompositionView.sequence&&record.origin==origin&&record.source==origin&&origin==part.origin&&
        record.window==parent.window&&record.tiles==parent.tiles&&record.cursor==origin+bytes&&
        record.context==parent.context&&record.size==bytes+1&&record.bytes[bytes]==0xff&&
        kotogba_compositionAuthorize(&parent,sCompositionView.version,sCompositionView.index,&part)&&
        CompositionPartVisible(sCompositionView.parent,part)&&!memcmp(record.bytes,parent.bytes+part.rawStart,bytes);
}
[[gnu::target("thumb"),gnu::noinline]] static ReaderFrame CompositionFrame(unsigned parent,unsigned ordinal,int scroll)
{
    if(sModalKind&&sHasShown)return {sShown.card,sShown.page,sShown.tab,sShown.menu,sShown.scroll};
    if(!gKotoTextRecords||gKotoTextCapacity<=COMPOSITION_SLOT||parent>=KOTO_TEXT_SLOTS)return {0,0,0,0,0};
    const auto& original=gKotoTextRecords[parent];const u32 version=kotogba_compositionVersion(&original);
    const unsigned count=kotogba_compositionCount(&original);unsigned index=0,visible=0;
    for(;index<count;++index)if(kotogba_compositionPart(&original,index,&sCompositionPartWork)&&
        CompositionPartVisible(parent,sCompositionPartWork)){if(visible++==ordinal)break;}
    if(!version||index==count||!kotogba_compositionAuthorize(&original,version,index,&sCompositionPartWork))return {0,0,0,0,0};
    auto& record=gKotoTextRecords[COMPOSITION_SLOT];
    if(!sCompositionView.ready||sCompositionView.parent!=parent||sCompositionView.index!=index||
       sCompositionView.version!=version||sCompositionView.parentSequence!=original.sequence||
       !CompositionGrammarAuthorized(COMPOSITION_SLOT,sCompositionPartWork.origin)){
        SceneChanged();record.ready=0;sCompositionView.ready=false;
        const unsigned bytes=sCompositionPartWork.rawEnd-sCompositionPartWork.rawStart;
        record.source=record.origin=sCompositionPartWork.origin;record.tiles=original.tiles;
        record.sequence=version;record.context=original.context;record.cursor=record.origin+bytes;
        record.window=original.window;record.x=original.x;record.y=original.y;record.font=original.font;
        record.size=bytes+1;record.speed=0;record.reserved=0;
        memcpy(record.bytes,original.bytes+sCompositionPartWork.rawStart,bytes);record.bytes[bytes]=0xff;
        sCorpusRefs[COMPOSITION_SLOT]={};
        sCompositionView={sCompositionPartWork,version,original.sequence,record.sequence,(u16)parent,(u16)index,true};
        record.ready=1;
    }
    return {0xc0320000u|(parent<<8)|index,(u16)(0x8000|COMPOSITION_SLOT),0,0,scroll};
}
[[gnu::target("thumb"),gnu::noinline]] static bool CompositionGrammarComplete(unsigned slot)
{
    return slot==COMPOSITION_SLOT&&sCompositionView.ready&&sCompositionView.parent<KOTO_TEXT_SLOTS&&
        kotogba_compositionComplete(gKotoTextRecords+sCompositionView.parent);
}
[[gnu::target("thumb"),gnu::noinline]] static void CompositionAddRoutes()
{
    if(!gKotoTextRecords||gKotoTextCapacity<=COMPOSITION_SLOT)return;
    sCompositionRouteSeen[0]=sCompositionRouteSeen[1]=0;
    for(unsigned i=0;i<sNativeBuildCount&&sNativeBuildCount<KOTO_TEXT_SLOTS+12;++i){
        const auto route=sNativeBuild[i];if(CompositionRoute(route)||route.record>=KOTO_TEXT_SLOTS)continue;
        const unsigned parent=route.record;const u32 bit=1u<<(parent&31);
        if(sCompositionRouteSeen[parent>>5]&bit)continue;sCompositionRouteSeen[parent>>5]|=bit;
        const unsigned count=CompositionVisibleCount(parent);if(!count)continue;
        for(unsigned j=sNativeBuildCount;j>i+1;--j)sNativeBuild[j]=sNativeBuild[j-1];
        sNativeBuild[i+1]={0xc0310000u|parent,COMPOSITION_ROUTE_PAGE,(u16)count,(u16)parent,route.reserved};
        ++sNativeBuildCount;++i;
    }
}
[[gnu::target("thumb"),gnu::noinline]] static unsigned CompositionTargetPage()
{
    if(sStudyTab||!sNativeCount)return 65535;
    unsigned page=0,current=65535;
    for(unsigned i=0;i<sNativeCount;++i){if(sStudyPage[0]>=page&&sStudyPage[0]<page+sNativeRoute[i].count){current=i;break;}page+=sNativeRoute[i].count;}
    if(current==65535)return 65535;
    const auto& active=sNativeRoute[current];const bool part=CompositionRoute(active);page=0;
    for(unsigned i=0;i<sNativeCount;++i){const auto& route=sNativeRoute[i];
        if(route.record==active.record&&CompositionRoute(route)!=part)return page;
        page+=route.count;
    }
    return 65535;
}
// Whole-message captions have no canonical source of their own. A tap can
// select one exact constructor part without assigning its identity to the
// complete message. Canonical orthography keeps its exact hit-test path;
// translated raw parents use the compositor's published Japanese origin.
[[gnu::target("thumb"),gnu::noinline]] static bool CompositionTouch(unsigned x,unsigned y)
{
    if(!sHasShown||sShown.tab||!NativeFrame(sShown)||!NativePublishedCoordinates()||OrthographyActive(sShown)||
       sShown.page&0x4000||sNativeTextCard!=sShown.card||sNativeTextPage!=sShown.page)return false;
    const unsigned parent=sShown.page&0x3fff;if(parent>=KOTO_TEXT_SLOTS||parent>=gKotoTextCapacity)return false;
    const auto& record=gKotoTextRecords[parent];const u32 version=kotogba_compositionVersion(&record);
    if(!version||!record.ready||record.sequence!=sNativeTextSequence)return false;
    const unsigned normalized=NativeNormalized(record,sNativeNormalized,true);u32 hash=2166136261u;
    for(unsigned i=0;i<normalized;++i)hash=(hash^sNativeNormalized[i])*16777619u;
    if(sShown.card!=(0x80000000u|(hash&0x7fffffffu)))return false;
    // Identical Japanese can move to another row/source within the same
    // constructor. Bind its ordered retained subset as well as its pixels.
    if(!CompositionPaintCurrent(parent,version)){SceneChanged();return false;}
    unsigned size=record.size;const u8* bytes=NativeCurrentText(record,size);
    unsigned xx=10,width=0,hit=65535;int yy=56+14*sCompositionPublished.prefixLines-sShown.scroll;
    for(unsigned at=0;at<size;++at){const u8 c=bytes[at];
        if(c==0xff||c==0xfa||c==0xfb)break;
        if(c==0xfc){if(++at>=size)return false;at+=NativeControlSize(bytes[at]);continue;}
        if(c==0xf8||c==0xf9){++at;continue;}
        if(c==0xfd)return false;
        if(c==0xfe){xx=10;width=0;yy+=14;continue;}
        const unsigned begin=at;const u32 cp=c==0xf7?(++at,0x2026u):sNativeCharacters[c];
        const unsigned cell=cp>255?14:6;
        if(width+cell>224){xx=10;width=0;yy+=14;}
        if((int)y>=yy&&(int)y<yy+14&&x+2>=xx&&x<xx+cell+2){hit=begin;break;}
        xx+=cell;width+=cell;if(yy>(int)y)return false;
    }
    if(hit==65535)return false;
    const unsigned count=kotogba_compositionCount(&record);unsigned selected=65535,ordinal=0;
    for(unsigned i=0;i<count;++i)if(kotogba_compositionPart(&record,i,&sCompositionPartWork)&&
       CompositionPartVisible(parent,sCompositionPartWork)){
        if(hit>=sCompositionPartWork.rawStart&&hit<sCompositionPartWork.rawEnd){selected=i;break;}++ordinal;}
    if(selected==65535)return false;
    unsigned page=0;
    for(unsigned i=0;i<sNativeCount;++i){const auto& route=sNativeRoute[i];
        if(CompositionRoute(route)&&route.record==parent&&ordinal<route.count){
            sCompositionPending={sCompositionPartWork,version,record.sequence,0xc0320000u|(parent<<8)|selected,
                (u16)(page+ordinal),(u16)parent,(u16)selected,true};
            ScrollMotionCancel();sStudyPage[0]=page+ordinal;sScroll=0;return true;}
        page+=route.count;}
    return false;
}
[[gnu::target("thumb"),gnu::noinline]] static void CompositionAutoOpen()
{
    auto& pending=sCompositionPending;if(!pending.ready)return;
    if(sModalKind||sStudyTab||pending.parent>=KOTO_TEXT_SLOTS||sStudyPage[0]!=pending.page){pending.ready=false;return;}
    const auto& parent=gKotoTextRecords[pending.parent];
    if(!parent.ready||parent.sequence!=pending.parentSequence){pending.ready=false;return;}
    if(!sHasShown||sShown.card!=pending.owner||sShown.page!=(0x8000|COMPOSITION_SLOT)||
       !OrthographyActive(sShown))return;
    if(kotogba_compositionVersion(&parent)!=pending.version||
       !kotogba_compositionAuthorize(&parent,pending.version,pending.index,&pending.part)){pending.ready=false;return;}
    if(!CompositionGrammarAuthorized(COMPOSITION_SLOT,gKotoTextRecords[COMPOSITION_SLOT].origin)){pending.ready=false;return;}
    const unsigned size=gKotoTextRecords[COMPOSITION_SLOT].size-1;
    u32 card=0;u16 ref=0xffff;
    if(OrthographyWord(0,size,card,ref)||GrammarAnchor(0,size,card,ref)){
        pending.ready=false;OpenTouchReference(ref,card);return;}
    if(CompositionGrammarComplete(COMPOSITION_SLOT)&&GrammarPublicationCurrent(COMPOSITION_SLOT))pending.ready=false;
}
