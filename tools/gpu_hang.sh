#!/bin/bash
# GPU 卡死签名（11-07 维护者：测试设备2 整机卡死——宿主日志已出 GPU Hang，局还在跑了几分钟、跑到超时才收）。
# 真机执行器 tools/dev_run.sh 局中每轮查一次，各线自己的真机脚本 source 本文件复用同一份判据：
#   source tools/gpu_hang.sh; if L=$(s2a_gpu_hang <host.log>); then <立即结束 App 进程、收局、判红>; fi
# s2a_gpu_hang <文件>：文件里有 Metal / IOGPU 报的命令缓冲卡死或页错误 → 打出第一行（截 160 字）、返回 0；没有 → 返回 1。
#   签名：「GPU Hang」（Metal 错误描述）、kIOGPUCommandBufferCallbackErrorHang、PageFault（kIOGPUCommandBufferCallbackErrorPageFault 等）。
#   只认系统 / 驱动报出的原文，不认宿主自己的「GPU 慢…继续等」（加载帧单个命令缓冲执行好几秒是正常现象，见 dev_run.sh slowgpu）
# 11-07 补：dev_run 局中读的是控制台流（只有原生层 LOG 的 [s2a] 行）。系统原文在 [dm] / [gal] 行里，走 stderr 只进机上 host.log，控制台流看不到——
#   测试设备2 金刚现场（ownplay/t040074a01bf12000_1107081853_机2/host.log 第 666–670 行）控制台流只有一行
#   「[s2a] 【红】直连异常（不切换，继续跑）：GPU 命令缓冲报错：未能完成操作。（MTL4CommandQueueErrorDomain错误1。）」，原签名一个都不中。
#   所以再认宿主自己打进 [s2a] 的三种行：「GPU 命令缓冲报错」（直连命令缓冲执行出错，含被别的进程挂死牵连的；真机局上一律当卡死停）、
#   「GPU 挂死安全停」（fix/gpu-hang-safe-stop 起 iOS 停在错误页、不退出）、「GPU 超时收局」（10-31 起的旧版收局行）；另补系统原文 ErrorTimeout
S2A_GPU_HANG_RE='GPU Hang|kIOGPUCommandBufferCallbackErrorHang|kIOGPUCommandBufferCallbackErrorTimeout|PageFault|GPU 命令缓冲报错|GPU 挂死安全停|GPU 超时收局'
# 增量扫（11-07 启动器线：自写脚本每 2 秒全量重扫 syslog，几百 MB 后一轮极慢，拖到设备锁看门狗 600 秒强收，win_line/dk_net/run2.sh）：
#   每个（文件, 调用方 shell $$）记上次扫到的字节偏移，下次只读新增部分；多回看 4 KB（S2A_GPU_HANG_BACK），签名跨两轮交界的那行不漏；
#   文件变短（轮转 / 整份重写成更短）→ 从头扫。调用方常用 L=$(s2a_gpu_hang f) 在子 shell 里调、变量带不回来，所以偏移存小状态文件
#   （S2A_GPU_HANG_STATE_DIR；不给就放被扫文件旁边 .<文件名>.gpu_hang_pos.<调用方 $$>，随局目录清，不往 TMPDIR 丢）。命中后照旧返回 0、打第一行（截 160 字）
s2a_gpu_hang() {
  local f=$1 sz pos st l back=${S2A_GPU_HANG_BACK:-4096}
  sz=$(stat -c %s "$f" 2> /dev/null || stat -f %z "$f" 2> /dev/null) || return 1
  # 状态缺省放在被扫文件旁边（随局目录一起清）；目录不可写才退回 TMPDIR（11-11 合入运维线：原先一律放 TMPDIR、从不删，每局一个，积了 6400 多个）
  if [ -n "${S2A_GPU_HANG_STATE_DIR:-}" ]; then st=$S2A_GPU_HANG_STATE_DIR/s2a_gpu_hang.$(printf '%s' "$f" | cksum | cut -d' ' -f1).$$
  elif [ -w "$(dirname "$f")" ]; then st=$(dirname "$f")/.$(basename "$f").gpu_hang_pos.$$
  else st=${TMPDIR:-/tmp}/s2a_gpu_hang.$(printf '%s' "$f" | cksum | cut -d' ' -f1).$$; fi
  pos=$(cat "$st" 2> /dev/null || echo 0); case $pos in ''|*[!0-9]*) pos=0;; esac
  [ "$sz" -lt "$pos" ] && pos=0
  [ "$pos" -gt "$back" ] && pos=$((pos - back)) || pos=0
  l=$(tail -c +$((pos + 1)) "$f" 2> /dev/null | LC_ALL=C /usr/bin/grep -aE -m1 "$S2A_GPU_HANG_RE")
  printf '%s\n' "$sz" > "$st" 2> /dev/null
  [ -n "$l" ] || return 1
  printf '%s\n' "$(printf '%s' "$l" | cut -c1-160)"
}
