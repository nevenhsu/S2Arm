#!/usr/bin/env bash
# 门禁只看：android/tests/vk/* android/tests/vkhost/* android/tests/first_draw_unnorm* android/native/nvn/backend_vulkan.c android/native/nvn/backend_vulkan_wsi.inc android/native/nvn/backend_vulkan_clear.inc android/native/nvn/backend_vulkan_draw_tex.inc android/native/nvn/backend_vulkan_draw_tex_spv.h graphics/nvn/replay/rp_draw_tex.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_format.c android/native/nvn/nvn_vk_driver_cache.c android/native/nvn/nvn_vk_driver_cache.h android/native/direct/sha256.c android/native/direct/sha256.h android/native/direct/spirv_workgroup.h android/native/direct/compile_worker.c android/native/direct/compile_worker.h android/native/gfx_plat/nvk_shader.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c graphics/nvn/gfx/nvn/nvn_backend.h graphics/nvn/gfx/nvn/nvn_obj.h graphics/nvn/gfx/nvn/nvn_refl.h android/native/direct/vk_device.c android/native/direct/vk_device.h android/native/direct/gs_path.c android/native/direct/gs_path.h graphics/nvn/gfx/tex/nvn_bcdec.c graphics/nvn/gfx/nvn/include/* tests/vulkan/*
# 门禁本机：Linux 直接执行；macOS 用本机 colima 中已准备的公共 Vulkan 镜像（同 tests/shader_cases_test.sh），gate-host 不具备此环境
# 安卓 nvn_gpu_* Vulkan 后端契约（android/tests/vk/run.sh：lavapipe + Khronos validation 跑 C01–C08 + C23/C25–C27 + BC1–BC7 逐像素解码；本机约 40 秒）
# 11-01 接线：扫描分母按 tests 目录后查出原先没有门禁调它。环境没备好时 tests/vulkan/run.sh 退 75（环境未就绪），不在门禁里装环境。
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd)
# 维护方：本机名单（tools/machine.local，不进仓库）配了 S2A_ANDROID_VK_REMOTE（如 air）→ 整步经私有 remote.sh 送那台跑，本机不起 colima 容器
#   （11-13：本机风扇，安卓的 Vulkan 契约用例改在专门的安卓专用 Mac上跑）。远端环境（PATH / NDK / docker 配置目录）由 S2A_ANDROID_VK_ENV 给，
#   配了 S2A_ANDROID_VK_LOCK 就和那台机上其他跑 run.sh 的人用同一把目录锁（mkdir 成功才跑、跑完删），没配不上锁。只在树是提交态时派（remote.sh 只收提交态），
#   已在远端里（S2A_REMOTE_NAME）不再派；没配 / 没有私有工具的（贡献者）照旧本机跑。远端每 60 秒打一行心跳：本机这头只是等 ssh，
#   门禁的无进展看门狗看日志 / CPU / 磁盘，安静 300 秒就判挂死（11-13 15:05 main 守望因此红）；等锁与跑用例都在这段心跳里（取代只在等锁时打心跳的写法）
. "$H/tools/machine.sh" 2> /dev/null || true
if [ -n "${S2A_ANDROID_VK_REMOTE:-}" ] && [ -z "${S2A_REMOTE_NAME:-}" ] && [ -f "${S2A_PRIVATE_TOOLS:-}/remote.sh" ] && [ -z "$(git -C "$H" status --porcelain 2> /dev/null)" ]; then
  cd "$H"
  # shellcheck disable=SC2086  # S2A_ANDROID_VK_ENV 是「K=V K=V」一串，按空格拆给 env
  rc=0; env S2A_REMOTE_PRIO=2 bash "$S2A_PRIVATE_TOOLS/remote.sh" --host "$S2A_ANDROID_VK_REMOTE" --name gate-android-vk --rev HEAD --timeout 2700 -- \
    bash -c 'L=$1; shift; ( while sleep 60; do echo "安卓 Vulkan 用例在远端跑着 / 等锁（$(date +%T)）"; done ) & hb=$!; held=
      trap "kill $hb 2> /dev/null; [ -n \"\$held\" ] && { rm -f \"$L/owner\"; rmdir \"$L\"; }" EXIT
      # 锁里记持有者（首字段 pid，与那台机上手跑的包装脚本同格式）。远端超时用 SIGKILL 收进程时 EXIT 收尾不跑，没记持有者的目录锁留下就再没人能拿
      #   （11-14 留下的一把空锁三天没人删，此后每次都等满 30 分钟退 75）：等锁时持有者 pid 已不在、或没记持有者且锁比最长持有（2700 秒）还旧，就收回
      if [ -n "$L" ]; then for _ in $(seq 360); do
          if mkdir "$L" 2> /dev/null; then held=1; echo "$$ $(date +%T) $PWD" > "$L/owner"; break; fi
          o=$(awk "{print \$1; exit}" "$L/owner" 2> /dev/null); age=$(( $(date +%s) - $(stat -c %Y "$L" 2> /dev/null || stat -f %m "$L" 2> /dev/null || date +%s) ))
          if { [ -n "$o" ] && ! kill -0 "$o" 2> /dev/null; } || { [ -z "$o" ] && [ "$age" -gt 2760 ]; }; then
            echo "收回陈旧的安卓 Vulkan 用例锁 ${L}（持有 pid ${o:-未记}，已 ${age} 秒）"; rm -f "$L/owner"; rmdir "$L" 2> /dev/null; continue; fi
          sleep 5; done; [ -n "$held" ] || { echo "等不到安卓 Vulkan 用例锁 ${L}（30 分钟）"; exit 75; }; fi
      env "$@" bash tests/android_vk_test.sh' _ "${S2A_ANDROID_VK_LOCK:-}" ${S2A_ANDROID_VK_ENV:-} || rc=$?
  # 远端自己没跑成（remote.sh 退 125：盘不足 / 连不上 / 排队超时）≠ 用例红：退 75 环境未就绪，列车等环境、不归责分支（11-14：远端机盘只剩 16G，contrib/pr1013 被当真红退回，同批 9 支逐支核、叠在上面的 10 支推测批作废，约 40 分钟）
  [ $rc = 125 ] && { echo "安卓 Vulkan 用例：环境未就绪（远端没跑成：remote 退出 125，原因见上面的【红】行）"; exit 75; }; exit $rc
fi
exec bash "$H/android/tests/vk/run.sh"
