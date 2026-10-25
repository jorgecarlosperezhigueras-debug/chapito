// R32 compact cards stay in the already verified, asynchronous source document.
// A modal pins its exact strings. Saved references encode only source/index;
// the persisted canonical identity prevents another sense opening after updates.
struct InlineModal {
    LinguisticInlineCard card;
    u32 source,generation;
    u16 index,readingHeight,surfaceHeight,meaningHeight,explanationHeight;
    bool pinned,frozen;
    u32 meaningKey,canonicalSource;
    u16 token;
};
struct InlinePending {u32 identity;u16 card,ordinal;bool ready;};
struct InlineFrozenPending {
    ReaderFrame frame;
    u32 identity,meaningKey,canonicalSource,sequence,context,cursor;
    u16 token,parent,pool;
    u8 mode,stage;
    char title[192];
};
[[gnu::section(".ewram.bss")]] static InlineModal sInlineModal;
[[gnu::section(".ewram.bss")]] static InlinePending sInlinePending;
[[gnu::section(".ewram.bss")]] static LinguisticInlineCard sInlineLookup;
[[gnu::section(".ewram.bss")]] static InlineFrozenPending sInlineFrozenPending;
[[gnu::section(".ewram.bss")]] static char sInlineFrozenTitle[192],sInlineFrozenWrapped[208];

static bool InlineReference(u32 card,u16 ordinal,u32& source,unsigned& index)
{
    if(card<0x8000||card>65535)return false;
    source=((card&0x7fff)<<8)|(ordinal>>8);index=ordinal&255;
    return source&&source<0x00800000u;
}
static unsigned InlineLines(const char* text)
{if(!*text)return 0;unsigned lines=1;while(*text)if(*text++=='\n')++lines;return lines;}
static unsigned InlineWidth(const char* text,bool ruby)
{
    unsigned width=0,max=0;while(*text){const u32 cp=DecodeUtf8(text);
        if(cp=='\n'){if(width>max)max=width;width=0;}
        else width+=ruby?8:cp>255?14:6;}
    return width>max?width:max;
}
static bool InlineResolve(unsigned& index,u32 expected)
{
    if(LinguisticInlineCardAt(index,sInlineLookup)&&(!expected||sInlineLookup.identity==expected))return true;
    if(expected)for(unsigned i=0;i<LinguisticInlineCardCount();++i)
        if(LinguisticInlineCardAt(i,sInlineLookup)&&sInlineLookup.identity==expected){index=i;return true;}
    return false;
}
static bool InlineVariableToken(unsigned index,unsigned& token,bool visible)
{
    unsigned rawBytes=0;const u8* raw=LinguisticRaw(rawBytes);LinguisticInlineSpan span;
    for(unsigned i=0;raw&&i<LinguisticInlineSpanCount();++i){
        if(!LinguisticInlineSpanAt(i,span)||span.card!=index||!(span.flags&1)||span.rawEnd!=span.rawStart+2||span.rawEnd>rawBytes)continue;
        if(visible&&(span.rawStart<sOrthography.begin||span.rawEnd>sOrthography.end))continue;
        const unsigned code=raw[span.rawStart];if(code!=0xfd&&code!=0xf7)continue;
        token=raw[span.rawStart+1]+(code==0xf7?768:0);return true;}
    return false;
}
static bool InlineVisibleSpan(unsigned index)
{
    LinguisticInlineSpan span;for(unsigned i=0;i<LinguisticInlineSpanCount();++i)
        if(LinguisticInlineSpanAt(i,span)&&span.card==index&&span.rawStart>=sOrthography.begin&&span.rawEnd<=sOrthography.end)return true;
    return false;
}
static bool InlineVariableResolve(unsigned& index,unsigned token)
{
    unsigned actual;
    if(LinguisticInlineCardAt(index,sInlineLookup)&&sInlineLookup.type==4&&InlineVariableToken(index,actual,false)&&actual==token)return true;
    for(unsigned i=0;i<LinguisticInlineCardCount();++i)
        if(LinguisticInlineCardAt(i,sInlineLookup)&&sInlineLookup.type==4&&InlineVariableToken(i,actual,false)&&actual==token){index=i;return true;}
    return false;
}
static u32 InlineFrozenKey(u32 role,const char* title)
{
    u32 hash=2166136261u;for(unsigned i=0;i<4;++i)hash=(hash^((role>>(8*i))&255))*16777619u;
    while(*title)hash=(hash^(u8)*title++)*16777619u;
    return 0x80000000u|(hash&0x7fffffffu);
}
static bool InlineCopyTitle(char* destination,unsigned capacity,const char* title)
{unsigned n=0;while(title[n]){if(n+1>=capacity)return false;destination[n]=title[n];++n;}destination[n]=0;return n!=0;}
static bool InlineWrapTitle()
{
    const char* p=sInlineFrozenTitle;unsigned out=0,width=0;
    while(*p){const char* begin=p;const u32 cp=DecodeUtf8(p);const unsigned cell=cp>255?14:6,bytes=p-begin;
        if(cp=='\n'||width+cell>224){if(out+1>=sizeof(sInlineFrozenWrapped))return false;sInlineFrozenWrapped[out++]='\n';width=0;if(cp=='\n')continue;}
        if(out+bytes+1>=sizeof(sInlineFrozenWrapped))return false;
        memcpy(sInlineFrozenWrapped+out,begin,bytes);out+=bytes;width+=cell;}
    sInlineFrozenWrapped[out]=0;return true;
}
[[gnu::target("thumb"),gnu::noinline]] static u32 InlineMemoryKey(u32 card,u16 ordinal)
{
    if(card==0x7ffe){
        if(ordinal==65535&&sModalKind==4&&sInlineModal.frozen)return sInlineModal.card.identity;
        return CFrozenIdentity(ordinal);}
    u32 source;unsigned index;if(!InlineReference(card,ordinal,source,index)||!LinguisticReady(source))return 0;
    return LinguisticInlineCardAt(index,sInlineLookup)?sInlineLookup.identity:0;
}
static void InlineClose()
{if(sInlineModal.pinned){LinguisticUnpin();sInlineModal.pinned=false;}sInlineModal.frozen=false;}
// Mode 3 keeps the already published parent while a canonical variable card
// owns the only KL29 document buffer. Closing must reload that parent before
// a shorter raw fallback can clamp its scroll or replace its saved pixels.
static void InlineSavedCancel(){sInlinePending.ready=false;if(sInlineFrozenPending.mode!=3)sInlineFrozenPending.mode=0;}
static unsigned InlineModalHeight()
{
    const auto& m=sInlineModal;
    return m.readingHeight+m.surfaceHeight+4+m.meaningHeight+(m.explanationHeight?4+m.explanationHeight:0);
}
[[gnu::target("thumb"),gnu::noinline]] static bool InlineOpen(unsigned index)
{
    const u32 source=LinguisticSourceOffset();
    if(!source||source>=0x00800000u||index>=256||!LinguisticInlineCardAt(index,sInlineLookup))return false;
    const unsigned reading=InlineLines(sInlineLookup.reading)*12,surface=InlineLines(sInlineLookup.surface)*18;
    const unsigned meaning=InlineLines(sInlineLookup.meaning)*18,explanation=InlineLines(sInlineLookup.explanation)*18;
    if(reading+surface+meaning+explanation+8>32767)return false;
    if(!LinguisticPin(source))return false;
    GrammarClose();InlineClose();sInlineModal.pinned=true;sInlineModal.frozen=false;sInlineModal.card=sInlineLookup;
    sInlineModal.source=source;sInlineModal.generation=LinguisticGeneration();sInlineModal.index=index;
    sInlineModal.readingHeight=reading;sInlineModal.surfaceHeight=surface;
    sInlineModal.meaningHeight=meaning;sInlineModal.explanationHeight=explanation;
    sModalKind=4;sModalCard=0x8000|(source>>8);sModalOrdinal=((source&255)<<8)|index;
    sModalPage=0;sModalFirst=index;sModalCount=InlineModalHeight();sModalScroll=0;sModalStatus=0;
    sModalWidth=240;sModalBody=nullptr;sInlinePending.ready=false;
    return true;
}
[[gnu::target("thumb"),gnu::noinline]] static bool InlineApplyFrozen(unsigned index,const char* title,u32 expected,
                                                                   unsigned token,u32 canonical,unsigned pool)
{
    if(!LinguisticInlineCardAt(index,sInlineLookup)||sInlineLookup.type!=4||
       !InlineCopyTitle(sInlineFrozenTitle,sizeof(sInlineFrozenTitle),title)||!InlineWrapTitle())return false;
    const u32 identity=InlineFrozenKey(sInlineLookup.identity,sInlineFrozenTitle);
    if(expected&&expected!=identity)return false;
    if(!InlineOpen(index))return false;
    auto& modal=sInlineModal;modal.frozen=true;modal.card.identity=identity;
    modal.card.surface=sInlineFrozenWrapped;modal.card.surfaceBytes=strlen(sInlineFrozenWrapped);
    modal.card.reading="";modal.card.readingBytes=0;modal.readingHeight=0;
    modal.surfaceHeight=InlineLines(sInlineFrozenWrapped)*18;
    modal.meaningKey=(modal.source<<8)|index;modal.token=token;modal.canonicalSource=canonical;
    // Only battle opcodes that explicitly add a prefix can give these kana
    // their grammatical role; a player's identical nickname remains literal.
    if(modal.source>=0x3bd6ddu&&modal.source<0x3bf900u&&(token==0x0d||(token>=0x0f&&token<=0x13))){
        const char* explanation=nullptr;
        if(!strncmp(title,"やせいの",12)||!strncmp(title,"野生の",9))
            explanation="やせいの / 野生の: «salvaje».\nEl nombre que sigue es el Pokémon.\nの une esta descripción al nombre.";
        else if(!strncmp(title,"あいての",12)||!strncmp(title,"相手の",9)||!strncmp(title,"てきの",9)||!strncmp(title,"敵の",6))
            explanation="あいての / 相手の: «del rival».\nの relaciona 相手 (rival) con el\nnombre del Pokémon que sigue.";
        if(explanation){modal.card.explanation=explanation;modal.card.explanationBytes=strlen(explanation);
            modal.explanationHeight=InlineLines(explanation)*18;}
    }
    sModalCard=0x7ffe;sModalOrdinal=pool;sModalCount=InlineModalHeight();
    return true;
}
static bool InlineFrozenCurrent(u32& identity,u32& key,u16& token,u32& canonical,const char*& title)
{
    if(sModalKind!=4||!sInlineModal.frozen||!sInlineModal.pinned)return false;
    identity=sInlineModal.card.identity;key=sInlineModal.meaningKey;token=sInlineModal.token;
    canonical=sInlineModal.canonicalSource;title=sInlineFrozenTitle;return true;
}
static bool InlineFrozenCurrentParent()
{
    const auto& pending=sInlineFrozenPending;
    if(pending.mode==1||pending.mode==3){
        if(sStudyTab||!sHasShown||!SamePage(pending.frame,sShown)||pending.frame.scroll!=sShown.scroll||
           pending.parent>=gKotoTextCapacity||!NativePublishedCoordinates())return false;
        const auto& r=gKotoTextRecords[pending.parent];
        return r.ready&&r.sequence==pending.sequence&&r.context==pending.context&&r.cursor==pending.cursor&&
            (r.origin==0x08000000u+(pending.meaningKey>>8));
    }
    if(pending.mode==2){
        if(sStudyTab!=1||CFrozenIdentity(pending.pool)!=pending.identity)return false;
        for(unsigned i=0;i<sSavedCount;++i)if(sSavedWords[i].card==0x7ffe&&sSavedWords[i].ordinal==pending.pool&&
            sSavedMemory[i]==pending.identity)return true;
        return false;}
    return false;
}
static bool InlineHoldReader()
{
    if(!sInlineFrozenPending.mode)return false;
    if(sInlineFrozenPending.mode==3&&sModalKind)return false;
    if(sModalKind||!InlineFrozenCurrentParent()){sInlineFrozenPending.mode=0;return false;}
    return true;
}
[[gnu::target("thumb"),gnu::noinline]] static bool InlineQueueVisibleVariable(unsigned index,unsigned slot)
{
    unsigned token;if(!InlineVariableToken(index,token,true))return false;
    const unsigned offset=OrthographyVariableOffset(token,false);
    if(offset==65535)return false;
    auto& pending=sInlineFrozenPending;pending.mode=0;
    if(!InlineCopyTitle(pending.title,sizeof(pending.title),sOrthographyVariables+offset))return false;
    const auto& record=gKotoTextRecords[slot];
    pending.frame=sShown;pending.identity=0;pending.meaningKey=(sOrthography.source<<8)|index;
    pending.token=token;pending.parent=slot;pending.pool=65535;
    pending.sequence=record.sequence;pending.context=record.context;pending.cursor=record.cursor;
    pending.canonicalSource=0;pending.stage=1;
    const unsigned role=CorpusEntityRoleFor(record.origin,token);
    const u32 canonical=role?kotogba_variableOrigin(record.origin,token,record.context):0;
    if(canonical>=0x08000000u&&CorpusEntitySource(role,canonical-0x08000000u)){
        pending.canonicalSource=canonical-0x08000000u;pending.stage=0;}
    pending.mode=1;
    // The visible parent is retained in the front buffer while the canonical
    // source is streamed. A new press or changed publication cancels the queue.
    GrammarReleaseFrame();OrthographyAbort();sRender.busy=false;sRender.text=nullptr;sGlyph.active=false;
    LinguisticRequest(pending.stage?pending.meaningKey>>8:pending.canonicalSource);return true;
}
[[gnu::target("thumb"),gnu::noinline]] static bool InlineOpenReference(unsigned index)
{
    // Reader hits have to refer to the exact published orthography snapshot.
    // Saved collection reopen uses its separate persisted-identity route below.
    if(!sHasShown||!OrthographyActive(sShown)||!NativePublishedCoordinates()||
       sOrthography.source!=LinguisticSourceOffset())return false;
    const unsigned slot=GrammarFrameSlot(sShown);
    if(slot>=gKotoTextCapacity||!gKotoTextRecords[slot].ready||!gKotoTextRecords[slot].sequence||
       (!OrthographyCaptureCurrent(sShown,gKotoTextRecords[slot])&&!CompositionPublishedLiteralCurrent(sShown))||!InlineVisibleSpan(index)||
       !LinguisticInlineCardAt(index,sInlineLookup))return false;
    if(sInlineLookup.type==4)return InlineQueueVisibleVariable(index,slot);
    return InlineOpen(index);
}
[[gnu::target("thumb"),gnu::noinline]] static bool InlineSavedLabels(u16 card,u16 ordinal,u32 expected,
                                                                   const char*& title,const char*& spanish)
{
    u32 source;unsigned index;
    if(!InlineReference(card,ordinal,source,index))return false;
    if(!LinguisticAvailable()||sLinguistic.failed==source){
        title="Ficha no disponible";spanish="Revisa el paquete de ayuda";return true;}
    if(!LinguisticReady(source)){LinguisticRequest(source);return false;}
    if(!expected||!InlineResolve(index,expected)){
        title="Ficha no disponible";spanish="Revisa el paquete de ayuda";return true;}
    title=sInlineLookup.surface;spanish=sInlineLookup.meaning;return true;
}
[[gnu::target("thumb"),gnu::noinline]] static void InlineSavedOpen(u16 card,u16 ordinal,u32 expected)
{
    u32 source;unsigned index;if(!expected||!InlineReference(card,ordinal,source,index))return;
    sInlinePending={expected,card,ordinal,true};LinguisticRequest(source);
}
[[gnu::target("thumb"),gnu::noinline]] static bool InlineFrozenLabels(u32 key,u16 token,u32 canonical,const char*& meaning)
{
    (void)canonical;const u32 source=key>>8;unsigned index=key&255;
    if(!source||source>=0x00800000u||!LinguisticAvailable()||sLinguistic.failed==source){meaning="Ficha no disponible";return true;}
    if(!LinguisticReady(source)){LinguisticRequest(source);return false;}
    if(!InlineVariableResolve(index,token)){meaning="Ficha no disponible";return true;}
    meaning=sInlineLookup.meaning;return true;
}
[[gnu::target("thumb"),gnu::noinline]] static void InlineFrozenSavedOpen(u32 identity,u32 key,u16 token,u32 canonical,
                                                                      const char* title,u16 pool)
{
    if(!identity||pool>=128||CFrozenIdentity(pool)!=identity||!InlineCopyTitle(sInlineFrozenPending.title,sizeof(sInlineFrozenPending.title),title))return;
    auto& pending=sInlineFrozenPending;pending.identity=identity;pending.meaningKey=key;pending.token=token;
    pending.canonicalSource=canonical;pending.pool=pool;pending.mode=2;pending.stage=1;
    LinguisticRequest(key>>8);
}
[[gnu::target("thumb"),gnu::noinline]] static bool InlineCanonicalOpen()
{
    unsigned rawBytes=0;if(!LinguisticRaw(rawBytes)||!rawBytes)return false;
    const unsigned end=rawBytes-1;LinguisticWord word;
    // Exact whole-name legacy cards retain their original contextual popup.
    for(unsigned i=0;i<LinguisticWordCount();++i)if(LinguisticWordAt(i,word)&&!word.rawStart&&word.rawEnd==end){
        OpenTouchReference(word.ref,word.card);if(sModalKind)return true;}
    LinguisticInlineSpan span;
    for(unsigned i=0;i<LinguisticInlineSpanCount();++i)if(LinguisticInlineSpanAt(i,span)&&!span.rawStart&&span.rawEnd==end&&!(span.flags&1)&&
       LinguisticInlineCardAt(span.card,sInlineLookup)&&sInlineLookup.type!=4)
        if(InlineOpen(span.card))return true;
    return false;
}
[[gnu::target("thumb"),gnu::noinline]] static void InlineFrozenService()
{
    auto& pending=sInlineFrozenPending;if(!pending.mode)return;
    if(pending.mode==3){
        if(sModalKind)return;
        const u32 source=pending.meaningKey>>8;
        if(InlineFrozenCurrentParent()&&LinguisticAvailable()&&sLinguistic.failed!=source&&!LinguisticReady(source)){
            LinguisticRequest(source);return;}
        pending.mode=0;return;
    }
    if(sModalKind||!InlineFrozenCurrentParent()){pending.mode=0;return;}
    if(!pending.stage&&pending.canonicalSource){
        if(LinguisticAvailable()&&sLinguistic.failed!=pending.canonicalSource){
            if(!LinguisticReady(pending.canonicalSource)){LinguisticRequest(pending.canonicalSource);return;}
            if(InlineCanonicalOpen()){pending.mode=3;return;}
        }
        pending.stage=1;LinguisticRequest(pending.meaningKey>>8);return;
    }
    const u32 source=pending.meaningKey>>8;unsigned index=pending.meaningKey&255;
    if(!LinguisticAvailable()||sLinguistic.failed==source){pending.mode=0;return;}
    if(!LinguisticReady(source)){LinguisticRequest(source);return;}
    if(!InlineVariableResolve(index,pending.token)){pending.mode=0;return;}
    if(InlineApplyFrozen(index,pending.title,pending.identity,pending.token,pending.canonicalSource,pending.pool))pending.mode=0;
    else pending.mode=0;
}
[[gnu::target("thumb"),gnu::noinline]] static void InlineSavedService()
{
    InlineFrozenService();
    auto& pending=sInlinePending;if(!pending.ready)return;
    if(sStudyTab!=1||sModalKind){pending.ready=false;return;}
    bool saved=false;for(unsigned i=0;i<sSavedCount;++i)
        if(sSavedWords[i].card==pending.card&&sSavedWords[i].ordinal==pending.ordinal&&sSavedMemory[i]==pending.identity){saved=true;break;}
    if(!saved){pending.ready=false;return;}
    u32 source;unsigned index;
    if(!InlineReference(pending.card,pending.ordinal,source,index)||!LinguisticAvailable()||sLinguistic.failed==source){pending.ready=false;return;}
    if(!LinguisticReady(source)){LinguisticRequest(source);return;}
    if(!InlineResolve(index,pending.identity)){pending.ready=false;return;}
    InlineOpen(index);
}
static int InlineReadingTop(){return sModalTop+26-sShownModalScroll;}
static int InlineSurfaceTop(){return InlineReadingTop()+sInlineModal.readingHeight;}
static int InlineMeaningTop(){return InlineSurfaceTop()+sInlineModal.surfaceHeight+4;}
static int InlineExplanationTop(){return InlineMeaningTop()+sInlineModal.meaningHeight+4;}
static int InlineTextX(const char* text,bool ruby)
{const unsigned width=InlineWidth(text,ruby);return sModalLeft+8+(width<224?(224-width)/2:0);}
// One field is submitted at a time; the common glyph painter yields normally.
static bool InlineModalRenderUnit()
{
    const auto& item=sInlineModal.card;
    switch(sRender.line++){
    case 0:
        sRender.clipTop=sModalTop+2;sRender.clipBottom=sModalTop+18;
        SetText(sModalLeft+8,sModalTop+4,item.type==1||item.type==2?"Gramática":"Vocabulario",Paper);return false;
    case 1:
        if(*item.reading)SetText(InlineTextX(item.reading,true),InlineReadingTop(),item.reading,Muted);
        return false;
    case 2:SetText(InlineTextX(item.surface,false),InlineSurfaceTop(),item.surface,Teal);return false;
    case 3:SetText(sModalLeft+8,InlineMeaningTop(),item.meaning,Ink);return false;
    case 4:if(*item.explanation)SetText(sModalLeft+8,InlineExplanationTop(),item.explanation,Ink);return false;
    default:return true;
    }
}
static u32 InlineGlyphInText(unsigned x,unsigned y,const char* text,int top,int left,unsigned step)
{
    int yy=top,xx=left;while(*text){const u32 cp=DecodeUtf8(text);
        if(cp=='\n'){yy+=step;xx=left;continue;}
        const unsigned width=cp>255?14:6;
        if((int)y>=yy&&(int)y<yy+16&&(int)x>=xx&&(int)x<xx+(int)width)return cp;
        xx+=width;if(yy>(int)y)break;}
    return 0;
}
static u32 InlineModalGlyphAt(unsigned x,unsigned y)
{
    if(!sInlineModal.pinned||sInlineModal.generation!=LinguisticGeneration())return 0;
    const auto& item=sInlineModal.card;
    const int shift=sShownModalScroll-sTapModalScroll;
    if(u32 cp=InlineGlyphInText(x,y,item.surface,InlineSurfaceTop()+shift,InlineTextX(item.surface,false),18))return cp;
    if(u32 cp=InlineGlyphInText(x,y,item.meaning,InlineMeaningTop()+shift,sModalLeft+8,18))return cp;
    return InlineGlyphInText(x,y,item.explanation,InlineExplanationTop()+shift,sModalLeft+8,18);
}
