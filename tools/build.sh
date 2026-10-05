#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -ne 2 ]; then
    echo 'Usage: build.sh <devkitPro directory> <native project directory>' >&2
    exit 2
fi
export DEVKITPRO="$(cygpath -u "$1")"
export DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:/usr/bin:$PATH"
cd "$(cygpath -u "$2")"
make -j2
