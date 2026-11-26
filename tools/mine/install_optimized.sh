#!/bin/bash
# 「最新 S2Arm 加上我的優化」：把 upstream main 合進自用分支 mine/optimized-s2arm（工作樹 wt-perf-pack），
#   編宿主確認、推到自己的倉庫（origin），再用 install_main.sh 整份出包換掉 /Applications/S2Arm.app（不備份；要回原版就下載官方版覆蓋）。
#
# 用法：bash tools/mine/install_optimized.sh（或 work/tools/install_optimized.sh，連到這裡） [--no-install]
#   --no-install：只合併、編、推，不出包安裝
#
# 合併有衝突就停下（不自動解）：保留 upstream 的新寫法、補回優化後再跑一次。
set -euo pipefail
WS=/Users/neven/Documents/projects/switch
REPO=$WS/s2arm
WT=$WS/wt-perf-pack
BR=mine/optimized-s2arm
INSTALL=1; [ "${1:-}" = "--no-install" ] && INSTALL=0
say() { printf '\n== %s\n' "$*"; }

say "取最新 upstream main"
git -C "$REPO" fetch -q upstream main
[ -d "$WT" ] || git -C "$REPO" worktree add "$WT" "$BR"
cd "$WT"
[ "$(git branch --show-current)" = "$BR" ] || git checkout "$BR"
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "工作樹有未提交的改動，先處理再跑"; exit 1; }

if git merge-base --is-ancestor upstream/main HEAD; then
  say "已包含最新 upstream main（$(git rev-parse --short upstream/main)）"
else
  say "合併 upstream main $(git rev-parse --short upstream/main)"
  if ! git merge --no-edit -q upstream/main; then
    echo "合併有衝突：$(git diff --name-only --diff-filter=U | tr '\n' ' ')"
    git merge --abort
    echo "已取消合併。保留 upstream 的新寫法、補回優化後再跑一次。"
    exit 1
  fi
fi

# 等 upstream 合入的 PR（#2242 AV1 影片解碼、#2262 帧比例实测帧时）：合進 upstream 後分支裡那份就是重複的，提醒（不自動拿掉）
if command -v gh >/dev/null; then
  for pr in 2242 2262; do
    [ "$(gh pr view "$pr" --repo jhaizhou-ops/S2Arm --json state -q .state 2>/dev/null)" = "MERGED" ] && echo "提醒：#$pr 已合進 upstream，分支裡的那份可以拿掉了"
  done
fi

say "編宿主確認（帶視窗前端）"
S2A_CS=1 S2A_MAC_FRONTEND=1 make -j"$(sysctl -n hw.ncpu)" build/s2arm-host > build/install_optimized_host.log 2>&1 || { tail -20 build/install_optimized_host.log; echo "宿主編不過"; exit 1; }
[ "$(strings build/s2arm-host | grep -c NSWindow)" -gt 0 ] || { echo "宿主沒有視窗前端（NSWindow 0）"; exit 1; }

say "推到自己的倉庫"
git push -q origin "$BR"
echo "origin/$BR = $(git rev-parse --short HEAD)"

[ "$INSTALL" = 1 ] || { say "--no-install：不出包安裝"; exit 0; }

say "出包並換掉 /Applications/S2Arm.app（install_main.sh）"
bash "$(dirname "$0")/install_main.sh" "$BR"

# 出包產物（wt-upstream-main/build/macos 裡的 S2Arm.app、S2ArmGame.app）不給聚焦搜尋收錄：
#   真目錄改名成 macos.noindex，原路徑留符號連結（install_main.sh / 測試腳本照舊用 build/macos）
B=$WS/wt-upstream-main/build
if [ -d "$B/macos" ] && [ ! -L "$B/macos" ]; then
  mv "$B/macos" "$B/macos.noindex" && ln -s macos.noindex "$B/macos"
  echo "出包產物改放 build/macos.noindex（聚焦搜尋不再列出多的 S2Arm / S2ArmGame）"
fi
say "完成：/Applications/S2Arm.app = $BR @ $(git -C "$WT" rev-parse --short HEAD)"
