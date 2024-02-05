#!/bin/bash

if [[ $1 == "dep" ]];
then
./build-macos-deps.sh || exit 1
fi
set -x

cpu_count="$(grep -c processor /proc/cpuinfo 2>/dev/null)" # linux cpu count
if [ -z "$cpu_count" ]; then
  cpu_count=`sysctl -n hw.ncpu | tr -d '\n'` # OS X cpu count
  if [ -z "$cpu_count" ]; then
    echo "warning, unable to determine cpu count, defaulting to 1"
    cpu_count=1 # else default to just 1, instead of blank, which means infinite
  fi
fi

echo "cpu count $cpu_count"
#make distclean

echo "configuring..."


BASE_DIR="$( cd "$( dirname "$0" )" > /dev/null 2>&1 && pwd )"
echo "base directory is ${BASE_DIR}"
SCRIPT_DIR="${BASE_DIR}/ffmpeg-apple-arm64-build/build"
echo "script directory is ${SCRIPT_DIR}"
TEST_DIR="${BASE_DIR}/macos-test"
echo "test directory is ${TEST_DIR}"
WORKING_DIR="$( pwd )"
echo "working directory is ${WORKING_DIR}"
TOOL_DIR="$WORKING_DIR/macos-tool"
echo "tool directory is ${TOOL_DIR}"
OUT_DIR="$WORKING_DIR/macos-build-out"
echo "output directory is ${OUT_DIR}"

export PKG_CONFIG_PATH="${TOOL_DIR}/bin/pkg-config" # let ffmpeg find our dependencies [currently not working :| ]

#ls -la $TOOL_DIR/lib/pkgconfig

if [[ ! -f $TOOL_DIR/lib/libssl.a ]]; then
  cd openssl-3.0.0 || exit 1
  ./config no-tests no-threads no-shared --libdir=$TOOL_DIR/lib --prefix=$TOOL_DIR || exit 1
  # ./configure shared mingw64 --prefix=$prefix || exit 1
  make clean || exit 1
  make -j$cpu_count || exit 1
  make install || exit 1
  cd ..
fi


libdir=macos-tool
export LIBS="-L$libdir/lib -I$libdir/include"
export LDFLAGS="$LIBS -Wl,-ld_classic"
export CFLAGS="$LIBS"
#export DESTDIR=macos-out/
./configure --prefix="macos-build-out" \
            --enable-gpl \
            --pkg-config-flags="--static" \
            --pkg-config=$PKG_CONFIG_PATH \
            --enable-shared \
            --enable-rpath \
            --install-name-dir='@rpath' \
            --disable-doc \
            --enable-pthreads \
            --enable-version3 \
            --enable-nonfree \
            --enable-libxml2 \
            --enable-muxer=whip \
            --enable-demuxer=dash \
            --enable-openssl \
            --enable-libopus \
            --enable-libx264 \
            --enable-libfdk-aac \
            --enable-libmp3lame \
            --enable-libvpx \
            --enable-libx265 \
            --enable-neon \
            --disable-sdl2 \
            --disable-ffplay \
            --enable-videotoolbox \
            --enable-audiotoolbox \
            || exit 1


echo "making"
make -j $cpu_count || exit 1
echo "installing"

make install -j $cpu_count  || exit 1
echo "zipping"

zip -r ffmpeg@6-webrtc-macos.zip $OUT_DIR

#            --enable-pthreads \
#                        --enable-version3 \
#                        --enable-muxer=whip \
#                        --enable-openssl \
#                        --enable-libaom \
#                        --enable-libjxl \
#                        --enable-libopus \
#                        --enable-librav1e \
#                        --enable-librist \
#                        --enable-librubberband \
#                        --enable-libsrt \
#                        --enable-libsvtav1 \
#                        --enable-libvidstab \
#                        --enable-libvmaf \
#                        --enable-libvorbis \
#                        --enable-libvpx \
#                        --enable-libwebp \
#                        --enable-libx264 \
#                        --enable-libx265 \
#                        --enable-libxml2 \
#                        --enable-libfontconfig \
#                        --enable-libfreetype \
#                        --enable-libass \
#                        --enable-libopenjpeg \
#                        --enable-libspeex \
#                        --enable-libzmq \
#                        --enable-libzimg \
#                        --disable-libjack \
#                        --disable-indev=jack \
#                        --enable-neon \
#                        --enable-videotoolbox \
#                        --enable-audiotoolbox
