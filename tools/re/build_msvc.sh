#!/usr/bin/env bash
# Git Bash / MSYS entry point; the PowerShell script discovers MSVC and the SDK.
# Usage: bash tools/re/build_msvc.sh [test|loader|ctl|dll|gui|all] [-RunTests]
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
TARGET="${1:-all}"
if (( $# > 0 )); then shift; fi

if command -v powershell.exe >/dev/null 2>&1; then
    POWERSHELL="$(command -v powershell.exe)"
elif command -v pwsh.exe >/dev/null 2>&1; then
    POWERSHELL="$(command -v pwsh.exe)"
else
    echo 'PowerShell was not found on PATH.' >&2
    exit 2
fi
BUILD_SCRIPT="$ROOT/tools/build.ps1"
if command -v cygpath >/dev/null 2>&1; then
    BUILD_SCRIPT="$(cygpath -w "$BUILD_SCRIPT")"
    # MSYS paths supplied as overrides are converted once; no shell eval is used.
    for name in FLAB_BUILD_DIR FLAB_PYTHON FLAB_IMAGE FLAB_VSWHERE FLAB_VSDEVCMD; do
        value="${!name:-}"
        if [[ "$value" == /* ]]; then export "$name=$(cygpath -w "$value")"; fi
    done
fi
MSYS_NO_PATHCONV=1 exec "$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$BUILD_SCRIPT" -Target "$TARGET" "$@"
