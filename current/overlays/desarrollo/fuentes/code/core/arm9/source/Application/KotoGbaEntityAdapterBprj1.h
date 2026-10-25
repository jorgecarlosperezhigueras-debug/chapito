#pragma once
#include "KotoGbaEntityProofBprj1.h"

// Game adaptation: only these reviewed BPRJ1 template slots carry an item.
// A byte-identical PLAYER/RIVAL/Pokemon nickname or unknown slot is literal.
// Other games supply their own roles and fixed-table bounds; the asynchronous
// resolver below uses the ordinary corpus index and its approved ES payload.
struct CorpusEntityRole {u32 source,slots;};
static const CorpusEntityRole sCorpusItemRoles[]={
    // Actual BPRJ Rev.1 obtain-item script: FD03 carries CopyItemName output.
    // The captured source proof is still required; spelling alone is not proof.
    {0x192c59,1u<<3},
    {0x3d941f,1u<<3},{0x3d9450,1u<<3},{0x3d9474,1u<<3},
    {0x3d9573,1u<<2},{0x3d9589,1u<<2},{0x3d95a1,1u<<2},{0x3d95b6,1u<<2},
    {0x3d96b0,1u<<2},{0x3d96c6,1u<<2},{0x3d96ef,1u<<2},
    {0x3d98f5,1u<<3},{0x3d9905,1u<<3},{0x3d9945,1u<<3},
    {0x3d996e,(1u<<2)|(1u<<3)},
    {0x3da0d2,1u<<2},{0x3da0e1,1u<<2}
    // Recap item slots are in sKotoRecapFDRoles. The earlier recap branch
    // returns before this table, so keeping those nine rows here duplicates
    // unreachable data (72 bytes) without adding any recognized origin.
};
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static unsigned CorpusEntityRoleFor(u32 origin,unsigned token)
{
    if(origin>=0x083dbf5a&&origin<=0x083dc887){
        const unsigned source=origin-0x083dbf00;
        if(token>=2&&token<=4){
            unsigned lo=0,hi=sizeof(sKotoRecapFDRoles)/sizeof(sKotoRecapFDRoles[0]);
            while(lo<hi){const unsigned mid=(lo+hi)/2;if(sKotoRecapFDRoles[mid].source<source)lo=mid+1;else hi=mid;}
            return lo<sizeof(sKotoRecapFDRoles)/sizeof(sKotoRecapFDRoles[0])&&sKotoRecapFDRoles[lo].source==source?(sKotoRecapFDRoles[lo].roles>>(4*(token-2)))&15:0;
        }
        if(token>=768&&token<773){
            unsigned lo=0,hi=sizeof(sKotoRecapF7Roles)/sizeof(sKotoRecapF7Roles[0]);
            while(lo<hi){const unsigned mid=(lo+hi)/2;if(sKotoRecapF7Roles[mid].source<source)lo=mid+1;else hi=mid;}
            if(lo<sizeof(sKotoRecapF7Roles)/sizeof(sKotoRecapF7Roles[0])&&sKotoRecapF7Roles[lo].source==source){
                const u8* roles=sKotoRecapF7Roles[lo].roles;const unsigned slot=token-768;
                return (roles[slot/2]>>(4*(slot&1)))&15;
            }
        }
        return 0;
    }
    // BattleStringExpandPlaceholders owns a distinct FD namespace. Only
    // engine-defined entity slots in its ROM text range receive these roles.
    if(origin>=0x083bd6dd&&origin<0x083bf900){
        if(token==0x14||token==0x15)return 2;
        if(token==0x16)return 1;
        if(token>=0x17&&token<=0x1b)return 3;
    }
    if(token<2||token>4||origin<0x08000000)return 0;
    const u32 source=origin-0x08000000;
    for(const auto& r:sCorpusItemRoles)if(r.source==source)return (r.slots&(1u<<token))?1:0;
    return 0;
}
[[gnu::target("thumb"),gnu::optimize("Os"),gnu::noinline]] static bool CorpusEntitySource(unsigned role,u32 source)
{
    if(role==1)return source>=0x39beb8&&source<=0x39f928&&!((source-0x39beb8)%40);
    if(role==2)return source>=0x1ffe78&&source<=0x200988&&!((source-0x1ffe78)&7);
    if(role==3)return source>=0x207a8c&&source<=0x207cf4&&!((source-0x207a8c)&7);
    if(role==4)return source==0x3d8b6d||(source>=0x1ff4d0&&source<0x1ffe78&&!((source-0x1ff4d0)%6));
    if(role==7)return source>=0x1f97f4&&source<=0x1ff4b4&&!((source-0x1f97f4)&31);
    const unsigned kind=KotoRecapNameRole(source);
    return role==8?kind==8:(role==5||role==6)&&(kind==5||kind==6);
}

