#!/bin/bash
cpu_count="$(grep -c processor /proc/cpuinfo 2>/dev/null)" # linux cpu count
if [ -z "$cpu_count" ]; then
  cpu_count=`sysctl -n hw.ncpu | tr -d '\n'` # OS X cpu count
  if [ -z "$cpu_count" ]; then
    echo "warning, unable to determine cpu count, defaulting to 1"
    cpu_count=1 # else default to just 1, instead of blank, which means infinite
  fi
fi

echo "cpu count $cpu_count"


echo "configuring..."
export LDFLAGS=-Wl,-ld_classic
export DESTDIR=macos-out/
./configure --enable-shared --enable-rpath \
            --install-name-dir='@rpath' \
            --enable-pthreads \
            --enable-gpl \
            --enable-version3 \
            --enable-muxer=whip \
            --enable-openssl \
            --enable-libaom \
            --enable-libjxl \
            --enable-libopus \
            --enable-librav1e \
            --enable-librist \
            --enable-librubberband \
            --enable-libsrt \
            --enable-libsvtav1 \
            --enable-libvidstab \
            --enable-libvmaf \
            --enable-libvorbis \
            --enable-libvpx \
            --enable-libwebp \
            --enable-libx264 \
            --enable-libx265 \
            --enable-libxml2 \
            --enable-libfontconfig \
            --enable-libfreetype \
            --enable-libass \
            --enable-libopenjpeg \
            --enable-libspeex \
            --enable-libzmq \
            --enable-libzimg \
            --disable-libjack \
            --disable-indev=jack \
            --enable-neon \
            --enable-videotoolbox \
            --enable-audiotoolbox
echo "making"
make -j $cpu_count || exit 1
echo "installing"

make install -j $cpu_count  || exit 1
echo "zipping"

zip -r ffmpeg@6-webrtc-macos.zip macos-out
