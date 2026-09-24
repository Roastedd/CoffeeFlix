#!/usr/bin/env bash
# Builds the third-party libraries CoffeeFlix needs that devkitPro doesn't ship
# as packages, and installs them into deps/install (picked up by the Makefile).
#
# Run inside the devkitpro/devkitppc container (or any shell with DEVKITPRO,
# DEVKITPPC and WUT_ROOT set).
#
#   tools/build-deps.sh          # Wii U libraries -> deps/install
#   tools/build-deps.sh --host   # same libraries for the desktop preview -> deps/host
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
JOBS="${JOBS:-$(nproc)}"
HOST=0
[ "${1:-}" = "--host" ] && HOST=1

if [ $HOST = 1 ]; then
    PREFIX="$DEPS/host"
else
    PREFIX="$DEPS/install"
    : "${DEVKITPRO:?DEVKITPRO must be set}"
    : "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
    : "${WUT_ROOT:=$DEVKITPRO/wut}"
    export PATH="$DEVKITPPC/bin:$PATH"
fi

FFMPEG_REPO="https://github.com/GaryOderNichts/FFmpeg-wiiu.git"
FFMPEG_REV="24997bdb3e5a3bc666f05e1497b0c102390f9ae0"
# libsmb2 master after 6.2 (2024), which misses a year of PDU validation fixes.
LIBSMB2_REPO="https://github.com/sahlberg/libsmb2.git"
LIBSMB2_REV="557e837d3e00636b543f17ba1b9bdf872fa1644d"

mkdir -p "$DEPS/src" "$PREFIX"

# Each library has its own stamp named after a hash of its revision and build
# options, so changing one library's recipe rebuilds only that library.
stamp_for() { # stamp_for <name> <revision and options...>
    echo "$PREFIX/.$1-$(printf '%s\n' "${@:2}" | sha1sum | cut -c1-12)"
}

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
    local target video_decoders
    if [ $HOST = 1 ]; then
        target=(--disable-asm --extra-cflags="-O2")
        video_decoders=h264,hevc,vp8,vp9,mpeg4,mpeg1video,mpeg2video,mjpeg,png
    else
        local arch="-mcpu=750 -meabi -mhard-float"
        local opt="-O3 -fomit-frame-pointer -fno-math-errno -fno-trapping-math"
        target=(
            --enable-cross-compile
            --cross-prefix="$DEVKITPPC/bin/powerpc-eabi-"
            --arch=ppc --cpu=750 --target-os=none
            --extra-cflags="-D__WIIU__ $opt $arch -I$WUT_ROOT/include -I$DEVKITPRO/portlibs/wiiu/include -I$DEVKITPRO/portlibs/ppc/include"
            --extra-cxxflags="-D__WIIU__ $opt $arch"
            --extra-ldflags="-specs=$WUT_ROOT/share/wut.specs -L$WUT_ROOT/lib -L$DEVKITPRO/portlibs/wiiu/lib -L$DEVKITPRO/portlibs/ppc/lib"
            --extra-libs="-lmbedtls -lmbedx509 -lmbedcrypto -lz -lwut"
        )
        video_decoders=h264_wiiu,h264,hevc,vp8,vp9,mpeg4,mpeg1video,mpeg2video,mjpeg,png
    fi

    local opts=(
        "${target[@]}"
        --disable-shared --enable-static
        --disable-runtime-cpudetect
        --disable-programs --disable-doc --disable-debug
        --disable-avdevice --disable-avfilter --disable-postproc
        --disable-everything
        --enable-version3
        --enable-network --enable-mbedtls --enable-zlib
        --disable-bzlib --disable-iconv --disable-lzma
        --disable-securetransport --disable-xlib
        --enable-decoder="$video_decoders"
        --enable-decoder=aac,aac_latm,ac3,eac3,mp3,mp3float,mp2,flac,vorbis,opus,alac,wavpack
        --enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8
        --enable-decoder=subrip,srt,ass,ssa,mov_text,webvtt
        --enable-demuxer=mov,matroska,avi,flv,mpegts,mpegps,hls,h264,hevc,m4v
        --enable-demuxer=mp3,aac,ac3,eac3,flac,ogg,wav,wv,image2
        --enable-demuxer=srt,ass,webvtt,concat
        --enable-parser=h264,hevc,vp8,vp9,mpeg4video,mpegvideo,aac,aac_latm,ac3,mpegaudio,flac,vorbis,opus
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata
        --enable-protocol=file,http,https,tcp,tls,hls,crypto,httpproxy,data
    )
    local stamp
    stamp="$(stamp_for ffmpeg "$FFMPEG_REV" "${opts[@]}")"
    [ -f "$stamp" ] && { echo "ffmpeg: up to date"; return; }
    echo "ffmpeg: building ($(basename "$stamp"))"

    fetch ffmpeg "$FFMPEG_REPO" "$FFMPEG_REV"
    local src="$DEPS/src/ffmpeg"
    git -C "$src" clean -fdxq   # out-of-tree builds need a pristine source dir
    local build="$DEPS/build/ffmpeg-$([ $HOST = 1 ] && echo host || echo wiiu)"
    rm -rf "$build" && mkdir -p "$build" && cd "$build"
    "$src/configure" --prefix="$PREFIX" "${opts[@]}"
    make -j"$JOBS"
    make install
    rm -f "$PREFIX"/.ffmpeg-*
    touch "$stamp"
}

# SMB2/3 client for network shares. The Wii U CMake wrapper selects the
# library's CafeOS port.
build_libsmb2() {
    local cmake opts=(
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_EXAMPLES=OFF
        -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_LIBDCERPC=OFF
    )
    if [ $HOST = 1 ]; then
        cmake=cmake
        opts+=(-DCMAKE_POSITION_INDEPENDENT_CODE=ON)  # linked into a PIE
    else
        cmake="$DEVKITPRO/portlibs/wiiu/bin/powerpc-eabi-cmake"  # no PIC: elf2rpl rejects it
    fi
    local stamp
    stamp="$(stamp_for libsmb2 "$LIBSMB2_REV" "${opts[@]}")"
    [ -f "$stamp" ] && { echo "libsmb2: up to date"; return; }
    echo "libsmb2: building ($(basename "$stamp"))"

    fetch libsmb2 "$LIBSMB2_REPO" "$LIBSMB2_REV"
    local build="$DEPS/build/libsmb2-$([ $HOST = 1 ] && echo host || echo wiiu)"
    rm -rf "$build"
    "$cmake" -S "$DEPS/src/libsmb2" -B "$build" -DCMAKE_INSTALL_PREFIX="$PREFIX" "${opts[@]}"
    cmake --build "$build" -j"$JOBS"
    cmake --install "$build"
    rm -f "$PREFIX"/.libsmb2-*
    touch "$stamp"
}

build_ffmpeg
build_libsmb2
echo "Dependencies installed to $PREFIX"
