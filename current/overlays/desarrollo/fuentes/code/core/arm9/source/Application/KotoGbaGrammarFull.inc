// Complete R29 notes share the verified KL29 document with Japanese. No text is
// copied, shortened or accumulated from old KS5 pages. A published frame/modal
// pins that document; this snapshot retains only identities and geometry.
struct GrammarNote {
    u32 id;
    u16 index,rawStart,rawEnd,displayStart,displayEnd,y,height;
};
struct GrammarSnapshot {
    ReaderFrame frame;
    u32 sequence,context,source,generation;
    u16 slot,base,height,count;
    GrammarNote notes[LINGUISTIC_MAX_NOTES];
};
struct GrammarPublication {
    u32 source,sequence,context,cursor,seen,generation,corpusGeneration;
    u16 slot;
    bool complete;
    u8 requirementsMask;
};
static_assert(sizeof(GrammarSnapshot)<=192);
static_assert(sizeof(GrammarPublication)==32);
[[gnu::section(".ewram.bss")]] static GrammarSnapshot sGrammar;
[[gnu::section(".ewram.bss")]] static GrammarPublication sGrammarPublication;
[[gnu::section(".ewram.bss")]] static bool sGrammarModalPinned,sGrammarFramePinned;
// Getter output lives outside the small IRQ stack. Rendering, publication and
// input are serialized by the reader; no observer/SD callback uses this work.
[[gnu::section(".ewram.bss")]] static LinguisticNote sGrammarWork;

// Only eight reviewed notes contain dynamic FD tokens, at most one such
// note per source. Their complete expansion is retained independently of the
// corpus/normalizer scratch. This is bounded storage, never a truncation path.
struct GrammarDynamic {
    u32 source,sequence,context,generation;
    u16 index,bytes,lines;
    bool ready,present;
    char text[1536];
};
[[gnu::section(".ewram.bss")]] static GrammarDynamic sGrammarDynamic;
[[gnu::section(".ewram.bss")]] volatile u32 gGrammarDynamicStatus;
static unsigned GrammarHex(char c)
{return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:256;}
static unsigned GrammarToken(const char* p,unsigned bytes)
{
    if(bytes<7||memcmp(p,"{FD:",4)||p[6]!='}')return 0;
    const unsigned a=GrammarHex(p[4]),b=GrammarHex(p[5]);
    return a<16&&b<16?1+(a<<4)+b:0;
}
[[gnu::target("arm"),gnu::noinline]] static bool GrammarDynamicWrap(unsigned& bytes,unsigned& lines)
{
    unsigned index=0,first=0,space=65535,width=0;lines=1;
    while(index<bytes){
        const char* p=sGrammarDynamic.text+index;const u32 cp=DecodeUtf8(p);
        const unsigned length=p-(sGrammarDynamic.text+index);
        if(cp=='\n'){index+=length;first=index;space=65535;width=0;++lines;continue;}
        if(cp==' ')space=index;
        const unsigned pixels=cp>=0xe000&&cp<0xe100?8:cp>255?14:6;
        if(width+pixels>224){
            if(space!=65535&&space>=first){sGrammarDynamic.text[space]='\n';index=space+1;}
            else{
                if(bytes+1>=sizeof(sGrammarDynamic.text))return false;
                memmove(sGrammarDynamic.text+index+1,sGrammarDynamic.text+index,bytes-index+1);
                sGrammarDynamic.text[index++]='\n';++bytes;
            }
            first=index;space=65535;width=0;++lines;continue;
        }
        width+=pixels;index+=length;
    }
    return true;
}
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarDynamicPrepare(unsigned slot)
{
    const auto& r=gKotoTextRecords[slot];const u32 source=r.origin-0x08000000u;
    if(sGrammarDynamic.ready&&sGrammarDynamic.source==source&&sGrammarDynamic.sequence==r.sequence&&
       sGrammarDynamic.context==r.context&&sGrammarDynamic.generation==LinguisticGeneration())return true;
    // An unfinished frame/modal may still consume this exact stable buffer.
    if(sGrammarFramePinned||sGrammarModalPinned)return false;
    sGrammarDynamic.ready=sGrammarDynamic.present=false;gGrammarDynamicStatus=0;
    unsigned found=65535;
    for(unsigned i=0;i<LinguisticNoteCount();++i){
        if(!LinguisticNoteAt(i,sGrammarWork))return false;
        for(unsigned j=0;j<sGrammarWork.bodyBytes;++j)if(GrammarToken(sGrammarWork.body+j,sGrammarWork.bodyBytes-j)){
            if(found!=65535){gGrammarDynamicStatus=2;return false;}found=i;break;
        }
    }
    if(found!=65535){
        if(!LinguisticNoteAt(found,sGrammarWork))return false;
        const char* p=sGrammarWork.body;unsigned at=0,out=0;
        while(at<sGrammarWork.bodyBytes){
            const unsigned token=GrammarToken(p+at,sGrammarWork.bodyBytes-at);
            if(token){
                if(!kotogba_variableSource(r.origin,token-1,r.context)){gGrammarDynamicStatus=3;return false;}
                const char* value=CorpusVariableText(r.origin,token-1,r.context);
                while(*value){if(out+1>=sizeof(sGrammarDynamic.text)){gGrammarDynamicStatus=4;return false;}sGrammarDynamic.text[out++]=*value++;}
                at+=7;
            }else{
                if(out+1>=sizeof(sGrammarDynamic.text)){gGrammarDynamicStatus=4;return false;}
                sGrammarDynamic.text[out++]=p[at++];
            }
        }
        sGrammarDynamic.text[out]=0;unsigned lines=0;
        if(!GrammarDynamicWrap(out,lines)){gGrammarDynamicStatus=4;return false;}
        sGrammarDynamic.index=found;sGrammarDynamic.bytes=out;sGrammarDynamic.lines=lines;sGrammarDynamic.present=true;gGrammarDynamicStatus=1;
    }
    sGrammarDynamic.source=source;sGrammarDynamic.sequence=r.sequence;sGrammarDynamic.context=r.context;
    sGrammarDynamic.generation=LinguisticGeneration();sGrammarDynamic.ready=true;
    return true;
}
static bool GrammarGetNote(unsigned index,LinguisticNote& note)
{
    if(!LinguisticNoteAt(index,note))return false;
    if(sGrammarDynamic.present&&sGrammarDynamic.ready&&sGrammarDynamic.index==index&&
       sGrammarDynamic.source==sGrammar.source&&sGrammarDynamic.sequence==sGrammar.sequence&&
       sGrammarDynamic.context==sGrammar.context&&sGrammarDynamic.generation==sGrammar.generation){
        note.body=sGrammarDynamic.text;note.bodyBytes=sGrammarDynamic.bytes;note.bodyLines=sGrammarDynamic.lines;
    }
    return true;
}

[[gnu::target("thumb"),gnu::noinline]] static unsigned GrammarFrameSlot(const ReaderFrame& f)
{
    if(f.tab||!gKotoTextRecords)return 65535;
    if(NativeFrame(f)||(f.card&0xc0000000u)==0x40000000u)return f.page&0x3fff;
    if(f.card<255)if(const u8* card=FindStudy(f.card))
        if(f.page!=Card16(card,4))return 65535;
    for(unsigned i=0;i<sNativeCount;++i){const auto& route=sNativeRoute[i];
        if(route.owner==f.card&&f.page>=route.page&&f.page<route.page+route.count&&route.record<gKotoTextCapacity)return route.record;}
    return 65535;
}
// Reader ownership covers both orthography and grammar strings. Every new
// render releases its abandoned frame before acquiring the next document;
// PresentFrame releases only this reference, never the modal reference.
static void GrammarReleaseFrame()
{if(sGrammarFramePinned){LinguisticUnpin();sGrammarFramePinned=false;}}
[[gnu::target("thumb"),gnu::noinline]] static void GrammarBeginFrame(const ReaderFrame& f)
{
    GrammarReleaseFrame();
    if(sModalKind||f.tab)return;
    const unsigned slot=GrammarFrameSlot(f);if(slot>=gKotoTextCapacity)return;
    const auto& r=gKotoTextRecords[slot];
    if(r.ready&&r.sequence&&r.origin>=0x08000000&&r.origin<0x0a000000)
        sGrammarFramePinned=LinguisticPin(r.origin-0x08000000u);
}
// A captured source identity is authoritative; byte-identical RAM/hash matches
// never unlock a source's notes. The complete mask belongs to the raw source,
// not the number of notes or the number of current display lines.
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarSourceComplete(unsigned slot)
{
    if(slot>=gKotoTextCapacity||!gKotoTextRecords||gCorpusLoadStatus!=10)return false;
    const auto& r=gKotoTextRecords[slot];const auto& ref=sCorpusRefs[slot];
    if(!r.ready||!r.sequence||r.origin<0x08000000||r.origin>=0x0a000000||
       (slot==KOTO_TEXT_SLOTS&&!CompositionGrammarAuthorized(slot,r.origin))||
       ref.ready!=1||ref.hash||ref.sequence!=r.sequence||ref.index!=sCorpusDocIndex||
       r.origin!=0x08000000u+ref.entry.start||!LinguisticReady(ref.entry.start))return false;
    const u32 mask=LinguisticPublicationMask();
    if(!mask||(r.speed&&r.speed!=255&&(ref.seen&mask)!=mask))return false;
    // The existing frozen FD bindings are required before publishing a note
    // about an expanded source. No literal token or live guest variable leaks.
    const u32 raw=Corpus32(sCorpusDocument+16),es=Corpus32(sCorpusDocument+20),flags=Corpus32(sCorpusDocument+24);
    const u8* vars=sCorpusDocument+32+raw+es+1+2*((flags>>8)&255);
    for(unsigned i=0;i<(flags>>16);++i){const unsigned token=Corpus16(vars+8*i+2);
        if((token<256||(token>=768&&token<1024))&&!kotogba_variableSource(r.origin,token,r.context))return false;}
    return r.ready&&ref.sequence==r.sequence;
}
// Dependency records in the teacher-reviewed authority are short, complete
// source strings (all current fixtures are <=23 raw bytes, with no FA/FB).
// The compiled package validates that restriction before setting dependencies.
// A composition additionally requires each exact source to remain in the
// current verified native route, rather than borrowing old accumulated text.
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarRequiredSource(u32 source,unsigned primary,bool composition)
{
    if(primary>=gKotoTextCapacity)return false;
    const auto& parent=gKotoTextRecords[primary];
    for(unsigned i=0;i<KOTO_TEXT_SLOTS&&i<gKotoTextCapacity;++i){const auto& r=gKotoTextRecords[i];
        if(!r.ready||!r.sequence||r.origin!=0x08000000u+source||
           (parent.context&&r.context&&parent.context!=r.context))continue;
        if(r.speed&&r.speed!=255){const auto& ref=sCorpusRefs[i];
            if(ref.ready!=1||ref.hash||ref.key!=source||ref.sequence!=r.sequence||!(ref.seen&1))continue;}
        if(composition){bool visible=false;
            for(unsigned j=0;j<sNativeCount;++j)if(sNativeRoute[j].record==i){visible=true;break;}
            if(!visible)continue;}
        return true;
    }
    return false;
}
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarRequirements(unsigned slot,const LinguisticNote& note)
{
    const bool composition=note.flags&LINGUISTIC_COMPOSITION_REQUIRED;
    // Only the reserved component view can authorize a constructed message's
    // notes, after its exact ordered constructor witness remains published.
    // A matching plain ROM/RAM string alone never grants this permission.
    if(composition&&(!CompositionGrammarAuthorized(slot,gKotoTextRecords[slot].origin)||
        !CompositionGrammarComplete(slot)))return false;
    if(!(note.flags&LINGUISTIC_DEPENDENCIES_REQUIRED))return true;
    bool any=false;
    for(unsigned i=0;i<2;++i)if(note.requiredSources[i]){
        any=true;if(!GrammarRequiredSource(note.requiredSources[i],slot,composition))return false;}
    return any;
}
// Called at the shallow reader entry, before StartRender. In particular, the
// variable binding proof is not nested below layout/font/composition frames.
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarRequest(const ReaderFrame& f)
{
    const bool wasComplete=sGrammarPublication.complete;
    sGrammarPublication.complete=false;
    if(f.tab||!LinguisticAvailable()||gCorpusLoadStatus!=10)return wasComplete;
    const unsigned slot=GrammarFrameSlot(f);if(slot>=gKotoTextCapacity)return wasComplete;
    const auto& r=gKotoTextRecords[slot];
    if(!r.ready||!r.sequence||r.origin<0x08000000||r.origin>=0x0a000000)return wasComplete;
    const u32 source=r.origin-0x08000000u;LinguisticRequest(source);
    if(!CorpusResolve(slot,r.origin,0,0))return wasComplete;
    const ReaderFrame native={0x80000000u,(u16)(0x8000|slot),0,0,0};CorpusPrepare(native);
    if(!GrammarSourceComplete(slot)||!GrammarDynamicPrepare(slot))return wasComplete;
    unsigned requirements=0;
    for(unsigned i=0;i<LinguisticNoteCount()&&i<LINGUISTIC_MAX_NOTES;++i){
        if(!LinguisticNoteAt(i,sGrammarWork))return wasComplete;
        if(GrammarRequirements(slot,sGrammarWork))requirements|=1u<<i;
    }
    const bool changed=!wasComplete||sGrammarPublication.source!=source||
        sGrammarPublication.sequence!=r.sequence||sGrammarPublication.context!=r.context||
        sGrammarPublication.cursor!=r.cursor||sGrammarPublication.seen!=sCorpusRefs[slot].seen||
        sGrammarPublication.generation!=LinguisticGeneration()||
        sGrammarPublication.corpusGeneration!=CorpusGeneration()||sGrammarPublication.slot!=slot||
        sGrammarPublication.requirementsMask!=requirements;
    sGrammarPublication={source,r.sequence,r.context,r.cursor,sCorpusRefs[slot].seen,
                         LinguisticGeneration(),CorpusGeneration(),(u16)slot,true,(u8)requirements};
    return changed;
}
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarPublicationCurrent(unsigned slot)
{
    if(!sGrammarPublication.complete||slot>=gKotoTextCapacity||slot!=sGrammarPublication.slot)return false;
    const auto& r=gKotoTextRecords[slot];
    if(slot==KOTO_TEXT_SLOTS&&!CompositionGrammarAuthorized(slot,r.origin))return false;
    return r.ready&&r.sequence==sGrammarPublication.sequence&&r.context==sGrammarPublication.context&&
        r.cursor==sGrammarPublication.cursor&&r.origin==0x08000000u+sGrammarPublication.source&&
        sCorpusRefs[slot].seen==sGrammarPublication.seen&&CorpusGeneration()==sGrammarPublication.corpusGeneration&&
        LinguisticGeneration()==sGrammarPublication.generation&&LinguisticReady(sGrammarPublication.source);
}
[[gnu::target("thumb"),gnu::noinline]] static unsigned GrammarPrepare(const ReaderFrame& f,unsigned height)
{
    sGrammar.frame=f;sGrammar.base=sGrammar.height=height;sGrammar.count=0;
    sGrammar.slot=GrammarFrameSlot(f);sGrammar.sequence=0;
    if(!height||f.tab||!LinguisticAvailable()||!GrammarPublicationCurrent(sGrammar.slot))return height;
    const auto& r=gKotoTextRecords[sGrammar.slot];
    sGrammar.source=r.origin-0x08000000u;sGrammar.generation=LinguisticGeneration();
    sGrammar.sequence=r.sequence;sGrammar.context=r.context;
    unsigned begin=0,end=65535;
    const bool full=(f.card&0xc0000000u)==0x40000000u||(NativeFrame(f)&&(f.page&0x4000));
    if(!full){
        // Even after the whole source was observed, the current-fragment page
        // publishes only anchors wholly inside this exact guest fragment.
        const unsigned count=LinguisticFragmentCount();unsigned fragment=r.reserved;
        if(!count){unsigned raw=0;if(!LinguisticRaw(raw)||!raw)return height;end=raw;}
        else{
            if(fragment>=count){if(count!=1)return height;fragment=0;}
            LinguisticFragment range;if(!LinguisticFragmentAt(fragment,range))return height;
            begin=range.rawStart;end=range.rawEnd;
        }
    }
    const unsigned count=LinguisticNoteCount();unsigned y=height+30;
    for(unsigned i=0;i<count&&i<LINGUISTIC_MAX_NOTES;++i){
        if(!(sGrammarPublication.requirementsMask&(1u<<i))||
           !GrammarGetNote(i,sGrammarWork)||!sGrammarWork.bodyLines||
           sGrammarWork.rawStart<begin||sGrammarWork.rawEnd>end)continue;
        // Japanese examples use 16-pixel glyphs. The shared inline/modal
        // grammar painter advances 18 pixels so the next row cannot overlap.
        const unsigned noteHeight=18u*sGrammarWork.bodyLines;
        if(y+noteHeight+14>65535)return height;
        auto& note=sGrammar.notes[sGrammar.count++];
        note={sGrammarWork.id,(u16)i,sGrammarWork.rawStart,sGrammarWork.rawEnd,
              sGrammarWork.displayStart,sGrammarWork.displayEnd,(u16)y,(u16)noteHeight};
        y+=noteHeight+10;
    }
    if(sGrammar.count)sGrammar.height=y+4;
    return sGrammar.height;
}
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarSnapshotCurrent()
{
    if(!sGrammar.count||sGrammar.slot>=gKotoTextCapacity||!gKotoTextRecords||
       sGrammar.generation!=LinguisticGeneration()||!LinguisticReady(sGrammar.source))return false;
    const auto& r=gKotoTextRecords[sGrammar.slot];
    if(sGrammar.slot==KOTO_TEXT_SLOTS&&!CompositionGrammarAuthorized(sGrammar.slot,r.origin))return false;
    return r.ready&&r.sequence==sGrammar.sequence&&r.context==sGrammar.context&&
           r.origin==0x08000000u+sGrammar.source;
}
static unsigned GrammarHeight(const ReaderFrame& f,unsigned height)
{return !f.tab&&SamePage(f,sGrammar.frame)&&GrammarSnapshotCurrent()?sGrammar.height:height;}
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarRenderUnit()
{
    if(!SamePage(sRender.target,sGrammar.frame)||!GrammarSnapshotCurrent())return true;
    const unsigned unit=sRender.chrome++;const int y=56+sGrammar.base+8-sRender.target.scroll;
    if(unit<10){
        static const u16 rows[]={0x7fc,0x405,0x57d,0x405,0x57d,0x405,0x57d,0x405,0x405,0x7fc};
        if(y+(int)unit>=sRender.clipTop&&y+(int)unit<sRender.clipBottom)
            for(unsigned x=0;x<11;++x)if(rows[unit]&(1u<<x))Pixel(12+x,y+unit,GrammarBlue);
        return false;
    }
    if(unit==10){SetText(28,y,"Gramática",GrammarBlue);return false;}
    const unsigned index=unit-11;if(index>=sGrammar.count)return true;
    const auto& note=sGrammar.notes[index];const int at=56+note.y-sRender.target.scroll;
    if(at+(int)note.height>sRender.clipTop&&at<sRender.clipBottom&&
       GrammarGetNote(note.index,sGrammarWork)&&sGrammarWork.id==note.id)
        SetText(12,at,sGrammarWork.body,Ink);
    return false;
}
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarTouch(unsigned x,unsigned y,u32& card,u16& ref)
{
    if(!sHasShown||(sRender.busy&&!SamePage(sRender.target,sShown))||sShown.tab||
       !SamePage(sShown,sGrammar.frame)||x<8||x>=244||!GrammarSnapshotCurrent())return false;
    const int at=(int)y-56+sShown.scroll;
    for(unsigned i=0;i<sGrammar.count;++i){const auto& note=sGrammar.notes[i];
        if(at>=(int)note.y-2&&at<(int)note.y+note.height+3){card=0;ref=0x8000|i;return true;}}
    return false;
}
// Japanese hit-testing supplies the raw span of the actual displayed glyph.
// This never derives note availability from a visual substring or old KS5 ID.
[[gnu::target("thumb"),gnu::noinline]] static bool GrammarAnchor(unsigned rawStart,unsigned rawEnd,u32& card,u16& ref)
{
    if(rawStart>=rawEnd||!GrammarSnapshotCurrent())return false;
    for(unsigned i=0;i<sGrammar.count;++i){const auto& note=sGrammar.notes[i];
        if(rawStart<note.rawEnd&&note.rawStart<rawEnd){card=0;ref=0x8000|i;return true;}}
    return false;
}
static bool GrammarLegacyAllowed(u32 card,u16 ref)
{return card==0||!(ref&0x8000)||!LinguisticAvailable();}
[[gnu::target("thumb"),gnu::noinline]] static void GrammarClose()
{if(sGrammarModalPinned){LinguisticUnpin();sGrammarModalPinned=false;}}
[[gnu::target("thumb"),gnu::noinline]] static void GrammarOpen(unsigned index)
{
    if(index>=sGrammar.count||!GrammarSnapshotCurrent())return;
    const auto& note=sGrammar.notes[index];
    if(!GrammarGetNote(note.index,sGrammarWork)||sGrammarWork.id!=note.id||!LinguisticPin(sGrammar.source))return;
    GrammarClose();sGrammarModalPinned=true;
    sModalKind=3;sModalCard=0;sModalStatus=0;sModalPage=0;sModalFirst=index;
    sModalCount=note.height;sModalOrdinal=index;sModalScroll=0;
    sModalBody=sGrammarWork.body;
}
