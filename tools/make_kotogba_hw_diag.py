#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
main = root / "code/core/arm9/source/main.cpp"
bootstrap = root / "code/bootstrap/arm9/source/main.cpp"

main_src = main.read_text(encoding="utf-8")
boot_src = bootstrap.read_text(encoding="utf-8")

# Core-side full-screen diagnostic colour on the lower display.
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
        raise SystemExit("core anchor missing")
    main_src = main_src.replace(anchor, anchor + helper, 1)

start_anchor = "    startSplashScreenAnimation();\n"
start_new = """    startSplashScreenAnimation();

    // HW DIAG: orange means core ARM9 entered and is about to mount storage.
    kotoGbaHwDiagColor(0x8000 | 31 | (16 << 5));
"""
if start_new not in main_src:
    if start_anchor not in main_src:
        raise SystemExit("start splash anchor missing")
    main_src = main_src.replace(start_anchor, start_new, 1)

failure_old = """    if (!mountResult)
    {
        GFX_PLTT_BG_MAIN[0] = 0x1F << 10;
        while (1);
    }
"""
failure_new = """    if (!mountResult)
    {
        // HW DIAG: red means the filesystem mount returned failure.
        kotoGbaHwDiagColor(0x8000 | 31);
        while (1);
    }

    // HW DIAG: green means storage mounted successfully.
    kotoGbaHwDiagColor(0x8000 | (31 << 5));
    while (1);
"""
if failure_old not in main_src:
    raise SystemExit("mount failure anchor missing")
main_src = main_src.replace(failure_old, failure_new, 1)

# Bootstrap-side blue: if this remains visible, bootstrap reached the core jump
# but the core did not get as far as the orange stage.
boot_anchor = """static void loadSplashScreen()
{
"""
diag_helper = r'''
static void kotoGbaBootstrapDiagBlue()
{
    videoSetModeSub(MODE_5_2D | DISPLAY_BG3_ACTIVE);
    vramSetBankC(VRAM_C_SUB_BG);
    const int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    u16* pixels = bgGetGfxPtr(bg);
    const u16 blue = ARGB16(1, 0, 0, 31);
    for (int i = 0; i < 256 * 192; ++i)
        pixels[i] = blue;
}

'''
if diag_helper.strip() not in boot_src:
    if boot_anchor not in boot_src:
        raise SystemExit("bootstrap helper anchor missing")
    boot_src = boot_src.replace(boot_anchor, diag_helper + boot_anchor, 1)

call_old = """    loadSplashScreen();

    DC_FlushAll();
"""
call_new = """    loadSplashScreen();
    // HW DIAG: blue means bootstrap is alive and about to jump to the core.
    kotoGbaBootstrapDiagBlue();

    DC_FlushAll();
"""
if call_old not in boot_src:
    raise SystemExit("bootstrap call anchor missing")
boot_src = boot_src.replace(call_old, call_new, 1)

main.write_text(main_src, encoding="utf-8")
bootstrap.write_text(boot_src, encoding="utf-8")
print("HW_DIAG_PATCH_OK")
