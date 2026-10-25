#!/usr/bin/env bash
# Rebuild Hito B using only this checkpoint and its retained ARM SDK.
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"
sdk_dir="${KOTO_SDK_ROOT:-$project_dir/entorno/sdk}"
if [[ -z "${DEVKITPRO:-}" ]]; then
  if [[ ! -d "$sdk_dir/toolchain/opt/devkitpro" ]]; then
    sdk_archive="$project_dir/entorno/sdk_linux_R3_conservado.tar.gz"
    python3 - "$sdk_archive" <<'PY'
import hashlib,sys
from pathlib import Path
p=Path(sys.argv[1]);expected='ed3ee8ec7f8c0c088e8f1110715cef8aa4fdd3b998829d129f2bf614d901bb4f'
if hashlib.sha256(p.read_bytes()).hexdigest()!=expected:raise SystemExit('The retained SDK archive SHA-256 differs.')
PY
    mkdir -p -- "$sdk_dir"
    tar -xzf "$sdk_archive" -C "$sdk_dir"
  fi
  export DEVKITPRO="$sdk_dir/toolchain/opt/devkitpro"
fi
export DEVKITARM="$DEVKITPRO/devkitARM"
export LIBNDS="$DEVKITPRO/libnds"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
for cc1 in "$DEVKITARM/libexec/gcc/arm-none-eabi/13.2.1/cc1" "$sdk_dir/toolchain/root/usr/lib/gcc/arm-none-eabi/13.2.1/cc1"; do
  if [[ -f "$cc1" ]]; then chmod u+x -- "$cc1"; fi
done
for tool in arm-none-eabi-gcc arm-none-eabi-g++ arm-none-eabi-as arm-none-eabi-ld arm-none-eabi-objcopy; do
  command -v "$tool" >/dev/null
done
if [[ "${1:-}" == --check-tools ]]; then
  printf 'Project: %s\nSDK: %s\n' "$project_dir" "$DEVKITPRO"
  arm-none-eabi-gcc --version | head -n 1
  exit 0
fi
if [[ $# -ne 0 ]]; then printf 'Usage: bash desarrollo/compilar_HITO_B.sh [--check-tools]\n' >&2; exit 2; fi
build_jobs="${KOTO_BUILD_JOBS:-3}"
if [[ ! "$build_jobs" =~ ^[1-9][0-9]?$ ]]; then printf 'KOTO_BUILD_JOBS must be an integer from 1 to 99.\n' >&2; exit 2; fi
# Makefiles keep absolute dependency paths; a relocated checkpoint starts clean.
for code_dir in "$project_dir/desarrollo/fuentes/code/core/arm7" "$project_dir/desarrollo/fuentes/code/core/arm9"; do
  marker="$code_dir/.koto-build-root"
  if [[ ! -f "$marker" ]] || [[ "$(cat "$marker")" != "$code_dir" ]]; then
    if [[ "$code_dir" == */arm7 ]]; then make -C "$code_dir" TARGET=GBARunner3 clean; else make -C "$code_dir" clean; fi
    printf '%s\n' "$code_dir" > "$marker"
  fi
done
make -C "$project_dir/desarrollo/fuentes/code/libs/libtwl" -j"$build_jobs"
make -C "$project_dir/desarrollo/fuentes/code/core/arm7" TARGET=GBARunner3 -j"$build_jobs"
make -C "$project_dir/desarrollo/fuentes/code/core/arm9" -j"$build_jobs"
cp -- "$project_dir/desarrollo/fuentes/code/core/arm9/build/arm9.map" "$project_dir/desarrollo/fuentes/code/core/arm9/arm9.map"
python3 "$script_dir/empaquetar_HITO_B.py"
