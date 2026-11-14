#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# MSL 的 NoContraction 辅助函数（spvFAdd / spvFSub / spvFMul / 三种矩阵乘）去掉 optnone、改内联后，舍入与原版逐位相同、且编译器无从再融合。
# 对照 = 同版本未改动的 spirv-cross 命令行（Homebrew 1.4.363.0，= 链锁），不跑被测实现推期望：
#   ① 进程内（本仓源码）生成的辅助函数：没有 optnone，fma 的 0 / 1 取自 volatile 读
#   ② 正式出库参数（-fmetal-math-mode=relaxed -fpreserve-invariance）编成 AIR：本仓版 0 次辅助函数调用、fma 没有字面 0 加数 / 字面 1 乘数；
#      原版有调用（对照本身成立）
#   ③ GPU 上同一份输入（随机 + 有号零 / 非正规数 / 极大 / 半个 ulp 附近 / inf / nan）：本仓版输出与原版逐字相同
#   ④ 负对照：辅助函数换成不受保护的 l * r / l + r / l - r，输出必须与原版不同（证明这份比对能看出融合）
# 门禁只看：graphics/third_party/spirv_cross/spirv_msl.cpp tests/msl_nocontraction/* tests/msl_nocontraction_test.sh
set -u
R=$(cd "$(dirname "$0")/.." && pwd)
PY=${S2A_PY:-${PY:-$HOME/.s2arm/venv/bin/python3}}
for t in glslangValidator spirv-cross xcrun clang; do command -v $t > /dev/null || { echo "缺 ${t}，本机做不了"; exit 75; }; done
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
bad=0; ck() { if [ "$2" = 1 ]; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
MATH=(-fmetal-math-mode=relaxed -fpreserve-invariance)
XC=(--msl --msl-version 30100 --msl-decoration-binding)
glslangValidator -V --target-env vulkan1.1 "$R/tests/msl_nocontraction/t.comp" -o "$T/t.spv" > /dev/null || { echo "不通 glslang 编不出测试着色器"; exit 1; }
spirv-cross "${XC[@]}" "$T/t.spv" --output "$T/ref.metal" || { echo "不通 命令行 spirv-cross 失败"; exit 1; }
(cd "$R/translator" && "$PY" -c "import spvc_inproc, sys; r = spvc_inproc.run(sys.argv[1:]); sys.exit(r.returncode)" "${XC[@]}" "$T/t.spv" --output "$T/new.metal") || { echo "不通 进程内 spirv-cross 失败"; exit 1; }
# ④ 的负对照：原版辅助函数换成不受保护的写法
"$PY" - "$T/ref.metal" "$T/plain.metal" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
s = s.replace('return fma(T(1), l, r);', 'return l + r;').replace('return fma(T(-1), r, l);', 'return l - r;').replace('return fma(l, r, T(0));', 'return l * r;')
s = s.replace('[[clang::optnone]] ', 'inline ')
open(sys.argv[2], 'w').write(s)
PY
n_opt=$(grep -cE '\[\[clang::optnone\]\] (T|vec<|matrix<)[^(]*spvF(Add|Sub|Mul)' "$T/new.metal"); n_vol=$(grep -c 'thread volatile float' "$T/new.metal")
ck "① 本仓版浮点辅助函数没有 optnone（$n_opt 处），fma 的 0 / 1 取自 volatile（$n_vol 处，6 个函数）" $([ "$n_opt" = 0 ] && [ "$n_vol" = 6 ] && echo 1)
for v in ref new plain; do
  xcrun -sdk macosx metal "${MATH[@]}" -c "$T/$v.metal" -o "$T/$v.air" 2> "$T/$v.err" || { echo "不通 $v 编不过："; cat "$T/$v.err"; exit 1; }
  xcrun -sdk macosx metallib "$T/$v.air" -o "$T/$v.metallib"
  xcrun -sdk macosx metal "${MATH[@]}" -S -emit-llvm "$T/$v.metal" -o "$T/$v.ll" 2> /dev/null
done
c_ref=$(grep -cE 'call .*@_Z[0-9]+spvF' "$T/ref.ll"); c_new=$(grep -cE 'call .*@_Z[0-9]+spvF' "$T/new.ll")
lit=$(grep -oE '@air\.fma\.(f32|v[0-9]f32)\([^)]*\)' "$T/new.ll" | grep -cE '(float|<[0-9] x float>) (0\.000000e\+00|zeroinitializer)\)$|^@air\.fma\.[^(]*\((float|<[0-9] x float>) (1\.000000e\+00|<float 1)')
ck "② AIR：原版 $c_ref 次辅助函数调用，本仓版 $c_new 次；本仓版 fma 字面 0 加数 / 1 乘数 $lit 处" $([ "$c_ref" -gt 0 ] && [ "$c_new" = 0 ] && [ "$lit" = 0 ] && echo 1)
clang -fobjc-arc -O2 -framework Metal -framework Foundation "$R/tests/msl_nocontraction/run.m" -o "$T/run" || { echo "缺 Metal 编译环境，本机做不了"; exit 75; }
out=$("$T/run" "$T/ref.metallib" "$T/new.metallib" "$T/plain.metallib") || { echo "$out"; echo "不通 GPU 跑不起来"; exit 1; }
echo "$out" | sed 's|'"$T"'/||g'
ck "③ GPU：本仓版输出与原版逐字相同" $(echo "$out" | grep -q '^相同 .*new.metallib' && echo 1)
ck "④ 负对照：不受保护的写法与原版不同" $(echo "$out" | grep -q '^不同 .*plain.metallib' && echo 1)
exit $bad
