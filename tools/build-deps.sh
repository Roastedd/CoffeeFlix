#!/usr/bin/env bash
# Builds the third-party libraries CoffeeFlix needs that devkitPro doesn't ship
# as packages, and installs them into deps/install (picked up by the Makefile).
#
# Run inside the devkitpro/devkitppc container (or any shell with DEVKITPRO,
# DEVKITPPC and WUT_ROOT set).
#
#   tools/build-deps.sh                 # Wii U libraries -> deps/install
#   tools/build-deps.sh --host          # same libraries for the desktop preview -> deps/host
#   tools/build-deps.sh --host libsmb2  # only the listed libraries (ffmpeg, mupdf, libsmb2)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
HOST=0
[ "${1:-}" = "--host" ] && { HOST=1; shift; }
ONLY="${*:-ffmpeg mupdf libsmb2}"

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
MUPDF_REPO="https://github.com/ArtifexSoftware/mupdf.git"
MUPDF_REV="73d3100d46d8a9ad634f6ef035bbe78f0f947886"  # 1.27.2
# libsmb2 master after 6.2 (2024), which misses a year of PDU validation fixes.
LIBSMB2_REPO="https://github.com/sahlberg/libsmb2.git"
LIBSMB2_REV="557e837d3e00636b543f17ba1b9bdf872fa1644d"

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

sha1() { if command -v sha1sum >/dev/null; then sha1sum; else shasum; fi; }  # macOS has only shasum

# A library is rebuilt when its revision or its build function changes.
stamp_for() { # stamp_for <name> <rev>
    echo "$PREFIX/.$1-$2-$(declare -f "build_$1" | sha1 | cut -c1-12)"
}

build_ffmpeg() {
    local stamp="$(stamp_for ffmpeg "$FFMPEG_REV")"
    [ -f "$stamp" ] && { echo "ffmpeg: up to date"; return; }

    fetch ffmpeg "$FFMPEG_REPO" "$FFMPEG_REV"
    local src="$DEPS/src/ffmpeg"
    git -C "$src" clean -fdxq   # out-of-tree builds need a pristine source dir
    local build="$DEPS/build/ffmpeg-$([ $HOST = 1 ] && echo host || echo wiiu)"
    rm -rf "$build" && mkdir -p "$build" && cd "$build"

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

    "$src/configure" --prefix="$PREFIX" \
        "${target[@]}" \
        --disable-shared --enable-static \
        --disable-runtime-cpudetect \
        --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-avfilter --disable-postproc \
        --disable-everything \
        --enable-version3 \
        --enable-network --enable-mbedtls --enable-zlib \
        --disable-bzlib --disable-iconv --disable-lzma \
        --disable-securetransport --disable-xlib \
        --enable-decoder="$video_decoders" \
        --enable-decoder=aac,aac_latm,ac3,eac3,mp3,mp3float,mp2,flac,vorbis,opus,alac,wavpack \
        --enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8 \
        --enable-decoder=subrip,srt,ass,ssa,mov_text,webvtt \
        --enable-demuxer=mov,matroska,avi,flv,mpegts,mpegps,hls,h264,hevc,m4v \
        --enable-demuxer=mp3,aac,ac3,eac3,flac,ogg,wav,wv,image2 \
        --enable-demuxer=srt,ass,webvtt,concat \
        --enable-parser=h264,hevc,vp8,vp9,mpeg4video,mpegvideo,aac,aac_latm,ac3,mpegaudio,flac,vorbis,opus \
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata \
        --enable-protocol=file,http,https,tcp,tls,hls,crypto,httpproxy,data

    make -j"$JOBS"
    make install
    rm -f "$PREFIX"/.ffmpeg-*
    touch "$stamp"
}

# PDF/EPUB rendering for the reader. Freetype, harfbuzz, libjpeg and zlib come
# from the system (the app links them already); jbig2dec, openjpeg and gumbo
# are MuPDF's bundled copies (mujs only for its regexp source). No JavaScript,
# colour management, hyphenation or CJK/Noto fonts (only the Base 14 set)
# keeps the library small.
build_mupdf() {
    local stamp="$(stamp_for mupdf "$MUPDF_REV")"
    [ -f "$stamp" ] && { echo "mupdf: up to date"; return; }

    fetch mupdf "$MUPDF_REPO" "$MUPDF_REV"
    local src="$DEPS/src/mupdf" lib url
    for lib in jbig2dec openjpeg gumbo-parser mujs; do
        url="$(git -C "$src" config -f .gitmodules "submodule.thirdparty/$lib.url")"
        fetch "mupdf/thirdparty/$lib" "${MUPDF_REPO%/*}/${url#../}" "$(git -C "$src" rev-parse "HEAD:thirdparty/$lib")"
    done
    local out="$DEPS/build/mupdf-$([ $HOST = 1 ] && echo host || echo wiiu)"
    rm -rf "$out" && mkdir -p "$out"

    local features="-DFZ_ENABLE_ICC=0 -DFZ_ENABLE_HYPHEN=0 -DFZ_ENABLE_OFFICE=0 -DFZ_ENABLE_FB2=0 -DFZ_ENABLE_MOBI=0 -DFZ_ENABLE_TXT=0"
    local target
    if [ $HOST = 1 ]; then
        # MuPDF only asks pkg-config for these on Linux; pass them so macOS (Homebrew) works too.
        target=(
            XCFLAGS="$features"
            SYS_FREETYPE_CFLAGS="$(pkg-config --cflags freetype2)" SYS_HARFBUZZ_CFLAGS="$(pkg-config --cflags harfbuzz)"
            SYS_LIBJPEG_CFLAGS="$(pkg-config --cflags libjpeg)"
        )
    else
        cat > "$out/wiiu-compat.h" <<'EOF'
/* newlib's <sys/types.h> defines `quad` as a macro, which breaks fz_stext_char::quad. */
#include <sys/types.h>
#undef quad
/* Missing from newlib; the app provides it (src/vendor/pdf/wiiu_time_utils.c). */
#include <time.h>
time_t timegm(struct tm *tm);
EOF
        local ppc="$DEVKITPRO/portlibs/ppc"
        target=(
            OS=wiiu CC=powerpc-eabi-gcc CXX=powerpc-eabi-g++ AR=powerpc-eabi-ar RANLIB=powerpc-eabi-ranlib
            XCFLAGS="$features -D__WIIU__ -mcpu=750 -meabi -mhard-float -include $out/wiiu-compat.h -I$ppc/include"
            SYS_FREETYPE_CFLAGS="-I$ppc/include/freetype2" SYS_HARFBUZZ_CFLAGS="-I$ppc/include/harfbuzz"
        )
    fi

    make -C "$src" -j"$JOBS" "${target[@]}" \
        OUT="$out" prefix="$PREFIX" build=release shared=no \
        HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_OBJCOPY=no \
        mujs=no brotli=no extract=no xps=no tofu=yes tofu_cjk=yes \
        USE_SYSTEM_FREETYPE=yes USE_SYSTEM_HARFBUZZ=yes USE_SYSTEM_LIBJPEG=yes USE_SYSTEM_ZLIB=yes \
        LCMS2_SRC= \
        install-libs
    rm -f "$PREFIX"/.mupdf-*
    touch "$stamp"
}

# SMB2/3 client for network shares. The Wii U CMake wrapper selects the
# library's CafeOS port.
build_libsmb2() {
    local stamp="$(stamp_for libsmb2 "$LIBSMB2_REV")"
    [ -f "$stamp" ] && { echo "libsmb2: up to date"; return; }

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
    fetch libsmb2 "$LIBSMB2_REPO" "$LIBSMB2_REV"
    local build="$DEPS/build/libsmb2-$([ $HOST = 1 ] && echo host || echo wiiu)"
    rm -rf "$build"
    "$cmake" -S "$DEPS/src/libsmb2" -B "$build" -DCMAKE_INSTALL_PREFIX="$PREFIX" "${opts[@]}"
    cmake --build "$build" -j"$JOBS"
    cmake --install "$build"
    rm -f "$PREFIX"/.libsmb2-*
    touch "$stamp"
}

for lib in $ONLY; do
    declare -F "build_$lib" >/dev/null || { echo "Unknown library: $lib (ffmpeg, mupdf, libsmb2)" >&2; exit 1; }
done
for lib in $ONLY; do "build_$lib"; done
echo "Dependencies installed to $PREFIX"
