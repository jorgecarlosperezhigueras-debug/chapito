#include "common.h"
#include "KotoGbaBootDiag.h"
#include "KotoGbaLatinFont6x10.h"
#pragma GCC optimize ("Os", "no-ipa-ra")
#pragma GCC target ("thumb") // Explicit BX returns under CP15.L4
// Packed labels keep the unchanged EWRAM heap/cache budget for gameplay.
[[gnu::section(".ewram")]] static const char sLabels[] =
    "INICIO\0"
    "SUELTA BOTONES\0"
    "MENU CERRADO\0"
    "LEYENDO BIOS\0"
    "BIOS ACEPTADA\0"
    "REUBICANDO BIOS\0"
    "ABRIENDO ROM\0"
    "CREANDO MAPA SD\0"
    "MAPA SD LISTO\0"
    "LEYENDO CABECERA\0"
    "LEYENDO ROM\0"
    "PARCHES DE AUDIO\0"
    "BUSCANDO PAQUETE\0"
    "CARGANDO ESTUDIO\0"
    "PROTECCION MEMORIA\0"
    "AJUSTES\0"
    "PARTIDA GUARDADA\0"
    "PARCHES MOTOR\0"
    "PANTALLAS\0"
    "INTERFAZ\0"
    "LIMPIANDO MEMORIA\0"
    "TABLAS DEL MOTOR\0"
    "JIT\0"
    "DMA\0"
    "SONIDO\0"
    "CACHE\0"
    "RELOJ\0"
    "ENTRANDO AL JUEGO\0"
    "RETORNO DEL MOTOR\0"
    "ERROR DE LECTURA\0"
    "TARJETAS KANJI\0"
    "KANJIS CARGADOS\0"
    "ABRIENDO ESTUDIO\0"
    "ESTUDIO: BYTES\0"
    "VALIDAR ESTUDIO\0"
    "VOCABULARIO\0"
    "ABRIR TRADUCCIONES\0"
    "CABECERA CORPUS\0"
    "INDICES CORPUS\0"
    "MAPA CORPUS\0"
    "ABRIENDO KANJIS\0"
    "KANJIS: BYTES\0"
    "MAPA KANJIS\0"
    "MEMORIA\0"
    "LISTAS PERSONALES\0"
    "DETECTOR\0"
    "ESTUDIO LISTO\0"
    "ERROR DE CONTENIDO\0"
    "JUEGO SIN ESTUDIO\0"
    "ESTUDIO NO CARGADO\0";
[[gnu::section(".ewram")]] static const char sTitle[] = "KOTOGBA R28.1 - DIAGNOSTICO";
[[gnu::section(".ewram")]] static const char sHex[] = "0123456789ABCDEF";
[[gnu::section(".ewram.bss")]] volatile u32 gBootDiagStage;
[[gnu::section(".ewram.bss")]] volatile u32 gBootDiagValue;
[[gnu::section(".ewram.bss")]] volatile u32 gKotoGbaBootMode;
static void text(int x, int y, const char* s)
{
    volatile u16* fb=(volatile u16*)0x06200000;
    while (*s && x<248) {
        const KotoGbaGlyph6x10* glyph=nullptr;
        for (const auto& g:kKotoGbaLatinGlyphs) if (g.codepoint==(u8)*s) {glyph=&g;break;}
        if (glyph) for (int yy=0;yy<10;++yy) for(int xx=0;xx<6;++xx)
            if (glyph->rows[yy] & (1u<<(5-xx))) fb[(y+yy)*256+x+xx]=0xFFFF;
        ++s;x+=6;
    }
}
extern "C" void kotogba_bootStep(u32 stage,u32 value)
{
    gBootDiagStage=stage;gBootDiagValue=value;
    if(stage>=18 && stage<29) return; // Keep the reader visible after its initialization.
    volatile u16* fb=(volatile u16*)0x06200000;
    for (int i=136*256;i<192*256;++i) fb[i]=0x9000;
    text(8,140,sTitle);
    char number[3]={(char)('0'+stage/10),(char)('0'+stage%10),0};
    text(8,155,number);
    if(stage<50){const char* label=sLabels;for(u32 i=0;i<stage;++i){while(*label)++label;++label;}text(30,155,label);}
    char hex[9];for(int i=0;i<8;++i)hex[i]=sHex[(value>>(28-4*i))&15];hex[8]=0;
    text(8,173,hex);
}
