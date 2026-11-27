#!/usr/bin/env python3
"""Host contract for production 3D buffer/image copy bounds; Vulkan calls are recorded only."""
from pathlib import Path
import os
import re
import sys
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'android/core-link'))
import link_symbols
src = (root / 'android/native/nvn/backend_vulkan.c').read_text()
match = re.search(r'static void bc_readback\([^;]+\);', src)
if not match:
    raise RuntimeError('production bc_readback declaration not found')
bc_signature = match.group(0)[:-1]
match = re.search(r'static void bc_copy_from_buf\([^;]+\);', src)
if not match:
    raise RuntimeError('production bc_copy_from_buf declaration not found')
bc_upload_signature = match.group(0)[:-1]
d24 = (root / 'android/native/nvn/backend_vulkan_d24_readback.inc').read_text()
match = re.search(r'static void d24_readback_copy\([^)]*\) \{', d24)
if not match:
    raise RuntimeError('production d24_readback_copy definition not found')
d24_readback_signature = match.group(0)
ledger = (root / 'android/native/nvn/backend_vulkan_cold_ledger.inc').read_text()
match = re.search(r'static void cold_note_write\([^)]*\) \{', ledger)
if not match:
    raise RuntimeError('production cold_note_write definition not found')
cold_write_signature = match.group(0)
def function(signature):
    start = src.index(signature)
    brace = src.index('{', start)
    depth, pos = 1, brace + 1
    while depth:
        depth += (src[pos] == '{') - (src[pos] == '}')
        pos += 1
    return src[start:pos]

preamble = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <vulkan/vulkan.h>
#include "nvn_backend.h"
#include "nvn_fmt_info.h"
#define REF_TEX 1
#define REF_BUF 2
struct nvn_gpu_tex_s { uint32_t w,h,depth,levels,target,layers,format,nvn_format,aspect; VkImage image; int bc_decoded; };   /* bc_decoded：压缩贴图现解；本测试的贴图都不是 */
struct nvn_gpu_buf_s { VkBuffer buffer; uint64_t size; };
struct nvn_gpu_cmd_s { int sticky,in_pass,xfer_dirty; VkCommandBuffer cb; };
static const struct { VkPipeline pipe; } g_d24_readback = {.pipe = VK_NULL_HANDLE};
static int copies, bc_calls, bc_upload_calls; static VkBufferImageCopy last;
static unsigned cold_writes;
static struct {
    nvn_gpu_cmd cmd;
    nvn_gpu_buf buf;
    uint64_t off, bytes;
    unsigned writer;
    int exact;
} last_cold_write;
static int nvn_vk_format_substituted(uint32_t f) { (void)f; return 0; }   /* #398 D24 换 D32：本测试的贴图都不换存储 */
static int rc_is(int got, int expected) { return got == expected; }
static void set_sticky(struct nvn_gpu_cmd_s *c, int rc, const char *why, ...) { (void)why; c->sticky = rc; }
static uint32_t fmt_bpp(uint32_t fmt) { (void)fmt; return 4; }
static int nvn_fmt_expand_target(uint32_t f, uint32_t *target, int *bpp) { (void)f; (void)target; (void)bpp; return 0; }
static int buf_range(struct nvn_gpu_buf_s *b, uint64_t off, uint64_t n) { return off <= b->size && n <= b->size-off; }
static void full_barrier(struct nvn_gpu_cmd_s *c) { (void)c; }
static void image_barrier(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, int l) { (void)c; (void)t; (void)l; }
static void cmd_ref(struct nvn_gpu_cmd_s *c, void *p, int type) { (void)c; (void)p; (void)type; }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout layout, uint32_t n, const VkBufferImageCopy *r) { (void)cb; (void)b; (void)i; (void)layout; (void)n; copies++; last=*r; }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout layout, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) { (void)cb; (void)i; (void)layout; (void)b; (void)n; copies++; last=*r; }
'''
# This test does not implement BC shadow readback; fail closed if it is reached.
bc_stub = bc_signature + r''' {
    (void)c; (void)t; (void)level; (void)layer; (void)r; (void)b; (void)off; (void)row_bytes; (void)image_bytes;
    bc_calls++; set_sticky(c, NVN_GPU_E_UNSUPPORTED, "BC readback not implemented by bounds fixture");
}
'''
# The new decoded-BC upload path is outside this bounds fixture too.
bc_upload_stub = bc_upload_signature + r''' {
    (void)c; (void)t; (void)level; (void)layer; (void)r; (void)b; (void)off; (void)row_bytes; (void)image_bytes;
    bc_upload_calls++; set_sticky(c, NVN_GPU_E_UNSUPPORTED, "BC upload not implemented by bounds fixture");
}
'''
# 夹具覆盖原生拷贝的深度边界，替换存储的转换由独立 Vulkan 用例覆盖。
d24_readback_stub = d24_readback_signature + r'''
    (void)t; (void)level; (void)layer; (void)region; (void)dst;
    (void)dst_off; (void)row; (void)image_bytes;
    set_sticky(c, NVN_GPU_E_UNSUPPORTED, "D24 conversion not implemented by bounds fixture");
}
'''
# Record production's handoff to the write ledger at the fixture boundary.
cold_write_stub = cold_write_signature + r'''
    cold_writes++;
    last_cold_write.cmd = c;
    last_cold_write.buf = buf;
    last_cold_write.off = off;
    last_cold_write.bytes = bytes;
    last_cold_write.writer = writer;
    last_cold_write.exact = exact;
}
'''
# #1581 K：一个拷贝区域只取一个面——面选择（copy_face_aspect）与 copy_buf_tex_impl 的 face 取值（COPY_FACE_*）在函数外，一起切出来
match = re.search(r'enum \{ COPY_FACE_DEFAULT[^}]*\};', src)
if not match:
    raise RuntimeError('production COPY_FACE_* enum not found')
face_enum = match.group(0)
production = face_enum + '\n' + '\n'.join(function(x) for x in (
    'static VkImageAspectFlags copy_face_aspect(',
    'static const char *readback_refusal(',   # copy_tex_buf 先问它拒不拒收（展开格式 / 替换存储，#1715）；与 NVN_GPU_FMT_READBACK 同一个判断
    'static void copy_region_block_clamp(', 'static int copy_region(', 'static int check_copy_params(',   # block_clamp：#697 小 mip 整块区域裁边，copy_region 调它
    'static void copy_buf_tex_impl(', 'void nvn_gpu_copy_buf_tex(', 'void nvn_gpu_copy_tex_buf('))   # copy_buf_tex_impl：#422 起 nvn_gpu_copy_buf_tex 是它的薄壳
main = r'''
static int check_case(int direction, unsigned layer, const NVNcopyRegion *region, int expected, const char *name) {
    struct nvn_gpu_tex_s t = {.w=4,.h=4,.depth=4,.levels=3,.target=NVN_TEXTURE_TARGET_3D,.layers=1};
    struct nvn_gpu_buf_s b = {.size=128}; struct nvn_gpu_cmd_s c = {0};
    copies = 0; cold_writes = 0; memset(&last, 0, sizeof last);
    if (direction == 0) nvn_gpu_copy_buf_tex(&c, &b, 4, 8, 32, &t, 1, layer, region);
    else nvn_gpu_copy_tex_buf(&c, &t, 1, layer, region, &b, 4, 8, 32);
    if (expected) {
        if (c.sticky != NVN_GPU_OK || copies != 1 || last.imageOffset.z != (int)layer ||
            last.imageExtent.width != 2 || last.imageExtent.height != 2 ||
            last.imageExtent.depth != (uint32_t)(region && region->depth > 0 ? region->depth : 1)) {
            fprintf(stderr, "%s direction=%d: expected valid copy, sticky=%d calls=%d\n", name, direction, c.sticky, copies); return 1;
        }
    } else if (c.sticky != NVN_GPU_E_ARG || copies != 0) {
        fprintf(stderr, "%s direction=%d: expected E_ARG and no Vulkan copy, sticky=%d calls=%d\n", name, direction, c.sticky, copies); return 1;
    }
    unsigned expected_writes = expected && direction == 1;
    if (cold_writes != expected_writes ||
        (expected_writes && (last_cold_write.cmd != &c || last_cold_write.buf != &b ||
         last_cold_write.off != 4 || last_cold_write.bytes != 32 ||
         last_cold_write.writer != 0 || last_cold_write.exact))) {
        fprintf(stderr, "%s direction=%d: copy write ledger does not match the destination span\n", name, direction); return 1;
    }
    return 0;
}
int main(void) {
    NVNcopyRegion two_slices = {.depth=2}; int errors = 0;
    for (int dir=0; dir<2; dir++) {
        errors += check_case(dir, 1, NULL, 1, "last mip slice");
        errors += check_case(dir, 2, NULL, 0, "slice beyond mip");
        errors += check_case(dir, 0, &two_slices, 1, "two slices end at mip limit");
        errors += check_case(dir, 1, &two_slices, 0, "depth crosses mip limit");
    }
    struct nvn_gpu_tex_s bc = {.w=4,.h=4,.depth=4,.levels=3,.target=NVN_TEXTURE_TARGET_3D,.layers=1,.bc_decoded=1};
    struct nvn_gpu_buf_s b = {.size=128}; struct nvn_gpu_cmd_s c = {0}; copies=0; bc_calls=0;
    nvn_gpu_copy_tex_buf(&c, &bc, 1, 0, NULL, &b, 0, 8, 32);
    if (bc_calls != 1 || copies || c.sticky != NVN_GPU_E_UNSUPPORTED) {
        fprintf(stderr,"FAIL BC boundary must report unsupported without a Vulkan copy\n"); errors++;
    }
    c.sticky=NVN_GPU_OK; copies=0; bc_upload_calls=0;
    nvn_gpu_copy_buf_tex(&c, &b, 0, 8, 32, &bc, 1, 0, NULL);
    if (bc_upload_calls != 1 || copies || c.sticky != NVN_GPU_E_UNSUPPORTED) {
        fprintf(stderr,"FAIL BC upload boundary must report unsupported without a Vulkan copy\n"); errors++;
    }
    if (errors) return 1;
    puts("3D copy mip depth bounds: both directions, valid controls and BC boundaries passed"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='copy-3d-depth-') as path:
    d = Path(path)
    ndk = link_symbols.find_ndk()
    if ndk is None: raise RuntimeError('NDK 29 headers required')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    sysinc = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include'
    inc = d / 'include'; inc.mkdir()
    for name in ('vulkan', 'vk_video'):
        if (sysinc / name).exists(): (inc / name).symlink_to(sysinc / name)
    def run(name, stub, body, expected=None):
        c_file = d / (name + '.c'); exe = d / name
        c_file.write_text(preamble + stub + bc_upload_stub + d24_readback_stub + cold_write_stub + body + main)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(inc), '-I' + str(root/'graphics/nvn/gfx/nvn'),
                        '-I' + str(root/'graphics/nvn/gfx/nvn/include'),
                        str(c_file), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        output = result.stdout + result.stderr
        if expected is None:
            if result.returncode: raise RuntimeError('positive behavior failed: ' + output)
            print(output, end='')
        elif result.returncode != 1 or expected not in output:
            raise RuntimeError(name + ': mutation did not fail expected assertion: ' + output)
        else:
            print(name + ': expected assertion failure: ' + expected)
    run('positive', bc_stub, production)
    original = 'set_sticky(c, NVN_GPU_E_UNSUPPORTED, "BC readback not implemented by bounds fixture");'
    assert bc_stub.count(original) == 1
    run('withdraw-bc-error', bc_stub.replace(original, '(void)c;'), production,
        'FAIL BC boundary must report unsupported without a Vulkan copy')
    original = 'if (off->z < 0 || (uint32_t)off->z >= d || ext->depth > d - (uint32_t)off->z) return -1;'
    assert production.count(original) == 1
    run('withdraw-depth-bounds', bc_stub, production.replace(original, 'if (0 && (off->z < 0 || (uint32_t)off->z >= d || ext->depth > d - (uint32_t)off->z)) return -1;'),
        'expected E_ARG and no Vulkan copy')
    original = 'cold_note_write(c, b, off, image_bytes, 0, 0);'
    assert production.count(original) == 1
    run('move-ledger-span', bc_stub, production.replace(original, 'cold_note_write(c, b, off + 4, image_bytes, 0, 0);'),
        'copy write ledger does not match the destination span')
