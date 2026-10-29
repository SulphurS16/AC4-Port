# Third-party components

## This repository's license

The port's own code is under the MIT license (`LICENSE`). It includes code from the ACFA native
port by the same author, under the same license. The license covers the source code in this
repository only. It grants nothing for Armored Core 4 or any other game data, keys or system
software, which users must supply themselves.

## In this repository

| Component | Use | License |
| --- | --- | --- |
| zlib 1.3.1 (Jean-loup Gailly, Mark Adler), `third_party/zlib`, unmodified | decompression of the game's containers | zlib license (`third_party/zlib/LICENSE`) |

## Build tools (downloaded by the launcher, not included)

The launcher downloads these from their official release pages and checks each archive against
the SHA-256 listed in `data/toolchain.txt`:

| Tool | Version | License |
| --- | --- | --- |
| llvm-mingw (github.com/mstorsjo/llvm-mingw): LLVM clang, lld, libc++, libunwind, compiler-rt; mingw-w64 headers and runtime | 20260908, UCRT, x86_64 | LLVM: Apache-2.0 WITH LLVM-exception (`licenses/LLVM-LICENSE.txt`); mingw-w64 runtime: its notices in `licenses/COPYING.MinGW-w64-runtime.txt` (from the toolchain) |
| CMake (github.com/Kitware/CMake) | 4.4.3 | BSD-3-Clause |
| Ninja (github.com/ninja-build/ninja) | 1.13.2 | Apache-2.0 |

## In the programs built from this repository

`AC4-Launcher.exe` (prebuilt on the release page) and the game executable built on your PC are
linked statically. They contain parts of LLVM's libc++, libunwind and compiler-rt (Apache-2.0 WITH
LLVM-exception, `licenses/LLVM-LICENSE.txt`), the mingw-w64 runtime, among it gdtoa by David M. Gay
(its copyright and license notices, which binary distributions must carry, are in
`licenses/COPYING.MinGW-w64-runtime.txt` and at the end of the release zip's `README.txt`), and
zlib (above).

## Used from Windows at run time (not redistributed)

Direct3D 11, DXGI, the HLSL compiler (d3dcompiler_47), Direct2D, DirectWrite and the system fonts
(Times New Roman, Bahnschrift or Segoe UI), XInput, WASAPI, WinHTTP, Windows CNG, the Universal
C Runtime and the common controls.

## The game

ARMORED CORE 4 is © FromSoftware, Inc. The port contains none of its data or code; each
player provides their own copy, which is prepared and built on their PC.
