#!/bin/sh
# Builds SFTV RX with the next build number. playdate/build_number holds the number of the last
# build (not BUILD: on a case-insensitive disk that is make's build/ folder); it goes up by one on
# every build, installed or not. The version is `version` in playdate/Source/pdxinfo. Both go
# into playdate/src/build_info.h (shown in About); the number also into pdxinfo's buildNumber.
# Usage: tools/playdate_build.sh          build only
#        tools/playdate_build.sh install  build, copy to the Playdate over USB and start it
set -e
cd "$(dirname "$0")/../playdate"
n=$(( $(cat build_number) + 1 ))
echo "$n" > build_number
ver=$(sed -n 's/^version=//p' Source/pdxinfo)
sed -i '' "s/^buildNumber=.*/buildNumber=$n/" Source/pdxinfo
printf '#pragma once\n/* written by tools/playdate_build.sh */\n#define SFTV_VERSION "%s"\n#define SFTV_BUILD %d\n' "$ver" "$n" > src/build_info.h
echo "SFTV RX $ver, build $n"
make
[ "$1" = install ] || exit 0

pdutil="${PLAYDATE_SDK_PATH:-$HOME/Developer/PlaydateSDK}/bin/pdutil"
port() { ls /dev/cu.usbmodemPDU1_* 2>/dev/null | head -n 1; }
p=$(port)
[ -n "$p" ] || { echo "no Playdate on USB"; exit 1; }
"$pdutil" "$p" datadisk
i=0; until [ -d /Volumes/PLAYDATE/Games ]; do i=$((i + 1)); [ $i -lt 30 ] || { echo "disk did not mount"; exit 1; }; sleep 1; done
rsync -a --delete SFTV_RX.pdx/ /Volumes/PLAYDATE/Games/SFTV_RX.pdx/
diskutil eject /Volumes/PLAYDATE >/dev/null
i=0; until [ -n "$(port)" ]; do i=$((i + 1)); [ $i -lt 30 ] || { echo "Playdate did not come back"; exit 1; }; sleep 1; done
sleep 1
"$pdutil" "$(port)" run /Games/SFTV_RX.pdx
echo "installed and started"
