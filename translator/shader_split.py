#!/usr/bin/env python3
# All of Mario's shaders → the "package directory" dmsh_build.py understands + the shader map + the translation query answer table (thread group / shared memory of compute shaders).
# Sources (a full scan, not relying on file names): every file under RomFS (a zstd file is decompressed first, whatever its name: Mario's .zs, Paper Mario: The Thousand-Year Door's .zst; Yaz0 is decompressed, SARC is expanded member by member) and ExeFS main (NSO, lz4-decompressed), looking for
#   · BNSH ('BNSH\0\0\0\0', carried by bfsha / particle effect esetb / UI layout blarc / embedded in main):
#       +0x60 grsc block; +0x7c variation count u32; +0x80 variation table pointer (pointers are relative to the BNSH start). A variation is 0x40 bytes; the binary program is at +0x10;
#       the binary program has 6 stage pointers from +0x08: vertex, hull, domain, geometry, pixel, compute; the stage code descriptor has +0x08 control pointer, +0x10 code area pointer, +0x18 code area length, +0x1c control length.
#   · BAHS (agl's sharcb: the agl / gsys resource packs, GameAglShader in the Secred pack): file header +0x1c code region start, +0x18 code region length;
#       each binary entry in the table area is 16 bytes {code offset (relative to the code region, 0x100-aligned), 0, control length 0x880 / 0x88c (3D World 0x848), signed offset of the control relative to this entry's address}.
#       Control sections are shared between programs, so pairing goes by entry, never by finding code from a control; entries are recognized by a triple check of "control magic + code magic + consistent lengths".
#       NVN2 (Switch 2) BAHS entries hold 0xd8-byte NVN2 control sections (see bahs_shaders); agl's NDSR pack (agl_ndsr.pck) and sead's NVN2 primitive drawer have layouts of their own (ndsr_shaders, sead_prim_shaders)
#   · Unity (UnityFS bundles and the serialized files under Data, read by unityfs.py): the Switch subprograms of each Shader object; the program code = a 0x60 header + one block per stage.
#       The header is 4 i32 ×6 arrays indexed by NVN stage (0 vertex 1 pixel 2 geometry 3 hull 4 domain 5 compute): [0] unused (0 or a scratch size), [1] block offset (from +0x60, -1 = no such stage), [2] control length, [3] code length;
#       block = control section + code area. A program's vertex / pixel stages form a pair. The game's own serialized files (data.unity3d, unity default resources) carry no type trees and borrow the bundles' Shader type tree of the same hash (second pass),
#       or take the layout unityfs.py has for that hash (KNOWN_TREES), or else the one generated from the Unity version the file records (unityfs.shader_layout), when no bundle has one
#       (Brain Memory Training Academy, Unity 2022.3: no bundles at all; Lost Castle / Romancing SaGa 3, 2017.4; ShakesOnAPlane, 2019.2; Terraria, 2019.4). Compute shaders without a tree have their
#       kernels' code found in the object bytes (unity_scan_kernels). An empty or inconsistent program block is left out, reported and counted (unity_program), not the end of the extraction
#       Before 2019.3 (Cuphead, Unity 2017.4; found by armyja, GitHub pull request #2) a subprogram holds one stage: program code = a 0x10 header {?, ?, control length, code length} + control section
#       + code area, and the stage is not in the code but in the Shader's m_ParsedForm (unity_stages)
#       Unity 2022 (Ace Attorney Investigations Collection, 2022.3) keeps the 0x60 header in the program code, but the blob also holds parameter entries, which unity_code_blobs leaves out
#   · MSHD (MercurySteam, Metroid Dread system/shd/*.bshdat): each stage = a control section (+0x20 = control length; sometimes a small compiler option table follows) + the code area starting at the next code header;
#       NV assembly text follows the first 0x880 bytes of the control section, and SASS disassembly text follows the code area. All 478 files hold "vertex, pixel" in pairs (1, 2 or 4 pairs), paired two by two in order
#   · G2S (Koei Tecmo's KTGL, Hyrule Warriors: Age of Calamity): GLSLC compiler outputs inside the IDRK entries of the RDB data files (zlib in chunks), see idrk_shaders
#   · SPKG (the older KTGL of Fire Emblem: Three Houses): shader packages of G2S containers, loose and inside the LINKDATA archive DATA0.bin / DATA1.bin, see kt_shaders and linkdata_jobs
#   · SDF (Capcom's RE Engine, Monster Hunter Rise): shader definitions inside the PAK archives (KPKA, entries zstd / deflate compressed), only the newest copy of each path, see kpka_jobs and sdf_shaders
#   · Trinity packs (Game Freak, Pokémon Legends: Z-A): BNSH inside the files of arc/data.trpfs (Oodle Mermaid, decoded by the game's own decoder in the Mac host), see trpfs_jobs and trinity_shaders
#   · SPK (Capcom's MT Framework, The Great Ace Attorney Chronicles): the shader package inside an ARC archive (zlib entries), code area + control section per shader, see mtarc_members and mtspk_shaders
#   · BEA (NintendoWare Bezel Engine archives, Big Brain Academy: Brain vs. Brain): BNSH in the .bfsha entries of the archives (zstd entries), see bea_members
#   · MGFX (MonoGame effects, Celeste): the effects in its XNB files and SpriteBatch's in main, each shader a control section and a code area, see mgfx_effect
#   · dvdbnd (FromSoftware, Dark Souls Remastered): GLSLC outputs in the BND3 archives (DCX zlib entries) of the .bdt files, indexed by the .bhd5 beside each, see bhd5_entries and bdt_shaders
#   · Falcom packages (The Legend of Heroes: Hajimari no Kiseki): one BNSH per LZ4 entry of the .pkg files under shaders/, see falcom_pkg and falcom_pkg_shaders
# Code area: 0x30 header + (graphics) 0x50 SPH + instructions; header magic 0x12345678 for graphics, 0x12345679 for compute. The control section starts with 0x98761234, +0x704 = code area length,
#   +0x700 = start of the c1 constant segment within the code area; for compute +0x748 thread group xyz (u32 ×3), +0x754 shared memory bytes (checked one by one against PC reflection on D2R's 22 compute shaders).
# Stage: BNSH uses the stage slot; BAHS uses bits 10..13 of SPH word 0 (1 vertex 2 hull 3 domain 4 geometry 5 pixel), and a compute header means compute.
# Key = sha256(first 0x880 bytes of the control section (its own length when control +0x20 says it is shorter, see ctl_len; two volatile spans zeroed, nvn_key.py) + code area)[:16], the same as the host's prog_get: in this game many shaders share a control section but differ in code
#   (SecredUber's 16600 stage shaders have only 3079 distinct control sections, with 6367 cases of same control, different code), so hashing the control alone picks the wrong library.
# Output: <package dir>/<stage>_<source group>.bin, each record = 'BTCDBTCD' + header (+0x28 control length, +0x2c code length) padded to 0x38 + code area + control section, one copy per key;
#   map rows have the same columns as D2R's shader_map.tsv (column 5 records where the key first appeared); the answer table has one row per compute shader {"local", "shared"} (shared memory rounded up to 16 bytes, as D2R did).
# Usage: shader_split.py <RomFS dir> <ExeFS main> <output package dir> <output map> <output answer table> [<output pair table>]   (decompressing main needs lz4: run with dm's venv python)   Environment J=concurrency (default S2A_JOBS / every core)
#   With an update: give RomFS and main as comma-separated lists (base game first, e.g. data/v0/RomFS,data/v327680/RomFS); the output is the union of both versions (see main())
#   Unity type trees need Unity's common string table, which is taken from the first main
# Pair table: (vertex key, pixel key) of every BNSH variation (every Unity program); dmsh_build uses it to give vertex shaders the outputs their pixel shaders read but they do not write (see dmsh_build.py vs_pad)
import collections, concurrent.futures as cf, hashlib, json, mmap, os, posixpath, re, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../graphics/nvn/games')); import game as _game
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import uepak
import ns2_shaders as ns2   # Switch 2 (NVN2) GLSLC outputs and control sections
import nvn_key   # Switch 1 shader key rule (the one Python copy, same as the host's nvn_key.h)
# 仓库里唯一的 RomFS 遍历（graphics/nvn/tools/romfs_list.py：walk 同 os.walk）
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'graphics', 'nvn', 'tools')); import romfs_list
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../graphics/tools/shbake')); import zstd_lib, unreal_pak   # the repository's one zstd decoder (libzstd beside the zstd tool; the player app's Python 3.12 has no compression.zstd)
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'graphics', 'tools')))
import nxfmt
_NXFMT_SRC = 'graphics/tools/nxfmt.py'  # 字面路径给 pack/stage_inputs.py 收进抽取 / 直连库的源码键
if os.path.basename(nxfmt.__file__) != os.path.basename(_NXFMT_SRC):
    raise ImportError(_NXFMT_SRC)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../tools')); from progress import Ticker   # the pack progress protocol (tools/progress.py)
GAME = _game.load()   # the current game profile (DM_GAME, required): source group rules and the table header note
GROUPS, NOTE = GAME.pairs('shader_groups'), GAME.get('shader_note')
# The sources are what the game files hold, with no profile switch: GLSLC outputs in main (glslc_shaders), the outputs the game compiles at run time when a capture is there (DM_GLSLC_CAPTURE,
# hle_glslc.c), the shader code archives of Unreal paks once decoded on the Mac (uepak.py, DM_OODLE_JOB, hle_oodle.c); the NSP script decides from the files which of the last two to make

CTL_MAGIC, GFX_MAGIC, CS_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
# An NVN2 BNSH program has stage slots past those six: 7 holds the task shader and 8 the mesh shader, their stage in the control section (+0x56: 6 / 7; their SPH reads as vertex or
# domain). The Legend of Zelda: Ocarina of Time's Model / Foliage .bfsha: read from the six slots alone, every mesh-shaded draw's shaders were missing from the map (11-05 测试设备1
# 第十一版: 11aa3147be208368 stage byte 6, 不在库里). Slot 6 has not been seen used; it is read the same way. Ported from 上游参考实现（MIT） PR #1299 shader_split.py (MIT), our checks
NVN2_SLOTS = 3
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}
def sph_stage(code):
    """A graphics code area's stage from its SPH word 0: shader type in bits 10..13. Catherine: Full Body's GLSLC (SDK 8.2.1, GPU code version 1.15) writes 0 there in every vertex shader
    (word 0 = 0x0061: SPH type 1 VTG, version 3), in its shaders_binary files and in what it compiles at run time, where every other game's vertex shaders have 1 (0x0461); Ryujinx's ShaderHeader
    reads a graphics header's 0 as a vertex shader as well, so it is one here"""
    w = struct.unpack_from('<I', code, 0x30)[0]
    return 'vs' if (w >> 10) & 0xf == 0 and w & 0x1f == 1 else SPH_STAGE.get((w >> 10) & 0xf)
STAGE_NAME = {'vs': 'vertex', 'hs': 'hull', 'ds': 'domain', 'gs': 'geometry', 'ps': 'pixel', 'cs': 'compute', 'ts': 'task', 'ms': 'mesh'}   # shader_map.tsv column 3

def red(msg): sys.exit('[RED] ' + msg)
# 直连包一条记录：'BTCDBTCD' + 0x38 头（+0x28 控制段长、+0x2c 代码长）+ 代码 + 控制段。读这些记录的都按 BTCDBTCD 分记录、按头里的长度切代码 / 控制段
#   （graphics/tools/direct/dsh/batch/PkgRead.cs Split、graphics/tools/direct/merge_extract.py、android/tools/direct_prepare.py），没有按 CTL_MAGIC 找控制段的。
#   原先代码区里出现 CTL_MAGIC 四个字节就整批退出（为早已不在仓库里的 dmsh_build），一支着色器碰巧含这四字节，整款离线直连库就是 0 支（#477）：改成只记一行。
#   分隔符出现在记录里仍然退出：按 BTCDBTCD 分记录的读法会真的切错
def pkg_record(key, ctl, code):
    rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); rec += code + ctl
    if rec.find(CTL_MAGIC) != 0x38 + len(code): print(f'[WARN] key {key}: the control magic also appears in the code area; readers cut by the header lengths, kept', flush=True)
    if rec.count(b'BTCDBTCD') != 1: red(f'key {key}: the record separator appears inside the record')
    return rec

def ctl_len(ctl):
    if ns2.is_nvn2(ctl): return ns2.KEY_CTL   # Switch 2: see ns2_shaders.py
    """Number of control bytes in the key: control +0x20 holds the section's own length. Every control section of Mario, Fire Emblem Engage, Metroid Dread and 13 Sentinels is at least 0x880 bytes, so the first 0x880 are used;
    SDK 10's Super Mario 3D World + Bowser's Fury has 2,888 that are only 0x850 bytes (the bfsha in ShaderData, 6 layouts), with other data right after them, so only their own length is used (the same as the host's keys_hash)"""
    return nvn_key.ctl_len(ctl)

skey = nvn_key.skey   # generation 2 (two volatile control spans zeroed), see nvn_key.py

def same_shader(a, b):
    """Two copies of one key hold the same shader: the code areas are equal, and so are the control sections up to their own length (control +0x20). Pokémon Legends: Z-A's UI layout archives
    (__ArchiveShader.bnsh inside the .truiv packs) give 104 KB control sections whose stage descriptor length runs 456 bytes past their own length, and those trailing bytes differ between copies.
    Switch 1 control sections compare as the key hashes them (generation 2, nvn_key.key_bytes: two volatile spans zeroed): two copies that differ only there are the same shader
    (same translation; that is why the key merges them). Compared raw, a Unity game's variants that differ only there were taken for a key collision, red() ended the worker and
    whole data files were dropped: 52,692 shaders down to 138"""
    (ca, ka), (cb, kb) = a, b
    if ns2.is_nvn2(ca): return ka == kb and ns2.key_ctl(ca) == ns2.key_ctl(cb)
    n = struct.unpack_from('<I', ca, 0x20)[0] if len(ca) >= 0x24 else len(ca)
    n = n if 0x760 <= n <= min(len(ca), len(cb)) else max(len(ca), len(cb))
    return ka == kb and nvn_key.key_bytes(ca[:n]) == nvn_key.key_bytes(cb[:n])

def check(where, st, ctl, code):
    """control section and code area consistent; returns the reason if not"""
    if ns2.is_nvn2(ctl): return ns2.check(st, ctl, code)
    if ctl[:4] != CTL_MAGIC or len(ctl) < ctl_len(ctl): return 'bad control section'
    # a compute code area may have no header (SDK 10's Super Mario 3D World + Bowser's Fury and D2R: instructions start at +0; dmsh_build strips only a 0x12345679 header)
    if st == 'cs' and code[:4] in (CS_MAGIC, GFX_MAGIC) and code[:4] != CS_MAGIC or st != 'cs' and code[:4] != GFX_MAGIC: return 'code area header magic does not match the stage'
    if struct.unpack_from('<I', ctl, 0x704)[0] != len(code): return 'control +0x704 does not match the code area length'
    return None

class BnshOut(Exception): pass

def bnsh_shaders(d, base, label):
    """A BNSH container's shaders. Its fields are offsets from the container start; one that points outside the file (10-30 SUPER ROBOT WARS X: offset 0x5381bbab670 in a 465 MB
    file, a container whose pointers are not offsets from its start: relocated, or another layout) skips this container with a red line and a SKIPPED entry, and the other
    containers are still read (it used to raise struct.error and the game got no direct library at all; a key it misses is compiled on the device at its first use)"""
    if d[base + 0x60:base + 0x64] != b'grsc': red(f'{label} BNSH @{base:#x} has no grsc block')
    def at(fmt, off, what):
        if off < 0 or off + struct.calcsize(fmt) > len(d): raise BnshOut(f'{what} at {off:#x} is outside the file ({len(d):#x} bytes)')
        return struct.unpack_from(fmt, d, off)
    out = []
    try:
        nvar, pvar = at('<IQ', base + 0x7c, 'variation table')
        for v in range(nvar):
            prog = at('<Q', base + pvar + 0x40 * v + 0x10, f'variation {v}')[0]
            if not prog: continue
            for si, st in enumerate(STAGES):
                sc = at('<Q', base + prog + 8 + 8 * si, f'variation {v} program')[0]
                if not sc: continue
                pctl, pcode, ncode, nctl = at('<QQII', base + sc + 8, f'variation {v} {st} shader')
                if base + pctl + nctl > len(d) or base + pcode + ncode > len(d): raise BnshOut(f'variation {v} {st}: control / code area runs past the file end')
                ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
                why = check(label, st, ctl, code)
                if why: red(f'{label} BNSH @{base:#x} variation {v} {st}: {why}')
                out.append((f'{label}#BNSH@{base:#x}#var{v}', st, ctl, code))
            for si in range(len(STAGES), len(STAGES) + NVN2_SLOTS):   # past the six NVN stages: taken only where the slot holds an NVN2 control section (an NVN program's words there are no stage records)
                o = base + prog + 8 + 8 * si
                if o + 8 > len(d): continue
                sc = struct.unpack_from('<Q', d, o)[0]
                if not sc or base + sc + 0x20 > len(d): continue
                pctl, pcode, ncode, nctl = struct.unpack_from('<QQII', d, base + sc + 8)
                if not (0x60 <= nctl < 0x10000 and base + pctl + nctl <= len(d) and base + pcode + ncode <= len(d)): continue
                ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
                if not ns2.is_nvn2(ctl): continue
                st = ns2.stage(ctl)
                if st not in ('ts', 'ms'): red(f'{label} BNSH @{base:#x} variation {v}: NVN2 stage slot {si} holds stage byte {ctl[0x56]}, not a task or mesh shader')
                why = check(label, st, ctl, code)
                if why: red(f'{label} BNSH @{base:#x} variation {v} {st}: {why}')
                out.append((f'{label}#BNSH@{base:#x}#var{v}', st, ctl, code))
    except BnshOut as e:
        where, why = f'{label}#BNSH@{base:#x}', f'BNSH pointers out of the file: {e}'
        print(f'[RED] {where}: container left out ({why})', flush=True); SKIPPED.append((where, why))
        return []
    return out

def bahs_shaders(d, label):
    region, rsz = struct.unpack_from('<I', d, 0x1c)[0], struct.unpack_from('<I', d, 0x18)[0]
    if region + rsz > len(d): red(f'{label} BAHS code region out of bounds')
    out = []
    for a in range(0, region - 16, 4):
        off, z, csz, rel = struct.unpack_from('<IIIi', d, a)
        # NVN2 (Switch 2: Donkey Kong Bananza / Splatoon Raiders' agl and gsys resources): the control section is 0xd8 bytes (+0x30 holds 0 in BAHS files), the code area's length at +0x44
        #   and the stage at +0x56 (ns2_shaders.py); layout from upstream dm/gfx/tools/shader_split.py bahs_shaders (777a5f6c, MIT)
        nvn2 = 0x48 <= csz < 0x10000 and 0 <= a + rel and a + rel + csz <= region and ns2.is_nvn2(d[a + rel:a + rel + csz])
        if z or not (nvn2 or csz in (0x880, 0x88c) or 0x760 <= csz < 0x880) or off % 0x100: continue   # control length: Mario 0x880 / 0x88c, 3D World's BAHS version 9 0x848
        c = a + rel
        if c < 0 or c + csz > region or d[c:c + 4] != CTL_MAGIC: continue
        n = struct.unpack_from('<I', d, c + (0x44 if nvn2 else 0x704))[0]
        if off + n > rsz: continue
        code, ctl = d[region + off:region + off + n], d[c:c + csz]
        st = ns2.stage(ctl) if nvn2 else 'cs' if code[:4] == CS_MAGIC else sph_stage(code) if code[:4] == GFX_MAGIC else None
        if not st or check(label, st, ctl, code): continue
        # NVN2: the scan also hits words that only look like an entry (Bananza 6 of the 16,279 entries its BAHS files give, Raiders 7 of 21,474: control "lengths" 0x80 .. 0x2800, code areas that are not shaders). A real entry is the tail
        #   of a binary record {u32 record size, u32 kind (0 vertex, 1 fragment, 2 geometry, 3 compute), two u32, the entry}: every one of the 37,740 others has a record size of at least 0x20
        #   and the kind of its control section's stage; every stray one fails that (record layout: upstream shader_split.py bahs_variations, 777a5f6c, MIT)
        if nvn2:
            rs, kind = struct.unpack_from('<II', d, a - 0x10) if a >= 0x10 else (0, -1)
            if rs < 0x20 or kind != {'vs': 0, 'ps': 1, 'gs': 2, 'cs': 3}.get(st, kind): continue
        out.append((f'{label}#BAHS@{a:#x}', st, ctl, code))
    return out

def ndsr_shaders(d, label, base=0):
    """agl's NDSR pack (Lib/agl/agl_resource.*.sarc.zs agl_ndsr*.pck: Donkey Kong Bananza / Splatoon Raiders / Ocarina of Time's dynamic super-resolution upscaler; also inside
    other files: Ocarina's NDVE video files carry their decoder's pack at +0x28), starting at base: 'NDSR', a header, then 8-byte items, each u32 offset (from the pack start) and
    u32 (size << 4 | type). A shader is a control section item (its offset holds an NVN2 control section) followed by its code area item (type + 1: vertex 0 / 1, pixel 3 / 4,
    compute 6 / 7; size = control +0x44). Items of other types (constant data, tables) and header words sit between the pairs, so the pairs are not at a fixed stride: the
    16-byte stride read from upstream (dm/gfx/tools/shader_split.py ndsr_shaders, 777a5f6c, MIT) missed the pairs that fall on the odd 8 bytes (11-06 Ocarina: 2 of the 20 in
    agl_ndsr_vr.pck, 2 of agl_ndsr_vr_light.pck's, 1 of the NDVE's, all submitted on the opening and translated on the device), so each control section's offset is looked up
    as an item word in the pack's first 64 KB. One control section may head several items with different code areas (agl_ndsr_vr_light.pck): each is a shader"""
    if d[base:base + 4] != b'NDSR' or len(d) - base < 0x20: return []
    out, top = [], min(len(d), base + 0x10000)
    for m in re.compile(re.escape(ns2.CTL_MAGIC + ns2.CTL_VERSION)).finditer(d, base + 8):
        c = m.start(); word = struct.pack('<I', c - base); o = d.find(word, base + 8, min(c, top))
        while o >= 0:
            if (o - base) % 4 == 0 and o + 16 <= len(d):
                cl, k, kl = struct.unpack_from('<3I', d, o + 4)
                ctl = d[c:c + max(cl >> 4, ns2.KEY_CTL)]
                if cl >> 4 >= ns2.KEY_CTL and c + (cl >> 4) <= len(d) and kl & 0xf == (cl & 0xf) + 1 and kl >> 4 == ns2.code_len(ctl) and base + k + (kl >> 4) <= len(d):
                    code = d[base + k:base + k + (kl >> 4)]; st = ns2.stage(ctl)
                    if st is not None and not check(label, st, ctl, code): out.append((f'{label}#NDSR@{c - base:#x}', st, ctl, code))
            o = d.find(word, o + 1, min(c, top))
    return out

def sead_prim_shaders(d, label):
    """sead's primitive drawer for NVN2 (Lib/sead/primitive_renderer/primitive_drawer_nvn2_shader.bin, Donkey Kong Bananza / Splatoon Raiders): four u32 offsets (vertex control section,
    pixel control section, vertex code area, pixel code area); each control section names its own length (+0x30) and its code area's (+0x44). Not this layout → []. Layout from upstream
    dm/gfx/tools/shader_split.py sead_prim_shaders (777a5f6c, MIT)"""
    if len(d) < 0x10: return []
    offs = struct.unpack_from('<4I', d); out = []
    for st, c, k in (('vs', offs[0], offs[2]), ('ps', offs[1], offs[3])):
        if not (0x10 <= c and c + ns2.KEY_CTL <= len(d) and ns2.is_nvn2(d[c:c + ns2.KEY_CTL])): return []
        ctl = d[c:c + max(struct.unpack_from('<I', d, c + 0x30)[0], ns2.KEY_CTL)]
        code = d[k:k + ns2.code_len(ctl)]
        if len(code) != ns2.code_len(ctl) or check(label, st, ctl, code): return []
        out.append((f'{label}#SEADPRIM@{c:#x}', st, ctl, code))
    return out

def mshd_shaders(d, label):
    out, c = [], d.find(CTL_MAGIC)
    while c >= 0:
        nctl = struct.unpack_from('<I', d, c + 0x20)[0]
        g = d.find(GFX_MAGIC, c + nctl)
        if nctl < 0x880 or g < 0 or g - c - nctl > 0x400: red(f'{label} MSHD control section @{c:#x}: length {nctl:#x}, no code area header after it')
        ncode = struct.unpack_from('<I', d, c + 0x704)[0]
        ctl, code = d[c:c + nctl], d[g:g + ncode]
        st = sph_stage(code)
        if st != ('vs', 'ps')[len(out) % 2]: red(f'{label} MSHD @{c:#x}: stage {len(out)} is {st}, not alternating vertex / pixel')
        why = check(label, st, ctl, code)
        if why: red(f'{label} MSHD @{c:#x} {st}: {why}')
        out.append((f'{label}#MSHD#var{len(out) // 2}', st, ctl, code))
        c = d.find(CTL_MAGIC, g + ncode)
    if len(out) % 2: red(f'{label} MSHD: {len(out)} stages, not in pairs')
    return out

def glslc_stage(ctl, code):
    """A GLSLC output's stage, from its code area's header (graphics: the SPH's stage bits), or compute when the code has no header and the control's thread group (+0x748) is filled"""
    headerless_cs = code[:4] not in (CS_MAGIC, GFX_MAGIC) and len(ctl) >= 0x754 and all(struct.unpack_from('<3I', ctl, 0x748))
    return 'cs' if code[:4] == CS_MAGIC or headerless_cs else sph_stage(code) if code[:4] == GFX_MAGIC and len(code) >= 0x34 else None

def glslc_section(d, h, size):
    """The GPU code section of the GLSLC output at h, from its section headers: output +0x4c = section count, section i's header at +0x90 + 0x90 i = {u32 size, u32 data offset (from the
    output), u32 type (0 = GPU code)}; a GPU code header adds +0x30 control offset, +0x34 code offset (both from the section's data), +0x38 code length, +0x3c control length.
    → (control, code) of its only GPU code section, or None"""
    n, secs = struct.unpack_from('<I', d, h + 0x4c)[0], []
    if not 0 < n <= 16 or h + 0x90 * (n + 1) > len(d): return None
    for i in range(n):
        s = h + 0x90 + 0x90 * i
        ssz, soff, typ = struct.unpack_from('<3I', d, s)
        if typ: continue
        co, ko, kn, cn = struct.unpack_from('<4I', d, s + 0x30)
        if soff + ssz > size or co + cn > ssz or ko + kn > ssz: return None
        secs.append((d[h + soff + co:h + soff + co + cn], d[h + soff + ko:h + soff + ko + kn]))
    return secs[0] if len(secs) == 1 else None

def glslc_shaders(d, label, need=True):
    """GLSLC compiler outputs (838 in 13 Sentinels' main read-only segment, one stage each): from the magic 0x19866891, +0x44 = the whole output's length; each output holds exactly one control section with the code area right after it (control +0x20 = control length).
    The magic is only 4 bytes, so only outputs that pass the triple check are kept. Every game's main is searched: Unity's player has 11 of its own (Fire Emblem Engage, Ace Attorney Investigations
    Collection) beside the game's shaders in its bundles, KTGL and Vanillaware's engine compile theirs into main (13 Sentinels: 838), the others have none.
    need: stop when there is none (a capture file); a Koei Tecmo G2S container (idrk_shaders) or a main may hold none.
    SDK 6's GLSLC (output version 0x1d.2, Fire Emblem: Three Houses) writes 0x848- and 0x850-byte control sections (ctl_len's lower range) and compute code with no 0x12345679 header: the instructions
    start at code +0 (the first word is a scheduling word), and the stage then comes from the control's thread group (+0x748), which only compute sections fill. Its few version-0x1d.0 outputs keep
    something else at control +0x20 (odd values such as 0x136b), so a length that is not a multiple of 4 is not a control length"""
    out = []
    for m in re.finditer(b'\x91\x68\x86\x19', d):
        h = m.start()
        if h + 0x48 > len(d): continue   # the magic sits at the file's end: the length field itself is not in the file (check before reading, audit P2-45)
        size = struct.unpack_from('<I', d, h + 0x44)[0]
        c = d.find(CTL_MAGIC, h, h + size) if 0x760 < size <= len(d) - h else -1
        if c < 0 or c + 0x708 > h + size or d.find(CTL_MAGIC, c + 4, h + size) >= 0: continue   # both control lengths (+0x20, +0x704) must lie inside this output
        nctl, ncode = struct.unpack_from('<I', d, c + 0x20)[0], struct.unpack_from('<I', d, c + 0x704)[0]
        ctl, code = d[c:c + nctl], d[c + nctl:c + nctl + ncode]
        st = glslc_stage(ctl, code) if nctl >= 0x760 and not nctl & 3 and c + nctl + ncode <= h + size else None
        if not st or check(label, st, ctl, code):
            # SDK 4's GLSLC (output version 0x20.4, Dark Souls Remastered: 44 of main's 72 outputs and the pixel shaders in its archives) writes control sections whose +0x20 is
            # 12 bytes short of their length, so the code area is not right after it; the output's own section header says where both are (glslc_section). Only outputs the rule
            # above rejects are read this way: on every output it keeps (1,073 in 13 Sentinels, Three Houses, FINAL FANTASY TACTICS, Engage and Age of Calamity) both give the same key
            sec = glslc_section(d, h, size)
            if not sec: continue
            ctl, code = sec; st = glslc_stage(ctl, code)
            if not st or check(label, st, ctl, code): continue
        out.append((f'{label}#GLSLC@{h:#x}', st, ctl, code))
    out += ns2.glslc_shaders(d, label)   # the Switch 2's GLSLC (version 0x00220015): another header, read by ns2_shaders.py
    if not out and need: red(f'{label}: no GLSLC output')
    return out

IDRK, PDRK, ZCHUNKED = b'IDRK0000', b'PDRK0000', 0x100000
def idrk_entries(d):
    """Koei Tecmo RDB data files (Hyrule Warriors: Age of Calamity: asset/*.rdb.bin*, asset/data/*.file) are a run of IDRK entries: 'IDRK0000', u64 entry length, u64 stored length, u64 length,
    u32 0, u32 file KTID, u32 type KTID, u32 flags, a few parameter words, and the stored data at the end of the entry. Entries follow each other directly, some after padding (to 64 KB).
    → (start of the stored data, its end, flags) for each entry"""
    o = 0
    while o + 0x30 <= len(d):
        if d[o:o + 8] != IDRK:
            o = d.find(IDRK, o)
            if o < 0: return
        esz, csz = struct.unpack_from('<QQ', d, o + 8)
        yield o + esz - csz, o + esz, struct.unpack_from('<I', d, o + 0x2c)[0]
        o += esz

def idrk_data(d, a, b, flags, head=None):
    """An entry's data. Flag 0x100000 alone = zlib in chunks (Age of Calamity): u32 stored length + a zlib stream each, a zero length ends; the chunks inflate to at most 16 KB.
    Flag 0x400000 = chunks of another form (Pokémon Pokopia's .fdata, all 74,000 entries; ROMANCE OF THE THREE KINGDOMS 8 REMAKE): u16 stored length, 8 bytes that the reader
    does not need, the stream; a zero length ends, and each chunk inflates to at most 16 KB (0x4000). The streams are LZ4 blocks when 0x100000 is set too, zlib otherwise
    (the reference port dac4a433 idrk_data, MIT). Anything else is stored. head: only the first bytes"""
    import zlib
    new, lz4b = flags & 0x400000, flags & 0x500000 == 0x500000
    if not flags & 0x500000: return d[a:min(b, a + head) if head else b]
    out = []
    while a + 4 <= b:
        if new: cs = struct.unpack_from('<H', d, a)[0]; a += 10
        else: cs = struct.unpack_from('<I', d, a)[0]; a += 4
        if not cs: break
        if lz4b:
            import lz4.block
            r = lz4.block.decompress(d[a:a + cs], uncompressed_size=0x4000)
            if head: return r[:head]
        elif head: return zlib.decompressobj().decompress(d[a:a + min(cs, 1024)], head)
        else: r = zlib.decompress(d[a:a + cs])
        out.append(r); a += cs
    return b''.join(out)

def idrk_shaders(path, label, out):
    """Koei Tecmo's KTGL keeps its shaders in G2S containers ('_S2G3000', type KTID 0x7bcd279f in Age of Calamity: 23,108 entries in three RDB files, 2.3 GB inflated), each a material's variations
    as GLSLC compiler outputs (glslc_shaders). One '_DOK0000' container (type KTID 0x20a6a0bb, the engine's system resources; once in system.rdb.bin and once in the update's .bin1) holds 151 bare
    GLSLC outputs without a G2S wrapper, among them 21 shaders the first battle draws. No other entry type holds any. The RDB files are up to 3.8 GB, so they are mapped rather than read,
    and only entries whose data starts with one of those two magics are inflated"""
    import mmap
    with romfs_list.map_file(path) as d:
        for a, b, flags in idrk_entries(d):
            if idrk_data(d, a, b, flags, 4) in (b'_S2G', b'_DOK'): out.extend(glslc_shaders(idrk_data(d, a, b, flags), f'{label}#IDRK@{a:#x}', need=False))

G2S, SPKG = b'_S2G3000', b'SPKG'
def kt_gz(d):
    """Koei Tecmo's older chunked zlib (Fire Emblem: Three Houses: the compressed entries of DATA1.bin and the patch*/…/*.gz files): u32 chunk size (0x10000), u32 chunk count, u32 inflated length,
    u32 stored length of each chunk, then the chunks at 0x80 alignment, each a u32 length + one zlib stream, or the plain bytes when the stored length equals the chunk's inflated length (a short last
    chunk, or one that does not compress). → the inflated data, or None when d is not in that form"""
    import zlib
    if len(d) < 16: return None
    cs, n, tot = struct.unpack_from('<III', d)
    if cs != 0x10000 or not n or n != (tot + cs - 1) // cs or 12 + 4 * n > len(d): return None
    sizes = struct.unpack_from(f'<{n}I', d, 12); o = (12 + 4 * n + 0x7f) & ~0x7f; out = []
    for i, s in enumerate(sizes):
        if o + s > len(d): return None
        if s == min(cs, tot - i * cs): out.append(bytes(d[o:o + s]))
        else:
            ln = struct.unpack_from('<I', d, o)[0]
            try: out.append(zlib.decompress(d[o + 4:o + 4 + ln]))
            except zlib.error: return None
        o = (o + s + 0x7f) & ~0x7f
    return b''.join(out)

def kt_shaders(d, label, out):
    """Three Houses' KTGL keeps its shaders in SPKG packages (patch*/nx/shader/*.spkg, and 132 entries of DATA1.bin), each holding G2S containers ('_S2G3000', as in Age of Calamity) of GLSLC outputs,
    and in '_SPK4100' packages without G2S (DATA1.bin entry 549, the post effects: 27 outputs); map and model archives in DATA1.bin carry packages of their own. So every inflated file or entry is
    searched for GLSLC outputs, and its SARC archives (the Nintendo layouts, 2D UI) for BNSH, as everywhere else"""
    z = kt_gz(d)
    if z is not None: d = z
    if b'\x91\x68\x86\x19' in d: out.extend(glslc_shaders(d, label, need=False))
    if d[:4] == b'SARC' or b'BNSH\0\0\0\0' in d: scan_blob(d, label, out)

def linkdata_index(path):
    """The LINKDATA index beside a data file: DATA0.bin beside Three Houses' DATA1.bin, LI_<name>.IDX beside Pokopia's LD_<name>.BIN (LD_C0_R.BIN, the same 32-byte entries), LINKDATA.IDX beside
    DRAGON QUEST BUILDERS 2's LINKDATA.BIN (1.1 GB, 59,193 records; upstream (MIT) 12aeebe91); None: not one"""
    b = os.path.basename(path)
    idx = 'DATA0.bin' if b == 'DATA1.bin' else 'LINKDATA.IDX' if b == 'LINKDATA.BIN' else 'LI_' + b[3:-4] + '.IDX' if b.startswith('LD_') and b.endswith('.BIN') else None
    return os.path.join(os.path.dirname(path), idx) if idx and romfs_list.exists(os.path.join(os.path.dirname(path), idx)) else None

def linkdata_jobs(path, rel, parts=64):
    """Three Houses' LINKDATA: DATA0.bin, beside DATA1.bin (6.9 GB), indexes it with 32-byte entries {u64 offset, u64 inflated length, u64 stored length, u64 compressed}. The entries are split into
    ranges of about equal stored size, one scan job each. → [(path, rel, (first entry, end entry)), …], or None when there is no index beside the file"""
    idx = linkdata_index(path)
    if not idx: return None
    n = romfs_list.file_size(idx) // 32; step = romfs_list.file_size(path) / parts; jobs, lo = [], 0
    with romfs_list.open_file(idx) as f: ents = [struct.unpack('<QQQQ', f.read(32)) for _ in range(n)]
    for i in range(1, n + 1):
        if i == n or ents[i][0] >= (len(jobs) + 1) * step: jobs.append((path, rel, (lo, i))); lo = i
    return jobs

def linkdata_shaders(path, rel, rng, out):
    import mmap
    with romfs_list.open_file(linkdata_index(path)) as f: idx = f.read()
    with romfs_list.map_file(path) as d:
        for i in range(*rng):
            off, n, stored, comp = struct.unpack_from('<QQQQ', idx, i * 32)
            if n: kt_shaders(d[off:off + (stored if comp else n)], f'/{rel}#{i}', out)

KPKA = b'KPKA'
SDF_STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']   # an SDF entry's stage offsets, in BNSH order
def kpka_entries(path):
    """Capcom's RE Engine PAK (Monster Hunter Rise: re_chunk_000.pak plus 16 .patch_NNN.pak, 19.6 GB): 'KPKA', u8 version 4, u8 0, u16 features (0: the table is not encrypted), u32 count, u32 fingerprint,
    then count 0x30-byte entries {u32 hash of the lower-case path, u32 hash of the upper-case path, u64 offset, u64 stored length, u64 length, u64 attributes, u64 checksum}.
    Attribute bits 0-3: 0 stored, 1 raw deflate, 2 zstd. → [(path hash, offset, stored length, length, attributes)]"""
    with romfs_list.open_file(path) as f:
        h = f.read(16)
        if h[:4] != KPKA: return None
        ver, feat, n = h[4], struct.unpack_from('<H', h, 6)[0], struct.unpack_from('<I', h, 8)[0]
        if ver != 4 or feat: red(f'{path}: PAK version {ver}, features {feat:#x}: only version 4 with a plain entry table is known')
        t = f.read(0x30 * n)
    return [(lo | hi << 32, off, csz, usz, attr) for lo, hi, off, csz, usz, attr, _ in struct.iter_unpack('<IIqqqqQ', t)]

def kpka_data(d, e, head=None):
    """An entry's data (head: only its first bytes)"""
    import zlib
    _, off, csz, usz, attr = e; raw = d[off:off + csz]
    if attr & 0xf == 0: return raw[:head] if head else raw
    if attr & 0xf == 1: return zlib.decompressobj(-15).decompress(raw, head or 0)
    if attr & 0xf == 2:
        return zstd_lib.decompress(raw, max_length=head or None)
    red(f'PAK entry {e[0]:016x}: compression {attr & 0xf} is not known')

def kpka_jobs(files, parts=64):
    """The game reads its PAKs as one view: a path in a later patch PAK (re_chunk_000.pak.patch_NNN.pak, in name order after re_chunk_000.pak) replaces the same path hash in earlier ones.
    Only the newest copy of each path is scanned, so shaders of replaced files do not enter the tables. → ([(path, rel, entries), …] in about equal stored sizes, the PAK paths taken)"""
    paks = [(p, r) for p, r in files if p.endswith('.pak') and kpka_entries(p) is not None]
    if not paks: return [], set()
    view = {}
    for p, r in sorted(paks, key=lambda x: x[1]):
        for e in kpka_entries(p): view[e[0]] = (p, r, e)
    jobs, cur, size, step = [], collections.defaultdict(list), 0, sum(e[2] for _, _, e in view.values()) / parts
    for p, r, e in sorted(view.values(), key=lambda x: (x[1], x[2][1])):
        cur[(p, r)].append(e); size += e[2]
        if size >= step: jobs += [(p_, r_, tuple(es)) for (p_, r_), es in cur.items()]; cur, size = collections.defaultdict(list), 0
    jobs += [(p_, r_, tuple(es)) for (p_, r_), es in cur.items()]
    return jobs, {p for p, _ in paks}

def sdf_shaders(d, label):
    """RE Engine shader definition ('SDF\\0', 2,685 entries in Monster Hunter Rise's PAKs): u16 version 1, u16 entry count, u64, then 0x108-byte entries from +0x10. Entry +0x10: six u64 offsets
    (vertex, hull, domain, geometry, pixel, compute; 0 = no stage), each to {u32 control length, u32 code length} followed by the control section and the code area. Compute code has no header.
    The stages of one entry form a program, so its vertex / pixel stages are a pair"""
    ver, n = struct.unpack_from('<HH', d, 4); out = []
    if not ver and not n: return out   # an empty definition: 16 bytes, version 0, no entries
    if ver != 1: red(f'{label}: SDF version {ver}')
    for i in range(n):
        offs = struct.unpack_from('<6Q', d, 0x10 + 0x108 * i + 0x10)
        for st, o in zip(SDF_STAGES, offs):
            if not o: continue
            nctl, ncode = struct.unpack_from('<II', d, o); ctl, code = d[o + 8:o + 8 + nctl], d[o + 8 + nctl:o + 8 + nctl + ncode]
            why = check(label, st, ctl, code)
            if not why and st != 'cs' and sph_stage(code) != st: why = 'the SPH stage does not match the entry slot'
            if why: red(f'{label} SDF entry {i} {st}: {why}')
            out.append((f'{label}#SDF#var{i}', st, ctl, code))
    return out

def kpka_shaders(path, rel, ents, out):
    import mmap
    with romfs_list.map_file(path) as d:
        for e in ents:
            if e[3] >= 0x10 and kpka_data(d, e, 4) == b'SDF\0': out.extend(sdf_shaders(kpka_data(d, e), f'/{rel}#{e[0]:016x}'))

MTARC, MTSPK = b'ARC\0', b'SPK\0'
def mtarc_members(d, label):
    """Capcom's MT Framework archive: 'ARC\\0', u16 version 7, u16 entry count, then one entry per file
    {char path[P] ('\\\\'-separated, no extension), u32 type hash, u32 stored length, u32 length in the low 29 bits (flags above), u32 offset}; the data is a zlib stream, or stored when the
    stored length equals the length. P is 128 (The Great Ace Attorney Chronicles: 447 '.arc' files under nativeNXx64, 0x90-byte entries) or 64 (RESIDENT EVIL 6: 3213 '.arc' files,
    0x50-byte entries; read at 0x90 the sizes were path text and 94161 of 208105 entries failed to inflate). The version does not say which, so P is the one whose every entry is
    self-consistent: every entry's data after the entry table and inside the file (at the wrong width the sizes are path text, far past the end). → (path, data) of each entry"""
    import zlib
    ver, n = struct.unpack_from('<HH', d, 4)
    if ver != 7: red(f'{label}: ARC version {ver}: only version 7 is known')
    def fits(plen):
        table = 8 + (plen + 16) * n
        if table > len(d): return False
        for i in range(n):
            csz, usz, off = struct.unpack_from('<3I', d, 8 + (plen + 16) * i + plen + 4); usz &= 0x1fffffff
            if off < table or off + csz > len(d): return False   # no csz <= usz test: tiny entries inflate (RE6 XfEnvCube: 0x18 stored, 0x10 long)
        return True
    plen = next((p for p in (0x80, 0x40) if fits(p)), None)
    if plen is None: red(f'{label}: ARC version 7 with {n} entries fits neither 128- nor 64-byte paths')
    for i in range(n):
        o = 8 + (plen + 16) * i
        path = d[o:o + plen].split(b'\0')[0].decode('latin-1')
        csz, usz, off = struct.unpack_from('<3I', d, o + plen + 4); usz &= 0x1fffffff
        data = d[off:off + csz] if csz == usz else zlib.decompress(d[off:off + csz])
        if len(data) != usz: red(f'{label}#{path}: {len(data):#x} bytes, the entry says {usz:#x}')
        yield path.replace('\\', '/'), data

def mtspk_shaders(d, label):
    """MT Framework's shader package ('SPK\\0'): after the package's tables the shaders run to the end of the file, each a control section and the code area its +0x704 length names,
    in one of two orders the package keeps throughout (read from its first shader, then checked on every one):
    - code area right before its control section, back to back (The Great Ace Attorney Chronicles: its only package, sc\\NX_64\\root in sa/NX_64/root.arc, 230 vertex then 230 pixel shaders);
    - control section with its code area right after it, each preceded by a few dozen bytes of the shader's own data (RESIDENT EVIL REVELATIONS 2: 88 packages, 2,104-byte control
      sections, 76 to 196 bytes between one code area's end and the next control section).
    The tables that say which vertex and pixel shaders the engine combines are not read, so the shaders are paired as single-stage programs (separable_pairs)"""
    out, end, c = [], None, d.find(CTL_MAGIC)
    if c >= 0:   # the order, from the first shader: its code area right before the control section, or right after it
        nctl, ncode = struct.unpack_from('<I', d, c + 0x20)[0], struct.unpack_from('<I', d, c + 0x704)[0]
        before_ok, after_ok = c >= ncode and d[c - ncode:c - ncode + 4] == GFX_MAGIC, d[c + nctl:c + nctl + 4] == GFX_MAGIC
    after = c >= 0 and after_ok and not before_ok
    while c >= 0:
        nctl, ncode = struct.unpack_from('<I', d, c + 0x20)[0], struct.unpack_from('<I', d, c + 0x704)[0]
        a = c + nctl if after else c - ncode
        if after:
            if d[a:a + 4] != GFX_MAGIC: red(f'{label} SPK control section @{c:#x}: its code area does not start right after it')
        elif a < 0 or d[a:a + 4] != GFX_MAGIC or end is not None and a != end: red(f'{label} SPK control section @{c:#x}: its code area does not end right before it')
        ctl, code = d[c:c + nctl], d[a:a + ncode]
        st = sph_stage(code)
        why = check(label, st, ctl, code) if st else 'no stage in the SPH'
        if why: red(f'{label} SPK @{a:#x}: {why}')
        out.append((f'{label}#MTSPK@{a:#x}', st, ctl, code))
        end = a + ncode if after else c + nctl; c = d.find(CTL_MAGIC, end)
    if not after and end != len(d): red(f'{label} SPK: the last shader ends at {end}, not at the end of the package ({len(d):#x})')
    if after and end is not None and end > len(d): red(f'{label} SPK: the last code area runs past the end of the package ({end:#x} > {len(d):#x})')
    return out

def mtarc_shaders(d, label, out):
    for path, m in mtarc_members(d, label):
        if m[:4] == MTSPK: out.extend(mtspk_shaders(m, f'{label}#{path}'))

SCNE, ZSTD_MAGIC = b'SCNE', bytes.fromhex('28b52ffd')
BIG = 128 << 20   # plain files above this are scanned through mmap (scan_file); the same threshold as graphics/tools/shbake/nxsh_extract.py BIG

class Mapped(mmap.mmap):
    """A read-only map of a big file that the readers take like bytes. A bare mmap differs from bytes silently in two places: `b'BNSH' in m` is False even when
    m holds it (mmap's `in` only matches one byte), and iterating yields 1-byte bytes instead of ints. Slicing, len, indexing, find, re and struct already agree;
    any other bytes method is missing and fails loudly"""
    def __contains__(self, x): return self.find(x) >= 0 if isinstance(x, (bytes, bytearray)) else super().__contains__(x)
    def __iter__(self): return iter(memoryview(self))

class MappedSpan(romfs_list._ImageSpan):
    """镜像里起点不对齐的大文件（romfs_list.map_file 的 _ImageSpan：文件映射、下标平移）：补上 Mapped 的「多字节包含」按内容判与逐字节遍历给整数"""
    def __contains__(self, x): return self.find(x if isinstance(x, (bytes, bytearray)) else bytes([x])) >= 0
    def __iter__(self): return iter(memoryview(self))

def map_big(path):
    """大文件按 Mapped 映射（同 Mapped(open(path).fileno(), 0, ACCESS_READ)）。RomFS 只有镜像 romfs.std（不落散文件，Windows 单一 IMG 出包正是这样）时
    映射镜像里那一段：起点按映射粒度对齐就是镜像上的 Mapped，不对齐就是 MappedSpan（同 romfs_list.map_file，#1439）；都是文件映射、零拷贝，页走系统文件缓存。
    原先这里拷进同长的匿名映射：大乱斗特别版 RomFS 那个约 16 GB 的单文件一拷就是 16 GB，15 GB 内存的 Windows 机在「直连抽取 0/114」卡死、整机换页（#1560）。
    Python 3.12 以下没有 __buffer__（PEP 688），不对齐时正则 / struct 拿不到平移后的内容，才退回拷贝"""
    try:
        with open(path, 'rb') as f: return Mapped(f.fileno(), 0, access=mmap.ACCESS_READ)
    except FileNotFoundError:
        pass
    hit = romfs_list._in_image(path)
    if hit is not None and hit[1] in hit[0]:
        im, rel = hit; start, size = im.span(rel); delta = start % mmap.ALLOCATIONGRANULARITY
        if size and (delta == 0 or sys.version_info >= (3, 12)):
            with open(im.path, 'rb') as g:
                if delta == 0: return Mapped(g.fileno(), size, offset=start, access=mmap.ACCESS_READ)
                return MappedSpan(mmap.mmap(g.fileno(), size + delta, offset=start - delta, access=mmap.ACCESS_READ), delta, size)
    n = romfs_list.file_size(path); m = Mapped(-1, n)
    with romfs_list.open_file(path) as f:
        view, at = memoryview(m), 0
        try:
            while at < n:
                got = f.readinto(view[at:at + (8 << 20)])
                if not got: raise OSError(f'{path}：从 RomFS 镜像读短了（{at} / {n} 字节）')
                at += got
        finally:
            view.release()
    return m


def bea_members(d, label):
    """NintendoWare Bezel Engine archive ('.bea', magic 'SCNE', a NintendoWare binary file of version 0x10100; Big Brain Academy: Brain vs. Brain keeps nearly all of RomFS in 603 of them):
    +0x20 u64 entry count, +0x28 u64 offset of the entry pointer array (u64 each), each pointing at a 0x30-byte 'ASST' entry {+0x10 u16 kind, u16 alignment shift, u32 stored length,
    u32 length, +0x20 u64 data offset, +0x28 u64 name offset (u16 length + the path)}. Every entry of the game's 7,665 is one zstd frame (kind 2, and 258 for the one sound project),
    even the two whose stored length equals their length, so the frame magic decides.
    File version 0x0600 (header +0x08; Super Mario Party Jamboree, all 949 of its archives): +0x28 is the first entry itself, the entries one after another, each 'ASST' + u32 size
    (0x48) {+0x10 u16 kind, u16 alignment shift, u32 stored length, u32 length, +0x1c 8-byte tag, +0x28 16-byte hash, +0x38 u64 data offset, +0x40 u64 name offset}.
    → (path, data) of each entry"""
    n, arr = struct.unpack_from('<QQ', d, 0x20)
    inline = n and d[arr:arr + 4] == b'ASST'
    for i in range(n):
        if inline: o = arr + i * struct.unpack_from('<I', d, arr + 4)[0]
        else: o = struct.unpack_from('<Q', d, arr + 8 * i)[0]
        if d[o:o + 4] != b'ASST': red(f'{label}: entry {i} @{o:#x} is not an ASST entry')
        if inline: (csz, usz), (off, nm) = struct.unpack_from('<II', d, o + 0x14), struct.unpack_from('<QQ', d, o + 0x38)
        else: csz, usz, off, nm = struct.unpack_from('<II4xQQ', d, o + 0x14)
        path = d[nm + 2:nm + 2 + struct.unpack_from('<H', d, nm)[0]].decode('utf-8', 'replace')
        data = d[off:off + csz]
        if data[:4] == ZSTD_MAGIC:
            data = zstd_lib.decompress(data)
        if len(data) != usz: red(f'{label}/{path}: {len(data):#x} bytes, the entry says {usz:#x}')
        yield path, data

def falcom_pkg(d):
    """Nihon Falcom's package ('.pkg', no magic). The Legend of Heroes: Hajimari no Kiseki (010053D014C44000, 11-19) keeps its RomFS in 16,884 of them and every shader it draws
    with in 16 under shaders/, one BNSH per entry, 148,359 in all; read as plain files they gave none, the offline library held the 4 shaders of main and each of the others was
    compiled on the device at its first use. Layout: u32 0, u32 entry count n, n × 0x50-byte entries {+0 name, 64 bytes padded with zeros, +0x40 u32 length, u32 stored length,
    u32 offset from the file start, u32 flags}, then the data, the first entry's right after the table (true of all 16,884). Flags seen: 1 = an LZ4 block (295,041 entries, each
    decompresses to its exact length); 9 with stored length 0 = data kept outside the package (59,669 textures).
    → [(name, length, stored length, offset, flags)] when d is such a package (the table decides: the first data offset is the table's end and every entry lies inside the file), else None"""
    if len(d) < 8 + 0x50: return None
    zero, n = struct.unpack_from('<II', d, 0)
    if zero or not n or 8 + 0x50 * n > len(d): return None
    if struct.unpack_from('<I', d, 8 + 0x48)[0] != 8 + 0x50 * n: return None   # before the table is read: a plain file that starts with a zero word answers here, whatever its n
    es = []
    for o in range(8, 8 + 0x50 * n, 0x50):
        usz, csz, off, fl = struct.unpack_from('<4I', d, o + 0x40)
        if off + csz > len(d): return None
        es.append((bytes(d[o:o + 0x40]).split(b'\0')[0].decode('utf-8', 'replace'), usz, csz, off, fl))
    return es

def falcom_pkg_shaders(d, entries, label, out):
    """Each entry of a Falcom package (falcom_pkg) scanned as a file of its own. Flags 1 = LZ4 block; 0 with equal lengths = stored as it is (not seen in the game above, the sizes say so);
    an entry with nothing stored is not here to read. Any other flags with data: that entry is left out and the package gets one [WARN] line (never silently)"""
    import lz4.block
    other = collections.Counter()
    for name, usz, csz, off, fl in entries:
        if not csz: continue
        if fl == 1:
            if usz > 255 * csz: red(f'{label}/{name}: {usz:#x} bytes from {csz:#x} stored is more than an LZ4 block can hold (255 to 1)')   # checked first: the decoder allocates the stated length before it reads the block
            try: m = lz4.block.decompress(d[off:off + csz], uncompressed_size=usz)
            except lz4.block.LZ4BlockError as e: red(f'{label}/{name}: its {csz:#x} stored bytes are not an LZ4 block ({e})')
            if len(m) != usz: red(f'{label}/{name}: {len(m):#x} bytes, the entry says {usz:#x}')
        elif fl == 0 and csz == usz: m = d[off:off + csz]
        else: other[fl] += 1; continue
        scan_blob(m, f'{label}/{name}', out)
    if other: print(f'[WARN] {label}: {sum(other.values())} entries not read, their storage flags (' + ', '.join(f'{f:#x} × {n}' for f, n in sorted(other.items())) + ') are not ones this reader knows (1 = LZ4, 0 = stored)', flush=True)

NLG_DICT = bytes.fromhex('5824f3a9')
def nlg_dict(d):
    """Next Level Games' archive index (Luigi's Mansion 3: '<name>.dict' beside '<name>.data', every RomFS file but the Wwise banks): u32 magic 0xa9f32458, u8 4, u8 1, u8 compressed,
    u8 0, u32 the largest stored chunk, u8 chunk count, u8 record count, u8 name count, u8 0; then one 0x18-byte record per variant {u32 hash, u16 top-level entry count of its
    table, u16, u8 chunk of each of its 16 sections (0 = none)}, one 0x10-byte chunk per piece of data {u32 offset, u32 length, u32 stored length (one zlib stream), u16 alignment
    (log2 in the low 5 bits), u8 file, u8 GPU memory}, and the file names ('.data', '.debug', '.nxpc'). Record v's own entry table is chunk v: 12 bytes per entry {u32 kind (bit 31: a
    list of w1 entries from entry w2; bits 27..30 section; bits 22..26 log2 alignment), u32 length, u32 offset in the section}. The game reads the archive in main+0x1ba00 (Luigi's
    Mansion 3 1.4.0). → (compressed, records [(top, chunks)], chunks [(offset, length, stored, file)], file names)"""
    comp, nchunk, nrec, nname = d[6], d[0xc], d[0xd], d[0xe]
    recs = [(struct.unpack_from('<H', d, 0x14 + 0x18 * i)[0], list(d[0x18 + 0x18 * i:0x28 + 0x18 * i])) for i in range(nrec)]
    co = 0x10 + 0x18 * nrec
    chunks = [struct.unpack_from('<3I2xB', d, co + 0x10 * i) for i in range(nchunk)]
    names = d[co + 0x10 * nchunk:].split(b'\0')[:nname]
    return comp, recs, chunks, [n.decode('latin-1') for n in names]

def nlg_walk(tab, start, count, out):
    """The plain entries of a variant's table in the game's order (lists inline, depth first; main+0x1cbd0)"""
    for i in range(start, start + count):
        w0, w1, w2 = struct.unpack_from('<3I', tab, 12 * i)
        if w0 >> 31: nlg_walk(tab, w2, w1, out)
        else: out.append(i)

def nlg_patch_section(tab, order, s, old, body, cut, end, size):
    """An update's section s, rebuilt as the game does after loading the old one (main+0x1c8d0 and main+0x1cbd0, Luigi's Mansion 3 1.4.0): the entries of section s in table order are packed
    from the start of the section at their alignment, each taking its data from the old section (offsets below its length) or from the update's body (the rest, counted from the old length),
    and the entry's offset becomes the new one. The game does it in place and keeps a copy of old[cut:end] for the data the packing overwrites before it is read; this does the same"""
    buf = bytearray(max(len(old), size)); buf[:len(old)] = old
    keep, cur = bytes(old[cut:end]) if end else b'', 0
    for i in order:
        w0, n, src = struct.unpack_from('<3I', tab, 12 * i)
        if (w0 >> 27) & 0xf != s: continue
        al = 1 << ((w0 >> 22) & 0x1f) if w0 & 0x7c00000 else 1
        cur = (cur + al - 1) & -al
        if src >= len(old): buf[cur:cur + n] = body[src - len(old):src - len(old) + n]
        elif end and cut <= src < end: buf[cur:cur + n] = keep[src - cut:src - cut + n]
        elif src != cur: buf[cur:cur + n] = bytes(buf[src:src + n])
        struct.pack_into('<I', tab, 12 * i + 8, cur); cur += n
    return bytes(buf[:size])

NLG4 = bytes([4, 4, 3, 0])   # the archive header bytes 4..7 of the newer layout (Mario Strikers: Battle League; Luigi's Mansion 3 has 4, 1, <compressed>, 0)

def nlg4_dict(d, label):
    """Next Level Games' archive index, newer layout (Mario Strikers: Battle League 1.3.2, all 879 .dict files): u32 magic 0xa9f32458, bytes 4 4 3 0, 32 bytes of hashes, u32 the length
    of the compressed index (the file length - 0x40), u32 top-level entry count, u32 entry count, u32 frame count, u32 0, u8 section count, u8 name count, 2 bytes; the index is one zstd
    frame from 0x40 to the end: per section 28 bytes {u32 offset in its file, stored length, inflated length, first frame, frame count, i32, u8 alignment, u8 file (1-based, in name
    order), u8 flags, u8 0}, per entry 12 bytes {u32 kind (bits 24..31 the section, bit 23 a list), u32 length (a list: its entry count), u32 offset in the section (a list: its first
    entry)}, per top-level entry 8 bytes (hashes), per frame 8 bytes {stored, inflated} (a section is its frames back to back, each one zstd frame), then the file names ('data', 'nxpc',
    'debug'). Every byte is accounted for (checked on all 879 files). → (sections, entries, top-level count, names)"""
    zlen, ntop, nent, nfr = struct.unpack_from('<4I', d, 0x28); nsec = d[0x3c]
    if zlen != len(d) - 0x40: red(f'{label}: the index says {zlen:#x} compressed bytes, the file holds {len(d) - 0x40:#x}')
    x = zstd_lib.decompress(d[0x40:]); o = 28 * nsec + 12 * nent
    secs = [struct.unpack_from('<5Ii4B', x, 28 * i) for i in range(nsec)]
    ents = [struct.unpack_from('<3I', x, 28 * nsec + 12 * i) for i in range(nent)]
    frames = [struct.unpack_from('<2I', x, o + 8 * ntop + 8 * i) for i in range(nfr)]
    names = x[o + 8 * ntop + 8 * nfr:].split(b'\0')
    if names[-1] != b'': red(f'{label}: the index does not end with its file names')
    for s, (off, stored, n, f0, fc, *_) in enumerate(secs):
        fr = frames[f0:f0 + fc]
        if sum(a for a, _ in fr) != stored or sum(b for _, b in fr) != n: red(f'{label}: section {s} frames add up to {sum(a for a, _ in fr):#x} / {sum(b for _, b in fr):#x}, the section says {stored:#x} / {n:#x}')
    return secs, ents, ntop, frames, [m.decode('latin-1') for m in names[:-1]]

def nlg4_patch(p, nsec, label):
    """An update's '<name>.patch' beside an archive of the newer layout: u32 magic, bytes 4 5 3 0, u32 inflated body length, u32 top-level entry count, u32 entry count, u32 section count,
    then one zstd frame from 0x18 to the end: the new entry table (it replaces the archive's), 8 bytes per top-level entry, 20 bytes per section, then the new data — every entry whose
    offset is at least its section's old inflated length takes its bytes from here at (offset - old length); the others still read the archive's section (checked on all 48 .patch files
    of Mario Strikers 1.3.2). Offset 0xffffffff is an entry of a file that is not shipped. → (entries, top-level count, new data)"""
    blen, ntop, nent, ns = struct.unpack_from('<4I', p, 8)
    if ns != nsec: red(f'{label}: the patch is for {ns} sections, the archive has {nsec}')
    b = zstd_lib.decompress(p[0x18:])
    if len(b) != blen: red(f'{label}: the patch body inflates to {len(b):#x} bytes, its header says {blen:#x}')
    return [struct.unpack_from('<3I', b, 12 * i) for i in range(nent)], ntop, b[12 * nent + 8 * ntop + 20 * ns:]

def nlg4_shaders(path, d, label, out):
    """Shaders of an archive of the newer layout (nlg4_dict, with its update nlg4_patch): each program is a list entry of type 0x8db400 whose children name its code area (type 0x0db401)
    and control section (type 0x0db402), one pair per file it ships for ('data' here; the 'nxpc' pair points into a file that is not shipped). The pair is the one the list names, not
    two sections read back to back. Mario Strikers 1.3.2: init holds all 3,149 (2,819 control sections and 709 code areas from the update, the rest from the archive)"""
    stem = path[:-len('.dict')]; secs, ents, ntop, frames, names = nlg4_dict(d, label)
    files = {i + 1: romfs_list.open_file(f'{stem}.{nm}').read() for i, nm in enumerate(names) if romfs_list.exists(f'{stem}.{nm}')}
    patch = nlg4_patch(romfs_list.open_file(stem + '.patch').read(), len(secs), label) if romfs_list.exists(stem + '.patch') else None
    if patch: ents, ntop, blob = patch
    cache = {}
    def section(s):
        if s not in cache:
            off, stored, n, f0, fc, _, _, fi, _, _ = secs[s]
            if fi not in files: cache[s] = None
            else:
                parts, p = [], off
                for cs, us in frames[f0:f0 + fc]: parts.append(zstd_lib.decompress(files[fi][p:p + cs])); p += cs
                cache[s] = b''.join(parts)
                if len(cache[s]) != n: red(f'{label}: section {s} inflates to {len(cache[s]):#x} bytes, the index says {n:#x}')
        return cache[s]
    def data(e):
        k, n, off = e; s = k >> 24; old = secs[s][2]
        if patch and off >= old: return None if off == 0xffffffff else blob[off - old:off - old + n]
        sec = section(s); return None if sec is None else sec[off:off + n]
    progs, stack = [], [(0, ntop)]
    while stack:   # the table in order, lists depth first (a program list is one program; other lists hold more entries)
        s0, n = stack.pop()
        for i in range(s0 + n - 1, s0 - 1, -1):
            k, a, b = ents[i]
            if k & 0xffffff == 0x8db400: progs.append(i)
            elif k & 0x800000: stack.append((b, a))
    for i in sorted(progs):
        k, a, b = ents[i]; ctl = code = None
        for j in range(b, b + a):
            t, got = ents[j][0] & 0xffffff, data(ents[j])
            if got is None: continue
            if t == 0x0db402 and ctl is None: ctl = got
            elif t == 0x0db401 and code is None: code = got
        if ctl is None or code is None: red(f'{label}: program entry {i} has no shipped control section and code area')
        st = glslc_stage(ctl, code)
        if not st or check(label, st, ctl, code): red(f'{label}: program entry {i}: the control section and code area the list names do not form a shader')
        out.append((f'{label}#NLG4@{i}', st, ctl, code))

def nlg_shaders(path, label, out):
    """Shaders of a Next Level Games archive (nlg_dict). Luigi's Mansion 3 keeps all 5,149 of the base game (5,317 in update 1.4.0) in init: section 8 of each variant starts with the control
    sections back to back (0x850 bytes each), section 7 with their code areas in the same order (a compute code area without a header), and the engine makes one program per stage. An update
    ships '<name>.patch' beside the unchanged archive: u64 the variants it patches (bit = record), u32 offset, stored and inflated length of its body (one zlib stream: 16 u32 section
    offsets, 16 u16 alignments, then each section's new data), then per patched variant (in record order) 0xd0 bytes {u32 offset, stored length and entry count of its new table (one zlib
    stream), u16 top-level entry count, u16, u32 new length of each of 16 sections, 16 u32 end and 16 u32 start of the old data the packing overwrites before reading it} (main+0x1bb60).
    Every variant whose files are in RomFS is read (the 13 even records; the odd ones point into '.nxpc', which is not shipped), and the shaders found pair by order (checked one by one)"""
    import zlib
    d = romfs_list.open_file(path).read(); stem = path[:-len('.dict')]
    if d[4:8] == NLG4: return nlg4_shaders(path, d, label, out)   # the newer layout (nlg4_dict)
    comp, recs, chunks, names = nlg_dict(d)
    files = {i: romfs_list.open_file(stem + nm).read() for i, nm in enumerate(names) if romfs_list.exists(stem + nm)}
    patch = romfs_list.open_file(stem + '.patch').read() if romfs_list.exists(stem + '.patch') else None
    cache = {}
    def chunk(ci):
        if ci not in cache:
            off, n, stored, f = chunks[ci]
            raw = files[f][off:off + (stored if comp else n)]   # an archive stored as is keeps 0 there (Movies, flow)
            cache[ci] = zlib.decompressobj().decompress(raw) if comp else raw[:n]
            if len(cache[ci]) != n: red(f'{label}: chunk {ci} inflates to {len(cache[ci]):#x} bytes, the index says {n:#x}')
        return cache[ci]
    mask, body = 0, b''
    if patch:
        mask, (boff, bstored, bn) = struct.unpack_from('<Q', patch)[0], struct.unpack_from('<3I', patch, 8)
        body = zlib.decompress(patch[boff:boff + bstored])
        if len(body) != bn: red(f'{label}: the patch body inflates to {len(body):#x} bytes, its header says {bn:#x}')
        bsec = struct.unpack_from('<16I', body)
    for v, (top, secs) in enumerate(recs):
        if any(chunks[ci][3] not in files for ci in [v] + [c for c in secs if c]): continue   # a variant of a file the game does not ship
        if mask >> v & 1:
            r = struct.unpack_from('<4I48I', patch, 0x18 + 0xd0 * bin(mask & ((1 << v) - 1)).count('1'))
            tab = bytearray(zlib.decompress(patch[r[0]:r[0] + r[1]]))
            if len(tab) != 12 * r[2]: red(f'{label}: variant {v} patch table has {len(tab)} bytes for {r[2]} entries')
            order = []; nlg_walk(tab, 0, r[3] & 0xffff, order)
            sec = {}
            def section(s):
                if s not in sec:
                    old = chunk(secs[s]) if secs[s] else b''
                    sec[s] = nlg_patch_section(tab, order, s, old, body[bsec[s]:bsec[s + 1] if s < 15 else len(body)], r[36 + s], r[20 + s], r[4 + s])
                return sec[s]
            live = [s for s in range(16) if r[4 + s]]
        else:
            section = lambda s: chunk(secs[s]); live = [s for s in range(16) if secs[s]]
        for c in [s for s in live if section(s)[:4] == CTL_MAGIC]:
            ctls = section(c); n = struct.unpack_from('<I', ctls, 0x20)[0]
            for k in live:   # the section whose data pairs with every control section in turn: code areas back to back from its start
                if k == c: continue
                code, found, p, i = section(k), [], 0, 0
                while ctls[i * n:i * n + 4] == CTL_MAGIC:
                    ctl = ctls[i * n:(i + 1) * n]; ln = struct.unpack_from('<I', ctl, 0x704)[0]; cd = code[p:p + ln]
                    st = glslc_stage(ctl, cd)
                    if not st or check(label, st, ctl, cd): found = None; break
                    found.append((f'{label}#NLG{v}:{c}@{i * n:#x}', st, ctl, cd)); p += ln; i += 1
                if found: out.extend(found); break
            else: red(f'{label}: variant {v} section {c} holds control sections, but no section of the variant holds their code areas in order')

BHD5, DCX, BND3 = b'BHD5', b'DCX\0', b'BND3'
def bhd5_entries(path):
    """FromSoftware's dvdbnd archive (Dark Souls Remastered: dvdbnd0.bdt and dvdbnd1.bdt, 3.6 GB, starting 'BDF3'), indexed by the .bhd5 file beside it: 'BHD5', 0xff (little-endian), u32 1, u32 file size,
    u32 bucket count, u32 0, u64 bucket table offset; each 16-byte bucket {u32 count, u32, u64 offset} points at count 16-byte entries {u32 path hash, u32 size, u64 offset in the .bdt}.
    The index holds only path hashes, no names. → [(hash, offset, size)], or None when there is no index beside the file"""
    idx = path[:-4] + '.bhd5'
    if not path.endswith('.bdt') or not romfs_list.exists(idx): return None
    with romfs_list.open_file(idx) as f: d = f.read()
    if d[:5] != BHD5 + b'\xff': red(f'{idx}: not a little-endian BHD5 index')
    nb, tab = struct.unpack_from('<I4xQ', d, 0x10)
    out = []
    for i in range(nb):
        n, _, at = struct.unpack_from('<IIQ', d, tab + 16 * i)
        out += [(h, off, size) for h, size, off in struct.iter_unpack('<IIQ', d[at:at + 16 * n])]
    return out

def bdt_jobs(files, parts=64):
    """The entries of every dvdbnd archive, in ranges of about equal stored size, one scan job each. → ([(path, rel, entries), …], the .bdt paths taken)"""
    jobs, taken = [], set()
    for p, r in files:
        ents = bhd5_entries(p)
        if ents is None: continue
        taken.add(p); ents.sort(key=lambda e: e[1]); step, cur, size = sum(e[2] for e in ents) / parts, [], 0
        for e in ents:
            cur.append(e); size += e[2]
            if size >= step: jobs.append((p, r, tuple(cur))); cur, size = [], 0
        if cur: jobs.append((p, r, tuple(cur)))
    return jobs, taken

def undcx(d, label):
    """FromSoftware's DCX container (big-endian): 'DCX\\0', +0x18 'DCS\\0' u32 length, u32 stored length, +0x24 'DCP\\0' + the method, +0x44 'DCA\\0' u32 8, then the stream. Dark Souls Remastered
    uses only DFLT (a zlib stream) on all 5,184 of its DCX entries"""
    import zlib
    n, csz = struct.unpack_from('>II', d, 0x1c)
    if d[0x28:0x2c] != b'DFLT' or d[0x44:0x48] != b'DCA\0': red(f'{label}: DCX method {d[0x28:0x2c]}: only DFLT is known')
    out = zlib.decompress(d[0x4c:0x4c + csz])
    if len(out) != n: red(f'{label}: DCX holds {len(out):#x} bytes, its header says {n:#x}')
    return out

def bnd3_members(d, label):
    """FromSoftware's BND3 archive: 'BND3', 8 bytes of version, u8 format, u8 big-endian, u8 bit-big-endian, u8 0, i32 count, i32 header end, 8 bytes 0, then count 28-byte entries {u8 flags, 3 bytes 0,
    i32 length, u64 offset, i32 ID, u32 name offset, i32 unpacked length} (the formats Dark Souls Remastered uses, 0x5c and 0x7c: 64-bit offsets, IDs, names and the unpacked length). Names are
    Shift JIS paths ('N:\\FRPG\\Source\\Shader\\…') or base names. → (name, data) of each entry"""
    if d[12] not in (0x5c, 0x7c) or d[13]: red(f'{label}: BND3 format {d[12]:#x}, big-endian {d[13]}: only formats 0x5c and 0x7c, little-endian, are known')
    for i in range(struct.unpack_from('<i', d, 0x10)[0]):
        _, n, off, _, nm, usz = struct.unpack_from('<B3xiQiIi', d, 0x20 + 28 * i)
        if n != usz: red(f'{label}: BND3 entry {i} is compressed ({n:#x} of {usz:#x} bytes)')
        name = d[nm:d.index(b'\0', nm)].decode('shift-jis', 'replace').replace('\\', '/')
        yield name.split('/Source/', 1)[-1], d[off:off + n]

def bdt_shaders(path, rel, ents, out):
    """Dark Souls Remastered's shaders: GLSLC outputs, one per BND3 entry ('.vpo' vertex, '.fpo' pixel; 3,225 in six DCX-compressed BND3 archives in dvdbnd0.bdt, 3,088 of them the
    FLVER material shaders). The engine combines vertex and pixel shaders per draw, so they are paired as single-stage programs (separable_pairs)"""
    import mmap
    with romfs_list.map_file(path) as m:
        for h, off, n in ents:
            label = f'/{rel}#{h:08x}'; d = m[off:off + n]
            if d[:4] == DCX: d = undcx(d, label)
            if d[:4] != BND3 or b'\x91\x68\x86\x19' not in d: continue   # the game's other BND3 archives use other formats (0x78) and hold no shaders
            for nm, b in bnd3_members(d, label):
                if b[:4] == b'\x91\x68\x86\x19': out.extend(glslc_shaders(b, f'{label}/{nm}'))

class _Rd:   # a little-endian reader over bytes, with .NET's BinaryReader strings (7-bit encoded length + UTF-8)
    def __init__(s, d, p): s.d, s.p = d, p
    def u8(s): s.p += 1; return s.d[s.p - 1]
    def un(s, f): v = struct.unpack_from(f, s.d, s.p); s.p += struct.calcsize(f); return v[0] if len(v) == 1 else v
    def raw(s, n): s.p += n; return s.d[s.p - n:s.p]
    def str(s):
        n = sh = 0
        while True:
            b = s.u8(); n |= (b & 0x7f) << sh; sh += 7
            if b < 0x80: return s.raw(n)

MGFX = b'MGFX'
def mgfx_effect(d, p, label):
    """A MonoGame effect at d[p:] (Celeste: MonoGame for the Switch, its C# compiled to C++ by Sickhead's BRUTE; 13 in Content/Effects/*.xnb, whose XNB 'S' files are read by EffectReader,
    and SpriteBatch's in main): 'MGFX', u8 version 8, u8 profile, u32 effect key; the constant buffers {name, i16 size, u8 count, {u8 parameter, u16 offset} each}; the shaders {u8 vertex (1) or
    pixel (0), u32 length, the bytecode, u8 sampler count, {u8 type, u8 texture slot, u8 sampler slot, u8 has state (then 20 bytes of state), name, u8 parameter} each, u8 count + constant buffer
    indices, u8 count + attributes {name, u8 usage, u8 index, i16 location}}; the bytecode is u32 control length, the control section, u32 code length, the code area. Then the parameters and the
    techniques and passes, read to the end so that a misread shows: a pass names its vertex and pixel shader by index, 255 for none. → [(label, stage, ctl, code)], or None when the header is not
    version 8's or its first shader is not an NVN one (four bytes 'MGFX' anywhere in a main)"""
    if d[p + 4:p + 5] != b'\x08': return None
    r = _Rd(d, p + 10)
    try:
        for _ in range(r.u8()): r.str(); r.raw(2); r.raw(3 * r.u8())
        if d[r.p + 10:r.p + 14] != CTL_MAGIC: return None   # shader count, then the first shader's stage, length and control length
    except IndexError: return None
    def no_annotations():
        if r.u8(): red(f'{label} MGFX @{p:#x}: annotations (MonoGame writes none)')
    out = []
    for i in range(r.u8()):
        vs, n = r.u8(), r.un('<I'); bc = r.raw(n)
        nctl = struct.unpack_from('<I', bc, 0)[0]; ncode = struct.unpack_from('<I', bc, 4 + nctl)[0] if 8 + nctl <= n else -1
        if vs > 1 or 8 + nctl + ncode != n: red(f'{label} MGFX @{p:#x} shader {i}: {n:#x} bytes, not a control section and a code area')
        ctl, code = bc[4:4 + nctl], bc[8 + nctl:]
        st = sph_stage(code) if code[:4] == GFX_MAGIC else None
        why = check(label, st, ctl, code) if st == ('ps', 'vs')[vs] else f'the SPH stage is {st}, the effect says {("pixel", "vertex")[vs]}'
        if why: red(f'{label} MGFX @{p:#x} shader {i}: {why}')
        out.append((f'{label}#MGFX@{p:#x}#{i}', st, ctl, code))
        for _ in range(r.u8()):
            r.raw(3)
            if r.u8(): r.raw(20)   # address u / v / w, border colour, filter, max anisotropy, max mip level, mip LOD bias
            r.str(); r.u8()
        r.raw(r.u8())
        for _ in range(r.u8()): r.str(); r.raw(4)
    def params():   # {u8 class, u8 type, name, semantic, annotations, u8 rows, u8 columns, elements, struct members, the value when it has neither} each → how many
        n = r.u8()
        for _ in range(n):
            typ = r.raw(2)[1]; r.str(); r.str(); no_annotations()
            rows, cols = r.u8(), r.u8()
            if params() + params(): continue
            if typ in (1, 2, 3): r.raw(4 * rows * cols)   # bool, int32, single
            elif typ == 4: r.str()
        return n
    params()
    for _ in range(r.u8()):   # techniques
        r.str(); no_annotations()
        for _ in range(r.u8()):   # passes: name, annotations, vertex / pixel shader index, then the blend, depth-stencil and rasterizer states when present
            r.str(); no_annotations(); v, f = r.u8(), r.u8()
            if v != 255 and v >= len(out) or f != 255 and f >= len(out): red(f'{label} MGFX @{p:#x}: a pass names shader {v} / {f} of {len(out)}')
            for n in (18, 25, 12):
                if r.u8(): r.raw(n)
    if r.p > len(d) or label.endswith('.xnb') and r.p != len(d): red(f'{label} MGFX @{p:#x}: the effect ends at {r.p:#x}, the file at {len(d):#x}')
    return out

def mgfx_shaders(d, label):
    out = []
    for m in re.finditer(MGFX, d): out += mgfx_effect(d, m.start(), label) or []
    if label.endswith('.xnb') and b'EffectReader' in d[:0x200] and not out: red(f'{label}: an XNB effect without an MGFX version 8 effect of NVN shaders')
    return out

ONEPACK = b'ONEPACK\0'
def fb_field(d, t, i):
    """FlatBuffers: absolute offset of field i of the table at t, None when the field is absent"""
    vt = t - struct.unpack_from('<i', d, t)[0]
    if 4 + 2 * i >= struct.unpack_from('<H', d, vt)[0]: return None
    off = struct.unpack_from('<H', d, vt + 4 + 2 * i)[0]
    return t + off if off else None

def fb_vec(d, t, i):
    """FlatBuffers: (start, count) of the vector in field i"""
    p = fb_field(d, t, i)
    if p is None: return 0, 0
    p += struct.unpack_from('<I', d, p)[0]
    return p + 4, struct.unpack_from('<I', d, p)[0]

def fb_refs(d, t, i):   # the tables / strings a vector of offsets points at
    st, n = fb_vec(d, t, i)
    return [st + 4 * k + struct.unpack_from('<I', d, st + 4 * k)[0] for k in range(n)]

def fb_root(d, at=0): return at + struct.unpack_from('<I', d, at)[0]

def trpfs_jobs(files, parts=64):
    """Game Freak's Trinity engine (Pokémon Legends: Z-A): almost everything is in arc/data.trpfs, packs of files, with arc/data.trpfd as their index, both FlatBuffers. The descriptor's root table:
    0 file hashes [u64], 1 pack names [string], 2 file infos [{0 pack index u64}] (in file hash order), 3 pack infos [{0 pack size u64, 1 file count u64}]. The file system: 'ONEPACK\\0', u64 offset of its
    root table {0 pack name hashes [u64], 1 pack offsets [u64]}, each pack a table of its own at its offset. A pack is named through its first file's hash (its own name hash is not a plain FNV-1a of the
    descriptor's name). → ([(path, rel, ((pack offset, pack name), …)), …] in about equal sizes, the paths taken)"""
    import mmap
    jobs, taken = [], set()
    for p, r in files:
        if not p.endswith('.trpfs') or not romfs_list.exists(p[:-1] + 'd'): continue
        with romfs_list.open_file(p) as f:
            if f.read(8) != ONEPACK: continue
        fd = romfs_list.open_file(p[:-1] + 'd').read(); fr = fb_root(fd)
        hashes = struct.unpack_from(f'<{fb_vec(fd, fr, 0)[1]}Q', fd, fb_vec(fd, fr, 0)[0])
        names = [fd[s + 4:s + 4 + struct.unpack_from('<I', fd, s)[0]].decode() for s in fb_refs(fd, fr, 1)]
        pack_of = {h: struct.unpack_from('<Q', fd, fb_field(fd, t, 0))[0] if fb_field(fd, t, 0) is not None else 0 for h, t in zip(hashes, fb_refs(fd, fr, 2))}
        sizes = [struct.unpack_from('<Q', fd, fb_field(fd, t, 0))[0] for t in fb_refs(fd, fr, 3)]
        with romfs_list.map_file(p) as d:
            sr = fb_root(d, struct.unpack_from('<Q', d, 8)[0]); st, n = fb_vec(d, sr, 1)
            packs = []
            for off in struct.unpack_from(f'<{n}Q', d, st):
                pr = fb_root(d, off); h0 = fb_vec(d, pr, 0)
                if not h0[1] or struct.unpack_from('<Q', d, h0[0])[0] not in pack_of: red(f'{r}: the pack at {off:#x} has no file the descriptor lists')
                i = pack_of[struct.unpack_from('<Q', d, h0[0])[0]]; packs.append((off, names[i], sizes[i]))
        cur, size, step = [], 0, sum(s for _, _, s in packs) / parts
        for off, name, s in sorted(packs):
            cur.append((off, name)); size += s
            if size >= step: jobs.append((p, r, tuple(cur))); cur, size = [], 0
        if cur: jobs.append((p, r, tuple(cur)))
        taken |= {p, p[:-1] + 'd'}
    return jobs, taken

# The game's decoder defaults (11-07 DRAGON QUEST XI S, packq on a private root: extract.py sets neither variable, so the LZ5 paks went red here while pack/ue_pak.py and
#   nxsh_extract found the same host): uepak.oodle_host_find, the one lookup the three readers share (DM_OODLE_HOST else this tree's build/s2arm-host; DM_OODLE_MODULES else the
#   RomFS's parent, the product root holding main.fixups.tsv). Set in the environment so the worker processes (spawned) see them; the reason is kept for the red line when not found
def oodle_defaults(romfs):
    if os.environ.get('DM_OODLE_HOST') and os.environ.get('DM_OODLE_MODULES'): return
    got, why = uepak.oodle_host_find(near=romfs)
    if got: os.environ.setdefault('DM_OODLE_HOST', got[0]); os.environ.setdefault('DM_OODLE_MODULES', got[1])
    else: os.environ['S2A_OODLE_WHY'] = why

_OODLE = None
def oodle(comp, n, need=True):
    """The game's own decoder: OodleLZ_Decompress (Trinity's packs are Oodle Mermaid, and Oodle is linked into main), or LZ5_decompress_safe (DRAGON QUEST XI S's legacy Unreal paks):
    one Mac host per worker process in its tool mode (uepak.OodleHost, the one place that starts it), fed over a pipe. DM_OODLE_HOST = the host, DM_OODLE_MODULES = the rewritten
    modules it loads (the NSP script sets both for a game with Trinity packs or legacy Unreal paks, profile_guess.py shader_sources).
    need=False: a stream the decoder rejects → None (the host answers 0 and keeps serving); the host failing is red either way"""
    global _OODLE
    if _OODLE is None:
        host, mods = os.environ.get('DM_OODLE_HOST'), os.environ.get('DM_OODLE_MODULES')
        if not host or not mods: red('Oodle- or LZ5-compressed game files: ' + os.environ.get('S2A_OODLE_WHY', '') + ' — set DM_OODLE_HOST (the Mac host, make -C dm/host GAME=<id> dmhost) and DM_OODLE_MODULES (the rewritten modules) to decode them with the game\'s own decoder')
        _OODLE = uepak.OodleHost(host, mods)
    try: out = _OODLE.decode(comp, n)
    except ValueError as e: red(str(e))
    if out is None and need: red(f'the game\'s decoder in the Mac host rejected a {len(comp)} → {n} byte stream; ---- host output ----\n' + _OODLE.output())
    return out

def trinity_shaders(path, rel, packs, out):
    """A range of Trinity packs (trpfs_jobs). Pack table: 0 file hashes [u64], 1 files [{1 compression i8 (3 = Oodle; stored otherwise), 2 level u8, 3 length u64, 4 data [u8]}]; each file is scanned like a loose one"""
    import mmap
    with romfs_list.map_file(path) as d:
        for off, name in packs:
            pr = fb_root(d, off); hs = fb_vec(d, pr, 0)
            for k, t in enumerate(fb_refs(d, pr, 1)):
                c, ln = fb_field(d, t, 1), fb_field(d, t, 3)
                comp, n = (struct.unpack_from('<b', d, c)[0] if c is not None else 0), (struct.unpack_from('<Q', d, ln)[0] if ln is not None else 0)
                st, sz = fb_vec(d, t, 4)
                if comp == 3: data = oodle(d[st:st + sz], n)
                elif sz == n: data = d[st:st + sz]
                else: red(f'/{rel}#{name}: file {k} has compression {comp} ({sz} → {n} bytes), which is not known')
                scan_blob(data, f'/{rel}#{name}#{struct.unpack_from("<Q", d, hs[0] + 8 * k)[0]:016x}', out)

UE_LEGACY = 'UE legacy pak'
def legacy_key(pak, mains, known):
    """The AES key of a legacy pak whose index is encrypted (Borderlands 3: all 85 paks, one key), None for a plain index: a key already found that passes the index SHA-1, else the one
    graphics/tools/shbake/pak_crypto.py finds in a main (embedded or built by code, checked against the same SHA-1). No key → red, naming the pak"""
    with romfs_list.open_file(pak) as f:
        ft = uepak._footer(f)   # the index SHA-1 from the parsed footer: its distance from the end differs by version (0x2d up to 7, 0xcd for 8: Shin Megami Tensei V)
        if not ft['enc']: return None
        f.seek(ft['off']); idx = f.read(ft['size']); digest = ft['digest']
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../graphics/tools/shbake')); import pak_crypto
    for k in known:
        if hashlib.sha1(pak_crypto.aes_ecb(k, idx)).digest() == digest: return k
    for m in dict.fromkeys(mains):
        k = pak_crypto.find_key(romfs_list.open_file(m).read(), idx, digest)
        if k: known.append(k); return k
    red(f'{pak}: the legacy index is encrypted and no main holds a key that passes its SHA-1')

def ue_legacy_jobs(files, mains=(), parts=64):
    """Legacy Unreal paks (version 4, DRAGON QUEST XI S: pakchunk0-Switch.pak and pakchunk1-Switch.pak, 13.8 GB, and the update's *_0_P.pak): the engine mounts them all as one view, and a patch pak
    (name ending in _P.pak) wins over the others for the same path. Only the newest copy of each path is scanned. → ([(pak path, rel, (UE_LEGACY, names)), …] in about equal raw sizes, the pak paths taken)"""
    def legacy(p):
        try:
            with romfs_list.open_file(p) as f: return uepak.pak_version(f) in uepak.LEGACY
        except (OSError, ValueError, struct.error): return False
    paks = [(p, r) for p, r in files if p.endswith('.pak') and legacy(p)]
    if not paks: return [], set()
    view, keys, known = {}, {}, []
    for p, r in sorted(paks, key=lambda x: (x[1].endswith('_P.pak'), x[1])):   # patch paks last, so they replace; one view per pak folder (one per version given)
        keys[p] = legacy_key(p, mains, known); pk = uepak.Pak(p, keys[p])
        for name, e in pk.files.items(): view[(os.path.dirname(p), posixpath.normpath(pk.mount + name))] = (p, r, name, e['raw'])
    jobs, cur, size, step = [], collections.defaultdict(list), 0, sum(v[3] for v in view.values()) / parts
    for p, r, name, raw in sorted(view.values(), key=lambda x: (x[1], x[2])):
        cur[(p, r)].append(name); size += raw
        if size >= step: jobs += [(p_, r_, (UE_LEGACY, tuple(ns), keys[p_])) for (p_, r_), ns in cur.items()]; cur, size = collections.defaultdict(list), 0
    jobs += [(p_, r_, (UE_LEGACY, tuple(ns), keys[p_])) for (p_, r_), ns in cur.items()]
    return jobs, {p for p, _ in paks}

_PAKS = {}
def ue_legacy_shaders(path, rel, names, out, key=None):
    """Files of a legacy Unreal pak (ue_legacy_jobs), each decoded block by block with the game's own decoder (DRAGON QUEST XI S: LZ5) and scanned for the shader records the engine keeps inline"""
    pk = _PAKS.get(path) or _PAKS.setdefault(path, uepak.Pak(path, key))
    for name in names:
        d = pk.read(name, decode=oodle)
        inner = re.sub(r"^(\.\./)+", "", posixpath.normpath(pk.mount + name))   # 反斜杠不进 f-string 表达式：3.12 之前不认（作业层最低 3.11）
        ue_inline_shaders(d, f'/{rel}#{inner}', out)   # pak 内路径一律 /：os.path 在 Windows 上换成 \、前缀 ../ 去不掉（11-05 Windows 出包机勇者斗恶龙 XI S 来源列全是 ..\graphics\，分组全落 *_other）

ZLIB_HEAD = re.compile(b'\x78[\x01\x5e\x9c\xda]')
OODLE_HEAD = re.compile(b'[\x8c\xcc][\x06\x0a\x0b\x0c]')
oodle_frames = unreal_pak.oodle_frames   # Oodle 流帧结构校验只有一份（graphics/tools/shbake/unreal_pak.py；出包抽取 pack/ue_pak.py 的内嵌布局同用）

def ue_inline_shaders(d, label, out):
    """Shaders kept inline in cooked Unreal packages (UE 4.18, DRAGON QUEST XI S: each material's shader map in its .uexp, 38 to 87 shaders in the ones looked at, and the engine's global shaders in
    Engine/GlobalShaderCache-GLSL_SWITCH.bin, 945): each shader's code is a byte array (i32 length, then the bytes) holding one zlib stream, which inflates to an LSLG record as in the shader code
    archives (uepak.lslg). Found by the zlib header and the length in front of it: a stream that inflates whole from exactly that many bytes and starts with LSLG (every such stream in the files
    looked at). Borderlands 3 compresses each shader's code with Oodle instead: u64 uncompressed length, then the byte array (i32 length, then one Oodle stream: header byte 0x8C or 0xCC and the
    compressor, 6 Kraken / 10 Mermaid / 11 Selkie / 12 Leviathan, laid out block by block to exactly the array's length: oodle_frames), which the game's own decoder (oodle) inflates to an LSLG record; found by the header and both lengths in front of it, and a candidate the
    decoder rejects or that does not inflate to LSLG is not a shader. A record stored without compression (LSLG right after its length) is taken too. The engine combines vertex and pixel shaders per
    draw (separable_pairs)"""
    import zlib
    def take(s, at):
        rec = uepak.lslg(s)
        if not rec: red(f'{label}: the shader record at {at:#x} is not an LSLG record of the known layout')
        tag, ctl, code = rec; st = UE_TAG.get(tag)
        if not st: red(f'{label}: the shader record at {at:#x} has stage tag {tag}')
        why = check(label, st, ctl, code)
        if why: red(f'{label}: shader at {at:#x} ({st}): {why}')
        out.append((f'{label}#UE{at:x}', st, ctl, code))
    for m in ZLIB_HEAD.finditer(d):
        i = m.start(); n = int.from_bytes(d[i - 4:i], 'little') if i >= 4 else 0
        if not 2 <= n <= len(d) - i: continue
        z = zlib.decompressobj()
        try: s = z.decompress(d[i:i + n])
        except zlib.error: continue
        if z.eof and not z.unused_data and s[:4] == b'LSLG': take(s, i)
    for m in OODLE_HEAD.finditer(d):
        i = m.start()
        if i < 12: continue
        n, raw = int.from_bytes(d[i - 4:i], 'little'), int.from_bytes(d[i - 12:i - 4], 'little')
        if not (8 <= n <= len(d) - i and n < raw <= 1 << 26): continue
        if not oodle_frames(d, i, n, raw): continue
        s = oodle(d[i:i + n], raw, need=False)
        if s and s[:4] == b'LSLG': take(s, i)
    for m in re.finditer(b'LSLG', d):
        i = m.start(); n = int.from_bytes(d[i - 4:i], 'little') if i >= 4 else 0
        if 2 <= n <= len(d) - i and uepak.lslg(d[i:i + n]): take(d[i:i + n], i)

def yaz0(s):
    """Yaz0 decompress via graphics/tools/nxfmt.py (C shared library, pure Python if it does not build)."""
    return nxfmt.yaz0_decompress(s)


def sarc_members(d):
    """SARC members in node order. Endian comes from the BOM (little and big)."""
    return nxfmt.sarc_members(d)

NV_STAGES = ['vs', 'ps', 'gs', 'hs', 'ds', 'cs']   # NVN stage order
COMMON, TREES, SECOND = None, {}, False   # Unity common strings, borrowed type trees, and whether this is the second pass, which generates Shader layouts by version (given when the pool starts)
ZDICTS = {}       # zstd dictionaries by ID (given when the pool starts; zstd_lib keeps their digested form per process)

def _pool_init(common, trees, zdicts={}, second=False):
    global COMMON, TREES, ZDICTS, SECOND
    COMMON, TREES, ZDICTS, SECOND = common, trees, zdicts, second

def zs_dicts(romfs):
    """Tears of the Kingdom compresses its RomFS files with zstd dictionaries (the frame header names one, ID 1 for the shader packages). They ship as Pack/ZsDic.pack.zs:
    a SARC (itself plain zstd) of *.zsdic files, each a zstd dictionary (magic 0xEC30A437, then its ID). → {ID: dictionary}; {} without that file (Mario's .zs are plain zstd)"""
    p = os.path.join(romfs, 'Pack/ZsDic.pack.zs')
    if not romfs_list.exists(p): return {}
    d = zstd_lib.decompress(romfs_list.open_file(p).read())
    return {struct.unpack_from('<I', m, 4)[0]: m for _, m in sarc_members(d) if m[:4] == bytes.fromhex('37a430ec')}

def unzs(path):
    """A zstd file decompressed in process (zstd_lib / libzstd): frames without a dictionary plainly, frames that name one with that dictionary from ZDICTS.
    (11-06 speed line: the no-dictionary frames used to go through one zstd command per file — Splatoon 3's 13,986 files spent 108 of 157 s in process start-up)"""
    d = romfs_list.open_file(path).read()
    fhd = d[4] if len(d) > 5 and d[:4] == bytes.fromhex('28b52ffd') else 0
    n, o = (0, 1, 2, 4)[fhd & 3], 6 - (fhd >> 5 & 1)   # dictionary ID size; it follows the window descriptor, which single-segment frames omit
    did = int.from_bytes(d[o:o + n], 'little')
    if not did: return zstd_lib.decompress(d)
    if did not in ZDICTS: red(f'{path}: a frame needs zstd dictionary {did}, which Pack/ZsDic.pack.zs does not have')
    return zstd_lib.decompress(d, dict_bytes=ZDICTS[did])

SKIPPED = []   # Unity program blocks left out in this process: (where, why); scan_counted hands each file's share back, main counts them

def unity_block(code, stage):
    """Unity's Switch program code → ([(stage, control, code)], None), or ([], why) when the block is empty or does not hold consistent stage blocks"""
    if not code: return [], 'empty program code (0 bytes)'
    if len(code) >= 0x14 and code[0x10:0x14] == CTL_MAGIC:   # before 2019.3: 0x10 header + control section + code area, one stage
        nctl, ncode = struct.unpack_from('<2i', code, 8); a = 0x10; end = a + nctl + ncode
        ctl, cd = code[a:a + nctl], code[a + nctl:end]
        why = check('', stage, ctl, cd) if end <= len(code) else 'block out of bounds'
        if why: return [], f'{stage}: {why}'
        if end != len(code): return [], f'program code is {len(code):#x} bytes, but control section + code area end at {end:#x}'
        return [(stage, ctl, cd)], None
    if len(code) < 0x60: return [], f'program code is {len(code):#x} bytes, shorter than its 0x60 header'
    h = struct.unpack_from('<24i', code, 0); out = []; end = 0x60
    for i, st in enumerate(NV_STAGES):
        off, nctl, ncode = h[6 + i], h[12 + i], h[18 + i]
        if off == -1: continue
        a = 0x60 + off; ctl, cd = code[a:a + nctl], code[a + nctl:a + nctl + ncode]
        why = check('', st, ctl, cd) if off >= 0 and a + nctl + ncode <= len(code) else 'block out of bounds'
        if why: return [], f'{st}: {why}'
        out.append((st, ctl, cd)); end = max(end, a + nctl + ncode)
    if end != len(code): return [], f'program code is {len(code):#x} bytes, but the stage blocks end at {end:#x}'
    return out, None

def unity_program(code, where, stage=None):
    """Unity's Switch program code → [(stage, control, code)]; stage = the subprogram's stage from m_ParsedForm, which only the pre-2019.3 layout needs.
    An empty or inconsistent block is left out, reported and counted (SKIPPED), not the end of the extraction: Servonauts (Unity 2021.3) has a compute kernel whose code is 0 bytes (S2Arm 10-29)"""
    if stage is None and len(code) >= 0x14 and code[0x10:0x14] == CTL_MAGIC: red(f'{where}: pre-2019.3 subprogram whose stage is not in m_ParsedForm')
    out, why = unity_block(code, stage)
    if why:
        print(f'[RED] {where}: program block left out ({why})', flush=True); SKIPPED.append((where, why))
    return out

def unity_scan_kernels(b):
    """A ComputeShader object's bytes → [code] of its kernel variants, read without a type tree: each code is a byte vector (i32 length, then the program code) whose program code
    unity_block takes as one compute stage. The kernels' layout changes with the Unity version (kernel_variants) but the code vectors do not, and a program code checks itself
    (control magic, code magic, lengths that add up to the vector), so no layout is needed to find them"""
    out, p = [], 0
    while p + 4 + 0x14 <= len(b):
        n = struct.unpack_from('<i', b, p)[0]; c = p + 4
        if 0x14 <= n <= len(b) - c and (b[c + 0x10:c + 0x14] == CTL_MAGIC or n >= 0x60 and b[c + 0x60 + max(struct.unpack_from('<i', b, c + 44)[0], 0):][:4] == CTL_MAGIC):   # the cs block's offset, +0x2c
            blocks, why = unity_block(b[p + 4:p + 4 + n], 'cs')
            if not why and [st for st, _, _ in blocks] == ['cs']:
                out.append(b[p + 4:p + 4 + n]); p = (p + 4 + n + 3) & ~3; continue
        p += 4
    return out

def unity_stages(o):
    """Shader object → {subprogram index: stage}, from the passes of m_ParsedForm (progVertex / progFragment / ... m_SubPrograms[].m_BlobIndex)"""
    m = {}
    for sub in (o.get('m_ParsedForm') or {}).get('m_SubShaders', []):
        for p in sub.get('m_Passes', []):
            for field, st in (('progVertex', 'vs'), ('progFragment', 'ps'), ('progGeometry', 'gs'), ('progHull', 'hs'), ('progDomain', 'ds')):
                for sp in (p.get(field) or {}).get('m_SubPrograms', []): m.setdefault(sp.get('m_BlobIndex'), st)
    return m

def unity_pass_index(o):
    """Shader object → {subprogram index: [(SubShader number, Pass number, stage, hardware tier)…]} from m_ParsedForm: the Pass a subprogram belongs to, for pairing (unity_kw_pairs)"""
    m = collections.defaultdict(list)
    for si, sub in enumerate((o.get('m_ParsedForm') or {}).get('m_SubShaders', [])):
        for pi, p in enumerate(sub.get('m_Passes', [])):
            for field, st in (('progVertex', 'vs'), ('progFragment', 'ps')):
                for sp in (p.get(field) or {}).get('m_SubPrograms', []): m[sp.get('m_BlobIndex')].append((si, pi, st, sp.get('m_ShaderHardwareTier', 0)))
    return m

# Unity before 2019.3 (Ori and the Will of the Wisps, Unity 2018.4) stores each subprogram with one stage: the vertex and pixel programs of a Pass are separate blob entries, numbered apart,
#   so var_pairs (vs + ps of one entry) pairs none of them (01008DD013200000: 34 pairs in all, from main's GLSLC; none of the 51 pairs its prologue draws with). Unity picks a Pass's programs
#   by the keyword set of the draw, each stage from its own list: the vertex and pixel subprograms of one Pass, platform and hardware tier with the same keywords are a pair. A vertex
#   subprogram with no pixel subprogram of the same keywords (keywords one stage strips) takes the pixel subprograms whose keyword set differs least. Pairs of the file being scanned,
#   taken by var_pairs in the same worker
UNITY_PAIRS = set()
def unity_kw_pairs(progs):
    """progs: [(group (platform, SubShader, Pass, tier), stage, frozenset keywords, key)] → {(vertex key, pixel key)}"""
    by = collections.defaultdict(lambda: ([], []))
    for g, st, kw, key in progs: by[g][0 if st == 'vs' else 1].append((kw, key))
    pairs = set()
    for vss, pss in by.values():
        if not pss: continue
        for kw, vk in vss:
            same = [pk for pkw, pk in pss if pkw == kw]
            if not same:
                d = min(len(kw ^ pkw) for pkw, _ in pss); same = [pk for pkw, pk in pss if len(kw ^ pkw) == d]
            pairs.update((vk, pk) for pk in same)
    return pairs

def unity_code_blobs(o):
    """Shader object → the subprogram indices that hold program code, or None when every entry does (before Unity 2022). Unity 2022 also keeps the parameters of each program in the blob
    as entries of their own (m_ParameterBlobIndices of each program), and the player's programs are m_PlayerSubPrograms[tier][].m_BlobIndex (Ace Attorney Investigations Collection, Unity 2022.3).
    Both fields came together: UnityPy's type trees for 2020.3.18 (Engage) and 2021.3 have neither, so older games read every entry as before"""
    progs = [p.get(f) or {} for sub in (o.get('m_ParsedForm') or {}).get('m_SubShaders', []) for p in sub.get('m_Passes', [])
             for f in ('progVertex', 'progFragment', 'progGeometry', 'progHull', 'progDomain')]
    if not any('m_ParameterBlobIndices' in p for p in progs): return None
    return {sp['m_BlobIndex'] for p in progs for tier in p.get('m_PlayerSubPrograms', []) for sp in tier}

def kernel_variants(k, where):
    """A ComputeShader kernel's variants, each with its 'code', by Unity version: 2022.3 lists them in uniqueVariants, 2020.3 keys them by keyword set in variantMap,
    2018.4 has no variants: the kernel itself carries name and code (S2Arm 10-25: Moving Out, Unity 2018.4.4f1, stopped the direct library on KeyError 'variantMap', #379).
    Any other layout is reported with its fields, never skipped silently"""
    if 'uniqueVariants' in k: return k['uniqueVariants']
    if 'variantMap' in k: return [p['second'] for p in k['variantMap']]
    if 'code' in k: return [k]
    red(f'{where}: ComputeShader kernel {k.get("name", "?")} of an unknown layout (fields {sorted(k)}): no uniqueVariants, variantMap or code')

def unity_compute(x, base, trees):
    """ComputeShader objects (class 72) of a serialized file → [(where, 'cs', control, code)]: each kernel variant's code is a program code with one stage, compute (Monster Train 2,
    Unity 2022.3: the post-processing stack's 12 compute shaders, its colour grading LUT baker among them, in Data/sharedassets2.assets). Raises NoTree in the first pass, so the file is scanned
    again with the borrowed trees; in the second pass a class still without a tree has its kernels' code found in the object bytes (unity_scan_kernels: unityfs.py has no ComputeShader layout).
    Unity 2022.3 lists a kernel's variants in uniqueVariants; Unity 2020.3 keys them by keyword set in variantMap (AI: THE SOMNIUM FILES - nirvanA Initiative's bundles, whose type trees say so)"""
    import unityfs
    out = []
    try:
        for cid, pid, o in unityfs.serialized_objects(x, COMMON, {unityfs.CLASS_COMPUTE_SHADER}, trees):
            for v in o['variants']:
                for k in v['kernels']:
                    for i, u in enumerate(kernel_variants(k, f'{base}#{o["m_Name"]}')):
                        where = f'{base}#{o["m_Name"]}#{k["name"]}.{i}'
                        out.extend((where, st, ctl, cd) for st, ctl, cd in unity_program(u['code'], where, 'cs'))
    except unityfs.NoTree:
        if not SECOND: raise
        out = []
        for cid, pid, b in unityfs.raw_objects(x, COMMON, {unityfs.CLASS_COMPUTE_SHADER}):
            n = struct.unpack_from('<i', b, 0)[0]; name = b[4:4 + n].decode('utf-8', 'replace')
            for i, code in enumerate(unity_scan_kernels(b)):
                out.extend((f'{base}#{name}#kernel{i}', st, ctl, cd) for st, ctl, cd in unity_program(code, f'{base}#{name}#kernel{i}', 'cs'))
    return out

def unity_shaders(d, label, out, trees):
    """Shaders and compute shaders in a UnityFS bundle or serialized file → out; raises NoTree when no type tree is available.
    In the second pass (SECOND) a Shader no tree is found for takes the layout generated from the Unity version the file (or its bundle) records (unityfs.shader_layout)"""
    import unityfs
    if COMMON is None: red(f'{label}: a Unity asset, but main has no Unity common string table')
    bundle = d[:8] == b'UnityFS\0'
    sers, engine = (unityfs.bundle_files(d), unityfs.bundle_version(d)) if bundle else ([('', d)], '')
    for node, x in sers:
        if not unityfs.is_serialized(x): continue
        try:
            for cid, pid, o in unityfs.serialized_objects(x, COMMON, {unityfs.CLASS_SHADER}, trees, layouts=SECOND, engine=engine):
                name = o['m_ParsedForm']['m_Name'] if 'm_ParsedForm' in o else o.get('m_Name', '?')
                stages = unity_stages(o); passes = unity_pass_index(o); progs = []
                for plat, k, sp in unityfs.shader_programs(o, unity_code_blobs(o)):
                    where = f'{label}{":" + node if node else ""}#{name}#var{plat}.{k}'
                    sub = unityfs.subprogram(sp); kw = frozenset(sub[2]) | frozenset(sub[3])
                    got = unity_program(sub[4], where, stages.get(k))
                    out.extend((where, st, ctl, cd) for st, ctl, cd in got)
                    if len(got) == 1 and got[0][0] in ('vs', 'ps'):   # one stage per entry (before 2019.3): pair by Pass and keywords
                        for si, pi, st, tier in passes.get(k, []):
                            if st == got[0][0]: progs.append(((plat, si, pi, tier), st, kw, skey(got[0][1], got[0][2])))
                UNITY_PAIRS.update(unity_kw_pairs(progs))
        except unityfs.LayoutMismatch as e: red(f'{label}{":" + node if node else ""}: {e}')
        out.extend(unity_compute(x, f'{label}{":" + node if node else ""}', trees))

def scan_blob(d, label, out):
    if d[:4] == b'Yaz0': d = yaz0(d)
    if d[:4] == b'SARC':
        for nm, m in sarc_members(d): scan_blob(m, f'{label}/{nm}', out)
        return
    if d[:4] == b'BAHS': out.extend(bahs_shaders(d, label)); return
    if d[:4] == b'MSHD': out.extend(mshd_shaders(d, label)); return
    if d[:4] == b'NDSR': out.extend(ndsr_shaders(d, label)); return
    if shaders := sead_prim_shaders(d, label): out.extend(shaders); return
    n0 = len(out)
    for m in re.finditer(b'BNSH\0\0\0\0', d):
        size = struct.unpack_from('<I', d, m.start() + 0x1c)[0]
        if m.start() + size <= len(d) and d[m.start() + 0x60:m.start() + 0x64] == b'grsc': out.extend(bnsh_shaders(d, m.start(), label))
    if MGFX in d: out.extend(mgfx_shaders(d, label))
    if ns2.CTL_MAGIC + ns2.CTL_VERSION in d:   # NVN2 shaders outside the containers above (Ocarina's Movie/Ndve/*.dat): an NDSR pack inside the file, then bare control block + code area
        for m in re.finditer(b'NDSR', d): out.extend(ndsr_shaders(d, f'{label}@{m.start():#x}', m.start()))
        out.extend(ns2.bare_programs(d, label, {ns2.key_ctl(c) for _, _, c, _ in out[n0:] if ns2.is_nvn2(c)}))

def group(label):
    """source group (second half of the package file name)"""
    if label.startswith('main'): return 'main'
    for k, g in GROUPS:   # the profile's shader_groups, first match wins
        if k in label: return g
    return 'other'

def ps_inputs(code):
    """Generic attributes a pixel shader reads: SPH words 6–13 are the input map, 8 bits for each of the 32 attributes (Ryujinx ShaderHeader.ImapTypes), non-zero = read; the translated MSL inputs are exactly user(locn<attribute>)"""
    return {i for i in range(32) if struct.unpack_from('<I', code, 0x30 + 4 * (6 + (i >> 2)))[0] >> ((i & 3) * 8) & 0xff}

SINGLE_STAGE = ('#GLSLC@', '#UE', '#MTSPK@', '#MGFX@', '#NLG', '#DNBC@', '#LZ4UE3@')   # sources whose every shader is a program of one stage: GLSLC outputs, Unreal's shader records, MT Framework's shader package (BNSH variations, Unity programs, MSHD and SDF pair theirs);
# MonoGame's effects, whose passes may name no vertex shader and keep the one bound (Celeste: 10 of its 13 effects draw through SpriteBatch's vertex shader, which is in main); and Next Level Games'
# archives (Luigi's Mansion 3 makes 5,317 programs of one stage each at boot)
def separable_pairs(keys):
    """Single-stage programs (13 Sentinels: each GLSLC output becomes a program with one stage, and BindProgram's stage bits combine vertex and pixel programs per draw; Unreal combines its shaders per draw
    from the pipeline state) have their real pairs only at draw time. vs_pad uses only the union of the locations the paired pixel shaders read, so every single-stage vertex shader is paired with
    a "cover": the fewest pixel shaders whose read locations together equal those of all pixel shaders (greedy, ties go to the smaller key); padded that way, a vertex shader links with any pixel shader.
    All pairs would be 376 × 375 rows; the cover needs a few rows × 376. No single-stage vertex shader, no cover"""
    single = [v for v in keys if keys[v][1] == 'vs' and any(s in keys[v][0] for s in SINGLE_STAGE)]
    if not single: return set(), []
    ins = {k: ps_inputs(v[3]) for k, v in keys.items() if v[1] == 'ps'}
    left, cover = set().union(*ins.values()) if ins else set(), []
    while left:
        k = min(ins, key=lambda k: (-len(ins[k] & left), k)); cover.append(k); left -= ins[k]
    return {(v, p) for v in single for p in cover}, cover

UE_TAG = {'SV': 'vs', 'SH': 'hs', 'SD': 'ds', 'SG': 'gs', 'SP': 'ps', 'SC': 'cs'}
def main_imports(img):   # the names main imports (its dynamic symbols)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../graphics/nvn/tools')); import nso_syms
    return {s for s, defined, _ in nso_syms.dynsyms(img) if not defined}

def glslc_module(exefs):   # a module of the profile's that defines glslcCompile, beside main in its ExeFS (profile_guess.py build_sources); None: main's import is never bound
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../graphics/nvn/tools')); import nso_syms
    for m in GAME.words('modules', 'main'):
        p = os.path.join(exefs, m)
        if m != 'main' and romfs_list.exists(p) and any(s == 'glslcCompile' and defined for s, defined, _ in nso_syms.dynsyms(nso_image(p))): return m
    return None

def ue_shaders(d, label):
    """An Unreal Engine shader code archive with every shader decoded (Octopath Traveler 0: the two .ushaderbytecode files of its pak, whose shaders are Oodle-compressed; the NSP script decodes them
    on the Mac with the game's own Oodle, uepak.py): each shader is an LSLG record holding one stage's code area and control section (uepak.lslg); the stage is the record's tag (SV vertex,
    SP pixel, SC compute), checked against the archive entry's frequency. The engine combines vertex and pixel shaders per draw, so they are paired as single-stage programs (separable_pairs)"""
    a, out = uepak.archive(d), []
    for i, (off, size, raw, fr) in enumerate(a['shaders']):
        if size != raw: red(f'{label}: shader {i} is still compressed (an archive the NSP script has not decoded)')
        rec = uepak.lslg(d[a['code'] + off:a['code'] + off + size])
        if not rec: red(f'{label}: shader {i} is not an LSLG record')
        tag, ctl, code = rec; st = UE_TAG.get(tag)
        if not st or uepak.FREQ.get(fr) != st: red(f'{label}: shader {i} has tag {tag} but frequency {fr}')
        why = check(label, st, ctl, code)
        if why: red(f'{label}: shader {i} ({st}): {why}')
        out.append((f'{label}#UE{i}', st, ctl, code))
    return out

def var_pairs(found):
    """(vertex key, pixel key) within the same BNSH variation, and the Unity Pass / keyword pairs of the file (UNITY_PAIRS, taken and cleared here)"""
    var = collections.defaultdict(dict)
    for where, st, ctl, code in found:
        if '#var' in where: var[where][st] = skey(ctl, code)
    out = {(v['vs'], v['ps']) for v in var.values() if 'vs' in v and 'ps' in v} | UNITY_PAIRS
    UNITY_PAIRS.clear()
    return out

FILE_RED = '\0file_red'   # marks a file left out whole in the SKIPPED channel (v0.3.1)


DEGRADE_ROWS_CAP = 50   # 每类逐条写进降级表的上限：超出的并成一行「另有 N 条」（处数合计不变）


def write_degrade(path, file_red, blocks, same_key, cap=DEGRADE_ROWS_CAP):
    """降级表 degrade.tsv（五列：阶段 / 类别 / 处数 / 玩家提示 / 说明，pack/jobs/build.py degrade_rows 读）：每个被跳过的文件 / 被丢的程序块 / 同键不同内容各写一行
    （处数 1、说明带文件名或位置与原因），每类最多 cap 行、其余并成一行（处数 = 余数），各类处数合计 = 实际条数。玩家提示留空（不上完成卡，只进诊断）。
    原先每类只写一行「首个」：诊断里只看得到第一个文件，被丢的程序块整类不写（#1122）。没有降级写空文件（调用方按有无行判）"""
    kinds = (('split:file_skipped', '直连抽取跳过读不了的文件（里面的着色器运行时首遇现编）', file_red),
             ('split:block_skipped', '直连抽取丢了 Unity 程序块（这支着色器不进映射表、运行时首遇现编）', blocks),
             ('split:same_key', '同键不同内容，留先到的那份', same_key))
    clean = lambda m: ' '.join(str(m).split())[:300]   # 说明一列不许有制表符 / 换行（degrade_rows 按制表符切五列）
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        for kind, what, items in kinds:
            for m in items[:cap]: f.write(f'extract\t{kind}\t1\t\t{what}：{clean(m)}\n')
            if len(items) > cap: f.write(f'extract\t{kind}\t{len(items) - cap}\t\t{what}：另有 {len(items) - cap} 条未逐列（见 split.log）\n')


def skip_summary(file_red, skipped, same_key, show=3):
    """降级汇总的收尾行（标准错误，进 split.log）：读不了、整份没抽的文件计红（个数 + 前 show 个路径与原因），丢了程序块 / 同键不同内容照旧黄。
    读不了的文件原先只并进一行黄（RomFS 只有镜像 romfs.std 时 19012 个文件全部这样跳过，另一路抽取补齐才没露馅）"""
    out = []
    if file_red: out.append(f'【红】直连抽取：{len(file_red)} 个文件读不了、整份没抽（前 {min(show, len(file_red))} 个：{"；".join(" ".join(str(m).split())[:200] for m in file_red[:show])}），其余照抽；这些文件里的着色器运行时首遇现编，逐条见上方 [WARN]')
    if skipped or same_key: out.append(f'【黄】直连抽取：丢了程序块 {len(skipped)} 个、同键不同内容 {len(same_key)} 处（留先到的），其余照抽；逐条见上方 [WARN] / [RED]')
    return out


def scan_counted(job):
    """scan_file + the Unity program blocks this file had left out (SKIPPED). A file the readers cannot read (a [RED] exit or a parse
    error inside its scan) is left out alone and recorded (FILE_RED), the rest are read on (v0.3.1: do not stop on what we cannot be sure of;
    one unreadable file used to end the whole split and leave the game with no offline library at all). Out of memory still stops"""
    n = len(SKIPPED)
    try: r = scan_file(job)
    except MemoryError: raise
    except SystemExit as e:
        if not str(e).startswith('[RED]'): raise
        return (job[1], {}, set(), {}, [(FILE_RED, f'{job[1]}: {str(e)[6:]}')])
    except Exception as e:   # struct.error / IndexError / ValueError / zlib.error … from a container the reader got wrong
        return (job[1], {}, set(), {}, [(FILE_RED, f'{job[1]}: {type(e).__name__}: {e}')])
    return (*r, SKIPPED[n:])


# ---------- Unreal Engine 3 packages (BioShock Infinite, licensee 78) ----------
# A .xxx package is compressed in chunks: the summary holds n × {u32 uncompressed offset, u32 uncompressed size, u32 stored offset, u32 stored size} after a u32 n, and each
# stored chunk is {u32 0x9E2A83C1, u32 block size 0x20000, u32 stored total, u32 uncompressed total} + n × {u32 stored, u32 uncompressed} + the blocks (LZ4 block format:
# every block of the game's 2,119 packages decompresses to its exact size; not LZO or zlib). The whole package = file bytes up to the first chunk + every chunk at its offset.
# Some packages (Core / Engine / XCore / Startup, 10) are compressed whole: the file is a chain of chunks from 0. The table is found without the licensee's summary fields
# (it is not 4-byte aligned in every package): n followed by n entries whose stored offsets all hold the chunk tag and whose uncompressed ranges follow on.
# Inside a package the shader code is compressed again: {u32 uncompressed, u32 stored, LZ4 block} that decompresses to DNBC records (ue3_dnbc_shaders);
# GlobalShaderCache-NX.bin holds the same blocks uncompressed around them
UE3_TAG, UE3_DNBC = 0x9E2A83C1, b'DNBC\x01\0\0\0'


def ue3_chunk(d, c, where):
    import lz4.block
    _, bs, cs, us = struct.unpack_from('<4I', d, c)
    if not ue3_chain(d, c): red(f'{where}: UE3 chunk @{c:#x}: header {{block size {bs:#x}, stored {cs:#x}, uncompressed {us:#x}}} or its block table is not a chunk')   # 11-05 MK11: bs 0 divided by zero
    n = (us + bs - 1) // bs
    blocks = [struct.unpack_from('<II', d, c + 16 + 8 * i) for i in range(n)]
    if sum(b[0] for b in blocks) != cs or sum(b[1] for b in blocks) != us: red(f'{where}: UE3 chunk @{c:#x}: its block table does not add up')
    out, o = bytearray(), c + 16 + 8 * n
    for bc, bu in blocks:
        r = d[o:o + bc] if bc == bu else lz4.block.decompress(d[o:o + bc], uncompressed_size=bu)
        if len(r) != bu: red(f'{where}: UE3 chunk @{c:#x}: a block gave {len(r)} of {bu} bytes')
        out += r; o += bc
    return bytes(out), 16 + 8 * n + cs


def ue3_head(d, p):
    """A plausible chunk header at p (tag, block size a power of 2 in 0x10000–0x100000, stored / uncompressed non-zero): whether a block-table entry
    points at a chunk at all. Whether its block table adds up is ue3_chunk's to check (and red on) — a real table pointing at a corrupt chunk is bad data,
    not a non-table (11-06: MK11's false table points at a size-0 header and is skipped here; t_ue3_dnbc ⑥ a real table with a bad sub-block sum still reds)"""
    if p + 16 > len(d): return False
    tag, bs, cs, us = struct.unpack_from('<4I', d, p)
    return tag == UE3_TAG and not bs & (bs - 1) and 0x10000 <= bs <= 0x100000 and cs > 0 and us > 0


def ue3_chain(d, p):
    """A chunk at p whose header and block table hold together: its full length; else 0 (the package tag at 0 alone does not make one: every package starts with it)"""
    if p + 16 > len(d): return 0
    tag, bs, cs, us = struct.unpack_from('<4I', d, p)
    if tag != UE3_TAG or bs & (bs - 1) or not 0x10000 <= bs <= 0x100000 or not cs or not us: return 0   # block size 0x20000 (BioShock Infinite) or 0x80000 (Outlast 2)
    n = (us + bs - 1) // bs
    if p + 16 + 8 * n > len(d): return 0
    blocks = [struct.unpack_from('<II', d, p + 16 + 8 * i) for i in range(n)]
    return 16 + 8 * n + cs if sum(b[0] for b in blocks) == cs and sum(b[1] for b in blocks) == us and p + 16 + 8 * n + cs <= len(d) else 0


def ue3_package(d, where):
    """The whole (decompressed) package bytes; a package stored uncompressed comes back as it is"""
    if ue3_chain(d, 0):   # compressed whole: a chain of chunks from 0, then zero padding
        out, p = bytearray(), 0
        while ue3_chain(d, p): r, n = ue3_chunk(d, p, where); out += r; p += n
        if any(d[p:]): red(f'{where}: UE3 package compressed whole: {len(d) - p} bytes after its last chunk are not a chunk or padding')
        return bytes(out)
    for p, (fmt, w) in ((p, f) for p in range(4, min(len(d) - 4, 0x100000)) for f in (('<4I', 16), ('<QIQI', 24))):   # entries {u32 ×4} (BioShock Infinite) or
        n = struct.unpack_from('<I', d, p)[0]                                                                             # {u64 offset, u32 size, u64 stored offset, u32 stored size} (Outlast 2)
        if not 0 < n < 4096 or p + 4 + w * n > len(d): continue
        e = [struct.unpack_from(fmt, d, p + 4 + w * i) for i in range(n)]
        # every entry must point at a plausible chunk header (ue3_head), not just a tag whose +12 equals its size: Mortal Kombat 11 (UE3, licensee 0x9d, packages stored uncompressed)
        #   has n = 1 entries {0x5ba8, 0, 0x5ba8, 0} inside name records pointing at an embedded {tag, 0, 0x20000, 0} — a size 0 "chunk" that divided by zero in ue3_chunk
        if all(p + 4 + w * n <= c and c + 16 <= len(d) and struct.unpack_from('<I', d, c)[0] == UE3_TAG and struct.unpack_from('<I', d, c + 12)[0] == ul and ue3_head(d, c) for _, ul, c, _ in e) \
           and all(e[i][0] + e[i][1] == e[i + 1][0] for i in range(n - 1)):
            out = bytearray(d[:e[0][0]])
            for uo, ul, c, _ in e:
                if len(out) != uo: red(f'{where}: UE3 chunk table: chunk at {uo:#x} after {len(out):#x} bytes')
                out += ue3_chunk(d, c, where)[0]
            return bytes(out)
    return d


UE3_LZ4_REC = re.compile(rb'(?s)(?=\x00.[\x00-\xff][\x07-\x1f]..[\x00-\x3f]\x00)')   # a record header from its +4: u8 0, u8, u16 control length (0x700–0x1fff), u16, u16 n < 64


def ue3_lz4_shaders(d, label, out):
    """Outlast 2 (UE3, licensee 200): each shader's code is a record {u32 L, u8 0, u8, u16 control length, u16, u16 n, n × u16, then an LZ4 block up to the record's end (L bytes after
    the length)} that decompresses to the code area followed by the control section, in GlobalShaderCache-Switch.bin (1,985 records, every control magic of the file inside one) and in the
    material shader maps of the packages. A header is taken only when the block decompresses to exactly that: control magic at the end minus the control length, and control +0x704
    = the bytes before it"""
    import lz4.block
    end = 0
    for m in UE3_LZ4_REC.finditer(d):
        p = m.start() - 4
        if p < end: continue
        L = struct.unpack_from('<I', d, p)[0]; _, _, nctl, _, n = struct.unpack_from('<BBHHH', d, p + 4); q = p + 12 + 2 * n
        if not q < p + 4 + L <= len(d): continue
        try: b = lz4.block.decompress(d[q:p + 4 + L], uncompressed_size=nctl + 0x200000)
        except Exception: continue
        k = len(b) - nctl
        if k <= 0 or b[k:k + 4] != CTL_MAGIC or struct.unpack_from('<I', b, k + 0x704)[0] != k: continue
        ctl, code = b[k:], b[:k]; st = glslc_stage(ctl[:0x880], code)
        why = check(label, st, ctl, code) if st else 'no stage in its code area'
        if why: red(f'{label}: UE3 LZ4 shader record @{p:#x}: {why}')
        out.append((f'{label}#LZ4UE3@{p:#x}', st, ctl, code)); end = p + 4 + L


def ue3_dnbc_shaders(d, label, out):
    """The compressed shader blocks of a UE3 package {u32 uncompressed, u32 stored, LZ4 block} and the DNBC records in them, one after another:
    a 0x20 header {'DNBC', u32 1, u32 code size A, u32 0, u32 control size B, u32 A, u32 reflection tail R, u32 A + B}, then code A + control B + reflection R (input semantics).
    A block is found from the 'DNBC' literal near its start: it is the first literal run of the LZ4 block, right after its token (and length bytes when it is 15 or longer).
    Other serialized fields may sit between records, so each record is found by its magic and cut by the lengths in its header"""
    import lz4.block
    h, seen = d.find(UE3_DNBC), set()
    while h >= 0:
        for tok in (h - 1, h - 2, h - 3):
            if tok < 8 or tok in seen: continue
            us, cs = struct.unpack_from('<II', d, tok - 8)
            if not 0 < cs <= us < 1 << 28 or tok + cs > len(d): continue
            try: b = lz4.block.decompress(d[tok:tok + cs], uncompressed_size=us)
            except Exception: continue
            if len(b) != us or b[:8] != UE3_DNBC: continue
            seen.add(tok); where = f'{label}@{tok:#x}'; p = 0
            while p >= 0:
                if p + 0x20 > len(b): red(f'{where}: DNBC @{p:#x} header runs past the end')
                a, z, bb, a2, r, ab = struct.unpack_from('<6I', b, p + 8)
                if z or a2 != a or ab != a + bb or p + 0x20 + a + bb + r > len(b): red(f'{where}: DNBC @{p:#x}: header {a:#x}/{z}/{bb:#x}/{a2:#x}/{r:#x}/{ab:#x} is not a code + control record')
                code, ctl = b[p + 0x20:p + 0x20 + a], b[p + 0x20 + a:p + 0x20 + a + bb]
                if ctl[:4] != CTL_MAGIC: red(f'{where}: DNBC @{p:#x}: no control magic after the code area')
                st = glslc_stage(ctl[:0x880], code)
                if not st: red(f'{where}: DNBC @{p:#x}: the code area has no stage')
                ctl = ctl[:ctl_len(ctl)]
                why = check(where, st, ctl, code)
                if why: red(f'{where}: DNBC @{p:#x} {st}: {why}')
                out.append((f'{where}#DNBC@{p:#x}', st, ctl, code))
                p = b.find(UE3_DNBC, p + 0x20 + a + bb + r)
            h = d.find(UE3_DNBC, tok + cs); break
        else: h = d.find(UE3_DNBC, h + 1)


def scan_file(args):
    """→ (relative path, {key: …}, pairs, extra); extra = this file's Unity type trees, or 'defer' (no type tree, scanned again in the second pass)"""
    path, rel = args[:2]
    stem, ext = os.path.splitext(path)
    if ext in ('.data', '.patch', '.debug', '.nxpc') and romfs_list.exists(stem + '.dict'): return rel, {}, set(), {}   # read through its Next Level Games index (nlg_shaders)
    with romfs_list.open_file(path) as f: head = f.read(16)
    found = []; trees = {}
    if head[:4] == NLG_DICT:
        nlg_shaders(path, '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:8] in (IDRK, PDRK):   # PDRK: Pokopia's .fdata, 'PDRK0000' + u64 0x10 before its IDRK entries
        idrk_shaders(path, '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == b'MDB1':   # Media.Vision 的 MVGL 归档（时空异客 / Cyber Sleuth）：条目由 S2Arm 抽取（nxsh_extract 的 mdb1_visit）逐个解出、
        return rel, {}, set(), {}   # 相邻对在那边配；这里不按裸字节扫——ounce64（Switch 2 版着色器变体）裸扫会配出控制段与代码区对不上的假键（直连库全数「NVN2 控制段长度不合」）
    if len(args) > 2 and head[:8] == ONEPACK:   # a range of Trinity packs (trpfs_jobs)
        trinity_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and args[2][0] == UE_LEGACY:   # files of a legacy Unreal pak (ue_legacy_jobs)
        ue_legacy_shaders(path, rel, args[2][1], found, args[2][2])
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:4] == KPKA:   # entries of an RE Engine PAK (kpka_jobs)
        kpka_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2 and head[:4] == b'BDF3':   # entries of a FromSoftware dvdbnd archive (bdt_jobs)
        bdt_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(args) > 2:   # a range of LINKDATA entries (linkdata_jobs)
        linkdata_shaders(path, rel, args[2], found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == SPKG or head[:4] == b'\0\0\1\0' and path.endswith('.gz'):   # Three Houses' loose shader packages and chunked zlib files (kt_shaders)
        kt_shaders(romfs_list.open_file(path).read(), '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == MTARC:   # an MT Framework archive (mtarc_members): its shader package
        mtarc_shaders(romfs_list.open_file(path).read(), '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if head[:4] == SCNE:   # a Bezel Engine archive (bea_members): each entry scanned as a file of its own (the shaders are BNSH in its .bfsha entries)
        for nm, m in bea_members(romfs_list.open_file(path).read(), '/' + rel): scan_blob(m, f'/{rel}/{nm}', found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if len(head) >= 8 and head[:4] == b'\0\0\0\0' and 0 < 8 + 0x50 * struct.unpack_from('<I', head, 4)[0] <= (size := romfs_list.file_size(path)):   # maybe a Falcom package: no magic, its entry table decides (falcom_pkg)
        mm = d = map_big(path) if size > BIG else romfs_list.open_file(path).read()
        try:
            entries = falcom_pkg(d)
            if entries is None: return scan_plain(path, rel, d, found, trees)
            falcom_pkg_shaders(d, entries, '/' + rel, found)
            return rel, uniq_keys(found), var_pairs(found), trees
        finally:
            if size > BIG: mm.close()
    if head[:4] == ZSTD_MAGIC: d = unzs(path)   # by the magic: Paper Mario: The Thousand-Year Door names its zstd files .zst (8,726, its shaders among them)
    elif romfs_list.file_size(path) > BIG:   # a big plain file is mapped, not copied into this process (Persona 5 Royal's ALL_ASIA.CPK, 12.7 GB: one worker held 13 GB on a 16 GB Windows PC)
        mm = d = map_big(path)
        try: return scan_plain(path, rel, d, found, trees)
        finally: mm.close()
    else: d = romfs_list.open_file(path).read()
    return scan_plain(path, rel, d, found, trees)

def scan_plain(path, rel, d, found, trees):
    """The rest of scan_file for a plain file's contents d: bytes, or an mmap of a big file (slicing, `in`, find, re and struct read it the same; the Yaz0 and Unity readers take bytes, so those copy)"""
    if d[:4] == b'Yaz0': d = yaz0(d[:] if isinstance(d, (Mapped, MappedSpan)) else d)
    if path.endswith('.xxx') and d[:4] == struct.pack('<I', 0x9E2A83C1) or UE3_DNBC in d or d[:4] == b'BMSG':   # an Unreal Engine 3 package, or a file holding its shader blocks plain (GlobalShaderCache-NX.bin, -Switch.bin)
        u = ue3_package(d, '/' + rel) if path.endswith('.xxx') else d
        ue3_dnbc_shaders(u, '/' + rel, found); ue3_lz4_shaders(u, '/' + rel, found)
        return rel, uniq_keys(found), var_pairs(found), trees
    if d[:8] == b'UnityFS\0' or (d[8:11] == b'\0\0\0' and __import__('unityfs').is_serialized(d)):
        import unityfs
        try: unity_shaders(d[:] if isinstance(d, (Mapped, MappedSpan)) else d, '/' + rel, found, trees if not TREES else dict(TREES))
        except unityfs.NoTree as e:
            if SECOND: print(f'[RED] /{rel}: {e}', flush=True)
            return rel, {}, set(), 'defer'
    elif b'BNSH\0\0\0\0' not in d and b'BAHS' not in d and d[:4] != b'MSHD' and d[:4] != b'NDSR' and ns2.CTL_MAGIC + ns2.CTL_VERSION not in d and not (d[:4] == b'SARC' and b'Yaz0' in d) and b'\x91\x68\x86\x19' not in d and MGFX not in d: return rel, {}, set(), {}   # a SARC may hold Yaz0 members
    else:
        scan_blob(d, '/' + rel, found)
        # whole GLSLC outputs in a file no other reader takes (FINAL FANTASY TACTICS - The Ivalice Chronicles: its engine's technique files, resources_nx/…/shader/*.tec, 261 outputs beside
        # main's 49). Metroid Dread's MSHD files hold GLSLC outputs too, which mshd_shaders already reads; no other profile's loose files hold any
        if not found and d[:4] != b'MSHD' and b'\x91\x68\x86\x19' in d: found += glslc_shaders(d, '/' + rel, need=False)
    return rel, uniq_keys(found), var_pairs(found), trees

def uniq_keys(found):
    uniq = {}
    for where, st, ctl, code in found:
        key = skey(ctl, code)
        if key in uniq and not same_shader(uniq[key][2:4], (ctl, code)): red(f'key {key} is the same but the control section or code area differs ({where})')
        uniq.setdefault(key, (where, st, ctl, code))
    return uniq

def nso_image(path):
    import lz4.block
    d = romfs_list.open_file(path).read()
    flags = struct.unpack_from('<I', d, 0xc)[0]; segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return bytes(img)

def list_arg(s):
    """A RomFS / main list argument: comma-separated, or @<list file> with one path per line (11-05 Tianyan, Monster Hunter Rise with 273 add-on
    contents: the comma string passed Windows' 32767-character command-line limit and Python could not even start)"""
    if s.startswith('@'):
        with open(s[1:], encoding='utf-8') as f: return [l.rstrip('\r\n') for l in f if l.strip()]
    return s.split(',')

def main():
    romfs_l, main_l, pkg, mapout, factsout = sys.argv[1:6]
    pairsout = sys.argv[6] if len(sys.argv) > 6 else None
    # Several versions (base game + update): the RomFS and main arguments are comma-separated and paired one to one; the output is their union (keys are content hashes, shaders shared by versions appear once).
    # The first version's source column is unchanged; later versions are prefixed with their workspace directory name (e.g. v327680): /v327680/Shader/…, main@v327680. They sort after the first version, so existing rows keep their package indices.
    romfs_v, main_v = list_arg(romfs_l), list_arg(main_l)
    if len(romfs_v) != len(main_v): red(f'{len(romfs_v)} RomFS and {len(main_v)} main: they must pair one to one')
    files, common = [], None
    nsos = {}
    # 每个文件的结果一到就并进按键去重的总表 keys（先到先留，并入顺序同原先「全部收齐再按顺序并」：main → 大文件分段 → 逐文件 → 第二遍 Unity），
    #   只留小件（变体配对、类型树、丢弃记录）。原先整份 results 留到最后再并：同一支着色器在多个文件里各一份字节，再加 keys、拼包记录、join 各一份，
    #   主进程常驻是去重后着色器字节的三四倍（11-18 8 GB 机器 S 级升级实测抽取段超线）
    keys, pairs, skipped, file_red, same_key, trees, defer = {}, set(), [], [], [], {}, []
    per_src = collections.Counter()
    def fold(r):
        rel, uniq, pr, extra, sk = r
        if extra == 'defer': defer.append(rel); return
        if extra: trees.update(extra)
        pairs.update(pr)
        for x in sk: (file_red.append(x[1]) if x[0] == FILE_RED else skipped.append(x))
        for key, v in uniq.items():
            if key in keys:
                if not same_shader(keys[key][2:4], v[2:4]): same_key.append(f'key {key} is the same but the control section or code area differs ({v[0]} / {keys[key][0]}): kept the first')
                continue
            keys[key] = v; per_src[group(v[0])] += 1
    remaining = collections.Counter(os.path.realpath(p) for p in main_v)
    for i, (romfs, mainnso) in enumerate(zip(romfs_v, main_v)):
        tag = os.path.basename(os.path.dirname(os.path.abspath(romfs))) if i else ''
        files += sorted((os.path.join(dp, f), (tag + '/' if tag else '') + os.path.relpath(os.path.join(dp, f), romfs).replace(os.sep, '/')) for dp, _, fs in romfs_list.walk(romfs) for f in fs)
        path = os.path.realpath(mainnso)
        stat = os.stat(mainnso)
        identity = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
        cached = nsos.get(path)
        reuse = cached is not None and cached[0] == identity
        img = cached[1] if reuse else nso_image(mainnso)
        if not i:
            import unityfs
            try: common = unityfs.common_strings(img)
            except ValueError: common = None   # not a Unity game
        if reuse:
            found = list(cached[2])
        else:
            found = []; scan_blob(img, 'main' + ('@' + tag if tag else ''), found)
            found += glslc_shaders(img, 'main' + ('@' + tag if tag else ''), need=False)
            # Output keeps each key's first source; reuse that first NSO source, not per-root captures.
            nsos[path] = (identity, img, tuple(found))
        # GLSLC outputs the game compiles at run time, captured on the Mac (hle_glslc.c): <version dir>/glslc_capture, or DM_GLSLC_CAPTURE. A game needs them when main holds GLSL sources and
        # calls glslcCompile in a module it loads (Fire Emblem: Three Houses; profile_guess.py shader_sources). Captain Toad: Treasure Tracker imports it with agl's GLSL sources, but no module defines it
        cap = os.environ.get('DM_GLSLC_CAPTURE') or os.path.join(os.path.dirname(os.path.abspath(romfs)), 'glslc_capture')
        names = sorted(f for f in os.listdir(cap) if f.endswith('.glslc') and not f.startswith('._')) if os.path.isdir(cap) else []
        if not names and img.count(b'#version ') and 'glslcCompile' in main_imports(img) and glslc_module(os.path.dirname(os.path.abspath(mainnso))):
            print(f'[RED] main holds GLSL sources and imports glslcCompile, but there are no captured GLSLC outputs in {cap}: the shaders the game compiles at run time will have no translation', flush=True)
        for nm in names: found += glslc_shaders(romfs_list.open_file(os.path.join(cap, nm)).read(), f'/glslc_capture/{nm}')
        # Unreal Engine shader code archives decoded on the Mac (uepak.py): <version dir>/ue_shaders, or DM_UE_SHADERS; needed when the RomFS has Unreal paks
        ued = os.environ.get('DM_UE_SHADERS') or os.path.join(os.path.dirname(os.path.abspath(romfs)), 'ue_shaders')
        names = sorted(f for f in os.listdir(ued) if f.endswith('.ushaderbytecode') and not f.startswith('._')) if os.path.isdir(ued) else []
        if not names and uepak.unreal_paks(romfs, legacy=False): red(f'the RomFS has Unreal paks, but there are no decoded shader code archives in {ued} (the NSP script writes them)')
        for nm in names: found += ue_shaders(romfs_list.open_file(os.path.join(ued, nm)).read(), f'/ue_shaders/{nm}')
        fold(('main', {skey(c, k): (w, s, c, k) for w, s, c, k in found}, var_pairs(found), {}, []))
        remaining[path] -= 1
        if not remaining[path]: del nsos[path]
    zdicts = {k: v for r in romfs_v for k, v in zs_dicts(r).items()}   # Tears of the Kingdom's zstd dictionaries (zs_dicts)
    # First pass: every file; Unity bundles carry type trees, and the Shader trees are collected on the way. Second pass: Unity serialized files without type trees are scanned again with the borrowed trees
    from pool_workers import process_pool_size, env_jobs   # tools/ is on sys.path (progress, above)
    J = process_pool_size(env_jobs('J', 'S2A_JOBS'))   # workers = J (no J = S2A_JOBS / every core); at most 61 on Windows (#1332)
    big = [j for p, r in files for j in (linkdata_jobs(p, r) or [])]   # Three Houses' 6.9 GB DATA1.bin: one job per range of entries, not one read of the whole file
    pak_jobs, paks = kpka_jobs(files)   # Monster Hunter Rise's PAKs: the newest copy of each path, in ranges of entries
    tr_jobs, trp = trpfs_jobs(files)   # Pokémon Legends: Z-A's Trinity packs (data.trpfs and its index data.trpfd), decoded by the game's own Oodle in the Mac host
    ue_jobs, uep = ue_legacy_jobs(files, main_v)   # DRAGON QUEST XI S's legacy Unreal paks, decoded by the game's own LZ5 in the Mac host
    bd_jobs, bdts = bdt_jobs(files)   # Dark Souls Remastered's dvdbnd archives: the entries of each .bdt by its .bhd5 index, in ranges
    big += pak_jobs + tr_jobs + ue_jobs + bd_jobs
    if (tr_jobs or ue_jobs) and not (os.environ.get('DM_OODLE_HOST') and os.environ.get('DM_OODLE_MODULES')):   # the game's decoder (oodle): unset, take the defaults pack/ue_pak.py
        found, _ = uepak.oodle_host_find(romfs_v[0])                                                        # uses on the same tree (build/s2arm-host, main.fixups.tsv beside the RomFS)
        if found: os.environ['DM_OODLE_HOST'], os.environ['DM_OODLE_MODULES'] = map(str, found)            # workers inherit it; none found → oodle() reds when a file needs it
    files = [(p, r) for p, r in files if not linkdata_jobs(p, r) and p not in paks and p not in trp and p not in uep and p not in bdts]
    with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(common, {}, zdicts)) as ex:
        tk = Ticker('直连库·直连抽取', len(big) + len(files))
        for m in (ex.map(scan_counted, big), ex.map(scan_counted, files, chunksize=16)):
            for r in m: fold(r); tk.step()
    if defer:
        # a hash no bundle supplied takes the layout unityfs.py has for it (KNOWN_TREES), then the one generated from the file's Unity version (SECOND); only here, so files a bundle lends
        # a tree to keep their place in the tables (Ace Attorney's built-ins)
        import unityfs
        for k in unityfs.KNOWN_TREES: trees.setdefault(k, unityfs.known_tree(*k))
        byrel = {r: p for p, r in files}
        first, defer[:] = defer[:], []
        with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(common, trees, zdicts, True)) as ex:
            for r in ex.map(scan_counted, [(byrel[r], r) for r in first]):
                if r[3] == 'defer': red(f'{r[0]}: Unity serialized file without type trees, no bundle has a Shader type tree of the same hash to borrow, and no layout comes from its Unity version (see the [RED] above)')
                fold(r)
    if skipped and not keys: red(f'every Unity program block was left out ({len(skipped)}, the first: {skipped[0][0]}: {skipped[0][1]}): a reader that is wrong for this game, not bad blocks')
    # 降级汇总（v0.3.1）：读不了的文件 / 同键不同内容逐条打 [WARN]，写 <shader_map.tsv 同目录>/degrade.tsv（pack/stage_extract.sh 并进 degrade.extract.tsv，
    #   出包收尾存进任务日志目录、诊断包收它）。#1122：被丢的 Unity 程序块原先只在 split.log 打 [RED]，诊断里看不到——两位玩家 1.2.0 映射表比 1.1.0 少 53 条、无从定位
    for m in file_red: print('[WARN] skipped: ' + m, flush=True)
    for m in same_key: print('[WARN] ' + m, flush=True)
    write_degrade(os.path.join(os.path.dirname(os.path.abspath(mapout)), 'degrade.tsv'), file_red, [f'{w}: {why}' for w, why in skipped], same_key)
    for line in skip_summary(file_red, skipped, same_key): print(line, file=sys.stderr)
    sp, cover = separable_pairs(keys); pairs |= sp
    if cover: print(f'single-stage programs: {len({v for v, _ in sp}):,} vertex shaders are paired with a cover of {len(cover)} pixel shader(s) ({" ".join(cover)})')
    os.makedirs(pkg, exist_ok=True)
    for f in os.listdir(pkg):
        if f.endswith('.bin'): os.unlink(os.path.join(pkg, f))
    # 包文件逐条写（原先每条记录先进 recs、最后每个包 b''.join：着色器字节在内存里又多两份）；同一包内的顺序与序号同原先
    outs = {}; nrec = collections.Counter(); rows = []; facts = {}; stat = collections.Counter()
    try:
      for key in sorted(keys, key=lambda k: (group(keys[k][0]), keys[k][0], k)):   # the same input gives byte-identical output
        where, st, ctl, code = keys[key]
        name = f'{st}_{group(where)}.bin'
        if name not in outs: outs[name] = open(os.path.join(pkg, name), 'wb')
        outs[name].write(pkg_record(key, ctl, code))
        rows.append((name, nrec[name], STAGE_NAME[st], key, where)); nrec[name] += 1; stat[st] += 1
        if st == 'cs':
            nvn2 = ns2.is_nvn2(ctl)
            (x, y, z), sh = ns2.compute_facts(ctl) if nvn2 else (struct.unpack_from('<3I', ctl, 0x748), struct.unpack_from('<I', ctl, 0x754)[0])
            if not x * y * z: red(f'compute shader {key}: control +{0x58 if nvn2 else 0x748:#x} thread group is 0 ({where})')
            facts[key] = {'local': [x, y, z], 'shared': (sh + 15) // 16 * 16}
    finally:
        for f in outs.values(): f.close()
    with open(mapout, 'w') as f:
        kinds = ' / '.join(k for k, hit in (('BNSH / BAHS', any('#BNSH@' in r[4] or '#BAHS@' in r[4] for r in rows)), ('Unity Shader', any('#var' in r[4] and '#BNSH@' not in r[4] and '#MSHD#' not in r[4] and '#SDF#' not in r[4] for r in rows)),
                                            ('MSHD', any('#MSHD#' in r[4] for r in rows)), ('GLSLC', any('#GLSLC@' in r[4] or '#GLSLC2@' in r[4] for r in rows)), ('RE Engine SDF', any('#SDF#' in r[4] for r in rows)),
                                            ('MT Framework SPK', any('#MTSPK@' in r[4] for r in rows)), ('UE3 DNBC', any('#DNBC@' in r[4] for r in rows)), ('UE3 LZ4 records', any('#LZ4UE3@' in r[4] for r in rows)), ('MonoGame MGFX', any('#MGFX@' in r[4] for r in rows)),
                                            ('Unreal Engine shader code archives', any('#UE' in r[4] for r in rows))) if hit)
        f.write(f'# kind: evidence  {NOTE} ({kinds} in RomFS + main) → control section + code area keys (dm/gfx/tools/shader_split.py)\n')
        f.write('# column 5 records where each key first appears; this game has no PC version to pair with\n')
        f.write('package\tindex\tstage\tdm_ctl_sha16\tsource\n')
        for r in rows: f.write('\t'.join(map(str, r)) + '\n')
    # Hand-written facts the machine code cannot give (the profile's shader_facts_manual.tsv, same row format) are merged over the generated ones, so the table stays reproducible
    manual = GAME.table('shader_facts_manual.tsv')
    if romfs_list.exists(manual):
        for ln in open(manual):
            if ln.strip() and not ln.startswith('#'): k, v = ln.rstrip('\n').split('\t', 1); facts[k] = {**facts.get(k, {}), **json.loads(v)}
    with open(factsout, 'w') as f:
        f.write('# Answer table for translation queries (generated by dm/gfx/tools/shader_split.py, do not edit): key <TAB> fact JSON. This game has no PC version to pair with: only the thread group / shared memory of compute shaders (taken from the control section); other queries get the translator defaults\n')
        for k in sorted(facts): f.write(k + '\t' + json.dumps(facts[k], sort_keys=True, separators=(',', ':')) + '\n')
    if pairsout:
        with open(pairsout, 'w') as f:
            f.write('# kind: evidence  vertex / pixel shader pairs within BNSH variations (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n')
            for a, b in sorted(pairs): f.write(f'{a}\t{b}\n')
    print(f'shaders: {len(rows)} (' + ', '.join(f'{STAGE_NAME[s]} {n}' for s, n in sorted(stat.items())) + '); sources ' + ', '.join(f'{g} {n}' for g, n in per_src.most_common()) + f'; compute shader answers {len(facts)} rows' + (f'; variation pairs {len(pairs)}' if pairsout else '') + (f'; Unity program blocks left out {len(skipped)}' if skipped else ''))

if __name__ == '__main__': main()
