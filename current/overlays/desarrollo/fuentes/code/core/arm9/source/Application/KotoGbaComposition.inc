// BPRJ1 EasyChat's constructor is an ordered grid, not a lexical substring
// search. Freeze its real word codes at entry and verify every completed byte
// against the cached ROM table-selected words before retaining any witness.
// Completed whole-string copies may carry that witness to another RAM buffer.
enum { KOTO_COMPOSITION_MAX_PARTS=16, KOTO_COMPOSITION_WITNESSES=8 };
struct KotoCompositionWitness {
    u32 dest,serial,hash,stack,array;
    u16 total,count,width,height;
    u8 ready,pending,pad[2];
    u16 words[KOTO_COMPOSITION_MAX_PARTS];
    KotoCompositionPart parts[KOTO_COMPOSITION_MAX_PARTS];
};
static_assert(sizeof(KotoCompositionWitness)==256);
struct KotoCompositionBinding {u32 serial;u16 witness,pad;};
struct KotoCompositionCopy {u32 serial;u16 witness,pad;};
[[gnu::section(".ewram.bss")]] static KotoCompositionWitness sCompositions[KOTO_COMPOSITION_WITNESSES];
[[gnu::section(".ewram.bss")]] static KotoCompositionBinding sCompositionBindings[KOTO_TEXT_SLOTS];
[[gnu::section(".ewram.bss")]] static KotoCompositionCopy sCompositionCopies[4];
[[gnu::section(".ewram.bss")]] static u32 sCompositionSerial;
[[gnu::section(".ewram.bss")]] static u8 sCompositionCursor;
// Experience template13 begins with FD00. Its selected bonus suffix330 is
// copied by the real StringAppend path inside that value, not identified by
// searching the final battle sentence for matching Japanese bytes.
struct KotoBattleComposition {
    u32 dest,context,stack;
    u16 prefixBytes,begin,end,pad;
    u8 prefix[64];
};
[[gnu::section(".ewram.bss")]] static KotoBattleComposition sBattleComposition;
#define KOTO_COMPOSITION_CODE [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]]

KOTO_COMPOSITION_CODE static void CompositionReset()
{
    memset(sCompositions,0,sizeof(sCompositions));
    memset(sCompositionBindings,0,sizeof(sCompositionBindings));
    memset(sCompositionCopies,0,sizeof(sCompositionCopies));
    sBattleComposition={};
    sCompositionCursor=0;
    // Do not recycle serials when windows reset: a retained reader snapshot
    // must never become authorized again merely because a buffer is reused.
}
KOTO_COMPOSITION_CODE static void CompositionInvalidate(u32 dest)
{
    bool changed=false;
    for(auto& e:sCompositions)if(e.ready&&dest>=e.dest&&dest-e.dest<e.total){e.ready=0;changed=true;}
    if(changed)++gKotoTextGeneration;
}
KOTO_COMPOSITION_CODE static unsigned CompositionReserve(u32 dest,unsigned avoid=KOTO_COMPOSITION_WITNESSES)
{
    for(unsigned i=0;i<KOTO_COMPOSITION_WITNESSES;++i)if(i!=avoid&&sCompositions[i].dest==dest)return i;
    unsigned index=sCompositionCursor++&(KOTO_COMPOSITION_WITNESSES-1);
    if(index==avoid)index=sCompositionCursor++&(KOTO_COMPOSITION_WITNESSES-1);
    if(sCompositions[index].ready)++gKotoTextGeneration;
    return index;
}
KOTO_COMPOSITION_CODE static bool CompositionCurrent(const KotoCompositionWitness& e)
{
    const u8* p=e.ready&&e.total&&e.total<=1024?Ram(e.dest,e.total):nullptr;
    return p&&p[e.total-1]==0xff&&TextHash(p,e.total)==e.hash;
}
// Preserve ascending witness order. Copy/bind require an exact current
// witness; literal retention also excludes a constructor still in progress.
KOTO_COMPOSITION_CODE static const KotoCompositionWitness* CompositionFind(u32 source,bool pending)
{
    const auto* e=sCompositions;
    for(unsigned n=KOTO_COMPOSITION_WITNESSES;n;--n,++e)
        if(e->dest==source&&((pending&&e->pending)||CompositionCurrent(*e)))return e;
    return nullptr;
}
KOTO_COMPOSITION_CODE static bool CompositionRead(u32 address,unsigned bytes,u32& value)
{
    value=0;
    for(unsigned i=0;i<bytes;++i){const u8* p=Cached(address+i);if(!p)return false;value|=u32(*p)<<(8*i);}
    return true;
}
KOTO_COMPOSITION_CODE static u32 CompositionWord(u16 word)
{
    const unsigned group=word>>9,index=word&511;
    // The game's lookup has two fixed-width name tables in addition to its
    // ordinary pointer/count table. These are actual source addresses too.
    if(group==0||group==21)return index<=411?0x081ff4d0u+6*index:0;
    if(group==18||group==19)return index<=354?0x081ffe78u+8*index:0;
    if(group>=22)return 0;
    u32 table,count,source;
    if(!CompositionRead(0x083af204u+8*group,4,table)||
       !CompositionRead(0x083af208u+8*group,2,count)||index>=count||
       table<0x08000000u||table>=0x0a000000u||
       !CompositionRead(table+12*index,4,source)||source<0x08000000u||source>=0x0a000000u)return 0;
    return source;
}
KOTO_COMPOSITION_CODE static bool CompositionFinish(KotoCompositionWitness& e,u32 last)
{
    const unsigned words=e.width*e.height;
    const u8* input=Ram(e.array,2*words);
    if(!input||memcmp(input,e.words,2*words)||last<e.dest||last-e.dest>=1024)return false;
    const unsigned total=last-e.dest+1;const u8* output=Ram(e.dest,total);
    if(!output||output[total-1]!=0xff)return false;
    unsigned at=0;e.count=0;
    for(unsigned i=0;i<words;++i){
        const unsigned row=i/e.width,column=i%e.width;const u16 word=e.words[i];
        if(word!=65535){
            const u32 source=CompositionWord(word);if(!source)return false;
            const unsigned begin=at;bool complete=false;
            for(unsigned j=0;j<96;++j){const u8* original=Cached(source+j);if(!original)return false;
                const u8 c=*original;if(c==0xff){complete=true;break;}
                // EasyChat values are literal words. Refuse expansion/control
                // atoms rather than inventing their source/display offsets.
                if(c>=0xf7||at+1>=total||output[at++]!=c)return false;
            }
            if(!complete||at==begin)return false;
            e.parts[e.count++]={source,(u16)begin,(u16)at,word,(u8)row,(u8)column};
            if(column+1<e.width){if(at+1>=total||output[at++]!=0)return false;}
        }
        if(column+1==e.width){
            const u8 separator=row+1==e.height?0xff:0xfe;
            if(at>=total||output[at++]!=separator)return false;
        }
    }
    if(at!=total||!e.count)return false;
    e.total=total;e.hash=TextHash(output,total);
    asm volatile("":::"memory");e.ready=1;return true;
}
KOTO_COMPOSITION_CODE static void CompositionObserve(const u32* regs,unsigned kind)
{
    if(kind==19){
        const u32 dest=regs[0];InvalidateDestination(dest);
        for(auto& old:sCompositions)if(old.pending&&old.stack==regs[15])old.pending=0;
        const unsigned width=regs[2]&65535,height=regs[3]&65535;
        if(!width||!height||width>KOTO_COMPOSITION_MAX_PARTS||height>KOTO_COMPOSITION_MAX_PARTS||
           width*height>KOTO_COMPOSITION_MAX_PARTS||!Ram(dest,1))return;
        const u8* input=Ram(regs[1],2*width*height);if(!input)return;
        auto& e=sCompositions[CompositionReserve(dest)];e.ready=e.pending=0;
        e.dest=dest;e.serial=++sCompositionSerial;if(!e.serial)e.serial=++sCompositionSerial;
        e.hash=0;e.stack=regs[15];e.array=regs[1];e.total=e.count=0;e.width=width;e.height=height;
        memcpy(e.words,input,2*width*height);asm volatile("":::"memory");e.pending=1;
    }else{
        for(auto& e:sCompositions)if(e.pending&&e.stack==regs[15]){
            e.pending=0;CompositionFinish(e,regs[4]);return;
        }
    }
}
KOTO_COMPOSITION_CODE static void CompositionCopyStart(unsigned slot,u32 source,u32 dest)
{
    auto& copy=sCompositionCopies[slot];copy.serial=0;
    if(source==dest)return;
    if(const auto* e=CompositionFind(source,false)){copy.witness=e-sCompositions;copy.serial=e->serial;}
}
KOTO_COMPOSITION_CODE static bool CompositionCopyPending(unsigned slot)
{return slot<4&&sCompositionCopies[slot].serial!=0;}
KOTO_COMPOSITION_CODE static void CompositionCopyDone(unsigned slot,u32 dest,unsigned total)
{
    const auto copy=sCompositionCopies[slot];sCompositionCopies[slot].serial=0;
    if(!copy.serial||copy.witness>=KOTO_COMPOSITION_WITNESSES)return;
    const auto& source=sCompositions[copy.witness];
    const u8* bytes=Ram(dest,total);
    if(source.serial!=copy.serial||!CompositionCurrent(source)||source.total!=total||!bytes||
       bytes[total-1]!=0xff||TextHash(bytes,total)!=source.hash)return;
    auto& target=sCompositions[CompositionReserve(dest,copy.witness)];target.ready=target.pending=0;
    target.dest=dest;target.serial=++sCompositionSerial;if(!target.serial)target.serial=++sCompositionSerial;
    target.hash=source.hash;target.stack=target.array=0;target.total=source.total;
    target.count=source.count;target.width=source.width;target.height=source.height;
    memcpy(target.words,source.words,sizeof(target.words));memcpy(target.parts,source.parts,sizeof(target.parts));
    asm volatile("":::"memory");target.ready=1;
}
KOTO_COMPOSITION_CODE static void CompositionBattleBegin(const Expansion& expansion)
{
    sBattleComposition={};
    if(expansion.origin!=0x083bd710u)return;
    sBattleComposition.dest=expansion.dest;sBattleComposition.context=expansion.context;
    sBattleComposition.stack=expansion.stack;
}
KOTO_COMPOSITION_CODE static void CompositionBattleVariable(const Expansion& expansion,unsigned token,u32 value)
{
    auto& pending=sBattleComposition;
    if(token||expansion.origin!=0x083bd710u||expansion.dest!=pending.dest||
       expansion.context!=pending.context||expansion.stack!=pending.stack)return;
    pending.prefixBytes=0;
    for(const auto& copy:sCopies){
        if(copy.origin!=0x083bd726u||copy.total!=8||copy.dest<value||copy.dest-value>56||
           !ProvenOutput(copy.dest,copy.total,copy.size,copy.snapshot,copy.hash))continue;
        const unsigned bytes=copy.dest-value+copy.total-1;
        const u8* prefix=Ram(value,bytes+1);if(!prefix||prefix[bytes]!=0xff)continue;
        // Before the copied suffix is only a literal nickname, not another
        // expansion, line break or transport control with different offsets.
        bool literal=true;for(unsigned i=0;i<copy.dest-value;++i)if(prefix[i]>=0xf7){literal=false;break;}
        if(!literal)continue;
        pending.begin=copy.dest-value;pending.end=bytes;
        memcpy(pending.prefix,prefix,bytes);pending.prefixBytes=bytes;return;
    }
}
KOTO_COMPOSITION_CODE static void CompositionBattleDone(const Expansion& expansion)
{
    const auto pending=sBattleComposition;sBattleComposition.prefixBytes=0;
    if(!pending.prefixBytes||expansion.origin!=0x083bd710u||expansion.dest!=pending.dest||
       expansion.context!=pending.context||expansion.stack!=pending.stack||
       !expansion.ready||expansion.ready<=pending.prefixBytes)return;
    const u8* output=Ram(expansion.dest,expansion.ready);
    if(!output||memcmp(output,pending.prefix,pending.prefixBytes)||output[expansion.ready-1]!=0xff)return;
    auto& e=sCompositions[CompositionReserve(expansion.dest)];e.ready=e.pending=0;
    e.dest=expansion.dest;e.serial=++sCompositionSerial;if(!e.serial)e.serial=++sCompositionSerial;
    e.stack=e.array=0;e.total=expansion.ready;e.count=1;e.width=1;e.height=1;
    for(unsigned i=0;i+1<e.total;++i)if(output[i]==0xfe)++e.height;
    e.parts[0]={0x083bd726u,pending.begin,pending.end,65535,0,0};
    e.hash=TextHash(output,e.total);asm volatile("":::"memory");e.ready=1;
}
KOTO_COMPOSITION_CODE static void CompositionBindRecord(unsigned slot)
{
    if(slot>=KOTO_TEXT_SLOTS)return;
    auto& binding=sCompositionBindings[slot];binding.serial=0;binding.pad=0;
    const auto& record=gKotoTextRecords[slot];
    if(const auto* e=CompositionFind(record.source,false)){
        binding.witness=e-sCompositions;asm volatile("":::"memory");binding.serial=e->serial;}
}
KOTO_COMPOSITION_CODE static bool CompositionAdvance(unsigned slot)
{
    if(slot>=KOTO_TEXT_SLOTS)return false;
    auto& binding=sCompositionBindings[slot];const auto& record=gKotoTextRecords[slot];
    if(!binding.serial||binding.witness>=KOTO_COMPOSITION_WITNESSES||!record.ready)return false;
    const auto& e=sCompositions[binding.witness];
    if(e.serial!=binding.serial||record.source!=e.dest||!e.ready)return false;
    unsigned printed=e.total;
    if(record.speed&&record.speed!=255){
        const u8* printer=Ram(0x02020030u+32*record.window,32);
        if(!printer||printer[4]!=record.window)return false;
        const u32 cursor=*(const u32*)printer;
        if(cursor<e.dest||cursor-e.dest>e.total)return false;printed=cursor-e.dest;
    }
    if(binding.pad==printed)return false;binding.pad=printed;return true;
}
KOTO_COMPOSITION_CODE static bool CompositionBound(const KotoTextRecord& record)
{
    if(!gKotoTextRecords||!record.sequence)return false;
    const u32 offset=(u32)&record-(u32)gKotoTextRecords;
    if(offset>=KOTO_TEXT_SLOTS*sizeof(KotoTextRecord)||offset%sizeof(KotoTextRecord))return false;
    const auto& binding=sCompositionBindings[offset/sizeof(KotoTextRecord)];
    if(!binding.serial||binding.witness>=KOTO_COMPOSITION_WITNESSES)return false;
    const auto& e=sCompositions[binding.witness];
    return binding.serial==e.serial&&record.source==e.dest&&CompositionCurrent(e);
}
KOTO_COMPOSITION_CODE static const KotoCompositionWitness* CompositionRecord(const KotoTextRecord* record,unsigned& first,unsigned& end,unsigned& printed)
{
    if(!record||!gKotoTextRecords)return nullptr;
    const u32 offset=(u32)record-(u32)gKotoTextRecords;
    if(offset>=KOTO_TEXT_SLOTS*sizeof(KotoTextRecord)||offset%sizeof(KotoTextRecord))return nullptr;
    const auto& binding=sCompositionBindings[offset/sizeof(KotoTextRecord)];
    if(!binding.serial||binding.witness>=KOTO_COMPOSITION_WITNESSES||!record->ready||!record->sequence||
       !record->size||record->size>KOTO_TEXT_BYTES||record->window>=32)return nullptr;
    const auto& e=sCompositions[binding.witness];
    if(binding.serial!=e.serial||record->source!=e.dest||!CompositionCurrent(e))return nullptr;
    first=0;printed=e.total;
    if(record->speed&&record->speed!=255){
        // This frontier is frozen by the actual CopyWindowToVram publication
        // hook. A live guest printer pointer cannot reveal unpresented text.
        printed=binding.pad;if(printed>e.total)return nullptr;
        const u8* window=Ram(0x02020430u+12*record->window,12);
        unsigned rows=window?window[4]*8/14:2;if(!rows)rows=1;
        const u8* bytes=Ram(e.dest,e.total);unsigned observedRows=1;
        for(unsigned i=0;i<printed&&i+1<e.total;++i)if(bytes[i]==0xfe)++observedRows;
        if(observedRows>rows){unsigned remove=observedRows-rows;
            for(unsigned i=0;i<printed&&i+1<e.total&&remove;++i)if(bytes[i]==0xfe){first=i+1;--remove;}
            if(remove)return nullptr;
        }
    }
    unsigned compare=record->size;
    // RefreshPrintedFragment adds a synthetic EOS if its local240-byte
    // snapshot clips. It cannot authorize a partially captured source piece.
    if(compare&&record->bytes[compare-1]==0xff&&first+compare<e.total)--compare;
    if(first+compare>e.total||memcmp(record->bytes,Ram(e.dest+first,compare),compare))return nullptr;
    end=first+compare;
    return &e;
}
extern "C" KOTO_COMPOSITION_CODE u32 kotogba_compositionVersion(const KotoTextRecord* record)
{
    unsigned first,end,printed;const auto* e=CompositionRecord(record,first,end,printed);return e?e->serial:0;
}
extern "C" KOTO_COMPOSITION_CODE unsigned kotogba_compositionVisibleBytes(const KotoTextRecord* record)
{
    unsigned first,end,printed;const auto* e=CompositionRecord(record,first,end,printed);if(!e)return 0;
    const unsigned frontier=printed<end?printed:end;
    if(frontier<=first)return 0;
    unsigned bytes=frontier-first;
    // A synthetic EOS terminates only this already published view. Including
    // it makes the consumer's normalizer atom-complete without future text.
    if(bytes+1==record->size&&record->bytes[bytes]==0xff)++bytes;
    return bytes;
}
extern "C" KOTO_COMPOSITION_CODE bool kotogba_compositionComplete(const KotoTextRecord* record)
{
    unsigned first,end,printed;const auto* e=CompositionRecord(record,first,end,printed);
    return e&&printed+1>=e->total;
}
extern "C" KOTO_COMPOSITION_CODE unsigned kotogba_compositionCount(const KotoTextRecord* record)
{
    unsigned first,end,printed;const auto* e=CompositionRecord(record,first,end,printed);if(!e)return 0;
    unsigned count=0;for(unsigned i=0;i<e->count;++i){const auto& part=e->parts[i];
        if(part.rawStart>=first&&part.rawEnd<=end&&part.rawEnd<=printed)++count;}
    return count;
}
extern "C" KOTO_COMPOSITION_CODE bool kotogba_compositionPart(const KotoTextRecord* record,unsigned index,KotoCompositionPart* part)
{
    if(!part)return false;
    unsigned first,end,printed;const auto* e=CompositionRecord(record,first,end,printed);if(!e)return false;
    for(unsigned i=0;i<e->count;++i){const auto& item=e->parts[i];
        if(item.rawStart<first||item.rawEnd>end||item.rawEnd>printed)continue;
        if(index--==0){*part=item;part->rawStart-=first;part->rawEnd-=first;return true;}}
    return false;
}
extern "C" KOTO_COMPOSITION_CODE bool kotogba_compositionAuthorize(const KotoTextRecord* record,u32 version,unsigned index,const KotoCompositionPart* part)
{
    KotoCompositionPart current;
    return part&&version&&kotogba_compositionVersion(record)==version&&
        kotogba_compositionPart(record,index,&current)&&!memcmp(part,&current,sizeof(current));
}
#undef KOTO_COMPOSITION_CODE
