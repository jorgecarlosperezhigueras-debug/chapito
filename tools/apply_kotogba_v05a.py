#!/usr/bin/env python3
from pathlib import Path
import hashlib
import shutil
import sys

TARGET_COMMIT = "ecaa817815d9761745606592f04affe5ee9c3731"
MAIN_BLOB_SHA = "2aff810d2c3e8b04e451493ce169233673463c30"
VBLANK_BLOB_SHA = "98c00868a6d4e863d3d4e4becad46ac583dae129"
MEMLOAD8_BLOB_SHA = "acec9990cf19114e8e27bd7541e8855b53d64547"
MEMLOADROM_BLOB_SHA = "26ce58159674ba10dbe04a6b5a1bbd8d40af64de"
BOOTSTRAP_MAKEFILE_BLOB_SHA = "7a48f9d40305d473ad108b4e82ed11024418befd"
BOOTSTRAP_MAIN_BLOB_SHA = "92d196ccf69b8d397f6697725d440576f51f10ef"

SERVICE_BLOB_SHA = {
    "KotoGbaUiService.cpp": "aa82ffc625b672ca2d78e23b6f66fb109360a919",
    "KotoGbaUiService.h": "75fbc437801bdc315c2a9e584afe3f28acb97b14",
    "KotoGbaLauncherService.cpp": "f99edaa7bd8b8d8955b881d4fd049ba16d166efc",
    "KotoGbaLauncherService.h": "770103bb8ee997406ba2b71439ea266c370f8aa7",
    "KotoGbaRuntimeService.cpp": "fb8d4b06e134197b14fea840b8cc33fa14e30413",
    "KotoGbaRuntimeService.h": "9e310e75a421fd69923016f7532e9d31708a7fc8",
    "KotoGbaDetector.inc": "6e859375e75b7c24244bb9f17c36f6fe2588f1c3",
    "KotoGbaDetector.s": "fd81f1be275d65d6b21758b1cd07181cf29310e2",
}

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
HERE = Path(__file__).resolve().parents[1]
MAIN = ROOT / "code/core/arm9/source/main.cpp"
VBLANK = ROOT / "code/core/arm9/source/Emulator/VBlankIrq.s"
MEMLOAD8 = ROOT / "code/core/arm9/source/MemoryEmulator/MemoryLoad8.s"
MEMLOADROM = ROOT / "code/core/arm9/source/MemoryEmulator/MemoryLoadRom.s"
BOOTSTRAP_MAKEFILE = ROOT / "code/bootstrap/Makefile"
BOOTSTRAP_MAIN = ROOT / "code/bootstrap/arm9/source/main.cpp"
APP = ROOT / "code/core/arm9/source/Application"
PATCH_APP = HERE / "patch_files/code/core/arm9/source/Application"

def git_blob_sha(data: bytes) -> str:
    header = b"blob " + str(len(data)).encode("ascii") + b"\0"
    return hashlib.sha1(header + data).hexdigest()

def require_file(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"No encuentro {path}; no escribo nada.")

for path in (MAIN, VBLANK, MEMLOAD8, MEMLOADROM, BOOTSTRAP_MAKEFILE, BOOTSTRAP_MAIN):
    require_file(path)

if git_blob_sha(MAIN.read_bytes()) != MAIN_BLOB_SHA:
    raise SystemExit("main.cpp no coincide con ecaa817; no escribo nada.")
if git_blob_sha(VBLANK.read_bytes()) != VBLANK_BLOB_SHA:
    raise SystemExit("VBlankIrq.s no coincide con ecaa817; no escribo nada.")
if git_blob_sha(MEMLOAD8.read_bytes()) != MEMLOAD8_BLOB_SHA:
    raise SystemExit("MemoryLoad8.s no coincide con ecaa817; no escribo nada.")
if git_blob_sha(MEMLOADROM.read_bytes()) != MEMLOADROM_BLOB_SHA:
    raise SystemExit("MemoryLoadRom.s no coincide con ecaa817; no escribo nada.")
if git_blob_sha(BOOTSTRAP_MAKEFILE.read_bytes()) != BOOTSTRAP_MAKEFILE_BLOB_SHA:
    raise SystemExit("bootstrap/Makefile no coincide con ecaa817; no escribo nada.")
if git_blob_sha(BOOTSTRAP_MAIN.read_bytes()) != BOOTSTRAP_MAIN_BLOB_SHA:
    raise SystemExit("bootstrap/arm9/source/main.cpp no coincide con ecaa817; no escribo nada.")

for name, expected in SERVICE_BLOB_SHA.items():
    src = PATCH_APP / name
    require_file(src)
    if git_blob_sha(src.read_bytes()) != expected:
        raise SystemExit(f"{name} no coincide con el blob versionado esperado; no escribo nada.")

main_src = MAIN.read_text(encoding="utf-8")
include_anchor = '#include "Application/GbaBorderService.h"\n'
if include_anchor not in main_src:
    raise SystemExit("No encuentro el ancla GbaBorderService.h; no escribo nada.")
main_src = main_src.replace(
    include_anchor,
    include_anchor
    + '#include "Application/KotoGbaLauncherService.h"\n'
    + '#include "Application/KotoGbaUiService.h"\n'
    + '#include "Application/KotoGbaRuntimeService.h"\n',
    1,
)

mount_old = """    bool mountResult;
    if (shouldMountDsiSd(argc, argv))
        mountResult = mountDsiSd();
    else
        mountResult = mountDldi();

    if (!mountResult)
    {
        GFX_PLTT_BG_MAIN[0] = 0x1F << 10;
        while (1);
    }
"""
mount_new = """#ifdef KOTOGBA_LAUNCHER_PROBE
    // Emulator-only UI probe: bypass storage mounting so the real launcher
    // can be rendered with synthetic entries without DLDI.
    bool mountResult = true;
#else
    bool mountResult;
    if (shouldMountDsiSd(argc, argv))
        mountResult = mountDsiSd();
    else
        mountResult = mountDldi();
#endif

    if (!mountResult)
    {
        GFX_PLTT_BG_MAIN[0] = 0x1F << 10;
        while (1);
    }
"""
if mount_old not in main_src:
    raise SystemExit("main.cpp no contiene el bloque de montaje esperado; no escribo nada.")
main_src = main_src.replace(mount_old, mount_new, 1)

settings_old = "    gAppSettingsService.TryLoadAppSettings(SETTINGS_FILE_PATH);\n"
settings_new = """#ifndef KOTOGBA_LAUNCHER_PROBE
    gAppSettingsService.TryLoadAppSettings(SETTINGS_FILE_PATH);
#endif
"""
if settings_old not in main_src:
    raise SystemExit("main.cpp no contiene la carga de settings esperada; no escribo nada.")
main_src = main_src.replace(settings_old, settings_new, 1)

old_runtime = """    patch_resetSwiPatches();
    loadGbaBios();
    relocateGbaBios();
    applyBiosVmPatches();
    const char* romPath = argc > 1 ? argv[1] : DEFAULT_ROM_FILE_PATH;
    loadGbaRom(romPath);
    char* romExtension = strrchr(romPath, '.');
    if (romExtension)
    {
        romExtension[1] = 's';
        romExtension[2] = 'a';
        romExtension[3] = 'v';
        romExtension[4] = '\\0';
    }
    loadGameSpecificSettings();
    handleSave(romPath);
    SelfModifyingPatches().ApplyPatches(gAppSettingsService.GetAppSettings().runSettings);

    waitSplashScreenAnimation();
    stopSplashScreenAnimation();
    delete sSplashScreen;
    sSplashScreen = nullptr;

    const auto& displaySettings = gAppSettingsService.GetAppSettings().displaySettings;
    gGbaDisplayConfigurationService.ApplyDisplaySettings(displaySettings);
    if (displaySettings.enableCenterAndMask)
    {
        gGbaBorderService.SetupBorder(displaySettings.borderImage, gRomHeader.gameCode);
    }
"""

new_runtime = """    // kotoGba launcher: choose the ROM immediately after storage is mounted.
    // argv launching remains supported for TWiLight Menu++ and other frontends.
    char selectedRomPath[256] { };
    const char* romPath = nullptr;

    // kotoGba launcher/package code is linked into host EWRAM (0x02040000).
    // GBARunner3 normally marks main memory as data-only for instruction fetches.
    // Allow execution only while the launcher/package resolver is active.
    mpu_setRegionInstructionAccessPermission(
        MPU_REGION_1, MPU_ACCESS_PERMISSION_PRIV_READ_WRITE);

    // Some DS frontends pass their own extra arguments. Only treat argv[1]
    // as a direct game path when it is actually a .gba file; otherwise show
    // the kotoGba launcher.
    if (argc > 1 && argv[1])
    {
        const char* argExtension = strrchr(argv[1], '.');
        if (argExtension &&
            (argExtension[1] == 'g' || argExtension[1] == 'G') &&
            (argExtension[2] == 'b' || argExtension[2] == 'B') &&
            (argExtension[3] == 'a' || argExtension[3] == 'A') &&
            argExtension[4] == '\\0')
        {
            romPath = argv[1];
        }
    }

    if (!romPath)
    {
        waitSplashScreenAnimation();
        stopSplashScreenAnimation();
        delete sSplashScreen;
        sSplashScreen = nullptr;

        if (!gKotoGbaLauncherService.SelectRom(selectedRomPath, sizeof(selectedRomPath)))
            while (true);
        romPath = selectedRomPath;
    }

    patch_resetSwiPatches();
    loadGbaBios();
    relocateGbaBios();
    applyBiosVmPatches();
    loadGbaRom(romPath);

    // Resolve the canonical .koto package from the loaded ROM header so direct
    // argv launching and launcher selection behave identically.
    gKotoGbaLauncherService.ResolvePackageForHeader(gRomHeader);

    // Restore GBARunner3's normal protection before entering emulation.
    mpu_setRegionInstructionAccessPermission(
        MPU_REGION_1, MPU_ACCESS_PERMISSION_NONE);

    char savePath[512] { };
    strncpy(savePath, romPath, sizeof(savePath) - 1);
    char* romExtension = strrchr(savePath, '.');
    if (romExtension)
    {
        romExtension[1] = 's';
        romExtension[2] = 'a';
        romExtension[3] = 'v';
        romExtension[4] = '\\0';
    }
    loadGameSpecificSettings();
    handleSave(savePath);
    SelfModifyingPatches().ApplyPatches(gAppSettingsService.GetAppSettings().runSettings);

    if (sSplashScreen)
    {
        waitSplashScreenAnimation();
        stopSplashScreenAnimation();
        delete sSplashScreen;
        sSplashScreen = nullptr;
    }

    // kotoGba keeps GBA on the top physical screen and reserves the sub engine
    // for the learning UI. Disabling capture/centering also frees VRAM C.
    auto displaySettings = gAppSettingsService.GetAppSettings().displaySettings;
    displaySettings.gbaScreen = GbaScreen::Top;
    displaySettings.enableCenterAndMask = false;
    gGbaDisplayConfigurationService.ApplyDisplaySettings(displaySettings);
    gKotoGbaUiService.Initialize();
    gKotoGbaRuntimeService.Initialize(
        gRomHeader.gameCode, gRomHeader.softwareVersion,
        gKotoGbaLauncherService.GetPackageInfo().installed);
"""
if old_runtime not in main_src:
    raise SystemExit("main.cpp no contiene el bloque de runtime esperado; no escribo nada.")
main_src = main_src.replace(old_runtime, new_runtime, 1)

vblank_src = VBLANK.read_text(encoding="utf-8")
entry_old = """jumpToCaptureUpdate:
    nop
"""
entry_new = """jumpToCaptureUpdate:
    // kotoGba V0: keep the IRQ capture path unreachable so VRAM C stays SUB_BG.
    b kotogba_skipDisplayCapture
"""
skip_old = """checkSaveWrite:
    str r13, jumpToCaptureUpdate

    // This is replaced by a nop when no vblank dma is in use
"""
skip_new = """checkSaveWrite:
    str r13, jumpToCaptureUpdate

kotogba_skipDisplayCapture:
    // kotoGba V0.5-A: capture disabled; VRAM C belongs to SUB_BG.
    // Refresh the learning UI once per VBlank. The routine lives in ITCM and
    // returns immediately unless the detector has published a new card id.
    ldr sp,= dtcmIrqStackEnd
    push {r0-r3,r12,lr}
    bl kotogba_vblankUpdate
    pop {r0-r3,r12,lr}

    // Continue with the normal VBlank DMA/save path.
    // This is replaced by a nop when no vblank dma is in use
"""
if entry_old not in vblank_src or skip_old not in vblank_src:
    raise SystemExit("VBlankIrq.s no contiene las anclas esperadas; no escribo nada.")
vblank_src = vblank_src.replace(entry_old, entry_new, 1).replace(skip_old, skip_new, 1)

memload8_src = MEMLOAD8.read_text(encoding="utf-8")
memloadrom_src = MEMLOADROM.read_text(encoding="utf-8")

memload8_include_old = '#include "MemoryEmulator/MemoryLoadStoreTableDefs.inc"\n'
memload8_include_new = (
    memload8_include_old + '#include "Application/KotoGbaDetector.inc"\n'
)
if memload8_include_old not in memload8_src:
    raise SystemExit("MemoryLoad8.s no contiene el ancla de includes esperada; no escribo nada.")
memload8_src = memload8_src.replace(memload8_include_old, memload8_include_new, 1)

memloadrom_include_old = '#include "SdCache/SdCacheDefs.h"\n'
memloadrom_include_new = (
    memloadrom_include_old + '#include "Application/KotoGbaDetector.inc"\n'
)
if memloadrom_include_old not in memloadrom_src:
    raise SystemExit("MemoryLoadRom.s no contiene el ancla de includes esperada; no escribo nada.")
memloadrom_src = memloadrom_src.replace(memloadrom_include_old, memloadrom_include_new, 1)

memload8_ewram_old = """arm_func memu_load8Ewram
    cmp r8, #ROM_LINEAR_END_DS_ADDRESS
"""
memload8_ewram_new = """arm_func memu_load8Ewram
    kotogba_detectIntroText
    cmp r8, #ROM_LINEAR_END_DS_ADDRESS
"""
if memload8_ewram_old not in memload8_src:
    raise SystemExit("MemoryLoad8.s no contiene el handler EWRAM esperado; no escribo nada.")
memload8_src = memload8_src.replace(memload8_ewram_old, memload8_ewram_new, 1)

memloadrom_rom_old = """arm_func memu_load8Rom
    ldr r11, DTCM(memu_adjustedRomBlockToCacheBlockAddress)
"""
memloadrom_rom_new = """arm_func memu_load8Rom
    kotogba_detectIntroTextFixedRom
.global kotogba_load8RomAfterDetector
kotogba_load8RomAfterDetector:
"""
if memloadrom_rom_old not in memloadrom_src:
    raise SystemExit("MemoryLoadRom.s no contiene el handler ROM esperado; no escribo nada.")
memloadrom_src = memloadrom_src.replace(memloadrom_rom_old, memloadrom_rom_new, 1)

bootstrap_main_src = BOOTSTRAP_MAIN.read_text(encoding="utf-8")
bootstrap_ipc_old = """    initIpc();
    tryInitDldi();
"""
bootstrap_ipc_new = """    initIpc();
#ifndef KOTOGBA_LAUNCHER_PROBE
    tryInitDldi();
#endif
"""
if bootstrap_ipc_old not in bootstrap_main_src:
    raise SystemExit("bootstrap ARM9 no contiene la inicializacion DLDI esperada; no escribo nada.")
bootstrap_main_src = bootstrap_main_src.replace(bootstrap_ipc_old, bootstrap_ipc_new, 1)

makefile_src = BOOTSTRAP_MAKEFILE.read_text(encoding="utf-8")
old_meta = """GAME_TITLE     := GBARunner 3
GAME_SUBTITLE1 := By Gericom
GAME_SUBTITLE2 :=
"""
new_meta = """GAME_TITLE     := kotoGba
GAME_SUBTITLE1 := Japanese GBA learning
GAME_SUBTITLE2 := GBARunner3 core
"""
if old_meta not in makefile_src:
    raise SystemExit("bootstrap/Makefile no contiene los metadatos esperados; no escribo nada.")
makefile_src = makefile_src.replace(old_meta, new_meta, 1)

checks = [
    (main_src.count('#include "Application/KotoGbaLauncherService.h"') == 1, "include launcher"),
    (main_src.count('#include "Application/KotoGbaUiService.h"') == 1, "include UI"),
    (main_src.count('#include "Application/KotoGbaRuntimeService.h"') == 1, "include runtime"),
    (main_src.count("gKotoGbaLauncherService.SelectRom") == 1, "selector"),
    (main_src.count("gKotoGbaLauncherService.ResolvePackageForHeader(gRomHeader);") == 1, "package association"),
    (main_src.count("gKotoGbaUiService.Initialize();") == 1, "UI init"),
    (main_src.count("gKotoGbaRuntimeService.Initialize(") == 1, "runtime init"),
    (memload8_src.count("kotogba_detectIntroText") == 1, "linear ROM detector hook"),
    (memloadrom_src.count("kotogba_detectIntroText") == 1, "cached ROM detector hook"),
    (main_src.count("MPU_REGION_1, MPU_ACCESS_PERMISSION_PRIV_READ_WRITE") == 1, "EWRAM execution enable"),
    (main_src.count("MPU_REGION_1, MPU_ACCESS_PERMISSION_NONE") == 1, "EWRAM execution restore"),
    (vblank_src.count("kotoGba V0.5-A: capture disabled; VRAM C belongs to SUB_BG.") == 1, "VBlank"),
    (vblank_src.count("bl kotogba_vblankUpdate") == 1, "VBlank runtime update"),
    ("GAME_TITLE     := kotoGba" in makefile_src, "NDS title"),
    (bootstrap_main_src.count("#ifndef KOTOGBA_LAUNCHER_PROBE") == 1, "bootstrap probe guard"),
]
for ok, label in checks:
    if not ok:
        raise SystemExit(f"Estado inesperado ({label}); no escribo nada.")

MAIN.write_text(main_src, encoding="utf-8")
VBLANK.write_text(vblank_src, encoding="utf-8")
MEMLOAD8.write_text(memload8_src, encoding="utf-8")
MEMLOADROM.write_text(memloadrom_src, encoding="utf-8")
BOOTSTRAP_MAKEFILE.write_text(makefile_src, encoding="utf-8")
BOOTSTRAP_MAIN.write_text(bootstrap_main_src, encoding="utf-8")
APP.mkdir(parents=True, exist_ok=True)
for name in SERVICE_BLOB_SHA:
    shutil.copy2(PATCH_APP / name, APP / name)

print("PATCH_V05A_OK")
print(f"BASE={TARGET_COMMIT}")
print("CAMBIOS=main.cpp,VBlankIrq.s,MemoryLoad8.s,MemoryLoadRom.s,bootstrap/Makefile,bootstrap/arm9/main.cpp,KotoGbaUiService.*,KotoGbaLauncherService.*,KotoGbaRuntimeService.*")
print("DETECTOR=FireRed JP Rev1 I001-I005 automatic byte-read detector")
print("PACKAGE_ALIAS=<GAMECODE>_<REV_HEX>.koto")
print("LAUNCHER=direct launch -> ROM browser; argv launch -> direct ROM")
