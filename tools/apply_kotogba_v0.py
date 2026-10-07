#!/usr/bin/env python3
from pathlib import Path
import hashlib
import shutil
import sys

TARGET_COMMIT = "ecaa817815d9761745606592f04affe5ee9c3731"
MAIN_BLOB_SHA = "2aff810d2c3e8b04e451493ce169233673463c30"
VBLANK_BLOB_SHA = "98c00868a6d4e863d3d4e4becad46ac583dae129"
SERVICE_CPP_SHA256 = "0f14583d238b680f4bf0471ad49b0729d9ad5079e567714a3c660090e3a2ffa3"
SERVICE_H_SHA256 = "5b7adc6e54c2cc22e2c70d2a2a2a493dd8898f565fdb10f4d87fbf78dba5a672"

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
MAIN = ROOT / "code/core/arm9/source/main.cpp"
VBLANK = ROOT / "code/core/arm9/source/Emulator/VBlankIrq.s"
APP = ROOT / "code/core/arm9/source/Application"
HERE = Path(__file__).resolve().parents[1]


def git_blob_sha(data: bytes) -> str:
    header = b"blob " + str(len(data)).encode("ascii") + b"\0"
    return hashlib.sha1(header + data).hexdigest()


def require_file(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"No encuentro {path}. Usa el checkout completo de Gericom/GBARunner3 en {TARGET_COMMIT}.")


require_file(MAIN)
require_file(VBLANK)
SERVICE_FILES = {
    "KotoGbaUiService.cpp": SERVICE_CPP_SHA256,
    "KotoGbaUiService.h": SERVICE_H_SHA256,
}
for name, expected_sha256 in SERVICE_FILES.items():
    src = HERE / "patch_files/code/core/arm9/source/Application" / name
    require_file(src)
    if hashlib.sha256(src.read_bytes()).hexdigest() != expected_sha256:
        raise SystemExit(f"{name} del checkpoint no coincide con su SHA-256 esperado; no escribo nada.")

# 1) Main: force GBA top, disable center/mask, then initialize kotoGba bottom UI.
main_src = MAIN.read_text(encoding="utf-8")
main_already_patched = "gKotoGbaUiService.Initialize();" in main_src
if not main_already_patched:
    if git_blob_sha(MAIN.read_bytes()) != MAIN_BLOB_SHA:
        raise SystemExit("main.cpp no coincide con el blob del commit objetivo; no escribo nada.")

    include_anchor = '#include "Application/GbaBorderService.h"\n'
    if '#include "Application/KotoGbaUiService.h"' not in main_src:
        if include_anchor not in main_src:
            raise SystemExit("main.cpp no coincide con la base esperada: falta GbaBorderService.h")
        main_src = main_src.replace(
            include_anchor,
            include_anchor + '#include "Application/KotoGbaUiService.h"\n',
            1,
        )

    old = '''    const auto& displaySettings = gAppSettingsService.GetAppSettings().displaySettings;\n    gGbaDisplayConfigurationService.ApplyDisplaySettings(displaySettings);\n    if (displaySettings.enableCenterAndMask)\n    {\n        gGbaBorderService.SetupBorder(displaySettings.borderImage, gRomHeader.gameCode);\n    }\n'''
    new = '''    // kotoGba V0: keep GBA on the top physical screen and reserve the sub engine\n    // for the learning UI. Disabling capture/centering also frees VRAM C.\n    auto displaySettings = gAppSettingsService.GetAppSettings().displaySettings;\n    displaySettings.gbaScreen = GbaScreen::Top;\n    displaySettings.enableCenterAndMask = false;\n    gGbaDisplayConfigurationService.ApplyDisplaySettings(displaySettings);\n    gKotoGbaUiService.Initialize();\n'''
    if old not in main_src:
        raise SystemExit("main.cpp no coincide con el bloque de pantalla del commit objetivo; no escribo nada.")
    main_src = main_src.replace(old, new, 1)

# 2) VBlank: pinned ecaa817 toggles VRAM C/D capture every VBlank regardless of the setting.
# kotoGba forces center-and-mask off, so skip that capture path entirely. This keeps VRAM C
# mapped to SUB_BG for the lower UI instead of having the IRQ remap it one frame later.
vblank_src = VBLANK.read_text(encoding="utf-8")
vblank_marker = "kotoGba V0: capture disabled; VRAM C belongs to SUB_BG."
if vblank_marker not in vblank_src:
    if git_blob_sha(VBLANK.read_bytes()) != VBLANK_BLOB_SHA:
        raise SystemExit("VBlankIrq.s no coincide con el blob del commit objetivo; no escribo nada.")
    entry_old = '''jumpToCaptureUpdate:\n    nop\n'''
    entry_new = '''jumpToCaptureUpdate:\n    // kotoGba V0: keep the IRQ capture path unreachable so VRAM C stays SUB_BG.\n    b kotogba_skipDisplayCapture\n'''
    skip_old = '''checkSaveWrite:\n    str r13, jumpToCaptureUpdate\n\n    // This is replaced by a nop when no vblank dma is in use\n'''
    skip_new = '''checkSaveWrite:\n    str r13, jumpToCaptureUpdate\n\nkotogba_skipDisplayCapture:\n    // kotoGba V0: capture disabled; VRAM C belongs to SUB_BG.\n    // Continue with the normal VBlank DMA/save path, skipping the selector store above.\n    // This is replaced by a nop when no vblank dma is in use\n'''
    if entry_old not in vblank_src or skip_old not in vblank_src:
        raise SystemExit("VBlankIrq.s no contiene las anclas de captura esperadas; no escribo nada.")
    vblank_src = vblank_src.replace(entry_old, entry_new, 1).replace(skip_old, skip_new, 1)

# Validate desired state before writing any source file.
if main_src.count('#include "Application/KotoGbaUiService.h"') != 1:
    raise SystemExit("Estado inesperado: include kotoGba duplicado; no escribo nada.")
if main_src.count("gKotoGbaUiService.Initialize();") != 1:
    raise SystemExit("Estado inesperado: inicialización kotoGba ausente/duplicada; no escribo nada.")
if vblank_src.count(vblank_marker) != 1:
    raise SystemExit("Estado inesperado: parche VBlank ausente/duplicado; no escribo nada.")

MAIN.write_text(main_src, encoding="utf-8")
VBLANK.write_text(vblank_src, encoding="utf-8")
APP.mkdir(parents=True, exist_ok=True)
for name in SERVICE_FILES:
    shutil.copy2(HERE / "patch_files/code/core/arm9/source/Application" / name, APP / name)

print("PATCH_OK")
print(f"BASE={TARGET_COMMIT}")
print("CAMBIOS=main.cpp,KotoGbaUiService.h,KotoGbaUiService.cpp,VBlankIrq.s")
print("Copia además sd/_gba/kotogba/home.raw16 a la SD conservando esa ruta.")
