// run <ref.metallib> <cand.metallib>...: same inputs (random + numeric edge cases), outputs compared word for word with the first library
#import <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv){ @autoreleasepool {
  id<MTLDevice> d = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [d newCommandQueue];
  const int N = 65536;
  const uint32_t ev[] = {0x00000000,0x80000000,0x00000001,0x80000001,0x007fffff,0x00800000,0x3effffff,0x3f000001,0x3f7fffff,0x3f800001,
    0x4b000000,0x4b000001,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00000,0x34000000,0xb4000000,0x33800001,0x3f800000,0xbf800000};
  id<MTLBuffer> in = [d newBufferWithLength:(size_t)N * 3 * 16 options:0], mb = [d newBufferWithLength:64 options:0];
  uint32_t *u = in.contents; float *f = in.contents; srand(11);
  for (long k = 0; k < (long)N * 12; k++) f[k] = ((rand() % 2000001) - 1000000) / 977.0f * ((rand() % 7) ? 1.0f : 1e-30f);
  for (long i = 0; i < N; i += 4) for (int c = 0; c < 12; c++) u[i * 12 + c] = ev[(i / 4 * 5 + c * 7) % (sizeof ev / 4)];
  float *m = mb.contents; for (int k = 0; k < 16; k++) m[k] = ((rand() % 2001) - 1000) / 333.0f;
  uint32_t *ref = NULL; int bad = 0;
  for (int a = 1; a < argc; a++) {
    NSError *e = nil; id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[a])] error:&e];
    id<MTLComputePipelineState> p = [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"main0"] error:&e];
    if (!p) { printf("不通 %s 建不出管线：%s\n", argv[a], e.description.UTF8String); return 1; }
    id<MTLBuffer> out = [d newBufferWithLength:(size_t)N * 5 * 16 options:0]; memset(out.contents, 0xcd, (size_t)N * 80);
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce setComputePipelineState:p];
    [ce setBuffer:in offset:0 atIndex:0]; [ce setBuffer:out offset:0 atIndex:1]; [ce setBuffer:mb offset:0 atIndex:2];
    [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint32_t *o = out.contents; size_t nw = (size_t)N * 20;
    if (!ref) { ref = malloc(nw * 4); memcpy(ref, o, nw * 4); printf("参照 %s\n", argv[a]); continue; }
    size_t diff = 0; for (size_t k = 0; k < nw; k++) diff += o[k] != ref[k];
    printf("%s %s：%zu / %zu 个字与参照不同\n", diff ? "不同" : "相同", argv[a], diff, nw);
    bad |= diff != 0 ? (1 << a) : 0;
  }
  return 0;
} }
