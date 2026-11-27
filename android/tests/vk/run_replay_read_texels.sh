#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# ReadTexels 读 GPU 内容（rp_read_texels.inc）+ 真后端 + lavapipe，两级回放关 / 开各一遍，始终开 validation 与同步校验。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
if [ "$(uname -s)" = Darwin ]; then
    export S2A_VULKAN_CACHE="$repo/build/vulkan-cache"
    exec bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh
fi
icd=
for f in /usr/share/vulkan/icd.d/lvp_icd.json /usr/share/vulkan/icd.d/lvp_icd.*.json "$HOME"/.local/share/vulkan/icd.d/lvp_icd*.json; do
    if [ -f "$f" ]; then icd=$f; break; fi
done
layer=
for d in /usr/share/vulkan/explicit_layer.d "$HOME"/.local/share/vulkan/explicit_layer.d; do
    if [ -f "$d/VkLayer_khronos_validation.json" ]; then layer=$d; break; fi
done
if [ -z "$icd" ] || [ -z "$layer" ]; then echo '【红】缺 lavapipe 或 validation layer' >&2; exit 2; fi
mkdir -p "$repo/build/vk-rt-tmp"
out=$(mktemp -d "$repo/build/vk-rt-tmp/run.XXXXXX")
trap 'rm -rf "$out"' EXIT
# -Wno-address：夹具在同一编译单元里给了提交锁的强定义，.inc 里「弱引用在不在」的判断对它恒真
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-address -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    -I "$repo/android/tests/vkhost" -I "$repo/graphics/nvn/third_party/vulkan-headers/include" \
    -I "$repo/graphics/nvn" -I "$repo/graphics/nvn/replay" -I "$repo/graphics/nvn/gfx/nvn" \
    -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/android/native" -I "$repo/android/native/nvn" -I "$repo/android/native/direct" \
    "$repo/android/tests/vk/t_replay_read_texels.c" "$repo/graphics/nvn/replay/rp_copy_t2b.c" \
    "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" \
    "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/graphics/nvn/gfx/nvn/nvn_tex.c" \
    "$repo/android/native/nvn/nvn_vk_bcdec.c" "$repo/android/native/nvn/nvn_vk_driver_cache.c" "$repo/android/native/direct/sha256.c" \
    "$repo/android/native/nvn/nvn_pipe_flight.c" "$repo/android/native/nvn/nvn_vk_pipe_once.c" \
    -Wl,--wrap=vkAllocateDescriptorSets -Wl,--wrap=vkGetPhysicalDeviceFormatProperties \
    -Wl,--wrap=vkCreatePipelineCache -Wl,--wrap=vkGetPipelineCacheData \
    -Wl,--wrap=vkCreateGraphicsPipelines -Wl,--wrap=vkCreateComputePipelines -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES=$icd VK_LAYER_PATH=$layer VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1
for dq in 0 1; do   # 两级回放关 / 开：读回段在 DQ 下由录制线程录，提交 / 等完的顺序要照样成立
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    set +e
    timeout 180 "$out/test" 2>&1 | tee "$out/log-$dq"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q 'device: llvmpipe' "$out/log-$dq" || ! grep -q '^PASS replay ReadTexels GPU' "$out/log-$dq" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/log-$dq"; then
        echo "【红】回放 ReadTexels GPU Vulkan 验证失败（DQ=${dq}）rc=${rc}" >&2; exit 1
    fi
done
unset S2A_TEST_NVK_DQ
echo '【绿】回放 ReadTexels GPU：数组 / 通道清色 / 三维 / BC1 原块 / 格式拒收，DQ 关 / 开，validation 0 条' 
