#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 无号 32 位乘以常数 2^k 取高位改写成右移（Simplification.TryRewriteMultiplyHighU32ByPowerOfTwo）。直接跑生产函数 Simplification.RunPass；不需要 GPU / 游戏数据。
# 门禁只看：tests/mulhi_pow2/* tests/mulhi_pow2_test.sh compat/tree/src/Ryujinx.Graphics.Shader/Translation/Optimizations/*
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
# Resolve the repository's pinned SDK, not whichever SDK the caller's cwd selects.
cd "$ROOT/compat/tree"
DN=${DOTNET:-dotnet}
"$DN" build "$ROOT/tests/mulhi_pow2/MulhiPow2.csproj" -c Release \
  --artifacts-path "$OUT/dotnet" -o "$OUT/tool" \
  -p:UseSharedCompilation=false -m:1 -v quiet
"$DN" "$OUT/tool/dsh_cmp.dll"
