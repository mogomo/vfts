#!/usr/bin/env bash
# Build the static, position-independent libraries vfts links into libvfts.so.
# Run third_party/fetch.sh first. Output: third_party/install/{include,lib}.
#
#   third_party/build.sh [jobs]      (default: all cores)
#
# Static linking means nothing has to be installed on the Vertica nodes. The library is linked with
# -Wl,--exclude-libs,ALL (see Makefile), so these copies of ICU and Snowball stay private to
# libvfts.so and never clash with the ICU inside the Vertica server process.
set -euo pipefail
cd "$(dirname "$0")"
JOBS=${1:-$(nproc)}
TP=$(pwd)
PREFIX=$TP/install
case "$PREFIX" in
  *[[:space:]]*) echo "build.sh: the path $PREFIX has a space; ICU's configure cannot handle that: move the repository" >&2; exit 1 ;;
esac
mkdir -p "$PREFIX/include" "$PREFIX/lib" build

# ICU 79.1: common (break iterators, normalization, case folding) and i18n (transliteration is not
# used). Data is linked in statically (libicudata.a).
# (the tools are needed: they package the data; without them libicudata.a is a 1 KB stub)
if [ ! -f "$PREFIX/lib/libicuuc.a" ] || [ "$(stat -c %s "$PREFIX/lib/libicudata.a" 2>/dev/null || echo 0)" -lt 1000000 ]; then
  rm -rf build/icu && mkdir -p build/icu && tar xzf src/icu4c-79.1-sources.tgz -C build/icu
  ( cd build/icu/icu/source
    CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC -std=c++17 -D_GLIBCXX_USE_CXX11_ABI=1" \
      ./runConfigureICU Linux --prefix="$PREFIX" --enable-static --disable-shared \
        --disable-tests --disable-samples --disable-extras --disable-icuio --disable-layoutex \
        --with-data-packaging=static > configure.log 2>&1 \
      || { tail -40 configure.log; exit 1; }
    make -j"$JOBS" > make.log 2>&1 || { tail -60 make.log; exit 1; }
    make install > install.log 2>&1 || { tail -40 install.log; exit 1; } )
fi

# libstemmer_c 3.2.0 (Snowball): only the UTF-8 stemmers are used.
if [ ! -f "$PREFIX/lib/libstemmer.a" ]; then
  rm -rf build/libstemmer_c-3.2.0 && tar xzf src/libstemmer_c-3.2.0.tar.gz -C build
  ( cd build/libstemmer_c-3.2.0 && make -j"$JOBS" libstemmer.a CFLAGS="-O2 -fPIC" > make.log 2>&1 \
      || { tail -40 make.log; exit 1; } )
  cp build/libstemmer_c-3.2.0/libstemmer.a "$PREFIX/lib/"
  cp build/libstemmer_c-3.2.0/include/libstemmer.h "$PREFIX/include/"
fi

# CRoaring 5.5.5, amalgamated: one C file.
if [ ! -f "$PREFIX/lib/libroaring.a" ]; then
  cp src/roaring.h "$PREFIX/include/"
  gcc -O3 -fPIC -std=c11 -c src/roaring.c -I src -o build/roaring.o
  ar -cr "$PREFIX/lib/libroaring.a" build/roaring.o
fi

ls -la "$PREFIX/lib"
