#include "common.h"
#include "KotoGbaRuntimeService.h"
extern "C" void kotogba_initializeReader(u32,u8,bool);
extern "C" void kotogba_readerBiosError();
extern "C" void kotogba_readerUpdate();
extern "C" void kotogba_readerResume();
extern "C" unsigned kotogba_readerBudgetLines();
extern "C" unsigned gKotoGbaReaderWindowEnd;
extern "C" unsigned gKotoGbaReaderInputDone;
static u32 enter() {u32 v; asm volatile("mrc p15,0,%0,c5,c0,3":"=r"(v));u32 n=(v&~0xf0u)|0x10u;asm volatile("mcr p15,0,%0,c5,c0,3"::"r"(n):"memory");return v;}
static void leave(u32 v){asm volatile("mcr p15,0,%0,c5,c0,3"::"r"(v):"memory");}
void KotoGbaRuntimeService::Initialize(u32 g,u8 r,bool p){u32 v=enter();kotogba_initializeReader(g,r,p);leave(v);}
void KotoGbaRuntimeService::ShowBiosError(){u32 v=enter();kotogba_readerBiosError();leave(v);}
extern "C" void kotogba_vblankUpdate()
{
    const unsigned line=*(volatile u16*)0x04000006;
    u32 v=enter();
    // One shared deadline, including audio resumes. Active work gets24 lines,
    // retained-parent copies48 and idle work8. Never reach active display.
    const unsigned lines=kotogba_readerBudgetLines();
    gKotoGbaReaderWindowEnd=line>=192 && line<260 ? (line+lines<260?line+lines:260) : 0;
    gKotoGbaReaderInputDone=0;
    kotogba_readerUpdate();leave(v);
}
extern "C" void kotogba_audioUpdate()
{
    const unsigned line=*(volatile u16*)0x04000006;
    if(line<192 || line>=gKotoGbaReaderWindowEnd)return;
    // VBlank may have arrived while sound DMA was being serviced.
    if(*(volatile u32*)0x04000214 & 1u)return;
    u32 v=enter();kotogba_readerResume();leave(v);
}
