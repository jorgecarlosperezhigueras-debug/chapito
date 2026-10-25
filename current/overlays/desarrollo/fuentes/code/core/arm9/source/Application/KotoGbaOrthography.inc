// R29: source-bound orthography over the immutable capture. The sidecar supplies
// reviewed spelling/reading, never evidence that an unseen fragment was drawn.
// All coordinates are rebuilt once from RAW ranges; no old pixel rectangle is
// reused after kanji contraction or expansion of a frozen FD/F7 variable.
// Include after Corpus + Grammar + the Linguistic reader implementation.
enum { OrthographyMaxUnits=192,OrthographyVariableBytes=1024,OrthographyLine=30 };
struct OrthographyPlacement {u16 x,y,width,mode;};
struct OrthographyVariable {u16 token,offset;};
struct OrthographyCursor {u16 at,varOffset,varAt;};
struct OrthographyState {
    ReaderFrame frame;
    LinguisticUnit unit;
    const u8* raw;
    u32 source,sequence,generation,context,recordCursor,requestSource;
    u16 rawSize,begin,end,count,height,top,variableBytes;
    OrthographyVariable variables[6];
    u16 variableCount,paintUnit,paintPhase,paintAt,paintX,paintY,paintDisplayBase;
    OrthographyCursor cursor,boundCursor,hitCursor;
    u16 atomBegin,atomEnd;
    u32 hitCp;
    u16 hitBegin,hitEnd;
    u16 hitDisplayBegin,hitDisplayEnd;
    bool active,pinned,failed;
};
[[gnu::section(".ewram.bss")]] static OrthographyPlacement sOrthographyPlaces[OrthographyMaxUnits];
[[gnu::section(".ewram.bss")]] static char sOrthographyVariables[OrthographyVariableBytes];
[[gnu::section(".ewram.bss")]] static OrthographyState sOrthography;
[[gnu::section(".ewram.bss")]] static LinguisticInlineCard sOrthographyInlineColor;
[[gnu::section(".ewram.bss")]] static LinguisticInlineCard sOrthographyInlineMatch;
static_assert(sizeof(sOrthographyPlaces)+sizeof(sOrthographyVariables)+sizeof(sOrthography)<=2816,
              "R29 orthography keeps its complete bounded layout below 2.75 KiB");
static bool OrthographySourceReady(u32 source)
{return source>=0x08000000u&&source<0x0a000000u&&LinguisticReady(source-0x08000000u)&&LinguisticWordsComplete();}
static u32 OrthographyGeneration(){return LinguisticGeneration();}
static void OrthographyAbort()
{
    if(sOrthography.pinned)LinguisticUnpin();
    sOrthography.pinned=false;sOrthography.active=false;
}
static bool OrthographyActive(const ReaderFrame& f)
{return sOrthography.active&&SamePage(f,sOrthography.frame)&&LinguisticReady(sOrthography.source)&&
       sOrthography.generation==LinguisticGeneration();}
static bool OrthographyBindFrame(const ReaderFrame&);
static bool OrthographyLayout();
static bool OrthographyCaptureCurrent(const ReaderFrame& f,const KotoTextRecord& r)
{
    u32 source=r.origin?r.origin:r.source;
    if(source>=0x02200000u&&source<0x02400000u)source=source-0x02200000u+0x08000000u;
    return OrthographyActive(f)&&source==0x08000000u+sOrthography.requestSource&&sOrthography.sequence==r.sequence&&
        sOrthography.context==r.context&&sOrthography.recordCursor==r.cursor;
}
// Request performs bounded CPU preparation only. The service owns all I/O.
[[gnu::target("thumb"),gnu::noinline]] static void OrthographyRequestFrame(const ReaderFrame& f)
{
    if(f.tab){OrthographyAbort();return;}
    const unsigned slot=GrammarFrameSlot(f);if(slot>=gKotoTextCapacity){OrthographyAbort();return;}
    const auto& r=gKotoTextRecords[slot];u32 source=r.origin?r.origin:r.source;
    if(!r.ready||!r.sequence){OrthographyAbort();return;}
    if(source>=0x02200000u&&source<0x02400000u)source=source-0x02200000u+0x08000000u;
    if(source<0x08000000u||source>=0x0a000000u){OrthographyAbort();return;}
    source-=0x08000000u;
    if(sOrthography.pinned&&(!LinguisticReady(source)||sOrthography.requestSource!=source||sOrthography.sequence!=r.sequence||sOrthography.context!=r.context||
       sOrthography.recordCursor!=r.cursor||
       !SamePage(f,sOrthography.frame)))OrthographyAbort();
    LinguisticRequest(source);
    // Freeze and bind before StartRender/NativeDecode retain their large
    // compositor frames. These phases perform no I/O and never use live names.
    if(!NativeFrame(f)||OrthographyCaptureCurrent(f,r)||!OrthographySourceReady(source+0x08000000u))return;
    if((f.page&0x4000)&&!CorpusSentenceProven(slot,false))return;
    if(!OrthographyBindFrame(f)||!OrthographyLayout())OrthographyAbort();
    else{
        // Only a successful new layout needs a positioned JP origin. A
        // rejected binding must let the existing fallback finish painting.
        sNativeTextPage=0xffff;
    }
}
[[gnu::always_inline]] static inline unsigned OrthographyAtom(unsigned at)
{
    const auto& w=sOrthography;if(at>=w.rawSize)return 0;const u8 c=w.raw[at];
    unsigned bytes=c==0xfc?(at+1<w.rawSize?2+NativeControlSize(w.raw[at+1]):0):
                   c==0xf7||c==0xf8||c==0xf9||c==0xfd?2:1;
    return bytes&&at+bytes<=w.rawSize?bytes:0;
}
// The historical native table simplified seven punctuation/UI bytes. Reviewed
// orthography keeps their exact source glyphs, including two-dot leader.
static u32 OrthographyCodepoint(u8 ch)
{
    switch(ch){case 0xaf:return 0xb7;case 0xb0:return 0x2025;case 0xb7:return 0xa5;
        case 0xb8:return ',';case 0xb9:return 0xd7;case 0xef:return 0x25b6;case 0xf0:return ':';default:return sNativeCharacters[ch];}
}
// Reconstruct the same current window interval as RefreshPrintedFragment.
// Direct-ROM typewriters can additionally stop inside a word: such an interval
// uses its literal source kana until the whole reviewed mapping unit is seen.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static bool OrthographyVisibleRange(const KotoTextRecord& r,bool full)
{
    auto& w=sOrthography;const u32 base=0x08000000u+w.source;
    w.begin=0;w.end=w.rawSize;
    if(full){if(w.end&&w.raw[w.end-1]==0xff)--w.end;return true;}
    const bool async=r.speed&&r.speed!=255;
    if(!async){
        unsigned begin=r.origin>=base&&r.origin-base<w.rawSize?r.origin-base:0;
        unsigned end=begin;
        while(end<w.rawSize){const unsigned atom=OrthographyAtom(end);if(!atom)return false;
            const u8 c=w.raw[end];if(c==0xfa||c==0xfb||c==0xff)break;end+=atom;}
        // A static direct-ROM caption may start at an interior source label.
        if(r.source>=base&&r.source<base+w.rawSize){begin=r.source-base;
            const unsigned limit=begin+(r.size?r.size-1:0);if(limit<end)end=limit;}
        w.begin=begin;w.end=end;return begin<end;
    }
    if(r.reserved>=32)return false;
    unsigned end=0,ordinal=0;
    while(end<w.rawSize){const unsigned atom=OrthographyAtom(end);if(!atom)return false;const u8 c=w.raw[end];
        if(c==0xff||((c==0xfa||c==0xfb)&&ordinal==r.reserved))break;
        if(c==0xfa||c==0xfb)++ordinal;end+=atom;
    }
    if(ordinal!=r.reserved)return false;
    if(r.cursor>=base&&r.cursor-base<end)end=r.cursor-base;
    unsigned maxRows=2;
    if(r.window<32){const u8* window=(const u8*)(0x02020430+12*r.window);maxRows=window[4]*8/14;if(!maxRows)maxRows=1;}
    unsigned first=0,rows=1;
    for(unsigned at=0;at<end;){const unsigned atom=OrthographyAtom(at);if(!atom||at+atom>end)break;
        const u8 c=w.raw[at];at+=atom;
        if(c==0xfb){first=at;rows=1;continue;}
        if(c==0xfa||c==0xfe){++rows;if(rows>maxRows){
            while(first<at){const unsigned old=OrthographyAtom(first);if(!old)return false;
                const u8 before=w.raw[first];first+=old;if(before==0xfe||before==0xfa||before==0xfb)break;}
            --rows;
        }}
    }
    w.begin=first;w.end=end;return first<end;
}
[[gnu::target("thumb"),gnu::noinline]] static unsigned OrthographyVariableOffset(unsigned token,bool freeze)
{
    auto& w=sOrthography;
    for(unsigned i=0;i<w.variableCount;++i)if(w.variables[i].token==token)return w.variables[i].offset;
    if(!freeze||w.variableCount==6)return 65535;
    const auto& r=gKotoTextRecords[w.frame.page&0x3fff];
    const u32 pointer=kotogba_variableSource(r.origin,token,r.context);
    if(!pointer)return 65535; // never substitute a future/live name or [rival]
    const char* value=CorpusVariableDecode(pointer,token);const unsigned bytes=strlen(value)+1;
    if(bytes>sizeof(sOrthographyVariables)-w.variableBytes)return 65535;
    const unsigned offset=w.variableBytes;memcpy(sOrthographyVariables+offset,value,bytes);
    w.variableBytes+=bytes;w.variables[w.variableCount++]={(u16)token,(u16)offset};return offset;
}
// One raw GBA atom or one already-frozen variable glyph. All control operands
// are skipped atomically; delimiter-valued FC arguments cannot create a row.
[[gnu::target("arm"),gnu::noinline]] static u32 OrthographyNextRaw(OrthographyCursor& c,unsigned stop,bool freeze)
{
    auto& w=sOrthography;
    for(unsigned work=0;work<512;++work){
        if(c.varOffset!=65535){const char* p=sOrthographyVariables+c.varOffset+c.varAt;
            if(*p){const char* start=p;const u32 cp=DecodeUtf8(p);c.varAt+=p-start;return cp;}
            c.varOffset=65535;c.varAt=0;
        }
        if(c.at>=stop)return 0;const unsigned at=c.at,atom=OrthographyAtom(at);
        if(!atom||at+atom>stop){c.at=stop;return 0;}
        c.at+=atom;w.atomBegin=at;w.atomEnd=c.at;const u8 ch=w.raw[at];
        if(ch==0xff){c.at=stop;return 0;}
        if(ch==0xfc||ch==0xf8||ch==0xf9)continue;
        if(ch==0xfd||ch==0xf7){const unsigned token=w.raw[at+1]+(ch==0xf7?768:0);
            c.varOffset=OrthographyVariableOffset(token,freeze);c.varAt=0;
            if(c.varOffset==65535){w.failed=true;return 0;}continue;}
        if(ch==0xfe||ch==0xfa||ch==0xfb)return '\n';
        return OrthographyCodepoint(ch);
    }
    w.failed=true;return 0;
}
// Bind the reviewed source to this immutable visible publication as well as
// its source ID. The observer may retain an entire typewriter box, so only
// the prefix proved by the original cursor is required here. Expanded RAM
// captures are compared through their frozen FD glyphs, never live memory.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static u32 OrthographyNextCapture(const KotoTextRecord& r,OrthographyCursor& c)
{
    for(unsigned work=0;work<512;++work){
        if(c.varOffset!=65535){const char* p=sOrthographyVariables+c.varOffset+c.varAt;
            if(*p){const char* start=p;const u32 cp=DecodeUtf8(p);c.varAt+=p-start;return cp;}
            c.varOffset=65535;c.varAt=0;
        }
        if(c.at>=r.size)return 0;const unsigned at=c.at;const u8 ch=r.bytes[at];
        const unsigned atom=ch==0xfc?(at+1<r.size?2+NativeControlSize(r.bytes[at+1]):0):
                            ch==0xf7||ch==0xf8||ch==0xf9||ch==0xfd?2:1;
        if(!atom||at+atom>r.size){sOrthography.failed=true;return 0;}c.at+=atom;
        if(ch==0xff)return 0;
        if(ch==0xfc||ch==0xf8||ch==0xf9)continue;
        if(ch==0xfd||ch==0xf7){c.varOffset=OrthographyVariableOffset(r.bytes[at+1]+(ch==0xf7?768:0),false);c.varAt=0;
            if(c.varOffset==65535){sOrthography.failed=true;return 0;}continue;}
        if(ch==0xfe||ch==0xfa||ch==0xfb)return '\n';
        return OrthographyCodepoint(ch);
    }
    sOrthography.failed=true;return 0;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static bool OrthographyCaptureBound(const KotoTextRecord& r)
{
    auto& w=sOrthography;w.cursor={w.begin,65535,0};w.boundCursor={0,65535,0};
    while(const u32 cp=OrthographyNextRaw(w.cursor,w.end,false))
        if(cp!=OrthographyNextCapture(r,w.boundCursor))return false;
    return !w.failed;
}
[[gnu::target("thumb"),gnu::noinline]] static bool OrthographyFreezeRange()
{
    auto& w=sOrthography;
    for(unsigned at=w.begin;at<w.end;){const unsigned atom=OrthographyAtom(at);if(!atom||at+atom>w.end)break;
        const u8 ch=w.raw[at];if(ch==0xfd||ch==0xf7)
            if(OrthographyVariableOffset(w.raw[at+1]+(ch==0xf7?768:0),true)==65535)return false;
        at+=atom;
    }
    return true;
}
static unsigned OrthographyTextWidth(const char* text,unsigned cell)
{
    unsigned width=0;while(*text){const u32 cp=DecodeUtf8(text);width+=cell?cell:cp>255?14:6;}return width;
}
[[gnu::always_inline]] static inline void OrthographyAdvance(u32 cp,unsigned& x,unsigned& y)
{
    if(cp=='\n'){x=10;y+=OrthographyLine;return;}
    const unsigned width=cp>255?14:6;if(x+width>234){x=10;y+=OrthographyLine;}x+=width;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static bool OrthographyBindFrame(const ReaderFrame& f)
{
    OrthographyAbort();auto& w=sOrthography;
    if(!NativeFrame(f))return false;const unsigned slot=f.page&0x3fff;
    if(slot>=gKotoTextCapacity)return false;const auto& r=gKotoTextRecords[slot];
    u32 source=r.origin?r.origin:r.source;
    if(source>=0x02200000u&&source<0x02400000u)source=source-0x02200000u+0x08000000u;
    if(!OrthographySourceReady(source)||!LinguisticPin(source-0x08000000u))return false;
    memset(&w,0,sizeof(w));w.pinned=true;w.frame=f;w.sequence=r.sequence;w.context=r.context;w.recordCursor=r.cursor;w.requestSource=source-0x08000000u;
    w.source=LinguisticSourceOffset();w.generation=LinguisticGeneration();
    unsigned bytes=0;w.raw=LinguisticRaw(bytes);w.rawSize=bytes;w.count=LinguisticUnitCount();
    if(!w.raw||!bytes||bytes>512||w.count>OrthographyMaxUnits||!OrthographyVisibleRange(r,false)||
       !OrthographyFreezeRange()||!OrthographyCaptureBound(r)||
       ((f.page&0x4000)&&(!OrthographyVisibleRange(r,true)||!OrthographyFreezeRange()))){OrthographyAbort();return false;}
    return true;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static bool OrthographyLayout()
{
    auto& w=sOrthography;
    unsigned x=10,y=0;
    for(unsigned i=0;i<w.count;++i){auto& place=sOrthographyPlaces[i];place={0,0,0,0};
        if(!LinguisticUnitAt(i,w.unit)){w.failed=true;break;}
        const auto& unit=w.unit;if(unit.rawEnd<=w.begin||unit.rawStart>=w.end)continue;
        bool literal=true,different=false;const char* p=unit.display;
        if(!unit.readingBytes)for(unsigned at=unit.rawStart;at<unit.rawEnd;++at){
            if(w.raw[at]>=0xf7){literal=false;break;}
            if(!*p||DecodeUtf8(p)!=OrthographyCodepoint(w.raw[at]))different=true;}
        if(*p)different=true;
        if((unit.readingBytes||(literal&&different))&&unit.rawStart>=w.begin&&unit.rawEnd<=w.end){
            const unsigned base=OrthographyTextWidth(unit.display,0),ruby=OrthographyTextWidth(unit.reading,8);
            const unsigned width=base>ruby?base:ruby;if(width>224){w.failed=true;break;}
            if(x+width>234){x=10;y+=OrthographyLine;}
            place={(u16)x,(u16)y,(u16)width,(u16)(unit.readingBytes?1:3)};x+=width;
        }else{
            place={(u16)x,(u16)y,0,2};
            w.cursor={(u16)(unit.rawStart>w.begin?unit.rawStart:w.begin),65535,0};
            const unsigned stop=unit.rawEnd<w.end?unit.rawEnd:w.end;
            while(const u32 cp=OrthographyNextRaw(w.cursor,stop,false))OrthographyAdvance(cp,x,y);
            if(w.failed)break;
        }
    }
    if(w.failed){OrthographyAbort();return false;}
    if(w.count<OrthographyMaxUnits&&kotogba_recapNumber(&gKotoTextRecords[w.frame.page&0x3fff])){
        if(x+6>234){x=10;y+=OrthographyLine;}
        sOrthographyPlaces[w.count++]={(u16)x,(u16)y,6,4};
    }
    w.height=y+OrthographyLine;w.active=true;return true;
}
static bool OrthographyPrepare(const ReaderFrame& f)
{
    const unsigned slot=f.page&0x3fff;
    return NativeFrame(f)&&slot<gKotoTextCapacity&&OrthographyCaptureCurrent(f,gKotoTextRecords[slot]);
}
static bool OrthographyQuestion(const ReaderFrame& f)
{
    if(!f.menu||f.menu!=sGameMenu||f.tab||!NativeFrame(f)||(f.page&0x4000)||!FindStudy(sKotoGbaActiveCardId))return false;
    const unsigned slot=f.page&0x3fff,kind=f.menu>>4;
    if(slot>=gKotoTextCapacity||!kind||kind>3)return false;const auto& r=gKotoTextRecords[slot];
    if(r.window!=sPrimaryWindow||LegacyOriginalPage(r.cursor?r.cursor:r.origin)!=sKotoGbaActiveCardId)return false;
    return kind==2||(kind==1&&sKotoGbaActiveCardId==8)||(kind==3&&(sKotoGbaActiveCardId==26||sKotoGbaActiveCardId==27));
}
static unsigned OrthographyHeight(const ReaderFrame& f,unsigned height,bool translated)
{
    if(!OrthographyActive(f))return height;
    sOrthography.top=translated&&height>=42?height-42:0;
    const unsigned menu=OrthographyQuestion(f)?Read16(Read32(36)+8*sGameMenuPages[(f.menu>>4)-1]+6)+12:0;
    return height+sOrthography.height+menu;
}
// Reuse the already-validated complete Spanish template path. Its temporary
// layout lives in the existing native scratch; normalized JP lives in the
// pinned sidecar/placements and does not compete for those bytes.
[[gnu::target("thumb"),gnu::noinline]] static unsigned OrthographyFullSpanish(const ReaderFrame& f,const char* text,unsigned out)
{
    auto& v=LegacyWork();memset(&v,0,sizeof(v));const auto& r=gKotoTextRecords[f.page&0x3fff];
    v.origin=r.origin;v.context=r.context;v.out=out;
    if(!SentenceLegacyExpandWhole(text))return out;
    SentenceLegacyWrapSpanish();return v.out?v.out-1:out;
}
static void OrthographyRenderBegin()
{
    auto& w=sOrthography;w.paintUnit=w.paintPhase=w.paintAt=w.paintDisplayBase=0;
    w.cursor={0,65535,0};
}
static bool OrthographyInlineWord(unsigned begin,unsigned end,unsigned displayBegin,unsigned displayEnd,u32& card,u16& ref,u16& flags)
{
    LinguisticInlineSpan span;unsigned best=65535;bool found=false;
    for(unsigned i=0;i<LinguisticInlineSpanCount();++i){
        if(!LinguisticInlineSpanAt(i,span))continue;
        const bool display=displayBegin!=65535&&displayEnd!=65535;
        const unsigned a=display?span.displayStart:span.rawStart,b=display?span.displayEnd:span.rawEnd;
        const unsigned first=display?displayBegin:begin,last=display?displayEnd:end;
        if(a<=first&&last<=b&&b-a<best){best=b-a;card=65535;ref=span.card;flags=span.flags;found=true;}
    }
    return found;
}
static bool OrthographyWord(unsigned begin,unsigned end,u32& card,u16& ref,
                             unsigned displayBegin=65535,unsigned displayEnd=65535)
{
    // The optional sidecar word table preserves exact static KS5 destinations.
    // KC15 already-reviewed patterns remain a bounded additional fallback.
    const bool exactDisplay=displayBegin!=65535&&displayEnd!=65535&&LinguisticInlineSpanCount()>0;
    u32 inlineCard=0,identity=0;u16 inlineRef=65535,inlineFlags=0;
    const bool inlineFound=OrthographyInlineWord(begin,end,displayBegin,displayEnd,inlineCard,inlineRef,inlineFlags);
    // A narrowly reviewed source span may explicitly correct a historically
    // misbound popup. Only an exact display winner can exercise this opt-in;
    // original lexical tuples and their default priority remain unchanged.
    if(inlineFound&&exactDisplay&&(inlineFlags&2)){card=inlineCard;ref=inlineRef;return true;}
    if(inlineFound&&exactDisplay&&LinguisticInlineCardAt(inlineRef,sOrthographyInlineMatch))identity=sOrthographyInlineMatch.identity;
    LinguisticWord word;unsigned length=0;
    for(unsigned i=0;i<LinguisticWordCount();++i)if(LinguisticWordAt(i,word)&&begin<word.rawEnd&&word.rawStart<end&&
       (!exactDisplay||(word.rawStart<=begin&&end<=word.rawEnd)||(identity&&CorpusWordIdentity(word.card,word.ref)==identity))&&word.rawEnd-word.rawStart>length){
        length=word.rawEnd-word.rawStart;card=word.card;ref=word.ref;}
    if(length)return true;
    if(CorpusRawWord(begin,end,card,ref,identity,exactDisplay))return true;
    if(inlineFound){card=inlineCard;ref=inlineRef;return true;}return false;
}
static u8 OrthographyColor(unsigned begin,unsigned end,unsigned displayBegin=65535,unsigned displayEnd=65535)
{u32 card;u16 ref;if(OrthographyWord(begin,end,card,ref,displayBegin,displayEnd)){
    // Match the selected destination, not an overlapping lesson's full range.
    // Grammar uses the same ink in aligned, clipped and ruby glyph painters;
    // GrammarBlue is reserved for the section heading and notebook icon.
    if(card==65535&&LinguisticInlineCardAt(ref,sOrthographyInlineColor)&&
       (sOrthographyInlineColor.type==1||sOrthographyInlineColor.type==2))return Ink;
    return card!=65535&&(ref&0x8000)?Ink:Teal;}
 return Ink;}
[[gnu::noinline]] static u8 OrthographyRubyColor(unsigned begin,unsigned end,unsigned displayBegin,unsigned displayEnd)
{
    // Ruby belongs to the complete displayed base. A reviewed grammar override
    // may colour it only when that entire base fits its exact display span.
    // Shared raw ranges of converted mixed words must retain their old colour.
    u32 card;u16 ref,flags;
    if(OrthographyInlineWord(begin,end,displayBegin,displayEnd,card,ref,flags)&&
       (flags&2)&&LinguisticInlineCardAt(ref,sOrthographyInlineColor)&&
       (sOrthographyInlineColor.type==1||sOrthographyInlineColor.type==2))return Ink;
    return OrthographyColor(begin,end);
}
// A raw byte may correspond to a kana glyph only after exact identity is
// established. Converted kanji, control atoms and frozen variables stay atomic.
static bool OrthographyIdentityUnit(const LinguisticUnit& unit)
{
    if(unit.readingBytes||unit.rawStart>=unit.rawEnd||unit.rawEnd>sOrthography.rawSize)return false;
    const char* p=unit.display;for(unsigned at=unit.rawStart;at<unit.rawEnd;++at){
        const u8 c=sOrthography.raw[at];if(c>=0xf7||!*p||DecodeUtf8(p)!=OrthographyCodepoint(c))return false;}
    return !*p;
}
static bool OrthographyMenuRender()
{
    auto& w=sOrthography;if(!OrthographyQuestion(sRender.target))return true;
    const unsigned kind=sRender.target.menu>>4,choice=sRender.target.menu&15;
    const u32 pg=Read32(36)+8*sGameMenuPages[kind-1];const unsigned count=Read16(pg+4);
    const int top=56+w.top+w.height+12-sRender.target.scroll;
    if(w.paintAt<count){const u32 refs=Read32(40)+Read32(24)*8;
        const u32 line=Read32(40)+8*Read16(refs+2*(Read32(pg)+w.paintAt++));
        SetText(sStudyData[line],top+Read16(line+2),(const char*)sStudyData+Read32(line+4),sStudyData[line+1]==0?Teal:Ink);return false;}
    if(w.paintAt++==count){SetText(2,top+sGameMenuChoiceY[kind-1][choice]+3,">",Teal);return false;}
    return true;
}
// JP Quest Log stores defeated/caught counts as bytes. The dictionary reading
// ひき has a voiced first kana after 3 and a half-voiced one after 1/6/8/0.
// Use the immutable count belonging to this publication; a name or a live
// variable cannot alter ruby. All three kana have identical glyph geometry.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static u32 OrthographyRecapCounterRuby(const LinguisticUnit& unit,u32 cp)
{
    const auto& w=sOrthography;
    if(w.source<0x3dc454||w.source>0x3dc50f||unit.rawStart<2||
       unit.displayBytes!=3||unit.readingBytes!=6||
       (u8)unit.display[0]!=0xe5||(u8)unit.display[1]!=0x8c||(u8)unit.display[2]!=0xb9||
       w.raw[unit.rawStart-2]!=0xf7)return cp;
    const unsigned token=w.raw[unit.rawStart-1];if(token!=2&&token!=4)return cp;
    const auto& r=gKotoTextRecords[w.frame.page&0x3fff];
    const u8* value=(const u8*)kotogba_variableSource(r.origin,768+token,r.context);
    if(!value)return cp;
    unsigned n=0,last=0,i=0;
    while(i<3&&value[i]>=0xa1&&value[i]<=0xaa){last=value[i++]-0xa1;n=n*10+last;}
    if(!i||value[i]!=0xff||!n||n>255)return cp;
    return cp+(last==3?1:((1u<<last)&0x143)?2:0);
}
[[gnu::target("thumb"),gnu::noinline]] static bool OrthographyRenderUnit()
{
    auto& w=sOrthography;if(!OrthographyActive(sRender.target)||!sNativeTextOrthography||
       sNativeTextCard!=sRender.target.card||sNativeTextPage!=sRender.target.page)return true;
    // Scroll paints expose only a narrow strip. Do not resolve every lexical
    // span in the document when the Japanese block cannot touch that strip.
    // The retained placement map still owns every hit-test coordinate.
    const int blockTop=56+w.top-sRender.target.scroll;
    if(w.paintUnit<w.count&&(blockTop>=sRender.clipBottom||blockTop+w.height<=sRender.clipTop)){
        w.paintUnit=w.count;w.paintAt=0;
    }
    if(w.paintUnit>=w.count)return OrthographyMenuRender();
    const auto& place=sOrthographyPlaces[w.paintUnit];
    if(place.mode==4){
        const int y=blockTop+place.y+12;const unsigned number=kotogba_recapNumber(&gKotoTextRecords[w.frame.page&0x3fff]);
        if(number&&y<sRender.clipBottom&&y+16>sRender.clipTop)Glyph(place.x,y,number,Ink);
        ++w.paintUnit;w.paintPhase=w.paintAt=0;return false;
    }
    if(!LinguisticUnitAt(w.paintUnit,w.unit)){++w.paintUnit;return false;}
    const auto& unit=w.unit;
    if(!place.mode){w.paintDisplayBase+=unit.displayBytes;++w.paintUnit;w.paintPhase=w.paintAt=0;return false;}
    // Placements are monotonic in document Y. All later units are also below
    // the clip, so even their UTF8/color lookup is unnecessary for this paint.
    if(blockTop+place.y>=sRender.clipBottom){w.paintUnit=w.count;w.paintAt=0;return false;}
    if(place.mode==1||place.mode==3){
        if(blockTop+place.y+28<=sRender.clipTop){
            w.paintDisplayBase+=unit.displayBytes;++w.paintUnit;w.paintPhase=w.paintAt=0;return false;
        }
        if(place.mode==3)w.paintPhase=1;
        const char* begin=w.paintPhase?unit.display:unit.reading;const char* p=begin+w.paintAt;
        if(!*p){if(!w.paintPhase){w.paintPhase=1;w.paintAt=0;}else{w.paintDisplayBase+=unit.displayBytes;++w.paintUnit;w.paintPhase=w.paintAt=0;}return false;}
        if(!w.paintAt){const unsigned width=OrthographyTextWidth(begin,w.paintPhase?0:8);w.paintX=place.x+(place.width-width)/2;}
        const unsigned before=w.paintAt;u32 cp=DecodeUtf8(p);w.paintAt=p-begin;const unsigned width=w.paintPhase?(cp>255?14:6):8;
        const int y=56+w.top+place.y+(w.paintPhase?12:0)-sRender.target.scroll;
        if(!w.paintPhase&&!before&&cp==0x3072)cp=OrthographyRecapCounterRuby(unit,cp);
        if(!w.paintPhase&&cp>=0x3040&&cp<=0x30ff)cp=0xe000+cp-0x3040;
        const unsigned base=w.paintDisplayBase;
        const unsigned db=w.paintPhase?base+before:65535,de=w.paintPhase?base+w.paintAt:65535;
        if(y<sRender.clipBottom&&y+16>sRender.clipTop)
            Glyph(w.paintX,y,cp,w.paintPhase?OrthographyColor(unit.rawStart,unit.rawEnd,db,de):
                  OrthographyRubyColor(unit.rawStart,unit.rawEnd,base,base+unit.displayBytes));
        w.paintX+=width;return false;
    }
    if(!w.paintPhase){w.paintPhase=OrthographyIdentityUnit(unit)?3:1;w.paintX=place.x;w.paintY=place.y;
        w.cursor={(u16)(unit.rawStart>w.begin?unit.rawStart:w.begin),65535,0};w.paintAt=0;
        if(w.paintPhase==3){const char* p=unit.display;for(unsigned at=unit.rawStart;at<w.cursor.at;++at)DecodeUtf8(p);w.paintAt=p-unit.display;}}
    const unsigned stop=unit.rawEnd<w.end?unit.rawEnd:w.end;
    const u32 cp=OrthographyNextRaw(w.cursor,stop,false);
    if(!cp){w.paintDisplayBase+=unit.displayBytes;++w.paintUnit;w.paintPhase=w.paintAt=0;return false;}
    const unsigned before=w.paintAt;
    if(w.paintPhase==3){const char* p=unit.display+before;DecodeUtf8(p);w.paintAt=p-unit.display;}
    if(cp=='\n'){w.paintX=10;w.paintY+=OrthographyLine;return false;}
    const unsigned width=cp>255?14:6;if(w.paintX+width>234){w.paintX=10;w.paintY+=OrthographyLine;}
    const unsigned db=w.paintPhase==3?w.paintDisplayBase+before:65535,de=w.paintPhase==3?w.paintDisplayBase+w.paintAt:65535;
    const int y=blockTop+w.paintY+12;
    if(y<sRender.clipBottom&&y+16>sRender.clipTop)
        Glyph(w.paintX,y,cp,OrthographyColor(w.atomBegin,w.atomEnd,db,de));
    w.paintX+=width;return false;
}
// Hit testing repeats the same bounded layout, not an approximate UTF8 offset.
// Raw spans preserve lexical IDs even when one glyph replaces several kana.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static bool OrthographyFindGlyph(unsigned x,unsigned y)
{
    auto& w=sOrthography;w.hitCp=0;w.hitDisplayBegin=w.hitDisplayEnd=65535;
    if(!sHasShown||!sNativeTextOrthography||sNativeTextCard!=sShown.card||sNativeTextPage!=sShown.page||
       !OrthographyActive(sShown)||(sRender.busy&&!SamePage(sRender.target,sShown))||y<56||y>=173)return false;
    const int target=(int)y-56+sShown.scroll-w.top;
    unsigned displayBase=0;
    for(unsigned i=0;i<w.count;++i){const auto& place=sOrthographyPlaces[i];
        if(place.mode==4)continue;
        if(!LinguisticUnitAt(i,w.unit))return false;const auto& unit=w.unit;
        const unsigned unitDisplayBase=displayBase;displayBase+=unit.displayBytes;if(!place.mode)continue;
        if(place.mode==1||place.mode==3){
            if(target<(int)place.y||target>=(int)place.y+28)continue;
            const unsigned width=OrthographyTextWidth(unit.display,0);unsigned xx=place.x+(place.width-width)/2;
            const bool identity=OrthographyIdentityUnit(unit);unsigned raw=unit.rawStart;
            const char* p=unit.display;while(*p){const char* before=p;const u32 cp=DecodeUtf8(p);const unsigned cell=cp>255?14:6;
                if(x+2>=xx&&x<xx+cell+2){w.hitCp=cp;w.hitBegin=identity?raw:unit.rawStart;w.hitEnd=identity?raw+1:unit.rawEnd;
                    w.hitDisplayBegin=unitDisplayBase+(before-unit.display);w.hitDisplayEnd=unitDisplayBase+(p-unit.display);return true;}xx+=cell;++raw;}
            // Ruby outside the centered base still selects its lexical group.
            if(x>=place.x&&x<place.x+place.width){
                p=unit.display;const char* selected=p;unsigned ordinal=0,selectedOrdinal=0;
                while(*p){selected=p;selectedOrdinal=ordinal++;DecodeUtf8(p);if(x<place.x+place.width/2)break;}
                p=selected;w.hitCp=DecodeUtf8(p);w.hitBegin=identity?unit.rawStart+selectedOrdinal:unit.rawStart;w.hitEnd=identity?w.hitBegin+1:unit.rawEnd;
                w.hitDisplayBegin=unitDisplayBase+(selected-unit.display);w.hitDisplayEnd=unitDisplayBase+(p-unit.display);return true;}
        }else{
            if(target<(int)place.y)continue;
            unsigned xx=place.x,yy=place.y;w.hitCursor={(u16)(unit.rawStart>w.begin?unit.rawStart:w.begin),65535,0};
            const bool identity=OrthographyIdentityUnit(unit);const char* display=unit.display;
            if(identity)for(unsigned at=unit.rawStart;at<w.hitCursor.at;++at)DecodeUtf8(display);
            const unsigned stop=unit.rawEnd<w.end?unit.rawEnd:w.end;
            while(const u32 cp=OrthographyNextRaw(w.hitCursor,stop,false)){
                const char* displayBefore=display;if(identity)DecodeUtf8(display);
                if(cp=='\n'){xx=10;yy+=OrthographyLine;continue;}
                const unsigned cell=cp>255?14:6;if(xx+cell>234){xx=10;yy+=OrthographyLine;}
                if(target>=(int)yy+12&&target<(int)yy+28&&x+2>=xx&&x<xx+cell+2){w.hitCp=cp;w.hitBegin=w.atomBegin;w.hitEnd=w.atomEnd;
                    if(identity){w.hitDisplayBegin=unitDisplayBase+(displayBefore-unit.display);w.hitDisplayEnd=unitDisplayBase+(display-unit.display);}return true;}xx+=cell;
                if((int)yy>target)break;
            }
        }
    }
    return false;
}
[[gnu::target("thumb"),gnu::noinline]] static bool OrthographyMenuTouch(unsigned x,unsigned y,u32& card,u16& ref)
{
    // These are the original appended answer lines and their original KS5
    // reference IDs, now shifted below the reviewed native question.
    if(!sHasShown||!sNativeTextOrthography||sNativeTextCard!=sShown.card||sNativeTextPage!=sShown.page||
       y<56||y>=173||(sRender.busy&&!SamePage(sRender.target,sShown))||
       !OrthographyActive(sShown)||!OrthographyQuestion(sShown)||!sTouchOffset)return false;
    const unsigned page=sGameMenuPages[(sShown.menu>>4)-1];unsigned lo=0,hi=Read16(sTouchOffset);
    while(lo<hi){const unsigned m=(lo+hi)/2;if(Read16(sTouchOffset+4+m*6)<page)lo=m+1;else hi=m;}
    if(lo==Read16(sTouchOffset))return false;const u32 group=sTouchOffset+4+lo*6;if(Read16(group)!=page)return false;
    const u32 data=sTouchOffset+4+Read16(sTouchOffset)*6,pg=Read32(36)+8*page,refs=Read32(40)+Read32(24)*8;
    for(unsigned i=0;i<Read16(group+4);++i){const u32 hit=data+(Read16(group+2)+i)*5;const unsigned local=sStudyData[hit+2];
        if(local>=Read16(pg+4))continue;const u32 line=Read32(40)+8*Read16(refs+2*(Read32(pg)+local));
        const int yy=56+sOrthography.top+sOrthography.height+12+Read16(line+2)-sShown.scroll;
        const int ruby=sStudyData[Read32(line+4)]>=0x80?12:0;
        if((int)y>=yy-ruby&&(int)y<yy+16&&x+2>=sStudyData[hit+3]&&x<sStudyData[hit+4]+2){card=sKotoGbaActiveCardId;ref=Read16(hit);return true;}}
    return false;
}
[[gnu::target("thumb"),gnu::noinline]] static bool OrthographyTouch(unsigned x,unsigned y,u32& card,u16& ref)
{
    if(OrthographyFindGlyph(x,y))return OrthographyWord(sOrthography.hitBegin,sOrthography.hitEnd,card,ref,
        sOrthography.hitDisplayBegin,sOrthography.hitDisplayEnd)||
        GrammarAnchor(sOrthography.hitBegin,sOrthography.hitEnd,card,ref);
    return OrthographyMenuTouch(x,y,card,ref);
}
static u32 OrthographyGlyphAt(unsigned x,unsigned y)
{return OrthographyFindGlyph(x,y)?sOrthography.hitCp:0;}
