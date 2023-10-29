## Installing FFmpeg

1. Type `./configure` to create the configuration. A list of configure
options is printed by running `configure --help`.

    `configure` can be launched from a directory different from the FFmpeg
sources to build the objects out of tree. To do this, use an absolute
path when launching `configure`, e.g. `/ffmpegdir/ffmpeg/configure`.

2. Then type `make` to build FFmpeg. GNU Make 3.81 or later is required.

3. Type `make install` to install all binaries and libraries you built.

NOTICE
------

 - Non system dependencies (e.g. libx264, libvpx) are disabled by default.


## Example webrtc build

With LDFlags for build on MacOS

```sh
./configure --prefix=output \
            --enable-shared \
            --enable-pthreads \
            --enable-version3 \
            --host-ldflags='-Wl,-ld_classic' \
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
            --enable-libx264 \
            --enable-gpl \
            --enable-libopus \
            --enable-neon \
            --enable-videotoolbox
            --enable-audiotoolbox

make install -j20

```
