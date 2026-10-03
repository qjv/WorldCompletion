#!/bin/sh
set -eu
root=$1
toolbox=$2
toolchain=$3
config=$4
export WINEPREFIX="$root/build/wine-prefix"
export WINEDEBUG=-all
export PATH="$toolchain/bin/x86:$PATH"
wine_bin=$(command -v wine64 || command -v wine)
"$wine_bin" wineboot -i >/dev/null 2>&1 </dev/null
"$wine_bin" explorer /desktop=wcbuild,1x1 >/dev/null 2>&1 </dev/null &
desktop_pid=$!
trap 'kill "$desktop_pid" >/dev/null 2>&1 || true' EXIT
sleep 2
cmake -S "$root" -B "$root/build/local" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86 \
    -DCMAKE_CXX_COMPILER="$toolchain/bin/x86/cl" \
    -DCMAKE_RC_COMPILER="$toolchain/bin/x86/rc" \
    -DCMAKE_MT="$toolchain/bin/x86/mt" \
    -DCMAKE_BUILD_TYPE="$config" -DGWTOOLBOX_ROOT="$toolbox" -DGWTOOLBOX_BUILD="$toolbox/build-wine" \
    -DCMAKE_JOB_POOL_COMPILE=console -DCMAKE_JOB_POOL_LINK=console
cmake --build "$root/build/local" --target WorldCompletion -j 1
