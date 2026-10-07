#include "common.h"
#include <libtwl/mem/memVram.h>
#include <libtwl/gfx/gfx.h>
#include <libtwl/gfx/gfxBackground.h>
#include "Fat/ff.h"
#include "MemCopy.h"
#include "SystemIpc.h"
#include "KotoGbaUiService.h"

#define KOTOGBA_HOME_PATH       "/_gba/kotogba/home.raw16"
#define KOTOGBA_SCREEN_WIDTH    256u
#define KOTOGBA_SCREEN_HEIGHT   192u
#define KOTOGBA_SCREEN_BYTES    (KOTOGBA_SCREEN_WIDTH * KOTOGBA_SCREEN_HEIGHT * 2u)
#define KOTOGBA_IO_CHUNK        4096u

KotoGbaUiService gKotoGbaUiService;

[[gnu::section(".ewram.bss")]]
alignas(4) static u8 sKotoGbaIoBuffer[KOTOGBA_IO_CHUNK];

static void FillLowerScreen(u16 color)
{
    vu16* dst = GFX_BG_SUB;
    for (u32 i = 0; i < KOTOGBA_SCREEN_WIDTH * KOTOGBA_SCREEN_HEIGHT; ++i)
        dst[i] = color;
}

static bool LoadRaw16(const char* path)
{
    FIL file { };
    if (f_open(&file, path, FA_OPEN_EXISTING | FA_READ) != FR_OK)
        return false;

    u32 offset = 0;
    bool ok = true;
    while (offset < KOTOGBA_SCREEN_BYTES)
    {
        const u32 remaining = KOTOGBA_SCREEN_BYTES - offset;
        const UINT requested = remaining < KOTOGBA_IO_CHUNK ? remaining : KOTOGBA_IO_CHUNK;
        UINT bytesRead = 0;
        if (f_read(&file, sKotoGbaIoBuffer, requested, &bytesRead) != FR_OK ||
            bytesRead != requested || (bytesRead & 3u) != 0)
        {
            ok = false;
            break;
        }

        mem_copy32(sKotoGbaIoBuffer,
            reinterpret_cast<void*>(reinterpret_cast<u32>(GFX_BG_SUB) + offset),
            bytesRead);
        offset += bytesRead;
    }

    f_close(&file);
    return ok && offset == KOTOGBA_SCREEN_BYTES;
}

bool KotoGbaUiService::Initialize()
{
    // With center-and-mask disabled, VRAM C is not needed for the GBA capture path.
    // Give all 128 KiB of C to the sub engine: enough for a 256x256 direct-color BG.
    mem_setVramCMapping(MEM_VRAM_C_SUB_BG_00000);

    // Mode 5 + BG3, direct-color 256x256 bitmap at base 0.
    REG_DISPCNT_SUB = 0x00010805;
    REG_BG3CNT_SUB = 0x4084;
    gfx_setSubBg3Affine(256, 0, 0, 256, 0, 0);

    // GBARunner3 normally blanks the unused screen. kotoGba needs it visible.
    REG_MASTER_BRIGHT_SUB = 0;
    sysipc_setBottomBacklight(true);

    // Dark fallback if the SD asset is missing/corrupt.
    FillLowerScreen(0x8000 | 0x0842);
    const bool loaded = LoadRaw16(KOTOGBA_HOME_PATH);
    if (!loaded)
    {
        // A truncated file or a late I/O error may already have copied pixels.
        // Restore the entire fallback instead of leaving a partial home image.
        FillLowerScreen(0x8000 | 0x0842);
    }
    return loaded;
}
