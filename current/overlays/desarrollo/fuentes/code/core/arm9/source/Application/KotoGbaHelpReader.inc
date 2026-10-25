// The Help System owns BG0/character block 3 while it is visible. These five
// renderer destinations describe its real panels, not particular help text.
struct NativeHelpPanel {u32 tiles;u8 left,top,width,height;};
static const NativeHelpPanel sNativeHelpPanels[]={
    {0x0201c000,16,24,26,16},
    {0x0201c9c0,16,48,26,13},
    {0x0201e3c0,16,112,26,5},
    {0x0201f400,104,0,16,2},
    {0x0201fd00,8,0,7,2},
    {0x0201c020,8,40,28,10},
};
#define KOTO_HELP_READER_CODE [[gnu::target("thumb"),gnu::noinline,gnu::section(".ewram.text")]]

KOTO_HELP_READER_CODE static bool NativeHelpTile(const KotoTextRecord& r,unsigned px,unsigned py)
{
    if(r.window<KOTO_HELP_MAIN||r.window>KOTO_SAVE_FAILED)return false;
    const auto& p=sNativeHelpPanels[r.window-KOTO_HELP_MAIN];
    if(r.tiles!=p.tiles||px>=p.width*8||py>=p.height*8)return false;
    const unsigned x=p.left+px,y=p.top+py;if(x>=240||y>=160)return false;
    const unsigned tile=*(volatile const u16*)(0x0600f800+2*((y/8)*32+x/8))&1023;
    const unsigned expected=(p.tiles-0x0201c000)/32+(py/8)*p.width+px/8;
    return tile==expected;
}

KOTO_HELP_READER_CODE static unsigned NativeHelpWidth(unsigned font,unsigned glyph)
{
    // These are the exact BPRJ1 decompressor width tables. They lie in the
    // permanently loaded first 2 MiB, so this never loads an SD-cache block.
    if(!font)return 8;
    if(!glyph)return 10;
    u8 width=0;NativeRomByte((font==5?0x081ea83c:0x081da60c)+glyph,width);
    return width<=16?width:0;
}

KOTO_HELP_READER_CODE static bool NativeHelpGlyph(const KotoTextRecord& r,unsigned px,unsigned py,unsigned width)
{
    // Check only tiles intersecting this glyph's actual clipped rectangle.
    // The middle tile matters when a 16-pixel glyph crosses three tile cells.
    for(unsigned row=py;row<py+12;row=(row&~7u)+8)
        for(unsigned col=px;col<px+width;col=(col&~7u)+8)
            if(NativeHelpTile(r,col,row))return true;
    return false;
}

KOTO_HELP_READER_CODE static bool NativeHelpVisible(const KotoTextRecord& r,int& x,int& y)
{
    const unsigned scene=kotogba_overlayState();
    if(r.window==KOTO_SAVE_FAILED){
        const unsigned state=*(volatile const u8*)0x0203aac8;
        if(scene!=9||state<5||state>7)return false;
    }else if(scene!=5)return false;
    if(NativeGpu(0)!=0x0100||NativeGpu(8)!=0x1f0c||
       NativeGpu(16)||NativeGpu(18)||r.window<KOTO_HELP_MAIN||r.window>KOTO_SAVE_FAILED)return false;
    const auto& p=sNativeHelpPanels[r.window-KOTO_HELP_MAIN];
    if(r.tiles!=p.tiles||r.x>=p.width*8||r.y>=p.height*8)return false;
    bool found=false;unsigned px=r.x,py=r.y;
    // Mirror the real renderer's advances, including variable-width glyphs,
    // button icons and frozen names. Spaces never prove readable text. A
    // covered first tile cannot hide a still-visible later glyph or row.
    for(unsigned at=0;!found&&at<r.size;++at){unsigned c=r.bytes[at];
        if(c==0xff||c==0xfa||c==0xfb)break;
        if(c==0xfe){px=r.x;py=(py+15)&255;continue;}
        if(c==0xfc){if(++at>=r.size)break;at+=NativeControlSize(r.bytes[at]);continue;}
        if(c==0xf8){if(++at>=r.size)break;u8 width=0;
            NativeRomByte(0x081c9dd8+4*r.bytes[at]+2,width);px=(px+width)&255;continue;}
        if(c==0xfd){if(++at>=r.size)break;const unsigned token=r.bytes[at];
            if(token!=1&&token!=2)continue;
            const u32 address=kotogba_variableSource(r.origin,token,r.context);
            if(address<0x02000000||address>=0x02400000)return false;
            const u8* name=(const u8*)address;
            for(unsigned i=0;i<(token==1?10u:64u)&&name[i]!=0xff;++i){
                const unsigned width=NativeHelpWidth(r.font,name[i]);
                if(name[i]&&NativeHelpGlyph(r,px,py,width)){found=true;break;}
                px=(px+width+(r.font?1:0))&255;
            }
            continue;
        }
        if(c==0xf9){if(++at>=r.size)break;c=0x100+r.bytes[at];}
        const unsigned width=NativeHelpWidth(r.font,c);
        if(c)found=NativeHelpGlyph(r,px,py,width);
        px=(px+width+(r.font?1:0))&255;
    }
    if(!found)return false;x=p.left+r.x;y=p.top+r.y;return true;
}

KOTO_HELP_READER_CODE static u32 NativeHelpSelection()
{
    const unsigned slot=gKotoHelpArrowSlot;
    if(slot>=KOTO_TEXT_SLOTS||slot>=gKotoTextCapacity)return 0;
    const auto& r=gKotoTextRecords[slot];
    if(!r.ready||!r.sequence||r.window!=KOTO_HELP_MAIN||r.size!=2||r.bytes[0]!=0xef)return 0;
    // A retained list cursor is not selection in a help article/welcome page.
    // The game's own list controller must still be in a menu input state.
    const u8* menu=(const u8*)0x0203f104;
    if(menu[14]!=0&&menu[14]!=3)return 0;
    const unsigned total=*(const u16*)(menu+4),shown=*(const u16*)(menu+6);
    if(!shown||shown>7||shown>total||total>52||menu[13]>=shown||menu[12]+shown>total)return 0;
    int x,y;if(!NativeHelpVisible(r,x,y)||x>=236)return 0;
    return 0x80000000u|(KOTO_HELP_MAIN<<16)|(unsigned(y)<<8)|unsigned(x+4);
}

#undef KOTO_HELP_READER_CODE
