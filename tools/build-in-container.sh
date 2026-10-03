#!/bin/sh
set -eu
export PATH="/opt/msvc/bin/x86:$PATH"
export WINEDEBUG=-all
wine_bin=$(command -v wine64 || command -v wine)
"$wine_bin" wineboot -i >/dev/null 2>&1 </dev/null
"$wine_bin" explorer /desktop=wcbuild,1x1 >/dev/null 2>&1 </dev/null &
desktop_pid=$!
cleanup() {
    kill "$desktop_pid" >/dev/null 2>&1 || true
    chown -R "$HOST_UID:$HOST_GID" /work/build /work/bin
}
trap cleanup EXIT
sleep 2
cmake -S /work -B /work/build -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86 \
    -DCMAKE_CXX_COMPILER=/opt/msvc/bin/x86/cl \
    -DCMAKE_RC_COMPILER=/opt/msvc/bin/x86/rc \
    -DCMAKE_MT=/opt/msvc/bin/x86/mt \
    -DCMAKE_BUILD_TYPE="$CONFIG" -DGWTOOLBOX_ROOT=/src \
    -DCMAKE_JOB_POOL_COMPILE=console -DCMAKE_JOB_POOL_LINK=console
cmake --build /work/build --target WorldCompletion -j 1
