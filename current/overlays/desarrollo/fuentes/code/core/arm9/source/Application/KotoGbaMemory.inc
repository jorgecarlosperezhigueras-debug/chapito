// Step 7: immutable identity index. Loaded and validated before the VM; no
// dictionary/FAT/heap access from the UI or game hooks. KS5 remains authoritative.
struct MemoryLink {u32 reference,identity;};
[[gnu::section(".ewram.bss")]] static MemoryLink sMemoryLinks[768];
[[gnu::section(".ewram.bss")]] static u16 sMemoryCount;
[[gnu::section(".ewram.bss")]] static u32 sSavedMemory[512];
[[gnu::section(".ewram.bss")]] volatile u32 gMemoryLoadStatus;
[[gnu::section(".ewram.bss")]] static u32 sMemoryLastRef,sMemoryLastIdentity;
[[gnu::section(".ewram.bss")]] static u32 sSavedCheckKey;
[[gnu::section(".ewram.bss")]] static u16 sSavedCheckCount;
[[gnu::section(".ewram.bss")]] static bool sSavedCheckResult;

extern "C" [[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline,gnu::optimize("Os")]] void kotogba_loadMemory()
{
    sMemoryCount=0;sMemoryLastRef=0xffffffff;sMemoryLastIdentity=0;sSavedCheckCount=65535;gMemoryLoadStatus=1;
    if(!sStudyReady)return;
    FIL f{};UINT got=0;u32 header[8];
    if(f_open(&f,"/_gba/kotogba/packages/BPRJ_01.km7",FA_READ)!=FR_OK)return;
    const u32 size=f_size(&f);
    bool ok=size>=32&&size<=32+sizeof(sMemoryLinks)&&
        f_read(&f,header,32,&got)==FR_OK&&got==32;
    if(ok)ok=!memcmp(header,"KOTOLM7",8)&&header[2]==1&&header[3]==size&&
        header[4]<=768&&size==32+header[4]*8&&header[5]==Read32(8)&&header[6]==Read32(12);
    if(ok)ok=f_read(&f,sMemoryLinks,size-32,&got)==FR_OK&&got==size-32;
    f_close(&f);
    if(!ok){gMemoryLoadStatus=2;return;}
    const u8* p=(const u8*)sMemoryLinks;u32 hash=2166136261u;
    for(u32 i=0;i<size-32;++i)hash=(hash^p[i])*16777619u;
    if(hash!=header[7]){gMemoryLoadStatus=3;return;}
    for(unsigned i=0;i<header[4];++i){
        const auto& r=sMemoryLinks[i];const unsigned cardId=r.reference>>16,ref=r.reference&65535;
        const u8* card=FindStudy(cardId);
        if(!r.identity||r.identity&0x80000000u||(i&&sMemoryLinks[i-1].reference>=r.reference)||
           !card||ref&0x8000||((ref>>9)&63)>=Card16(card,12)) {gMemoryLoadStatus=4;return;}
        const u32 page=Read32(36)+8*(Card16(card,6)+((ref>>9)&63));
        if((ref&511)>=Read16(page+4)){gMemoryLoadStatus=4;return;}
    }
    sMemoryCount=header[4];gMemoryLoadStatus=10;
}

extern "C" [[gnu::noinline]] u32 kotogba_memoryLookup(u32 card,u16 ref)
{
    if(card>65535||!sMemoryCount)return 0;
    const u32 key=(card<<16)|ref;
    if(key==sMemoryLastRef)return sMemoryLastIdentity;
    unsigned lo=0,hi=sMemoryCount;
    while(lo<hi){unsigned m=(lo+hi)/2;if(sMemoryLinks[m].reference<key)lo=m+1;else hi=m;}
    sMemoryLastRef=key;
    return sMemoryLastIdentity=lo<sMemoryCount&&sMemoryLinks[lo].reference==key?sMemoryLinks[lo].identity:0;
}
static u32 MemoryWordKey(u32 card,u16 ordinal)
{
    if(card==0x7ffe||card&0x8000)return InlineMemoryKey(card,ordinal);
    u16 ref;
    if(ordinal&0x8000)ref=ordinal&0x7fff;
    else{
        if(ordinal>=sizeof(sKotoTapAnchors)/sizeof(sKotoTapAnchors[0]))return 0;
        const auto& a=sKotoTapAnchors[ordinal];if(a.card!=card)return 0;
        ref=a.card>=133&&a.card<=135?a.firstLine<<9:a.firstLine;
    }
    return kotogba_memoryLookup(card,ref);
}
