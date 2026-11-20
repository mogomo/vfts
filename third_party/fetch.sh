#!/usr/bin/env bash
# Download the pinned sources of the three libraries vtext links statically:
#   ICU 79.1 (tokenizer, normalization), libstemmer_c 3.2.0 (Snowball), CRoaring 5.5.5 (bitmaps).
# Every file is checked against its SHA-256 before it is used.
#
#   third_party/fetch.sh            download into third_party/src (skips files that are already there)
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p src

fetch() {   # url file sha256
  local url=$1 file=$2 sum=$3
  if [ ! -f "src/$file" ]; then
    echo "fetch $file"
    curl -fsSL --retry 3 --connect-timeout 20 --max-time 900 -o "src/$file.part" "$url"
    mv "src/$file.part" "src/$file"
  fi
  if [ -n "$sum" ]; then
    echo "$sum  src/$file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; exit 1; }
  else
    echo "no checksum pinned for $file: $(sha256sum "src/$file" | cut -d' ' -f1)"
  fi
}

fetch https://github.com/unicode-org/icu/releases/download/release-79.1/icu4c-79.1-sources.tgz \
      icu4c-79.1-sources.tgz 06fa63ecb521048f8082f12ad988f6b678d90cbd88aafce24cbc94902b191d99
fetch https://snowballstem.org/dist/libstemmer_c-3.2.0.tar.gz \
      libstemmer_c-3.2.0.tar.gz 561a8864a7dd6a366c0faca88d76b516d844a8b4505949a1dc43cd21bb76eea2
fetch https://github.com/RoaringBitmap/CRoaring/releases/download/v5.5.5/roaring.c roaring.c 190216f349ac5aad430b58ed9b9d5b1966a2462a14ce045b0ca59d8507a9cee8
fetch https://github.com/RoaringBitmap/CRoaring/releases/download/v5.5.5/roaring.h roaring.h 1a336307c3b5a732045941d1bd33232aa4b45661366ac9643c583e6d67513a34
echo "sources ready in third_party/src"
