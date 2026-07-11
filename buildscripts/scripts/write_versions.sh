#!/bin/bash
# Extract versions from compiled output and replace placeholders in Utils.kt

cd "$( dirname "${BASH_SOURCE[0]}" )/.."

ARCH=$1
if [ -z "$ARCH" ]; then
    ARCH="arm64"
fi

MPV_VERSION=$(cat deps/mpv/_build${ARCH}/common/version.h | grep "#define VERSION" | cut -d '"' -f 2)
LIBPLACEBO_VERSION=$(cat deps/libplacebo/_build${ARCH}/src/version.h | grep "#define BUILD_VERSION" | cut -d '"' -f 2)
FFMPEG_VERSION=$(echo $(cd deps/ffmpeg/ && git rev-parse --short HEAD))

# get build date from compiled object file
START_RODATA=0x$(readelf deps/mpv/_build${ARCH}/libmpv.so.p/common_version.c.o -S | grep .rodata | cut -d ' ' -f 27)
START=0x$(readelf deps/mpv/_build${ARCH}/libmpv.so.p/common_version.c.o -s | grep mpv_builddate | cut -d ' ' -f 7)
SIZE=$(readelf deps/mpv/_build${ARCH}/libmpv.so.p/common_version.c.o -s | grep mpv_builddate | cut -d ' ' -f 11)
SKIP=$(($START_RODATA + $START - 1))
dd if=deps/mpv/_build${ARCH}/libmpv.so.p/common_version.c.o of=date.txt bs=1 skip=$SKIP count=$SIZE
DATE=$(cat date.txt)
rm date.txt

# write versions to Utils.kt
sed -i "s/%MPV_VERSION%/$MPV_VERSION/g" ../app/src/main/java/is/xyz/mpv/Utils.kt
sed -i "s/%LIBPLACEBO_VERSION%/$LIBPLACEBO_VERSION/g" ../app/src/main/java/is/xyz/mpv/Utils.kt
sed -i "s/%FFMPEG_VERSION%/$FFMPEG_VERSION/g" ../app/src/main/java/is/xyz/mpv/Utils.kt
sed -i "s/%DATE%/$DATE/g" ../app/src/main/java/is/xyz/mpv/Utils.kt
