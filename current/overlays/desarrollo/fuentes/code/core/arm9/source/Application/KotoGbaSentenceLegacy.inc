// Complete sentences over the approved static pages.  There is no second KS5
// or copied glyph bank: coordinates are rebased and original touch owners stay
// attached to their own vocabulary/grammar references.
#include "KotoGbaSentences.generated.h"
static const char* CorpusSentenceTranslation(unsigned slot);

struct [[gnu::may_alias]] SentenceLegacyWork {
    const SentenceLegacyPart* part;
    unsigned index,phase,local,page,first,refs,line,x,y,style;
    const char* text;
};
static_assert(sizeof(SentenceLegacyWork)<=sizeof(CorpusComposeWork));
[[gnu::always_inline]] static inline SentenceLegacyWork& SentenceLegacyScratch()
{return *(SentenceLegacyWork*)&sCorpusCompose;}
static unsigned SentenceLegacyGroupFor(u32 source)
{
    unsigned lo=0,hi=sizeof(sSentenceLegacyGroups)/sizeof(sSentenceLegacyGroups[0]);
    while(lo<hi){const unsigned mid=(lo+hi)/2;if(sSentenceLegacyGroups[mid].source<source)lo=mid+1;else hi=mid;}
    return lo<sizeof(sSentenceLegacyGroups)/sizeof(sSentenceLegacyGroups[0])&&sSentenceLegacyGroups[lo].source==source?lo:65535;
}
static bool IsSentenceLegacyFull(const ReaderFrame& f)
{
    if((f.card&0xc0000000u)!=0x40000000u||(f.card&0x3fffffffu)>=sizeof(sSentenceLegacyGroups)/sizeof(sSentenceLegacyGroups[0])||f.page>=KOTO_TEXT_TOTAL_SLOTS||f.tab||!gKotoTextRecords)return false;
    const auto& record=gKotoTextRecords[f.page];const auto& group=sSentenceLegacyGroups[f.card&0x3fffffffu];
    if(!record.ready||!record.sequence||record.origin!=group.source)return false;
    u32 mask=0;for(unsigned i=0;i<group.count;++i)mask|=1u<<sSentenceLegacyParts[group.first+i].ordinal;
    return (sCorpusRefs[f.page].seen&mask)==mask;
}
static unsigned SentenceLegacyMask(unsigned group)
{
    if(group>=sizeof(sSentenceLegacyGroups)/sizeof(sSentenceLegacyGroups[0]))return 0;
    const auto& g=sSentenceLegacyGroups[group];u32 mask=0;
    for(unsigned i=0;i<g.count;++i)mask|=1u<<sSentenceLegacyParts[g.first+i].ordinal;
    return mask;
}
[[gnu::always_inline]] static inline u32 SentenceLegacyPage(const SentenceLegacyPart& part)
{return Read32(36)+8*Card16(FindStudy(part.card),4);}
[[gnu::always_inline]] static inline u32 SentenceLegacyLineAddress(u32 page,unsigned local)
{return Read32(40)+8*Read16(Read32(40)+Read32(24)*8+2*(Read32(page)+local));}
static unsigned SentenceLegacyLineCount(const ReaderFrame& f)
{
    if(!IsSentenceLegacyFull(f))return 0;
    const auto& g=sSentenceLegacyGroups[f.card&0x3fffffffu];unsigned count=4;
    for(unsigned i=0;i<g.count;++i)count+=sSentenceLegacyParts[g.first+i].jpCount;
    return count;
}
static unsigned SentenceLegacyJapaneseHeader(const SentenceLegacyGroup& group)
{
    const auto& last=sSentenceLegacyParts[group.first+group.count-1];
    return Read16(SentenceLegacyLineAddress(SentenceLegacyPage(last),last.esCount)+2)+last.esShift+27;
}
// Resolve one virtual line into the existing corpus compose scratch.  This
// scratch is idle while a static view is drawn; LegacyExpand uses another
// existing work area and its own normalized-text scratch.
[[gnu::target("thumb"),gnu::noinline]] static bool SentenceLegacyResolve(const ReaderFrame& f,unsigned index)
{
    if(!IsSentenceLegacyFull(f))return false;
    auto& w=SentenceLegacyScratch();const auto& g=sSentenceLegacyGroups[f.card&0x3fffffffu];
    w.part=nullptr;w.phase=0;w.x=10;w.style=0;w.y=index?27:0;
    if(index<2){w.text=index?"En español":"Oración completa";return true;}
    if(index==2){w.y=54;w.style=1;w.text="";return true;}
    w.phase=1;if(index==3){w.y=SentenceLegacyJapaneseHeader(g);w.text="Lectura con kanji";return true;}
    index-=4;
    for(w.index=0;w.index<g.count;++w.index){const auto& p=sSentenceLegacyParts[g.first+w.index];
        if(index<p.jpCount){w.part=&p;w.local=p.jpFirst+index;break;}index-=p.jpCount;}
    if(!w.part)return false;
    w.page=SentenceLegacyPage(*w.part);w.line=SentenceLegacyLineAddress(w.page,w.local);
    w.x=sStudyData[w.line];w.style=sStudyData[w.line+1];
    w.y=Read16(w.line+2)+w.part->jpShift;
    w.text=(const char*)sStudyData+Read32(w.line+4);return true;
}
// Copy a whole translated template before wrapping it. Literal and variable
// chunks must share the same word boundary: a dynamic Pablo is not Pabl / o.
[[gnu::always_inline]] static inline bool SentenceLegacyCopyUnit(const char*& source)
{
    auto& v=LegacyWork();const u8 lead=(u8)*source;
    const unsigned count=lead>=0xc2&&lead<=0xdf?2:lead>=0xe0&&lead<=0xef?3:lead>=0xf0&&lead<=0xf4?4:1;
    if(v.out+count>=1536)return false;
    for(unsigned i=0;i<count;++i)sNativeRenderText[v.out++]=*source++;
    return true;
}
[[gnu::target("arm"),gnu::noinline]] static bool SentenceLegacyExpandWhole(const char* whole)
{
    auto& v=LegacyWork();v.at=whole;
    while(*v.at){
        v.token=*v.at=='{'?LegacyToken(v.at):0;
        if(v.token){
            v.at+=v.token>>16;v.begin=CorpusVariableText(v.origin,v.token&65535,v.context);
            while(*v.begin)if(!SentenceLegacyCopyUnit(v.begin))return false;
        }else if(!SentenceLegacyCopyUnit(v.at))return false;
    }
    sNativeRenderText[v.out++]=0;return true;
}
// Wrap the complete copied string, with dynamic values already inserted.
// Existing spaces are replaced by newlines; long single words can hard-wrap.
[[gnu::target("arm"),gnu::noinline]] static unsigned SentenceLegacyWrapSpanish()
{
    auto& v=LegacyWork();v.first=0;v.refs=0xffffffffu;v.width=0;v.total=0;
    for(v.index=0;v.index+1<v.out;){
        const char* p=sNativeRenderText+v.index;const u32 cp=DecodeUtf8(p);const unsigned bytes=p-(sNativeRenderText+v.index),width=cp>255?14:6;
        if(cp=='\n'){v.index+=bytes;v.first=v.index;v.refs=0xffffffffu;v.width=0;++v.total;continue;}
        if(cp==' ')v.refs=v.index;
        if(v.width+width>224){
            if(v.refs!=0xffffffffu&&v.refs>=v.first){sNativeRenderText[v.refs]='\n';v.index=v.refs+1;}
            else{if(v.out==1536)break;memmove(sNativeRenderText+v.index+1,sNativeRenderText+v.index,v.out-v.index);sNativeRenderText[v.index++]='\n';++v.out;}
            v.first=v.index;v.refs=0xffffffffu;v.width=0;++v.total;continue;
        }
        v.width+=width;v.index+=bytes;
    }
    return v.total+1;
}
// Only expanded lines need scratch entries.  A sentence may have more than
// 64 visible lines; its variable-bearing lines remain bounded by this existing
// 64-entry layout.  No render line index is used as an unchecked array index.
[[gnu::target("arm"),gnu::noinline]] static unsigned SentenceLegacyPrepare(const ReaderFrame& f)
{
    if(!IsSentenceLegacyFull(f)||!gKotoTextRecords)return 0;
    const auto& record=gKotoTextRecords[f.page];
    auto& v=LegacyWork();
    if(sLegacyTextCard==f.card&&sLegacyTextPage==f.page&&v.index==record.sequence&&v.origin==record.origin&&v.context==record.context&&v.refs==CorpusGeneration())return v.height+v.total;
    const char* whole=CorpusSentenceTranslation(f.page);
    if(!whole)return sLegacyTextCard==f.card&&sLegacyTextPage==f.page&&v.index==record.sequence&&v.origin==record.origin&&v.context==record.context?v.height+v.total:0;
    sNativeTextCard=0;sLegacyTextCard=0;memset(&v,0,sizeof(v));
    v.origin=record.origin;v.context=record.context;v.page=f.page;
    const auto& group=sSentenceLegacyGroups[f.card&0x3fffffffu];
    const auto& last=sSentenceLegacyParts[group.first+group.count-1];
    v.height=Read16(SentenceLegacyLineAddress(SentenceLegacyPage(last),last.jpFirst+last.jpCount-1)+2)+last.jpShift+18;
    v.japanese=0;if(!SentenceLegacyExpandWhole(whole))return 0;
    v.refs=54+14*(SentenceLegacyWrapSpanish()-1)+27-SentenceLegacyJapaneseHeader(group);
    v.height+=(s32)v.refs;v.total=0;v.lines[0]={0,0,3};v.count=1;
    v.first=SentenceLegacyLineCount(f);
    for(v.index=4;v.index<v.first;++v.index){
        if(!SentenceLegacyResolve(f,v.index))continue;auto& w=SentenceLegacyScratch();
        bool variable=false;for(const char* p=w.text;*p;++p)if(*p=='{'&&LegacyToken(p)){variable=true;break;}
        if(!variable)continue;
        // The approved source inventory has at most a handful of variable
        // lines per sentence. Leave a complete per-line bound before expanding.
        if(v.count==64||v.out+512>1536)continue;
        const unsigned offset=v.out;v.at=w.text;v.width=0;v.japanese=w.phase;
        LegacyExpand();
        for(unsigned at=offset;at+1<v.out;++at)if(sNativeRenderText[at]=='\n')v.total+=14;
        v.lines[v.count++]={(u16)offset,(u16)v.total,(u16)(v.index+1)};
    }
    v.first=v.refs;v.index=record.sequence;v.refs=CorpusGeneration();sLegacyTextCard=f.card;sLegacyTextPage=f.page;return v.height+v.total;
}
static unsigned SentenceLegacyHeight(const ReaderFrame& f)
{return SentenceLegacyPrepare(f);}
[[gnu::target("arm"),gnu::noinline]] static void SentenceLegacyReadLine(const ReaderFrame& f,unsigned index)
{
    if(!SentenceLegacyResolve(f,index))return;
    auto& w=SentenceLegacyScratch();const auto& v=LegacyWork();unsigned shift=0;const char* text=w.text;
    if(sLegacyTextCard==f.card&&sLegacyTextPage==f.page)for(unsigned i=0;i<v.count;++i){const auto& item=v.lines[i];
        if(item.expanded==index+1){text=sNativeRenderText+item.offset;break;}
        if(item.expanded>index+1)break;shift=item.shift;}
    // A future pathological template must never expose its raw control tokens.
    // Current source fixtures verify that every variable line is cached above.
    if(text==w.text)for(const char* p=text;*p;++p)if(*p=='{'&&LegacyToken(p)){text="…";break;}
    const int y=56+(int)w.y+(index>=3?(s32)v.first:0)+shift-f.scroll;
    if(y+16>sRender.clipTop&&y<sRender.clipBottom)SetText(w.x,y,text,w.style==0?Teal:Ink);
}
// Tap coordinates are transformed back to the exact original page and owner.
// No reference ordinal is reinterpreted under the first fragment's card.
[[gnu::target("thumb"),gnu::noinline]] static bool SentenceLegacyTouch(unsigned x,unsigned y,u32& card,u16& ref)
{
    if(!sHasShown||(sRender.busy&&!SamePage(sRender.target,sShown))||!IsSentenceLegacyFull(sShown)||!sTouchOffset||y<56||y>=173)return false;
    const unsigned count=SentenceLegacyLineCount(sShown);const auto& v=LegacyWork();unsigned shift=0;
    for(unsigned index=4;index<count;++index){
        if(!SentenceLegacyResolve(sShown,index))continue;auto& w=SentenceLegacyScratch();
        if(!w.part)continue;
        if(sLegacyTextCard==sShown.card&&sLegacyTextPage==sShown.page)for(unsigned i=0;i<v.count;++i){
            if(v.lines[i].expanded>=index+1)break;shift=v.lines[i].shift;}
        const int yy=56+(int)w.y+(s32)v.first+shift-sShown.scroll;
        const int ruby=((const u8*)w.text)[0]>=0x80?12:0;
        if((int)y<yy-ruby||(int)y>=yy+16)continue;
        const unsigned page=Card16(FindStudy(w.part->card),4);unsigned lo=0,hi=Read16(sTouchOffset);
        while(lo<hi){const unsigned mid=(lo+hi)/2;if(Read16(sTouchOffset+4+6*mid)<page)lo=mid+1;else hi=mid;}
        if(lo==Read16(sTouchOffset))continue;const u32 group=sTouchOffset+4+6*lo;
        if(Read16(group)!=page)continue;
        const u32 hits=sTouchOffset+4+6*Read16(sTouchOffset);const unsigned first=Read16(group+2),n=Read16(group+4);
        for(unsigned i=0;i<n;++i){const u32 hit=hits+5*(first+i);
            if(sStudyData[hit+2]==w.local&&x+2>=sStudyData[hit+3]&&x<sStudyData[hit+4]+2){
                card=w.part->card;ref=Read16(hit);return true;}}
    }
    return false;
}
