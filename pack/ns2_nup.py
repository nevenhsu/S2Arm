#!/usr/bin/env python3
"""Switch 2 的 NUP 容器（.nup，一个文件装本体 + 更新 + DLC）→ 导出目录布局，再交 pack/ns2_dump.py（pack/ns2_nup.py）。

NUP 是公开的打包格式（https://github.com/a55uka/nup ，SPEC.md v1）：超级块 + 目录表（TOC，带 SHA-256）+ 数据区；
每个包（本体 / 更新 / DLC）是一棵 exefs/ 与 romfs/ 树，文件原样或单帧 zstd 存，每个文件带 SHA-256，不加密、不要密钥；
包的元数据块里可能带控制分区（NACP、图标）。11-04 维护者 给的塞尔达 时之笛（0400486024DEC000 v131072）就是这个格式。

本脚本只做一件事：把容器展开成 pack/ns2_dump.py 已经认的导出目录布局
    <输出>/base/exefs|romfs/...                     本体（容器里没有本体或本体无文件时不写，见下）
    <输出>/updates/<版本号>/exefs|romfs/...         只写最新那一版更新（ns2_dump 只用最新更新 + 本体判全量 / 差量）
    <输出>/updates/<版本号>/control.nacp、icon_AmericanEnglish.dat   该包带了控制分区才写（ns2_dump 认作原件；没带就由它按目录名合成）
    <输出>/dlc/<DLC 标题号>/0/romfs/...             每个 DLC
镜像形态（缺省，S2A_ROMFS_IMAGE=0 回散文件）：本体 / 更新的 romfs 不展开成散文件，改写一份清单 <版本目录>/romfs.nupmap（每个文件在容器里的位置、编码、SHA-256，
格式即 nx_unpack --romfs-image 的清单），由 ns2_dump 选好全量 / 差量后交 nx_unpack 直接从容器建 romfs.std；DLC 照旧展开。
后面的程序号判定、全量 / 差量、控制分区、unpack.tsv 都由 ns2_dump 照原样做，这里不重复。
没有本体的容器（SPEC §3.1.2 的 standalone_update，或本体记录没有文件）：把最新更新同时写成 base/，即把更新当整款
（容器里证明不了更新是全量，所以打黄字说明，同 the reference port）。
每个文件读出时核大小与 SHA-256：大小不对报红，只是 SHA-256 不对打黄照常写出（维护者 11-17）。<输出>/.ns2_nup_key = 目录表 SHA-256 + 本脚本内容的哈希，相同就复用不重做；
先写 <输出>.tmp，成功才改名。

容器读取（超级块 / 目录表 / 包记录 / 文件记录的布局与校验、scan 的检查）借自 the reference port scripts/pack_nup.py
（seedds/the reference port 777a5f6c，MIT），改为写导出目录布局、zstd 解压另认 zstandard（Windows 侧 Python 3.12 没有 compression.zstd）。

用法：
    ns2_nup.py --detect <路径>              是 .nup（或文件夹里恰有一个 .nup）就打印那个文件，退出 0；否则退出 1
    ns2_nup.py --inspect <路径>             同上认出的 .nup 打一行「路径<TAB>字节<TAB>修改时间<TAB>程序号」（同 pack/srcs.sh src_ids 的行）
    ns2_nup.py <x.nup | 含 .nup 的文件夹> <输出导出目录>
    ns2_nup.py --to-extract <x.nup | 含 .nup 的文件夹> <解包目录> [--name <款名>]
                                            NUP → 解包目录一步到位（与 nx_unpack 解包同形：exefs/ romfs/ ctl/ aoc/ unpack.tsv）：展开到 <解包目录>.ns2_dump，
                                            交 pack/ns2_dump.py 整理（S2A_NX_UNPACK 照传给它解 ZBIC），成功失败都删中间目录。
                                            --name 缺省 = 文件名去掉「 [程序号][版本]」（没有控制分区时 ns2_dump 拿它当标题）。
                                            启动器作业层（pack/jobs/build.py）与共享解包阶段（pack/stages/unpack.sh）都走这一个入口
"""
import hashlib
import mmap
import os
import shutil
import struct
import sys

try:
    from compression import zstd as _zstd   # Python 3.14+
except ImportError:
    try:
        from backports import zstd as _zstd   # pyright: ignore[reportMissingImports]
    except ImportError:
        _zstd = None
_zstandard = None
if _zstd is None:
    try:
        import zstandard as _zstandard   # pyright: ignore[reportMissingImports]
    except ImportError:
        _zstandard = None


class Error(Exception):
    pass


SUPER = struct.Struct('<4sHHQQ32sQQII16s32s')   # 0x80
TOC = struct.Struct('<4sIQQQQQ')   # 0x30
PKG = struct.Struct('<QIIIIBBHIQI20x')   # 0x40
DIR = struct.Struct('<IIIIQ')   # 0x18
FILE = struct.Struct('<IIQQQ32sBBH12x')   # 0x50
TLV = struct.Struct('<HHI')
NO_DIR = 0xFFFFFFFF
BASE, UPDATE, ADDON = 0, 1, 2
KINDS = ('base', 'update', 'addon', 'addon_update', 'system')
STANDALONE = 1   # 包标志 bit 0：没有本体的容器里的更新
T_NACP, T_ICON, T_LICONS = 2, 3, 10
ARTIFACTS = {b'.listing'}   # 导出工具留下的文件（SPEC §6.3），不是游戏文件
NO_BASE = '容器里没有本体：最新更新当整款写成 base/（容器证明不了更新是全量；游戏若缺文件停住，说明更新只含改动，要连本体一起打包）'


def _decompressor():
    if _zstd is not None:
        return _zstd.ZstdDecompressor(), _zstd.ZstdError
    if _zstandard is not None:
        return _zstandard.ZstdDecompressor().decompressobj(), _zstandard.ZstdError
    raise Error('读 NUP 要 zstd 模块：compression.zstd（Python 3.14+）、pip install backports.zstd 或 zstandard')


class Package:
    def __init__(self, nup, index, rec):
        self.nup, self.index = nup, index
        (self.title_id, self.root, self.exefs, self.romfs, self.version, self.kind, self.flags,
         r1, r2, self.meta_off, self.meta_size) = rec
        if self.kind >= len(KINDS):
            raise Error(f'{nup.path}：第 {index} 个包类型未知（{self.kind}）')
        if r1 or r2 or self.flags & ~STANDALONE:
            raise Error(f'{nup.path}：第 {index} 个包保留字段或标志非 0')

    def meta(self):   # {TLV 类型: 内容}
        out = {}
        b = self.nup.mm[self.meta_off:self.meta_off + self.meta_size] if self.meta_size else b''
        p = 0
        while p + TLV.size <= len(b):
            t, _, n = TLV.unpack_from(b, p)
            p += TLV.size
            if p + n > len(b):
                raise Error(f'{self.nup.path}：第 {self.index} 个包的元数据 {t:#x} 越出块尾')
            out.setdefault(t, bytes(b[p:p + n]))
            p += n
        return out


class Nup:
    """校验过的 NUP（SPEC §6.1）：超级块、目录表 SHA-256、每条记录的边界；文件内容读出时再核 SHA-256。"""

    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            try:
                self.mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            except ValueError:
                raise Error(f'{path}：空文件') from None
        try:
            self._read()
        except BaseException:
            self.mm.close()
            raise

    def _read(self):
        if len(self.mm) < SUPER.size:
            raise Error(f'{self.path}：不是 NUP（太短）')
        magic, hsize, _, toc_off, toc_size, toc_sha, self.blob, fsize, npkg, flags, _uuid, _ = SUPER.unpack_from(self.mm)
        if magic[:3] != b'NUP':
            raise Error(f'{self.path}：不是 NUP')
        if magic[3] != 1:
            raise Error(f'{self.path}：NUP 格式版本 {magic[3]}，本脚本只读版本 1')
        if hsize < SUPER.size or toc_off != hsize or flags:
            raise Error(f'{self.path}：不支持的 NUP 头')
        if fsize != len(self.mm):
            raise Error(f'{self.path}：文件 {len(self.mm):,} 字节，头里写 {fsize:,}（没拷完 / 没下完？）')
        if self.blob < toc_off + toc_size or self.blob % 16 or self.blob > fsize:
            raise Error(f'{self.path}：目录表与数据区重叠')
        toc = self.mm[toc_off:toc_off + toc_size]
        self.toc_sha = toc_sha
        if hashlib.sha256(toc).digest() != toc_sha:   # 维护者 11-17：只打黄照常读；目录表真坏时下面的逐条边界检查会按原因报
            print(f'🟡 {os.path.basename(self.path)}：目录表和它的 SHA-256 对不上（疑改版 / 整合版或损坏），照常读', flush=True)
            print('【校验跳过】NUP 目录表\t\t', flush=True)  # 非玩家句
        tmagic, tpkg, self.ndirs, nfiles, pool_off, pool_size, _ = TOC.unpack_from(toc)
        need = TOC.size + tpkg * PKG.size + self.ndirs * DIR.size + nfiles * FILE.size
        if (tmagic != b'TOC\x01' or tpkg != npkg or not npkg or len(toc) < need or pool_off < need
                or pool_off + pool_size > len(toc) or (pool_off - need) % 4):
            raise Error(f'{self.path}：目录表格式不对')
        pool = toc[pool_off:pool_off + pool_size]

        def name(off, n):
            if off + n > len(pool):
                raise Error(f'{self.path}：名字越界')
            return pool[off:off + n]
        p = TOC.size
        self.packages = [Package(self, i, PKG.unpack_from(toc, p + i * PKG.size)) for i in range(npkg)]
        p += npkg * PKG.size
        self.dirs = []   # (名字, 第一个子项槽, 子项数)
        for _ in range(self.ndirs):
            o, n, first, count, _ = DIR.unpack_from(toc, p)
            p += DIR.size
            self.dirs.append((name(o, n), first, count))
        self.files = []   # (名字, 大小, sha256, 数据偏移, 数据长度, 编码)
        for _ in range(nfiles):
            o, n, size, eoff, esz, sha, codec, cflags, _ = FILE.unpack_from(toc, p)
            p += FILE.size
            if codec not in (0, 1) or cflags:
                raise Error(f'{self.path}：文件 {name(o, n)!r} 用编码 {codec}，本脚本只读原样与 zstd')
            if size and (eoff < self.blob or eoff + esz > fsize or eoff % 16 or not esz or codec == 0 and esz != size):
                raise Error(f'{self.path}：文件 {name(o, n)!r} 越界')
            if not size and (eoff or esz or codec):
                raise Error(f'{self.path}：空文件 {name(o, n)!r} 却有数据')
            self.files.append((name(o, n), size, sha, eoff, esz, codec))
        self.slots = struct.unpack_from(f'<{(pool_off - p) // 4}I', toc, p)
        for n, first, count in self.dirs:
            if first + count > len(self.slots):
                raise Error(f'{self.path}：目录 {n!r} 越界')
        for s in self.slots:
            if s >= self.ndirs + nfiles:
                raise Error(f'{self.path}：子项 {s} 越界')
        for k in self.packages:
            if (k.root >= self.ndirs or any(d != NO_DIR and d >= self.ndirs for d in (k.exefs, k.romfs))
                    or k.meta_size and k.meta_off + k.meta_size > fsize):
                raise Error(f'{self.path}：第 {k.index} 个包越界')

    def close(self):
        self.mm.close()

    def __enter__(self):
        return self

    def __exit__(self, et, ev, tb):
        # with 块里已经出错时，关映射失败不能盖掉原异常（11-06 Windows 出包机0400486024DEC000：并发出包把 T 盘写满，原样文件写到一半抛 OSError 28，
        #   退出时 mm.close() 因还有导出的视图抛 BufferError，日志只剩 BufferError）：原异常照抛，附一句关映射失败的说明
        try:
            self.close()
        except BufferError as e:
            if ev is None:
                raise
            ev.add_note(f'（随后关闭原件映射也失败：{e}；以上面的原异常为准）')

    def children(self, d):   # [(名字, 是目录, 目录或文件下标)]，按名字排序，去掉导出工具留下的文件
        _, first, count = self.dirs[d]
        out = []
        for s in self.slots[first:first + count]:
            if s < self.ndirs:
                out.append((self.dirs[s][0], True, s))
            elif self.files[s - self.ndirs][0] not in ARTIFACTS:
                out.append((self.files[s - self.ndirs][0], False, s - self.ndirs))
        return sorted(out)

    def child(self, d, name):
        return next((i for n, isdir, i in self.children(d) if n == name and not isdir), None)

    def tree_bytes(self, d):
        return sum(self.files[i][1] if not isdir else self.tree_bytes(i) for _, isdir, i in self.children(d))

    def copy(self, i, out):
        """第 i 个文件的内容流式写到 out（有 write 的对象），核大小与 SHA-256；返回字节数。"""
        name, size, sha, eoff, esz, codec = self.files[i]
        h, n, view = hashlib.sha256(), 0, memoryview(self.mm)

        def put(b):
            nonlocal n
            h.update(b)
            out.write(b)
            n += len(b)
        try:
            if codec == 0:
                for p in range(eoff, eoff + esz, 8 << 20):
                    s = view[p:min(p + (8 << 20), eoff + esz)]
                    try:
                        put(s)
                    finally:
                        s.release()   # 写出失败时这块切片还挂在异常回溯的栈帧里，不放掉 mmap 就关不上（BufferError）
            elif size:
                d, zerr = _decompressor()
                try:
                    for p in range(eoff, eoff + esz, 4 << 20):
                        put(d.decompress(view[p:min(p + (4 << 20), eoff + esz)]))
                        if n > size:
                            break
                except zerr as e:
                    raise Error(f'{self.path}：{name.decode(errors="replace")}：{e}') from e
                if not getattr(d, 'eof', True):
                    n = -1
        finally:
            view.release()
        if n != size:   # 大小不对：读出的东西放不进清单给的位置，物理上用不了，照旧报红
            raise Error(f'{self.path}：{name.decode(errors="replace")} 大小对不上（文件损坏）')
        if h.digest() != sha:   # 只是哈希不对（维护者 11-17：「校验不过就整个拒绝」一律撤掉）：打黄 + 【校验跳过】机读行，照常写出
            print(f'🟡 {os.path.basename(self.path)}：{name.decode(errors="replace")} 的 SHA-256 和目录表对不上（疑改版 / 整合版或损坏），照常写出', flush=True)
            print(f'【校验跳过】NUP 容器\t\t文件 {name.decode(errors="replace")}', flush=True)  # 非玩家句
        return n

    def read(self, i):
        class Buf(bytearray):
            write = bytearray.extend
        b = Buf()
        self.copy(i, b)
        return bytes(b)


def aoc_base(nacp, app):
    """DLC 号段的基准号：控制分区（NACP，0x4000 字节）0x3070 的 AddOnContentBaseId，DLC 标题号 = 基准号 + 序号（1…2000）。
    运行时同一个口径（Ryujinx IAddOnContentManager.GetAddOnContentBaseIdFromTitleId：读 NACP 的这个字段，为 0 才退回程序号 + 0x1000）。
    D-533D（mac 0.6.8）：Switch 2 版程序 0400E95004038001 的 DLC 是 Switch 1 本体号段的 0100E95004039001——Switch 2 版和 Switch 1 本体共用一套 DLC，
    NACP 里写的就是 Switch 1 的号段。没带控制分区或字段为 0：按应用号（程序号去掉末位，#1567）+ 0x1000"""
    b = struct.unpack_from('<Q', nacp, 0x3070)[0] if nacp and len(nacp) >= 0x3078 else 0
    return b or (app & ~0xF) + 0x1000


def program_id(npdm, where):   # NPDM 的 ACI0 程序号
    if npdm[:4] != b'META':
        raise Error(f'{where}：不是 NPDM')
    off, size = struct.unpack_from('<II', npdm, 0x70)
    if npdm[off:off + 4] != b'ACI0' or size < 0x18:
        raise Error(f'{where}：没有 ACI0 段')
    return struct.unpack_from('<Q', npdm, off + 0x10)[0]


def is_nup(path):
    try:
        with open(path, 'rb') as f:
            return f.read(3) == b'NUP'
    except OSError:
        return False


def find_nup(path):
    """路径本身是 .nup 就返回它；是文件夹且里面恰有一个 NUP 文件就返回那个；否则 None。"""
    if os.path.isfile(path):
        return path if is_nup(path) else None
    if not os.path.isdir(path):
        return None
    hits = [os.path.join(path, n) for n in sorted(os.listdir(path))
            if n.lower().endswith('.nup') and os.path.isfile(os.path.join(path, n)) and is_nup(os.path.join(path, n))]
    return hits[0] if len(hits) == 1 else None


def scan(n):
    """容器里有什么：(程序号, 本体包或 None, {更新版本: 包}, {DLC 标题号: 包})，检查同 the reference port pack_nup.scan。"""
    bases = [k for k in n.packages if k.kind == BASE]
    if len(bases) > 1:
        raise Error(f'{n.path}：{len(bases)} 个本体包')
    updates = [k for k in n.packages if k.kind == UPDATE]
    if not bases and (not updates or any(not k.flags & STANDALONE for k in updates)):
        raise Error(f'{n.path}：没有本体：要么把本体和更新打进同一个 NUP，要么更新带 standalone_update 且有完整 exefs/ 与 romfs/')
    base = bases[0] if bases and n.tree_bytes(bases[0].root) else None
    ids = {program_id(n.read(i), f'{k.kind} {k.version} exefs/main.npdm') for k in n.packages
           if k.kind in (BASE, UPDATE) and k.exefs != NO_DIR and (i := n.child(k.exefs, b'main.npdm')) is not None}
    if not ids:
        raise Error(f'{n.path}：本体和更新里都没有 exefs/main.npdm')
    if len(ids) != 1:
        raise Error(f'{n.path}：本体与更新程序号不同：' + '、'.join(f'{x:016X}' for x in sorted(ids)))
    app = ids.pop()
    if app & 0xFF0:   # 末位是多程序应用里的程序序号：单独导出的程序 n（如 Switch 2 版程序 1，#1567）照收，DLC 按应用号（去掉末位）认
        raise Error(f'{n.path}：程序号 {app:016X} 不是本体标题号')
    # DLC 归属按控制分区的 AddOnContentBaseId（aoc_base）：最新更新的控制分区优先（运行时用的就是它），没有再看本体
    ctl = [k for k in sorted(n.packages, key=lambda k: (k.kind != UPDATE, -k.version)) if k.kind in (BASE, UPDATE)]
    nacp = next((m for m in (k.meta().get(T_NACP) for k in ctl) if m and len(m) == 0x4000), None)
    first = aoc_base(nacp, app)
    vers, dlc = {}, {}
    for k in n.packages:
        if k.kind == UPDATE:
            if k.version in vers:
                raise Error(f'{n.path}：两个版本 {k.version} 的更新')
            if not k.version:
                raise Error(f'{n.path}：版本 0 的更新')
            vers[k.version] = k
        elif k.kind == ADDON:
            if not 0 < k.title_id - first <= 2000:
                raise Error(f'{n.path}：{k.title_id:016X} 不是 {app:016X} 的 DLC（DLC 号段从 {first + 1:016X} 起）')
            if k.romfs == NO_DIR:
                raise Error(f'{n.path}：DLC {k.title_id:016X} 没有 romfs/')
            if k.title_id in dlc:
                raise Error(f'{n.path}：DLC {k.title_id:016X} 出现多次')
            dlc[k.title_id] = k
    return app, base, vers, dlc


def safe_name(n, raw):
    name = os.fsdecode(raw)
    if name in ('', '.', '..') or '\0' in name or '/' in name or '\\' in name:
        raise Error(f'{n.path}：非法文件名 {raw!r}')
    return name


def write_nupmap(n, d, dst, stat):
    """镜像形态：容器里的 romfs 目录 d 不展开，写清单 dst（nx_unpack --romfs-image 格式：相对路径 字节数 容器 偏移 长度 编码 SHA-256）。
    数据不在这里读，SHA-256 由 nx_unpack 写镜像时核。"""
    rows = []

    def walk(d, pre):
        for raw, isdir, i in n.children(d):
            rel = pre + safe_name(n, raw)
            if isdir:
                walk(i, rel + '/')
                continue
            _, size, sha, eoff, esz, codec = n.files[i]
            if '\t' in rel or '\n' in rel or '\r' in rel:
                raise Error(f'{n.path}：文件名带制表 / 换行，写不进清单：{rel!r}')
            rows.append(f'{rel}\t{size}\t{os.path.abspath(n.path)}\t{eoff}\t{esz}\t{"zstd" if codec else "raw"}\t{sha.hex()}\n')
            stat[0] += 1
    walk(d, '')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(rows)


def write_tree(n, d, dst, stat, written=None):
    """容器里的目录 d 整棵写到 dst（逐文件核 SHA-256）。written（一次展开共用的 {(内容 SHA-256, 大小): 已写的路径}）：
    内容相同的文件只真写一次，其余从已写的那份克隆（同卷 APFS 不占新空间）/ 硬链接——NUP 容器按块去重，本体与整份打过补丁的更新
    大部分文件相同，原先各写一份，解包时盘上同时有近两份（26 GB 的款峰值到 50 GiB）"""
    os.makedirs(dst, exist_ok=True)
    for raw, isdir, i in n.children(d):
        p = os.path.join(dst, safe_name(n, raw))
        if isdir:
            write_tree(n, i, p, stat, written)
            continue
        _, size, sha, _, _, _ = n.files[i]
        same = written.get((sha, size)) if written is not None else None
        if same is not None:
            _share(same, p)
            stat[2] += size
        else:
            with open(p, 'wb') as f:
                stat[1] += n.copy(i, f)
            if written is not None:
                written[(sha, size)] = p
        stat[0] += 1


def _share(src, dst):
    """同内容的第二份：macOS 同卷克隆（不占新空间）→ 硬链接（这棵中间目录只给 pack/ns2_dump.py 读出、随后整棵删，不会有人原地改）→ 真拷"""
    if _clone_only(src, dst):
        return
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def localized_icon(payload):   # 本地化图标记录（TLV 0x000A）→ 英语图标，没有就第一个
    out, p = {}, 0
    while p < len(payload):
        ln = payload[p]
        tag = payload[p + 1:p + 1 + ln].decode(errors='replace').lower()
        p += 1 + ln
        if p + 4 > len(payload):
            break
        dn = struct.unpack_from('<I', payload, p)[0]
        p += 4
        out.setdefault(tag, payload[p:p + dn])
        p += dn
    return out.get('en-us') or out.get('en') or next(iter(out.values()), None)


def write_control(n, k, base, dst):
    """包带了控制分区就写 control.nacp + icon_AmericanEnglish.dat（ns2_dump 认作原件）；没带本体时从本体记录补。"""
    meta = {}
    if k.kind != BASE and base is None:
        holder = next((x for x in n.packages if x.kind == BASE), None)
        if holder is not None:
            meta.update(holder.meta())
    meta.update(k.meta())
    nacp = meta.get(T_NACP)
    icon = meta.get(T_ICON) or localized_icon(meta.get(T_LICONS, b''))
    if nacp and len(nacp) == 0x4000:
        with open(os.path.join(dst, 'control.nacp'), 'wb') as f:
            f.write(nacp)
        if icon:
            with open(os.path.join(dst, 'icon_AmericanEnglish.dat'), 'wb') as f:
                f.write(icon)
        return True
    return False


def image_mode():   # 镜像形态开关（同 nx_unpack / ns2_dump）：缺省开，S2A_ROMFS_IMAGE=0 回旧路径（展开散文件）
    return os.environ.get('S2A_ROMFS_IMAGE') != '0'


def key_of(n):
    me = open(os.path.abspath(__file__), 'rb').read()
    return hashlib.sha256(n.toc_sha + me + (b'romfs-image' if image_mode() else b'')).hexdigest()   # 两种形态展开的东西不同，不能互相复用


def convert(src, out, say=print):
    with Nup(src) as n:
        app, base, vers, dlc = scan(n)
        key = key_of(n)
        kf = os.path.join(out, '.ns2_nup_key')
        if os.path.isfile(kf) and open(kf).read().strip() == key:
            say(f'🟢 NUP 没变（键 {key[:16]}），复用 {out}')
            return app
        ver = max(vers, default=0)
        say(f'NUP {os.path.basename(src)}：程序号 {app:016X}，本体 {"有" if base else "无"}，更新 {sorted(vers) or "无"}，DLC {len(dlc)} 个；展开本体'
            + (f' + 最新更新 v{ver}' if ver else '') + (f' + {len(dlc)} 个 DLC' if dlc else ''))
        if base is None:
            say(f'🟡 {NO_BASE}')
        tmp = out + '.tmp'
        shutil.rmtree(tmp, ignore_errors=True)
        stat, written = [0, 0, 0], {}   # 文件数、真写字节、按内容共用省下的字节
        for k, rel in ([(base, 'base')] if base else []) + ([(vers[ver], os.path.join('updates', str(ver)))] if ver else []):
            if k.exefs == NO_DIR or n.child(k.exefs, b'main') is None or n.child(k.exefs, b'main.npdm') is None:
                raise Error(f'{k.kind} v{k.version}：没有 exefs/main 与 exefs/main.npdm')
            write_tree(n, k.exefs, os.path.join(tmp, rel, 'exefs'), stat, written)
            if k.romfs != NO_DIR and image_mode():
                write_nupmap(n, k.romfs, os.path.join(tmp, rel, 'romfs.nupmap'), stat)
            elif k.romfs != NO_DIR:
                write_tree(n, k.romfs, os.path.join(tmp, rel, 'romfs'), stat, written)
            if write_control(n, k, base, os.path.join(tmp, rel)):
                say(f'   {rel}：带控制分区（control.nacp + 图标），用原件')
        if base is None:   # 没有本体：最新更新当整款，同时写成 base/（ns2_dump 要 base/exefs/main.npdm 认目录）
            u = os.path.join(tmp, 'updates', str(ver))
            for part in ('exefs', 'romfs'):
                if os.path.isdir(os.path.join(u, part)):
                    shutil.copytree(os.path.join(u, part), os.path.join(tmp, 'base', part), copy_function=_clone)
            if os.path.isfile(os.path.join(u, 'romfs.nupmap')):
                shutil.copyfile(os.path.join(u, 'romfs.nupmap'), os.path.join(tmp, 'base', 'romfs.nupmap'))
        for tid, k in sorted(dlc.items()):
            write_tree(n, k.romfs, os.path.join(tmp, 'dlc', f'{tid:016X}', '0', 'romfs'), stat, written)
        with open(os.path.join(tmp, '.ns2_nup_key'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(key + '\n')
        shutil.rmtree(out, ignore_errors=True)
        from orch.dirrename import rename_dir   # 刚写完十几万个文件，Windows 上杀软 / 索引扫描开着句柄时目录改名报 WinError 5：按 #361 同一套退避重试
        rename_dir(tmp, out)                     #   （11-15 Windows 出 Switch 2 大款：展开 22 分钟后在这一步报拒绝访问、整单失败）
        say(f'🟢 展开完成：{stat[0]:,} 个文件，真写 {stat[1] / 2**30:.2f} GB，同内容共用 {stat[2] / 2**30:.2f} GB（{stat[2]:,} 字节）→ {out}')
        return app


def _clone_only(s, d):   # macOS 同卷 APFS 克隆（不占新空间）→ True；其余平台或克隆不成 → False（ctypes.CDLL(None) 在 Windows 上抛 TypeError，#128）
    if sys.platform == 'darwin':
        try:
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            if hasattr(libc, 'clonefile') and libc.clonefile(os.fsencode(s), os.fsencode(d), 0) == 0:
                return True
        except OSError:
            pass
    return False


def _clone(s, d):   # 克隆不成就真拷
    return d if _clone_only(s, d) else shutil.copy2(s, d)


def inspect_row(src):
    """认包行：路径 字节 修改时间（纳秒精度，同 pack/srcs.sh 的 stat %.9Y）程序号（大写 16 位）"""
    with Nup(src) as n:
        app = scan(n)[0]
    st = os.stat(src)
    return f'{src}\t{st.st_size}\t{st.st_mtime_ns // 10**9}.{st.st_mtime_ns % 10**9:09d}\t{app:016X}'


def default_name(src):   # 同 pack/oneclick/oneclick.sh / pack/orch/oneclick_win.py：文件名去掉「 [程序号][版本]」
    return os.path.splitext(os.path.basename(src))[0].split(' [')[0]


def to_extract(src, out, name=None, say=print):
    """NUP → 解包目录：本脚本展开成导出目录布局，再交 pack/ns2_dump.py 整理（子进程：它报红即退出）。返回退出码"""
    import subprocess
    out = os.path.abspath(out)
    dump = out + '.ns2_dump'
    try:
        convert(src, dump, say)
        sys.stdout.flush()   # 日志里展开的行在 ns2_dump 的行前面（管道下 stdout 是块缓冲）
        return subprocess.run([sys.executable, '-X', 'utf8', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ns2_dump.py'),
                               dump, out, '--name', name or default_name(src), '--consume']).returncode
    finally:
        shutil.rmtree(dump, ignore_errors=True)   # 中间目录：散文件 / DLC 已被 --consume 挪进解包目录，余下（exefs 等）是克隆来源，留着白占一份
        shutil.rmtree(dump + '.tmp', ignore_errors=True)


def main(a):
    if len(a) == 2 and a[0] == '--detect':
        f = find_nup(a[1])
        if f:
            print(f)
            return 0
        return 1
    if len(a) == 2 and a[0] == '--inspect':
        f = find_nup(a[1])
        if not f:
            print(f'🔴 {a[1]} 不是 NUP，或文件夹里不止 / 没有一个 .nup', file=sys.stderr)
            return 1
        try:
            print(inspect_row(os.path.abspath(f)))
        except (Error, OSError) as e:
            print(f'🔴 {e}', file=sys.stderr)
            return 1
        return 0
    if a[:1] == ['--to-extract']:
        name = None
        if '--name' in a:
            i = a.index('--name')
            name = a[i + 1] if i + 1 < len(a) else ''
            a = a[:i] + a[i + 2:]
        if len(a) != 3 or name == '':
            print(__doc__, file=sys.stderr)
            return 2
        f = find_nup(a[1])
        if not f:
            print(f'🔴 {a[1]} 不是 NUP，或文件夹里不止 / 没有一个 .nup', file=sys.stderr)
            return 1
        try:
            return to_extract(f, a[2], name)
        except (Error, OSError) as e:
            print(f'🔴 {e}', file=sys.stderr)
            return 1
    if len(a) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    f = find_nup(a[0])
    if not f:
        print(f'🔴 {a[0]} 不是 NUP，或文件夹里不止 / 没有一个 .nup', file=sys.stderr)
        return 1
    try:
        convert(f, os.path.abspath(a[1]))
    except (Error, OSError) as e:
        print(f'🔴 {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
