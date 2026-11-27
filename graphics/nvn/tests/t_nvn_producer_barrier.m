// Real production encoder wrappers, real dependent GPU work, independent bytes.
#ifndef NVN_PRODUCER_SOURCE
#define NVN_PRODUCER_SOURCE "../gfx/nvn/nvn_mtl.m"
#endif
#include NVN_PRODUCER_SOURCE

static unsigned checks, failures;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"SETUP FAIL %d: %s\n",__LINE__,#c); exit(2); } } while(0)
#define CHECK(c, text) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL %s\n",text); } } while(0)

static id<MTLComputePipelineState> add_pipeline;
static id<MTLRenderPipelineState> color_pipeline;
static __typeof__(&nvn_gpu_compute_to_graphics) publish_compute;

// Resolve the actual production exports: a missing contract entry must fail an
// assertion at runtime, not stop at a link error. Types and names come from the
// real contract header; pool_release is the pre-existing loader control.
static void contract_entries(void) {
    CHECK(dlsym(RTLD_DEFAULT,"nvn_gpu_pool_release")!=NULL,"existing Metal contract export is visible");
#define LOAD(fn) (__typeof__(&fn))dlsym(RTLD_DEFAULT,#fn)
    __typeof__(&nvn_gpu_buf_access_span) buf_span=LOAD(nvn_gpu_buf_access_span);
    __typeof__(&nvn_gpu_view_access_span) view_span=LOAD(nvn_gpu_view_access_span);
    publish_compute=LOAD(nvn_gpu_compute_to_graphics);
#undef LOAD
    CHECK(buf_span!=NULL,"Metal exports nvn_gpu_buf_access_span");
    CHECK(view_span!=NULL,"Metal exports nvn_gpu_view_access_span");
    CHECK(publish_compute!=NULL,"Metal exports nvn_gpu_compute_to_graphics");
    // The legacy Metal replay cannot construct these opaque contract handles.
    // Unknown is 0, never a fabricated identity/success; clear stale output.
    nvn_gpu_access_span span; unsigned char zero[sizeof span]={0};
    if(buf_span) {
        memset(&span,0xa5,sizeof span);
        CHECK(buf_span(NULL,0,16,&span)==0 && !memcmp(&span,zero,sizeof span),"unknown buffer span returns 0 and clears output");
        CHECK(buf_span(NULL,0,16,NULL)==0,"buffer span permits missing output");
    }
    if(view_span) {
        memset(&span,0xa5,sizeof span);
        CHECK(view_span(NULL,&span)==0 && !memcmp(&span,zero,sizeof span),"unknown view span returns 0 and clears output");
        CHECK(view_span(NULL,NULL)==0,"view span permits missing output");
    }
}

static void add_seven(id<MTLBuffer> data) {
    id e=enc_c_begin(NULL);
    enc_c_args(e,data.gpuAddress,0); enc_c_pso(e,add_pipeline);
    enc_c_dispatch(e,MTLSizeMake(256,1,1),MTLSizeMake(1,1,1));
    enc_c_end(e);
}

static void color(id<MTLTexture> target,id<MTLBuffer> data) {
    // Metal's legacy replay has no nvn_gpu_cmd wrapper. The compatibility hook
    // must leave its existing encoder barriers in charge, without a submit/wait.
    id cb=g_cb, blit=g_blit; uint64_t fence=g_fence, full=g_bar_full;
    if(publish_compute) publish_compute(NULL);
    CHECK(g_cb==cb && g_blit==blit && g_fence==fence && g_bar_full==full,"contract hook does not cut or submit the Metal command stream");
    MTL4RenderPassDescriptor *d=pass_desc();
    d.colorAttachments[0].texture=target;
    d.colorAttachments[0].loadAction=MTLLoadActionDontCare;
    d.colorAttachments[0].storeAction=MTLStoreActionStore;
    id e=enc_r_begin_pass(d);
    enc_r_pso(e,color_pipeline);
    enc_r_args(e,MTLRenderStageFragment,data.gpuAddress,0);
    enc_r_draw(e,MTLPrimitiveTypeTriangle,0,3);
    g_stat_draw++; // This direct encoder draw participates in the real cut budget.
    enc_r_end_pass(e);
    if(!g_m3 && barrier_game()) CHECK(!g_bar_main_full,"relaxed producer cannot certify full coverage");
}

static void read_pixels(id<MTLTexture> target,id<MTLBuffer> out) {
    id e=blit_open_k(0);
    enc_b_copy_texture_buffer(e,target,0,0,MTLOriginMake(0,0,0),MTLSizeMake(4,4,1),out,0,256,1024);
    enc_b_end(e); g_blit=nil;
}

static void finish(void) {
    uint64_t fence=nvn_be_sub_commit();
    REQUIRE(nvn_be_wait(fence,10000000000ull)); REQUIRE(!mtl_last_commit_error());
    CHECK(!g_bar_main_cb && !g_bar_main_encoder && !g_bar_main_full,"final commit clears same-CB coverage");
}

static void pixels(id<MTLBuffer> out,uint32_t expected) {
    const uint32_t *v=out.contents;
    unsigned wrong=0;
    for(unsigned y=0;y<4;y++) for(unsigned x=0;x<4;x++) wrong+=v[y*64+x]!=expected;
    CHECK(!wrong,"render -> copy -> compute produces all 16 independently expected pixels");
    if(wrong) fprintf(stderr,"expected %08x, first %08x, wrong %u\n",expected,v[0],wrong);
}

int main(int argc,char **argv) {
    @autoreleasepool {
        contract_entries();
        REQUIRE(!setenv("DM_NVN_CUT","1",1));
        mtl_init();
        printf("backend=%s recorder=%d cover=%d\n",g_m3?"Metal3":"Metal4",rec_on(),producer_cover_on());
        NSError *error=nil;
        NSString *msl=@"#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void add7(device uint *v [[buffer(0)]],uint i [[thread_position_in_grid]]) { v[i]+=7; }\n"
          "vertex float4 triangle(uint i [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
          "fragment float4 colors(device const uint *v [[buffer(0)]]) { return float4(v[0],v[1],v[2],255)/255.0f; }\n";
        id<MTLLibrary> lib=[g_dev newLibraryWithSource:msl options:nil error:&error];
        if(!lib) fprintf(stderr,"MSL: %s\n",error.localizedDescription.UTF8String); REQUIRE(lib);
        add_pipeline=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"add7"] error:&error]; REQUIRE(add_pipeline);
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
        pd.vertexFunction=[lib newFunctionWithName:@"triangle"]; pd.fragmentFunction=[lib newFunctionWithName:@"colors"];
        pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        color_pipeline=[g_dev newRenderPipelineStateWithDescriptor:pd error:&error]; REQUIRE(color_pipeline);
        id<MTLBuffer> data=[g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
        id<MTLBuffer> out=[g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; REQUIRE(data && out);
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        td.storageMode=MTLStorageModePrivate; td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        id<MTLTexture> target=[g_dev newTextureWithDescriptor:td]; REQUIRE(target);
        res_add(data); res_add(out); res_add(target);

        uint32_t *v=data.contents; for(unsigned i=0;i<256;i++) v[i]=5; v[1]=27; v[2]=49;
        memset(out.contents,0xa5,1024);
        uint64_t covered_before=g_bar_covered;
        nvn_be_sub_begin(); CHECK(!g_bar_main_full,"first encoder has no inherited producer");
        add_seven(data); color(target,data); read_pixels(target,out); add_seven(out); finish();
        pixels(out,0xff38220cu+7); // (5,27,49)+7 -> RGBA(12,34,56,255), then uint +7.
        CHECK(g_bar_covered-covered_before==(g_m3||!producer_cover_on()?0u:barrier_game()?2u:3u),"only full-producer same-CB successor entries are covered");

        nvn_be_sub_begin();
        add_seven(data);
        uint64_t prior=g_bar_covered;
        mdi_wait(); // Real mid-submission commit, event wait and fresh main CB.
        CHECK(!g_bar_main_full,"mdi_wait cannot carry a producer across command buffers");
        color(target,data);
        CHECK(g_bar_covered==prior,"first render after mdi_wait keeps its consumer");
        read_pixels(target,out); add_seven(out); finish();
        pixels(out,0xff3f2913u+7); // Input now (19,41,63).

        // An independently encoded upload ends while the main CB is still open.
        nvn_be_sub_begin(); add_seven(data);
        if(!g_m3) {
            MTL4CommandAllocatorDescriptor *ad=[MTL4CommandAllocatorDescriptor new];
            id<MTL4CommandAllocator> allocator=[g_dev newCommandAllocatorWithDescriptor:ad error:&error]; REQUIRE(allocator);
            id<MTL4CommandBuffer> upload=[g_dev newCommandBuffer]; REQUIRE(upload);
            [upload beginCommandBufferWithAllocator:allocator];
            id e=[upload computeCommandEncoder]; enc_full_barrier_begin(e);
            [e copyFromBuffer:data sourceOffset:0 toBuffer:out destinationOffset:0 size:1024];
            m4_end(e);
            CHECK(!g_bar_main_full,"foreign upload cannot certify the main CB");
            e=enc_c_begin_on(upload,YES);
            [e copyFromBuffer:data sourceOffset:0 toBuffer:out destinationOffset:0 size:1024];
            enc_end_on(e,YES);
            CHECK(!g_bar_main_full,"presentation/capture wrappers cannot certify the main CB");
            [upload endCommandBuffer];
            res_commit(); [g_q4 commit:&upload count:1];
            [g_q4 signalEvent:g_mdi_ev value:++g_mdi_v];
            REQUIRE([g_mdi_ev waitUntilSignaledValue:g_mdi_v timeoutMS:10000]);
            // No ordering assumption about uncommitted main work: overwrite
            // the output through the real main render/copy chain below.
        }
        color(target,data); read_pixels(target,out); add_seven(out); finish();
        pixels(out,0xff46301au+7); // Main input now (26,48,70).

        nvn_be_sub_begin(); sub_cut(); // Start the actual one-draw segment budget.
        add_seven(data); color(target,data);
        uint64_t cuts_before=g_cuts;
        sub_cut(); // The real draw above fills the budget; commit without a CPU wait.
        CHECK(g_cuts==cuts_before+1,"actual draw budget cuts the command buffer");
        CHECK(!g_bar_main_full,"sub_cut cannot carry coverage into a new CB");
        prior=g_bar_covered; read_pixels(target,out);
        CHECK(g_bar_covered==prior,"first copy after sub_cut keeps its consumer");
        add_seven(out); finish(); pixels(out,0xff4d3721u+7);

        if(barrier_game()) {
            // In draw_barriers=game mode, fragment-only ordering/invalidate bits stay relaxed, while primitive/indirect ordering forces a full consumer barrier.
            nvn_be_sub_begin();
            color(target,data); // establishes g_last_end_relaxed=1
            uint64_t skip0=g_bar_skip, full0=g_bar_full;
            g_game_barrier=NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT;
            color(target,data);
            CHECK(g_bar_skip==skip0+1 && g_bar_full==full0,"fragment-only NVN barrier keeps relaxed consumer skip");
            skip0=g_bar_skip; full0=g_bar_full;
            g_game_barrier=NVN_BARRIER_ORDER_INDIRECT_DATA_BIT;
            color(target,data);
            CHECK(g_bar_full==full0+1 && g_bar_skip==skip0,"indirect NVN barrier forces full consumer barrier");
            skip0=g_bar_skip; full0=g_bar_full;
            g_game_barrier=NVN_BARRIER_ORDER_PRIMITIVES_BIT;
            color(target,data);
            CHECK(g_bar_full==full0+1 && g_bar_skip==skip0,"primitive NVN barrier forces full consumer barrier");
            skip0=g_bar_skip; full0=g_bar_full;
            g_game_barrier=NVN_BARRIER_INVALIDATE_SHADER_BIT;
            color(target,data);
            CHECK(g_bar_full==full0+1 && g_bar_skip==skip0,"shader invalidate NVN barrier forces full consumer barrier");
            CHECK(g_last_end_relaxed,"relaxed draw pass end still tracks same-CB chaining");
            main_barrier_reset();
            CHECK(!g_last_end_relaxed,"main_barrier_reset clears relaxed end across CB/submission boundaries");
            full0=g_bar_full; skip0=g_bar_skip;
            color(target,data);
            CHECK(g_bar_full==full0+1 && g_bar_skip==skip0,"first draw after main_barrier_reset keeps full consumer barrier");
            finish();
        }

        if(argc==2 && !strcmp(argv[1],"--bench")) {
            // Original 4x4 render workload, not a game/performance acceptance.
            // Pipeline/resource creation and GPU wait are outside encode CPU.
            const unsigned passes=512;
            nvn_be_set_frame(100); nvn_be_sub_begin(); prior=g_bar_covered;
            uint64_t c0=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), w0=now_ns();
            for(unsigned i=0;i<passes;i++) color(target,data);
            read_pixels(target,out);
            uint64_t fence=nvn_be_sub_commit();
            uint64_t wall=now_ns()-w0, cpu=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)-c0;
            REQUIRE(nvn_be_wait(fence,10000000000ull)); REQUIRE(!mtl_last_commit_error());
            uint64_t first,last; REQUIRE(nvn_be_gpu_span(100,&first,&last));
            pixels(out,0xff4d3721u);
            printf("BENCH passes=%u recorder=%d cover=%d encode_cpu_ns=%llu encode_wall_ns=%llu tagged_GPU_sum_ms=%.6f tagged_GPU_envelope_ns=%llu covered=%llu\n",
                passes,rec_on(),producer_cover_on(),(unsigned long long)cpu,(unsigned long long)wall,
                nvn_be_gpu_ms(100),(unsigned long long)(last-first),(unsigned long long)(g_bar_covered-prior));
        }
    }
    printf("%s t_nvn_producer_barrier: %u checks, %u failures\n",failures?"✗":"✓",checks,failures);
    return failures?1:0;
}
