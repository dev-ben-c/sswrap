#!/bin/sh
# Build sswrap's opengl32.dll with 32-bit MinGW inside a throwaway Debian container:
#   docker run --rm -v "$PWD":/src -w /src debian:stable-slim sh build-in-docker.sh
set -e
apt-get update -qq && apt-get install -y -qq gcc-mingw-w64-i686 python3 >/dev/null
python3 gen.py
i686-w64-mingw32-gcc -std=gnu99 -O2 -Wall -Wno-unused-function -shared -static-libgcc \
  -Wl,--enable-stdcall-fixup -o opengl32.dll sswrap.c thunks.S opengl32.def -lgdi32 -luser32
i686-w64-mingw32-strip opengl32.dll
echo BUILD-OK
