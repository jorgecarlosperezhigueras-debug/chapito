// BPRJ1's L/R Help System renders directly into gDecompressionBuffer. It
// never creates a normal Window or TextPrinter. Observe the real renderer
// arguments and its CommitTilemap publication, retaining ordinary captures
// while the game saves/restores their tiles behind this temporary scene.
[[gnu::section(".ewram.bss")]] volatile u8 gKotoHelpArrowSlot;
[[gnu::section(".ewram.bss")]] static u8 sHelpPendingSlot;

#define KOTO_HELP_CODE [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]]

extern "C" KOTO_HELP_CODE unsigned kotogba_overlayState()
{
    // RunSaveFailedScreen uses the same renderer, but a separate controller.
    // Its flag remains set through the display-restoration states; do not
    // let ordinary retained windows claim their old tiles during that span.
    if(*(volatile const u32*)0x030053e0==1&&*(volatile const u8*)0x0203aac8<=8)return 9;
    const unsigned state=*(volatile const u8*)0x0203f101;
    return *(volatile const u8*)0x0203f0eb==1&&state>=1&&state<=8?state:0;
}

KOTO_HELP_CODE static unsigned HelpPanel(u32 dest,unsigned width,unsigned height)
{
    if(dest==0x0201c000&&width==26&&height==16)return KOTO_HELP_MAIN;
    if(dest==0x0201c9c0&&width==26&&height==13)return KOTO_HELP_ANSWER;
    if(dest==0x0201e3c0&&width==26&&height==5)return KOTO_HELP_DESCRIPTION;
    if(dest==0x0201f400&&width==16&&height==2)return KOTO_HELP_CONTROLS;
    if(dest==0x0201fd00&&width==7&&height==2)return KOTO_HELP_TITLE;
    if(dest==0x0201c020&&width==28&&height==10)return KOTO_SAVE_FAILED;
    return 255;
}

KOTO_HELP_CODE static void HelpInvalidate(bool all)
{
    for(unsigned i=0;i<KOTO_TEXT_SLOTS&&i<gKotoTextCapacity;++i){auto& r=gKotoTextRecords[i];
        if(r.window<KOTO_HELP_MAIN||r.window>KOTO_SAVE_FAILED||(!all&&r.window>KOTO_HELP_DESCRIPTION))continue;
        r.ready=0;r.sequence=0;
        if(gKotoTextFragmentMasks)gKotoTextFragmentMasks[i*KOTO_TEXT_FRAGMENT_MASK_STRIDE]=0;
    }
    gKotoHelpArrowSlot=sHelpPendingSlot=255;++gKotoTextGeneration;
}

KOTO_HELP_CODE static void HelpObserve(const u32* regs,unsigned kind)
{
    if(kind==24){HelpInvalidate(true);return;} // SaveMapTiles: a new help session
    if(kind==23||kind==25){HelpInvalidate(false);return;} // both main-panel fills
    if(kind==26){
        // The renderer has actually evaluated FLAG_SYS_NOT_SOMEONES_PC. This
        // hook witnesses its result, including quest-log flag indirection,
        // instead of guessing a name from a save-file layout or the caption.
        if(sHelpPendingSlot<gKotoTextCapacity){const auto& r=gKotoTextRecords[sHelpPendingSlot];
            if(r.window>=KOTO_HELP_MAIN&&r.window<=KOTO_HELP_TITLE&&r.origin)
                BindVariable(r.origin,2,regs[0]==1?0x083dd84e:0x083dd852,r.context);
        }
        return;
    }
    if(kind==21){
        sHelpPendingSlot=255;
        // At MOV r9,r2 the renderer has saved 88 bytes on the guest stack.
        // These are the actual y/width/height call arguments, not UI guesses.
        const u8* stack=Ram(regs[15]+88,12);if(!stack||regs[0]>5)return;
        const unsigned y=*(const u32*)stack&255,width=*(const u32*)(stack+4)&255,height=*(const u32*)(stack+8)&255;
        const unsigned panel=HelpPanel(regs[1],width,height),x=regs[3]&255;
        if(panel==255||x>=width*8||y>=height*8)return;
        const unsigned capacity=gKotoTextCapacity<KOTO_TEXT_SLOTS?gKotoTextCapacity:KOTO_TEXT_SLOTS;
        if(!capacity)return;
        unsigned slot=capacity;u32 oldest=0xffffffffu;
        for(unsigned i=0;i<capacity;++i){const auto& r=gKotoTextRecords[i];
            if(!r.sequence||(r.window==panel&&r.x==x&&r.y==y)){slot=i;break;}
            if(r.sequence<oldest){oldest=r.sequence;slot=i;}}
        auto& r=gKotoTextRecords[slot];
        if(r.sequence&&!(r.window==panel&&r.x==x&&r.y==y))++gKotoTextOverflow;
        if(gKotoHelpArrowSlot==slot)gKotoHelpArrowSlot=255;
        r.ready=0;r.sequence=0;r.source=regs[2];r.origin=Origin(r.source);r.tiles=regs[1];
        r.context=++sTextSerial;r.cursor=0;r.window=panel;r.x=x;r.y=y;r.font=regs[0];r.size=0;r.speed=0;r.reserved=0;
        if(gKotoTextFragmentMasks)gKotoTextFragmentMasks[slot*KOTO_TEXT_FRAGMENT_MASK_STRIDE]=0;
        CompositionBindRecord(slot);CopyText(r);
        // Player names are read directly by this renderer rather than through
        // StringExpand. Freeze only the FD01 occurrences that it will draw.
        for(unsigned at=0;at<r.size;){u8 c;const unsigned atom=CaptureAtom(r.source,at,r.size,c);if(!atom)break;
            if(c==0xfd&&atom==2&&r.bytes[at+1]==1&&r.origin)
                BindVariable(r.origin,1,*(const u32*)0x03004fac,r.context);
            at+=atom;
        }
        r.sequence=++gKotoTextGeneration;sHelpPendingSlot=slot;return;
    }
    // CommitTilemap follows all glyph reads and the corresponding panel/map
    // writes. Publish only a complete atom-safe string, in the same bounded
    // capture storage as ordinary windows. The complete help inventory fits
    // the 240-byte payload (including the 153-byte welcome message), without
    // a source-specific route or a truncation at its initial visible lines.
    if(kind==22||kind==27){bool changed=false;
        for(unsigned i=0;i<KOTO_TEXT_SLOTS&&i<gKotoTextCapacity;++i){auto& r=gKotoTextRecords[i];
            // Each DMA path publishes only its own panel family. A retained
            // Help caption must never become ready from the save-error copy.
            if(r.window<KOTO_HELP_MAIN||r.window>KOTO_SAVE_FAILED||
               (kind==27)!=(r.window==KOTO_SAVE_FAILED)||!r.sequence||r.ready)continue;
            CopyText(r);if(!r.size||r.bytes[r.size-1]!=0xff)continue;
            // A control's final operand can itself be FF at the payload
            // boundary. Only an EOS atom proves the entire caption arrived.
            unsigned end=0;u8 last=0;
            while(end<r.size){u8 c;const unsigned atom=CaptureAtom(r.source,end,r.size,c);
                if(!atom)break;end+=atom;last=c;}
            if(end!=r.size||last!=0xff)continue;
            r.cursor=r.origin?r.origin+r.size-1:0;
            asm volatile("":::"memory");r.ready=1;changed=true;
            if(r.size==2&&r.bytes[0]==0xef)gKotoHelpArrowSlot=i;
        }
        if(changed)++gKotoTextGeneration;
    }
}

#undef KOTO_HELP_CODE
