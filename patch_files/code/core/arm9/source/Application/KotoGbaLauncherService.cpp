#include "common.h"
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include "Fat/ff.h"
#include "SystemIpc.h"
#include "KotoGbaLauncherService.h"

#define KOTOGBA_MAX_ENTRIES     64
#define KOTOGBA_NAME_BYTES      128
#define KOTOGBA_PATH_BYTES      256
#define KOTOGBA_VISIBLE_ROWS    11

struct KotoGbaLauncherEntry
{
    char name[KOTOGBA_NAME_BYTES];
    bool isDirectory;
};

KotoGbaLauncherService gKotoGbaLauncherService;

[[gnu::section(".ewram.bss")]]
static KotoGbaLauncherEntry sEntries[KOTOGBA_MAX_ENTRIES];

static int AsciiLower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

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

static int ReadDirectory(const char* path)
{
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

        strncpy(sEntries[count].name, info.fname, KOTOGBA_NAME_BYTES - 1);
        sEntries[count].name[KOTOGBA_NAME_BYTES - 1] = '\0';
        sEntries[count].isDirectory = isDirectory;
        ++count;
    }

    f_closedir(&directory);
    SortEntries(count);
    return count;
}

static bool DirectoryExists(const char* path)
{
    DIR directory { };
    const FRESULT result = f_opendir(&directory, path);
    if (result != FR_OK)
        return false;
    f_closedir(&directory);
    return true;
}

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
            strncpy(path, candidate, pathSize - 1);
            path[pathSize - 1] = '\0';
            return;
        }
    }

    strncpy(path, "/", pathSize - 1);
    path[pathSize - 1] = '\0';
}

static bool BuildChildPath(const char* parent, const char* name, char* output, size_t outputSize)
{
    const int written = !strcmp(parent, "/")
        ? snprintf(output, outputSize, "/%s", name)
        : snprintf(output, outputSize, "%s/%s", parent, name);
    return written > 0 && (size_t)written < outputSize;
}

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

static void WaitForNextFrame()
{
    while (REG_VCOUNT >= 192);
    while (REG_VCOUNT < 192);
}

static u16 ReadPressedKeys()
{
    static u16 previous = 0;
    const u16 held = (u16)(~REG_KEYINPUT) & 0x03FF;
    const u16 pressed = held & ~previous;
    previous = held;
    return pressed;
}

static void RenderLauncher(const char* path, int count, int selected)
{
    iprintf("\x1b[2J\x1b[1;1H");
    iprintf("\x1b[37mkoto\x1b[31mG\x1b[37mba\n");
    iprintf("-------------------------------\n");
    iprintf("ELIGE TU JUEGO\n\n");
    iprintf("%.31s\n\n", path);

    if (count < 0)
    {
        iprintf("No puedo abrir esta carpeta.\n");
    }
    else if (count == 0)
    {
        iprintf("No hay juegos .gba aqui.\n");
        iprintf("B: carpeta anterior\n");
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

        for (int i = first; i < last; ++i)
        {
            const char marker = i == selected ? '>' : ' ';
            if (sEntries[i].isDirectory)
                iprintf("%c [DIR] %.23s\n", marker, sEntries[i].name);
            else
                iprintf("%c       %.23s\n", marker, sEntries[i].name);
        }
    }

    iprintf("\nA: ABRIR/JUGAR   B: ATRAS\n");
}

bool KotoGbaLauncherService::SelectRom(char* outputPath, size_t outputPathSize)
{
    if (!outputPath || outputPathSize < 8)
        return false;

    // The GBA core is not running yet. Reuse VRAM C for a temporary text launcher.
    consoleDemoInit();
    REG_MASTER_BRIGHT = 0x8010; // Hide the old GBARunner splash on the top screen.
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

        if (pressed & KEY_UP)
        {
            if (count > 0)
            {
                selected = selected > 0 ? selected - 1 : count - 1;
                redraw = true;
            }
        }
        else if (pressed & KEY_DOWN)
        {
            if (count > 0)
            {
                selected = selected + 1 < count ? selected + 1 : 0;
                redraw = true;
            }
        }
        else if (pressed & KEY_B)
        {
            if (strcmp(currentPath, "/"))
            {
                GoToParent(currentPath);
                selected = 0;
                count = ReadDirectory(currentPath);
                redraw = true;
            }
        }
        else if ((pressed & KEY_A) && count > 0)
        {
            char nextPath[KOTOGBA_PATH_BYTES];
            if (!BuildChildPath(currentPath, sEntries[selected].name, nextPath, sizeof(nextPath)))
            {
                iprintf("\nRuta demasiado larga.\n");
                continue;
            }

            if (sEntries[selected].isDirectory)
            {
                strncpy(currentPath, nextPath, sizeof(currentPath) - 1);
                currentPath[sizeof(currentPath) - 1] = '\0';
                selected = 0;
                count = ReadDirectory(currentPath);
                redraw = true;
            }
            else
            {
                if (strlen(nextPath) + 1 > outputPathSize)
                {
                    iprintf("\nRuta demasiado larga.\n");
                    continue;
                }
                strcpy(outputPath, nextPath);
                return true;
            }
        }

        if (redraw)
            RenderLauncher(currentPath, count, selected);
    }
}
