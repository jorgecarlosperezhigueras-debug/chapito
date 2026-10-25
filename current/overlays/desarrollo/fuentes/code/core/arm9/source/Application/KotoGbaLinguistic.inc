// R29 KL29 sidecar. Startup is the only FatFs caller. Runtime work is a
// resumable state machine using one aligned document and fs_tryRead/pollRead.
// No heap, synchronous wait, ROM-cache replacement or guest-memory access.
#include "KotoGbaLinguisticApi.h"

struct LinguisticState {
    u32 size,count,noteTotal,buckets,addressBase,dataBase,database,documentLimit;
    u32 wanted,failed,loadingStart,loadingEnd,documentSector,documentBytes;
    u32 readyStart,readyEnd,readyBytes,generation;
    u32 cachedSector,pendingSector,copied,verifyAt,verifyHash;
    u32 unitsAt,unitCount,notesAt,noteCount,stringsAt,rawBytes;
    u32 fragmentsAt,fragmentCount,spanishAt,spanishBytes,validationAt;
    u32 wordsAt,wordCount,flags;
    u32 inlineAt,inlineSpans,inlineCards,inlineDisplayBytes;
    u16 clusterSize,pins;
    u16 boundaryUnit,boundaryDisplay,boundaryRaw;
    u8 device,job,phase,readSectors;
};
static_assert(sizeof(LinguisticState)<=192);
[[gnu::section(".ewram.bss")]] static LinguisticState sLinguistic;
[[gnu::section(".ewram.bss")]] static u32 sLinguisticRoots[352];
[[gnu::section(".ewram.bss")]] static DWORD sLinguisticClusters[2048];
[[gnu::section(".ewram.bss")]] alignas(32) static u8 sLinguisticIndex[512];
[[gnu::section(".ewram.bss")]] alignas(32) static u8 sLinguisticDocument[LINGUISTIC_DOCUMENT_BYTES];
[[gnu::section(".ewram.bss")]] static u16 sLinguisticNoteLines[LINGUISTIC_MAX_NOTES];
[[gnu::section(".ewram.bss")]] static FsWaitToken sLinguisticToken;
[[gnu::section(".ewram.bss")]] volatile u32 gLinguisticLoadStatus,gLinguisticReads,gLinguisticMisses;

static unsigned Linguistic16(const u8* p){return p[0]|(p[1]<<8);}
static u32 Linguistic32(const u8* p){return p[0]|(p[1]<<8)|(p[2]<<16)|((u32)p[3]<<24);}
static const char* LinguisticString(unsigned offset)
{return offset?(const char*)sLinguisticDocument+offset:"";}
static bool LinguisticRange(u32 at,u32 bytes,u32 limit)
{return at<=limit&&bytes<=limit-at;}
static bool LinguisticAvailable(){return gLinguisticLoadStatus==10;}
static bool LinguisticReady(u32 source)
{return LinguisticAvailable()&&sLinguistic.readyBytes&&source>=sLinguistic.readyStart&&source<sLinguistic.readyEnd;}
// A changed visible source deserves the same bounded work window as a scroll.
// Pinned documents cannot be replaced, so they do not request extra idle work.
static bool LinguisticReaderPending()
{return LinguisticAvailable()&&sLinguistic.wanted&&sLinguistic.failed!=sLinguistic.wanted&&
    !sLinguistic.pins&&!LinguisticReady(sLinguistic.wanted);}
static u32 LinguisticGeneration(){return sLinguistic.generation;}
static u32 LinguisticSourceOffset(){return sLinguistic.readyBytes?sLinguistic.readyStart:0;}
static u32 LinguisticPublicationMask()
{
    if(!sLinguistic.readyBytes)return 0;
    const unsigned n=sLinguistic.fragmentCount;
    return n==32?0xffffffffu:n?(1u<<n)-1:1;
}
static void LinguisticRequest(u32 source)
{
    if(source>=0x02000000u)return;
    if(sLinguistic.wanted!=source)sLinguistic.failed=0xffffffffu;
    sLinguistic.wanted=source;
}
static bool LinguisticPin(u32 source)
{
    if(!LinguisticReady(source)||sLinguistic.pins==65535)return false;
    ++sLinguistic.pins;return true;
}
static void LinguisticUnpin(){if(sLinguistic.pins)--sLinguistic.pins;}
static const u8* LinguisticRaw(unsigned& bytes)
{bytes=sLinguistic.readyBytes?sLinguistic.rawBytes:0;return bytes?sLinguisticDocument+96:nullptr;}
static unsigned LinguisticUnitCount(){return sLinguistic.readyBytes?sLinguistic.unitCount:0;}
static bool LinguisticUnitAt(unsigned index,LinguisticUnit& unit)
{
    if(!sLinguistic.readyBytes||index>=sLinguistic.unitCount)return false;
    const u8* p=sLinguisticDocument+sLinguistic.unitsAt+index*12;
    unit.rawStart=Linguistic16(p);unit.rawEnd=Linguistic16(p+2);
    unit.display=LinguisticString(Linguistic16(p+4));
    unit.displayBytes=Linguistic16(p+6);
    unit.reading=LinguisticString(Linguistic16(p+8));
    unit.readingBytes=Linguistic16(p+10);return true;
}
static unsigned LinguisticNoteCount(){return sLinguistic.readyBytes?sLinguistic.noteCount:0;}
static bool LinguisticNoteAt(unsigned index,LinguisticNote& note)
{
    if(!sLinguistic.readyBytes||index>=sLinguistic.noteCount)return false;
    const u8* p=sLinguisticDocument+sLinguistic.notesAt+index*32;
    note.id=Linguistic32(p);note.rawStart=Linguistic16(p+4);note.rawEnd=Linguistic16(p+6);
    note.displayStart=Linguistic16(p+8);note.displayEnd=Linguistic16(p+10);
    note.body=LinguisticString(Linguistic16(p+12));note.bodyBytes=Linguistic16(p+14);
    note.anchor=LinguisticString(Linguistic16(p+16));note.anchorBytes=Linguistic16(p+18);
    note.requiredSources[0]=Linguistic32(p+20);note.requiredSources[1]=Linguistic32(p+24);
    note.flags=Linguistic16(p+28);note.bodyLines=sLinguisticNoteLines[index];return true;
}
static unsigned LinguisticFragmentCount(){return sLinguistic.readyBytes?sLinguistic.fragmentCount:0;}
static bool LinguisticFragmentAt(unsigned index,LinguisticFragment& fragment)
{
    if(!sLinguistic.readyBytes||index>=sLinguistic.fragmentCount)return false;
    const u8* p=sLinguisticDocument+sLinguistic.fragmentsAt+index*16;
    fragment.rawStart=Linguistic32(p);fragment.rawEnd=Linguistic32(p+4);
    fragment.spanish=LinguisticString(Linguistic32(p+8));
    fragment.spanishBytes=Linguistic32(p+12);return true;
}
static const char* LinguisticSpanish(unsigned& bytes)
{bytes=sLinguistic.readyBytes?sLinguistic.spanishBytes:0;return sLinguistic.readyBytes?LinguisticString(sLinguistic.spanishAt):nullptr;}
static unsigned LinguisticWordCount(){return sLinguistic.readyBytes?sLinguistic.wordCount:0;}
static bool LinguisticWordAt(unsigned index,LinguisticWord& word)
{
    if(!sLinguistic.readyBytes||index>=sLinguistic.wordCount)return false;
    const u8* p=sLinguisticDocument+sLinguistic.wordsAt+4+index*8;
    word.rawStart=Linguistic16(p);word.rawEnd=Linguistic16(p+2);
    word.card=Linguistic16(p+4);word.ref=Linguistic16(p+6);return true;
}
static bool LinguisticWordsComplete(){return sLinguistic.readyBytes&&(sLinguistic.flags&2);}
static unsigned LinguisticInlineSpanCount(){return sLinguistic.readyBytes?sLinguistic.inlineSpans:0;}
static unsigned LinguisticInlineCardCount(){return sLinguistic.readyBytes?sLinguistic.inlineCards:0;}
static bool LinguisticInlineSpanAt(unsigned index,LinguisticInlineSpan& span)
{
    if(index>=LinguisticInlineSpanCount())return false;
    const u8* p=sLinguisticDocument+sLinguistic.inlineAt+16+12*index;
    span={u16(Linguistic16(p)),u16(Linguistic16(p+2)),u16(Linguistic16(p+4)),
          u16(Linguistic16(p+6)),u16(Linguistic16(p+8)),u16(Linguistic16(p+10))};return true;
}
static bool LinguisticInlineCardAt(unsigned index,LinguisticInlineCard& card)
{
    if(index>=LinguisticInlineCardCount())return false;
    const u8* p=sLinguisticDocument+sLinguistic.inlineAt+16+12*sLinguistic.inlineSpans+24*index;
    card.identity=Linguistic32(p);card.surface=LinguisticString(Linguistic16(p+4));
    card.surfaceBytes=Linguistic16(p+6);card.reading=LinguisticString(Linguistic16(p+8));
    card.readingBytes=Linguistic16(p+10);card.meaning=LinguisticString(Linguistic16(p+12));
    card.meaningBytes=Linguistic16(p+14);card.explanation=LinguisticString(Linguistic16(p+16));
    card.explanationBytes=Linguistic16(p+18);card.type=Linguistic16(p+20);return true;
}

// Bind to the exact retained packages, not just filenames. This helper and the
// entire loader execute before the VM. Their FIL objects never reach an IRQ.
[[gnu::target("arm"),gnu::noinline]] static bool LinguisticBinding(const char* path,const char* magic,unsigned at,u32 expected)
{
    FIL f{};UINT got=0;if(f_open(&f,path,FA_READ)!=FR_OK)return false;
    const bool ok=f_read(&f,sLinguisticIndex,64,&got)==FR_OK&&got==64&&
        !memcmp(sLinguisticIndex,magic,8)&&Linguistic32(sLinguisticIndex+at)==expected;
    f_close(&f);return ok;
}
extern "C" [[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline,gnu::optimize("Os")]] void kotogba_loadLinguistic()
{
    gLinguisticLoadStatus=1;gLinguisticReads=gLinguisticMisses=0;
    memset(&sLinguistic,0,sizeof(sLinguistic));
    sLinguistic.cachedSector=sLinguistic.failed=0xffffffffu;
    if(!sStudyReady)return;
    FIL f{};UINT got=0;if(f_open(&f,"/_gba/kotogba/packages/BPRJ_01.kl29",FA_READ)!=FR_OK)return;
    auto& s=sLinguistic;s.size=f_size(&f);
    bool ok=s.size>=1024&&!(s.size&511)&&f_read(&f,sLinguisticIndex,512,&got)==FR_OK&&got==512;
    u32 roots=0,ks=0,kc=0,km=0;
    if(ok){const u8* h=sLinguisticIndex;
        s.count=Linguistic32(h+16);s.noteTotal=Linguistic32(h+20);s.buckets=Linguistic32(h+24);
        roots=Linguistic32(h+28);s.addressBase=Linguistic32(h+32);s.dataBase=Linguistic32(h+36);
        s.documentLimit=Linguistic32(h+40);ks=Linguistic32(h+44);kc=Linguistic32(h+48);km=Linguistic32(h+52);
        const u32 provenance=Linguistic32(h+56),provenanceBytes=Linguistic32(h+60);
        ok=!memcmp(h,"KOTOL029",8)&&Linguistic32(h+8)==1&&Linguistic32(h+12)==s.size&&
            s.count&&s.count<=11264&&s.noteTotal<=65535&&s.buckets==(s.count+31)/32&&s.buckets<=352&&
            roots==(s.buckets*4+511)/512&&s.addressBase==1+roots&&s.dataBase==s.addressBase+s.buckets&&
            s.dataBase<s.size/512&&(s.documentLimit==LINGUISTIC_DOCUMENT_BYTES||
                s.documentLimit==LINGUISTIC_R39_DOCUMENT_BYTES||
                s.documentLimit==LINGUISTIC_LEGACY_DOCUMENT_BYTES)&&ks==Read32(12)&&
            provenance>=s.dataBase&&provenance<s.size/512&&provenanceBytes<=s.size-provenance*512;
    }
    if(ok)ok=f_lseek(&f,512)==FR_OK&&f_read(&f,sLinguisticRoots,s.buckets*4,&got)==FR_OK&&got==s.buckets*4;
    for(unsigned i=1;ok&&i<s.buckets;++i)ok=sLinguisticRoots[i-1]<sLinguisticRoots[i];
    if(!ok){f_close(&f);gLinguisticLoadStatus=2;return;}
    // Resolve the larger bounded fragmentation map once, before the VM starts.
    sLinguisticClusters[0]=sizeof(sLinguisticClusters)/sizeof(*sLinguisticClusters);f.cltbl=sLinguisticClusters;
    ok=f_lseek(&f,CREATE_LINKMAP)==FR_OK;
    if(f.obj.fs){s.device=f.obj.fs->pdrv;s.database=f.obj.fs->database;s.clusterSize=f.obj.fs->csize;}else ok=false;
    ok=ok&&s.device<=1&&s.clusterSize&&!(s.clusterSize&(s.clusterSize-1));
    f_close(&f);if(!ok){gLinguisticLoadStatus=4;return;}
    if(!LinguisticBinding("/_gba/kotogba/packages/BPRJ_01.kc15","KOTOC015",48,kc)||
       !LinguisticBinding("/_gba/kotogba/packages/BPRJ_01.km7","KOTOLM7",28,km)){
        gLinguisticLoadStatus=3;return;
    }
    s.cachedSector=0xffffffffu;gLinguisticLoadStatus=10;
}

// Read up to four contiguous document sectors per accepted request. The index
// keeps its single-sector buffer. The burst stays within its FAT cluster and
// the rounded document; submission never blocks or aliases the shared token.
// readSectors reuses the former state padding byte, with no new allocation.
[[gnu::target("thumb"),gnu::noinline]] static bool LinguisticReadSector(u32 logical,void* destination,u8 job)
{
    auto& s=sLinguistic;if(s.job||logical>=s.size/512)return false;
    u32 cluster=logical/s.clusterSize;const unsigned within=logical&(s.clusterSize-1);
    unsigned count=1;
    if(job==2){
        const unsigned rounded=(s.documentBytes+511)&~511u;
        if(s.copied>=rounded)return false;
        count=(rounded-s.copied)/512;if(count>4)count=4;
        if(count>s.clusterSize-within)count=s.clusterSize-within;
    }
    if(count>s.size/512-logical){gLinguisticLoadStatus=14;return false;}
    const DWORD* p=sLinguisticClusters+1;const DWORD* end=sLinguisticClusters+sizeof(sLinguisticClusters)/sizeof(*sLinguisticClusters)-1;
    while(p<end&&*p&&cluster>=*p){cluster-=*p;p+=2;}
    if(p>=end||!*p){gLinguisticLoadStatus=14;return false;}
    const u32 sector=s.database+(p[1]+cluster-2)*s.clusterSize+within;
    if(!fs_tryRead(s.device==0?FS_DEVICE_DLDI:FS_DEVICE_DSI_SD,destination,sector,count,&sLinguisticToken))return false;
    s.pendingSector=logical;s.readSectors=count;s.job=job;++gLinguisticReads;return true;
}
static bool LinguisticText(u32 offset,u32 bytes)
{
    const auto& s=sLinguistic;
    if(!offset&&!bytes)return true;
    return offset>=s.stringsAt&&LinguisticRange(offset,bytes+1,s.documentBytes)&&
        !sLinguisticDocument[offset+bytes]&&!memchr(sLinguisticDocument+offset,0,bytes);
}
static bool LinguisticDisplayBoundary(unsigned offset)
{
    auto& s=sLinguistic;if(offset==s.inlineDisplayBytes)return true;
    // Spans normally advance through the document. Reuse only a checked
    // unit boundary; overlapping/backward spans restart the exact same walk.
    if(offset<s.boundaryDisplay){s.boundaryUnit=0;s.boundaryDisplay=0;}
    while(s.boundaryUnit<s.unitCount){
        const u8* unit=sLinguisticDocument+s.unitsAt+12*s.boundaryUnit;
        const unsigned bytes=Linguistic16(unit+6);
        if(offset-s.boundaryDisplay<bytes){const unsigned at=Linguistic16(unit+4)+offset-s.boundaryDisplay;
            return (sLinguisticDocument[at]&0xc0)!=0x80;}
        s.boundaryDisplay+=bytes;++s.boundaryUnit;
    }
    return false;
}
static bool LinguisticRawBoundary(unsigned offset)
{
    auto& s=sLinguistic;const u8* raw=sLinguisticDocument+96;
    unsigned at=offset<s.boundaryRaw?0:s.boundaryRaw;
    while(at<offset){const u8 c=raw[at];
        const unsigned bytes=c==0xfc?(at+1<sLinguistic.rawBytes?2+NativeControlSize(raw[at+1]):0):
            c==0xf7||c==0xf8||c==0xf9||c==0xfd?2:1;
        if(!bytes||bytes>sLinguistic.rawBytes-at)return false;at+=bytes;}
    if(at!=offset)return false;s.boundaryRaw=at;return true;
}
static bool LinguisticLoadTarget()
{
    auto& s=sLinguistic;unsigned lo=0,hi=s.buckets;
    while(lo<hi){const unsigned m=(lo+hi)/2;if(sLinguisticRoots[m]<=s.wanted)lo=m+1;else hi=m;}
    if(!lo){s.failed=s.wanted;++gLinguisticMisses;return false;}
    const unsigned bucket=lo-1,sector=s.addressBase+bucket;
    if(s.cachedSector!=sector){LinguisticReadSector(sector,sLinguisticIndex,1);return false;}
    const unsigned count=s.count-bucket*32<32?s.count-bucket*32:32;
    for(unsigned i=0;i<count;++i){const u8* e=sLinguisticIndex+i*16;
        const u32 a=Linguistic32(e),b=Linguistic32(e+4),doc=Linguistic32(e+8),bytes=Linguistic32(e+12);
        if(!a||a>=b||b-a>512||doc<s.dataBase||doc>=s.size/512||!bytes||bytes>s.documentLimit||
           (bytes+511)/512>s.size/512-doc||(!i&&a!=sLinguisticRoots[bucket])||
           (i&&a<=Linguistic32(e-16))){gLinguisticLoadStatus=14;return false;}
        if(a<=s.wanted&&s.wanted<b){
            // Index I/O may run while a document is pinned; document DMA may
            // begin only after every renderer/modal has released its pointer.
            if(s.pins)return false;
            s.loadingStart=a;s.loadingEnd=b;s.documentSector=doc;s.documentBytes=bytes;
            s.copied=0;s.readyBytes=0;s.phase=1;return true;
        }
    }
    s.failed=s.wanted;++gLinguisticMisses;return false;
}
static bool LinguisticDocumentHeader()
{
    auto& s=sLinguistic;const u8* h=sLinguisticDocument;
    if(s.documentBytes<96||memcmp(h,"KOTOD029",8)||Linguistic32(h+8)!=s.loadingStart||Linguistic32(h+12)!=s.documentBytes)return false;
    s.unitsAt=Linguistic32(h+20);s.unitCount=Linguistic32(h+24);s.notesAt=Linguistic32(h+28);s.noteCount=Linguistic32(h+32);
    s.stringsAt=Linguistic32(h+36);s.rawBytes=Linguistic32(h+40);s.fragmentsAt=Linguistic32(h+44);s.fragmentCount=Linguistic32(h+48);
    s.spanishAt=Linguistic32(h+52);s.spanishBytes=Linguistic32(h+56);
    s.flags=Linguistic32(h+60);s.wordsAt=(s.flags>>8)&65535;s.wordCount=0;
    if(!s.rawBytes||s.rawBytes>512||s.rawBytes!=s.loadingEnd-s.loadingStart||
       s.unitCount>LINGUISTIC_MAX_UNITS||s.noteCount>LINGUISTIC_MAX_NOTES||s.fragmentCount>32||
       s.unitsAt<96+s.rawBytes||!LinguisticRange(s.unitsAt,s.unitCount*12,s.documentBytes)||
       s.notesAt<s.unitsAt+s.unitCount*12||!LinguisticRange(s.notesAt,s.noteCount*32,s.documentBytes)||
       s.stringsAt<s.notesAt+s.noteCount*32||s.stringsAt>s.documentBytes||
       (s.fragmentCount&&(!LinguisticRange(s.fragmentsAt,s.fragmentCount*16,s.stringsAt)||s.fragmentsAt<s.notesAt+s.noteCount*32))||
       (s.flags&0xff0000f8u)||!LinguisticText(s.spanishAt,s.spanishBytes))return false;
    s.inlineAt=s.inlineSpans=s.inlineCards=s.inlineDisplayBytes=0;
    if(s.flags&1){
        const u32 previous=s.fragmentCount?s.fragmentsAt+s.fragmentCount*16:s.notesAt+s.noteCount*32;
        if(s.wordsAt<previous||!LinguisticRange(s.wordsAt,4,s.stringsAt))return false;
        const u8* p=sLinguisticDocument+s.wordsAt;s.wordCount=Linguistic16(p);
        if(Linguistic16(p+2)||s.wordCount>256||!LinguisticRange(s.wordsAt+4,s.wordCount*8,s.stringsAt))return false;
    }else if(s.wordsAt)return false;
    if(s.flags&4){
        if(!(s.flags&1))return false;
        s.inlineAt=s.wordsAt+4+s.wordCount*8;
        if(!LinguisticRange(s.inlineAt,16,s.stringsAt))return false;
        const u8* p=sLinguisticDocument+s.inlineAt;
        s.inlineSpans=Linguistic16(p+8);s.inlineCards=Linguistic16(p+10);
        if(memcmp(p,"KOTOV032",8)||!s.inlineSpans||s.inlineSpans>512||!s.inlineCards||s.inlineCards>256||
           Linguistic16(p+12)||Linguistic16(p+14)||
           !LinguisticRange(s.inlineAt+16,s.inlineSpans*12+s.inlineCards*24,s.stringsAt))return false;
    }
    memset(sLinguisticNoteLines,0,sizeof(sLinguisticNoteLines));s.verifyAt=64;s.verifyHash=2166136261u;return true;
}
// Strict UTF8 checks consume one codepoint and bounded look-ahead. Pool NULs
// are valid separators; embedded NULs in a declared string are rejected above.
static unsigned LinguisticUtf8(unsigned at)
{
    const auto& s=sLinguistic;const u8* p=sLinguisticDocument+at;const unsigned c=p[0];
    if(c<128)return 1;
    const unsigned n=c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
    if(!n||!LinguisticRange(at,n,s.documentBytes))return 0;
    for(unsigned i=1;i<n;++i)if((p[i]&0xc0)!=0x80)return 0;
    if((c==0xe0&&p[1]<0xa0)||(c==0xed&&p[1]>=0xa0)||(c==0xf0&&p[1]<0x90)||(c==0xf4&&p[1]>=0x90))return 0;
    return n;
}
[[gnu::target("thumb"),gnu::noinline,gnu::optimize("Os")]] static void LinguisticSlice(unsigned start)
{
    auto& s=sLinguistic;if(!LinguisticAvailable()||SliceExpired(start))return;
    if(s.job){
        if(!fs_pollRead(&sLinguisticToken))return;
        const unsigned job=s.job;s.job=0;
        if(job==1)s.cachedSector=s.pendingSector;
        else s.copied=s.copied+s.readSectors*512;
    }
    // A changed request may abandon a completed/staged read, never an active
    // DMA. No staged bytes become visible before checksum and all validation.
    if(s.phase&&!(s.wanted>=s.loadingStart&&s.wanted<s.loadingEnd))s.phase=0;
    if(!s.wanted||s.failed==s.wanted||LinguisticReady(s.wanted))return;
    if(!s.phase&&!LinguisticLoadTarget())return;
    if(s.phase==1){
        if(s.pins)return;
        const unsigned rounded=(s.documentBytes+511)&~511u;
        if(s.copied<rounded){LinguisticReadSector(s.documentSector+s.copied/512,sLinguisticDocument+s.copied,2);return;}
        if(!LinguisticDocumentHeader()){gLinguisticLoadStatus=14;return;}
        s.phase=2;
    }
    if(s.phase==2){
        // The shared scanline/audio deadline is the work limit. An arbitrary
        // 512-byte cap forced another frame even with most of VBlank unused.
        for(unsigned work=0;s.verifyAt<s.documentBytes;++work){
            if(!(work&31)&&SliceExpired(start))return;
            s.verifyHash=(s.verifyHash^sLinguisticDocument[s.verifyAt++])*16777619u;
        }
        if(s.verifyAt<s.documentBytes)return;
        if(s.verifyHash!=Linguistic32(sLinguisticDocument+16)){gLinguisticLoadStatus=14;return;}
        s.validationAt=0;s.phase=3;
    }
    if(s.phase==3){
        while(s.validationAt<s.unitCount){
            if(SliceExpired(start))return;
            const unsigned i=s.validationAt++;const u8* p=sLinguisticDocument+s.unitsAt+12*i;
            const unsigned a=Linguistic16(p),b=Linguistic16(p+2);
            const unsigned previous=i?Linguistic16(p-10):0;
            if(a>=b||b>s.rawBytes||a<previous||!LinguisticText(Linguistic16(p+4),Linguistic16(p+6))||
               !LinguisticText(Linguistic16(p+8),Linguistic16(p+10))){gLinguisticLoadStatus=14;return;}
            s.inlineDisplayBytes+=Linguistic16(p+6);
            if(s.inlineDisplayBytes>65535){gLinguisticLoadStatus=14;return;}
        }
        s.validationAt=0;s.phase=4;
    }
    if(s.phase==4){
        while(s.validationAt<s.noteCount){
            if(SliceExpired(start))return;
            const unsigned i=s.validationAt++;const u8* p=sLinguisticDocument+s.notesAt+32*i;
            const unsigned flags=Linguistic16(p+28),a=Linguistic16(p+4),b=Linguistic16(p+6);
            if(Linguistic32(p)>=s.noteTotal||a>=b||b>s.rawBytes||Linguistic16(p+8)>=Linguistic16(p+10)||
               !Linguistic16(p+14)||!LinguisticText(Linguistic16(p+12),Linguistic16(p+14))||
               !LinguisticText(Linguistic16(p+16),Linguistic16(p+18))||Linguistic16(p+30)||(flags&~7u)||
               !(flags&LINGUISTIC_SOURCE_COMPLETE_REQUIRED)||Linguistic32(p+20)>=0x02000000u||Linguistic32(p+24)>=0x02000000u||
               bool(flags&LINGUISTIC_DEPENDENCIES_REQUIRED)!=bool(Linguistic32(p+20)||Linguistic32(p+24))){gLinguisticLoadStatus=14;return;}
            sLinguisticNoteLines[i]=1;
        }
        s.validationAt=0;s.phase=5;
    }
    if(s.phase==5){
        // Word records are small and bounded; validate at most 32 per slice.
        if(s.validationAt<s.wordCount){
            const unsigned end=s.validationAt+32<s.wordCount?s.validationAt+32:s.wordCount;
            while(s.validationAt<end){
                if(SliceExpired(start))return;
                const u8* p=sLinguisticDocument+s.wordsAt+4+8*s.validationAt++;
                if(Linguistic16(p)>=Linguistic16(p+2)||Linguistic16(p+2)>s.rawBytes||
                   !CorpusValidWord(Linguistic16(p+4),Linguistic16(p+6))){gLinguisticLoadStatus=14;return;}
            }
            if(s.validationAt<s.wordCount)return;
        }
        s.validationAt=0;s.boundaryUnit=s.boundaryDisplay=s.boundaryRaw=0;s.phase=8;
    }
    if(s.phase==8){
        // Optional inline spans never reinterpret controls or guess a display
        // ratio. The generator supplies both exact raw and UTF8 display ranges.
        while(s.validationAt<s.inlineSpans){
            if(SliceExpired(start))return;
            const u8* p=sLinguisticDocument+s.inlineAt+16+12*s.validationAt++;
            if(Linguistic16(p)>=Linguistic16(p+2)||Linguistic16(p+2)>s.rawBytes||
               Linguistic16(p+4)>=Linguistic16(p+6)||Linguistic16(p+6)>s.inlineDisplayBytes||
               Linguistic16(p+8)>=s.inlineCards||(Linguistic16(p+10)&~3u)||
               !LinguisticDisplayBoundary(Linguistic16(p+4))||!LinguisticDisplayBoundary(Linguistic16(p+6))||
               !LinguisticRawBoundary(Linguistic16(p))||!LinguisticRawBoundary(Linguistic16(p+2))){
                gLinguisticLoadStatus=14;return;}
        }
        s.validationAt=0;s.phase=9;
    }
    if(s.phase==9){
        while(s.validationAt<s.inlineCards){
            if(SliceExpired(start))return;
            const u8* p=sLinguisticDocument+s.inlineAt+16+s.inlineSpans*12+24*s.validationAt++;
            if(!Linguistic32(p)||!Linguistic16(p+6)||!Linguistic16(p+14)||Linguistic16(p+20)>4||Linguistic16(p+22)){
                gLinguisticLoadStatus=14;return;}
            for(unsigned k=4;k<20;k+=4)if(!LinguisticText(Linguistic16(p+k),Linguistic16(p+k+2))){
                gLinguisticLoadStatus=14;return;}
        }
        s.validationAt=0;s.phase=7;
    }
    if(s.phase==7){
        while(s.validationAt<s.fragmentCount){
            if(SliceExpired(start))return;
            const unsigned i=s.validationAt++;const u8* p=sLinguisticDocument+s.fragmentsAt+i*16;
            const u32 a=Linguistic32(p),b=Linguistic32(p+4);
            if(a>b||b>s.rawBytes||(i&&a<=Linguistic32(p-12))||
               Linguistic32(p+12)>65535||!LinguisticText(Linguistic32(p+8),Linguistic32(p+12))){gLinguisticLoadStatus=14;return;}
        }
        s.validationAt=s.stringsAt;s.phase=6;
    }
    if(s.phase==6){
        // Keep every UTF8/range/line check, but use the real deadline instead
        // of forcing a fresh frame for every128 string-pool codepoints.
        while(s.validationAt<s.documentBytes){
            if(SliceExpired(start))return;
            const unsigned at=s.validationAt,n=LinguisticUtf8(at);if(!n){gLinguisticLoadStatus=14;return;}
            if(sLinguisticDocument[at]=='\n')for(unsigned i=0;i<s.noteCount;++i){const u8* p=sLinguisticDocument+s.notesAt+i*32;
                const unsigned a=Linguistic16(p+12),b=a+Linguistic16(p+14);if(at>=a&&at<b)++sLinguisticNoteLines[i];}
            s.validationAt+=n;
        }
        if(s.validationAt<s.documentBytes)return;
        s.readyStart=s.loadingStart;s.readyEnd=s.loadingEnd;s.readyBytes=s.documentBytes;
        ++s.generation;s.phase=0;
        // Caller compares generation and schedules repaint. Never call UI from
        // the loader; this makes publication independently testable.
    }
}
