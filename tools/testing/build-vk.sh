#!/bin/sh
# The build-vk build (out/build-vk) with the workspace's usual inputs; the log goes to out/build-final-vk.log.
# Paths are relative to the workspace (the repository's parent); run from anywhere.
here=$(cd "$(dirname "$0")" && pwd)
ws=$(cd "$here/../../.." && pwd)
cd "$ws/sr-recomp" || exit 1
nsp=$(cd "$ws/dump" 2>/dev/null && ls *.nsp 2>/dev/null | head -1)
[ -n "$nsp" ] || { echo "no .nsp in $ws/dump"; exit 1; }
nsp="../dump/$nsp"   # relative: the venv python is a Windows program
../.toolchains/venv/Scripts/python.exe -I tools/build_runtime.py --gen ../out/gen --obj ../out/obj --exefs ../out/exefs \
  --nsp "$nsp" --vendor ../vendor --clang ../.toolchains/llvm-mingw-20261020-ucrt-x86_64/bin/clang.exe --out ../out/build-vk \
  --profile vulkan --deps ../.toolchains --xlate ../.toolchains/sr_xlate --dotnet ../.toolchains/dotnet --fonts ../out/fonts \
  --dlss ../.toolchains/dlss-dl/nvngx_dlss.dll --streamline ../.toolchains/streamline \
  --fidelityfx ../.toolchains/fidelityfx/FidelityFX-SDK-2.4.0 > ../out/build-final-vk.log 2>&1
echo "build exit $?"
tail -n 1 ../out/build-final-vk.log | cut -c1-120
