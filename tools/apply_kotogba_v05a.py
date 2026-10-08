#!/usr/bin/env python3
"""Apply the V0.5-B automatic-help layer on top of the approved V0.3 runtime."""

from pathlib import Path
import hashlib
import shutil
import subprocess
import sys

TARGET_COMMIT = "ecaa817815d9761745606592f04affe5ee9c3731"

V05_SERVICE_BLOB_SHA = {
    "KotoGbaRuntimeService.cpp": "0a0813ac89245d069ea7c12f2cd28f7e0108ff24",
    "KotoGbaRuntimeService.h": "9e310e75a421fd69923016f7532e9d31708a7fc8",
    "KotoGbaDetector.inc": "6e859375e75b7c24244bb9f17c36f6fe2588f1c3",
    "KotoGbaDetector.s": "46f6951c924ced321cc794233de7a5ad6c594c56",
    "KotoGbaLatinFont6x10.h": "eb185228168315c3cc4b1ec914f84b6e12e19431",
}

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
HERE = Path(__file__).resolve().parents[1]
BASE_PATCH = HERE / "tools/apply_kotogba_v03.py"
MAIN = ROOT / "code/core/arm9/source/main.cpp"
VBLANK = ROOT / "code/core/arm9/source/Emulator/VBlankIrq.s"
MEMLOAD8 = ROOT / "code/core/arm9/source/MemoryEmulator/MemoryLoad8.s"
MEMLOADROM = ROOT / "code/core/arm9/source/MemoryEmulator/MemoryLoadRom.s"
APP = ROOT / "code/core/arm9/source/Application"
PATCH_APP = HERE / "patch_files/code/core/arm9/source/Application"


def git_blob_sha(data: bytes) -> str:
    header = b"blob " + str(len(data)).encode("ascii") + b"\0"
    return hashlib.sha1(header + data).hexdigest()


def require_file(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f"No encuentro {path}; no escribo nada.")


require_file(BASE_PATCH)
for name, expected in V05_SERVICE_BLOB_SHA.items():
    source = PATCH_APP / name
    require_file(source)
    actual = git_blob_sha(source.read_bytes())
    if actual != expected:
        raise SystemExit(
            f"{name} no coincide con el blob V0.5-B esperado "
            f"({actual} != {expected}); no escribo nada."
        )

# Reuse the exact hardware-approved launcher, package resolver, screen setup,
# MPU handling and bootstrap changes instead of duplicating that patch here.
subprocess.run(
    [sys.executable, str(BASE_PATCH), str(ROOT)],
    check=True,
)

for path in (MAIN, VBLANK, MEMLOAD8, MEMLOADROM):
    require_file(path)

main_src = MAIN.read_text(encoding="utf-8")

# V0.5 runtime needs the package-installed flag after GBARunner3 restores
# EWRAM to non-executable. Read it while the hardware-approved V0.3 MPU
# execution window is still open, then carry only the bool forward.
package_anchor = "    gKotoGbaLauncherService.ResolvePackageForHeader(gRomHeader);\n"
package_replacement = """    gKotoGbaLauncherService.ResolvePackageForHeader(gRomHeader);
    const bool kotoPackageInstalled =
        gKotoGbaLauncherService.GetPackageInfo().installed;
"""
if main_src.count(package_anchor) != 1:
    raise SystemExit("No encuentro una única resolución de paquete V0.3; no escribo nada.")
main_src = main_src.replace(package_anchor, package_replacement, 1)
include_anchor = '#include "Application/KotoGbaUiService.h"\n'
if main_src.count(include_anchor) != 1:
    raise SystemExit("No encuentro un único include de KotoGbaUiService; no escribo nada.")
main_src = main_src.replace(
    include_anchor,
    include_anchor + '#include "Application/KotoGbaRuntimeService.h"\n',
    1,
)

ui_anchor = "    gKotoGbaUiService.Initialize();\n"
ui_replacement = """    gKotoGbaUiService.Initialize();
    gKotoGbaRuntimeService.Initialize(
        gRomHeader.gameCode, gRomHeader.softwareVersion,
        kotoPackageInstalled);
"""
if main_src.count(ui_anchor) != 1:
    raise SystemExit("No encuentro una única inicialización de la UI V0.3; no escribo nada.")
main_src = main_src.replace(ui_anchor, ui_replacement, 1)

vblank_src = VBLANK.read_text(encoding="utf-8")
vblank_old = """kotogba_skipDisplayCapture:
    // kotoGba V0: capture disabled; VRAM C belongs to SUB_BG.
    // Continue with the normal VBlank DMA/save path, skipping the selector store above.
    // This is replaced by a nop when no vblank dma is in use
"""
vblank_new = """kotogba_skipDisplayCapture:
    // kotoGba V0.5-B: capture disabled; VRAM C belongs to SUB_BG.
    // Refresh automatic help only when the detector publishes a new card.
    ldr sp,= dtcmIrqStackEnd
    push {r0-r3,r12,lr}
    bl kotogba_vblankUpdate
    pop {r0-r3,r12,lr}

    // Continue with the normal VBlank DMA/save path.
    // This is replaced by a nop when no vblank dma is in use
"""
if vblank_src.count(vblank_old) != 1:
    raise SystemExit("VBlankIrq.s no contiene el bloque V0.3 esperado; no escribo nada.")
vblank_src = vblank_src.replace(vblank_old, vblank_new, 1)

memload8_src = MEMLOAD8.read_text(encoding="utf-8")
memload8_include = '#include "MemoryEmulator/MemoryLoadStoreTableDefs.inc"\n'
if memload8_src.count(memload8_include) != 1:
    raise SystemExit("MemoryLoad8.s no contiene el include esperado; no escribo nada.")
memload8_src = memload8_src.replace(
    memload8_include,
    memload8_include + '#include "Application/KotoGbaDetector.inc"\n',
    1,
)

memload8_handler = """arm_func memu_load8Ewram
    cmp r8, #ROM_LINEAR_END_DS_ADDRESS
"""
memload8_patched = """arm_func memu_load8Ewram
    kotogba_detectIntroText
    cmp r8, #ROM_LINEAR_END_DS_ADDRESS
"""
if memload8_src.count(memload8_handler) != 1:
    raise SystemExit("MemoryLoad8.s no contiene el handler EWRAM esperado; no escribo nada.")
memload8_src = memload8_src.replace(memload8_handler, memload8_patched, 1)

memloadrom_src = MEMLOADROM.read_text(encoding="utf-8")
memloadrom_include = '#include "SdCache/SdCacheDefs.h"\n'
if memloadrom_src.count(memloadrom_include) != 1:
    raise SystemExit("MemoryLoadRom.s no contiene el include esperado; no escribo nada.")
memloadrom_src = memloadrom_src.replace(
    memloadrom_include,
    memloadrom_include + '#include "Application/KotoGbaDetector.inc"\n',
    1,
)

memloadrom_handler = """arm_func memu_load8Rom
    ldr r11, DTCM(memu_adjustedRomBlockToCacheBlockAddress)
"""
memloadrom_patched = """arm_func memu_load8Rom
    kotogba_detectIntroTextFixedRom
.global kotogba_load8RomAfterDetector
kotogba_load8RomAfterDetector:
"""
if memloadrom_src.count(memloadrom_handler) != 1:
    raise SystemExit("MemoryLoadRom.s no contiene el handler ROM esperado; no escribo nada.")
memloadrom_src = memloadrom_src.replace(memloadrom_handler, memloadrom_patched, 1)

checks = [
    (main_src.count('#include "Application/KotoGbaRuntimeService.h"') == 1, "include runtime"),
    (main_src.count("gKotoGbaRuntimeService.Initialize(") == 1, "runtime init"),
    (vblank_src.count("bl kotogba_vblankUpdate") == 1, "VBlank runtime update"),
    (memload8_src.count("kotogba_detectIntroText") == 1, "linear detector hook"),
    (memloadrom_src.count("kotogba_detectIntroTextFixedRom") == 1, "cached detector hook"),
]
for ok, label in checks:
    if not ok:
        raise SystemExit(f"Estado inesperado ({label}); no escribo nada.")

MAIN.write_text(main_src, encoding="utf-8")
VBLANK.write_text(vblank_src, encoding="utf-8")
MEMLOAD8.write_text(memload8_src, encoding="utf-8")
MEMLOADROM.write_text(memloadrom_src, encoding="utf-8")
APP.mkdir(parents=True, exist_ok=True)
for name in V05_SERVICE_BLOB_SHA:
    shutil.copy2(PATCH_APP / name, APP / name)

print("PATCH_V05A_OK")
print(f"BASE={TARGET_COMMIT}")
print("BASE_RUNTIME=hardware-approved V0.3 patch")
print("DETECTOR=FireRed JP Rev1 I001-I005 byte-read detector with LR preservation")
print("UI=automatic Spanish translation on lower screen")
