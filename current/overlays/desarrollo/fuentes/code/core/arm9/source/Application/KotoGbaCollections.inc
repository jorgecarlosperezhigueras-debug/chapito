// KM8 owns two preallocated16KiB banks. KM7/K20 remain read-only imports.
// Stable linguistic IDs and the original four-byte word references survive
// migration. Frozen dynamic names share the same CRC/commit as the lists.
// FAT is used only before the VM; runtime writes one sector per async slice.
struct CHeader {char magic[8];u32 generation;u16 words,kanjis;u32 format,hash;};
enum { CBankBytes=16384,CBankSectors=32,CFrozenCard=0x7ffe,CFrozenLimit=128,CFrozenOffset=9216 };
struct CFrozenWord {u32 identity;char title[40];u16 token,flags;u32 canonicalSource,meaningKey;};
static_assert(sizeof(CFrozenWord)==56&&CFrozenOffset+CFrozenLimit*sizeof(CFrozenWord)==CBankBytes);
[[gnu::section(".ewram.bss")]] alignas(32) static u8 sCSaveImage[CBankBytes];
[[gnu::section(".ewram.bss")]] static CFrozenWord sCFrozen[CFrozenLimit];
[[gnu::section(".ewram.bss")]] static u16 sSavedKanjis[2400];
[[gnu::section(".ewram.bss")]] static u16 sSavedKanjiCount,sCSnapshotFrozen;
[[gnu::section(".ewram.bss")]] static u32 sCSectors[2*CBankSectors],sCDiskGeneration,sCHash;
[[gnu::section(".ewram.bss")]] static CHeader sCSnapshot;
[[gnu::section(".ewram.bss")]] static FsWaitToken sCToken;
[[gnu::section(".ewram.bss")]] static u16 sCPosition;
[[gnu::section(".ewram.bss")]] static u8 sCDevice,sCDiskSlot,sCDestSlot,sCState,sCWriteIndex;
[[gnu::section(".ewram.bss")]] static bool sCReady,sCDirty;
[[gnu::section(".ewram.bss")]] volatile u32 gCollectionsLoadStatus;

static u32 CHash(const u8* p,unsigned size)
{u32 h=2166136261u;for(unsigned i=0;i<size;++i)if(i<20||i>=24)h=(h^p[i])*16777619u;return h;}
static bool CValid(bool legacy)
{const CHeader& h=*(const CHeader*)sCSaveImage;return !memcmp(h.magic,legacy?"KOTOC20":"KOTOCM7",8)&&h.format==(legacy?0x4b323001u:0x4b4d3701u)&&h.words<=512&&h.kanjis<=2400&&h.hash==CHash(sCSaveImage,legacy?8192:9216);}
static void CApply(bool legacy)
{const CHeader& h=*(const CHeader*)sCSaveImage;sCDiskGeneration=h.generation;sSavedCount=h.words;sSavedKanjiCount=h.kanjis;memcpy(sSavedWords,sCSaveImage+32,h.words*4);memcpy(sSavedKanjis,sCSaveImage+2080,h.kanjis*2);
 if(legacy)memset(sSavedMemory,0,sizeof(sSavedMemory));else memcpy(sSavedMemory,sCSaveImage+6880,h.words*4);}
static unsigned CFrozenTitleBytes(const char* title)
{
    unsigned at=0;
    while(at<40&&title[at]){
        const unsigned c=(u8)title[at];unsigned n=0;
        if(c>=32&&c<128)n=1;
        else if(c>=0xc2&&c<=0xdf)n=2;
        else if(c>=0xe0&&c<=0xef)n=3;
        else if(c>=0xf0&&c<=0xf4)n=4;
        if(!n||at+n>=40)return 0;
        for(unsigned i=1;i<n;++i)if(((u8)title[at+i]&0xc0)!=0x80)return 0;
        const unsigned second=(u8)title[at+1];
        if((c==0xe0&&second<0xa0)||(c==0xed&&second>=0xa0)||(c==0xf0&&second<0x90)||(c==0xf4&&second>=0x90))return 0;
        at+=n;
    }
    return at&&at<40?at:0;
}
static u32 CFrozenIdentity(u16 slot)
{return slot<CFrozenLimit?sCFrozen[slot].identity:0;}
static bool CValid8()
{
    const CHeader& h=*(const CHeader*)sCSaveImage;
    const unsigned count=sCSaveImage[24]|sCSaveImage[25]<<8;
    if(memcmp(h.magic,"KOTOCM8",8)||h.format!=0x4b4d3801u||h.words>512||h.kanjis>2400||
       count>CFrozenLimit||sCSaveImage[26]!=1||sCSaveImage[27]||sCSaveImage[28]||sCSaveImage[29]||sCSaveImage[30]||sCSaveImage[31]||
       h.hash!=CHash(sCSaveImage,CBankBytes))return false;
    const auto* pool=(const CFrozenWord*)(sCSaveImage+CFrozenOffset);unsigned actual=0;
    for(unsigned i=0;i<CFrozenLimit;++i){const auto& word=pool[i];
        if(!word.identity){const u8* p=(const u8*)&word;for(unsigned j=0;j<sizeof(word);++j)if(p[j])return false;continue;}
        ++actual;const unsigned bytes=CFrozenTitleBytes(word.title);
        if(!bytes||word.flags||!word.meaningKey||word.meaningKey&0x80000000u||word.canonicalSource>=0x02000000u||word.token>1023)return false;
        for(unsigned j=bytes+1;j<sizeof(word.title);++j)if(word.title[j])return false;
    }
    if(actual!=count)return false;
    const auto* words=(const SavedWord*)(sCSaveImage+32);const auto* identities=(const u32*)(sCSaveImage+6880);
    for(unsigned i=0;i<h.words;++i)if(words[i].card==CFrozenCard){
        const unsigned slot=words[i].ordinal;
        if(slot>=CFrozenLimit||!pool[slot].identity||pool[slot].identity!=identities[i])return false;}
    return true;
}
static void CApply8()
{CApply(false);memcpy(sCFrozen,sCSaveImage+CFrozenOffset,sizeof(sCFrozen));}
static unsigned CFrozenCount()
{unsigned count=0;for(const auto& entry:sCFrozen)count+=entry.identity!=0;return count;}
static void CPrepareSnapshot()
{
    memcpy(sCSnapshot.magic,"KOTOCM8",8);sCSnapshot.generation=sCDiskGeneration+1;
    sCSnapshot.words=sSavedCount;sCSnapshot.kanjis=sSavedKanjiCount;sCSnapshot.format=0x4b4d3801;
    sCSnapshotFrozen=CFrozenCount();sCPosition=0;sCHash=2166136261u;sCDestSlot=sCDiskSlot^1;
}
static void CLoadFailure(unsigned status)
{sCReady=false;sSavedCount=sSavedKanjiCount=0;memset(sSavedMemory,0,sizeof(sSavedMemory));memset(sCFrozen,0,sizeof(sCFrozen));gCollectionsLoadStatus=status;}
// Return0=absent,1=valid bank(s),2=historical empty file,3=I/O or corruption.
[[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]] static unsigned CImportLegacy(const char* path,unsigned bankBytes,bool legacy)
{
    FIL f{};UINT got=0;const FRESULT opened=f_open(&f,path,FA_READ);
    if(opened==FR_NO_FILE||opened==FR_NO_PATH)return 0;
    if(opened!=FR_OK)return 3;
    if(f_size(&f)!=2*bankBytes){f_close(&f);return 3;}
    bool have=false,nonzero=false,ok=true;
    for(unsigned slot=0;slot<2;++slot){
        if(f_lseek(&f,slot*bankBytes)!=FR_OK||f_read(&f,sCSaveImage,bankBytes,&got)!=FR_OK||got!=bankBytes){ok=false;break;}
        for(unsigned i=0;i<bankBytes;++i)nonzero|=sCSaveImage[i]!=0;
        if(CValid(legacy)){
            const u32 generation=((CHeader*)sCSaveImage)->generation;
            if(!have||(s32)(generation-sCDiskGeneration)>0){CApply(legacy);have=true;}}
    }
    if(f_close(&f)!=FR_OK)ok=false;
    return ok?(have?1:nonzero?3:2):3;
}
static bool CNormalizeIdentities()
{
    bool changed=false;unsigned kept=0;
    for(unsigned i=0;i<sSavedCount;++i){
        // Frozen IDs belong to their persisted exact names; boot has no live
        // game binding to recreate them and must never replace them from RAM.
        const u32 resolved=sSavedWords[i].card==CFrozenCard?CFrozenIdentity(sSavedWords[i].ordinal):MemoryWordKey(sSavedWords[i].card,sSavedWords[i].ordinal);
        const u32 identity=resolved?resolved:sSavedMemory[i];changed|=identity!=sSavedMemory[i];
        bool duplicate=false;
        for(unsigned j=0;j<kept;++j)if((identity&&sSavedMemory[j]==identity)||
            (sSavedWords[j].card==sSavedWords[i].card&&sSavedWords[j].ordinal==sSavedWords[i].ordinal)){duplicate=true;break;}
        if(duplicate){changed=true;continue;}
        sSavedWords[kept]=sSavedWords[i];sSavedMemory[kept++]=identity;
    }
    sSavedCount=kept;return changed;
}
static u8 CByte(unsigned pos);
extern "C" [[gnu::target("arm"),gnu::section(".ewram.text"),gnu::noinline]] void kotogba_loadCollections()
{
    gCollectionsLoadStatus=1;sCReady=false;sCState=0;sCDirty=false;sSavedKanjiCount=0;sCDiskGeneration=0;sCDiskSlot=1;
    sSavedCheckCount=65535;memset(sSavedMemory,0,sizeof(sSavedMemory));memset(sCFrozen,0,sizeof(sCFrozen));
    FIL f{};UINT got=0;bool have=false,created=false;
    FRESULT opened=f_open(&f,"/_gba/kotogba/mis-colecciones.km8",FA_READ|FA_WRITE);
    if(opened==FR_NO_FILE||opened==FR_NO_PATH){
        // Legacy files are never opened for write, resized or repurposed.
        const unsigned prior=CImportLegacy("/_gba/kotogba/mis-colecciones.km7",9216,false);
        if(prior==3||(prior!=1&&CImportLegacy("/_gba/kotogba/mis-colecciones.k20",8192,true)==3)){CLoadFailure(6);return;}
        CNormalizeIdentities();
        opened=f_open(&f,"/_gba/kotogba/mis-colecciones.km8",FA_CREATE_NEW|FA_READ|FA_WRITE);
        if(opened!=FR_OK){CLoadFailure(5);return;}
        created=true;
        // FatFs seek-based allocation leaves old cluster contents untouched.
        // Write both banks in full before exposing the initialized file size.
        memset(sCSaveImage,0,sizeof(sCSaveImage));
        bool ok=f_write(&f,sCSaveImage,CBankBytes,&got)==FR_OK&&got==CBankBytes&&
                f_write(&f,sCSaveImage,CBankBytes,&got)==FR_OK&&got==CBankBytes&&f_sync(&f)==FR_OK;
        if(ok){
            CPrepareSnapshot();
            for(unsigned i=0;i<CBankBytes;++i)sCSaveImage[i]=CByte(i);
            ((CHeader*)sCSaveImage)->hash=CHash(sCSaveImage,CBankBytes);
            // A first valid bank is committed before the VM. An interruption
            // before this point leaves the read-only old journal recoverable.
            ok=f_lseek(&f,0)==FR_OK&&f_write(&f,sCSaveImage,CBankBytes,&got)==FR_OK&&got==CBankBytes&&f_sync(&f)==FR_OK;
            if(ok){sCDiskGeneration=sCSnapshot.generation;sCDiskSlot=0;have=true;}
        }
        if(!ok){f_close(&f);CLoadFailure(5);return;}
    }else if(opened!=FR_OK){CLoadFailure(5);return;}
    if(f_size(&f)!=2*CBankBytes){f_close(&f);CLoadFailure(3);return;}
    if(!created){
        gCollectionsLoadStatus=2;
        for(unsigned slot=0;slot<2;++slot){
            if(f_lseek(&f,slot*CBankBytes)!=FR_OK||f_read(&f,sCSaveImage,CBankBytes,&got)!=FR_OK||got!=CBankBytes){f_close(&f);CLoadFailure(5);return;}
            if(CValid8()){
                const u32 generation=((CHeader*)sCSaveImage)->generation;
                if(!have||(s32)(generation-sCDiskGeneration)>0){CApply8();sCDiskSlot=slot;have=true;}}
        }
        if(!have){f_close(&f);CLoadFailure(4);return;}
    }
    const bool identitiesAdded=CNormalizeIdentities();
    DWORD map[132];map[0]=132;f.cltbl=map;bool ok=f_lseek(&f,CREATE_LINKMAP)==FR_OK;
    if(ok&&f.obj.fs){sCDevice=f.obj.fs->pdrv;const unsigned size=f.obj.fs->csize;const u32 base=f.obj.fs->database;
        ok=sCDevice<=1&&size&&!(size&(size-1));
        for(unsigned i=0;ok&&i<2*CBankSectors;++i){unsigned cluster=i/size;const DWORD* t=map+1;
            while(*t&&cluster>=*t){cluster-=*t;t+=2;}if(!*t){ok=false;break;}
            sCSectors[i]=base+(t[1]+cluster-2)*size+i%size;}
    }else ok=false;
    if(f_close(&f)!=FR_OK)ok=false;
    sCReady=ok;if(ok){gCollectionsLoadStatus=10;sCDirty=identitiesAdded;}else CLoadFailure(5);
}

[[gnu::target("arm"),gnu::optimize("Os"),gnu::noinline]] static unsigned CFindKanji(u16 doc)
{
    u16* const end=sSavedKanjis+sSavedKanjiCount;
    u16* read=sSavedKanjis;
    while(end-read>=16){
        u32 value,matched;
        // Postincrement keeps each check to three ARM instructions even when
        // Os would otherwise add a conditional pointer fixup per candidate.
        asm volatile(
            ".rept 16\n"
            "ldrh %[value],[%[read]],#2\n"
            "cmp %[value],%[doc]\n"
            "beq 1f\n"
            ".endr\n"
            "mov %[matched],#0\n"
            "b 2f\n"
            "1: mov %[matched],#1\n"
            "2:\n"
            :[read]"+&r"(read),[value]"=&r"(value),[matched]"=&r"(matched)
            :[doc]"r"((u32)doc):"cc","memory");
        if(matched)return read-sSavedKanjis-1;
    }
    while(read!=end)if(*read++==doc)return read-sSavedKanjis-1;
    return sSavedKanjiCount;
}
static bool CIsKanjiSaved(u16 doc)
{return CFindKanji(doc)!=sSavedKanjiCount;}
static void CChanged()
{
    // Serialization reads the collection arrays a few bytes per IRQ. If they
    // change before any sector is submitted, rebuild that snapshot in full.
    // Once writing starts the image is immutable: finish its commit, then
    // persist the new dirty state in the other slot.
    if(sCState==1)sCState=0;
    sCDirty=true;sSavedCheckCount=65535;
    // A restored list is a visual parent, but a changed membership requires
    // new rows/hit coordinates before another list item can be selected.
    for(auto& background:sReaderBackdrops)if(background.active&&background.frame.tab)background.shown=false;
    if(sStudyTab){
        const int height=(sStudyTab==1?sSavedCount:sSavedKanjiCount)*CRowHeight(sStudyTab);
        const int limit=height>117?height-117:0;
        if(sScroll>limit)sScroll=limit;
    }
    if(sModalKind)sShownModalStatus=255;else sHasShown=false;
    sRender.busy=false;sKRepaint=true;
}
static void CWordChanged(bool saved)
{
    CChanged();
    // The toggle has just established this exact sense's state. Seed only
    // that query; another word still performs the normal identity lookup.
    sSavedCheckKey=(sModalCard<<16)|sModalOrdinal;
    sSavedCheckCount=sSavedCount;sSavedCheckResult=saved;
}
// New literal names are admitted only from the exact frozen modal. The
// canonical identity and role metadata never come from a live guest buffer.
[[gnu::target("arm"),gnu::optimize("Os"),gnu::noinline]] static bool CFrozenPrepareCurrent()
{
    u32 identity,meaningKey,canonicalSource;u16 token;const char* title;
    if(!InlineFrozenCurrent(identity,meaningKey,token,canonicalSource,title)||!identity||
       !meaningKey||meaningKey&0x80000000u||canonicalSource>=0x02000000u||token>1023)return false;
    const unsigned bytes=CFrozenTitleBytes(title);if(!bytes)return false;
    unsigned slot=CFrozenLimit;
    for(unsigned i=0;i<CFrozenLimit;++i)if(sCFrozen[i].identity==identity){
        const auto& entry=sCFrozen[i];
        if(entry.token!=token||entry.canonicalSource!=canonicalSource||strcmp(entry.title,title))return false;
        slot=i;break;}
    bool saved=false;for(unsigned i=0;i<sSavedCount;++i)if(sSavedMemory[i]==identity){saved=true;break;}
    if(!saved&&sSavedCount>=512)return false;
    if(slot==CFrozenLimit){
        // Build one16-byte usage map instead of rescanning512 references for
        // every occupied pool slot. Orphans can be reused after removal.
        u32 used[4]={};
        for(unsigned i=0;i<sSavedCount;++i)if(sSavedWords[i].card==CFrozenCard&&sSavedWords[i].ordinal<CFrozenLimit)
            used[sSavedWords[i].ordinal>>5]|=1u<<(sSavedWords[i].ordinal&31);
        for(unsigned i=0;i<CFrozenLimit;++i)if(!(used[i>>5]&(1u<<(i&31)))){slot=i;break;}
        if(slot==CFrozenLimit)return false;
        auto& entry=sCFrozen[slot];memset(&entry,0,sizeof(entry));
        entry.identity=identity;memcpy(entry.title,title,bytes+1);entry.token=token;entry.canonicalSource=canonicalSource;
    }
    sCFrozen[slot].meaningKey=meaningKey;sModalCard=CFrozenCard;sModalOrdinal=slot;return true;
}
static bool CFrozenLabels(u16 slot,u32 expected,const char*& title,const char*& spanish)
{
    if(slot>=CFrozenLimit||!expected||sCFrozen[slot].identity!=expected){
        title="Ficha no disponible";spanish="Revisa tus colecciones";return true;}
    const auto& entry=sCFrozen[slot];title=entry.title;
    return InlineFrozenLabels(entry.meaningKey,entry.token,entry.canonicalSource,spanish);
}
static void CFrozenOpen(u16 slot,u32 expected)
{
    if(slot>=CFrozenLimit||!expected||sCFrozen[slot].identity!=expected)return;
    const auto& entry=sCFrozen[slot];
    InlineFrozenSavedOpen(entry.identity,entry.meaningKey,entry.token,entry.canonicalSource,entry.title,slot);
}

[[gnu::target("arm"),gnu::optimize("Os"),gnu::noinline]] static bool ToggleCurrentWord()
{
    if(!sCReady||sModalCard>65535)return false;
    if(sModalCard==CFrozenCard&&!CFrozenPrepareCurrent())return false;
    const u32 identity=MemoryWordKey(sModalCard,sModalOrdinal);
    // Inline cards are saved only after their verified stable sense is known.
    // The packed source/reference still occupies the original four bytes.
    // The contextual pair is exactly four serialized bytes, and the array is
    // explicitly word-aligned. may_alias permits bounded word loads/stores
    // without a memcpy call per entry in the IRQ.
    typedef u32 WordBits [[gnu::may_alias]];
    WordBits* words=(WordBits*)sSavedWords;
    const u32 reference=sModalCard|((u32)sModalOrdinal<<16);
    unsigned first=0;
    for(;first<sSavedCount;++first)
        if(words[first]==reference||(identity&&sSavedMemory[first]==identity))break;
    if(first==sSavedCount){
        // Imported unknown references remain removable. Only a new inline
        // modal requires a verified identity before adding another entry.
        if(sModalKind==4&&((sModalCard&0x8000)||sModalCard==CFrozenCard)&&!identity)return false;
        if(sSavedCount>=512)return false;
        sSavedMemory[sSavedCount]=identity;words[sSavedCount++]=reference;CWordChanged(true);return true;
    }
    unsigned kept=first;
    if(identity)for(unsigned i=first+1;i<sSavedCount;++i){
        // The same sense may have been saved from another sentence. Remove
        // every occurrence of that identity, keeping other senses and order.
        const u32 word=words[i],savedIdentity=sSavedMemory[i];
        if(word==reference||savedIdentity==identity)continue;
        words[kept]=word;sSavedMemory[kept++]=savedIdentity;
    }
    else for(unsigned i=first+1;i<sSavedCount;++i){
        const u32 word=words[i];if(word==reference)continue;
        words[kept]=word;sSavedMemory[kept++]=sSavedMemory[i];
    }
    sSavedCount=kept;CWordChanged(false);return true;
}
[[gnu::target("arm"),gnu::optimize("Os"),gnu::noinline]] static bool CToggleKanji(u16 doc)
{
    if(!sCReady||doc>=sKPages||!sKDocs[doc].kind)return false;
    u16* const end=sSavedKanjis+sSavedKanjiCount;
    u16* read=sSavedKanjis+CFindKanji(doc);
    // Explicit bounded batches keep the full 2400-entry legacy case within
    // the same IRQ window. No list is rescanned or shifted once per duplicate.
    if(read==end){
        if(sSavedKanjiCount>=2400)return false;
        *read=doc;++sSavedKanjiCount;if(doc==sKPage)sKSaveFlags=2;CChanged();return true;
    }
    u16* write=read++;
#define C_KEEP_KANJI() {const u16 value=*read++;if(value!=doc)*write++=value;}
    while(end-read>=16){
        C_KEEP_KANJI();C_KEEP_KANJI();C_KEEP_KANJI();C_KEEP_KANJI();
        C_KEEP_KANJI();C_KEEP_KANJI();C_KEEP_KANJI();C_KEEP_KANJI();
        C_KEEP_KANJI();C_KEEP_KANJI();C_KEEP_KANJI();C_KEEP_KANJI();
        C_KEEP_KANJI();C_KEEP_KANJI();C_KEEP_KANJI();C_KEEP_KANJI();
    }
    while(read!=end){C_KEEP_KANJI();}
#undef C_KEEP_KANJI
    sSavedKanjiCount=write-sSavedKanjis;if(doc==sKPage)sKSaveFlags=0;CChanged();return true;
}
static u8 CByte(unsigned pos)
{
    if(pos<20)return ((const u8*)&sCSnapshot)[pos];
    if(pos==24)return sCSnapshotFrozen;
    if(pos==25)return sCSnapshotFrozen>>8;
    if(pos==26)return 1;
    if(pos>=32&&pos<32u+sCSnapshot.words*4u)return ((const u8*)sSavedWords)[pos-32];
    if(pos>=2080&&pos<2080u+sCSnapshot.kanjis*2u)return ((const u8*)sSavedKanjis)[pos-2080];
    if(pos>=6880&&pos<6880u+sCSnapshot.words*4u)return ((const u8*)sSavedMemory)[pos-6880];
    if(pos>=CFrozenOffset&&pos<CBankBytes)return ((const u8*)sCFrozen)[pos-CFrozenOffset];
    return 0;
}
static void CSlice(unsigned start)
{
    if(!sCReady)return;
    if(!sCState&&sCDirty){CPrepareSnapshot();sCState=1;sCDirty=false;}
    if(sCState==1){
        for(unsigned n=0;n<64&&sCPosition<CBankBytes;++n){if((n&15)==0&&SliceExpired(start))return;const unsigned i=sCPosition++;const u8 b=CByte(i);sCSaveImage[i]=b;if(i<20||i>=24)sCHash=(sCHash^b)*16777619u;}
        if(sCPosition<CBankBytes)return;
        ((CHeader*)sCSaveImage)->hash=sCHash;sCWriteIndex=1;sCState=2;
    }
    if(SliceExpired(start))return;
    if(sCState==3){if(!fs_pollRead(&sCToken))return;
        if(!sCWriteIndex){sCDiskSlot=sCDestSlot;sCDiskGeneration=sCSnapshot.generation;sCState=0;if(!sModalKind)sHasShown=false;sKRepaint=true;return;}
        sCWriteIndex=sCWriteIndex==CBankSectors-1?0:sCWriteIndex+1;sCState=2;
    }
    if(sCState==2&&fs_tryWrite(sCDevice==0?FS_DEVICE_DLDI:FS_DEVICE_DSI_SD,sCSaveImage+sCWriteIndex*512,sCSectors[sCDestSlot*CBankSectors+sCWriteIndex],1,&sCToken))sCState=3;
}
static bool CWordRef(const SavedWord& word,u16& ref)
{
    if((word.card&0x8000)||word.card==CFrozenCard)return false; // packed inline references are not KS5 IDs
    if(word.ordinal&0x8000){ref=word.ordinal&0x7fff;return true;}
    if(word.ordinal>=sizeof(sKotoTapAnchors)/sizeof(sKotoTapAnchors[0]))return false;
    const auto& a=sKotoTapAnchors[word.ordinal];unsigned page=0,first=a.firstLine;
    if(a.card>=133&&a.card<=135){page=first;first=0;}
    ref=(page<<9)|first;return true;
}
static const char* CWordTitle(unsigned index)
{
    if(index>=sSavedCount)return "";const SavedWord& w=sSavedWords[index];u16 ref;
    if(w.card==CFrozenCard){const char* title;const char* spanish;
        return CFrozenLabels(w.ordinal,sSavedMemory[index],title,spanish)?title:"Cargando...";}
    if(w.card&0x8000){const char* title;const char* spanish;
        return InlineSavedLabels(w.card,w.ordinal,sSavedMemory[index],title,spanish)?title:"Cargando...";}
    const u8* card=FindStudy(w.card);if(!card||!CWordRef(w,ref))return "Palabra guardada";
    const unsigned page=(ref>>9)&63,first=ref&511;
    if(page>=Card16(card,12))return "Palabra guardada";
    const u32 pg=Read32(36)+8*(Card16(card,6)+page),refs=Read32(40)+Read32(24)*8;
    if(first>=Read16(pg+4))return "Palabra guardada";
    const u32 line=Read32(40)+8*Read16(refs+2*(Read32(pg)+first));return (const char*)sStudyData+Read32(line+4);
}
static void COpenWord(unsigned index)
{
    if(index>=sSavedCount)return;const SavedWord& word=sSavedWords[index];
    if(word.card==CFrozenCard){CFrozenOpen(word.ordinal,sSavedMemory[index]);return;}
    if(word.card&0x8000){InlineSavedOpen(word.card,word.ordinal,sSavedMemory[index]);return;}
    u16 ref;if(!CWordRef(word,ref))return;OpenTouchReference(ref,word.card);sModalOrdinal=word.ordinal;
}
static unsigned CListCount(unsigned tab){return tab==1?sSavedCount:sSavedKanjiCount;}
static const char* CListTitle(unsigned tab,unsigned index)
{if(tab==1)return CWordTitle(index);if(index>=sSavedKanjiCount||sSavedKanjis[index]>=sKPages)return "Ficha guardada";return sKDocs[sSavedKanjis[index]].title;}
static u32 CListGlyph(unsigned tab,unsigned index)
{return tab==2&&index<sSavedKanjiCount&&sSavedKanjis[index]<sKPages?sKDocs[sSavedKanjis[index]].glyph:0;}
// Resolve and compose one source line at a time to stay within the IRQ budget.
struct CLabel {char text[128];u16 used,width;u8 row;bool full;};
[[gnu::section(".ewram.bss")]] static CLabel sCJapanese,sCSpanish;
[[gnu::section(".ewram.bss")]] static u32 sCLabelFirst,sCLabelRefs;
[[gnu::section(".ewram.bss")]] static u16 sCLabelAt,sCLabelEnd,sCLabelTitleY;
// Only nearby visible rows are cached, as owned strings. A KL29 document can
// then be replaced for the next row without leaving a dangling list pointer.
// These labels are presentation excerpts, never replacements for full cards.
struct CInlineLabel {u32 reference,identity;CLabel japanese,spanish;bool valid;};
[[gnu::section(".ewram.bss")]] static CInlineLabel sCInlineLabels[6];
[[gnu::section(".ewram.bss")]] static u32 sCInlineReference,sCInlineIdentity;
[[gnu::section(".ewram.bss")]] static u8 sCInlineNext;
[[gnu::section(".ewram.bss")]] static bool sCInlinePending;
static_assert(sizeof(sCInlineLabels)<=1728,"Inline list labels stay bounded below 1.7 KiB");
static void CLabelAppend(CLabel& label,const char* text)
{
    while(*text&&!label.full){
        const char* begin=text;const u32 cp=DecodeUtf8(text);const unsigned width=cp>255?14:6;
        // Two lines per column. Reserve room for an ellipsis on the last line.
        if(label.width+width>(label.row?90u:108u)){
            if(label.row){memcpy(label.text+label.used,"...",4);label.full=true;return;}
            label.text[label.used++]='\n';label.row=1;label.width=0;
            if(cp==' ')continue;
        }
        const unsigned bytes=text-begin;
        if(label.used+bytes+4>=sizeof(label.text)){label.full=true;break;}
        memcpy(label.text+label.used,begin,bytes);label.used+=bytes;label.width+=width;
    }
    label.text[label.used]=0;
}
static void CBeginWordLabel(unsigned index)
{
    sCJapanese={};sCSpanish={};sCLabelAt=sCLabelEnd=0;sCInlinePending=false;
    if(index>=sSavedCount)return;
    const SavedWord& word=sSavedWords[index];
    if((word.card&0x8000)||word.card==CFrozenCard){
        sCInlineReference=word.card|((u32)word.ordinal<<16);sCInlineIdentity=sSavedMemory[index];
        for(const auto& cached:sCInlineLabels)if(cached.valid&&cached.reference==sCInlineReference&&cached.identity==sCInlineIdentity){
            sCJapanese=cached.japanese;sCSpanish=cached.spanish;return;}
        sCInlinePending=true;return;
    }
    u16 ref;const u8* card=FindStudy(word.card);
    if(!card||!CWordRef(word,ref)||((ref>>9)&63)>=Card16(card,12))return;
    const u32 pg=Read32(36)+8*(Card16(card,6)+((ref>>9)&63));
    sCLabelFirst=Read32(pg);sCLabelRefs=Read32(40)+Read32(24)*8;
    sCLabelAt=ref&511;sCLabelEnd=Read16(pg+4);
    if(sCLabelAt>=sCLabelEnd){sCLabelEnd=0;return;}
    const u32 line=Read32(40)+8*Read16(sCLabelRefs+2*(sCLabelFirst+sCLabelAt));
    sCLabelTitleY=Read16(line+2)+(sStudyData[line+1]==2?12:0);
}
static bool CWordLabelUnit()
{
    if(sCInlinePending){
        const char* japanese;const char* spanish;
        const bool frozen=(sCInlineReference&65535)==CFrozenCard;
        if(frozen?!CFrozenLabels(sCInlineReference>>16,sCInlineIdentity,japanese,spanish):
           !InlineSavedLabels(sCInlineReference&65535,sCInlineReference>>16,sCInlineIdentity,japanese,spanish))return false;
        CLabelAppend(sCJapanese,japanese);CLabelAppend(sCSpanish,spanish);
        auto& cached=sCInlineLabels[sCInlineNext];sCInlineNext=(sCInlineNext+1)%6;
        cached.reference=sCInlineReference;cached.identity=sCInlineIdentity;
        cached.japanese=sCJapanese;cached.spanish=sCSpanish;cached.valid=true;sCInlinePending=false;return true;
    }
    if(sCLabelAt>=sCLabelEnd)return true;
    const u32 line=Read32(40)+8*Read16(sCLabelRefs+2*(sCLabelFirst+sCLabelAt++));
    const unsigned style=sStudyData[line+1];
    if(style!=1&&Read16(line+2)>sCLabelTitleY)return true;
    const char* text=(const char*)sStudyData+Read32(line+4);
    if(style==0)CLabelAppend(sCJapanese,text); // Join split kanji + okurigana.
    else if(style==1){if(sCSpanish.used)CLabelAppend(sCSpanish," ");CLabelAppend(sCSpanish,text);}
    return sCJapanese.full&&sCSpanish.full;
}
static const char* CWordLabel(bool spanish)
{const CLabel& label=spanish?sCSpanish:sCJapanese;return label.used?label.text:spanish?"Sin traducción":"Palabra guardada";}
static bool CSaving(){return sCState||sCDirty;}
