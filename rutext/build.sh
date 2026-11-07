#!/bin/bash
# Cross-compile RuText.asi (x64 DLL) in a throwaway mingw-w64 container.
set -euo pipefail
cd "$(dirname "$0")"
docker build -q -t rdr2-asi-build . >/dev/null
mkdir -p out
docker run --rm -v "$PWD":/src -w /src rdr2-asi-build \
  x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -Wall -Wextra -Wno-cast-function-type \
  -shared -static -static-libgcc -static-libstdc++ -s \
  -o out/RuText.asi src/rutext.cpp -lpsapi
ls -l out/RuText.asi
