#!/usr/bin/env python3
"""Switch 2 导出目录 → 一键出包的解包目录（pack/ns2_dump.py）。

Switch 2 游戏现在拿到手的形态是主机上导出的松散目录（11-04 维护者 给的金刚 / raiders 都是），不是 NSP / XCI：
    <导出目录>/base/exefs/{main,main.npdm,rtld,sdk,subsdk*}   本体
    <导出目录>/base/romfs/...
    <导出目录>/updates/<版本号>/exefs|romfs/...                更新（导出工具挂的是打过补丁的文件系统 = 叠加后的全量）
    <导出目录>/dlc/<DLC 标题号>/<n>/romfs/...                   追加内容（也认 dlc/<标题号>/romfs/）
    （.listing 是 FTP 镜像留下的目录清单，忽略）
没有控制分区（control.nacp），nx_unpack 也读不了（它只认 NCA 容器）。本脚本把它整理成 nx_unpack 解包产物同一形态：
    exefs/   取最新更新的（没有更新取本体）；NSO 原样（LZ4 / 不压缩下游都认；0x40 位透传）；ZBIC（0x80）交 nx_unpack --nso-normalize
             解成段不压缩的 NSO（S2A_NX_UNPACK = nx_unpack 程序，一键出包会给；没给就报红）
    romfs/   最新更新的 romfs 里有与本体逐字节相同的文件 = 全量（导出工具挂的是打过补丁的文件系统），只用它；
             没有 = 差量，本体叠更新（更新覆盖），打黄字说明
             镜像形态（缺省，S2A_ROMFS_IMAGE=0 回 romfs/ 散文件）：不出 romfs/ 散文件，选出的文件交 nx_unpack --romfs-image 直接建 romfs.std（标准 RomFS）；
             版本目录里是 romfs.nupmap（镜像形态的 pack/ns2_nup.py 展开）时，数据直接从 NUP 容器读
    aoc/<序号>/ + aoc.tsv   序号 = DLC 标题号 − 应用号（程序号去掉末位）− 0x1000（同 nx_unpack）
    ctl/control.nacp       导出带了控制分区（版本目录里 romfs 之外的 control.nacp）就用原件 + icon.jpg；没带才合成（README.synth 标明非原件，
                           标题取目录名、不是游戏自带信息）：16 语言标题、启动要账号、应用号（0x3038）与存档属主 = 程序号、账号存档 128 MB / 日志 64 MB；游戏代码挂临时存储 / 本机存档 / 缓存存储的（按 nn::fs 导入名判）另声明对应大小（save_kinds）
                           （字段与取值同 11-03 火纹合成件，那份在 Mac / 测试设备2 实玩过）
    unpack.tsv             title_id / name / name_zh / display_version / update / base / builds / aoc
程序号从 base/exefs/main.npdm 的 ACI0 读，不看目录名。文件一律 APFS 克隆（不占新空间），跨卷才真拷。
--consume：导出目录是调用方的中间目录、整理完就整棵删（pack/ns2_nup.py --to-extract 展开的那份）：romfs 散文件与 DLC 同盘挪过去、不留第二份
  （#444：exFAT 1 MB 簇的外接盘上克隆不了，中间目录与解包目录两份同时在盘上、共 5 万个文件、各占约 29 GB）；挪不了（跨卷等）才照常克隆 / 拷。玩家自己的导出目录不传。
同一形态的 MIT 做法见 the reference port scripts/pack_dump.py（seedds/the reference port 777a5f6c，源自 d2r-mobile）：更新 romfs 是打过补丁的全量、单独用。
先写 <out>.tmp，成功才改名；<out>/.ns2_dump_key = 源清单（路径 字节 修改时间）+ 本脚本内容的哈希，相同就复用不重做。

用法：
    ns2_dump.py --detect <目录>                 是导出目录就打印程序号、退出 0；不是退出 1
    ns2_dump.py <导出目录> <输出解包目录> [--name <款名>] [--base <本体目录>] [--consume]   本体在别处（散文件解包目录）：<导出目录> 可以是空目录
"""
import ctypes
import hashlib
import os
import shutil
import struct
import subprocess
import sys

NACP_SIZE = 0x4000
SKIP = {'.listing'}


def red(msg):
    print(f'🔴 {msg}', file=sys.stderr)
    sys.exit(1)


def read_tid(npdm):
    with open(npdm, 'rb') as f:
        b = f.read()
    if b[:4] != b'META' or len(b) < 0x80:
        raise ValueError(f'{npdm} 不是 META 头')
    aci, size = struct.unpack_from('<II', b, 0x70)
    if b[aci:aci + 4] != b'ACI0':
        raise ValueError(f'{npdm} 的 ACI0 不在 0x{aci:x}')
    return struct.unpack_from('<Q', b, aci + 0x10)[0]


def find_dump(root):
    """root 本身或下一层是导出目录（有 base/exefs/main.npdm）就返回它，否则 None。"""
    for d in [root] + sorted(os.path.join(root, n) for n in os.listdir(root) if os.path.isdir(os.path.join(root, n))):
        if os.path.isfile(os.path.join(d, 'base', 'exefs', 'main.npdm')):
            return d
    return None


def numdirs(d):
    """d 下名字是十进制数的子目录，按数值升序。"""
    if not os.path.isdir(d):
        return []
    return sorted((n for n in os.listdir(d) if n.isdigit() and os.path.isdir(os.path.join(d, n))), key=int)


def walk(root):
    """相对路径 → 绝对路径（只文件，跳过 .listing）。"""
    out = {}
    for dp, dns, fns in os.walk(root):
        dns.sort()
        for f in sorted(fns):
            if f in SKIP:
                continue
            a = os.path.join(dp, f)
            out[os.path.relpath(a, root)] = a
    return out


_libc = ctypes.CDLL(None, use_errno=True) if sys.platform == 'darwin' else None


def clone(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if _libc is not None and _libc.clonefile(src.encode(), dst.encode(), 0) == 0:
        return
    shutil.copy2(src, dst)   # 跨卷 / 非 APFS：真拷


def take(src, dst, consume):
    """src 放到 dst：consume（来源是用完即删的中间目录）先同盘改名挪过去，不占第二份；否则 / 挪不了照 clone"""
    if consume:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        try:
            os.rename(src, dst)
            return
        except OSError:
            pass
    clone(src, dst)


def normalize_exefs(exsrc, dst, tid, nxu=None):
    """把松散 exefs 规范化到 dst：处理 ZBIC NSO，并核对 main / NPDM 程序号。

    供完整 dump 转换和 S 级原始程序提取共用；不读取或写入 romfs。失败抛异常，调用方决定如何呈现。
    """
    os.makedirs(dst, exist_ok=True)
    builds = []
    for rel, src in walk(exsrc).items():
        if os.sep in rel:
            continue
        with open(src, 'rb') as f:
            hdr = f.read(0x50)
        if hdr[:4] == b'NSO0':
            fl = struct.unpack_from('<I', hdr, 0x0C)[0]
            if fl & ~0xFF:
                raise ValueError(f'{src}：NSO 标志 0x{fl:x} 有不认识的位 0x{fl & ~0xFF:x}，压缩方式不明不收')
            builds.append(f'{rel}:{hdr[0x40:0x50].hex()}')
            if not fl & 0x80:   # 解不开在这里就点名报（不等到后面找符号名时抛 Python 栈，#1522）；哈希对不上记黄照常用
                nso_segments(src, lambda n, s: print(f'🟡 程序文件 {n} 的 {s} 段数据和文件头记录的哈希对不上（多半是打过补丁 / mod，或个别块损坏），照常用'))
            if fl & 0x80:
                nxu = nxu or os.environ.get('S2A_NX_UNPACK')
                if not nxu or not os.path.isfile(nxu):
                    raise FileNotFoundError(f'{src}：NSO 标志 0x{fl:x} 带 ZBIC（0x80）压缩，要 nx_unpack 解：设 S2A_NX_UNPACK=<nx_unpack 程序>（一键出包会给）')
                target = os.path.join(dst, rel)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                r = subprocess.run([nxu, '--nso-normalize', src, target], capture_output=True, text=True)
                if r.returncode:
                    raise RuntimeError(f'{src}：ZBIC 解不开（nx_unpack 退出 {r.returncode}）：{(r.stderr or r.stdout).strip()[-300:]}')
                print(f'   {rel}：ZBIC 压缩，已用 nx_unpack 解成段不压缩')
                continue
        clone(src, os.path.join(dst, rel))
    main = os.path.join(dst, 'main')
    npdm = os.path.join(dst, 'main.npdm')
    if not os.path.isfile(main) or not os.path.isfile(npdm):
        raise FileNotFoundError(f'{exsrc} 里没有完整的 main / main.npdm')
    if read_tid(npdm) != tid:
        raise ValueError('main.npdm 程序号与本体不一致')
    return builds


# 合成控制分区里各类存档按游戏代码实际挂的建（11-05 0400486024DEC000：游戏挂临时存储 MountTemporaryStorage，合成件 TemporaryStorageSize = 0，
#   启动时 EnsureApplicationSaveData 不建它，OpenSaveDataFileSystem（fs 命令 51）回 2002-1002、游戏 Break）。
#   判法：游戏代码模块（main、subsdk*；sdk 定义全部 nn::fs 函数，不算）的动态符号名里有没有 nn::fs 对应挂载函数（导入名在 .dynstr，按段解开后找修饰名）；有就按下面的大小声明（与账号存档同量级），
#   没有就保持 0（不凭空多建）。字段位置同 Ryujinx.Horizon/Sdk/Ns/ApplicationControlProperty.cs。
SAVE_KINDS = (   # （nn::fs 修饰名片段，[(nacp 偏移, 值)…]，说明）
    (b'_ZN2nn2fs21MountTemporaryStorageE', [(0x3168, 0x08000000)], '临时存储 128 MB'),
    (b'_ZN2nn2fs19MountDeviceSaveDataE', [(0x3090, 0x08000000), (0x3098, 0x04000000)], '本机存档 128 / 64 MB'),
    (b'_ZN2nn2fs17MountCacheStorageE', [(0x3170, 0x08000000), (0x3178, 0x04000000), (0x3180, 0x0C000000)], '缓存存储 128 / 64 MB'),
)


NSO_SEGS = ('text', 'rodata', 'data')
DAMAGED = '这个程序文件坏了或被改过，请重新导出（最好导成 NSP / XCI）'


def nso_segments(path, mismatch=None):
    """NSO 三段解开（段压缩：标志 0–2 位，LZ4 块）；标志 3–5 位要求核段哈希（头 0xA0 起每段 SHA-256，按解开后的内容）就核。
    → [(段名, 字节)…]。物理上解不开 → NsoDamaged（点名文件、哪一段）。
    哈希对不上但解得开不拒（维护者 11-17：校验不过不拒收——可能是玩家打过补丁 / mod 的程序，照样能玩）：交 mismatch(文件名, 段名) 记一笔，内容照常用。
    ZBIC（0x80）不在这里解（交 nx_unpack）"""
    d = open(path, 'rb').read()
    fl = struct.unpack_from('<I', d, 0x0C)[0]
    out = []
    for i, seg in enumerate(NSO_SEGS):
        fo, _, size = struct.unpack_from('<III', d, 0x10 + i * 16)
        csz = struct.unpack_from('<I', d, 0x60 + i * 4)[0]
        raw = d[fo:fo + (csz if fl & (1 << i) else size)]
        if fl & (1 << i) and raw:
            import lz4.block   # 出包 Python 依赖（launchers/macos/requirements-runtime.txt）
            try:
                raw = lz4.block.decompress(raw, uncompressed_size=size)
            except lz4.block.LZ4BlockError:
                raise NsoDamaged(os.path.basename(path), seg) from None
        if fl & (8 << i) and mismatch and hashlib.sha256(raw).digest() != d[0xA0 + i * 32:0xC0 + i * 32]:
            mismatch(os.path.basename(path), seg)
        out.append((seg, raw))
    return out


class NsoDamaged(ValueError):
    def __init__(self, name, seg):
        self.name, self.seg = name, seg
        super().__init__(f'程序文件 {name} 的 {seg} 段解不开：{DAMAGED}')


def nso_image(path):
    """NSO 三段解开后拼起来（只为找符号名，不按内存布局摆）。ZBIC 已在上面交 nx_unpack 解成不压缩"""
    with open(path, 'rb') as f:
        if f.read(4) != b'NSO0' or os.path.getsize(path) < 0x100:
            return open(path, 'rb').read()
    return b''.join(raw for _, raw in nso_segments(path))


def check_exefs(exefs):
    """导入 / 整理时先把 exefs 里每个 NSO 试解一遍（散文件没有整包校验，坏了要到出包才发现，#1522）：解不开抛 NsoDamaged；
    → 段哈希对不上的 [(文件名, 段名)…]（照常收，调用方记黄）。ZBIC（标志 0x80）交 nx_unpack 解时自己会报，这里跳过"""
    off = []
    for f in sorted(os.listdir(exefs)):
        p = os.path.join(exefs, f)
        if not os.path.isfile(p) or os.path.getsize(p) < 0x100:
            continue
        with open(p, 'rb') as h:
            hdr = h.read(0x10)
        if hdr[:4] == b'NSO0' and not struct.unpack_from('<I', hdr, 0x0C)[0] & 0x80:
            nso_segments(p, lambda n, s: off.append((n, s)))
    return off


def save_kinds(exefs):
    """exefs 里游戏代码（main、subsdk*）用到的存档类别 → [(偏移, 值)…] 与说明列表。sdk / rtld 是系统库，自己定义全部 nn::fs 函数（名字都在它的导出表里），不算"""
    imgs = [nso_image(os.path.join(exefs, f)) for f in sorted(os.listdir(exefs)) if f == 'main' or f.startswith('subsdk')]
    fields, notes = [], []
    for sym, fs, note in SAVE_KINDS:
        if any(sym in b for b in imgs):
            fields += fs; notes.append(note)
    return fields, notes


def synth_nacp(tid, name, saves=()):
    n = bytearray(NACP_SIZE)
    t = name.encode('utf-8')[:0x1ff]
    for i in range(16):   # ApplicationTitle[16]：名字 0x200 + 发行商 0x100
        n[i * 0x300:i * 0x300 + len(t)] = t
    n[0x3025] = 1                                   # StartupUserAccount = Required
    # SupportedLanguageFlag：不知道游戏支持哪些语言时只声明美式英语（位 0）。留 0 = 「一种都不支持」，服务层把系统语言原样交给游戏，
    #   游戏不认（#401：系统简体中文 → 游戏判无效语言、去读不存在的语言库、一直黑屏）；声明了英语，系统语言不在表里时就退到英语。
    #   不按资源目录猜语言：各款语言资源的组织方式不同，没有通用判据
    struct.pack_into('<I', n, 0x302C, 1)
    n[0x3060:0x3061] = b'?'                         # DisplayVersion 未知
    struct.pack_into('<Q', n, 0x3038, tid)          # PresenceGroupId = 程序号（常规 NACP 的应用号；安卓出包 / 装机 / 启动都按 0x3038 认游戏，留 0 则出包末步停在「has no application id」）
    struct.pack_into('<Q', n, 0x3078, tid)          # SaveDataOwnerId = 程序号（错了开存档 2002-6400，11-03 火纹）
    struct.pack_into('<q', n, 0x3080, 0x08000000)   # UserAccountSaveDataSize 128 MB
    struct.pack_into('<q', n, 0x3088, 0x04000000)   # UserAccountSaveDataJournalSize 64 MB
    for off, v in saves:                            # 游戏代码挂的其余存档类别（save_kinds）
        struct.pack_into('<q', n, off, v)
    return bytes(n)


def fill_fixed(nacp, tid, saves=()):
    """原件的固定段（0x3000 起：启动账号、语言位、存档归属号与各存档尺寸）全零时补上存档字段 → (新内容, 是否补了)。
    某款 Switch 2 游戏的更新包元数据里带的控制数据只有标题和发行商、0x3000 往后全 0：原样用时宿主不建账号存档，游戏开存档 2002-1002 自己中止、起局黑屏。
    补法同 synth_nacp 的存档部分（启动账号 Required、应用号与归属号 = 程序号、账号存档 128 / 64 MB、游戏代码挂的其余存档类别）；
    语言位保持 0：Switch 2 游戏支持表未知时服务层按宿主语言（#465），不写死英语。固定段不是全零（真原件）一字不动"""
    if len(nacp) < NACP_SIZE or any(nacp[0x3000:NACP_SIZE]):
        return nacp, False
    n = bytearray(nacp)
    n[0x3025] = 1
    struct.pack_into('<Q', n, 0x3038, tid)   # 应用号（同 synth_nacp 的说明）
    struct.pack_into('<Q', n, 0x3078, tid)
    struct.pack_into('<q', n, 0x3080, 0x08000000)
    struct.pack_into('<q', n, 0x3088, 0x04000000)
    for off, v in saves:
        struct.pack_into('<q', n, off, v)
    return bytes(n), True


def image_mode():   # 镜像形态开关（同 nx_unpack / ns2_nup）：romfs 不落散文件、直接出 romfs.std；缺省开，S2A_ROMFS_IMAGE=0 回旧路径
    return os.environ.get('S2A_ROMFS_IMAGE') != '0'


def romfs_of(verdir):
    """版本目录的 romfs：相对路径 → 来源。导出目录里是散文件（来源 = 路径）；镜像形态展开的 NUP 只有清单 romfs.nupmap（pack/ns2_nup.py write_nupmap），
    来源 = (字节数, 容器, 偏移, 长度, 编码, SHA-256)。"""
    m = os.path.join(verdir, 'romfs.nupmap')
    if not os.path.isfile(m):
        return walk(os.path.join(verdir, 'romfs'))
    out = {}
    with open(m, encoding='utf-8', newline='\n') as f:
        for line in f:
            rel, size, src, off, ln, codec, sha = line.rstrip('\n').split('\t')
            out[rel.replace('/', os.sep)] = (int(size), src, int(off), int(ln), codec, sha)
    return out


def fsize(v):
    return v[0] if isinstance(v, tuple) else os.path.getsize(v)


def write_image(rom, out):
    """镜像形态：rom（相对路径 → 来源）交 nx_unpack --romfs-image 建 out（标准 RomFS，与先克隆成 romfs/ 再 services/romfs_index.sh --std 逐字节相同）"""
    nxu = os.environ.get('S2A_NX_UNPACK')
    if not nxu or not os.path.isfile(nxu):
        red('镜像形态（缺省，S2A_ROMFS_IMAGE=0 关）由 nx_unpack 建 romfs.std：设 S2A_NX_UNPACK=<nx_unpack 程序>（一键出包会给）')
    lst = out + '.list'
    with open(lst, 'w', encoding='utf-8', newline='\n') as f:
        for rel, a in rom.items():
            r = rel.replace(os.sep, '/')
            if '\t' in r or '\n' in r or '\r' in r:
                red(f'romfs 文件名带制表 / 换行，写不进镜像清单：{rel!r}')
            if isinstance(a, tuple):
                f.write('\t'.join([r, str(a[0]), a[1], str(a[2]), str(a[3]), a[4], a[5]]) + '\n')
            else:
                n = os.path.getsize(a)
                f.write(f'{r}\t{n}\t{os.path.abspath(a)}\t0\t{n}\tfile\t-\n')
    # 标准输出（进度行、黄字）透传进出包日志，只收标准错误做报红原因（#1612：原先整段收住，玩家日志在这一步一行没有，4 小时分不清在跑还是卡住）
    sys.stdout.flush()
    try:
        r = subprocess.run([nxu, '--romfs-image', lst, out], stderr=subprocess.PIPE, text=True)
    finally:
        os.unlink(lst)
    if r.returncode:
        red(f'建 romfs.std 失败（nx_unpack 退出 {r.returncode}）：{(r.stderr or "").strip()[-300:]}')


def same_file(rb, ru, limit=64):
    """全量 / 差量判法：导出工具挂的是打过补丁的文件系统 = 全量，里面一定有没改过、与本体逐字节相同的文件；
    差量包只装改过 / 新增的，没有这种文件。只看同路径同大小的前 limit 个，找到一个就返回它的路径，找不到返回 None。
    （不能按「本体路径全在更新里」判：补丁会删掉旧版本文件，11-04 金刚 41 个、raiders 86 个 Product.100 旧文件只在本体里。）"""
    n = 0
    for rel in sorted(set(rb) & set(ru)):
        if fsize(rb[rel]) != fsize(ru[rel]):
            continue
        if isinstance(rb[rel], tuple) or isinstance(ru[rel], tuple):   # 镜像形态的 NUP 清单：同大小同 SHA-256 即逐字节相同（写镜像时按 SHA-256 核过数据）
            if isinstance(rb[rel], tuple) and isinstance(ru[rel], tuple) and rb[rel][5] == ru[rel][5]:
                return rel
            n += 1
            if n >= limit:
                break
            continue
        with open(rb[rel], 'rb') as a, open(ru[rel], 'rb') as b:
            while True:
                x, y = a.read(1 << 20), b.read(1 << 20)
                if x != y:
                    break
                if not x:
                    return rel
        n += 1
        if n >= limit:
            break
    return None


CONTENT_DIRS = ('romfs', 'exefs')   # 版本目录里的游戏内容分区：找控制分区时不进


def aoc_base(nacp, tid):   # DLC 号段基准号：NACP 0x3070 的 AddOnContentBaseId，为 0 / 没有控制分区按应用号 + 0x1000（同 ns2_nup.aoc_base 与运行时；本脚本可被单独按路径加载，不 import ns2_nup）
    b = struct.unpack_from('<Q', nacp, 0x3070)[0] if nacp and len(nacp) >= 0x3078 else 0
    return b or (tid & ~0xF) + 0x1000


def find_control(dirs):
    """版本目录（先最新更新、后本体）里 romfs 之外、两层以内名为 control.nacp 的文件（0x4000 字节）→ (内容, [icon_*.dat…])；都没有 → None。
    导出工具若带了控制分区，这里就是游戏自带的标题 / 显示版本 / 图标；11-04 金刚、raiders 的导出都没带。"""
    for d in dirs:   # 只看版本目录本层与下一层（不进 romfs / exefs），不递归：RomFS 遍历只走 romfs_list.py
        for dp in [d] + [os.path.join(d, n) for n in sorted(os.listdir(d)) if n not in CONTENT_DIRS and os.path.isdir(os.path.join(d, n))]:
            c = os.path.join(dp, 'control.nacp')
            if os.path.isfile(c) and os.path.getsize(c) == NACP_SIZE:
                return open(c, 'rb').read(), sorted(os.path.join(dp, f) for f in os.listdir(dp) if f.startswith('icon_') and f.endswith('.dat'))
    return None


def nacp_title(n, lang):   # ApplicationTitle[lang] 的名字（0x200 字节 UTF-8），空返回 None；语言序号同 nx_unpack（0 美式英语，13 繁中，14 简中）
    t = n[lang * 0x300:lang * 0x300 + 0x200].split(b'\0')[0].decode('utf-8', 'replace')
    return t or None


def src_key(dump, base=None):
    h = hashlib.sha256()
    files = dict(walk(dump))
    if base:   # 本体在别处（--base，散文件解包目录不链接进来）：按 base/<相对路径> 计入，同链接进来时一样
        files.update({os.path.join('base', rel): a for rel, a in walk(base).items()})
    for rel, a in sorted(files.items()):
        st = os.stat(a)
        h.update(f'{rel}\t{st.st_size}\t{st.st_mtime_ns}\n'.encode())   # 纳秒（审计 P1-54：秒级键同一秒内换同大小的文件认不出）
    h.update(hashlib.sha256(open(os.path.abspath(__file__), 'rb').read()).digest())
    if image_mode():   # 两种形态出的东西不同（romfs/ 或 romfs.std），不能互相复用；缺省键不变
        h.update(b'romfs-image')
    nxu = os.environ.get('S2A_NX_UNPACK')   # ZBIC 由它解：换了解码器（自写替换 ZstdSharp.Zbic）就重做
    if nxu and os.path.isfile(nxu):
        h.update(hashlib.sha256(open(nxu, 'rb').read()).digest())
    return h.hexdigest()[:16]


def convert(dump, out, name=None, base=None, consume=False):
    base = base or os.path.join(dump, 'base')
    try:
        tid = read_tid(os.path.join(base, 'exefs', 'main.npdm'))
    except (OSError, ValueError, struct.error) as e:
        red(f'读不出程序号：{e}')
    ups = numdirs(os.path.join(dump, 'updates'))
    upd = os.path.join(dump, 'updates', ups[-1]) if ups else None
    if len(ups) > 1:
        print(f'🟡 有 {len(ups)} 个更新（{", ".join(ups)}），取最新 {ups[-1]}')
    key = src_key(dump, base if base != os.path.join(dump, 'base') else None)
    kf = os.path.join(out, '.ns2_dump_key')
    if os.path.isfile(kf) and open(kf).read().strip() == key:
        print(f'🟢 导出目录没变（键 {key}），复用 {out}')
        return 0
    tmp = out + '.tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)

    # exefs
    exsrc = os.path.join(upd, 'exefs') if upd and os.path.isfile(os.path.join(upd, 'exefs', 'main')) else os.path.join(base, 'exefs')
    try:
        builds = normalize_exefs(exsrc, os.path.join(tmp, 'exefs'), tid)
    except (OSError, ValueError, RuntimeError, struct.error) as e:
        shutil.rmtree(tmp, ignore_errors=True)   # 程序文件坏了等：不留半成品
        red(str(e))

    # romfs
    rb = romfs_of(base)
    ru = romfs_of(upd) if upd else {}
    same = same_file(rb, ru)
    if ru and not rb:
        rom, how = ru, f'更新 {ups[-1]}（本体没带 romfs）'
    elif ru and same:
        rom, how = ru, f'更新 {ups[-1]} 的全量（与本体逐字节相同的文件如 {same}；本体独有 {len(set(rb) - set(ru))} 个旧版本文件不收）'
    elif ru:
        rom = dict(rb)
        rom.update(ru)
        how = f'本体 {len(rb)} + 更新 {len(ru)} 叠加（更新里没有与本体逐字节相同的文件，按差量处理）'
        print(f'🟡 romfs：{how}')
    else:
        rom, how = rb, f'本体（无更新）'
    if not rom:
        red(f'{dump} 没有 romfs 文件')
    low = {}
    for rel in rom:
        k = rel.lower()
        if k in low:
            red(f'romfs 里有只差大小写的路径：{low[k]} / {rel}（文件系统不分大小写）')
        low[k] = rel
    nb = sum(fsize(a) for a in rom.values())
    if image_mode():
        write_image(rom, os.path.join(tmp, 'romfs.std'))
    elif any(isinstance(a, tuple) for a in rom.values()):
        red('这份 NUP 是按镜像形态展开的（只有 romfs.nupmap），整理也要镜像形态：别设 S2A_ROMFS_IMAGE=0')
    else:
        for rel, a in rom.items():
            take(a, os.path.join(tmp, 'romfs', rel), consume)

    # DLC：序号 = DLC 标题号 − 控制分区里的 AddOnContentBaseId（ns2_nup.aoc_base，运行时同口径；没带控制分区按应用号 + 0x1000）
    real = find_control([upd, base] if upd else [base])
    first = aoc_base(real[0] if real else None, tid)
    aoc_rows, aoc_idx = [], []
    dlc = os.path.join(dump, 'dlc')
    for d in sorted(os.listdir(dlc)) if os.path.isdir(dlc) else []:
        try:
            atid = int(d, 16)
        except ValueError:
            continue
        idx = atid - first
        if not 0 < idx < 0x1000:
            print(f'🟡 DLC {d} 不属于程序号 {tid:016x}，跳过')
            continue
        vs = [v for v in numdirs(os.path.join(dlc, d)) if os.path.isdir(os.path.join(dlc, d, v, 'romfs'))]
        rd = os.path.join(dlc, d, vs[-1], 'romfs') if vs else os.path.join(dlc, d, 'romfs')   # 也认 dlc/<标题号>/romfs（中间没有 <n>）
        if not os.path.isdir(rd):
            print(f'🟡 DLC {d} 没有 romfs（只有授权？），记仅授权')
            aoc_rows.append(f'{idx}\t{atid:016x}\t0\t-1\t0')
            continue
        fs = walk(rd)
        nbytes = sum(os.path.getsize(a) for a in fs.values())   # 挪之前量（--consume 挪走后来源就不在了）
        for rel, a in fs.items():
            take(a, os.path.join(tmp, 'aoc', str(idx), rel), consume)
        aoc_idx.append(idx)
        aoc_rows.append(f'{idx}\t{atid:016x}\t{vs[-1] if vs else 0}\t{len(fs)}\t{nbytes}')
    with open(os.path.join(tmp, 'aoc.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('idx\ttitle_id\tversion\tfiles\tbytes\n' + ''.join(r + '\n' for r in aoc_rows))

    os.makedirs(os.path.join(tmp, 'ctl'))
    if real:   # 导出带了控制分区：用原件，标题 / 显示版本 / 图标都是游戏自带的
        nacp, icons = real
        nacp, filled = fill_fixed(nacp, tid, save_kinds(os.path.join(tmp, 'exefs'))[0])
        with open(os.path.join(tmp, 'ctl', 'control.nacp'), 'wb') as f:
            f.write(nacp)
        if icons:
            clone(icons[0], os.path.join(tmp, 'icon.jpg'))
        nm, nzh, dver = nacp_title(nacp, 0) or '', nacp_title(nacp, 14) or nacp_title(nacp, 13) or '', nacp[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace')
        nm = nm or nzh or name or os.path.basename(os.path.normpath(dump))
        ctl_note = '控制分区用原件' + ('（原件固定段全零，补了存档字段）' if filled else '')
    else:   # 没带：合成。README.synth 只标明来源（标题取导出目录名；维护者 11-04：导不出控制分区就用文件夹名，取名函数照读）
        nm, nzh, dver = name or os.path.basename(os.path.normpath(dump)), '', ''
        saves, snotes = save_kinds(os.path.join(tmp, 'exefs'))
        with open(os.path.join(tmp, 'ctl', 'control.nacp'), 'wb') as f:
            f.write(synth_nacp(tid, nm, saves))
        with open(os.path.join(tmp, 'ctl', 'README.synth'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(f'# pack/ns2_dump.py 合成（Switch 2 导出目录没有控制分区）：非游戏原件。程序号 {tid:016x}，标题「{nm}」取自导出目录名，不是游戏自带信息\n')
        ctl_note = 'control.nacp 合成（导出没带控制分区；另建 ' + ('、'.join(snotes) or '无') + '）'
    with open(os.path.join(tmp, 'unpack.tsv'), 'w', encoding='utf-8', newline='\n') as f:   # Windows 上缺省写 CRLF：Mac 读回程序号带 \r（11-05 金刚Windows 出包机产物根拉回 Mac，direct_lib 找不到配置档）
        f.write(f'title_id\t{tid:016x}\nname\t{nm}\nname_zh\t{nzh}\ndisplay_version\t{dver}\n'
                f'update\t{ups[-1] if ups else ""}\nbase\t0\nbuilds\t{" ".join(builds)}\naoc\t{",".join(map(str, aoc_idx))}\n')
    with open(os.path.join(tmp, '.ns2_dump_key'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(key + '\n')
    shutil.rmtree(out, ignore_errors=True)
    from orch.dirrename import rename_dir   # 刚写完整份解包，Windows 上杀软 / 索引扫描开着句柄时目录改名报 WinError 5：按 #361 同一套退避重试
    rename_dir(tmp, out)
    print(f'🟢 Switch 2 导出目录 → {out}：程序号 {tid:016x}；exefs {" ".join(b.split(":")[0] for b in builds)}；'
          f'romfs {len(rom)} 个文件 {nb >> 20} MB（{how}）；DLC {len(aoc_idx)} 个；{ctl_note}；标题「{nm}」')
    return 0


def main(a):
    if len(a) == 2 and a[0] == '--detect':
        d = find_dump(a[1]) if os.path.isdir(a[1]) else None
        if not d:
            return 1
        print(f'{read_tid(os.path.join(d, "base", "exefs", "main.npdm")):016X}\t{d}')
        return 0
    name = base = None
    consume = '--consume' in a
    a = [x for x in a if x != '--consume']
    if '--base' in a:   # 本体目录（exefs/ + romfs/）在别处：作业层导入的散文件解包目录直接交进来，不再链接成 <导出目录>/base（玩家诊断 #0fd185：链接可能建不了）
        i = a.index('--base')
        base = os.path.abspath(a[i + 1])
        a = a[:i] + a[i + 2:]
    if '--name' in a:
        i = a.index('--name')
        name = a[i + 1]
        a = a[:i] + a[i + 2:]
    if len(a) != 2:
        print(__doc__.split('用法：')[1], file=sys.stderr)
        return 2
    if base:
        if not os.path.isfile(os.path.join(base, 'exefs', 'main.npdm')):
            red(f'{base} 不是本体目录（要有 exefs/main.npdm）')
        if not os.path.isdir(a[0]):
            red(f'{a[0]} 不是目录')
        return convert(os.path.abspath(a[0]), os.path.abspath(a[1]), name, base, consume)
    d = find_dump(a[0]) if os.path.isdir(a[0]) else None
    if not d:
        red(f'{a[0]} 不是 Switch 2 导出目录（要有 base/exefs/main.npdm）')
    return convert(d, os.path.abspath(a[1]), name, consume=consume)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
