#include "common.h"
#include <string.h>
#include <libtwl/mem/memVram.h>
#include <libtwl/gfx/gfx.h>
#include <libtwl/gfx/gfxBackground.h>
#include "Fat/ff.h"
#include "GbaHeader.h"
#include "SystemIpc.h"
#include "KotoGbaLauncherService.h"

#define KOTOGBA_MAX_ENTRIES     64
#define KOTOGBA_NAME_BYTES      128
#define KOTOGBA_PATH_BYTES      256
#define KOTOGBA_VISIBLE_ROWS    10

#define KOTOGBA_REG_VCOUNT      (*(vu16*)0x04000006)
#define KOTOGBA_REG_KEYINPUT    (*(vu16*)0x04000130)

#define KOTOGBA_KEY_A           (1u << 0)
#define KOTOGBA_KEY_B           (1u << 1)
#define KOTOGBA_KEY_UP          (1u << 6)
#define KOTOGBA_KEY_DOWN        (1u << 7)

#define KOTOGBA_COLOR_BG        (0x8000u | 2u | (2u << 5) | (3u << 10))
#define KOTOGBA_COLOR_ROW       (0x8000u | 5u | (5u << 5) | (7u << 10))
#define KOTOGBA_COLOR_WHITE     0xFFFFu
#define KOTOGBA_COLOR_MUTED     (0x8000u | 20u | (20u << 5) | (20u << 10))
#define KOTOGBA_COLOR_RED       (0x8000u | 31u)

#define KOTOGBA_EWRAM_CODE      [[gnu::noinline]]

struct KotoGbaLauncherEntry
{
    char name[KOTOGBA_NAME_BYTES];
    bool isDirectory;
};

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
    {'9',{7,5,7,1,6}},
    {'.',{0,0,0,0,2}}, {'-',{0,0,7,0,0}}, {'_',{0,0,0,0,7}},
    {'/',{1,1,2,4,4}}, {':',{0,2,0,2,0}}, {'?',{6,1,2,0,2}},
    {'[',{6,4,4,4,6}}, {']',{3,1,1,1,3}}, {'>',{4,2,1,2,4}},
};

KotoGbaLauncherService gKotoGbaLauncherService;

[[gnu::section(".ewram.bss")]]
static KotoGbaLauncherEntry sEntries[KOTOGBA_MAX_ENTRIES];

[[gnu::section(".ewram.bss")]]
static KotoGbaPackageInfo sPackageInfo;

[[gnu::section(".ewram.bss")]]
static u16 sPreviousKeys;

KOTOGBA_EWRAM_CODE
static int AsciiLower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

KOTOGBA_EWRAM_CODE
static char DisplayChar(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char)(c - ('a' - 'A'));
    if ((unsigned char)c < 32 || (unsigned char)c > 126)
        return '?';
    return c;
}

KOTOGBA_EWRAM_CODE
static int CompareNames(const char* a, const char* b)
{
    while (*a && *b)
    {
        const int ca = AsciiLower((unsigned char)*a);
        const int cb = AsciiLower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

KOTOGBA_EWRAM_CODE
static bool IsGbaFile(const char* name)
{
    const size_t len = strlen(name);
    if (len < 4)
        return false;

    const char* ext = name + len - 4;
    return ext[0] == '.' &&
        AsciiLower((unsigned char)ext[1]) == 'g' &&
        AsciiLower((unsigned char)ext[2]) == 'b' &&
        AsciiLower((unsigned char)ext[3]) == 'a';
}

KOTOGBA_EWRAM_CODE
static void CopyName(char* destination, size_t destinationSize, const char* source)
{
    size_t length = strlen(source);
    if (length >= destinationSize)
        length = destinationSize - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

KOTOGBA_EWRAM_CODE
static void SortEntries(int count)
{
    for (int i = 1; i < count; ++i)
    {
        KotoGbaLauncherEntry value = sEntries[i];
        int j = i - 1;
        while (j >= 0)
        {
            const bool valueBefore =
                (value.isDirectory && !sEntries[j].isDirectory) ||
                (value.isDirectory == sEntries[j].isDirectory &&
                 CompareNames(value.name, sEntries[j].name) < 0);
            if (!valueBefore)
                break;
            sEntries[j + 1] = sEntries[j];
            --j;
        }
        sEntries[j + 1] = value;
    }
}

KOTOGBA_EWRAM_CODE
static int ReadDirectory(const char* path)
{
#ifdef KOTOGBA_LAUNCHER_PROBE
    (void)path;
    static const struct
    {
        const char* name;
        bool directory;
    } probeEntries[] =
    {
        {"RPG", true},
        {"Pocket Monsters - FireRed (Japan) (Rev 1).gba", false},
        {"Mother 3 (Japan).gba", false},
        {"Dragon Quest Monsters - Caravan Heart.gba", false},
    };

    const int count = sizeof(probeEntries) / sizeof(probeEntries[0]);
    for (int i = 0; i < count; ++i)
    {
        CopyName(sEntries[i].name, sizeof(sEntries[i].name), probeEntries[i].name);
        sEntries[i].isDirectory = probeEntries[i].directory;
    }
    SortEntries(count);
    return count;
#else
    DIR directory { };
    if (f_opendir(&directory, path) != FR_OK)
        return -1;

    int count = 0;
    FILINFO info { };
    while (count < KOTOGBA_MAX_ENTRIES)
    {
        if (f_readdir(&directory, &info) != FR_OK || info.fname[0] == '\0')
            break;

        if ((info.fattrib & (AM_HID | AM_SYS)) != 0)
            continue;
        if (!strcmp(info.fname, ".") || !strcmp(info.fname, ".."))
            continue;

        const bool isDirectory = (info.fattrib & AM_DIR) != 0;
        if (!isDirectory && !IsGbaFile(info.fname))
            continue;

        CopyName(sEntries[count].name, sizeof(sEntries[count].name), info.fname);
        sEntries[count].isDirectory = isDirectory;
        ++count;
    }

    f_closedir(&directory);
    SortEntries(count);
    return count;
#endif
}

KOTOGBA_EWRAM_CODE
static bool DirectoryExists(const char* path)
{
#ifdef KOTOGBA_LAUNCHER_PROBE
    (void)path;
    return true;
#else
    DIR directory { };
    const FRESULT result = f_opendir(&directory, path);
    if (result != FR_OK)
        return false;
    f_closedir(&directory);
    return true;
#endif
}

KOTOGBA_EWRAM_CODE
static void ChooseInitialPath(char* path, size_t pathSize)
{
    static const char* candidates[] =
    {
        "/_gba/kotogba/games",
        "/roms/gba",
        "/gba",
        "/"
    };

    for (const char* candidate : candidates)
    {
        if (DirectoryExists(candidate))
        {
            CopyName(path, pathSize, candidate);
            return;
        }
    }

    CopyName(path, pathSize, "/");
}

KOTOGBA_EWRAM_CODE
static bool BuildChildPath(const char* parent, const char* name, char* output, size_t outputSize)
{
    const size_t parentLength = strlen(parent);
    const size_t nameLength = strlen(name);
    const bool root = parentLength == 1 && parent[0] == '/';
    const size_t required = parentLength + (root ? 0 : 1) + nameLength + 1;
    if (required > outputSize)
        return false;

    memcpy(output, parent, parentLength);
    size_t offset = parentLength;
    if (!root)
        output[offset++] = '/';
    memcpy(output + offset, name, nameLength);
    output[offset + nameLength] = '\0';
    return true;
}

KOTOGBA_EWRAM_CODE
static void GoToParent(char* path)
{
    if (!strcmp(path, "/"))
        return;

    char* lastSlash = strrchr(path, '/');
    if (!lastSlash || lastSlash == path)
    {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    *lastSlash = '\0';
}

KOTOGBA_EWRAM_CODE
static void WaitForNextFrame()
{
    while (KOTOGBA_REG_VCOUNT >= 192);
    while (KOTOGBA_REG_VCOUNT < 192);
}

KOTOGBA_EWRAM_CODE
static u16 ReadPressedKeys()
{
    const u16 held = (u16)(~KOTOGBA_REG_KEYINPUT) & 0x03FF;
    const u16 pressed = held & ~sPreviousKeys;
    sPreviousKeys = held;
    return pressed;
}

KOTOGBA_EWRAM_CODE
static void WaitForKeysReleased()
{
    while (((u16)(~KOTOGBA_REG_KEYINPUT) & 0x03FF) != 0)
        WaitForNextFrame();
    sPreviousKeys = 0;
}

KOTOGBA_EWRAM_CODE
static void FillRect(int x, int y, int width, int height, u16 color)
{
    vu16* framebuffer = GFX_BG_SUB;
    for (int py = 0; py < height; ++py)
    {
        const int yy = y + py;
        if (yy < 0 || yy >= 192)
            continue;
        for (int px = 0; px < width; ++px)
        {
            const int xx = x + px;
            if (xx >= 0 && xx < 256)
                framebuffer[yy * 256 + xx] = color;
        }
    }
}

KOTOGBA_EWRAM_CODE
static const u8* FindGlyph(char character)
{
    character = DisplayChar(character);
    if (character == ' ')
        return nullptr;

    for (const auto& glyph : sGlyphs)
    {
        if (glyph.character == character)
            return glyph.row;
    }

    for (const auto& glyph : sGlyphs)
    {
        if (glyph.character == '?')
            return glyph.row;
    }
    return nullptr;
}

KOTOGBA_EWRAM_CODE
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

KOTOGBA_EWRAM_CODE
static void DrawText(int x, int y, const char* text, u16 color, int scale, int maxCharacters)
{
    const int advance = 4 * scale;
    int count = 0;
    while (*text && count < maxCharacters)
    {
        DrawChar(x, y, *text, color, scale);
        x += advance;
        ++text;
        ++count;
    }
}

KOTOGBA_EWRAM_CODE
static void DrawLogo()
{
    const char* title = "KOTOGBA";
    const int scale = 3;
    const int advance = 4 * scale;
    const int width = 7 * advance;
    int x = (256 - width) / 2;

    for (int i = 0; title[i]; ++i)
    {
        const u16 color = title[i] == 'G' ? KOTOGBA_COLOR_RED : KOTOGBA_COLOR_WHITE;
        DrawChar(x, 10, title[i], color, scale);
        x += advance;
    }
}

KOTOGBA_EWRAM_CODE
static const char* TailOfPath(const char* path, int maxCharacters)
{
    const size_t length = strlen(path);
    if ((int)length <= maxCharacters)
        return path;
    return path + length - maxCharacters;
}

KOTOGBA_EWRAM_CODE
static bool IsGameCodeCharacter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

KOTOGBA_EWRAM_CODE
static bool ValidateGbaHeader(const GbaHeader& header)
{
    if (header.fixedValue != 0x96)
        return false;

    const char code[4] =
    {
        (char)(header.gameCode & 0xFF),
        (char)((header.gameCode >> 8) & 0xFF),
        (char)((header.gameCode >> 16) & 0xFF),
        (char)((header.gameCode >> 24) & 0xFF)
    };
    for (char c : code)
    {
        if (!IsGameCodeCharacter(c))
            return false;
    }

    const u8* raw = reinterpret_cast<const u8*>(&header);
    u8 checksum = 0;
    for (int i = 0xA0; i <= 0xBC; ++i)
        checksum = (u8)(checksum - raw[i]);
    checksum = (u8)(checksum - 0x19);
    return checksum == header.headerChecksum;
}

KOTOGBA_EWRAM_CODE
static char HexDigit(u8 value)
{
    value &= 0x0F;
    return value < 10 ? (char)('0' + value) : (char)('A' + value - 10);
}

KOTOGBA_EWRAM_CODE
static void BuildPackagePath(const GbaHeader& header, char* output)
{
    static const char prefix[] = "/_gba/kotogba/packages/";
    static const char suffix[] = ".koto";

    int offset = 0;
    for (int i = 0; prefix[i]; ++i)
        output[offset++] = prefix[i];

    output[offset++] = (char)(header.gameCode & 0xFF);
    output[offset++] = (char)((header.gameCode >> 8) & 0xFF);
    output[offset++] = (char)((header.gameCode >> 16) & 0xFF);
    output[offset++] = (char)((header.gameCode >> 24) & 0xFF);
    output[offset++] = '_';
    output[offset++] = HexDigit(header.softwareVersion >> 4);
    output[offset++] = HexDigit(header.softwareVersion);

    for (int i = 0; suffix[i]; ++i)
        output[offset++] = suffix[i];
    output[offset] = '\0';
}

KOTOGBA_EWRAM_CODE
static bool PackageFileExists(const char* path)
{
    FIL file { };
    if (f_open(&file, path, FA_OPEN_EXISTING | FA_READ) != FR_OK)
        return false;
    f_close(&file);
    return true;
}

KOTOGBA_EWRAM_CODE
static void ClearPackageInfo()
{
    memset(&sPackageInfo, 0, sizeof(sPackageInfo));
}

KOTOGBA_EWRAM_CODE
static void BuildIdentityText(char* output)
{
    int offset = 0;
    output[offset++] = sPackageInfo.gameCode[0];
    output[offset++] = sPackageInfo.gameCode[1];
    output[offset++] = sPackageInfo.gameCode[2];
    output[offset++] = sPackageInfo.gameCode[3];
    output[offset++] = ' ';
    output[offset++] = 'R';
    output[offset++] = 'E';
    output[offset++] = 'V';
    output[offset++] = ' ';
    output[offset++] = HexDigit(sPackageInfo.revision >> 4);
    output[offset++] = HexDigit(sPackageInfo.revision);
    output[offset] = '\0';
}

KOTOGBA_EWRAM_CODE
static void RenderRomDetails(const char* romPath)
{
    FillRect(0, 0, 256, 192, KOTOGBA_COLOR_BG);
    DrawLogo();
    DrawText(72, 40, "JUEGO DETECTADO", KOTOGBA_COLOR_MUTED, 1, 30);
    DrawText(8, 62, TailOfPath(romPath, 60), KOTOGBA_COLOR_WHITE, 1, 60);

    if (!sPackageInfo.validRom)
    {
        DrawText(8, 88, "ROM GBA NO VALIDA", KOTOGBA_COLOR_RED, 1, 40);
        DrawText(8, 181, "B ATRAS", KOTOGBA_COLOR_MUTED, 1, 20);
        return;
    }

    char identity[16] { };
    BuildIdentityText(identity);
    DrawText(8, 88, identity, KOTOGBA_COLOR_WHITE, 1, 20);
    DrawText(8, 106,
        sPackageInfo.installed ? "KOTO: INSTALADO" : "KOTO: NO INSTALADO",
        sPackageInfo.installed ? KOTOGBA_COLOR_WHITE : KOTOGBA_COLOR_MUTED,
        1, 30);
    DrawText(8, 181, "A JUGAR   B ATRAS", KOTOGBA_COLOR_MUTED, 1, 40);
}

KOTOGBA_EWRAM_CODE
static void RenderLauncher(const char* path, int count, int selected)
{
    FillRect(0, 0, 256, 192, KOTOGBA_COLOR_BG);
    DrawLogo();
    DrawText(68, 32, "ELIGE TU JUEGO", KOTOGBA_COLOR_MUTED, 1, 30);
    DrawText(8, 51, TailOfPath(path, 60), KOTOGBA_COLOR_MUTED, 1, 60);

    if (count < 0)
    {
        DrawText(8, 76, "NO PUEDO ABRIR ESTA CARPETA", KOTOGBA_COLOR_WHITE, 1, 60);
    }
    else if (count == 0)
    {
        DrawText(8, 76, "NO HAY JUEGOS .GBA AQUI", KOTOGBA_COLOR_WHITE, 1, 60);
    }
    else
    {
        int first = selected - (KOTOGBA_VISIBLE_ROWS / 2);
        if (first < 0)
            first = 0;
        if (first + KOTOGBA_VISIBLE_ROWS > count)
            first = count - KOTOGBA_VISIBLE_ROWS;
        if (first < 0)
            first = 0;

        const int last = (first + KOTOGBA_VISIBLE_ROWS < count)
            ? first + KOTOGBA_VISIBLE_ROWS
            : count;

        int row = 0;
        for (int i = first; i < last; ++i, ++row)
        {
            const int y = 68 + row * 10;
            if (i == selected)
                FillRect(4, y - 2, 248, 9, KOTOGBA_COLOR_ROW);

            if (sEntries[i].isDirectory)
            {
                DrawText(8, y, "DIR", KOTOGBA_COLOR_MUTED, 1, 3);
                DrawText(28, y, sEntries[i].name, KOTOGBA_COLOR_WHITE, 1, 55);
            }
            else
            {
                DrawText(8, y, sEntries[i].name, KOTOGBA_COLOR_WHITE, 1, 60);
            }
        }
    }

    DrawText(8, 181, "A JUGAR   B ATRAS", KOTOGBA_COLOR_MUTED, 1, 40);
}

KOTOGBA_EWRAM_CODE
bool KotoGbaLauncherService::SelectRom(char* outputPath, size_t outputPathSize)
{
    if (!outputPath || outputPathSize < 8)
        return false;

    // Lightweight launcher: direct-color framebuffer in VRAM C, no console/newlib UI.
    mem_setVramCMapping(MEM_VRAM_C_SUB_BG_00000);
    REG_DISPCNT_SUB = 0x00010805;
    REG_BG3CNT_SUB = 0x4084;
    gfx_setSubBg3Affine(256, 0, 0, 256, 0, 0);
    REG_MASTER_BRIGHT = 0x8010;
    REG_MASTER_BRIGHT_SUB = 0;
    sysipc_setBottomBacklight(true);

    char currentPath[KOTOGBA_PATH_BYTES];
    ChooseInitialPath(currentPath, sizeof(currentPath));

    int selected = 0;
    int count = ReadDirectory(currentPath);
    RenderLauncher(currentPath, count, selected);

    while (true)
    {
        WaitForNextFrame();
        const u16 pressed = ReadPressedKeys();
        if (!pressed)
            continue;

        bool redraw = false;

        if (pressed & KOTOGBA_KEY_UP)
        {
            if (count > 0)
            {
                selected = selected > 0 ? selected - 1 : count - 1;
                redraw = true;
            }
        }
        else if (pressed & KOTOGBA_KEY_DOWN)
        {
            if (count > 0)
            {
                selected = selected + 1 < count ? selected + 1 : 0;
                redraw = true;
            }
        }
        else if (pressed & KOTOGBA_KEY_B)
        {
            if (strcmp(currentPath, "/"))
            {
                GoToParent(currentPath);
                selected = 0;
                count = ReadDirectory(currentPath);
                redraw = true;
            }
        }
        else if ((pressed & KOTOGBA_KEY_A) && count > 0)
        {
            char nextPath[KOTOGBA_PATH_BYTES];
            if (!BuildChildPath(currentPath, sEntries[selected].name, nextPath, sizeof(nextPath)))
                continue;

            if (sEntries[selected].isDirectory)
            {
                CopyName(currentPath, sizeof(currentPath), nextPath);
                selected = 0;
                count = ReadDirectory(currentPath);
                redraw = true;
            }
            else
            {
                gKotoGbaLauncherService.ResolvePackageForRom(nextPath);
                RenderRomDetails(nextPath);

                while (true)
                {
                    WaitForNextFrame();
                    const u16 detailPressed = ReadPressedKeys();

                    if (detailPressed & KOTOGBA_KEY_B)
                    {
                        redraw = true;
                        break;
                    }

                    if ((detailPressed & KOTOGBA_KEY_A) && sPackageInfo.validRom)
                    {
                        if (strlen(nextPath) + 1 > outputPathSize)
                            break;
                        CopyName(outputPath, outputPathSize, nextPath);
                        WaitForKeysReleased();
                        return true;
                    }
                }
            }
        }

        if (redraw)
            RenderLauncher(currentPath, count, selected);
    }
}


KOTOGBA_EWRAM_CODE
bool KotoGbaLauncherService::ResolvePackageForRom(const char* romPath)
{
    ClearPackageInfo();
    if (!romPath)
        return false;

    FIL file { };
    if (f_open(&file, romPath, FA_OPEN_EXISTING | FA_READ) != FR_OK)
        return false;

    GbaHeader header { };
    UINT bytesRead = 0;
    const FRESULT readResult = f_read(&file, &header, sizeof(header), &bytesRead);
    f_close(&file);

    if (readResult != FR_OK || bytesRead != sizeof(header))
        return false;

    return ResolvePackageForHeader(header);
}

KOTOGBA_EWRAM_CODE
bool KotoGbaLauncherService::ResolvePackageForHeader(const GbaHeader& header)
{
    ClearPackageInfo();
    if (!ValidateGbaHeader(header))
        return false;

    sPackageInfo.gameCode[0] = (char)(header.gameCode & 0xFF);
    sPackageInfo.gameCode[1] = (char)((header.gameCode >> 8) & 0xFF);
    sPackageInfo.gameCode[2] = (char)((header.gameCode >> 16) & 0xFF);
    sPackageInfo.gameCode[3] = (char)((header.gameCode >> 24) & 0xFF);
    sPackageInfo.gameCode[4] = '\0';
    sPackageInfo.revision = header.softwareVersion;
    sPackageInfo.validRom = true;

    BuildPackagePath(header, sPackageInfo.packagePath);
    sPackageInfo.installed = PackageFileExists(sPackageInfo.packagePath);
    return true;
}

KOTOGBA_EWRAM_CODE
const KotoGbaPackageInfo& KotoGbaLauncherService::GetPackageInfo() const
{
    return sPackageInfo;
}
