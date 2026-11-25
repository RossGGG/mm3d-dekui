#!/bin/sh
# Copies the game dump and a desktop-prepared user folder into Dekui's folder
# on an Apple TV (Library/Caches/Dekui in the app's container). tvOS has no
# Files app, so this is how the game gets there. Install the app first: the
# script opens it once so that the app itself creates its folders (folders
# devicectl creates belong to root, and the app could not write its saves and
# settings into them).
#
#   tools/tvos/copy-game.sh <device> <folder with the .cci and user/> [tvOS user] [bundle id]
#
# <device> is a devicectl identifier or name (xcrun devicectl list devices).
# Each Apple TV profile has its own Dekui folder (saves, settings, the game).
# [tvOS user] is the profile's user name, "mobile" by default; Dekui shows it
# in its folder path (setup screen, Settings > About). Switch the Apple TV to
# that profile first: the script opens Dekui in the current profile.
# tvOS may purge Caches when the Apple TV runs short of storage; run this again
# if Dekui asks for the game. Save data lives in user/ too, so copy it back
# first with: xcrun devicectl device copy from --device <device> \
#   --domain-type appDataContainer --domain-identifier <bundle id> \
#   --user <tvOS user> --source Library/Caches/Dekui/user --destination <folder>
set -eu

if [ $# -lt 2 ]; then
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
DEVICE=$1
SOURCE=$2
TV_USER=${3:-mobile}
BUNDLE_ID=${4:-org.azahar-emu.azahar.aot}
DESTINATION=Library/Caches/Dekui

ROMS=$(find "${SOURCE}" -maxdepth 1 -type f \( -iname '*.cci' -o -iname '*.3ds' \))
COUNT=$(printf '%s\n' "${ROMS}" | grep -c . || true)
if [ "${COUNT}" -ne 1 ]; then
    echo "Expected exactly one .cci or .3ds file in ${SOURCE}, found ${COUNT}" >&2
    exit 1
fi
if [ ! -f "${SOURCE}/user/setup/ready.json" ]; then
    echo "${SOURCE}/user is not a prepared Dekui user folder (no setup/ready.json)" >&2
    exit 1
fi

copy() {
    xcrun devicectl device copy to --device "${DEVICE}" \
        --domain-type appDataContainer --domain-identifier "${BUNDLE_ID}" \
        --user "${TV_USER}" --source "$1" --destination "$2"
}

echo "Opening Dekui once so it creates its folders"
xcrun devicectl device process launch --device "${DEVICE}" --terminate-existing \
    --json-output "${TMPDIR:-/tmp}/dekui-launch.json" "${BUNDLE_ID}" > /dev/null
sleep 5
PID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["result"]["process"]["processIdentifier"])' \
    "${TMPDIR:-/tmp}/dekui-launch.json")
xcrun devicectl device process terminate --device "${DEVICE}" --pid "${PID}" > /dev/null

echo "Copying the game (about 1 GB over the network; this takes a few minutes)"
copy "${ROMS}" "${DESTINATION}/$(basename "${ROMS}")"
echo "Copying the user folder"
copy "${SOURCE}/user" "${DESTINATION}/user"
if [ -d "${SOURCE}/licenses" ]; then
    copy "${SOURCE}/licenses" "${DESTINATION}/licenses"
fi
echo "Done. Open Dekui on the Apple TV."
