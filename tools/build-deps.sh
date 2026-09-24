#!/usr/bin/env bash
# Builds the third-party libraries CoffeeFlix needs that devkitPro doesn't ship
# as packages, and installs them into deps/install (picked up by the Makefile).
#
# Run inside the devkitpro/devkitppc container (or any shell with DEVKITPRO,
# DEVKITPPC and WUT_ROOT set).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
PREFIX="$DEPS/install"
JOBS="${JOBS:-$(nproc)}"

: "${DEVKITPRO:?DEVKITPRO must be set}"
: "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
: "${WUT_ROOT:=$DEVKITPRO/wut}"
export PATH="$DEVKITPPC/bin:$PATH"

FFMPEG_REPO="https://github.com/GaryOderNichts/FFmpeg-wiiu.git"
FFMPEG_REV="24997bdb3e5a3bc666f05e1497b0c102390f9ae0"

mkdir -p "$DEPS/src" "$PREFIX"

fetch() { # fetch <dir> <repo> <rev>
    local dir="$DEPS/src/$1"
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$2"
    fi
    if [ "$(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null)" != "$3" ]; then
        git -C "$dir" fetch -q --depth 1 origin "$3"
        git -C "$dir" checkout -q --force FETCH_HEAD
    fi
}

build_ffmpeg() {
    local stamp="$PREFIX/.ffmpeg-$FFMPEG_REV-$(sha1sum "$0" | cut -c1-12)"
    [ -f "$stamp" ] && { echo "ffmpeg: up to date"; return; }

    fetch ffmpeg "$FFMPEG_REPO" "$FFMPEG_REV"
    cd "$DEPS/src/ffmpeg"

    local arch="-mcpu=750 -meabi -mhard-float"
    local opt="-O3 -fomit-frame-pointer -fno-math-errno -fno-trapping-math"

    ./configure --prefix="$PREFIX" \
        --enable-cross-compile \
        --cross-prefix="$DEVKITPPC/bin/powerpc-eabi-" \
        --arch=ppc --cpu=750 --target-os=none \
        --disable-shared --enable-static \
        --disable-runtime-cpudetect \
        --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-avfilter --disable-postproc \
        --disable-everything \
        --enable-version3 \
        --enable-network --enable-mbedtls --enable-zlib \
        --disable-bzlib --disable-iconv --disable-lzma \
        --disable-securetransport --disable-xlib \
        --enable-decoder=h264_wiiu,h264,hevc,vp8,vp9,mpeg4,mpeg1video,mpeg2video,mjpeg,png \
        --enable-decoder=aac,aac_latm,ac3,eac3,mp3,mp3float,mp2,flac,vorbis,opus,alac,wavpack \
        --enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8 \
        --enable-decoder=subrip,srt,ass,ssa,mov_text,webvtt \
        --enable-demuxer=mov,matroska,avi,flv,mpegts,mpegps,hls,h264,hevc,m4v \
        --enable-demuxer=mp3,aac,ac3,eac3,flac,ogg,wav,wv,image2 \
        --enable-demuxer=srt,ass,webvtt,concat \
        --enable-parser=h264,hevc,vp8,vp9,mpeg4video,mpegvideo,aac,aac_latm,ac3,mpegaudio,flac,vorbis,opus \
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata \
        --enable-protocol=file,http,https,tcp,tls,hls,crypto,httpproxy,data \
        --extra-cflags="-D__WIIU__ $opt $arch -I$WUT_ROOT/include -I$DEVKITPRO/portlibs/wiiu/include -I$DEVKITPRO/portlibs/ppc/include" \
        --extra-cxxflags="-D__WIIU__ $opt $arch" \
        --extra-ldflags="-specs=$WUT_ROOT/share/wut.specs -L$WUT_ROOT/lib -L$DEVKITPRO/portlibs/wiiu/lib -L$DEVKITPRO/portlibs/ppc/lib" \
        --extra-libs="-lmbedtls -lmbedx509 -lmbedcrypto -lz -lwut"

    make -j"$JOBS"
    make install
    rm -f "$PREFIX"/.ffmpeg-*
    touch "$stamp"
}

build_ffmpeg
echo "Dependencies installed to $PREFIX"
