#!/bin/sh
# Build opengl32.dll (32-bit Windows DLL) with MinGW-w64. Needs i686-w64-mingw32-gcc and python3.
# On Debian/Ubuntu: apt install gcc-mingw-w64-i686 python3
set -e
cd "$(dirname "$0")"
python3 gen.py
i686-w64-mingw32-gcc -std=gnu99 -O2 -Wall -Wno-unused-function -shared -static-libgcc \
  -Wl,--enable-stdcall-fixup -o opengl32.dll sswrap.c thunks.S opengl32.def -lgdi32 -luser32
i686-w64-mingw32-strip opengl32.dll
echo BUILD-OK
