// Bounded bilingual corpus. FatFs/link-map validation runs only before the VM.
// Runtime reads use the existing nonblocking ARM7 sector service and never
// allocate, replace ROM cache blocks, or call FatFs inside an observation/IRQ.
struct CorpusEntry {u32 start,end,document;u16 owner,page;};
struct CorpusRef {u32 key,sequence,index;CorpusEntry entry;u8 ready,hash,pad[2];u32 seen;};
static_assert(sizeof(CorpusRef)==4*KOTO_TEXT_FRAGMENT_MASK_STRIDE);
[[gnu::section(".ewram.bss")]] static u32 sCorpusRoots[352],sCorpusHashRoots[352];
[[gnu::section(".ewram.bss")]] static DWORD sCorpusClusters[1024];
[[gnu::section(".ewram.bss")]] alignas(32) static u8 sCorpusIndex[512],sCorpusDocument[1536];
[[gnu::section(".ewram.bss")]] static CorpusRef sCorpusRefs[KOTO_TEXT_TOTAL_SLOTS];
[[gnu::section(".ewram.bss")]] static FsWaitToken sCorpusToken;
[[gnu::section(".ewram.bss")]] volatile u32 gCorpusLoadStatus,gCorpusReads,gCorpusHits,gCorpusMisses;
[[gnu::section(".ewram.bss")]] static u32 sCorpusSize,sCorpusCount,sCorpusHashCount,sCorpusBuckets,sCorpusHashBuckets,sCorpusAddressBase,sCorpusHashBase,sCorpusDataBase,sCorpusDatabase;
[[gnu::section(".ewram.bss")]] static u32 sCorpusCachedSector,sCorpusPendingSector,sCorpusDocIndex,sCorpusWantedIndex,sCorpusDocSector,sCorpusDocBytes,sCorpusCopied;
[[gnu::section(".ewram.bss")]] static u16 sCorpusClusterSize,sCorpusReadBytes;
[[gnu::section(".ewram.bss")]] static u8 sCorpusDevice,sCorpusJob;
[[gnu::section(".ewram.bss")]] static u32 sCorpusGeneration;
[[gnu::section(".ewram.bss")]] static u32 sCorpusRequestedSector;
[[gnu::section(".ewram.bss")]] static u32 sCorpusExpectedSource;
[[gnu::section(".ewram.bss")]] static u32 sCorpusVerifyHash,sCorpusVerifyPos;
[[gnu::section(".ewram.bss")]] static bool sCorpusVerifying;
static u32 CorpusGeneration(){return sCorpusGeneration;}
static void CorpusEntityReset();
static bool CorpusEntityPending();
static void CorpusEntitySlice();
static void CorpusEntityFinish();
static u32 Corpus32(const u8* p){return p[0]|(p[1]<<8)|(p[2]<<16)|((u32)p[3]<<24);}
static u32 CorpusHash(const u8* p,unsigned n){u32 h=2166136261u;for(unsigned i=0;i<n;++i)h=(h^p[i])*16777619u;return h;}
static bool CorpusValidWord(unsigned id,unsigned ref)
{
    const u8* card=FindStudy(id);if(!card||ref&0x8000)return false;
    const unsigned page=(ref>>9)&63,first=ref&511;
    return page<Card16(card,12)&&first<Read16(Read32(36)+8*(Card16(card,6)+page)+4);
}
static bool CorpusRequest(u32 logical,unsigned count,void* destination,u8 job)
{
    if(sCorpusJob||gCorpusLoadStatus!=10)return false;
    const unsigned within=logical&(sCorpusClusterSize-1);u32 cluster=logical/sCorpusClusterSize;
    const DWORD* t=sCorpusClusters+1;const DWORD* end=sCorpusClusters+sizeof(sCorpusClusters)/sizeof(*sCorpusClusters)-1;
    while(t<end&&*t&&cluster>=*t){cluster-=*t;t+=2;}
    if(t>=end||!*t){gCorpusLoadStatus=12;return false;}
    if(count>sCorpusClusterSize-within)count=sCorpusClusterSize-within;
    if((logical+count)*512>sCorpusSize){gCorpusLoadStatus=13;return false;}
    const u32 sector=sCorpusDatabase+(t[1]+cluster-2)*sCorpusClusterSize+within;
    if(!fs_tryRead(sCorpusDevice==0?FS_DEVICE_DLDI:FS_DEVICE_DSI_SD,destination,sector,count,&sCorpusToken))return false;
    sCorpusPendingSector=logical;sCorpusReadBytes=count*512;sCorpusJob=job;++gCorpusReads;return true;
}
extern "C" [[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]] void kotogba_loadCorpus()
{
    gCorpusLoadStatus=1;sCorpusJob=0;sCorpusVerifying=false;sCorpusCachedSector=sCorpusDocIndex=sCorpusWantedIndex=sCorpusRequestedSector=0xffffffff;
    memset(sCorpusRefs,0,sizeof(sCorpusRefs));gCorpusReads=gCorpusHits=gCorpusMisses=0;
    gKotoTextFragmentMasks=&sCorpusRefs[0].seen;
    if(!sStudyReady)return;
    FIL f{};UINT got=0;
    kotogba_bootStep(36,0);
    if(f_open(&f,"/_gba/kotogba/packages/BPRJ_01.kc15",FA_READ)!=FR_OK)return;
    kotogba_bootStep(37,0);
    sCorpusSize=f_size(&f);bool ok=sCorpusSize>=512&&!(sCorpusSize&511)&&f_read(&f,sCorpusIndex,512,&got)==FR_OK&&got==512;
    const u32* h=(const u32*)sCorpusIndex;
    if(!ok||memcmp(h,"KOTOC015",8)||h[2]!=1||h[3]!=sCorpusSize||h[11]!=Read32(12)){f_close(&f);gCorpusLoadStatus=2;return;}
    sCorpusCount=h[4];sCorpusHashCount=h[5];sCorpusBuckets=h[6];sCorpusHashBuckets=h[7];sCorpusAddressBase=h[8];sCorpusHashBase=h[9];sCorpusDataBase=h[10];const u32 rootSectors=h[13];
    ok=sCorpusCount&&sCorpusCount<=11264&&sCorpusBuckets==(sCorpusCount+31)/32&&sCorpusBuckets<=352&&sCorpusHashBuckets==(sCorpusHashCount+31)/32&&sCorpusHashBuckets<=352&&rootSectors==(sCorpusBuckets*4+511)/512&&sCorpusAddressBase==1+2*rootSectors&&sCorpusHashBase==sCorpusAddressBase+sCorpusBuckets&&sCorpusDataBase==sCorpusHashBase+sCorpusBuckets&&sCorpusDataBase*512<sCorpusSize;
    kotogba_bootStep(38,0);
    if(ok)ok=f_lseek(&f,512)==FR_OK&&f_read(&f,sCorpusRoots,sCorpusBuckets*4,&got)==FR_OK&&got==sCorpusBuckets*4;
    if(ok)ok=f_lseek(&f,(1+rootSectors)*512)==FR_OK&&f_read(&f,sCorpusHashRoots,sCorpusHashBuckets*4,&got)==FR_OK&&got==sCorpusHashBuckets*4;
    for(unsigned i=1;ok&&i<sCorpusBuckets;++i)ok=sCorpusRoots[i-1]<sCorpusRoots[i];
    for(unsigned i=1;ok&&i<sCorpusHashBuckets;++i)ok=sCorpusHashRoots[i-1]<=sCorpusHashRoots[i];
    kotogba_bootStep(39,0);
    if(ok){sCorpusClusters[0]=sizeof(sCorpusClusters)/sizeof(*sCorpusClusters);f.cltbl=sCorpusClusters;ok=f_lseek(&f,CREATE_LINKMAP)==FR_OK;
        if(f.obj.fs){sCorpusDevice=f.obj.fs->pdrv;sCorpusDatabase=f.obj.fs->database;sCorpusClusterSize=f.obj.fs->csize;}else ok=false;
        ok=ok&&sCorpusDevice<=1&&sCorpusClusterSize&&!(sCorpusClusterSize&(sCorpusClusterSize-1));}
    f_close(&f);gCorpusLoadStatus=ok?10:3;CorpusEntityReset();
}
[[gnu::target("thumb"),gnu::noinline,gnu::optimize("Os")]] static void CorpusSlice(unsigned start)
{
    if(gCorpusLoadStatus!=10||SliceExpired(start))return;
    if(sCorpusJob){
        if(!fs_pollRead(&sCorpusToken))return;const u8 job=sCorpusJob;sCorpusJob=0;
        if(job==1){sCorpusCachedSector=sCorpusPendingSector;++sCorpusGeneration;}
        else if(job==3)CorpusEntityFinish();
        else{
            sCorpusCopied+=sCorpusReadBytes;
            if(sCorpusCopied==512){
                const u32 bytes=Corpus32(sCorpusDocument+28);
                if(memcmp(sCorpusDocument,"KOTODO15",8)||bytes>sizeof(sCorpusDocument)-32){gCorpusLoadStatus=14;return;}
                sCorpusDocBytes=(32+bytes+511)&~511u;
            }
            if(sCorpusCopied>=sCorpusDocBytes){
                const u32 raw=Corpus32(sCorpusDocument+16),es=Corpus32(sCorpusDocument+20),bytes=Corpus32(sCorpusDocument+28);
                const u32 flags=Corpus32(sCorpusDocument+24),base=raw+es+1+2*((flags>>8)&255)+8*(flags>>16);
                if(memcmp(sCorpusDocument,"KOTODO15",8)||Corpus32(sCorpusDocument+12)!=sCorpusExpectedSource||raw>512||es>1024||base>bytes||bytes>sizeof(sCorpusDocument)-32||sCorpusDocument[32+raw+es]){
                    gCorpusLoadStatus=14;sCorpusDocIndex=0xffffffff;return;}
                unsigned tail=base;
                if(flags&2){
                    if(tail+2>bytes){gCorpusLoadStatus=14;return;}
                    const unsigned count=sCorpusDocument[32+tail]|(sCorpusDocument[33+tail]<<8);tail+=2;
                    if(!count||count>32||tail+6*count>bytes){gCorpusLoadStatus=14;return;}
                    unsigned previous=0,previousText=0;const bool mapped=(sCorpusDocument[32+tail+4]|(sCorpusDocument[32+tail+5]<<8))!=65535;
                    for(unsigned i=0;i<count;++i){const u8* p=sCorpusDocument+32+tail+6*i;const unsigned a=p[0]|(p[1]<<8),b=p[2]|(p[3]<<8),t=p[4]|(p[5]<<8);
                        if(a>b||a<previous||b>1024||mapped!=(t!=65535)||(mapped&&(t>=es||(!i&&t)||(i&&(t<=previousText||sCorpusDocument[32+raw+t-1]!='\n'))))){gCorpusLoadStatus=14;return;}previous=b+1;previousText=t;}
                    tail+=6*count;
                }
                if(flags&4){
                    if(tail+2>bytes){gCorpusLoadStatus=14;return;}
                    const unsigned count=sCorpusDocument[32+tail]|(sCorpusDocument[33+tail]<<8);tail+=2;
                    if(count>8){gCorpusLoadStatus=14;return;}
                    for(unsigned i=0;i<count;++i){if(tail+6>bytes){gCorpusLoadStatus=14;return;}const u8* p=sCorpusDocument+32+tail;const unsigned n=p[4]|(p[5]<<8);tail+=6+n;
                        if(!n||n>KOTO_TEXT_BYTES||tail>bytes||!CorpusValidWord(p[0]|(p[1]<<8),p[2]|(p[3]<<8))){gCorpusLoadStatus=14;return;}
                        for(unsigned k=0;k<n;++k)if(p[6+k]>=0xf7){gCorpusLoadStatus=14;return;}}
                }
                if(flags&8){
                    if(!(flags&2)||tail+2>bytes){gCorpusLoadStatus=14;return;}
                    const unsigned n=sCorpusDocument[32+tail]|(sCorpusDocument[33+tail]<<8);tail+=2;
                    if(!n||n>512||tail+n>bytes||sCorpusDocument[32+tail+n-1]!=0xff){gCorpusLoadStatus=14;return;}tail+=n;
                }
                if(flags&16){
                    if(!(flags&8)||tail+2>bytes){gCorpusLoadStatus=14;return;}
                    const unsigned n=sCorpusDocument[32+tail]|(sCorpusDocument[33+tail]<<8);tail+=2;
                    if(!n||n>512||tail+n+1>bytes||sCorpusDocument[32+tail+n]){gCorpusLoadStatus=14;return;}tail+=n+1;
                }
                if(flags&32)tail=CorpusValidateGrammar(tail,bytes);
                if(tail!=bytes){gCorpusLoadStatus=14;return;}
                sCorpusVerifyHash=2166136261u;sCorpusVerifyPos=0;sCorpusVerifying=true;
            }
        }
    }
    if(sCorpusVerifying){
        const unsigned bytes=Corpus32(sCorpusDocument+28);
        while(sCorpusVerifyPos<bytes){if(SliceExpired(start))return;
            const unsigned end=sCorpusVerifyPos+32<bytes?sCorpusVerifyPos+32:bytes;
            while(sCorpusVerifyPos<end)sCorpusVerifyHash=(sCorpusVerifyHash^sCorpusDocument[32+sCorpusVerifyPos++])*16777619u;
        }
        sCorpusVerifying=false;
        if(sCorpusVerifyHash!=Corpus32(sCorpusDocument+8)){gCorpusLoadStatus=14;sCorpusDocIndex=0xffffffff;return;}
        sCorpusDocIndex=sCorpusWantedIndex;++sCorpusGeneration;if(!sModalKind)SceneChanged();
    }
    // Name resolution shares the existing index/document transfer buffers.
    // It runs in this shallow service frame, never in the capture observer.
    if(!sCorpusJob&&!sCorpusVerifying&&CorpusEntityPending())return CorpusEntitySlice();
    if(!sCorpusJob&&sCorpusWantedIndex!=0xffffffff&&sCorpusDocIndex!=sCorpusWantedIndex&&sCorpusCopied<sCorpusDocBytes)
        CorpusRequest(sCorpusDocSector+sCorpusCopied/512,(sCorpusDocBytes-sCorpusCopied)/512,sCorpusDocument+sCorpusCopied,2);
    if(!sCorpusJob&&sCorpusWantedIndex==sCorpusDocIndex&&sCorpusRequestedSector!=0xffffffff)
        if(CorpusRequest(sCorpusRequestedSector,1,sCorpusIndex,1))sCorpusRequestedSector=0xffffffff;
}
static unsigned CorpusBucket(u32 key,const u32* roots,unsigned count)
{
    unsigned lo=0,hi=count;while(lo<hi){unsigned m=(lo+hi)/2;if(roots[m]<=key)lo=m+1;else hi=m;}return lo?lo-1:0;
}
static bool CorpusIndexSector(u32 sector)
{
    // ARM7 can replace the bytes before CorpusSlice observes completion.
    // The old sector tag is not an authority while its buffer is in flight.
    if(sCorpusJob==1)return false;
    if(sCorpusCachedSector==sector)return true;
    // Submit I/O from the shallow service frame, not the nested scene builder.
    // This keeps the established 288-byte IRQ stack within its guard.
    if(!sCorpusJob&&sCorpusRequestedSector==0xffffffff)sCorpusRequestedSector=sector;
    return false;
}
#include "KotoGbaEntityNames.inc"
[[gnu::noinline]] static bool CorpusResolve(unsigned slot,u32 source,u32 hash,unsigned size)
{
    if(gCorpusLoadStatus!=10||slot>=KOTO_TEXT_TOTAL_SLOTS)return false;auto& ref=sCorpusRefs[slot];
    const auto& r=gKotoTextRecords[slot];const bool byHash=!source||(ref.hash&&ref.sequence==r.sequence);const u32 key=byHash?hash:source-0x08000000;
    if(ref.key==key&&ref.hash==byHash&&ref.sequence==r.sequence){
        if(ref.ready){++gCorpusHits;return ref.ready==1;}
    }else{
        // Transfer the retained authority while the previous reference still
        // exists. No reader deadline check separates this from its replacement.
        CompositionResolveReference(slot,key,byHash);
        for(unsigned i=0;i<8;++i)((volatile u32*)&ref)[i]=0;
        ref.key=key;ref.hash=byHash;ref.sequence=r.sequence;ref.index=0xffffffff;
    }
    if(byHash&&ref.index==0xffffffff){
        if(!sCorpusHashBuckets)return false;
        const unsigned bucket=CorpusBucket(key,sCorpusHashRoots,sCorpusHashBuckets);
        if(!CorpusIndexSector(sCorpusHashBase+bucket))return false;
        const unsigned end=sCorpusHashCount-bucket*32<32?sCorpusHashCount-bucket*32:32;
        for(unsigned i=0;i<end;++i){const u8* p=sCorpusIndex+i*16;if(Corpus32(p)==key&&Corpus32(p+8)==size){ref.index=Corpus32(p+4);break;}}
        if(ref.index==0xffffffff){ref.ready=2;++gCorpusMisses;return false;}
    }
    const unsigned bucket=byHash?ref.index/32:CorpusBucket(key,sCorpusRoots,sCorpusBuckets);
    if(!CorpusIndexSector(sCorpusAddressBase+bucket))return false;
    unsigned index=byHash?ref.index%32:0;
    if(!byHash){const unsigned end=sCorpusCount-bucket*32<32?sCorpusCount-bucket*32:32;bool found=false;
        for(unsigned i=0;i<end;++i){const auto* e=(const CorpusEntry*)(sCorpusIndex+i*16);if(e->start<=key&&key<e->end){index=i;found=true;}if(e->start>key)break;}
        if(!found){ref.hash=1;ref.key=hash;ref.ready=0;ref.index=0xffffffff;++gCorpusMisses;return false;}ref.index=bucket*32+index;}
    if(ref.index>=sCorpusCount)return false;
    ref.entry=*(const CorpusEntry*)(sCorpusIndex+index*16);
    if(ref.entry.start>=ref.entry.end||ref.entry.document<sCorpusDataBase||ref.entry.document*512>=sCorpusSize)return false;
    ref.ready=1;return true;
}
static bool CorpusReady(const ReaderFrame& f)
{
    if(!NativeFrame(f))return false;const unsigned slot=f.page&0x3fff;if(slot>=KOTO_TEXT_TOTAL_SLOTS)return false;
    const auto& ref=sCorpusRefs[slot];
    if(ref.ready!=1||ref.sequence!=gKotoTextRecords[slot].sequence||ref.index!=sCorpusDocIndex)return false;
    if(ref.hash){const unsigned n=NativeNormalized(gKotoTextRecords[slot],sNativeNormalized,true);
        if(n!=Corpus32(sCorpusDocument+16)||memcmp(sNativeNormalized,sCorpusDocument+32,n))return false;}
    return true;
}
// These two bytes belonged to padding in the capture's source reference.
// Keep only route/publication decisions across document eviction. Getters and
// renderers still require the currently loaded, pinned source and raw binding.
static bool CorpusOrthographyRoute(unsigned slot,u32 source)
{
    if(slot>=KOTO_TEXT_TOTAL_SLOTS||slot>=gKotoTextCapacity)return false;
    if(source>=0x02200000u&&source<0x02400000u)source=source-0x02200000u+0x08000000u;
    if(source<0x08000000u||source>=0x0a000000u)return false;
    auto& ref=sCorpusRefs[slot];const auto& r=gKotoTextRecords[slot];
    const bool bound=r.ready&&r.sequence&&ref.sequence==r.sequence&&!ref.hash&&ref.key==source-0x08000000u;
    if(!bound)return false;
    if(OrthographySourceReady(source))ref.pad[0]=1;
    return ref.pad[0];
}
static unsigned CorpusStaticPage(unsigned slot)
{const auto& ref=sCorpusRefs[slot];return ref.ready==1&&!ref.hash&&ref.entry.owner==255?ref.entry.page:65535;}
static void CorpusPrepare(const ReaderFrame& f)
{
    if((!NativeFrame(f)&&!IsSentenceLegacyFull(f))||gCorpusLoadStatus!=10)return;const unsigned slot=f.page&0x3fff;
    if(slot>=KOTO_TEXT_TOTAL_SLOTS||sCorpusRefs[slot].ready!=1)return;
    const auto& ref=sCorpusRefs[slot];if(ref.index==sCorpusDocIndex||sCorpusJob||sCorpusVerifying)return;
    // Never replace document bytes while a renderer is consuming them. Text
    // itself is copied to the existing native render buffer before publication.
    sCorpusWantedIndex=ref.index;sCorpusExpectedSource=ref.entry.start;sCorpusDocSector=ref.entry.document;sCorpusDocIndex=0xffffffff;sCorpusCopied=0;sCorpusDocBytes=512;
}
// Word wrapping uses the same glyph widths as the established renderer. The
// Japanese fragment stays the one actually visible; approved KS5 pages retain
// their kanji, ruby, vocabulary and touch references without conversion.
[[gnu::section(".ewram.text"),gnu::target("arm"),gnu::optimize("O2"),gnu::noinline]]
static unsigned CorpusAppend(unsigned out,const char* text,unsigned& width)
{
    // Corpus strings are verified UTF-8 or generated by our own encoder. Glyph
    // widths need only distinguish Latin-1 from later code points; avoid a
    // nested full decoder frame on the small IRQ stack. Invalid/truncated
    // units consume one byte, so look-ahead never passes a terminating NUL.
    while(*text&&out+8<1536){const u8 first=(u8)*text;unsigned bytes=first>=0xc2&&first<=0xdf?2:first>=0xe0&&first<=0xef?3:first>=0xf0&&first<=0xf4?4:1;
        for(unsigned i=1;i<bytes;++i)if(((u8)text[i]&0xc0)!=0x80){bytes=1;break;}
        if(first=='\n'){sNativeRenderText[out++]='\n';width=0;++text;continue;}
        if(first==' '){unsigned word=0;const char* next=text+1;while(*next&&*next!=' '&&*next!='\n'&&word<=224){const u8 lead=(u8)*next;unsigned n=lead>=0xc2&&lead<=0xdf?2:lead>=0xe0&&lead<=0xef?3:lead>=0xf0&&lead<=0xf4?4:1;
                for(unsigned i=1;i<n;++i)if(((u8)next[i]&0xc0)!=0x80){n=1;break;}
                word+=n>1&&lead>=0xc4?14:6;next+=n;}
            if(width&&width+6+word>224){sNativeRenderText[out++]='\n';width=0;++text;continue;}}
        const unsigned w=bytes>1&&first>=0xc4?14:6;
        if(width+w>224){sNativeRenderText[out++]='\n';width=0;}
        for(unsigned i=0;i<bytes;++i)sNativeRenderText[out++]=*text++;width+=w;
    }
    return out;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static const char* CorpusVariableDecode(u32 pointer,unsigned token)
{
    unsigned n=0;
    if(pointer)for(unsigned i=0;i<64&&n+4<512;++i){u8 ch;
        if((pointer+i>=0x02000000&&pointer+i<0x02200000)||(pointer+i>=0x03000000&&pointer+i<0x03008000))ch=*(const u8*)(pointer+i);
        else if(!NativeRomByte(pointer+i,ch))break;
        if(ch==0xff)break;
        if(ch==0xfc){u8 command;const u32 p=pointer+ ++i;
            if((p>=0x02000000&&p<0x02200000)||(p>=0x03000000&&p<0x03008000))command=*(const u8*)p;
            else if(!NativeRomByte(p,command))break;i+=NativeControlSize(command);continue;}
        if(ch==0xfa||ch==0xfb||ch==0xfe){sNativeNormalized[n++]=' ';continue;}
        const u32 cp=OrthographyCodepoint(ch);
        if(cp<128)sNativeNormalized[n++]=cp;
        else if(cp<2048){sNativeNormalized[n++]=0xc0|(cp>>6);sNativeNormalized[n++]=0x80|(cp&63);}
        else{sNativeNormalized[n++]=0xe0|(cp>>12);sNativeNormalized[n++]=0x80|((cp>>6)&63);sNativeNormalized[n++]=0x80|(cp&63);}
    }
    sNativeNormalized[n]=0;
    return pointer?(const char*)sNativeNormalized:token==1?"[tu nombre]":token==6?"[rival]":"…";
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static const char* CorpusVariableText(u32 origin,unsigned token,u32 context)
{
    const u32 pointer=kotogba_variableSource(origin,token,context);
    if((token>=2&&token<=4)||(token>=0x14&&token<=0x1b)||(token>=768&&token<776)){
        // Resolve role/provenance in this shallow frame. Unsupported slots
        // never enter the deeper entity/cache frame, while supported roles
        // retain all four register arguments and the frozen source identity.
        const unsigned role=CorpusEntityRoleFor(origin,token);
        if(role){
            const u32 proven=kotogba_variableOrigin(origin,token,context);
            if(role==8&&pointer&&proven==0x083dc3c2){
                const u8* raw=(const u8*)pointer;unsigned n=0;
                while(n<10&&raw[n]>=0xa1&&raw[n]<=0xaa){sNativeNormalized[n]='0'+raw[n]-0xa1;++n;}
                if(n&&raw[n]==0x0a&&raw[n+1]==0xff){
                    const char* suffix=sKotoRecapQuantitySuffix;
                    do{sNativeNormalized[n++]=*suffix;}while(*suffix++);
                    return (const char*)sNativeNormalized;
                }
            }
            if(const char* translated=CorpusEntityText(role,token,pointer,proven))return translated;
        }
    }
    // Tail-call the decoder after releasing this shallow dispatch frame. A
    // player name never traverses entity lookup or inherits its stack frame.
    return CorpusVariableDecode(pointer,token);
}
[[gnu::target("arm"),gnu::noinline]] static unsigned CorpusVariable(unsigned out,u32 origin,unsigned token,u32 context,unsigned& width)
{
    const char* text=CorpusVariableText(origin,token,context);
    return CorpusAppend(out,text,width);
}
static unsigned Corpus16(const u8* p){return p[0]|(p[1]<<8);}
static const u8* CorpusExtra()
{
    const u32 flags=Corpus32(sCorpusDocument+24);
    return sCorpusDocument+32+Corpus32(sCorpusDocument+16)+Corpus32(sCorpusDocument+20)+1+2*((flags>>8)&255)+8*(flags>>16);
}
// The existing signature remains the first box, for exact RAM hash fallback.
// R26 appends the original complete source separately; it is never rendered
// until all real guest fragments from this capture have finished printing.
[[gnu::target("thumb"),gnu::noinline]] static const u8* CorpusSentenceSource(unsigned& size)
{
    const unsigned flags=Corpus32(sCorpusDocument+24);size=0;if(!(flags&8))return nullptr;
    const u8* p=CorpusExtra();if(flags&2)p+=2+6*Corpus16(p);
    if(flags&4){const unsigned count=Corpus16(p);p+=2;for(unsigned i=0;i<count;++i)p+=6+Corpus16(p+4);}
    size=Corpus16(p);return p+2;
}
// The mask comes from actual printer publications, even while a different
// tab is open. Source proof, capture lifetime and frozen context are required;
// a byte-identical hash match alone never unlocks unseen source content.
[[gnu::target("thumb"),gnu::noinline]] static u32 CorpusSentenceMask()
{
    unsigned raw=0;const u8* original=CorpusSentenceSource(raw);if(!original)return 0;
    u32 expected=0;unsigned ordinal=0,parts=0;bool visible=false;
    for(unsigned i=0;i<raw;++i){const u8 ch=original[i];
        if(ch==0xfc){if(++i>=raw)return 0;i+=NativeControlSize(original[i]);continue;}
        if(ch==0xf8||ch==0xf9){++i;continue;}
        if(ch==0xfd||ch==0xf7){visible=true;++i;continue;}
        if(ch==0xfa||ch==0xfb||ch==0xff){if(visible){if(ordinal>=32)return 0;expected|=1u<<ordinal;++parts;}visible=false;++ordinal;if(ch==0xff)break;}
        else if(ch&&ch!=0xfe)visible=true;
    }
    return parts>=2?expected:0;
}
[[gnu::target("thumb"),gnu::noinline]] static const char* CorpusSentenceTranslation(unsigned slot)
{
    if(slot>=gKotoTextCapacity||gCorpusLoadStatus!=10)return nullptr;
    const auto& ref=sCorpusRefs[slot];const auto& r=gKotoTextRecords[slot];
    if(ref.ready!=1||ref.hash||ref.sequence!=r.sequence||ref.index!=sCorpusDocIndex||!r.origin||r.origin!=0x08000000+ref.entry.start||!(Corpus32(sCorpusDocument+24)&16))return nullptr;
    unsigned raw=0;const u8* p=CorpusSentenceSource(raw);return p?(const char*)(p+raw+2):nullptr;
}
[[gnu::target("thumb"),gnu::noinline]] static bool CorpusComplete(unsigned slot)
{
    if(gCorpusLoadStatus!=10||slot>=gKotoTextCapacity)return false;
    const auto& ref=sCorpusRefs[slot];const auto& r=gKotoTextRecords[slot];
    if(ref.ready!=1||ref.sequence!=r.sequence||ref.index!=sCorpusDocIndex||ref.hash||!r.ready||!r.origin||r.origin!=0x08000000+ref.entry.start)return false;
    const u32 expected=CorpusSentenceMask();if(!expected||(ref.seen&expected)!=expected)return false;
    const unsigned raw=Corpus32(sCorpusDocument+16),flags=Corpus32(sCorpusDocument+24),es=Corpus32(sCorpusDocument+20);
    const u8* vars=sCorpusDocument+32+raw+es+1+2*((flags>>8)&255);
    for(unsigned i=0;i<(flags>>16);++i){const unsigned token=Corpus16(vars+8*i+2);
        if((token<256||(token>=768&&token<1024))&&!kotogba_variableSource(r.origin,token,r.context))return false;}
    return true;
}
static bool CorpusSentenceProven(unsigned slot,bool translation)
{
    if(slot>=KOTO_TEXT_TOTAL_SLOTS||slot>=gKotoTextCapacity)return false;
    auto& ref=sCorpusRefs[slot];const auto& r=gKotoTextRecords[slot];
    if(!r.ready||!r.sequence||ref.sequence!=r.sequence||ref.hash||!r.origin||
       ref.key!=r.origin-0x08000000u||r.origin!=0x08000000u+ref.entry.start)return false;
    if(CorpusComplete(slot)){ref.pad[1]|=1;if(CorpusSentenceTranslation(slot))ref.pad[1]|=2;}
    return ref.seen&&(ref.pad[1]&1)&&(!translation||(ref.pad[1]&2));
}
[[gnu::target("thumb"),gnu::noinline]] static unsigned CorpusFullJapanese(const ReaderFrame& f)
{
    const unsigned slot=f.page&0x3fff;const auto& r=gKotoTextRecords[slot];
    unsigned out=0,width=0,raw=0;const u8* original=CorpusSentenceSource(raw);
    for(unsigned i=0;i<raw&&out+8<1536;++i){const u8 ch=original[i];
        if(ch==0xff)break;
        if(ch==0xfc){if(++i==raw)break;i+=NativeControlSize(original[i]);continue;}
        if(ch==0xf8||ch==0xf9){++i;continue;}
        if(ch==0xfd||ch==0xf7){if(++i==raw)break;
            const unsigned token=original[i]+(ch==0xf7?768:0);
            out=CorpusAppend(out,CorpusVariableDecode(kotogba_variableSource(r.origin,token,r.context),token),width);continue;}
        if(ch==0xfe||ch==0xfa||ch==0xfb){out=CorpusAppend(out,ch==0xfb?" ":" ",width);continue;}
        const u32 cp=sNativeCharacters[ch];char encoded[4];
        if(cp<128){encoded[0]=cp;encoded[1]=0;}
        else if(cp<2048){encoded[0]=0xc0|(cp>>6);encoded[1]=0x80|(cp&63);encoded[2]=0;}
        else{encoded[0]=0xe0|(cp>>12);encoded[1]=0x80|((cp>>6)&63);encoded[2]=0x80|(cp&63);encoded[3]=0;}
        out=CorpusAppend(out,encoded,width);
    }
    sNativeRenderText[out]=0;return out;
}
struct CorpusLinkHit {u16 begin,end,rawStart,rawEnd,card,ref;};
[[gnu::section(".ewram.bss")]] static CorpusLinkHit sCorpusLinkHits[16];
[[gnu::section(".ewram.bss")]] static unsigned sCorpusLinkCount,sCorpusLinkConverting;
static bool CorpusBoundary(u8 c)
{return c==0||c==0xab||c==0xac||c==0xad||c==0xb0||c==0xb1||c==0xb2||c==0xb3||c==0xb4||c==0xfa||c==0xfb||c==0xff;}
static bool CorpusWordEnd(const u8* text,unsigned size,unsigned at)
{
    // Exact particles following an already reviewed dictionary form. FE is
    // deliberately not a boundary: automatic line breaks can divide a word.
    static const u8 suffixes[][4]={{0},{1,0x1a},{1,0x37},{1,0x2d},{1,0x16},{1,0x1d},{1,0x44},{1,0x14},{1,0x19},{1,0x23},
        {2,0x06,0x27},{2,0x1f,0x44},{2,0x26,0x28},{2,0x50,0x13},{2,0x14,0x1a},{2,0x16,0x1a},{2,0x44,0x1a},
        {2,0x16,0x23},{2,0x44,0x23},{2,0x41,0x09},{2,0x0c,0x06},{2,0x15,0x45},{1,0x41},{2,0x44,0x0d},
        {3,0x44,0x0c,0x10},{2,0x41,0x26},{2,0x41,0x18},{2,0x41,0x15},
        {1,0x15},{2,0x15,0x19},{3,0x15,0x19,0x26},{3,0x15,0x2e,0x41},{1,0x26},{1,0x18},{2,0x41,0x40},{2,0x41,0x3f}};
    for(const auto& s:suffixes){const unsigned end=at+s[0];if(end<=size&&!memcmp(text+at,s+1,s[0])&&(end==size||CorpusBoundary(text[end])))return true;}
    return false;
}
[[gnu::target("thumb"),gnu::noinline,gnu::optimize("Os")]] static void CorpusMatchLinks(const ReaderFrame& f,const u8* text,unsigned size)
{
    sCorpusLinkCount=sCorpusLinkConverting=0;
    if(!CorpusReady(f)||!(Corpus32(sCorpusDocument+24)&4))return;
    const u8* base=CorpusExtra();if(Corpus32(sCorpusDocument+24)&2)base+=2+6*Corpus16(base);
    const unsigned count=Corpus16(base);base+=2;
    for(unsigned at=0;at<size&&sCorpusLinkCount<16;++at){
        if(at&&!CorpusBoundary(text[at-1]))continue;
        const u8* best=nullptr;unsigned length=0;const u8* p=base;
        for(unsigned i=0;i<count;++i){const unsigned n=Corpus16(p+4);
            if(n>length&&at+n<=size&&!memcmp(text+at,p+6,n)&&CorpusWordEnd(text,size,at+n)){best=p;length=n;}p+=6+n;}
        if(best){sCorpusLinkHits[sCorpusLinkCount++]={0,0,(u16)at,(u16)(at+length),(u16)Corpus16(best),(u16)Corpus16(best+2)};at+=length-1;}
    }
}
static void CorpusLinkPosition(unsigned raw,unsigned utf8)
{
    if(sCorpusLinkConverting>=sCorpusLinkCount)return;
    auto& hit=sCorpusLinkHits[sCorpusLinkConverting];if(raw==hit.rawStart)hit.begin=utf8;
    if(raw==hit.rawEnd){hit.end=utf8;++sCorpusLinkConverting;}
}
// R29 matching stays in original GBA offsets. UTF8 positions and old KS5
// rectangles cannot identify a word after several kana become one kanji.
static void CorpusOrthographyLinks(const ReaderFrame& f)
{
    unsigned bytes=0;const u8* raw=LinguisticRaw(bytes);
    CorpusMatchLinks(f,raw,raw?bytes:0);
}
static u32 CorpusWordIdentity(u32 card,u16 ref)
{
    if((ref&0xc000)==0x4000){card=255;ref&=0x3fff;}
    return kotogba_memoryLookup(card,ref);
}
static bool CorpusRawWord(unsigned begin,unsigned end,u32& card,u16& ref,u32 identity,bool restrictPartial)
{
    for(unsigned i=0;i<sCorpusLinkCount;++i){const auto& hit=sCorpusLinkHits[i];
        if(begin<hit.rawEnd&&hit.rawStart<end&&(!restrictPartial||
           (hit.rawStart<=begin&&end<=hit.rawEnd)||(identity&&CorpusWordIdentity(hit.card,hit.ref)==identity))){
            card=hit.card;ref=hit.ref;return true;}}
    return false;
}
static bool CorpusHasLink(unsigned offset)
{
    for(unsigned i=0;i<sCorpusLinkCount;++i)if(offset>=sCorpusLinkHits[i].begin&&offset<sCorpusLinkHits[i].end)return true;
    return false;
}
[[gnu::target("thumb"),gnu::noinline,gnu::optimize("Os")]] static bool CorpusTouch(unsigned x,unsigned y,u32& card,u16& ref)
{
    if(!sHasShown||(sRender.busy&&!SamePage(sRender.target,sShown))||sShown.tab||!NativeFrame(sShown)||!sCorpusLinkCount)return false;
    const char* p=sNativeRenderText;unsigned xx=10;int yy=56-sShown.scroll;
    while(*p){const unsigned at=p-sNativeRenderText;const u32 cp=DecodeUtf8(p);
        if(cp=='\n'){xx=10;yy+=14;continue;}const unsigned width=cp>255?14:6;
        if((int)y>=yy&&(int)y<yy+14&&x+2>=xx&&x<xx+width+2)
            for(unsigned i=0;i<sCorpusLinkCount;++i){const auto& hit=sCorpusLinkHits[i];if(at>=hit.begin&&at<hit.end){card=hit.card;ref=hit.ref;return true;}}
        xx+=width;if(yy>(int)y)return false;
    }
    return false;
}
// Composition is serialized by the existing reader/render service, just like
// sNativeRenderText. Keep its scratch values out of the 288-byte IRQ stack.
struct CorpusComposeWork {
    const char* spanish;const u8 *layout,*vars;
    unsigned es,flags,breaks,variables,at,limit,br,var,width,out;
    u32 origin,context;bool waiting,aligned;
};
[[gnu::section(".ewram.bss")]] static CorpusComposeWork sCorpusCompose;
[[gnu::target("thumb"),gnu::noinline]] static void CorpusSelectFragment(const ReaderFrame& f)
{
    auto& c=sCorpusCompose;const unsigned slot=f.page&0x3fff;
    const auto& record=gKotoTextRecords[slot];c.origin=record.origin;c.context=record.context;
    c.at=c.br=c.var=c.width=c.out=0;c.limit=c.es;c.waiting=c.aligned=false;
    if(c.flags&2){
        const u8* table=CorpusExtra();const unsigned count=Corpus16(table);table+=2;
        const u32 cursor=record.cursor?record.cursor:c.origin;
        const u32 base=0x08000000+sCorpusRefs[slot].entry.start;
        const u32 relative=cursor>=base?cursor-base:0;unsigned fragment=count;
        for(unsigned i=0;i<count;++i)if(Corpus16(table+6*i)<=relative&&relative<=Corpus16(table+6*i+2)){fragment=i;break;}
        if(fragment<count){
            c.at=Corpus16(table+6*fragment+4);c.aligned=c.at!=65535;
            if(c.aligned)c.limit=fragment+1<count?Corpus16(table+6*(fragment+1)+4)-1:c.es;
            else c.at=0;
        }
        const u32 all=count==32?0xffffffffu:(1u<<count)-1;
        c.waiting=!c.aligned&&(fragment==count||(sCorpusRefs[slot].seen&all)!=all);
    }
    // NativeDecode already validated the complete capture at its shallow
    // entry before touching full-source bytes. Do not retain this compositor
    // and fragment-selector stack through that proof a second time.
    if(f.page&0x4000){c.at=0;c.limit=c.es;c.waiting=false;c.aligned=false;}
    while(c.br<c.breaks&&Corpus16(c.layout+c.br*2)<c.at)++c.br;
    while(c.var<c.variables&&Corpus16(c.vars+c.var*8)<c.at)++c.var;
}
[[gnu::target("thumb"),gnu::noinline]] static unsigned CorpusCompose(const ReaderFrame& f,unsigned japaneseLength)
{
    memmove(sNativeRenderText+1536,sNativeRenderText,japaneseLength+1);
    auto& c=sCorpusCompose;const unsigned raw=Corpus32(sCorpusDocument+16);
    c.es=Corpus32(sCorpusDocument+20);c.flags=Corpus32(sCorpusDocument+24);
    c.spanish=(const char*)sCorpusDocument+32+raw;c.breaks=(c.flags>>8)&255;c.variables=c.flags>>16;
    c.layout=(const u8*)c.spanish+c.es+1;c.vars=c.layout+2*c.breaks;
    CorpusSelectFragment(f);
    c.out=CorpusAppend(c.out,f.page&0x4000?"Oración completa\n\nEn español\n\n":c.waiting?"Traducción de este recuadro\npendiente.\n\n":(c.flags&1)&&!c.aligned?"En español (diálogo completo)\n\n":"En español\n\n",c.width);
    if((f.page&0x4000)&&OrthographyActive(f))
        if(const char* whole=CorpusSentenceTranslation(f.page&0x3fff)){
            c.out=OrthographyFullSpanish(f,whole,c.out);c.at=c.limit;}
    if(c.waiting)c.at=c.limit;
    while(c.at<c.limit&&c.out+8<1536){
        const unsigned nextBreak=c.br<c.breaks?Corpus16(c.layout+c.br*2):c.es;
        const unsigned nextVar=c.var<c.variables?Corpus16(c.vars+c.var*8):c.es;
        unsigned next=nextBreak<nextVar?nextBreak:nextVar;if(next>c.limit)next=c.limit;
        if(next<c.at||next>c.es)break;
        const unsigned length=next-c.at<1527-c.out?next-c.at:1527-c.out;
        memcpy(sNativeRenderText+c.out,c.spanish+c.at,length);c.out+=length;c.at+=length;
        if(c.at!=next||c.at==c.limit)break;
        if(next==nextBreak){sNativeRenderText[c.out++]='\n';++c.at;++c.br;continue;}
        const u8* v=c.vars+8*c.var++;const unsigned token=Corpus16(v+2),bytes=Corpus16(v+4);c.width=Corpus16(v+6);
        if(token<256||(token>=768&&token<1024))c.out=CorpusVariable(c.out,c.origin,token,c.context,c.width);
        // F9:05 is the native level glyph; preserve the following frozen numeric value.
        else if(token==0x105)c.out=CorpusAppend(c.out,"nivel ",c.width);
        else if(token<512){sNativeRenderText[c.out++]='-';sNativeRenderText[c.out++]=' ';}
        else if(token>=1024)c.out=CorpusAppend(c.out,token==1024?"[A] ":token==1025?"[B] ":"[control] ",c.width);
        c.at+=bytes;
    }
    if(const unsigned number=kotogba_recapNumber(&gKotoTextRecords[f.page&0x3fff])){
        if(c.out+2<1536){sNativeRenderText[c.out++]=' ';sNativeRenderText[c.out++]=number;}
    }
    c.out=CorpusAppend(c.out,f.page&0x4000?"\n\nJaponés\n\n":"\n\nJaponés visible\n\n",c.width);
    for(unsigned i=0;i<sCorpusLinkCount;++i){sCorpusLinkHits[i].begin+=c.out;sCorpusLinkHits[i].end+=c.out;}
    const unsigned japaneseStart=c.out;
    if(c.out+japaneseLength+1>KOTO_NATIVE_RENDER_BYTES)japaneseLength=KOTO_NATIVE_RENDER_BYTES-1-c.out;
    memmove(sNativeRenderText+c.out,sNativeRenderText+1536,japaneseLength+1);c.out+=japaneseLength;sNativeRenderText[c.out]=0;
    unsigned lines=1,prefix=0;for(unsigned i=0;i<c.out;++i)if(sNativeRenderText[i]=='\n'){++lines;if(i<japaneseStart)++prefix;}
    sNativeJapanesePrefixLines=prefix;
    return 14*lines+28;
}
