#!/bin/bash
# RomFS 共享存储（pack/roots.sh，11-03）单测：临时目录，秒级。bash tests/roots_romfs_store_test.sh
# 期望按规则手写（不复述实现）：
#   播种提交后现行 romfs 是指向本款存储的相对软链、内容与原件逐字节相同、存储整棵只读；后续阶段开工不再复制 RomFS（与现行同一个 inode）；
#   往 romfs 写 / 删 / 建一律失败且存储不变；中止回滚干净；钉住的旧版本完整可读；要改 RomFS 的阶段（ROOT_OWN_ROMFS=1）拿到自己的真目录、提交成新身份；
#   旧身份在还有版本指向时保留、没有版本指向后删；romfs 软链指向别处 → 拒绝开工；整根快照（tools/clonedir.py）拿到真目录；
#   资源内容键（pack/resource_input_key.py）对存储软链与同内容真目录相同，指向别处的软链报错；并发重放声明 romfs 时以本阶段内容为准
#   以上按开发 / 门禁机（S2A_ROMFS_SEAL=1）；玩家机（不设）：存储里没有不可写的项（原件只读也加回写位），普通 rm -rf 整根删得掉
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
export S2A_ROOTS=$T/roots S2A_ROOTS_SHARED=$T/shared S2A_ROOTS_PRIV=$T/priv S2A_ROMFS_SEAL=1; source "$H/pack/roots.sh"   # 下面到「玩家机」一节前按开发 / 门禁机（设只读）
ino() { stat -c %i "$1" 2> /dev/null || stat -f %i "$1"; }
mode() { stat -c %a "$1" 2> /dev/null || stat -f %Lp "$1"; }
# 原件：两层目录 + 三个文件（含点开头的真文件）
O=$T/orig; mkdir -p $O/romfs/Layout/sub $O/exefs; echo a > $O/romfs/a.bin; echo lyt > $O/romfs/Layout/x.bflyt; echo cfg > $O/romfs/Layout/sub/.build.config; echo main > $O/exefs/main
W=$(root_begin g s0) && cp -cR $O/romfs $O/exefs $W/ 2> /dev/null || cp -R $O/romfs $O/exefs $W/; root_commit g s0
C=$(root_cur g); L=$(readlink $C/romfs); SID=$T/roots/.romfs_store/g/${L##*/}
ck "播种提交：现行 romfs 是指向本款存储的相对软链（${L}）、内容与原件逐字节相同" '[ -L $C/romfs ] && [[ "$L" == ../.romfs_store/g/* ]] && diff -r $O/romfs $C/romfs > /dev/null'
ck "存储整棵只读：身份目录、子目录、文件都没有写位" '[ -z "$(find $SID \( -perm -u+w -o -perm -g+w -o -perm -o+w \) -print -quit)" ]'
W=$(root_begin g s1); ck "下一阶段开工：W 的 romfs 同一条软链、文件与现行同一个 inode（不复制 RomFS）" '[ "$(readlink $W/romfs)" = "$L" ] && [ "$(ino $W/romfs/a.bin)" = "$(ino $C/romfs/a.bin)" ]'
( echo X > $W/romfs/a.bin ) 2> /dev/null; w1=$?; rm -f $W/romfs/a.bin 2> /dev/null; w2=$?; mkdir $W/romfs/new 2> /dev/null; w3=$?
python3 -c "open('$W/romfs/Layout/x.bflyt', 'r+b').write(b'Z')" 2> /dev/null; w4=$?
ck "往 romfs 写（覆盖 rc=${w1}、删 rc=${w2}、建目录 rc=${w3}、原地改字节 rc=${w4}）全部失败，存储内容不变" '[ $w1 != 0 ] && [ $w2 != 0 ] && [ $w3 != 0 ] && [ $w4 != 0 ] && diff -r $O/romfs $SID > /dev/null'
_root_rm $W/romfs; ck "删 W 的 romfs 软链只删链接：存储仍只读、内容不变" '[ ! -e $W/romfs ] && [ -z "$(find $SID -perm -u+w -print -quit)" ] && diff -r $O/romfs $SID > /dev/null'
root_abort g s1; ck "中止：.tmp 没了、现行不动、存储身份还在" '[ ! -e $T/roots/g@s1.tmp ] && [ "$(root_cur g)" = "$C" ] && [ -d $SID ]'
W=$(root_begin g s2) && echo p > $W/prod.txt && root_commit g s2
# 要改 RomFS 的阶段（界面布局）：ROOT_OWN_ROMFS=1 → 自己的真目录（可删可写）；提交成新身份
W=$(ROOT_OWN_ROMFS=1 root_begin g ui1)
ck "ROOT_OWN_ROMFS=1 开工：W 的 romfs 是真目录、内容同现行" '[ -d $W/romfs ] && [ ! -L $W/romfs ] && diff -r $O/romfs $W/romfs > /dev/null'
rm -f $W/romfs/Layout/x.bflyt && echo wide > $W/romfs/Layout/x.bflyt; ROOT_REPLAY='romfs' root_commit g ui1
C2=$(root_cur g); L2=$(readlink $C2/romfs)
ck "布局提交成新身份：现行软链换了（${L2}）、新内容在，旧身份原样还在（上一版 s2 指向它）" '[ "$L2" != "$L" ] && [ "$(cat $C2/romfs/Layout/x.bflyt)" = wide ] && [ "$(cat $T/roots/g@s2/romfs/Layout/x.bflyt)" = lyt ] && diff -r $O/romfs $SID > /dev/null'
ck "新身份同样整棵只读" '[ -z "$(find $T/roots/.romfs_store/g/${L2##*/} -perm -u+w -print -quit)" ]'
# 钉住的旧版本：s2 被一个活着的别的进程钉住，之后连提交两版，s2 仍在且 romfs 完整可读；没钉的 ui1 被剪
sleep 30 & PP=$!; printf '%s %s\n' "$PP" "$(_root_lstart $PP)" > $T/roots/.pin.g@s2.$PP
for c in s3 s4; do W=$(root_begin g $c) && root_commit g $c; sleep 0.05; done
ck "钉住的 s2 留着、romfs 完整可读（指向旧身份），没钉的 ui1 剪掉、两个身份都还被指向所以都在" '[ -d $T/roots/g@s2 ] && diff -r $O/romfs $T/roots/g@s2/romfs > /dev/null && [ ! -e $T/roots/g@ui1 ] && [ -d $SID ] && [ -d $T/roots/.romfs_store/g/${L2##*/} ]'
kill $PP 2> /dev/null; wait $PP 2> /dev/null; W=$(root_begin g s5) && root_commit g s5
ck "钉子失效后再提交：s2 剪掉，旧身份没有版本指向 → 删；现行的身份留着" '[ ! -e $T/roots/g@s2 ] && [ ! -e $SID ] && [ -d $T/roots/.romfs_store/g/${L2##*/} ] && [ "$(ls $T/roots/.romfs_store/g | wc -l | tr -d " ")" = 1 ]'
# 安全阀：romfs 软链指向存储以外 / 别的款的存储 → 拒绝开工
C=$(root_cur g); mkdir -p $T/roots/.romfs_store/h/x
mv $C/romfs $T/romfs.keep; ln -s ../.romfs_store/h/x $C/romfs; r1=0; root_begin g bad1 > /dev/null 2>&1 || r1=1
rm $C/romfs; ln -s $O/romfs $C/romfs; r2=0; root_begin g bad2 > /dev/null 2>&1 || r2=1
rm $C/romfs; mv $T/romfs.keep $C/romfs
ck "romfs 软链指向别的款的存储（拒=${r1}）/ 存储以外（拒=${r2}）→ 拒绝开工、不留 .tmp" '[ $r1 = 1 ] && [ $r2 = 1 ] && [ ! -e $T/roots/g@bad1.tmp ] && [ ! -e $T/roots/g@bad2.tmp ]'
ln -s /etc $C/stray; r3=0; root_begin g bad3 > /dev/null 2>&1 || r3=1; rm $C/stray
ck "romfs 之外的软链照旧拒绝（拒=${r3}）" '[ $r3 = 1 ] && [ ! -e $T/roots/g@bad3.tmp ]'
# 整根快照（Mac 局 in_snap / 私有根起步 / 兄弟根）：clonedir 跟随软链，拿到真目录、内容相同
python3 $H/tools/clonedir.py "$C" $T/snap > /dev/null
ck "整根快照：romfs 是真目录、内容与现行相同" '[ -d $T/snap/romfs ] && [ ! -L $T/snap/romfs ] && diff -r $C/romfs $T/snap/romfs > /dev/null'
# 资源内容键：存储软链 = 同内容真目录；软链指向别处 → 报错
K1=$(python3 -c "import sys; sys.path.insert(0, '$H/pack'); import resource_input_key as r; print(r.resource_key(sys.argv[1]))" "$C" 2>&1)
mkdir -p $T/phys && python3 $H/tools/clonedir.py "$C" $T/phys/g@p > /dev/null
K2=$(python3 -c "import sys; sys.path.insert(0, '$H/pack'); import resource_input_key as r; print(r.resource_key(sys.argv[1]))" $T/phys/g@p 2>&1)
mkdir -p $T/phys/.romfs_store/other/x && cp -R $O/romfs/. $T/phys/.romfs_store/other/x/ && python3 $H/tools/clonedir.py "$C" $T/phys/g@q > /dev/null && rm -rf $T/phys/g@q/romfs && ln -s ../.romfs_store/other/x $T/phys/g@q/romfs
K3=$(python3 -c "import sys; sys.path.insert(0, '$H/pack'); import resource_input_key as r; print(r.resource_key(sys.argv[1]))" $T/phys/g@q > /dev/null 2>&1; echo $?)
ck "资源内容键：存储软链根（${K1}）= 同内容真目录根（${K2}）；软链指向别的款的存储 → 报错（rc=${K3}）" '[[ "$K1" =~ ^[0-9a-f]{16}$ ]] && [ "$K1" = "$K2" ] && [ "$K3" != 0 ]'
# 并发重放：布局阶段（声明 romfs）开工后别的阶段先提交，布局后提交 → 重放到新现行，romfs 以布局阶段的为准、别的阶段产物也在
A=$(ROOT_OWN_ROMFS=1 root_begin g ui2); B=$(root_begin g rw2); echo rw > $B/rw.txt; ROOT_REPLAY='rw.txt' root_commit g rw2
rm -f $A/romfs/a.bin && echo a2 > $A/romfs/a.bin; ROOT_REPLAY='romfs' root_commit g ui2 2> /dev/null
C=$(root_cur g); ck "并发重放：现行 = ui2、romfs 是存储软链且 a.bin=a2、先提交的 rw.txt 在、存储只读" '[ "$(basename $C)" = g@ui2 ] && [ -L $C/romfs ] && [ "$(cat $C/romfs/a.bin)" = a2 ] && [ -f $C/rw.txt ] && [ -z "$(find $T/roots/.romfs_store/g/ -perm -u+w ! -type d -print -quit)" ]'
# 旧格式根（romfs 是真目录，现网全部存量）：开工照旧克隆，提交收进存储
mkdir -p $T/roots/k@old && cp -R $O/romfs $O/exefs $T/roots/k@old/ && printf '%s\n' k@old > $T/roots/k@现行
W=$(root_begin k n0); ( echo X > $W/romfs/a.bin ) 2> /dev/null; w5=$?; root_abort k n0
ck "旧格式根开工：克隆来的 romfs 是真目录但已只读，写入失败（rc=${w5}）" '[ $w5 != 0 ]'
W=$(root_begin k n1) && root_commit k n1; C=$(root_cur k)
ck "旧格式根：下一阶段提交后现行 romfs 换成存储软链、内容相同，旧版 k@old 原样是真目录" '[ -L $C/romfs ] && diff -r $O/romfs $C/romfs > /dev/null && [ -d $T/roots/k@old/romfs ] && [ ! -L $T/roots/k@old/romfs ]'
# 只出程序（pack/ipa.sh）逐项 cp -cRH：临时目录里 romfs 是真目录、内容相同
mkdir -p $T/ao && for e in $(ls -A "$C"); do cp -cRH "$C/$e" $T/ao/ 2> /dev/null || cp -RH "$C/$e" $T/ao/; done
ck "只出程序逐项复制（-H）：romfs 是真目录、内容相同" '[ -d $T/ao/romfs ] && [ ! -L $T/ao/romfs ] && diff -r $O/romfs $T/ao/romfs > /dev/null'
# 玩家机（S2A_ROMFS_SEAL 不设）：存储在玩家看得见的游戏库里，不许留只读（玩家在访达里删不掉旧产物）。
#   原件故意整棵只读（模拟旧版本留下 / 克隆带来的只读）：播种提交后存储里一个没写位的项都没有，界面布局新身份也一样；普通 rm -rf 整根删干净
export S2A_ROOTS=$T/proots S2A_ROMFS_SEAL=; P=$T/porig; cp -R $O $P; chmod -R a-w $P/romfs
W=$(root_begin p s0) && cp -R $P/romfs $P/exefs $W/ && root_commit p s0
PL=$(readlink $(root_cur p)/romfs); PS=$T/proots/.romfs_store/p/${PL##*/}
ck "玩家机播种：存储里没有不可写的项、内容与原件相同" '[ -z "$(find $PS ! -type l ! -perm -u+w -print -quit)" ] && diff -r $O/romfs $PS > /dev/null'
W=$(ROOT_OWN_ROMFS=1 root_begin p ui1) && echo wide > $W/romfs/Layout/x.bflyt && ROOT_REPLAY='romfs' root_commit p ui1
ck "玩家机布局新身份：同样没有不可写的项" '[ -z "$(find $T/proots/.romfs_store/p ! -type l ! -perm -u+w -print -quit)" ]'
rm -rf $T/proots 2> /dev/null; ck "玩家机：普通 rm -rf 整根删干净（不先改权限）" '[ ! -e $T/proots ]'
chmod -R u+w $T 2> /dev/null; rm -rf $T; [ $bad = 0 ] && echo "RomFS 共享存储单测全过" || { echo "RomFS 共享存储单测有不通"; exit 1; }
