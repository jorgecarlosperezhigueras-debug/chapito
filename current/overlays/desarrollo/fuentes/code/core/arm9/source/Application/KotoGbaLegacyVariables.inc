// Approved KS5 pages retain their IDs, ruby and touch destinations. Reviewed
// JP placeholders and ES templates use the same frozen expansion snapshots;
// Japanese always bypasses Spanish entity-name resolution. The shared native
// scratch is idle while a static page is drawn.
struct LegacyLine {u16 offset,shift,expanded;};
struct LegacyLayout {
    LegacyLine lines[64];
    u32 origin,context,japanese,out;
    unsigned width;
    u32 index,first,refs;
    const char *at,*begin;
    u32 token,total,height;
    u16 page,count;
};
static_assert(sizeof(LegacyLayout)==440);
[[gnu::section(".ewram.bss")]] static u32 sLegacyTextCard;
[[gnu::section(".ewram.bss")]] static u16 sLegacyTextPage,sLegacyTextHeight;
[[gnu::always_inline]] static inline LegacyLayout& LegacyWork()
{return *(LegacyLayout*)(sNativeRenderText+1536);}
// Packed token/length; braces in prose remain literal unless fully recognized.
[[gnu::target("arm"),gnu::noinline]] static u32 LegacyToken(const char* p)
{
    if(*p!='{')return 0;
    if(!memcmp(p,"{PLAYER}",8))return (8u<<16)|1;
    if(!memcmp(p,"{RIVAL}",7))return (7u<<16)|6;
    if(!memcmp(p,"{STR_VAR_",9)&&p[9]>='1'&&p[9]<='3'&&p[10]=='}')return (11u<<16)|(p[9]-'0'+1);
    if(!memcmp(p,"{FD_",4)){
        unsigned i=4,n=0;for(;i<7&&p[i]>='0'&&p[i]<='9';++i)n=n*10+p[i]-'0';
        if(i>4&&p[i]=='}'&&n<256)return ((i+1)<<16)|n;
    }
    if(!memcmp(p,"{DYNAMIC_",9)){
        unsigned i=9,n=0;for(;i<12&&p[i]>='0'&&p[i]<='9';++i)n=n*10+p[i]-'0';
        if(i>9&&p[i]=='}'&&n<256)return ((i+1)<<16)|(768+n);
    }
    return 0;
}
[[gnu::target("arm"),gnu::noinline]] static unsigned LegacyLiteral(unsigned out,const char* begin,const char* end,unsigned& width)
{
    // Current KS5 lines fit one chunk. The bounded fallback also handles a
    // future long literal without writing outside the existing 512B decoder.
    while(unsigned(end-begin)>511){memcpy(sNativeNormalized,begin,511);sNativeNormalized[511]=0;out=CorpusAppend(out,(const char*)sNativeNormalized,width);begin+=511;}
    memcpy(sNativeNormalized,begin,end-begin);sNativeNormalized[end-begin]=0;
    return CorpusAppend(out,(const char*)sNativeNormalized,width);
}
[[gnu::target("arm"),gnu::noinline]] static void LegacyExpand()
{
    auto& w=LegacyWork();w.begin=w.at;
    while(*w.at){
        if(*w.at=='{'&&(w.token=LegacyToken(w.at))){
            w.out=LegacyLiteral(w.out,w.begin,w.at,w.width);
            // Study Japanese must show the same frozen guest glyphs. The ES
            // resolver may translate an item name but must never enter JP.
            if(w.japanese)w.out=CorpusAppend(w.out,CorpusVariableDecode(kotogba_variableSource(w.origin,w.token&65535,w.context),0),w.width);
            else w.out=CorpusVariable(w.out,w.origin,w.token&65535,w.context,w.width);
            w.at+=w.token>>16;w.begin=w.at;
        }else ++w.at;
    }
    w.out=LegacyLiteral(w.out,w.begin,w.at,w.width);sNativeRenderText[w.out++]=0;
}
[[gnu::target("arm"),gnu::noinline]] static void LegacyLanguage()
{
    // Header comparisons run before expanding a line. Keep their constants
    // and temporary registers out of the deep entity-resolver call chain.
    auto& w=LegacyWork();w.japanese=!strcmp(w.at,"Lectura con kanji")||!strcmp(w.at,"Japonés");
}
[[gnu::target("arm"),gnu::noinline]] static unsigned LegacyBuild()
{
    auto& w=LegacyWork();
    for(w.index=0;w.index<w.count;++w.index){
        const u32 line=Read32(40)+8*Read16(w.refs+2*(w.first+w.index));
        w.lines[w.index].shift=w.total;w.at=(const char*)sStudyData+Read32(line+4);
        if(sStudyData[line+1]==0)LegacyLanguage();
        for(const char* p=w.at;*p;++p)if(*p=='{'&&LegacyToken(p)){w.lines[w.index].expanded=1;break;}
        if(!w.lines[w.index].expanded)continue;
        w.lines[w.index].offset=w.out;w.width=0;LegacyExpand();
        for(unsigned p=w.lines[w.index].offset;p+1<w.out;++p)if(sNativeRenderText[p]=='\n')w.total+=14;
    }
    return sLegacyTextHeight=w.height+w.total;
}
[[gnu::target("arm"),gnu::noinline]] static unsigned LegacyPrepare(const ReaderFrame& f)
{
    const u32 pg=Read32(36)+8*f.page;
    // A native composition shares these bytes and must be recomposed after
    // any static-page visit, including a page without a variable.
    sNativeTextCard=0;sLegacyTextCard=0;
    auto& w=LegacyWork();memset(&w,0,sizeof(w));w.page=f.page;w.count=Read16(pg+4);
    w.first=Read32(pg);w.refs=Read32(40)+Read32(24)*8;w.height=Read16(pg+6);
    if(w.count>64)return w.height; // Package preflight rejects variable pages above this bound.
    if(gKotoTextRecords)for(unsigned i=0;i<sNativeCount;++i){const auto& route=sNativeRoute[i];
        if(route.owner==f.card&&f.page>=route.page&&f.page<route.page+route.count&&route.record<gKotoTextCapacity){
            const auto& r=gKotoTextRecords[route.record];w.origin=r.origin?r.origin:r.source;w.context=r.context;break;}}
    sLegacyTextCard=f.card;sLegacyTextPage=f.page;return LegacyBuild();
}
[[gnu::always_inline]] static inline unsigned LegacyShift(const ReaderFrame& f,unsigned line)
{return sLegacyTextCard==f.card&&sLegacyTextPage==f.page&&line<64?LegacyWork().lines[line].shift:0;}
[[gnu::always_inline]] static inline const char* LegacyText(const ReaderFrame& f,unsigned line,const char* original)
{return sLegacyTextCard==f.card&&sLegacyTextPage==f.page&&line<64&&LegacyWork().lines[line].expanded?sNativeRenderText+LegacyWork().lines[line].offset:original;}
