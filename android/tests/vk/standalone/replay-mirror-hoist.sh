#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个执行。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-replay-mirror-hoist"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$S2A_VK_LAYER_DIR/VkLayer_khronos_validation.json" ] || { echo 'Khronos validation layer missing' >&2; exit 2; }
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
for shader in quad.vert quad.frag; do
    "${GLSLC:-glslc}" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
done
inc=()
for dir in build/c883-ndk android/tests/vkhost graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn \
    graphics/nvn/gfx/nvn/include graphics/nvn/gfx/tex android/native android/native/nvn android/native/direct runtime/include; do
    inc+=(-I "$repo/$dir")
done
src=(
    android/tests/vk/t_replay_mirror_hoist.c
    graphics/nvn/replay/{rp_buf,rp_entry,rp_state,nvn_unimpl}.c
    graphics/nvn/gfx/nvn/{nvn_dirty,nvn_fmt_expand,nvn_tex}.c
    graphics/nvn/gfx/tex/nvn_deswizzle.c
    android/native/nvn/{backend_vulkan,nvn_pipe_flight,nvn_vk_pipe_once,nvn_vk_format,nvn_vk_bcdec,nvn_vk_driver_cache}.c
    android/native/direct/{vk_device,gs_path,sha256}.c
    android/native/capprobe/capprobe_host.c
)
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    "${inc[@]}" "${src[@]/#/$repo/}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_DRIVER_FILES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
printf '%s\n' 'khronos_validation.validate_sync = true' > "$out/vk_layer_settings.txt"
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
failed=0
for dq in 0 1; do
    if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
    for arm in off gap overlap pending alias; do
        marker='^MIRROR_HOIST '
        if [ "$arm" = alias ]; then marker='^VPOOL_ALIAS '; fi
        log="$out/dq$dq-$arm.log"
        rc=0
        timeout 20 "$out/test" "$out/shaders" "$arm" > "$log" 2>&1 || rc=$?
        grep "$marker" "$log" || true
        if [ "$rc" -ne 0 ] || ! grep -q '^device: llvmpipe' "$log" ||
           ! grep -q "$marker" "$log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD' "$log"; then
            echo "【红】池镜像前置 DQ=$dq arm=$arm rc=$rc" >&2
            cat "$log" >&2
            failed=1
        fi
    done
done
exit "$failed"
