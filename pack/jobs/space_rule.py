#!/usr/bin/env python3
"""直连库开工前复核要多少盘（graphics/tools/direct/direct_lib.py 在抽取之后、翻译编译之前调；Mac / Windows 两条链都过这里，#265：预检放过、编到一半写满白编十几分钟）。
规则：本模块的 pack_direct_bytes（按原件大小分两档：小于 3 GB 记 5 GB、其余记 15 GB，出包预检同一份）与「抽取表着色器数 × DIRECT_SHADER_BYTES」取大（只有这里知道着色器数）。出包空间规则（常量、选包、原件大小、
  直连两档、整次出包要多少盘）只在本模块定义：library.py（出包预检）、winenv.py（Windows 环境检查）直接 import，本模块只用标准库，direct_lib.py 当脚本调
  （剃刀 H11：原先这里和 winenv 各用 AST 解析 library.py 源码再 exec 片段，被读的文件一重构就抛「共用定义缺失或不唯一」）。原件大小在这一步不知道，用产物根里解出的游戏内容
  （romfs/ exefs/ aoc/）代替——即预检 pack_input_bytes 取的 contentBytes 那一项。已在盘上的同款直连缓存（重出包复用）从中扣掉。
11-10 去掉原先「按着色器抽取量 × 系数」（缓存 9× + 库 3×）：抽取字节数不是直连占用的驱动量，抽取产物有的是压缩过的着色器档案、有的是原文——
  0100559011740000 抽取 0.48 GiB、01008FC00D964000 抽取 1.66 GiB，两款直连缓存峰值却差不多（6.3 / 7.0 GiB），乘系数注定一款多估一款少估。
CLI：space_rule.py direct-check <产物根> <直连缓存目录> <输出目录> → 恒退 0（复核自己没跑成退 2）：预估不够只打一行【黄】提示、不拦（不许堵门，11-14）"""
import os, shutil, sys

CONTENT_DIRS = ("romfs", "romfs.std", "exefs", "aoc")   # romfs.std：RomFS 不落散文件时的镜像（与 romfs/ 二选一，tree_bytes 对单个文件同样量）


def tree_bytes(path):
    """目录实际分配的字节（不跟软链，按 inode 去重）；不在 = 0"""
    seen, total, stack = set(), 0, [os.fspath(path)]
    while stack:
        p = stack.pop()
        try: st = os.lstat(p)
        except OSError: continue
        if (st.st_dev, st.st_ino) in seen or os.path.islink(p): continue
        seen.add((st.st_dev, st.st_ino))
        total += st.st_blocks * 512 if hasattr(st, "st_blocks") else st.st_size
        if os.path.isdir(p):
            try: stack.extend(os.path.join(p, n) for n in os.listdir(p))
            except OSError: pass
    return total


# 双轨包多占的空间（10-24 01006F8002326000实测，RomFS 10.7 GB）：直连库中间缓存（S2A_DIRECT_CACHE，跨次复用）+ 包里的直连库，按游戏体积换算
# 出包要多少盘（11-08 维护者）：不看这款做没做过，第一次出包也算得准，只有一个数。
#   解包 = 原包 × 1.1（解包目录已完整在盘上时算 0）；Mac 的资源包是 APFS 克隆不另算，Windows（NTFS 无克隆）再加一份原包 × 1.1；
#   直连（翻译缓存 + 着色器库 + 收尾临时，取上限）：原包 < 3 GB 记 5 GB，其余 15 GB（最坏样本巫师3 缓存 7.5 + 库 1.5 × 2 份 + 收尾临时约 4）
PACK_SMALL_BYTES, DIRECT_SMALL_BYTES, DIRECT_LARGE_BYTES = int(3e9), int(5e9), int(15e9)


def selected_packages(game):
    bases = [p for p in game["packages"] if p["type"] == "base"]
    updates = [p for p in game["packages"] if p["type"] == "update"]
    # Integrated packages can leave duplicate version=0 records after an old
    # source is moved or removed. Prefer a source that still exists so a stale
    # first entry cannot pin the build to the wrong game version.
    def rank(package):
        path = package.get("path") or ""   # 没记路径的包（夹具 / 旧目录）当作在
        available = os.path.exists(path) if os.path.isabs(path) else True
        return package["version"], available

    result = ([max(bases, key=rank)] if bases else [])
    if updates:
        result.append(max(updates, key=rank))
    dlc = {}
    for p in game["packages"]:
        if p["type"] == "dlc" and (p["titleId"] not in dlc or rank(dlc[p["titleId"]]) < rank(p)):
            dlc[p["titleId"]] = p
    return result + [dlc[k] for k in sorted(dlc)]


def pack_input_bytes(packages):
    # 同一 sha 的原件只计一次：取原件大小与所选各项内容字节的大值（NSZ/XCZ 解开比原件大）。
    # 压缩合集（XCZ 等）一个原件装多项，各项内容互不重叠，contentBytes 相加（#444：原先按 sha 去重只留最后一项，解开后的量少算）。
    # NUP 容器不同：不压缩、容器内按块去重，更新是整份打过补丁的文件系统、和本体共用数据块，各项 contentBytes 重叠——
    #   0400486024DEC000：文件 25.62 GB，本体、更新各 26.27 GB，相加成 52.5 GB 多估一倍——本体 / 更新取最大值，不相加；
    #   DLC 是另一棵文件树、不和本体共用（#444：010015100B514000 带 DLC，取最大值时整份漏算）——DLC 各项相加
    by_sha = {}
    for p in packages:
        size, summed, largest, dlc, nup = by_sha.get(p["sha256"], (0, 0, 0, 0, False))
        content = p.get("contentBytes", 0)
        is_dlc = p.get("type") == "dlc"
        by_sha[p["sha256"]] = (max(size, p.get("size", 0)), summed + content, largest if is_dlc else max(largest, content),
                               dlc + (content if is_dlc else 0), nup or p.get("format") == "nup")
    return sum(max(size, largest + dlc if nup else summed) for size, summed, largest, dlc, nup in by_sha.values())


def pack_direct_bytes(size):
    return DIRECT_SMALL_BYTES if size < PACK_SMALL_BYTES else DIRECT_LARGE_BYTES


def pack_need_bytes(packages, unpacked=False, windows=None, image=False, clones=None, links=True, keep_unpack=False, unpack_bytes=None):
    """这次出包要多少盘；packages 已按 selected_packages 选定。unpacked：解包目录已完整在盘上。windows 不给就按当前系统。
    image：打 iPhone 包（单一 IMG 交付）——出包最后一步把资源整份写成 game.img（真数据，克隆不了），写时解包目录、直连缓存都还在盘上，
    再加一份原件大小（11-11 实测 0400486024DEC000：IMG 24.49 GiB ≈ 资源内容 24.47 GiB，整次峰值 57.90 GiB 出在写 IMG 时；原先没算，预留 43.9 GB < 峰值 62.2 GB）。
    直连段按原件大小分两档（pack_direct_bytes）。11-10 删掉 #265 的「按着色器抽取量 × 系数」：抽取字节数不是直连占用的驱动量——
    抽取产物有的是压缩过的着色器档案（0100559011740000 只有 23 个、0.48 GiB），有的是原文（01008FC00D964000 1.66 GiB），两者直连编译缓存峰值
    却差不多（6.3 / 7.0 GiB），乘系数注定一款多一款少；那条分支从 #265 起读的是 v2 旧路径、从没生效过。按原件的规则在克隆感知、只量出包作业目录的
    峰值上三款都够（单测 test_space_budget_covers_measured_peaks）。
    #746（11-12）：写 IMG 时边写边删解包出来的 RomFS（pack/img.py convert --consume，出包收尾本来就清整个出包工作目录，只是提前），
    RomFS 那份逐文件挪进 game.img，不再同时占两份——IMG 这一项记 0。只有 keep_unpack（同一款接着还要出别的平台、或这次复用了已有解包：
    可能和别的产物共享克隆，删了腾不出来）才照旧再加一份原件大小。原先一律加：256 GB 的 Mac 出 43 GB 的游戏预估 106 GB，从能出变成出不了"""
    size = pack_input_bytes(packages)
    # unpack_bytes：解包目录在存放位置那块盘上实际要占的字节（按该卷分配单元逐文件向上取整，library.nup_unpack_bytes；#444：exFAT 1 MB 簇上
    #   解包中间目录与解包目录两份共 5 万个文件、13.4 GB，实占 59 GB）。给了就按它，不给按原件大小；解包、不能克隆另占的资源包、建不了链接的两份都是这同一棵树
    copy = (size if unpack_bytes is None else unpack_bytes) * 11 // 10
    windows = os.name == "nt" if windows is None else windows
    # clones=False：存放位置所在卷不能克隆（Mac 上 HFS+ 等非 APFS，v0.3.1 放开后克隆自动退回普通拷贝），资源包同 Windows 再占一份；不给 = 按系统
    no_clone = windows or clones is False
    direct = pack_direct_bytes(size)
    # links=False：存放位置所在卷建不了目录链接（Windows exFAT / FAT32，v0.3.1 确认后照用）：产物根走实目录模式，RomFS 不进共享存储、
    #   出包工作根也整份拷——按多两份算
    return (0 if unpacked else copy) + (copy if no_clone else 0) + (0 if links else 2 * copy) + direct + (size if image and (keep_unpack or unpacked) else 0)


def _existing(path):
    path = os.fspath(path)
    while path and not os.path.exists(path): path = os.path.dirname(path)
    return path or "/"


def _free(vol):
    """盘余量；S2A_DISK_FREE_G（GB，单测注入，与 ipa.sh / refresh 同名口径）给了就用它，单测结果不随本机磁盘余量变（11-09 门禁机余量紧时
    直接真跑 direct_lib.py 的单测被复核拦下、桩工具没调起）"""
    g = os.environ.get("S2A_DISK_FREE_G")
    return shutil.disk_usage(vol) if not g else shutil._ntuple_diskusage(0, 0, int(float(g) * 1e9))


# 开工前复核按着色器数（11-11）：直连段写盘 = 每个着色器的翻译结果（源码 + 表）+ 按源码量编的批库与 Metal 编译结果 + 收尾打包，
#   随要翻译编译的着色器数与每个的大小走，不随原件大小。实测（Mac 0.2.9 冷出包、临时库、library.disk_bytes 每 5 秒，从本复核那一刻量起）：
#     01008FC00D964000：原件 10.8 GB、29391 个，复核后涨 5.8 GiB → 0.20 MiB / 个（翻译 0.06 + 批库：源码 2.8 GB）
#     0400A3A01EE4C000：原件 6.7 GB、99084 个，翻译 13.6 GiB（0.14 MiB / 个）+ 批库源码 63 GB 按已编一半的斜率 0.37 外推 23 GiB
#       → 复核后约 41 GiB、0.43 MiB / 个；原件那一档只记 15 GB，少估近 3 倍（原先只靠运行时外推复核在半路停下）
#   出包预检拿不到着色器数（抽取在后面），这里是第一处知道的地方，又在大量写盘之前：按「着色器数 × 每个的上界」与原件那一档取大，保证不少估。
#   上界 0.5 MiB 盖住 0.43 留 16%；Switch 1 小着色器的款多估（01008FC00D964000 记 15.4 GB，原件那一档本来也是 15）。按每个着色器大小定更准的量在后续。
#   着色器数 = 抽取表 extract/split/shader_map.tsv 的唯一键（比实际翻译的键少几十个，后者还并进 S2Arm 抽取的独有键）
DIRECT_SHADER_BYTES = 512 << 10


def shader_count(root):
    """抽取阶段着色器表的唯一键数；没有表 = 0（只按原件那一档）"""
    p = os.path.join(root, "extract", "split", "shader_map.tsv")
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return len({c[3] for c in (l.rstrip("\n").split("\t") for l in f if l.strip() and not l.startswith(("#", "package\t"))) if len(c) > 3})
    except OSError:
        return 0


def direct_check(root, cache_dir, out_dir, free=_free):
    """→ None（够）或给玩家看的一句（不够）。缓存目录与输出目录各在哪块盘都核这一个数（同一块盘只核一次）"""
    size = sum(tree_bytes(os.path.join(root, d)) for d in CONTENT_DIRS)
    shaders = shader_count(root)
    rule = max(pack_direct_bytes(size), shaders * DIRECT_SHADER_BYTES); need = max(0, rule - tree_bytes(cache_dir))
    vols = {}
    for d in (cache_dir, out_dir): v = _existing(d); vols.setdefault(os.stat(v).st_dev, v)
    for vol in vols.values():
        have = free(vol).free
        if have < need:
            return f"磁盘空间不足：直连库预计还要 {need / 1e9:.1f} GB（游戏内容 {size / 1e9:.2f} GB、着色器 {shaders} 个，按原件那一档与着色器数取大记 {rule / 1e9:.1f} GB），这块盘只剩 {have / 1e9:.1f} GB"
    return None


def cli(argv, check=None):
    """direct-check 命令行 → 退出码。预估只当提示、不拦（不许堵门，11-14：预估偏保守，玩家盘可能其实够；诊断 D-QDK9 按预估停下、玩家没出路）：
    够 / 不够都退 0，不够只打一行【黄】；真写满时写盘报 ENOSPC，那才停，归 disk_full。复核自己没跑成（如缺 library.py）退 2"""
    if len(argv) != 4 or argv[0] != "direct-check":
        print("用法：space_rule.py direct-check <产物根> <直连缓存目录> <输出目录>"); return 2
    try: why = (check or direct_check)(*argv[1:])
    except Exception as e:
        print(f"【红】直连库开工前空间复核没跑成：{type(e).__name__}: {e}"); return 2
    if why: print(f"【黄】{why}：这是预估，照常开编；真写满时会按盘满停，释放空间后重试即可")
    return 0


if __name__ == "__main__":
    sys.exit(cli(sys.argv[1:]))
