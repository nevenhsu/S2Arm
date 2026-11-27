// SPDX-License-Identifier: Apache-2.0
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_exec.h"
#include "rp_res.h"

void nvn_be_tex_cpu_inferred(const tex_t *t);
int rp_tm_on;
static nvn_gpu_cmd active;
static uint64_t fixture_fence_value;
void rp_perf_add(int i, uint64_t n) { (void)i; (void)n; }
uint64_t rp_sub_serial(void) { return fixture_fence_value + 1; }
uint64_t rp_seg_serial(void) { return fixture_fence_value + 1; }
int rp_buf_gpu_pending(uint64_t addr, uint64_t n) { (void)addr; (void)n; return 0; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *sub) { *sub = rp_sub_serial(); return active; }

static void finish(void) {
    nvn_gpu_err e = {0};
    ++fixture_fence_value;
    CHECK(OK(nvn_gpu_cmd_commit(active, fixture_fence_value, on_done, NULL, &e)), "commit %s", e.why);
    CHECK(OK(nvn_gpu_fence_wait(fixture_fence_value, 5000000000ull, &e)), "wait %s", e.why);
    active = NULL;
}

static void check_pixels(nvn_gpu_pipe pipe, nvn_gpu_samp sampler, int id, const char *label,
                         int rg, int written) {
    nvn_gpu_buf result = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
    active = begin_cmd();
    nvn_gpu_view input = rp_view_by_id(id, active, NULL);
    CHECK(input, "registered sample view %s", label);
    nvn_gpu_binding binds[2] = {{.ent=0,.cls=NVN_GPU_RES_TEX_SAMP},{.ent=1,.cls=NVN_GPU_RES_SSBO}};
    binds[0].u.ts.view = input;
    binds[0].u.ts.samp = sampler;
    binds[1].u.b.buf = result;
    binds[1].u.b.len = 32;
    nvn_gpu_set_pipe(active, pipe);
    nvn_gpu_bind(active, NVN_GPU_SH_CS, binds, 2);
    nvn_gpu_dispatch(active, pipe, 2,1,1,NULL,0);
    finish();
    const float *got = (const float *)buf_cpu(result);
    for (int i=0; i<2; ++i) {
        const float want[4] = {written ? i+1 : 0, written ? -2 : 0, written && !rg ? 3 : 0,
                               rg ? 1.0f : written ? 0.5f : 0.0f};
        CHECK(!memcmp(&got[4*i],want,sizeof want), "%s texel %d want %g/%g/%g/%g got %g/%g/%g/%g",
              label,i,want[0],want[1],want[2],want[3],got[4*i],got[4*i+1],got[4*i+2],got[4*i+3]);
    }
    nvn_gpu_buf_release(result,NULL,NULL);
}

static void volume_case(const char *dir, const char *variant, int rg, int flat, int early) {
    nvn_gpu_layout_ent ent[2] = {
        {.set=0,.binding=0,.count=1,.cls=NVN_GPU_RES_IMAGE,.access=NVN_GPU_ACC_W,.stages=1u<<NVN_GPU_SH_CS},
        {.set=0,.binding=1,.count=1,.cls=NVN_GPU_RES_SSBO,.access=NVN_GPU_ACC_W,.stages=1u<<NVN_GPU_SH_CS}
    };
    nvn_gpu_layout layout = {.ent=ent,.nent=1};
    char name[64];
    snprintf(name,sizeof name,"%s-store.comp",variant);
    nvn_gpu_shader st=NULL, sm=NULL;
    nvn_gpu_pipe store=mk_cpipe(dir,name,&layout,&st);
    snprintf(name,sizeof name,"%s-sample.comp",variant);
    ent[0].cls=NVN_GPU_RES_TEX_SAMP; ent[0].access=NVN_GPU_ACC_R; layout.nent=2;
    nvn_gpu_pipe sample=mk_cpipe(dir,name,&layout,&sm);
    CHECK(store && sample,"compiled both %s shaders",variant);
    if (!store || !sample) return;
    nvn_gpu_samp sampler=NULL; samp_par params={0}; nvn_gpu_err e={0};
    CHECK(OK(nvn_gpu_samp_create(&params,&sampler,&e)),"create sampler");
    pool_t pool={0};
    tex_t writer={.serial=1,.pool=&pool};
    writer.d=(nvn_texdesc){.target=flat ? NVN_TEXTURE_TARGET_2D : NVN_TEXTURE_TARGET_3D,
        .format=rg ? NVN_FORMAT_RG16F : NVN_FORMAT_RGBA16F,.width=4,.height=4,
        .depth=flat ? 1u : 4u,.levels=1,.flags=NVN_TEXTURE_FLAGS_IMAGE};
    nvn_tex_layout(&writer.d,&writer.L);
    pool.size=2*writer.L.size;
    pool.mem=calloc(1,pool.size);
    if (!pool.mem) abort();
    writer.mem=pool.mem;
    tex_t reader=writer, other=writer;
    reader.serial=2; other.serial=3;
    other.mem+=writer.L.size; other.off=(int64_t)writer.L.size;
    nvn_be_reg_tex(1,&writer,NULL); nvn_be_reg_tex(2,&reader,NULL); nvn_be_reg_tex(3,&other,NULL);
    if (early == 2) {
        nvn_be_tex_cpu_inferred(&reader);
        check_pixels(sample,sampler,2,"CPU alias materialized first",rg,0);
    }
    active=begin_cmd();
    CHECK(rp_tex_resolve(&writer,active),"writer materialized");
    finish();
    if (early != 2) nvn_be_tex_cpu_inferred(&reader);
    if (early == 1) check_pixels(sample,sampler,2,"reader before GPU write",rg,0);
    active=begin_cmd();
    nvn_gpu_binding out={.ent=0,.cls=NVN_GPU_RES_IMAGE};
    out.u.view=rp_view_by_id(1,active,NULL);
    nvn_gpu_set_pipe(active,store); nvn_gpu_bind(active,NVN_GPU_SH_CS,&out,1);
    nvn_gpu_dispatch(active,store,2,1,1,NULL,0);
    rp_tex_mark_gpu_written(&writer);
    nvn_gpu_barrier(active,NVN_BARRIER_ORDER_PRIMITIVES_BIT|NVN_BARRIER_INVALIDATE_TEXTURE_BIT);
    finish();
    check_pixels(sample,sampler,1,"writer control",rg,1);
    check_pixels(sample,sampler,2,variant,rg,1);
    check_pixels(sample,sampler,3,"other offset control",rg,0);
    // A later explicit CPU update through the reader must win over the previous GPU store.
    memset(reader.mem,0,reader.L.size);
    tex_t cpu_writer = reader;
    if (early == 2) cpu_writer.serial = 4;  // WriteTexels may precede the first descriptor bind.
    nvn_be_tex_cpu_written(&cpu_writer);
    check_pixels(sample,sampler,2,"newer CPU write",rg,0);
    check_pixels(sample,sampler,1,"newer CPU write through alias",rg,0);
    rp_res_reset();
    free(pool.mem);
    nvn_gpu_samp_release(sampler); nvn_gpu_shader_release(st); nvn_gpu_shader_release(sm);
}
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    char why[256]={0};
    if(s2a_vk_device_create(&g_dev,why,sizeof why)) { fprintf(stderr,"%s\n",why); return 2; }
    printf("device: %s\n",g_dev.properties.deviceName);
    g_desc=(nvn_gpu_open_desc){.host_device=&g_dev,.app_label="storage-alias"};
    if(!open_backend()) return 2;
    volume_case(argv[1],"RG3D",1,0,0);
    volume_case(argv[1],"RGBA3D",0,0,1);
    volume_case(argv[1],"RGBA2D",0,1,0);
    volume_case(argv[1],"RG3D",1,0,2);
    nvn_gpu_err e={0};
    CHECK(OK(nvn_gpu_close(5000000000ull,&e)),"close");
    s2a_vk_device_destroy(&g_dev,1);
    printf("replay storage alias failures=%d\n",g_fail);
    return g_fail!=0;
}
