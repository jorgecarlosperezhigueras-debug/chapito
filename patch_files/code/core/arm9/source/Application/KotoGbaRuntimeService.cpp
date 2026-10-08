#include "common.h"
#include <libtwl/gfx/gfx.h>
#include <libtwl/gfx/gfxBackground.h>
#include "KotoGbaRuntimeService.h"

#define KOTOGBA_SCREEN_WIDTH  256
#define KOTOGBA_SCREEN_HEIGHT 192
#define KOTOGBA_COLOR_BG      (0x8000u | 2u | (2u << 5) | (3u << 10))
#define KOTOGBA_COLOR_WHITE   0xFFFFu
#define KOTOGBA_COLOR_MUTED   (0x8000u | 20u | (20u << 5) | (20u << 10))
#define KOTOGBA_COLOR_RED     (0x8000u | 31u)
#define KOTOGBA_COLOR_ROW     (0x8000u | 5u | (5u << 5) | (7u << 10))

KotoGbaRuntimeService gKotoGbaRuntimeService;

[[gnu::section(".dtcm")]]
volatile u32 gKotoGbaRuntimeEnabled = 0;
[[gnu::section(".dtcm")]]
volatile u32 gKotoGbaPendingCardId = 0;
[[gnu::section(".dtcm")]]
static u32 sKotoGbaActiveCardId = 0;

struct KotoGlyph
{
    char character;
    u8 row[5];
};

static constexpr KotoGlyph sGlyphs[] =
{
    {'A',{2,5,7,5,5}}, {'B',{6,5,6,5,6}}, {'C',{3,4,4,4,3}},
    {'D',{6,5,5,5,6}}, {'E',{7,4,6,4,7}}, {'F',{7,4,6,4,4}},
    {'G',{3,4,5,5,3}}, {'H',{5,5,7,5,5}}, {'I',{7,2,2,2,7}},
    {'J',{1,1,1,5,2}}, {'K',{5,5,6,5,5}}, {'L',{4,4,4,4,7}},
    {'M',{5,7,7,5,5}}, {'N',{5,7,7,7,5}}, {'O',{2,5,5,5,2}},
    {'P',{6,5,6,4,4}}, {'Q',{2,5,5,3,1}}, {'R',{6,5,6,5,5}},
    {'S',{3,4,2,1,6}}, {'T',{7,2,2,2,2}}, {'U',{5,5,5,5,7}},
    {'V',{5,5,5,5,2}}, {'W',{5,5,7,7,5}}, {'X',{5,5,2,5,5}},
    {'Y',{5,5,2,2,2}}, {'Z',{7,1,2,4,7}},
    {'0',{7,5,5,5,7}}, {'1',{2,6,2,2,7}}, {'2',{6,1,7,4,7}},
    {'3',{6,1,3,1,6}}, {'4',{5,5,7,1,1}}, {'5',{7,4,6,1,6}},
    {'6',{3,4,7,5,7}}, {'7',{7,1,1,2,2}}, {'8',{7,5,7,5,7}},
    {'9',{7,5,7,1,6}}, {'.',{0,0,0,0,2}}, {'-',{0,0,7,0,0}},
    {':',{0,2,0,2,0}}, {'?',{6,1,2,0,2}},
};

static void FillRect(int x, int y, int width, int height, u16 color)
{
    vu16* framebuffer = GFX_BG_SUB;
    for (int py = 0; py < height; ++py)
    {
        const int yy = y + py;
        if (yy < 0 || yy >= KOTOGBA_SCREEN_HEIGHT)
            continue;
        for (int px = 0; px < width; ++px)
        {
            const int xx = x + px;
            if (xx >= 0 && xx < KOTOGBA_SCREEN_WIDTH)
                framebuffer[yy * KOTOGBA_SCREEN_WIDTH + xx] = color;
        }
    }
}

static const u8* FindGlyph(char character)
{
    if (character == ' ')
        return nullptr;

    for (const auto& glyph : sGlyphs)
    {
        if (glyph.character == character)
            return glyph.row;
    }
    return nullptr;
}

static void DrawChar(int x, int y, char character, u16 color, int scale)
{
    const u8* rows = FindGlyph(character);
    if (!rows)
        return;

    for (int row = 0; row < 5; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            if ((rows[row] & (1u << (2 - column))) == 0)
                continue;
            FillRect(x + column * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void DrawText(int x, int y, const char* text, u16 color, int scale)
{
    const int advance = 4 * scale;
    while (*text)
    {
        DrawChar(x, y, *text, color, scale);
        x += advance;
        ++text;
    }
}

static void DrawCardId(u32 cardId)
{
    char id[5] = {'I', '0', '0', '0', '\0'};
    if (cardId > 999)
        cardId = 999;
    id[1] = (char)('0' + ((cardId / 100) % 10));
    id[2] = (char)('0' + ((cardId / 10) % 10));
    id[3] = (char)('0' + (cardId % 10));

    FillRect(0, 0, 256, 192, KOTOGBA_COLOR_BG);
    FillRect(0, 0, 256, 28, KOTOGBA_COLOR_ROW);
    DrawText(20, 8, "KOTOGBA V0.5-A", KOTOGBA_COLOR_WHITE, 2);
    DrawText(52, 47, "AUTO SYNC", KOTOGBA_COLOR_MUTED, 2);
    DrawText(80, 78, id, KOTOGBA_COLOR_RED, 5);
    DrawText(48, 119, "TEXTO DETECTADO", KOTOGBA_COLOR_WHITE, 1);
    DrawText(38, 145, "SIN TOCAR KOTOGBA", KOTOGBA_COLOR_MUTED, 1);
    DrawText(58, 166, "SIGUE JUGANDO", KOTOGBA_COLOR_MUTED, 1);
}

void KotoGbaRuntimeService::Initialize(u32 gameCode, u8 revision, bool packageInstalled)
{
    const u32 bprj = (u32)'B' | ((u32)'P' << 8) | ((u32)'R' << 16) | ((u32)'J' << 24);
    gKotoGbaPendingCardId = 0;
    sKotoGbaActiveCardId = 0;
    gKotoGbaRuntimeEnabled = (gameCode == bprj && revision == 1 && packageInstalled) ? 1u : 0u;
}

extern "C" void kotogba_vblankUpdate()
{
    if (!gKotoGbaRuntimeEnabled)
        return;

    const u32 pending = gKotoGbaPendingCardId;
    if (pending == 0 || pending == sKotoGbaActiveCardId)
        return;

    sKotoGbaActiveCardId = pending;
    DrawCardId(pending);
}
