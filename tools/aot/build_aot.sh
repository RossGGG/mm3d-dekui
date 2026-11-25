#!/bin/sh
# Generates ahead-of-time translated MM3D code from the user's own dump.
#
#   DEKUI_AOT=/path/to/dekui_aot tools/aot/build_aot.sh <game.cci> <mods-dir> <out-dir> [dekui_aot options]
#
# <mods-dir> is the prepared user/load/mods/0004000000125500 folder (code.bps,
# exheader.bin). Build dekui_aot from a desktop configuration with
# ENABLE_MM3D_PRESENTATION=ON (target "dekui_aot"). Extra options such as
# "--coverage blocks.txt" are passed to "dekui_aot emit".
#
# The output is derived from the dump and must not be distributed. Configure
# an AArch64 build (visionOS, iOS, Apple silicon macOS) with
#   -DDEKUI_AOT_GENERATED_DIR=<out-dir>/generated
set -eu

if [ $# -lt 3 ]; then
    sed -n '2,13p' "$0"
    exit 2
fi
cci=$1 mods=$2 out=$3
shift 3
: "${DEKUI_AOT:?set DEKUI_AOT to the dekui_aot executable}"
here=$(cd "$(dirname "$0")" && pwd)

python3 -I "$here/extract_code.py" "$cci" "$out/work" --mods "$mods" > /dev/null
rm -rf "$out/generated"
"$DEKUI_AOT" emit "$out/work" "$out/generated" "$@"
echo "Configure with -DDEKUI_AOT_GENERATED_DIR=$out/generated"
