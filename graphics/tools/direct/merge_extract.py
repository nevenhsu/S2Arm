#!/usr/bin/env python3
# 直连着色器库·合并抽取（双轨 §12.3，着色器线 10-24）：把 S2Arm 抽取（graphics/tools/shbake/nxsh_extract.py）有、直连抽取（shader_split.py）没有的着色器，
# 补进直连包目录与着色器表，让直连出库（C# dsh_batch build，graphics/tools/direct/dsh/batch/Build.cs）原样出「两边并集」的直连库；同时出两张给运行时用的表。
#
# 键口径一律用直连口径（运行时 nvn_mtl.m prog_key 按它查库）：sha256(控制段[0:L] ‖ 机器码)[:16]，L = 控制段 +0x20 自报长度落在 [0x760, 0x880) 时取它，否则 0x880；第 2 代哈希前把两段易变字段置零（translator/nvn_key.py）。
#   S2Arm 抽取取完整自报长度，控制段长于 0x880 的两边键不同（10-24 动森 9794 个里 28 个），所以 S2Arm 那边的键在这里按直连口径重算，不直接用。
#
# 用法：merge_extract.py <S2Arm 抽取目录（含 src/ 与 index.tsv）> <直连包目录（*.bin）> <直连抽取 shader_map.tsv> <输出目录>
# 输出（都在 <输出目录>，输入一律不改）：
#   pkg/          直连包目录的克隆 + 新增的 <阶段>_s2a.bin（S2Arm 独有键；记录格式同 shader_split：'BTCDBTCD' + 0x38 头（+0x28 控制段长、+0x2c 机器码长）+ 机器码 + 控制段）
#   shader_map.tsv  直连原表 + 新增行（包名、序号、阶段、键、来源「s2a:<S2Arm 原键> <S2Arm 首个来源>」）
#   union_keys.tsv  并集键清单（一行一个），出库后与实际出成的库求交得 keys.tsv
#   key_map.tsv     S2Arm 键 → 直连键（并集全部；通用模式按烘焙出的「客体代码 → S2Arm 键」再经它换成直连键写 seen_shaders.tsv）
#   s2a_src/        直连抽取独有键按 S2Arm 抽取格式补的 src/*.pk（graphics/tools/shbake/srcpack.py）与 index.tsv（追加到 S2Arm 抽取后，通用模式的烘焙也覆盖并集）
#   shader_facts.tsv  答案表：配置档原表（<直连抽取 shader_map.tsv> 同目录的 shader_facts.tsv）逐行照抄，并集里的计算着色器缺线程组的按控制段补 local / shared
#                   （同 shader_split.py：+0x748 起 3 个 u32 线程组、+0x754 共享字节数按 16 对齐；配置档已有的字段优先）。原先只有直连抽取抽到的计算着色器有这两项，
#                   S2Arm 独有的没有 → 翻译按 1×1×1 编、反射线程组为空，运行时整次派发被跳过（10-25 暗黑2 直连场景全黑：库里 26 个计算着色器全是 S2Arm 独有）
#   merge.json      读数：两边各多少、交集、各自独有、并集
# 另一用法：merge_extract.py --alias <包目录> <shader_map.tsv> <输出 key_alias.tsv>：出包内第 1 代 → 第 2 代键别名表（直连库收表时与 shader_map.tsv 同处随包，见 alias）
import hashlib, json, os, shutil, struct, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'shbake'))
import srcpack   # S2Arm 抽取的着色器字节存放（src/*.pk）
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'translator'))
import ns2_shaders as ns2   # Switch 2（NVN2，控制段版本 0xc9）的键规则与控制段字段，与 shader_split.py / 宿主 nvn2_key 同一份
import nvn_key   # Switch 1 键规则，与 shader_split.py / 宿主 nvn_key.h 同一份

STAGE_WORD = {'vs': 'vertex', 'hs': 'hull', 'ds': 'domain', 'gs': 'geometry', 'ps': 'pixel', 'cs': 'compute', 'ts': 'task', 'ms': 'mesh'}   # = shader_split.py STAGE_NAME
SEP = b'BTCDBTCD'


ctl_len = nvn_key.ctl_len
nx_key = nvn_key.skey   # 第 2 代键（两段易变字段置零，translator/nvn_key.py）


def records(d):   # 同出库 Build.cs Records：按分隔符切
    out, p = [], d.find(SEP)
    while p >= 0:
        q = d.find(SEP, p + 1)
        out.append(d[p:q if q >= 0 else len(d)]); p = q
    return out


def split_rec(rec):
    """直连包记录 → (机器码, 控制段)"""
    nctl, ncode = struct.unpack_from('<2I', rec, 0x28)
    code = rec[0x38:0x38 + ncode]; ctl = rec[0x38 + ncode:0x38 + ncode + nctl]
    if len(code) != ncode or len(ctl) != nctl: raise ValueError('记录长度与头不符')
    return code, ctl


def make_rec(code, ctl):
    rec = bytearray(SEP + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); rec += code + ctl
    if bytes(rec).count(SEP) != 1: raise ValueError('记录分隔符出现在记录内部')
    return bytes(rec)


def cs_facts(ctl):
    """计算着色器控制段 → {'local': [x, y, z], 'shared': 字节数按 16 对齐}；读不到 / 越界返回 None（同 shader_split.py）"""
    if ns2.is_nvn2(ctl) and len(ctl) >= 0x68: (x, y, z), sh = ns2.compute_facts(ctl)   # NVN2：线程组 +0x58、共享内存 +0x64
    elif len(ctl) < 0x758: return None
    else: x, y, z = struct.unpack_from('<3I', ctl, 0x748); sh = struct.unpack_from('<I', ctl, 0x754)[0]
    if not (x and y and z and x * y * z <= 1024 and sh <= 0x10000): return None
    return {'local': [x, y, z], 'shared': (sh + 15) // 16 * 16}


def read_map(path):
    head, rows = [], []
    for ln in open(path, encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if ln.startswith('#') or c[0] == 'package' or len(c) < 5: head.append(ln); continue
        rows.append(c)
    return head, rows


def alias(pkg, mapf, outf):
    """包内键别名表 key_alias.tsv（随包，与 shader_map.tsv 同处）：表里每支着色器按记录的控制段 + 代码重算第 1 代键（不置零），与第 2 代键不同的写一行「第 1 代键<TAB>第 2 代键」。
    宿主读档恢复存档里的第 1 代键、读本机管线记录时按它换成第 2 代（nvn_mtl.m nvn_be_shader_key_canon）。Switch 2 着色器两代同键、不进表。→ 写入行数"""
    recs, out = {}, {}
    for r in read_map(mapf)[1]:
        name, idx, key = r[0], int(r[1]), r[3]
        if name not in recs: recs[name] = records(open(os.path.join(pkg, name), 'rb').read())
        code, ctl = split_rec(recs[name][idx])
        if nvn_key.skey(ctl, code) != key: continue   # 表行与记录对不上的不作别名（合并时已报过）
        v1 = nvn_key.skey(ctl, code, gen=1)
        if v1 != key: out[v1] = key
    with open(outf, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# 第 1 代键\t第 2 代键（translator/nvn_key.py）\n'); f.writelines(f'{a}\t{b}\n' for a, b in sorted(out.items()))
    return len(out)


def main(a):
    s2x, nxpkg, nxmap, out = a
    red, yellow = [], []
    os.makedirs(out, exist_ok=True)
    # 直连抽取侧：表里的键 + 包文件里的记录
    head, rows = read_map(nxmap)
    nx_keys = {r[3] for r in rows}
    packs = {}
    nx_rec = {}
    for r in rows:
        name, idx = r[0], int(r[1])
        if name not in packs: packs[name] = records(open(os.path.join(nxpkg, name), 'rb').read())
        code, ctl = split_rec(packs[name][idx])
        k = nx_key(ctl, code)
        if k != r[3]: red.append(f'直连抽取表 {name}#{idx} 键 {r[3]} 与记录重算 {k} 不符')
        nx_rec.setdefault(r[3], (r[2], code, ctl, f'nx:{name}#{idx} {r[4]}'))
    # S2Arm 侧：按直连口径重算
    stage_of, src_of = {}, {}
    for ln in open(os.path.join(s2x, 'index.tsv'), encoding='utf-8'):
        if ln.startswith('#'): continue
        c = ln.rstrip('\n').split('\t')
        if len(c) >= 3: stage_of[c[0]] = c[1]; src_of[c[0]] = c[2]
    s2 = {}; src = srcpack.Src(s2x)   # S2Arm 抽取的着色器字节在 src/*.pk（graphics/tools/shbake/srcpack.py）
    for k, st in stage_of.items():
        if k not in src:   # 表里有、src/*.pk 里没有字节（#1009：抽取写到一半 / 记录坏了，读端在坏记录处停）：这支跳过、运行时首遇现编，不让整次出包失败（不许堵门）
            yellow.append(f'S2Arm 键 {k} 在 index.tsv 里、src/*.pk 里没有字节：这支不进离线库，运行时首遇现编'); continue
        code, ctl = src.get(k)
        if st not in STAGE_WORD: red.append(f'S2Arm 键 {k} 阶段 {st} 不认识'); continue
        nk = nx_key(ctl, code)
        s2[nk] = (k, st, code, ctl)
    only = sorted(set(s2) - nx_keys)
    # 包目录：克隆直连抽取的（APFS 克隆不占空间），追加 <阶段>_s2a.bin
    pd = os.path.join(out, 'pkg')
    if os.path.exists(pd): shutil.rmtree(pd)
    os.makedirs(pd)
    for f in sorted(os.listdir(nxpkg)):
        if f.endswith('.bin'):
            if f.endswith('_s2a.bin'): red.append(f'直连包目录里已有 {f}（与本工具新增的文件重名）')
            s, d = os.path.join(nxpkg, f), os.path.join(pd, f)
            if sys.platform != 'darwin' or subprocess.run(['cp', '-c', s, d], capture_output=True).returncode: shutil.copyfile(s, d)   # macOS APFS 克隆，不支持就普通拷；别的系统直接普通拷（Windows 原生进程的 PATH 上没有 cp）
    add, per = [], {}
    for nk in only:
        k, st, code, ctl = s2[nk]
        name = f'{st}_s2a.bin'; per.setdefault(name, []).append(make_rec(code, ctl))
        add.append([name, str(len(per[name]) - 1), STAGE_WORD[st], nk, f's2a:{k} {src_of.get(k, "")}'])
    for name, recs in per.items():
        with open(os.path.join(pd, name), 'wb') as f: f.write(b''.join(recs))
    with open(os.path.join(out, 'shader_map.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(head); f.writelines('\t'.join(r) + '\n' for r in rows); f.writelines('\t'.join(r) + '\n' for r in add)
    # 反向：直连抽取独有键按 S2Arm 抽取格式补（键按 S2Arm 口径 = 整段控制段），供通用模式烘焙
    word_stage = {v: k for k, v in STAGE_WORD.items()}
    sd = os.path.join(out, 's2a_src'); os.makedirs(sd, exist_ok=True); sw = srcpack.Writer(sd)
    key_map, nx_only_rows = {s2[nk][0]: nk for nk in s2}, []
    no_s2a = 0
    for nk in sorted(nx_keys - set(s2)):
        word, code, ctl, src = nx_rec[nk]
        st = word_stage.get(word)
        if not st: red.append(f'直连键 {nk} 阶段 {word} 不认识'); continue
        if st in ('ts', 'ms'): no_s2a += 1; continue   # NVN2 任务 / 网格：S2Arm 抽取格式（nxsh_extract 六阶段）与通用模式都没有这两个阶段，不反向补
        sk = hashlib.sha256(nvn_key.key_bytes(ctl) + code).hexdigest()[:16]   # S2Arm 口径（整段控制段，nxsh_extract key_of），同样第 2 代置零
        sw.put(sk, code, ctl)
        nx_only_rows.append(f'{sk}\t{st}\t{src}\t0\n'); key_map[sk] = nk
    sw.flush()
    with open(os.path.join(sd, 'index.tsv'), 'w', encoding='utf-8', newline='\n') as f: f.write('# 键\t阶段\t首个来源\t变体号\n'); f.writelines(nx_only_rows)
    with open(os.path.join(out, 'key_map.tsv'), 'w', newline='\n') as f:
        f.write('# S2Arm 键\t直连键\n'); f.writelines(f'{a}\t{b}\n' for a, b in sorted(key_map.items()))
    # 答案表：配置档原表照抄，计算着色器缺线程组的按控制段补（配置档已有的字段优先）
    fp = os.path.join(os.path.dirname(os.path.abspath(nxmap)), 'shader_facts.tsv')
    fhead, facts = [], {}
    if os.path.exists(fp):
        for ln in open(fp, encoding='utf-8'):
            if ln.startswith('#') or not ln.strip(): fhead.append(ln); continue
            fk, fv = ln.rstrip('\n').split('\t', 1); facts[fk] = json.loads(fv)
    cs_ctl = {nk: ctl for nk, (word, code, ctl, src) in nx_rec.items() if word == 'compute'}
    cs_ctl.update({nk: v[3] for nk, v in s2.items() if v[1] == 'cs' and nk not in cs_ctl})
    filled, no_tg = 0, []
    for nk in sorted(cs_ctl):
        if 'local' in facts.get(nk, {}): continue
        cf = cs_facts(cs_ctl[nk])
        if cf is None: no_tg.append(nk); continue   # 读不出不挡出库：答案表不补（同修前），运行时这支派发照旧跳过；10-25 异度3 68 个里 65 个控制段 +0x748 全 0（这一代线程组不在控制段）
        facts[nk] = {**cf, **facts.get(nk, {})}; filled += 1
    with open(os.path.join(out, 'shader_facts.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(fhead); f.writelines(fk + '\t' + json.dumps(facts[fk], sort_keys=True, separators=(',', ':')) + '\n' for fk in sorted(facts))
    union = sorted(nx_keys | set(s2))
    with open(os.path.join(out, 'union_keys.tsv'), 'w', newline='\n') as f: f.writelines(k + '\n' for k in union)
    st = {'direct': len(nx_keys), 's2arm': len(s2), '交集': len(nx_keys & set(s2)), 's2arm独有': len(only), 'direct独有': len(nx_keys - set(s2)), '并集': len(union),
          '新增行按阶段': {n: len(r) for n, r in sorted(per.items())}, '反向补进 S2Arm': len(nx_only_rows), '任务 / 网格不反向补': no_s2a, '计算着色器': len(cs_ctl), '答案表补线程组': filled, '控制段读不出线程组': no_tg, '红': red}
    json.dump(st, open(os.path.join(out, 'merge.json'), 'w', newline='\n'), ensure_ascii=False, indent=1)
    for r in red: print('【红】' + r)
    for r in yellow[:20]: print('【黄】' + r)
    if len(yellow) > 20: print(f'【黄】……共 {len(yellow)} 支没有字节、跳过')
    print(f"{'🔴' if red else '🟢'} 合并抽取：直连抽取 {st['direct']}、S2Arm {st['s2arm']}、交集 {st['交集']}、S2Arm 独有 {st['s2arm独有']}（已补进包目录与表）、直连抽取独有 {st['direct独有']}（已按 S2Arm 格式补 {st['反向补进 S2Arm']} 个）、并集 {st['并集']}；计算着色器 {st['计算着色器']} 个、答案表按控制段补线程组 {filled} 个{f'；{len(no_tg)} 个控制段读不出线程组（不补，运行时这些派发会跳过）' if no_tg else ''}")
    return 1 if red else 0


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == '--alias':   # merge_extract.py --alias <包目录> <shader_map.tsv> <输出 key_alias.tsv>
        print(f'键别名：{alias(*sys.argv[2:])} 支第 1 代键 → 第 2 代'); sys.exit(0)
    if len(sys.argv) != 5: sys.exit(__doc__ or '用法：merge_extract.py <S2Arm 抽取目录> <直连包目录> <直连抽取 shader_map.tsv> <输出目录>')
    sys.exit(main(sys.argv[1:]))
