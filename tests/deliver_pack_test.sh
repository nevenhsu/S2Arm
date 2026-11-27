#!/bin/bash
# 一键出包交付资源包单测（pack/oneclick/deliver_pack.py，约 1 秒）：bash tests/deliver_pack_test.sh
# 期望（按资源包目录约定手写：index.tsv 列的文件 + index.tsv + manifest.json）：
#   ① 目标不存在 → 整份：克隆 3 个、删 0、目标文件逐字节等于源、带清单
#   ② 源只改了 a.bin、删了 sub/c.bin、新增 d.bin → 增量：克隆 2 个（a、d）、删 1 个，没变的 b.bin 不重做（inode 不变），sub/ 删空后目录也没了，内容逐字节等于源
#   ③ 目标里没变的文件被截短（大小对不上）→ 照样重克隆（克隆 1 个），不信旧索引
#   ④ 目标缺 manifest.json（上次中途失败）→ 整份重做
#   ⑤ 源没有 manifest.json（资源包不完整）→ 退出非 0
#   ⑥ #329 库内形态：源里没有 romfs.img、有 romfs.files/ → 交付 romfs.files 两个文件（内容等于源）、目标里旧格式的 romfs.img 删掉；再交付一次镜像行没变 → 不动（克隆 0）
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0; trap 'rm -rf "$T"' EXIT
PY=${S2A_PY:-python3}
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
mk() {   # mk <目录> <文件:内容>…：写文件、index.tsv（路径 字节数 SHA-256）与 manifest.json
  local d=$1 kv p; shift; rm -rf "$d"; mkdir -p "$d"
  for kv in "$@"; do p=${kv%%:*}; mkdir -p "$d/$(dirname "$p")"; printf '%s' "${kv#*:}" > "$d/$p"; done
  for kv in "$@"; do p=${kv%%:*}; printf '%s\t%s\t%s\n' "$p" "$(wc -c < "$d/$p" | tr -d ' ')" "$(shasum -a 256 "$d/$p" | cut -c1-64)"; done > "$d/index.tsv"
  echo '{"version":2}' > "$d/manifest.json"; }
same() { diff -r "$1" "$2" > /dev/null; }
DP() { "$PY" "$H/pack/oneclick/deliver_pack.py" "$@"; }
mk $T/s a.bin:AAA b.bin:BBBB sub/c.bin:CC
o1=$(DP $T/s $T/d); r1=$?
ck "① 整份：rc=${r1}、输出「${o1}」" '[ $r1 = 0 ] && [ "$(cut -f3,4,5 <<< "$o1")" = "3	0	整份" ] && same $T/s $T/d'
ib=$(stat -c %i $T/d/b.bin 2> /dev/null || stat -f %i $T/d/b.bin)
mk $T/s a.bin:AAAX b.bin:BBBB d.bin:DDDDD
o2=$(DP $T/s $T/d); r2=$?; ib2=$(stat -c %i $T/d/b.bin 2> /dev/null || stat -f %i $T/d/b.bin)
ck "② 增量：rc=${r2}、输出「${o2}」、b.bin inode ${ib} → ${ib2}、sub/ $([ -e $T/d/sub ] && echo 还在 || echo 没了)" \
  '[ $r2 = 0 ] && [ "$(cut -f1,3,4,5 <<< "$o2")" = "3	2	1	增量" ] && [ "$ib" = "$ib2" ] && [ ! -e $T/d/sub ] && same $T/s $T/d'
printf 'B' > $T/d/b.bin; o3=$(DP $T/s $T/d); r3=$?
ck "③ 目标文件被截短：rc=${r3}、输出「${o3}」" '[ $r3 = 0 ] && [ "$(cut -f3,5 <<< "$o3")" = "1	增量" ] && same $T/s $T/d'
rm $T/d/manifest.json; o4=$(DP $T/s $T/d); r4=$?
ck "④ 目标缺清单 → 整份：rc=${r4}、输出「${o4}」" '[ $r4 = 0 ] && [ "$(cut -f3,5 <<< "$o4")" = "3	整份" ] && same $T/s $T/d'
rm $T/s/manifest.json; DP $T/s $T/d > /dev/null 2>&1; r5=$?
ck "⑤ 源不完整 → 退出非 0：rc=${r5}" '[ $r5 != 0 ]'
mk $T/s6 x.bin:XX; printf 'romfs.img\t5\tfeed\n' >> $T/s6/index.tsv; mkdir -p $T/s6/romfs.files/sub; printf 'IMA' > $T/s6/romfs.files/a; printf 'GE' > $T/s6/romfs.files/sub/b; printf 'X' > $T/s6/romfs.files/stray   # stray 不在索引里：不交付
"$PY" - "$T/s6" <<'P'
import struct, sys
d = sys.argv[1]; names = [b'a', b'sub/b', b'ui/o']; sizes = [3, 2, 1]; offs = [0, 3, -1]   # ui/o 是覆盖层（偏移 -1），不在镜像里
idx = b'S2ARFS01' + struct.pack('<qi', 6, 3); po = 0
for n, z in zip(names, sizes): idx += struct.pack('<qqii', 0, z, po, len(n)); po += len(n)
open(d + '/romfs.s2aidx', 'wb').write(idx + struct.pack('<i', po) + b''.join(names) + struct.pack('<i', 0))
open(d + '/romfs.imgmap', 'wb').write(b'S2ARIM01' + struct.pack('<qi', 5, 3) + struct.pack('<3q', *offs))
P
mk $T/d6 x.bin:XX; printf 'IMAGE' > $T/d6/romfs.img; cp $T/s6/index.tsv $T/d6/index.tsv
o6=$(DP $T/s6 $T/d6); r6=$?
ck "⑥ 库内形态：rc=${r6}、输出「${o6}」、旧 romfs.img $([ -e $T/d6/romfs.img ] && echo 还在 || echo 删了)" '[ $r6 = 0 ] && [ ! -e $T/d6/romfs.img ] && [ "$(cat $T/d6/romfs.files/a)$(cat $T/d6/romfs.files/sub/b)" = IMAGE ] && [ ! -e $T/d6/romfs.files/stray ]'
o7=$(DP $T/s6 $T/d6); r7=$?
ck "⑥ 再交付一次镜像行没变：rc=${r7}、输出「${o7}」（克隆 0）" '[ $r7 = 0 ] && [ "$(cut -f3 <<< "$o7")" = 0 ]'
# ⑦ RomFS 不落散文件：源里没有 romfs.img、没有 romfs.files/、有 romfs.std（库内镜像，不在索引里）→ 交付这一个文件（克隆 x.bin 0 个 + romfs.std 1 个 = 1），
#    目标里上一次的 romfs.files/（⑥ 交付的）删掉；再交付一次镜像行没变 → 克隆 0；负对照：源里镜像也没有 → 退出非 0（不许静默交付一个缺 RomFS 的包）
mk $T/s7 x.bin:XX; cp $T/s6/index.tsv $T/s6/romfs.s2aidx $T/s6/romfs.imgmap $T/s7/; printf 'STDIMAGE' > $T/s7/romfs.std
o8=$(DP $T/s7 $T/d6); r8=$?
ck "⑦ 镜像形态：rc=${r8}、输出「${o8}」、romfs.std「$(cat $T/d6/romfs.std 2> /dev/null)」、romfs.files $([ -e $T/d6/romfs.files ] && echo 还在 || echo 删了)" \
  '[ $r8 = 0 ] && [ "$(cut -f3 <<< "$o8")" = 1 ] && [ "$(cat $T/d6/romfs.std)" = STDIMAGE ] && [ ! -e $T/d6/romfs.files ] && [ ! -e $T/d6/romfs.img ]'
o9=$(DP $T/s7 $T/d6); r9=$?
ck "⑦ 再交付一次镜像行没变：rc=${r9}、输出「${o9}」（克隆 0）" '[ $r9 = 0 ] && [ "$(cut -f3 <<< "$o9")" = 0 ]'
rm $T/s7/romfs.std; rm -rf $T/d7; DP $T/s7 $T/d7 > /dev/null 2>&1; r10=$?
ck "⑦ 负对照：源里镜像 / 逐文件 / romfs.img 都没有 → 退出非 0：rc=${r10}" '[ $r10 != 0 ]'
# ⑧ 非 macOS（Windows）交付 romfs.std 先硬链、不另占盘（诊断 #9ac7f7：原先整份真拷，镜像占两份、大游戏交付写满盘 WinError 112）：
#    进程内把 sys.platform 设成 win32 调 link_image → 目标与源同一个 inode（链接数 2）；目标已有旧文件先换掉
printf 'OLD' > $T/s7/romfs.std.old; printf 'STDIMAGE8' > $T/s8std
o11=$(cd "$H" && python3 -c "
import os, sys; sys.path.insert(0, 'pack/oneclick'); import deliver_pack as d
sys.platform = 'win32'
d.link_image('$T/s8std', '$T/s7/romfs.std.old')
a, b = os.stat('$T/s8std'), os.stat('$T/s7/romfs.std.old')
print(a.st_ino == b.st_ino, b.st_nlink, open('$T/s7/romfs.std.old').read())" 2>&1)
ck "⑧ 非 macOS 交付镜像：同 inode / 链接数 / 内容「${o11}」" '[ "$o11" = "True 2 STDIMAGE8" ]'
[ $bad = 0 ] && echo "deliver_pack 单测全过" || { echo "deliver_pack 单测有不通"; exit 1; }
