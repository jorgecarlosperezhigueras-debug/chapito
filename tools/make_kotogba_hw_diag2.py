#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
main = root / "code/core/arm9/source/main.cpp"
launcher = root / "code/core/arm9/source/Application/KotoGbaLauncherService.cpp"

main_src = main.read_text(encoding="utf-8")
launcher_src = launcher.read_text(encoding="utf-8")

anchor = "static SplashScreen* sSplashScreen;\n"
helper = r'''
static void kotoGbaHwDiagColor(u16 color)
{
    mem_setVramCMapping(MEM_VRAM_C_SUB_BG_00000);
    REG_DISPCNT_SUB = 0x00010805;
    REG_BG3CNT_SUB = 0x4084;
    gfx_setSubBg3Affine(256, 0, 0, 256, 0, 0);
    REG_MASTER_BRIGHT_SUB = 0;

    vu16* pixels = GFX_BG_SUB;
    for (u32 i = 0; i < 256u * 192u; ++i)
        pixels[i] = color;
}
'''
if helper.strip() not in main_src:
    if anchor not in main_src:
        raise SystemExit("core helper anchor missing")
    main_src = main_src.replace(anchor, anchor + helper, 1)

mount_success_anchor = """    if (!mountResult)
    {
        GFX_PLTT_BG_MAIN[0] = 0x1F << 10;
        while (1);
    }

"""
mount_success_new = """    if (!mountResult)
    {
        // RED: filesystem mount returned failure.
        kotoGbaHwDiagColor(0x8000 | 31);
        while (1);
    }

    // GREEN: filesystem mounted; continue into app startup.
    kotoGbaHwDiagColor(0x8000 | (31 << 5));

"""
if mount_success_anchor not in main_src:
    raise SystemExit("mount anchor missing")
main_src = main_src.replace(mount_success_anchor, mount_success_new, 1)

settings_anchor = """#ifndef KOTOGBA_LAUNCHER_PROBE
    gAppSettingsService.TryLoadAppSettings(SETTINGS_FILE_PATH);
#endif
"""
settings_new = """#ifndef KOTOGBA_LAUNCHER_PROBE
    // CYAN while loading global settings.
    kotoGbaHwDiagColor(0x8000 | (31 << 5) | (31 << 10));
    gAppSettingsService.TryLoadAppSettings(SETTINGS_FILE_PATH);
    // BLUE means global settings returned successfully.
    kotoGbaHwDiagColor(0x8000 | (31 << 10));
#endif
"""
if settings_anchor not in main_src:
    raise SystemExit("settings anchor missing")
main_src = main_src.replace(settings_anchor, settings_new, 1)

branch_anchor = """    if (!romPath)
    {
        waitSplashScreenAnimation();
        stopSplashScreenAnimation();
        delete sSplashScreen;
        sSplashScreen = nullptr;

        if (!gKotoGbaLauncherService.SelectRom(selectedRomPath, sizeof(selectedRomPath)))
            while (true);
        romPath = selectedRomPath;
    }
"""
branch_new = """    if (!romPath)
    {
        // YELLOW: direct-launch path selected, about to wait for the splash.
        kotoGbaHwDiagColor(0x8000 | 31 | (31 << 5));
        waitSplashScreenAnimation();

        // MAGENTA: splash wait returned.
        kotoGbaHwDiagColor(0x8000 | 31 | (31 << 10));
        stopSplashScreenAnimation();
        delete sSplashScreen;
        sSplashScreen = nullptr;

        // WHITE: about to enter the kotoGba ROM selector.
        kotoGbaHwDiagColor(0xFFFF);
        if (!gKotoGbaLauncherService.SelectRom(selectedRomPath, sizeof(selectedRomPath)))
            while (true);
        romPath = selectedRomPath;
    }
"""
if branch_anchor not in main_src:
    raise SystemExit("launcher branch anchor missing")
main_src = main_src.replace(branch_anchor, branch_new, 1)

launcher_anchor = """    // Lightweight launcher: direct-color framebuffer in VRAM C, no console/newlib UI.
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
"""
launcher_new = """    // Lightweight launcher: direct-color framebuffer in VRAM C, no console/newlib UI.
    mem_setVramCMapping(MEM_VRAM_C_SUB_BG_00000);
    REG_DISPCNT_SUB = 0x00010805;
    REG_BG3CNT_SUB = 0x4084;
    gfx_setSubBg3Affine(256, 0, 0, 256, 0, 0);
    REG_MASTER_BRIGHT = 0x8010;
    REG_MASTER_BRIGHT_SUB = 0;

    // ORANGE: launcher entered; about to request the bottom backlight.
    FillRect(0, 0, 256, 192, 0x8000 | 31 | (16 << 5));
    sysipc_setBottomBacklight(true);

    // PURPLE: backlight IPC returned; now probing initial directories.
    FillRect(0, 0, 256, 192, 0x8000 | 20 | (20 << 10));
    char currentPath[KOTOGBA_PATH_BYTES];
    ChooseInitialPath(currentPath, sizeof(currentPath));

    // TEAL: initial folder chosen; now reading its directory entries.
    FillRect(0, 0, 256, 192, 0x8000 | (20 << 5) | (20 << 10));
    int selected = 0;
    int count = ReadDirectory(currentPath);

    // If directory reading returns, the real launcher replaces the diagnostic colour.
    RenderLauncher(currentPath, count, selected);
"""
if launcher_anchor not in launcher_src:
    raise SystemExit("launcher init anchor missing")
launcher_src = launcher_src.replace(launcher_anchor, launcher_new, 1)

main.write_text(main_src, encoding="utf-8")
launcher.write_text(launcher_src, encoding="utf-8")
print("HW_DIAG2_PATCH_OK")
