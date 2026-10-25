// Shared asynchronous entity-name resolution; the game adapter defines roles.
#include "KotoGbaEntityAdapterBprj1.h"

// role's upper bits hold pending/ready/miss; the reclaimed u16 keeps exact
// source identity, so equal spellings cannot share a different contextual ES.
struct CorpusEntityValue {u8 raw[16];char text[40];u16 proof;u8 size,role;};
struct CorpusEntityWork {
    CorpusEntityValue values[5];
    u32 hash,index,source,sector;
    u8 stage,slot,cursor,pad;
};
static_assert(sizeof(CorpusEntityValue)==60&&sizeof(CorpusEntityWork)==320);
// The released corpus uses 272/352 hash roots. Reuse only the unused tail,
// after validating its size. A larger future package simply keeps literal JP
// for unlocalized variables; it never overwrites an index root or allocates.
[[gnu::always_inline]] static inline CorpusEntityWork* CorpusEntities()
{return sCorpusHashBuckets<=352-sizeof(CorpusEntityWork)/4?(CorpusEntityWork*)(sCorpusHashRoots+sCorpusHashBuckets):nullptr;}
static void CorpusEntityReset()
{if(auto* w=CorpusEntities())memset(w,0,sizeof(*w));}
static bool CorpusEntityPending()
{auto* w=CorpusEntities();return w&&w->stage;}
static void CorpusEntityMiss(CorpusEntityWork& w)
{w.values[w.slot].role=(w.values[w.slot].role&15)|32;w.stage=0;}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static const char* CorpusEntityText(u32 role,unsigned token,u32 pointer,u32 proven)
{
    auto* w=CorpusEntities();
    if(!role||!w||!pointer||gCorpusLoadStatus!=10)return nullptr;
    // New battle roles require construction provenance. An arbitrary nickname
    // equal to a move/ability never becomes a translated dictionary entity.
    if((role!=1||token==0x16)&&!proven)return nullptr;
    if(proven&&!CorpusEntitySource(role,proven-0x08000000))return nullptr;
    // Source/context binding is checked by variableSource; this pointer is its
    // frozen native snapshot. Controls and unterminated/long values stay JP.
    const u8* raw=(const u8*)pointer;unsigned size=0;
    while(size<16){const u8 c=raw[size++];if(c==0xff)break;if(c>=0xf7)return nullptr;}
    if(!size||raw[size-1]!=0xff)return nullptr;
    // At most sixteen frozen bytes, including EOS. Keep this shallow bounded
    // comparison/copy in the entity frame instead of nesting libc/Hash frames
    // below the legacy-page renderer on its fixed IRQ stack.
    const volatile u8* frozen=raw;
    const u16 proof=KotoEntityProof(proven);
    for(const auto& v:w->values)if((v.role>>4)&&(v.role&15)==role&&v.size==size&&v.proof==proof){
        unsigned same=0;while(same<size&&v.raw[same]==frozen[same])++same;
        if(same==size)return (v.role>>4)==1?v.text:proven?"…":nullptr;
    }
    if(w->stage)return proven?"…":nullptr;
    w->slot=w->cursor;w->cursor=w->cursor==4?0:w->cursor+1;auto& v=w->values[w->slot];v.role=role;v.size=size;v.proof=proof;
    u32 hash=2166136261u;for(unsigned i=0;i<size;++i){const u8 ch=frozen[i];v.raw[i]=ch;hash=(hash^ch)*16777619u;}
    w->hash=hash;w->index=0xffffffff;w->source=proven?proven-0x08000000:0;w->stage=1;
    return proven?"…":nullptr;
}
// Called only by the shallow sector service. No observer, filesystem operation,
// allocation or ROM cache replacement is added.
[[gnu::target("thumb"),gnu::noinline,gnu::optimize("Os")]] static void CorpusEntitySlice()
{
    if(sRender.busy)return;
    auto* work=CorpusEntities();if(!work)return;auto& w=*work;auto& value=w.values[w.slot];
    if(w.stage==1){
        if(!w.source&&!sCorpusHashBuckets){CorpusEntityMiss(w);return;}
        const unsigned bucket=w.source?CorpusBucket(w.source,sCorpusRoots,sCorpusBuckets):CorpusBucket(w.hash,sCorpusHashRoots,sCorpusHashBuckets),sector=(w.source?sCorpusAddressBase:sCorpusHashBase)+bucket;
        if(sCorpusCachedSector!=sector){CorpusRequest(sector,1,sCorpusIndex,1);return;}
        const unsigned count=w.source?sCorpusCount:sCorpusHashCount,end=count-bucket*32<32?count-bucket*32:32;
        for(unsigned i=0;i<end;++i){const u8* p=sCorpusIndex+i*16;
            if(w.source?Corpus32(p)==w.source:Corpus32(p)==w.hash&&Corpus32(p+8)==value.size){w.index=w.source?bucket*32+i:Corpus32(p+4);break;}}
        if(w.index>=sCorpusCount){CorpusEntityMiss(w);return;}w.stage=2;
    }
    if(w.stage==2){
        const unsigned sector=sCorpusAddressBase+w.index/32;
        if(sCorpusCachedSector!=sector){CorpusRequest(sector,1,sCorpusIndex,1);return;}
        const auto& e=*(const CorpusEntry*)(sCorpusIndex+16*(w.index%32));
        if(!CorpusEntitySource(value.role,e.start)||e.end<=e.start||e.end-e.start!=value.size||e.document<sCorpusDataBase||e.document*512>=sCorpusSize){CorpusEntityMiss(w);return;}
        w.source=e.start;w.sector=e.document;w.stage=3;
    }
    if(w.stage==3&&CorpusRequest(w.sector,1,sCorpusDocument,3)){
        // Parent text is already copied into render scratch. When the name
        // arrives, normal scene rebuilding reloads its document on demand.
        sCorpusDocIndex=sCorpusWantedIndex=0xffffffff;sCorpusCopied=sCorpusDocBytes=0;w.stage=4;
    }
}
[[gnu::target("thumb"),gnu::noinline,gnu::optimize("Os")]] static void CorpusEntityFinish()
{
    auto* work=CorpusEntities();if(!work||work->stage!=4)return;auto& w=*work;auto& v=w.values[w.slot];
    const unsigned raw=Corpus32(sCorpusDocument+16),es=Corpus32(sCorpusDocument+20),bytes=Corpus32(sCorpusDocument+28);
    const u8* body=sCorpusDocument+32;
    const unsigned flags=Corpus32(sCorpusDocument+24),base=raw+es+1;
    // A name may retain approved lexical links after its ES text (Poké Ball).
    // No fragments, variables or display wrapping are accepted for a name.
    const bool valid=!memcmp(sCorpusDocument,"KOTODO15",8)&&Corpus32(sCorpusDocument+12)==w.source&&raw==v.size&&
        es&&es<sizeof(v.text)&&!(flags&~4u)&&bytes<=480&&
        (flags&4?bytes>=base+2:bytes==base)&&
        !body[raw+es]&&!memcmp(body,v.raw,raw)&&CorpusHash(body,bytes)==Corpus32(sCorpusDocument+8);
    if(valid){memcpy(v.text,body+raw,es+1);v.role|=16;w.stage=0;}
    else CorpusEntityMiss(w);
    sCorpusDocIndex=sCorpusWantedIndex=0xffffffff;sCorpusCopied=sCorpusDocBytes=0;++sCorpusGeneration;SceneChanged();
}
