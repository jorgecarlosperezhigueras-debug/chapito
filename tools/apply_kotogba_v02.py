#!/usr/bin/env python3
from pathlib import Path
import hashlib
import shutil
import sys

TARGET_COMMIT = "ecaa817815d9761745606592f04affe5ee9c3731"
MAIN_BLOB_SHA = "2aff810d2c3e8b04e451493ce169233673463c30"
VBLANK_BLOB_SHA = "98c00868a6d4e863d3d4e4becad46ac583dae129"
BOOTSTRAP_MAKEFILE_BLOB_SHA = "7a48f9d40305d473ad108b4e82ed11024418befd"

SERVICE_SHA256 = {
    "KotoGbaUiService.cpp": "0f14583d238b680f4bf0471ad49b0729d9ad5079e567714a3c660090e3a2ffa3",
    "KotoGbaUiService.h": "5b7adc6e54c2cc22e2c70d2a2a493dd8898f565fdb10f4d87fbf78dba5a672",
    "KotoGbaLauncherService.cpp": "68ae28d56cf594f278226f21c4cc70a486957c793448800717ca37930e85c319",
    "KotoGbaLauncherService.h": "fcfeee789c91b922c755f329ce9da7e2afb0df3631d7c0d431b93534ab741548",
}

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
HERE = Path(__file__).resolve().parents[1]
MAIN = ROOT / "code/core/arm9/source/main.cpp"
VBLANK = ROOT / "code/core/arm9/source/Emulator/VBlankIrq.s"
BOOTSTRAP_MAKEFILE = ROOT / "code/bootstrap/Makefile"
APP = ROOT / "code/core/arm9/source/Application"
PATCH_APP = HERE / "patch_files/code/core/arm9/source/Application"

def git_blob_sha(data: bytes) -> str:
    header = b"blob " + str(len(data)).encode("ascii") + b"\\0"
    return hashlib.sha1(header + data).hexdigest()

def require_file(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"No encuentro {path}; no escribo nada.")

for path in (MAIN, VBLANK, BOOTSTRAP_MAKEFILE):
    require_file(path)

if git_blob_sha(MAIN.read_bytes()) != MAIN_BLOB_SHA:
    raise SystemExit("main.cpp no coincide con ecaa817; no escribo nada.")
if git_blob_sha(VBLANK.read_bytes()) != VBLANK_BLOB_SHA:
    raise SystemExit("VBlankIrq.s no coincide con ecaa817; no escribo nada.")
if git_blob_sha(BOOTSTRAP_MAKEFILE.read_bytes()) != BOOTSTRAP_MAKEFILE_BLOB_SHA:
    raise SystemExit("bootstrap/Makefile no coincide con ecaa817; no escribo nada.")

for name, expected in SERVICE_SHA256.items():
    src = PATCH_APP / name
    require_file(src)
    if hashlib.sha256(src.read_bytes()).hexdigest() != expected:
        raise SystemExit(f"{name} no coincide con su SHA-256 esperado; no escribo nada.")

main_src = MAIN.read_text(encoding="utf-8")
include_anchor = '#include "Application/GbaBorderService.h"\\n'
if include_anchor not in main_src:
    raise SystemExit("No encuentro el ancla GbaBorderService.h; no escribo nada.")
main_src = main_src.replace(
    include_anchor,
    include_anchor
    + '#include "Application/KotoGbaLauncherService.h"\\n'
    + '#include "Application/KotoGbaUiService.h"\\n',
    1,
)

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

new_runtime = """    patch_resetSwiPatches();
    loadGbaBios();
    relocateGbaBios();
    applyBiosVmPatches();

    // kotoGba V0.2: when launched directly, choose a .gba from the SD card.
    // Preserve argv launching so TWiLight Menu++ or another frontend can still
    // pass a ROM directly.
    char selectedRomPath[256] { };
    const char* romPath = argc > 1 ? argv[1] : nullptr;
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

    if (sSplashScreen)
    {
        waitSplashScreenAnimation();
        stopSplashScreenAnimation();
        delete sSplashScreen;
        sSplashScreen = nullptr;
    }

    // kotoGba V0: keep GBA on the top physical screen and reserve the sub engine
    // for the learning UI. Disabling capture/centering also frees VRAM C.
    auto displaySettings = gAppSettingsService.GetAppSettings().displaySettings;
    displaySettings.gbaScreen = GbaScreen::Top;
    displaySettings.enableCenterAndMask = false;
    gGbaDisplayConfigurationService.ApplyDisplaySettings(displaySettings);
    gKotoGbaUiService.Initialize();
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
    // kotoGba V0: capture disabled; VRAM C belongs to SUB_BG.
    // Continue with the normal VBlank DMA/save path, skipping the selector store above.
    // This is replaced by a nop when no vblank dma is in use
"""
if entry_old not in vblank_src or skip_old not in vblank_src:
    raise SystemExit("VBlankIrq.s no contiene las anclas esperadas; no escribo nada.")
vblank_src = vblank_src.replace(entry_old, entry_new, 1).replace(skip_old, skip_new, 1)

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
    (main_src.count("gKotoGbaLauncherService.SelectRom") == 1, "selector"),
    (main_src.count("gKotoGbaUiService.Initialize();") == 1, "UI init"),
    (vblank_src.count("kotoGba V0: capture disabled; VRAM C belongs to SUB_BG.") == 1, "VBlank"),
    ("GAME_TITLE     := kotoGba" in makefile_src, "NDS title"),
]
for ok, label in checks:
    if not ok:
        raise SystemExit(f"Estado inesperado ({label}); no escribo nada.")

MAIN.write_text(main_src, encoding="utf-8")
VBLANK.write_text(vblank_src, encoding="utf-8")
BOOTSTRAP_MAKEFILE.write_text(makefile_src, encoding="utf-8")
APP.mkdir(parents=True, exist_ok=True)
for name in SERVICE_SHA256:
    shutil.copy2(PATCH_APP / name, APP / name)

print("PATCH_V02_OK")
print(f"BASE={TARGET_COMMIT}")
print("CAMBIOS=main.cpp,VBlankIrq.s,bootstrap/Makefile,KotoGbaUiService.*,KotoGbaLauncherService.*")
print("LAUNCHER=direct launch -> ROM browser; argv launch -> direct ROM")
