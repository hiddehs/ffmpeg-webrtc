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

libdir=macos-tool
export LIBS="-L$libdir/lib -I$libdir/include"
export LDFLAGS="$LIBS -Wl,-ld_classic"
export CFLAGS="$LIBS"
#export DESTDIR=macos-out/
./configure --prefix="macos-build-out" --enable-gpl \
            --pkg-config-flags="--static" \
            --pkg-config=macos-tool/bin/pkg-config \
            --enable-shared \
            --enable-rpath \
            --install-name-dir='@rpath' \
            --disable-doc \
            --enable-pthreads \
            --enable-version3 \
            --enable-nonfree \
            --enable-libx264 \
            --enable-libwebp \
            --enable-libfdk-aac \
            --enable-libaom \
            --enable-libmp3lame \
            --enable-libvpx \
            || exit 1





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

echo "making"
make -j $cpu_count || exit 1
echo "installing"

make install -j $cpu_count  || exit 1
echo "zipping"

zip -r ffmpeg@6-webrtc-macos.zip macos-out
