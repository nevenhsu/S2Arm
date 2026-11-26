#!/bin/bash
# 「裝最新 main」：用 upstream main 最新提交出整份 Mac App，直接換掉正式版 /Applications/S2Arm.app。
# 不碰 S2Arm Test。不備份舊版（要回原版就下載官方版覆蓋）；~/S2Arm-backup 只放 entitlements（prod-ent.plist）。
#
# 用法：bash work/tools/install_main.sh [提交或分支，缺省 upstream/main]
#
# 已內建 11-23 第一次安裝踩到的兩個坑：
#   1. 字型生成器 make_open_fonts.py 預設用系統 Python（Homebrew 3.15）跑，缺 fonttools / skia-pathops → 一律用主檢出 venv（S2A_FONTS_PY）。
#   2. 最低系統檢查 verify_minimum_macos：Runtime 裡的 bash / readline / libintl 等取自本機 Homebrew，
#      bottle 是按本機 macOS 27 出的，高於宣告的 15.0 → 整包紅掉。只在本機裝、本機用，改成警告後繼續。
set -euo pipefail

WS=/Users/neven/Documents/projects/switch
REPO=$WS/s2arm
WT=$WS/wt-upstream-main
VENV_PY=$REPO/build/venv/bin/python3
APP=/Applications/S2Arm.app
BACKUP=$HOME/S2Arm-backup
REF=${1:-upstream/main}

say() { printf '\n== %s\n' "$*"; }

# 0. 正式版或遊戲在跑就停（換掉跑著的 App 會壞）
if pgrep -f "$APP/Contents/MacOS/" >/dev/null || pgrep -f s2arm-host >/dev/null; then
  echo "🔴 S2Arm 或遊戲正在跑，請先退出再重跑這支腳本。"; pgrep -fl "$APP/Contents/MacOS/|s2arm-host" || true; exit 1
fi
[ -x "$VENV_PY" ] || { echo "🔴 找不到 $VENV_PY（主檢出的 venv）"; exit 1; }

# 1. 取最新 main，放在獨立的分離工作樹（不動主檢出的分支）
say "取 $REF"
git -C "$REPO" fetch upstream main
if git -C "$REPO" worktree list --porcelain | grep -qx "worktree $WT"; then
  git -C "$WT" checkout --detach --force "$REF"
else
  [ -e "$WT" ] && { echo "🔴 $WT 存在但不是工作樹，請看一下再刪"; exit 1; }
  git -C "$REPO" worktree prune
  git -C "$REPO" worktree add --detach "$WT" "$REF"
fi
COMMIT=$(git -C "$WT" log -1 --format='%h %ci %s')
echo "$COMMIT"

# 2. 出 App（坑 1：字型用 venv；坑 2：最低系統檢查改警告）
say "出 App（約 10~20 分鐘）"
export S2A_PY=$VENV_PY S2A_FONTS_PY=$VENV_PY DOTNET_ROOT=$HOME/.dotnet PATH=$HOME/.dotnet:$PATH
mkdir -p "$WT/build/macos"
LOG=$WT/build/macos/install_main.log
# S2A_REUSE_APP=1：同一提交剛出好的 App 還在（上一輪停在安裝那步）就不再出一次
if [ "${S2A_REUSE_APP:-0}" = 1 ] && [ -d "$WT/build/macos/S2Arm.app/Contents/MacOS" ] && [ "$(git -C "$WT" rev-parse HEAD)" = "$(cat "$WT/build/macos/.built_commit" 2>/dev/null)" ]; then
  echo "S2A_REUSE_APP=1：沿用剛出好的 App（$(git -C "$WT" rev-parse --short HEAD)）"
else
( cd "$WT" && python3 - <<'PY' ) > "$LOG" 2>&1 || { tail -30 "$LOG"; echo "🔴 出包失敗，完整日誌：$LOG"; exit 1; }
import sys, runpy
sys.path.insert(0, "launchers/macos")
import runtime_audit
_orig = runtime_audit.verify_minimum_macos
def _warn_only(app, declared):
    try:
        return _orig(app, declared)
    except RuntimeError as e:
        print("🟡 本機安裝：最低系統檢查改為警告（Homebrew bottle 按本機系統出）\n" + str(e))
        return -1
runtime_audit.verify_minimum_macos = _warn_only
sys.argv = ["launchers/macos/build_app.py"]
runpy.run_path("launchers/macos/build_app.py", run_name="__main__")
PY
git -C "$WT" rev-parse HEAD > "$WT/build/macos/.built_commit"
fi
NEW=$WT/build/macos/S2Arm.app
[ -d "$NEW/Contents/MacOS" ] || { echo "🔴 沒有產出 $NEW"; exit 1; }
grep '🟡 本機安裝' -A8 "$LOG" || true

# 3. 檢查產物：簽名、遊戲宿主有視窗
HOST="$NEW/Contents/Resources/Engine/Binaries/S2ArmGame.app/Contents/MacOS/s2arm-host"
[ "$(strings "$HOST" | grep -c NSWindow)" -gt 0 ] || { echo "🔴 內建宿主沒有 NSWindow（沒帶 S2A_MAC_FRONTEND=1？）"; exit 1; }

# 4. 取正式版 entitlements（Team ID 簽的原版才取，之後沿用）。不備份 App（使用者 11-26：要回原版就自己下載覆蓋）
mkdir -p "$BACKUP"
ENT=$BACKUP/prod-ent.plist
if [ -d "$APP" ] && codesign -dv "$APP" 2>&1 | grep -q '^TeamIdentifier=[A-Z0-9]'; then
  codesign -d --entitlements :- "$APP" > "$ENT" 2>/dev/null
fi
# entitlements 檔不見（11-26 又遇到）：從目前裝著的 App 取（ad-hoc 簽的也帶著同一份 5 項）
if [ ! -s "$ENT" ] && [ -d "$APP" ] && codesign -d --entitlements :- "$APP" > "$ENT" 2>/dev/null && plutil -lint -s "$ENT"; then echo "entitlements 從已裝的 App 取回"; fi
[ -s "$ENT" ] || { echo "🔴 沒有正式版 entitlements（$ENT）"; exit 1; }

# 5. 安裝：先放 .new 簽好、驗過，再整份換上
say "安裝到 $APP"
STAGE=/Applications/S2Arm.app.new
rm -rf "$STAGE"
ditto "$NEW" "$STAGE"
codesign --force -s - --options runtime --entitlements "$ENT" "$STAGE/Contents/MacOS/S2ArmApple"
codesign --force -s - --options runtime --entitlements "$ENT" "$STAGE"
codesign --verify --deep --strict "$STAGE"
if [ -d "$APP" ]; then mv "$APP" /Applications/S2Arm.app.old; fi
mv "$STAGE" "$APP"
rm -rf /Applications/S2Arm.app.old
LSR=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
"$LSR" -f "$APP"
# 出包產物不留 App：build_app.py 每次把上一份改名 S2Arm-previous.app 留著，連同剛出的 S2Arm.app 會被啟動台 / 打開方式登記成多個 S2Arm
for x in "$WT/build/macos/S2Arm-previous.app" /Applications/S2Arm.app.old; do [ -e "$x" ] && { "$LSR" -u "$x" 2>/dev/null; rm -rf "$x"; }; done
"$LSR" -u "$NEW" 2>/dev/null; rm -rf "$NEW"   # 已裝進 /Applications，出包目錄不留第二份 App

say "完成"
echo "提交：$COMMIT"
/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" -c "Print :CFBundleVersion" -c "Print :S2ABuildID" "$APP/Contents/Info.plist"

