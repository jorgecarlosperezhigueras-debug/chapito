#include "KotoGbaRecapNames.inc"

// Compact identities fit the unused u16 in each frozen variable snapshot.
// Only an exact table start/alignment is accepted. Personal names and arbitrary
// RAM strings carry no identity, even when their glyphs equal a known entity.
extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] u16 KotoEntityProof(u32 source)
{
    if(source>=0x081ffe78&&source<=0x08200988&&!((source-0x081ffe78)&7))return 0x1001+((source-0x081ffe78)>>3);
    if(source>=0x08207a8c&&source<=0x08207cf4&&!((source-0x08207a8c)&7))return 0x2001+((source-0x08207a8c)>>3);
    if(source>=0x0839beb8&&source<=0x0839f928&&!((source-0x0839beb8)%40))return 0x3001+(source-0x0839beb8)/40;
    if(source>=0x081ff4d0&&source<0x081ffe78&&!((source-0x081ff4d0)%6))return 0x4001+(source-0x081ff4d0)/6;
    if(source>=0x08000000)if(const unsigned name=KotoRecapNameIndex(source-0x08000000))return 0x5000+name;
    if(source>=0x081f97f4&&source<=0x081ff4b4&&!((source-0x081f97f4)&31))return 0x6001+((source-0x081f97f4)>>5);
    return 0;
}
extern "C" [[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline,gnu::section(".ewram.text")]] u32 KotoEntitySource(u16 proof)
{
    const unsigned index=(proof&4095)-1;
    if((proof>>12)==1&&index<355)return 0x081ffe78+8*index;
    if((proof>>12)==2&&index<78)return 0x08207a8c+8*index;
    if((proof>>12)==3&&index<375)return 0x0839beb8+40*index;
    if((proof>>12)==4&&index<412)return 0x081ff4d0+6*index;
    if((proof>>12)==5)if(const u32 source=KotoRecapNameSource(index))return 0x08000000+source;
    if((proof>>12)==6&&index<743)return 0x081f97f4+32*index;
    return 0;
}
