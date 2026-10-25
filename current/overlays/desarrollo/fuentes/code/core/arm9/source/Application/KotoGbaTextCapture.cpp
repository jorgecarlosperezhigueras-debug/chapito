#include "common.h"
#include "KotoGbaTextCapture.h"
#include "KotoGbaEntityProofBprj1.h"
#include "KotoGbaEntityProofBprj1.inc"
#include "Patches/PatchSwi.h"
#include "SdCache/SdCache.h"
#include <string.h>

extern "C" volatile u32 gKotoGbaRuntimeEnabled;

// No file access, allocation or cache replacement from an observation hook.
// Direct ROM strings are copied only once the game's own reads cached them.
[[gnu::section(".ewram.bss")]] KotoTextRecord* gKotoTextRecords;
[[gnu::section(".ewram.bss")]] unsigned gKotoTextCapacity;
[[gnu::section(".ewram.bss")]] volatile u32 gKotoTextGeneration,gKotoTextOverflow,gKotoTextCaptureInstalled;
[[gnu::section(".ewram.bss")]] static void* sTextScratch;
[[gnu::section(".ewram.bss")]] u32* gKotoTextFragmentMasks;
[[gnu::section(".ewram.bss")]] volatile u32 gKotoTextWindowMask;
[[gnu::section(".ewram.bss")]] volatile u8 gKotoTextOptionsWindow,gKotoTextArrowSlot;
[[gnu::section(".ewram.bss")]] u8 gKotoTextLast[32],gKotoTextAsync[32];
struct Expansion {u32 dest,origin,stack,context,hash;u16 size,ready;u8 snapshot[96];};
[[gnu::section(".ewram.bss")]] static Expansion sExpansion[4];
struct CopyOrigin {u32 dest,origin,stack,context,hash;u16 size,total;u8 snapshot[96];};
[[gnu::section(".ewram.bss")]] static CopyOrigin sCopies[4];
[[gnu::section(".ewram.bss")]] static u8 sCopyCursor;
struct TextVariable {u32 origin,source,context;u16 token,pad;u8 snapshot[64];};
[[gnu::section(".ewram.bss")]] static TextVariable sTextVariables[16];
[[gnu::section(".ewram.bss")]] static u8 sTextVariableCursor;
[[gnu::section(".ewram.bss")]] static u32 sTextSerial,sLastOriginContext;
// FillWindowPixelRect edits the CPU buffer. A published literal remains on
// screen until CopyWindowToVram commits that buffer or its window is removed.
[[gnu::section(".ewram.bss")]] static u32 sTextPendingClear[2];
[[gnu::section(".ewram.bss")]] static Expansion sBattleExpansion,sDynamicExpansion;
[[gnu::section(".ewram.bss")]] static u32 sDynamicInput;
struct RecapAppend {u32 dest,stack,context;u16 size;u8 slot,digit;};
[[gnu::section(".ewram.bss")]] static RecapAppend sRecapAppend;
extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] u32 kotogba_variableSource(u32 origin,unsigned token,u32 context)
{
    for(const auto& v:sTextVariables)if(v.token==token&&v.context==context&&(context||v.origin==origin))return v.source;
    return 0;
}
extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] u32 kotogba_variableOrigin(u32 origin,unsigned token,u32 context)
{
    for(const auto& v:sTextVariables)if(v.source&&v.token==token&&v.context==context&&(context||v.origin==origin))return KotoEntitySource(v.pad);
    return 0;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static u8* Ram(u32 p,unsigned size)
{
    if(p>=0x02000000&&p<0x02040000&&size<=0x02040000-p)return (u8*)p;
    if(p>=0x03000000&&p<0x03008000&&size<=0x03008000-p)return (u8*)p;
    return nullptr;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static const u8* Cached(u32 p)
{
    if(const u8* ram=Ram(p,1))return ram;
    if(p>=0x02200000&&p<0x02400000)return (const u8*)p;
    if(p<0x08000000||p>=0x0a000000)return nullptr;
    const u32 off=p-0x08000000;
    if(off<0x200000)return (const u8*)(0x02200000+off);
    const u8* block=(const u8*)sdc_romBlockToCacheBlock[off>>SDC_BLOCK_SHIFT];
    return block?block+(off&SDC_BLOCK_MASK):nullptr;
}
static unsigned CaptureAtom(u32 source,unsigned offset,unsigned limit,u8& c);
static u32 Origin(u32 p);
static void CompositionInvalidate(u32 dest);
static void CompositionReset();
static void CompositionCopyStart(unsigned slot,u32 source,u32 dest);
static bool CompositionCopyPending(unsigned slot);
static void CompositionCopyDone(unsigned slot,u32 dest,unsigned total);
static void CompositionBindRecord(unsigned slot);
static bool CompositionAdvance(unsigned slot);
static void CompositionObserve(const u32* regs,unsigned kind);
// Only the frozen number actually appended by DrawPreviouslyOnQuestHeader.
// This is display metadata, never a replacement Japanese/Spanish sentence.
extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] unsigned kotogba_recapNumber(const KotoTextRecord* r)
{
    if(!r||r->origin!=0x083dbf5a||r->size!=13||r->bytes[12]!=0xff||
       r->bytes[11]<0xa1||r->bytes[11]>0xaa)return 0;
    return '0'+r->bytes[11]-0xa1;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static void BindVariable(u32 origin,unsigned token,u32 source,u32 context)
{
    unsigned slot=sTextVariableCursor++&15;
    for(unsigned i=0;i<16;++i)if(sTextVariables[i].origin==origin&&sTextVariables[i].token==token&&sTextVariables[i].context==context){slot=i;break;}
    auto& value=sTextVariables[slot];value.origin=origin;value.token=token;value.context=context;value.source=0;value.pad=0;
    // A NULL dynamic placeholder is deliberately omitted by the game. Keep
    // an explicit empty snapshot rather than confusing it with missing data.
    if(!source){value.snapshot[0]=0xff;value.source=(u32)value.snapshot;return;}
    for(unsigned i=0;i<64;){u8 c;const unsigned unit=CaptureAtom(source,i,64,c);if(!unit)return;
        for(unsigned j=0;j<unit;++j)value.snapshot[i+j]=*Cached(source+i+j);
        i+=unit;if(c==0xff){value.pad=KotoEntityProof(Origin(source));value.source=(u32)value.snapshot;return;}}
}
// Provenance covers the entire bounded output, including text beyond the
// 96-byte visible-fragment snapshot. A reused buffer with a changed suffix
// must not retain the previous source identity merely because its prefix fits.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static u32 TextHash(const u8* bytes,unsigned size)
{
    u32 value=2166136261u;for(unsigned i=0;i<size;++i)value=(value^bytes[i])*16777619u;return value;
}
[[gnu::section(".ewram")]] static const u8 sCaptureControlSizes[]={0,1,1,1,3,1,1,0,1,0,0,2,1,1,1,0,2,1,1,1,1,0,0,0,0};
// Every constructor invalidates previous proofs for its output address, even
// if this input has no provable origin. Identical bytes are not source identity.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static void InvalidateDestination(u32 dest,bool keepDynamic=false)
{
    CompositionInvalidate(dest);
    for(auto& e:sExpansion)if(e.dest==dest){e.ready=0;e.origin=0;}
    for(auto& e:sCopies)if(e.dest==dest){e.total=0;e.origin=0;}
    if(sBattleExpansion.dest==dest){sBattleExpansion.ready=0;sBattleExpansion.origin=0;}
    if(sDynamicExpansion.dest==dest&&!keepDynamic){sDynamicExpansion.ready=0;sDynamicExpansion.origin=0;}
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static bool ProvenOutput(u32 dest,unsigned total,unsigned size,const u8* snapshot,u32 hash)
{
    const u8* bytes=Ram(dest,total);
    return total&&bytes&&bytes[total-1]==0xff&&!memcmp(bytes,snapshot,size)&&TextHash(bytes,total)==hash;
}
// Expanded byte offsets differ from the ROM template. Map only actual box
// boundaries, and reject a variable which itself introduces a box boundary.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static u32 FragmentOrigin(u32 origin,u32 context,u32 dest,u32 p,unsigned total)
{
    if(p==dest)return origin;
    unsigned boundary=0;
    for(unsigned i=0;i<total;){u8 c;const unsigned unit=CaptureAtom(dest,i,total,c);if(!unit)return 0;i+=unit;
        if(c==0xfa||c==0xfb){++boundary;if(dest+i==p)break;}
        if(dest+i>=p||c==0xff)return 0;
    }
    if(!boundary)return 0;
    for(unsigned i=0;i<1024;){u8 c;const unsigned unit=CaptureAtom(origin,i,1024,c);if(!unit)return 0;
        if(c==0xfd||c==0xf7){const u8* token=Cached(origin+i+1);if(!token)return 0;
            const u32 value=kotogba_variableSource(origin,c==0xf7?768+*token:*token,context);if(!value)return 0;
            // Snapshots are native EWRAM, not guest RAM/cache addresses.
            const u8* raw=(const u8*)value;bool complete=false;
            for(unsigned n=0;n<64;){const u8 atom=raw[n];unsigned width=1;
                if(atom==0xfa||atom==0xfb||atom==0xfd||atom==0xf7)return 0;
                if(atom==0xff){complete=true;break;}
                if(atom==0xfc){if(n+1>=64)return 0;const u8 command=raw[n+1];
                    width=2+(command<25?sCaptureControlSizes[command]:0);}
                else if(atom==0xf7||atom==0xf8||atom==0xf9||atom==0xfd)width=2;
                if(width>64-n)return 0;n+=width;}
            if(!complete)return 0;
        }
        i+=unit;
        if(c==0xfa||c==0xfb){if(!--boundary)return origin+i;}
        if(c==0xff)return 0;
    }
    return 0;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static u32 ExpansionOrigin(const Expansion& e,u32 p)
{
    if(!e.ready||p<e.dest||p-e.dest>=e.ready||!ProvenOutput(e.dest,e.ready,e.size,e.snapshot,e.hash))return 0;
    return FragmentOrigin(e.origin,e.context,e.dest,p,e.ready);
}
// StringAppend finishes in StringCopy. Coloured move/item captions therefore
// have a proven copied name immediately after their control-only prefix. The
// prefix may change colour, but must not add any visible glyph or placeholder.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static bool ControlPrefix(u32 p,u32 end)
{
    if(p>=end||end-p>32||!Ram(p,end-p))return false;
    for(unsigned i=0;i<end-p;){u8 ch;const unsigned unit=CaptureAtom(p,i,end-p,ch);
        if(!unit||ch!=0xfc)return false;i+=unit;}
    return true;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static u32 Origin(u32 p)
{
    sLastOriginContext=0;
    if(p>=0x02200000&&p<0x02400000)return p-0x02200000+0x08000000;
    if(p>=0x08000000&&p<0x0a000000)return p;
    u32 origin=ExpansionOrigin(sDynamicExpansion,p);if(origin){sLastOriginContext=sDynamicExpansion.context;return origin;}
    origin=ExpansionOrigin(sBattleExpansion,p);if(origin){sLastOriginContext=sBattleExpansion.context;return origin;}
    for(const auto& e:sExpansion)if((origin=ExpansionOrigin(e,p))){sLastOriginContext=e.context;return origin;}
    for(const auto& e:sCopies)if(e.origin&&e.total&&((p>=e.dest&&p-e.dest<e.total)||ControlPrefix(p,e.dest))&&ProvenOutput(e.dest,e.total,e.size,e.snapshot,e.hash)){
        const u32 at=p<e.dest?e.dest:p;
        const u32 copyOrigin=e.context?FragmentOrigin(e.origin,e.context,e.dest,at,e.total):e.origin+at-e.dest;
        if(copyOrigin){sLastOriginContext=e.context;return copyOrigin;}}
    return 0;
}
// FC18 (RESUME_MUSIC) has no operand; treating it as one consumes the
// next visible character. These are the BPRJ1 printer's control widths.
// Read one complete printer atom without loading or replacing ROM cache blocks.
// Its operands may equal FA/FB/FF; only the atom's first byte is a delimiter.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static unsigned CaptureAtom(u32 source,unsigned offset,unsigned limit,u8& c)
{
    if(offset>=limit)return 0;
    const u8* p=Cached(source+offset);if(!p)return 0;c=*p;unsigned unit=1;
    if(c==0xfc){
        if(limit-offset<2)return 0;
        const u8* command=Cached(source+offset+1);if(!command)return 0;
        unit=2+(*command<25?sCaptureControlSizes[*command]:0);
    }else if(c==0xf7||c==0xf8||c==0xf9||c==0xfd)unit=2;
    if(unit>limit-offset)return 0;
    for(unsigned i=1;i<unit;++i)if(!Cached(source+offset+i))return 0;
    return unit;
}
#include "KotoGbaComposition.inc"
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static bool PublishedLiteral(const KotoTextRecord& r)
{
    // Only physical records reach this helper. A previous composition binding
    // remains an exclusion after its witness changes serial: an identical new
    // constructor still needs the ordinary Add/rebind and VRAM publication.
    if(!r.ready||!r.sequence||(r.speed&&r.speed!=255)||!r.size||r.size>KOTO_TEXT_BYTES||
       r.bytes[r.size-1]!=0xff||sCompositionBindings[&r-gKotoTextRecords].serial)return false;
    for(unsigned i=0;i+1<r.size;++i)if(r.bytes[i]>=0xf7)return false;
    // A plain previous capture may also acquire a current or pending source
    // constructor. Byte equality alone cannot transfer that ordered identity.
    return !CompositionFind(r.source,true);
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static bool SamePublishedLiteral(const KotoTextRecord& r,const u8* t,unsigned speed,u32 origin)
{
    if(!PublishedLiteral(r)||r.source!=*(const u32*)t||r.origin!=origin||r.context!=sLastOriginContext||
       r.window!=t[4]||r.font!=t[5]||r.x!=t[6]||r.y!=t[7]||r.speed!=speed||
       r.tiles!=*(const u32*)(0x02020438+12*r.window))return false;
    for(unsigned i=0;i<r.size;++i){const u8* c=Cached(r.source+i);if(!c||*c!=r.bytes[i])return false;}
    return r.cursor==kotogba_captureCursor(&r,r.source+r.size-1);
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static void CopyText(KotoTextRecord& r)
{
    unsigned n=0;
    while(n<KOTO_TEXT_BYTES){u8 c;const unsigned unit=CaptureAtom(r.source,n,KOTO_TEXT_BYTES,c);if(!unit)break;
        // Delimiter-valued operands are data, not text-box boundaries. Never
        // publish half a control/placeholder if its bytes are not all cached
        // or the fixed-size observation record cannot contain the whole unit.
        for(unsigned i=0;i<unit;++i)r.bytes[n+i]=*Cached(r.source+n+i);
        n+=unit;
        if(c==0xff||c==0xfa||c==0xfb)break;
    }
    r.size=n;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static bool RefreshPrintedFragment(KotoTextRecord& r)
{
    if(!r.speed||r.speed==255)return false;
    const u8* printer=Ram(0x02020030+32*r.window,32);if(!printer||printer[4]!=r.window)return false;
    const u32 cursor=*(const u32*)printer;if(cursor<r.source||cursor-r.source>1024)return false;
    const unsigned printed=cursor-r.source;
    const bool composition=CompositionBound(r);
    unsigned end=0;
    // Walk from the source so that a delimiter-valued operand just before the
    // cursor cannot be mistaken for a finished box. A printer stopped just
    // after FA/FB still owns the preceding fragment until it advances again.
    // RAM, linear ROM and already cached GBA ROM follow the same bounded path.
    while(end<1024){if(composition&&end>=printed)break;
        u8 c;const unsigned unit=CaptureAtom(r.source,end,1024,c);if(!unit)return false;
        if(composition&&unit>printed-end)break;
        if(c==0xff){if(printed>end+1)return false;break;}
        if((c==0xfa||c==0xfb)&&end+1>=printed)break;
        end+=unit;
    }
    if(end==1024)return false;
    const u8* window=Ram(0x02020430+12*r.window,12);unsigned maxRows=window?window[4]*8/14:2;if(!maxRows)maxRows=1;
    unsigned first=0,rows=1,fragment=0;
    for(unsigned i=0;i<end;){u8 c;const unsigned unit=CaptureAtom(r.source,i,end,c);if(!unit)return false;
        i+=unit;
        if(c==0xfb){first=i;rows=1;++fragment;continue;}
        if(c==0xfa||c==0xfe){if(c==0xfa)++fragment;++rows;
            if(rows>maxRows){while(first<i){u8 old;const unsigned oldUnit=CaptureAtom(r.source,first,i,old);if(!oldUnit)return false;
                    first+=oldUnit;if(old==0xfe||old==0xfa||old==0xfb)break;}--rows;}
        }
    }
    u8 snapshot[KOTO_TEXT_BYTES];unsigned n=0;
    for(unsigned i=first;i<end&&n+1<KOTO_TEXT_BYTES;){u8 c;const unsigned unit=CaptureAtom(r.source,i,end,c);if(!unit)return false;
        if(c==0xfc||c==0xf8||c==0xf9){i+=unit;continue;}
        if(unit>KOTO_TEXT_BYTES-n-1)break;
        if(c==0xfa||c==0xfb)snapshot[n++]=0xfe;
        else for(unsigned j=0;j<unit;++j)snapshot[n++]=*Cached(r.source+i+j);
        i+=unit;
    }
    snapshot[n++]=0xff;
    // A printer may advance within an unchanged retained box. Its proven
    // original cursor is part of the publication: otherwise the reader keeps
    // the first partial kana unit forever while the guest finishes the word.
    const u32 originalCursor=kotogba_captureCursor(&r,cursor);
    bool changed=r.size!=n||memcmp(r.bytes,snapshot,n)||r.reserved!=(fragment>255?255:fragment)||
        (originalCursor&&originalCursor!=r.cursor);
    if(changed||!r.cursor){
        // The bridge permits nested IRQs. Withdraw an already published record
        // until its cursor, fragment and bytes are consistent again. Barriers
        // keep the compiler from moving snapshot stores outside this guard.
        const u8 wasReady=r.ready;r.ready=0;asm volatile("":::"memory");
        r.cursor=originalCursor;r.reserved=fragment>255?255:fragment;memcpy(r.bytes,snapshot,n);r.size=n;
        asm volatile("":::"memory");r.ready=wasReady;
    }
    // A full sentence cannot reveal the tail of the last typewriter box.
    // Count it only after the guest cursor has printed through its delimiter.
    if(gKotoTextFragmentMasks&&fragment<32&&printed>=end&&n>1&&r.origin){
        u32& mask=gKotoTextFragmentMasks[(&r-gKotoTextRecords)*KOTO_TEXT_FRAGMENT_MASK_STRIDE];
        const u32 bit=1u<<fragment;if(!(mask&bit)){mask|=bit;changed=true;}
    }
    return changed;
}
#include "KotoGbaHelpCapture.inc"
extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] void kotogba_resetTextCapture()
{
    if(gKotoTextFragmentMasks)for(unsigned i=0;i<gKotoTextCapacity;++i)gKotoTextFragmentMasks[i*KOTO_TEXT_FRAGMENT_MASK_STRIDE]=0;
    if(gKotoTextRecords)memset(gKotoTextRecords,0,gKotoTextCapacity*sizeof(KotoTextRecord));memset(sExpansion,0,sizeof(sExpansion));memset(sCopies,0,sizeof(sCopies));memset(sTextVariables,0,sizeof(sTextVariables));sBattleExpansion={};sDynamicExpansion={};sDynamicInput=0;sRecapAppend={};sCopyCursor=sTextVariableCursor=0;
    CompositionReset();sTextPendingClear[0]=sTextPendingClear[1]=0;gKotoHelpArrowSlot=sHelpPendingSlot=255;gKotoTextGeneration=0;gKotoTextOverflow=0;gKotoTextWindowMask=0;gKotoTextOptionsWindow=gKotoTextArrowSlot=255;memset(gKotoTextLast,255,sizeof(gKotoTextLast));memset(gKotoTextAsync,255,sizeof(gKotoTextAsync));
}
// Called with all virtual registers saved and a separate native SVC stack.
// ready/sequence are committed last; the IRQ reader never sees a half record.
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static void ObserveRecapAppendBegin(const u32* regs,unsigned kind,u32 caller)
{
        sRecapAppend={};
        // BPRJ1's header expands first, then appends its decimal scene index.
        // Preserve the completed parent only for this actual caller, exact
        // destination/EOS and one frozen digit. Arbitrary appends still fail.
        // StringAppend itself calls StringCopy: its saved LR is one word
        // above StringCopy's saved LR on the real guest stack.
        const u32* parent=(const u32*)Ram(regs[15]+4,4);
        if(kind==8&&caller==0x080088a1&&parent&&(*parent==0x08111c23||*parent==0x02311c23)&&regs[1]==0x02021c4c){
            const u8* number=Ram(regs[1],2);
            if(number&&number[0]>=0xa1&&number[0]<=0xaa&&number[1]==0xff)
                for(unsigned i=0;i<4;++i){const auto& e=sExpansion[i];
                    if(e.origin==0x083dbf5a&&e.dest==0x02021c88&&e.ready==12&&regs[0]==e.dest+11&&
                       ProvenOutput(e.dest,e.ready,e.size,e.snapshot,e.hash))
                        sRecapAppend={e.dest,regs[15],e.context,e.ready,(u8)i,number[0]};
                }
        }
}

[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static void ObserveRecapAppendDone(const u32* regs)
{
        auto& pending=sRecapAppend;
        if(pending.dest&&pending.stack==regs[15]){
            auto& e=sExpansion[pending.slot];const unsigned size=pending.size+1;
            const u8* bytes=Ram(pending.dest,size);
            if(e.origin==0x083dbf5a&&e.dest==pending.dest&&e.context==pending.context&&
               regs[3]==pending.dest+size-1&&bytes&&bytes[size-1]==0xff&&
               bytes[size-2]==pending.digit&&!memcmp(bytes,e.snapshot,pending.size-1)){
                e.size=e.ready=size;memcpy(e.snapshot,bytes,size);e.hash=TextHash(bytes,size);
            }
            pending={};
        }
}

[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static void ObserveSpeciesName(const u32* regs)
{
        // Actual GetSpeciesName byte-loop completion; no StringCopy call
        // exists on this JP path. Validate the whole table entry and result,
        // then reuse an ordinary copy witness. Personal nicknames cannot
        // acquire this identity by merely sharing its printed glyphs.
        const u32 dest=regs[6],index=regs[4];InvalidateDestination(dest);
        if(index>=412||regs[1]>5)return;
        const unsigned size=regs[1]+1;const u32 source=0x081ff4d0+6*index;
        const u8* bytes=Ram(dest,size);if(!bytes||bytes[size-1]!=0xff)return;
        for(unsigned i=0;i<size;++i){const u8* original=Cached(source+i);if(!original||*original!=bytes[i])return;}
        unsigned slot=sCopyCursor++&3;for(unsigned i=0;i<4;++i)if(sCopies[i].dest==dest){slot=i;break;}
        auto& copy=sCopies[slot];copy.dest=dest;copy.origin=source;copy.context=0;copy.stack=regs[15];copy.size=copy.total=size;
        memcpy(copy.snapshot,bytes,size);copy.hash=TextHash(bytes,size);return;
}

[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] static void RetainDynamicValue(u32 dest)
{
        // Sold-item quantities first expand {F7:04}こ into StringVar3,
        // then insert that completed value into a second F7 construction.
        // Retain its whole-output witness in an existing copy slot, not a
        // second unbounded expansion or a value inferred from printed kana.
        const auto& previous=sDynamicExpansion;
        if(previous.dest!=dest&&previous.origin&&previous.ready&&
           ProvenOutput(previous.dest,previous.ready,previous.size,previous.snapshot,previous.hash)){
            auto& copy=sCopies[sCopyCursor++&3];copy.dest=previous.dest;copy.origin=previous.origin;
            copy.stack=previous.stack;copy.context=previous.context;copy.hash=previous.hash;
            copy.size=previous.size;copy.total=previous.ready;memcpy(copy.snapshot,previous.snapshot,copy.size);
        }
}

extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::section(".ewram.text"),gnu::noinline]] void kotogba_observeText(const u32* regs,unsigned kind)
{
    if(!gKotoGbaRuntimeEnabled||!gKotoTextRecords)return;
    if(kind>=21&&kind<=27){HelpObserve(regs,kind);return;}
    if(kind==19||kind==20){CompositionObserve(regs,kind);return;}
    if(kind==28){ObserveSpeciesName(regs);return;}
    // DynamicPlaceholderTextUtil_ExpandPlaceholders uses F7 + slot (0..7),
    // independently of StringExpand's FD tokens. Observe the real pointer at
    // the game's lookup, before it copies or deliberately omits that value.
    if(kind==13){
        const u32 source=Origin(regs[1]);const u32 context=sLastOriginContext;
        RetainDynamicValue(regs[0]);
        InvalidateDestination(regs[0]);sDynamicExpansion={};
        if(source&&Ram(regs[0],1)){auto& e=sDynamicExpansion;e.dest=regs[0];e.origin=source;e.stack=regs[15];e.context=context?context:++sTextSerial;sDynamicInput=regs[1];}
        return;
    }
    if(kind==14){
        const u8* marker=Cached(regs[4]-1);const u8* token=Cached(regs[4]);
        if(sDynamicExpansion.origin&&sDynamicExpansion.stack==regs[15]&&marker&&token&&*marker==0xf7&&*token<8&&regs[4]>=sDynamicInput&&regs[4]-sDynamicInput<1024)
            BindVariable(sDynamicExpansion.origin,768+*token,regs[1],sDynamicExpansion.context);
        return;
    }
    if(kind==15){
        auto& e=sDynamicExpansion;const unsigned total=regs[2]-e.dest+1;
        if(e.origin&&e.stack==regs[15]&&regs[2]>=e.dest&&total&&total<=1024)
            if(const u8* bytes=Ram(e.dest,total))if(bytes[total-1]==0xff){e.size=total>96?96:total;memcpy(e.snapshot,bytes,e.size);e.hash=TextHash(bytes,total);e.ready=total;}
        return;
    }
    if(kind==10){
        const u32 source=Origin(regs[0]);const u32 context=sLastOriginContext;InvalidateDestination(regs[1]);sBattleExpansion={};
        if(source&&Ram(regs[1],1)){sBattleExpansion.dest=regs[1];sBattleExpansion.origin=source;sBattleExpansion.stack=regs[15];sBattleExpansion.context=context?context:++sTextSerial;
            // Until completion, hash holds the actual input pointer. A copied
            // RAM template keeps its ROM identity but its FD cursor is in RAM.
            // kind12 replaces this temporary value with the full-output hash.
            sBattleExpansion.hash=regs[0];}
        CompositionBattleBegin(sBattleExpansion);
        return;
    }
    if(kind==11){
        const u8* marker=Cached(regs[9]-1);const u8* token=Cached(regs[9]);
        if(sBattleExpansion.origin&&!sBattleExpansion.ready&&sBattleExpansion.stack==regs[15]&&marker&&token&&*marker==0xfd&&regs[9]>=sBattleExpansion.hash&&regs[9]-sBattleExpansion.hash<1024){
            BindVariable(sBattleExpansion.origin,*token,regs[4],sBattleExpansion.context);
            CompositionBattleVariable(sBattleExpansion,*token,regs[4]);
            // A battle wrapper consisting solely of one FD atom is a transport
            // for another complete dialogue (trainer lose/win speech, etc.).
            // Preserve that dialogue's proven source and frozen context, so its
            // reviewed translation/fragment mapping is used instead of showing
            // the Japanese placeholder value in the Spanish translation area.
            const u8* end=Cached(regs[9]+1);
            if(regs[9]==sBattleExpansion.hash+1&&end&&*end==0xff)
                if(const u32 source=Origin(regs[4])){sBattleExpansion.origin=source;
                    if(sLastOriginContext)sBattleExpansion.context=sLastOriginContext;}
        }
        return;
    }
    if(kind==12){
        auto& e=sBattleExpansion;const unsigned total=regs[6];
        if(e.origin&&e.stack==regs[15]&&regs[8]==e.dest&&total&&total<=1024)
            if(const u8* bytes=Ram(e.dest,total))if(bytes[total-1]==0xff){e.size=total>96?96:total;memcpy(e.snapshot,bytes,e.size);e.hash=TextHash(bytes,total);e.ready=total;CompositionBattleDone(e);}
        return;
    }
    if(kind==8||kind==16){
        const u32 dest=regs[0],src=regs[1];if(!Ram(dest,1))return;
        // Copy provenance applies to the whole copied string, not an arbitrary
        // matching substring. Appending to it invalidates its stored EOS.
        const u32 source=Origin(src);const u32 context=sLastOriginContext;
        // F7's real implementation copies each value with StringCopy. A
        // verified nested call belongs to the active output construction;
        // preserve only that parent, never a completed or unrelated proof.
        // Thumb BL in the VM can retain the executable linear-ROM alias in
        // LR. Normalize only that actual ROM mapping, never a masked RAM LR.
        const u32 caller=regs[16]>=0x02200000&&regs[16]<0x02400000?regs[16]-0x02200000+0x08000000:regs[16];
        ObserveRecapAppendBegin(regs,kind,caller);
        const bool dynamicParent=kind==8&&sDynamicExpansion.origin&&!sDynamicExpansion.ready&&caller==0x0813d3a1&&regs[15]+4==sDynamicExpansion.stack&&regs[2]==dest;
        InvalidateDestination(dest,dynamicParent);
        unsigned slot=sCopyCursor++&3;
        for(unsigned i=0;i<4;++i)if(sCopies[i].dest==dest){slot=i;break;}
        auto& copy=sCopies[slot];copy.dest=dest;copy.origin=source;copy.context=context;copy.stack=regs[15];copy.total=0;copy.hash=src;copy.size=kind==16?(regs[2]&255):0;CompositionCopyStart(slot,src,dest);return;
    }
    if(kind==17){
        // StringCopyN copies exactly its u8 count and does not append EOS.
        // Establish evidence only when the copied range contains the whole
        // atom-complete source through its real terminating byte.
        for(auto& copy:sCopies)if((copy.origin||CompositionCopyPending(&copy-sCopies))&&copy.stack==regs[15]&&copy.dest==regs[4]&&copy.hash==regs[6]&&copy.size==regs[5]){
            const unsigned limit=copy.size;unsigned total=0;
            while(total<limit){u8 c;const unsigned unit=CaptureAtom(copy.hash,total,limit,c);if(!unit)break;total+=unit;if(c==0xff){
                if(const u8* bytes=Ram(copy.dest,total))if(bytes[total-1]==0xff){copy.size=total>96?96:total;memcpy(copy.snapshot,bytes,copy.size);copy.hash=TextHash(bytes,total);copy.total=total;CompositionCopyDone(&copy-sCopies,copy.dest,total);}break;}}
        }return;
    }
    if(kind==18){
        const u32 dest=regs[15]+8,source=regs[4];InvalidateDestination(dest);
        // The real Pokédex category is six zero-padded bytes. The printer
        // receives a bounded RAM copy plus EOS, separately from ポケモン.
        if(source<0x08409c00||source>=0x0840c654||(source-0x08409c00)%28||regs[5]>6)return;
        const unsigned size=regs[5];const u8* bytes=Ram(dest,size+1);if(!bytes||bytes[size]!=0xff)return;
        for(unsigned i=0;i<6;++i){const u8* original=Cached(source+i);if(!original||((i<size)?*original!=bytes[i]:*original!=0))return;}
        unsigned slot=sCopyCursor++&3;for(unsigned i=0;i<4;++i)if(sCopies[i].dest==dest){slot=i;break;}
        auto& copy=sCopies[slot];copy.dest=dest;copy.origin=source;copy.context=0;copy.stack=regs[15];copy.size=copy.total=size+1;
        memcpy(copy.snapshot,bytes,size+1);copy.hash=TextHash(bytes,size+1);return;
    }
    if(kind==9){
        if(sRecapAppend.dest)ObserveRecapAppendDone(regs);
        for(auto& copy:sCopies)if((copy.origin||CompositionCopyPending(&copy-sCopies))&&!copy.total&&copy.stack==regs[15]&&regs[3]>=copy.dest&&regs[3]-copy.dest<1024){
            const unsigned total=regs[3]-copy.dest+1;if(const u8* bytes=Ram(copy.dest,total)){
                if(bytes[total-1]!=0xff)continue;copy.size=total>96?96:total;memcpy(copy.snapshot,bytes,copy.size);copy.hash=TextHash(bytes,total);copy.total=total;CompositionCopyDone(&copy-sCopies,copy.dest,total);
            }}return;
    }
    if(kind==6){CompositionReset();sTextPendingClear[0]=sTextPendingClear[1]=0;gKotoHelpArrowSlot=sHelpPendingSlot=255;gKotoTextWindowMask=0;gKotoTextOptionsWindow=gKotoTextArrowSlot=255;memset(gKotoTextLast,255,sizeof(gKotoTextLast));memset(gKotoTextAsync,255,sizeof(gKotoTextAsync));for(unsigned i=0;i<gKotoTextCapacity;++i){gKotoTextRecords[i].sequence=0;gKotoTextRecords[i].ready=0;}memset(sExpansion,0,sizeof(sExpansion));memset(sCopies,0,sizeof(sCopies));memset(sTextVariables,0,sizeof(sTextVariables));sBattleExpansion={};sDynamicExpansion={};sDynamicInput=0;sRecapAppend={};++gKotoTextGeneration;return;}
    if(kind==7){
        for(auto& e:sExpansion)if(e.origin&&e.stack==regs[15]&&regs[4]>=e.dest){
            const unsigned total=regs[4]-e.dest+1;if(!total||total>1024)continue;unsigned n=total>96?96:total;
            if(const u8* bytes=Ram(e.dest,total))if(bytes[total-1]==0xff){memcpy(e.snapshot,bytes,n);e.size=n;e.hash=TextHash(bytes,total);e.ready=total;}
        }
        return;
    }
    if(kind==1){
        const u32 dest=regs[0],src=regs[1];if(!Ram(dest,1))return;
        const bool recursive=(regs[16]&0x1fffff)>=0x8ab4&&(regs[16]&0x1fffff)<0x8ba8;
        const u8* marker=Cached(regs[5]-2);const u8* token=Cached(regs[5]-1);
        const Expansion* parent=nullptr;
        if(recursive&&marker&&token&&*marker==0xfd)for(const auto& e:sExpansion)
            if(e.origin&&!e.ready&&regs[15]<e.stack&&e.stack-regs[15]<=512&&regs[5]>=e.hash&&regs[5]-e.hash<1024)
                if(!parent||e.context>parent->context)parent=&e;
        if(parent){BindVariable(parent->origin,*token,src,parent->context);return;}
        if(recursive)return;
        const u32 source=Origin(src),context=sLastOriginContext;InvalidateDestination(dest);
        unsigned slot=sCopyCursor++&3;for(unsigned i=0;i<4;++i)if(sExpansion[i].dest==dest){slot=i;break;}
        auto& e=sExpansion[slot];e={};
        if(source&&src!=dest){e.dest=dest;e.origin=source;e.stack=regs[15];e.context=context?context:++sTextSerial;e.hash=src;}
        return;
    }
    if(kind==0){
        const u8* t=Ram(regs[0],16);if(!t||t[4]>=32)return;
        const u32 source=*(const u32*)t;const u32* w=(const u32*)(0x02020430+12*t[4]);
        const unsigned physical=gKotoTextCapacity<KOTO_TEXT_SLOTS?gKotoTextCapacity:KOTO_TEXT_SLOTS;
        if(!physical)return;unsigned slot=physical;u32 oldest=0xffffffff;
        for(unsigned i=0;i<physical;++i){auto& r=gKotoTextRecords[i];
            if(!r.sequence||(r.window==t[4]&&r.x==t[6]&&r.y==t[7])){slot=i;break;}
            if(r.sequence<oldest){oldest=r.sequence;slot=i;}}
        auto& r=gKotoTextRecords[slot];if(r.sequence&&!(r.window==t[4]&&r.x==t[6]&&r.y==t[7]))++gKotoTextOverflow;
        const u32 origin=Origin(source);
        sTextPendingClear[slot>>5]&=~(1u<<(slot&31));
        if(SamePublishedLiteral(r,t,regs[1],origin)){
            // Exact glyphs and all their bindings are already published. Do
            // not expose an intermediate ready=0 to a nested reader IRQ.
            gKotoTextLast[r.window]=slot;gKotoTextWindowMask|=1u<<r.window;
            r.sequence=++gKotoTextGeneration;return;
        }
        if(gKotoTextFragmentMasks)gKotoTextFragmentMasks[slot*KOTO_TEXT_FRAGMENT_MASK_STRIDE]=0;
        r.ready=0;r.sequence=0;r.source=source;r.origin=origin;r.context=sLastOriginContext;r.cursor=0;r.reserved=255;r.tiles=w[2];
        r.window=t[4];gKotoTextLast[r.window]=slot;if(regs[1]&&regs[1]!=255)gKotoTextAsync[r.window]=slot;else if(gKotoTextAsync[r.window]==slot)gKotoTextAsync[r.window]=255;r.font=t[5];r.x=t[6];r.y=t[7];r.speed=regs[1];r.size=0;
        CopyText(r);if(r.origin==0x083dbcaa)gKotoTextOptionsWindow=r.window;if(r.size<=2&&r.size&&r.bytes[0]==0xef)gKotoTextArrowSlot=slot;gKotoTextWindowMask|=1u<<r.window;r.sequence=++gKotoTextGeneration;CompositionBindRecord(slot);
        return;
    }
    unsigned window=kind==4?regs[6]:regs[0];
    if(kind==2){
        const u8* stack=Ram(regs[15],12);if(!stack)return;window=32;
        for(unsigned w=0;w<32;++w){const u32* data=(const u32*)(0x02020430+12*w);
            if(data[0]==*(const u32*)stack&&data[1]==*(const u32*)(stack+4)&&data[2]==*(const u32*)(stack+8)){window=w;break;}}
    }
    if(window>=32)return;bool changed=false;
    if(kind==4)gKotoTextWindowMask&=~(1u<<window);else gKotoTextWindowMask|=1u<<window;
    if((kind==3||kind==4)&&window==gKotoTextOptionsWindow)gKotoTextOptionsWindow=255;
    for(unsigned i=0;i<gKotoTextCapacity;++i){auto& r=gKotoTextRecords[i];if(!r.sequence||r.window!=window)continue;
        if(kind==2&&(regs[5]&2)&&i<KOTO_TEXT_SLOTS&&(sTextPendingClear[i>>5]&(1u<<(i&31)))){
            // No identical/new caption replaced this CPU-buffer erasure.
            // Its old pixels cease to authorize a record at this commit.
            sTextPendingClear[i>>5]&=~(1u<<(i&31));
            if(gKotoTextArrowSlot==i)gKotoTextArrowSlot=255;if(gKotoTextLast[window]==i)gKotoTextLast[window]=255;if(gKotoTextAsync[window]==i)gKotoTextAsync[window]=255;
            r.ready=0;r.sequence=0;changed=true;continue;
        }
        if(kind==2){if(regs[5]&2){if(RefreshPrintedFragment(r))changed=true;if(!r.size)CopyText(r);if(!r.cursor)r.cursor=kotogba_captureCursor(&r,r.source+(r.size?r.size-1:0));if(r.size&&r.size<=2&&r.bytes[0]==0xef)gKotoTextArrowSlot=i;if(!r.ready){r.ready=1;changed=true;}if(CompositionAdvance(i))changed=true;}continue;}
        // The asynchronous printer clears its window at FA/FB boundaries,
        // then continues the same string without another AddTextPrinter call.
        // Retain source provenance until publication; removals still invalidate.
        if((kind==3||kind==5)&&r.speed&&r.speed!=255){
            const u8* active=(const u8*)(0x02020030+32*window);
            const u32 cursor=*(const u32*)active;
            if(active[27]&&active[4]==window&&cursor>=r.source&&cursor-r.source<=1024){
                if(kind==3){r.ready=0;++gKotoTextGeneration;}continue;
            }
        }
        if(kind==5){const u8* stack=Ram(regs[15]+40,8);if(!stack)continue;
            const unsigned x=regs[2]&65535,y=regs[3]&65535,width=*(const u32*)stack&65535,height=*(const u32*)(stack+4)&65535;
            // A small cursor erasure at the left of a label does not remove it.
            if(r.x<x||r.y<y||r.x>=x+width||r.y>=y+height)continue;
            if(i<KOTO_TEXT_SLOTS&&PublishedLiteral(r)){sTextPendingClear[i>>5]|=1u<<(i&31);continue;}
        }
        if(i<KOTO_TEXT_SLOTS)sTextPendingClear[i>>5]&=~(1u<<(i&31));
        if(gKotoTextArrowSlot==i)gKotoTextArrowSlot=255;if(gKotoTextLast[window]==i)gKotoTextLast[window]=255;if(gKotoTextAsync[window]==i)gKotoTextAsync[window]=255;r.ready=0;r.sequence=0;++gKotoTextGeneration;
    }
    if(changed)++gKotoTextGeneration;
}

extern "C" volatile u32 gKotoGbaRuntimeEnabled;
extern "C" void kotogba_textTrap(),kotogba_expandTrap(),kotogba_publishTrap(),kotogba_clearTrap(),kotogba_removeTrap(),kotogba_rectTrap(),kotogba_windowsTrap(),kotogba_expandDoneTrap(),kotogba_copyTrap(),kotogba_copyDoneTrap(),kotogba_battleTrap(),kotogba_battleVariableTrap(),kotogba_battleDoneTrap(),kotogba_dynamicTrap(),kotogba_dynamicVariableTrap(),kotogba_dynamicDoneTrap(),kotogba_copyNTrap(),kotogba_copyNDoneTrap(),kotogba_categoryTrap(),kotogba_easyChatTrap(),kotogba_easyChatDoneTrap();
extern "C" void kotogba_helpTextTrap(),kotogba_helpPublishTrap(),kotogba_helpClearTrap(),kotogba_helpBeginTrap(),kotogba_helpQuestionTrap(),kotogba_helpPcNameTrap();
extern "C" void kotogba_saveFailedPublishTrap();
extern "C" void kotogba_speciesDoneTrap();
// SdCache is ARMv5 and returns via LDM/POP PC. With the core's CP15.L4=1
// those returns preserve ARM state. Call it from this ARMv4T bridge, whose
// own BX return safely restores the Thumb installer. A tail call would pass
// the Thumb LR straight through and recreate the hardware failure.
[[gnu::target("arm"),gnu::optimize("Os","no-optimize-sibling-calls"),gnu::noinline,gnu::section(".ewram.text")]] static void* LoadCapturePatchBlock(u32 address)
{
    return sdc_loadRomBlockForPatching(address);
}
extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] bool kotogba_installTextCapture(u32 gameCode,u8 revision)
{
    const u32 bprj=(u32)'B'|((u32)'P'<<8)|((u32)'R'<<16)|((u32)'J'<<24);
    if(gameCode!=bprj||revision!=1)return false;
    struct Hook {u32 offset;u16 instruction;void* target;};
    [[gnu::section(".ewram")]] static const Hook hooks[]={
        {0x2c60,0x4694,(void*)kotogba_textTrap},
        {0x8ab8,0x1c0d,(void*)kotogba_expandTrap},
        {0x3ece,0xb003,(void*)kotogba_publishTrap},
        {0x43a0,0x4d0e,(void*)kotogba_clearTrap},
        {0x3d7a,0x491d,(void*)kotogba_removeTrap},
        {0x42ba,0x4699,(void*)kotogba_rectTrap},
        {0x3a5e,0x4657,(void*)kotogba_windowsTrap},
        {0x8b98,0x1c20,(void*)kotogba_expandDoneTrap},
        {0x886e,0x1c03,(void*)kotogba_copyTrap},
        {0x8884,0x1c18,(void*)kotogba_copyDoneTrap},
        {0xd88f6,0x4681,(void*)kotogba_battleTrap},
        {0xd941c,0x1c08,(void*)kotogba_battleVariableTrap},
        {0xd949e,0x1c30,(void*)kotogba_battleDoneTrap},
        {0x13d36e,0x1c02,(void*)kotogba_dynamicTrap},
        {0x13d396,0x2900,(void*)kotogba_dynamicVariableTrap},
        {0x13d3b0,0x1c10,(void*)kotogba_dynamicDoneTrap},
        {0x88a6,0x1c04,(void*)kotogba_copyNTrap},
        {0x88ca,0x1960,(void*)kotogba_copyNDoneTrap},
        {0x10669a,0x9700,(void*)kotogba_categoryTrap},
        {0xbeada,0x1c04,(void*)kotogba_easyChatTrap},
        {0xbeb46,0x1c20,(void*)kotogba_easyChatDoneTrap},
        {0x13c796,0x4691,(void*)kotogba_helpTextTrap},
        {0x13c36e,0x4804,(void*)kotogba_helpPublishTrap},
        {0x13cccc,0x4669,(void*)kotogba_helpClearTrap},
        {0x13c2be,0x4804,(void*)kotogba_helpBeginTrap},
        {0x13cbba,0x4680,(void*)kotogba_helpQuestionTrap},
        {0x13c8a4,0x2801,(void*)kotogba_helpPcNameTrap},
        {0x0f63ca,0x4804,(void*)kotogba_saveFailedPublishTrap},
        {0x4066c,0x1871,(void*)kotogba_speciesDoneTrap},
    };
    // Validate the complete set before changing any instruction.
    for(const auto& h:hooks)if(*(const u16*)(0x02200000+h.offset)!=h.instruction)return false;
    for(const auto& h:hooks){
        const unsigned number=patch_addSwiPatch(h.target);
        u16* cached=(u16*)LoadCapturePatchBlock(0x08000000+h.offset);
        *cached=0xdfa0+number;*(u16*)(0x02200000+h.offset)=*cached;
    }
    gKotoTextCaptureInstalled=1;return true;
}

extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] void kotogba_setupTextCapture(void* data,unsigned size)
{
 gKotoTextCapacity=size>KOTO_TEXT_SCRATCH?(size-KOTO_TEXT_SCRATCH)/sizeof(KotoTextRecord):0;if(gKotoTextCapacity>KOTO_TEXT_TOTAL_SLOTS)gKotoTextCapacity=KOTO_TEXT_TOTAL_SLOTS;
 gKotoTextRecords=gKotoTextCapacity?(KotoTextRecord*)data:nullptr;sTextScratch=gKotoTextCapacity?(u8*)data+gKotoTextCapacity*sizeof(KotoTextRecord):nullptr;kotogba_resetTextCapture();
}

extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] void* kotogba_textScratch(){return sTextScratch;}
