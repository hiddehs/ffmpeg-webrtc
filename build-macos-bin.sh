#!/bin/bash
echo "configuring..." 
export LDFLAGS=-Wl,-ld_classic
export DESTDIR=macos-out/
./configure --enable-shared --enable-rpath \
            --install-name-dir='@rpath' \
            --enable-pthreads \
            --enable-version3 \
            --enable-gpl \
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
            --enable-muxer=whip \
            --enable-openssl \
            --enable-version3 \
            --enable-gpl \
            --enable-neon \
            --enable-videotoolbox \
            --enable-audiotoolbox
echo "making"
make -j 10
echo "installing"

make install -j 10
echo "zipping"

zip -r ffmpeg@6-webrtc-macos.zip macos-out