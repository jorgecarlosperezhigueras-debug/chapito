#include "common.h"
#include <libtwl/gfx/gfx.h>
#include <libtwl/gfx/gfxBackground.h>
#include "KotoGbaRuntimeService.h"
#include "KotoGbaLatinFont6x10.h"

#pragma GCC optimize ("Os")

#define KOTOGBA_SCREEN_WIDTH  256
#define KOTOGBA_SCREEN_HEIGHT 192
#define KOTOGBA_GLYPH_WIDTH   6
#define KOTOGBA_GLYPH_HEIGHT  10
#define KOTOGBA_LINE_HEIGHT   14
#define KOTOGBA_COLOR_BG      (0x8000u | 2u | (2u << 5) | (3u << 10))
#define KOTOGBA_COLOR_PANEL   (0x8000u | 5u | (5u << 5) | (7u << 10))
#define KOTOGBA_COLOR_WHITE   0xFFFFu
#define KOTOGBA_COLOR_MUTED   (0x8000u | 20u | (20u << 5) | (20u << 10))
#define KOTOGBA_COLOR_RED     (0x8000u | 31u)

KotoGbaRuntimeService gKotoGbaRuntimeService;

[[gnu::section(".dtcm")]]
volatile u32 gKotoGbaRuntimeEnabled = 0;
[[gnu::section(".dtcm")]]
volatile u32 gKotoGbaPendingCardId = 0;
[[gnu::section(".dtcm")]]
static u32 sKotoGbaActiveCardId = 0;

struct KotoGbaCardPreview
{
    u32 id;
    const char* spanish;
};

// These strings come directly from the approved BPRJ_01.koto package.
// V0.5-B intentionally exposes only the automatic translation view.
// Vocabulary/grammar stay out of the passive screen and belong to investigation mode.
[[gnu::section(".ewram")]] static const char sIntroI001[] =
    "¡Encantado! ¡Bienvenido al mundo de los Pokémon! Me llamo Oak. Todos me conocen y respetan como el Profesor Pokémon.";
[[gnu::section(".ewram")]] static const char sIntroI002[] =
    "En este mundo viven por todas partes unas criaturas llamadas Pokémon.";
[[gnu::section(".ewram")]] static const char sIntroI003[] =
    "Las personas tienen a esas criaturas llamadas Pokémon como mascotas o las usan en combates... Y yo me dedico a investigar a los Pokémon.";
[[gnu::section(".ewram")]] static const char sIntroI004[] =
    "Pero antes, háblame un poco de ti.";
[[gnu::section(".ewram")]] static const char sIntroI005[] =
    "¿Cómo te llamas?";

[[gnu::section(".ewram")]] static const KotoGbaCardPreview sIntroCards[] =
{
    {1, sIntroI001},
    {2, sIntroI002},
    {3, sIntroI003},
    {4, sIntroI004},
    {5, sIntroI005},
};

[[gnu::section(".ewram")]] static const char sUiLogo[] = "kotoGBA";
[[gnu::section(".ewram")]] static const char sUiAutoHelp[] = "AYUDA AUTOMÁTICA";
[[gnu::section(".ewram")]] static const char sUiTranslation[] = "TRADUCCIÓN";
[[gnu::section(".ewram")]] static const char sUiAutomatic[] = "AUTOMÁTICO";

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

static const KotoGbaGlyph6x10* FindGlyph(u16 codepoint)
{
    for (unsigned i = 0; i < kKotoGbaLatinGlyphCount; ++i)
    {
        if (kKotoGbaLatinGlyphs[i].codepoint == codepoint)
            return &kKotoGbaLatinGlyphs[i];
    }

    if (codepoint != '?')
        return FindGlyph('?');
    return nullptr;
}

[[gnu::section(".itcm"), gnu::noinline]]
static void DrawGlyph(int x, int y, u16 codepoint, u16 color)
{
    if (codepoint == ' ')
        return;

    const KotoGbaGlyph6x10* glyph = FindGlyph(codepoint);
    if (!glyph)
        return;

    vu16* framebuffer = GFX_BG_SUB;
    for (int row = 0; row < KOTOGBA_GLYPH_HEIGHT; ++row)
    {
        const int yy = y + row;
        if (yy < 0 || yy >= KOTOGBA_SCREEN_HEIGHT)
            continue;

        const u8 bits = glyph->rows[row];
        for (int column = 0; column < KOTOGBA_GLYPH_WIDTH; ++column)
        {
            if ((bits & (1u << (5 - column))) == 0)
                continue;
            const int xx = x + column;
            if (xx >= 0 && xx < KOTOGBA_SCREEN_WIDTH)
                framebuffer[yy * KOTOGBA_SCREEN_WIDTH + xx] = color;
        }
    }
}

static u16 DecodeUtf8(const char*& text)
{
    const u8 first = (u8)*text++;
    if (first < 0x80)
        return first;

    if ((first & 0xE0) == 0xC0)
    {
        const u8 second = (u8)*text;
        if ((second & 0xC0) != 0x80)
            return '?';
        ++text;
        return (u16)(((first & 0x1F) << 6) | (second & 0x3F));
    }

    // The V0.5-B Spanish strings only require Latin-1 codepoints.
    // Consume any longer UTF-8 sequence safely and render a fallback glyph.
    if ((first & 0xF0) == 0xE0)
    {
        for (int i = 0; i < 2 && *text; ++i)
        {
            if ((((u8)*text) & 0xC0) == 0x80)
                ++text;
        }
    }
    else if ((first & 0xF8) == 0xF0)
    {
        for (int i = 0; i < 3 && *text; ++i)
        {
            if ((((u8)*text) & 0xC0) == 0x80)
                ++text;
        }
    }
    return '?';
}

static void DrawTextUtf8(int x, int y, const char* text, u16 color)
{
    const char* cursor = text;
    int column = 0;
    while (*cursor)
    {
        const u16 codepoint = DecodeUtf8(cursor);
        DrawGlyph(x + column * KOTOGBA_GLYPH_WIDTH, y, codepoint, color);
        ++column;
    }
}

static int CountWordCharacters(const char* text)
{
    const char* cursor = text;
    int count = 0;
    while (*cursor && *cursor != ' ' && *cursor != '\n')
    {
        DecodeUtf8(cursor);
        ++count;
    }
    return count;
}

[[gnu::section(".itcm"), gnu::noinline]]
static void DrawWrappedText(int x, int y, const char* text, u16 color,
    int maxColumns, int maxLines)
{
    const char* cursor = text;
    int column = 0;
    int line = 0;

    while (*cursor && line < maxLines)
    {
        while (*cursor == ' ')
            ++cursor;

        if (*cursor == '\n')
        {
            ++cursor;
            ++line;
            column = 0;
            continue;
        }
        if (!*cursor)
            break;

        const int wordLength = CountWordCharacters(cursor);
        if (column > 0 && column + 1 + wordLength > maxColumns)
        {
            ++line;
            column = 0;
            if (line >= maxLines)
                break;
        }

        if (column > 0)
            ++column;

        while (*cursor && *cursor != ' ' && *cursor != '\n')
        {
            if (column >= maxColumns)
            {
                ++line;
                column = 0;
                if (line >= maxLines)
                    return;
            }

            const u16 codepoint = DecodeUtf8(cursor);
            DrawGlyph(
                x + column * KOTOGBA_GLYPH_WIDTH,
                y + line * KOTOGBA_LINE_HEIGHT,
                codepoint,
                color);
            ++column;
        }
    }
}

static const KotoGbaCardPreview* FindCard(u32 cardId)
{
    for (const auto& card : sIntroCards)
    {
        if (card.id == cardId)
            return &card;
    }
    return nullptr;
}

static void BuildCardId(u32 cardId, char* output)
{
    output[0] = 'I';
    output[1] = (char)('0' + ((cardId / 100) % 10));
    output[2] = (char)('0' + ((cardId / 10) % 10));
    output[3] = (char)('0' + (cardId % 10));
    output[4] = '\0';
}

static void DrawLogo()
{
    DrawTextUtf8(8, 9, sUiLogo, KOTOGBA_COLOR_WHITE);
    DrawTextUtf8(8 + 4 * KOTOGBA_GLYPH_WIDTH, 9, "G", KOTOGBA_COLOR_RED);
}

static void DrawCard(u32 cardId)
{
    const KotoGbaCardPreview* card = FindCard(cardId);
    if (!card)
        return;

    char id[5];
    BuildCardId(cardId, id);

    FillRect(0, 0, KOTOGBA_SCREEN_WIDTH, KOTOGBA_SCREEN_HEIGHT, KOTOGBA_COLOR_BG);
    FillRect(0, 0, KOTOGBA_SCREEN_WIDTH, 30, KOTOGBA_COLOR_PANEL);

    DrawLogo();
    DrawTextUtf8(62, 9, sUiAutoHelp, KOTOGBA_COLOR_MUTED);
    DrawTextUtf8(8, 42, sUiTranslation, KOTOGBA_COLOR_RED);
    DrawWrappedText(8, 60, card->spanish, KOTOGBA_COLOR_WHITE, 40, 8);

    FillRect(8, 174, 240, 1, KOTOGBA_COLOR_PANEL);
    DrawTextUtf8(8, 178, id, KOTOGBA_COLOR_MUTED);
    DrawTextUtf8(44, 178, sUiAutomatic, KOTOGBA_COLOR_MUTED);
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
    DrawCard(pending);
}
