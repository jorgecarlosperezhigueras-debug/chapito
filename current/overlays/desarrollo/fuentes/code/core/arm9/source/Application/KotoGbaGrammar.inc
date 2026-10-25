// R29 keeps the legacy KC15 grammar tail structurally validated, but the
// complete teacher-reviewed authority lives in the pinned KL29 document.
// Called by the existing document parser after every earlier extension has
// been bounded. An invalid grammar tail rejects that document before paint.
[[gnu::target("thumb"),gnu::noinline]] static unsigned CorpusValidateGrammar(unsigned tail,unsigned bytes)
{
    if(tail+2>bytes)return 65535;
    const unsigned count=Corpus16(sCorpusDocument+32+tail);tail+=2;
    if(!count||count>3)return 65535;
    for(unsigned i=0;i<count;++i){
        if(tail+10>bytes)return 65535;
        const u8* p=sCorpusDocument+32+tail;const unsigned n=Corpus16(p+8),ref=Corpus16(p+6);
        const unsigned cardId=Corpus16(p+4);const u8* card=FindStudy(cardId);
        // Card zero is reserved for a self-contained, copied contextual note.
        // Other references retain the existing, bounded KS5 grammar target.
        if(cardId?(!card||!(ref&0x8000)||((ref>>9)&63)>=Card16(card,14)||
           (ref&511)>=Read16(Read32(36)+8*(Card16(card,8)+((ref>>9)&63))+4)):ref!=0x8000)return 65535;
        if(Corpus16(p)>=Corpus16(p+2)||Corpus16(p+2)>1024||!n||n>144||tail+10+n+1>bytes||p[10+n]||memchr(p+10,0,n))return 65535;
        tail+=11+n;
    }
    return tail;
}

#include "KotoGbaGrammarFull.inc"
