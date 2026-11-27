// #604: GPU-rendered stimuli through the real direct presenter. The drawable
// observer forwards to CAMetalLayer unchanged; it never supplies a replacement.
#include <sched.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#import <objc/runtime.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../gfx/nvn/nvn_mtl.h"
#include "s2a_gfx_mode.h"

void s2a_front_run(void (*guest_main)(void));
void *s2a_mac_front_layer(void);
void *front_metal_layer(void);
void nvn_be_set_origin_lower_left(int on);
double nvn_be_presented_time(uint64_t ticket);
void mtl_grab_next_present(void (^cb)(const uint8_t *, size_t, size_t, size_t));
int mtl_grab_inflight(void);

static int checked, failed, shown;
static id<MTLDevice> device;
static id<MTLCommandQueue> producer;
static id<MTLRenderPipelineState> patternPipeline;
static id<MTLSharedEvent> producerEvent;
static uint64_t producerValue;
static CAMetalLayer *layer;
static NSString *outputDirectory;
static pthread_mutex_t drawableMutex = PTHREAD_MUTEX_INITIALIZER;
static id<CAMetalDrawable> observedDrawable;
static IMP originalNextDrawable;
static uint64_t drawableCount;
static uint64_t lastTicket, asyncDrawableBase;
static BOOL observeAsync;
enum { ASYNC_FRAMES = 6, MAX_OBSERVATIONS = 32 };
static id<CAMetalDrawable> asyncDrawables[ASYNC_FRAMES];
typedef struct {
    uint64_t sequence, ticket, drawableID;
    uintptr_t address;
    unsigned callbacks;
    double acquired, callbackAt, platformTime, sampledTime;
} PresentObservation;
static PresentObservation observations[MAX_OBSERVATIONS];
static FILE *presentationLog;
#define CHECK(ok, ...) do { checked++; BOOL pass = !!(ok); failed += !pass; \
    printf("%s ", pass ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); } while (0)

// 只保存数值；不额外持有 drawable，以免改变三 drawable 的自然背压。
// Keep numeric observations only: retaining drawables would change pool pressure.
static void logPresentationLocked(const char *event, const PresentObservation *o) {
    fprintf(presentationLog, "%s\t%llu\t%llu\t%llu\t%llx\t%u\t%.9f\t%.9f\t%.9f\t%.9f\t%.9f\t%.9f\n",
        event, o->sequence, o->ticket, o->drawableID, (unsigned long long)o->address, o->callbacks,
        o->acquired, o->callbackAt, o->platformTime, o->sampledTime,
        o->ticket ? nvn_be_presented_time(o->ticket) : 0, CACurrentMediaTime());
    fflush(presentationLog);
}

static void notePresentation(const char *event, uint64_t sequence, uint64_t ticket, id<CAMetalDrawable> drawable) {
    double sample = drawable ? drawable.presentedTime : -1;
    pthread_mutex_lock(&drawableMutex);
    PresentObservation *o = &observations[sequence - 1];
    o->ticket = ticket;
    if (drawable) o->sampledTime = sample;
    logPresentationLocked(event, o);
    pthread_mutex_unlock(&drawableMutex);
}

static id<CAMetalDrawable> observeDrawable(id object, SEL selector) {
    id<CAMetalDrawable> drawable = ((id<CAMetalDrawable> (*)(id, SEL))originalNextDrawable)(object, selector);
    pthread_mutex_lock(&drawableMutex);
    observedDrawable = drawable; drawableCount++;
    uint64_t sequence = drawableCount;
    if (sequence > MAX_OBSERVATIONS) { fprintf(stderr, "drawable observation capacity exceeded\n"); exit(1); }
    observations[sequence - 1] = (PresentObservation){ .sequence = sequence, .drawableID = drawable.drawableID,
        .address = (uintptr_t)(__bridge void *)drawable, .acquired = CACurrentMediaTime() };
    logPresentationLocked("acquired", &observations[sequence - 1]);
    if (observeAsync && drawableCount > asyncDrawableBase && drawableCount <= asyncDrawableBase + ASYNC_FRAMES)
        asyncDrawables[drawableCount - asyncDrawableBase - 1] = drawable;
    pthread_mutex_unlock(&drawableMutex);
    // 在生产 present_note 注册之前，独立观察同一个真实 drawable 的平台回调。
    // Observe the real platform callback independently of the production ring.
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        pthread_mutex_lock(&drawableMutex);
        PresentObservation *o = &observations[sequence - 1];
        o->callbacks++;
        o->callbackAt = CACurrentMediaTime();
        o->platformTime = presented.presentedTime;
        logPresentationLocked("platform-handler", o);
        pthread_mutex_unlock(&drawableMutex);
    }];
    return drawable;
}

static BOOL presentationConsistent(const PresentObservation *o, double production) {
    if (o->sampledTime > 0 && production != o->sampledTime) return NO;
    if (o->callbacks) return production == (o->platformTime > 0 ? o->platformTime : -1);
    return production == (o->sampledTime > 0 ? o->sampledTime : 0);
}

// 门禁偶发：某格停在下面的等待里，直到 60 秒闹钟（11-21 起每天数局，单机连跑 144 格复现不出）。等过 5 秒打一行现场：
//   观测值、本窗口状态、最前台 App、压在本窗口上面的屏上窗口（所属 App / 层级 / 位置；不取窗口标题，不需要录屏授权）、主屏睡眠。
//   第一次打出时门槛临时改 0 实跑过一格（68/68 过、行如期出现）
static void stallReport(const char *name, uint64_t ticket, const PresentObservation *o, double production, double waited) {
    __block NSString *win = @"本窗口没找到";
    dispatch_sync(dispatch_get_main_queue(), ^{
        for (NSWindow *window in NSApp.windows) {
            if (window.contentView.layer != layer) continue;
            NSMutableString *above = [NSMutableString string];
            CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenAboveWindow | kCGWindowListExcludeDesktopElements, (CGWindowID)window.windowNumber);
            int system = 0;   // 菜单栏 / 程序坞 / 通知中心（层 ≥ 20）一直在上面，只计数
            for (NSDictionary *w in (__bridge NSArray *)list) {
                if ([w[(id)kCGWindowLayer] intValue] >= 20) { system++; continue; }
                CGRect r = CGRectZero; CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &r);
                [above appendFormat:@" [%@ 层%@ %.0fx%.0f@%.0f,%.0f]", w[(id)kCGWindowOwnerName], w[(id)kCGWindowLayer], r.size.width, r.size.height, r.origin.x, r.origin.y];
            }
            if (list) CFRelease(list);
            [above appendFormat:@"（另有系统层 %d 个）", system];
            win = [NSString stringWithFormat:@"窗口 #%ld visible=%d occluded=%d onActiveSpace=%d key=%d main=%d；本进程 active=%d；最前台 %@；上面的屏上窗口%@",
                (long)window.windowNumber, window.visible, !(window.occlusionState & NSWindowOcclusionStateVisible), window.onActiveSpace,
                window.keyWindow, window.mainWindow, NSApp.active, NSWorkspace.sharedWorkspace.frontmostApplication.localizedName ?: @"无",
                above];
        }
    });
    printf("【卡】%s ticket %llu 等呈现回调 %.1f 秒：handlers=%u platform=%.6f sampled=%.6f ring=%.6f drawable#%llu acquired=%.3f now=%.3f；%s；主屏睡眠=%d\n",
        name, ticket, waited, o->callbacks, o->platformTime, o->sampledTime, production, o->drawableID, o->acquired, CACurrentMediaTime(),
        win.UTF8String, CGDisplayIsAsleep(CGMainDisplayID()));
}

static BOOL checkPresentation(NSString *name, uint64_t sequence, uint64_t ticket, id<CAMetalDrawable> drawable) {
    PresentObservation o;
    // 等平台呈现回调，兜底 30 秒（原 3 秒：负载高时系统回调晚到就判不一致假红）；回调一到就退出循环
    double production, begin=CACurrentMediaTime(), end=begin+30; BOOL reported=NO;
    for (;;) {
        double sample=drawable ? drawable.presentedTime : -1;
        pthread_mutex_lock(&drawableMutex);
        PresentObservation *stored=&observations[sequence-1];
        stored->ticket=ticket;
        if (drawable) stored->sampledTime=sample;
        o=*stored;
        pthread_mutex_unlock(&drawableMutex);
        production=nvn_be_presented_time(ticket);
        if (((o.callbacks || o.sampledTime>0) && presentationConsistent(&o,production)) || CACurrentMediaTime()>=end) break;
        if (!reported && CACurrentMediaTime()-begin>5) { reported=YES; stallReport(name.UTF8String,ticket,&o,production,CACurrentMediaTime()-begin); }
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    // 没有 handler 且没有正呈现时间，只能确认 ring 尚无记录，不能算已显示。
    // An absent handler and zero time leave display unconfirmed, not a drop callback.
    BOOL consistent=presentationConsistent(&o,production);
    BOOL confirmed=consistent && production>0 && (o.platformTime>0 || o.sampledTime>0);
    notePresentation(name.UTF8String,sequence,ticket,drawable);
    CHECK(consistent,"%s ticket %llu platform/ring consistent: handlers=%u platform=%.6f sampled=%.6f ring=%.6f display=%s",
        name.UTF8String,ticket,o.callbacks,o.platformTime,o.sampledTime,production,confirmed?"confirmed":"unconfirmed");
    return confirmed;
}

static NSData *readTexture(id<MTLTexture> texture) {
    NSUInteger row = texture.width * 4, length = row * texture.height;
    id<MTLBuffer> buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [producer commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(texture.width, texture.height, 1) toBuffer:buffer destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:length];
    [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "GPU stimulus/readback error: %s\n", cb.error.description.UTF8String); exit(1); }
    return [NSData dataWithBytes:buffer.contents length:length];
}

static void saveImage(NSData *bytes, NSUInteger width, NSUInteger height, NSString *name) {
    NSError *writeError = nil;
    NSString *raw = [outputDirectory stringByAppendingPathComponent:[name stringByAppendingString:@".bgra"]];
    if (![bytes writeToFile:raw options:NSDataWritingAtomic error:&writeError]) {
        fprintf(stderr, "cannot write raw image artifact: %s\n", writeError.description.UTF8String); exit(1);
    }
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef data = CGDataProviderCreateWithCFData((__bridge CFDataRef)bytes);
    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, color,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst, data, NULL, false, kCGRenderingIntentDefault);
    NSURL *url = [NSURL fileURLWithPath:[outputDirectory stringByAppendingPathComponent:[name stringByAppendingString:@".png"]]];
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
    if (!image || !destination) { fprintf(stderr, "cannot create image artifact\n"); exit(1); }
    CGImageDestinationAddImage(destination, image, NULL);
    if (!CGImageDestinationFinalize(destination)) { fprintf(stderr, "cannot write image artifact\n"); exit(1); }
    CFRelease(destination); CGImageRelease(image); CGDataProviderRelease(data); CGColorSpaceRelease(color);
}

// All source pixels are produced by this real fragment shader. CPU assertions
// below use exact box coverage and sampling equations, not the AA implementation.
static id<MTLTexture> renderPattern(int width, int height, int kind, int ox, int oy, int cw, int ch, int flip) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:width height:height mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [producer commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
    int parameters[] = {kind, ox, oy, cw, ch, flip};
    [encoder setRenderPipelineState:patternPipeline];
    [encoder setFragmentBytes:parameters length:sizeof parameters atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding]; [cb encodeSignalEvent:producerEvent value:++producerValue];
    [cb commit]; [cb waitUntilCompleted];
    CHECK(texture && !cb.error, "GPU source render %dx%d pattern=%d", width, height, kind);
    if (!texture || cb.error) exit(1);
    return texture;
}

static int pixelDifference(NSData *a, NSData *b, int tolerance, int *maximum) {
    if (a.length != b.length) { *maximum = 255; return -1; }
    const uint8_t *pa = a.bytes, *pb = b.bytes; int different = 0; *maximum = 0;
    for (NSUInteger i = 0; i < a.length; i += 4) {
        int bad = 0;
        for (int c = 0; c < 4; c++) { int delta = abs(pa[i+c] - pb[i+c]); if (delta > *maximum) *maximum = delta; bad |= delta > tolerance; }
        different += bad;
    }
    return different;
}

static void noteWindow(NSString *phase, uint64_t ticket, id<CAMetalDrawable> drawable) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        for (NSWindow *window in NSApp.windows) {
            if (window.contentView.layer != layer) continue;
            printf("window %s ticket=%llu visible=%d occluded=%d minimized=%d key=%d active=%d drawable-time=%.6f now=%.6f\n",
                phase.UTF8String, ticket, window.visible,
                !(window.occlusionState & NSWindowOcclusionStateVisible), window.miniaturized,
                window.keyWindow, NSApp.active, drawable ? drawable.presentedTime : 0, CACurrentMediaTime());
        }
    });
}

// 滤镜 Fsr（AMD FSR 1）的格：截图 / 抓图口径不带 FSR（同选 Bilinear），所以画布与抓图本来就不同。capture 在这些格返回抓图（后面按「没过滤镜」写的判据
//   全部照核抓图 = 抓图口径不变），画布另存 fsrDrawable、与抓图不同的像素数累进 fsrChanged，由 run 里 Fsr 那段判；逐像素对错在 native-test-present_fsr（不开窗口）
static NSData *fsrDrawable; static int fsrChanged;
static NSData *capture(id<MTLTexture> source, const int crop[4], BOOL flip, int width, int height, NSString *name) {
    dispatch_sync(dispatch_get_main_queue(), ^{ layer.drawableSize = CGSizeMake(width, height); [CATransaction flush]; });
    noteWindow([name stringByAppendingString:@"-before"], 0, nil);
    // 上一次抓图的完成回调先唤醒我们、再清「抓图进行中」：清之前再上屏，这一帧不抓（请求留给下一次上屏），而本测一次只上屏一帧 → 干等到超时。
    //   等它清零再登记（同 t_nvn_m3 / t_nvn_present_init / t_nvn_rscale；11-20 门禁主机负载 68 时 mtl3 两格卡到 60 秒闹钟就是这个）
    while (mtl_grab_inflight()) sched_yield();
    nvn_be_set_origin_lower_left(flip);
    pthread_mutex_lock(&drawableMutex); observedDrawable = nil; uint64_t before = drawableCount; pthread_mutex_unlock(&drawableMutex);
    dispatch_semaphore_t captured = dispatch_semaphore_create(0);
    __block NSData *grab;
    __block size_t grabWidth, grabHeight;
    mtl_grab_next_present(^(const uint8_t *bytes, size_t w, size_t h, size_t row) {
        grabWidth = w; grabHeight = h;
        if (bytes && row == w * 4) grab = [NSData dataWithBytes:bytes length:row * h];
        dispatch_semaphore_signal(captured);
    });
    uint64_t serial = mtl_ext_tex((__bridge void *)source);
    uint64_t ticket = mtl_present_ext(serial, 0, (__bridge void *)producerEvent, producerValue, crop);
    lastTicket = ticket;
    BOOL got = dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) == 0;   // 兜底 30 秒（原 5 秒）
    CHECK(got && grab && grabWidth == (size_t)width && grabHeight == (size_t)height,
        "%s production grab %zux%zu", name.UTF8String, grabWidth, grabHeight);
    if (!got || !grab || grabWidth != (size_t)width || grabHeight != (size_t)height) exit(1);
    // Only one ticket is outstanding: this watermark is advanced by this frame's
    // actual GPU feedback; no display-link drop can advance it (DM_PRESENT_LINK=0).
    nvn_be_present_wait(ticket);
    pthread_mutex_lock(&drawableMutex);
    id<CAMetalDrawable> drawable = observedDrawable; uint64_t after = drawableCount;
    pthread_mutex_unlock(&drawableMutex);
    CHECK(drawable && after == before + 1 && drawable.texture.width == (NSUInteger)width && drawable.texture.height == (NSUInteger)height,
        "%s observed real CAMetalLayer drawable (%llu -> %llu)", name.UTF8String, before, after);
    if (!drawable || after != before + 1) exit(1);
    NSData *actual = readTexture(drawable.texture);
    int maximum, different = pixelDifference(actual, grab, 1, &maximum);
    BOOL fsr = !strcmp(getenv("S2A_SCALING_FILTER") ?: "", "Fsr");
    if (fsr) { fsrDrawable = actual; fsrChanged += different; }
    else CHECK(different == 0, "%s actual drawable vs production grab: different=%d max=%d", name.UTF8String, different, maximum);
    shown += checkPresentation(name,after,ticket,drawable);
    noteWindow([name stringByAppendingString:@"-after"], ticket, drawable);
    saveImage(actual, width, height, name);
    saveImage(grab, width, height, [name stringByAppendingString:@"-grab"]);
    if (fsr) return grab;
    return actual;
}

static double boxIntegral(double x) { return x <= 0 ? 0 : x < 1 ? x*x/2 : x-0.5; }
static double linearLight(double x) { return x <= 0.04045 ? x/12.92 : pow((x+0.055)/1.055, 2.4); }
static double coverageError(NSData *image, int width, int height, BOOL linear) {
    const uint8_t *p = image.bytes; double sum = 0;
    for (int y = 3; y < height-3; y++) for (int x = 3; x < width-3; x++) {
        double a = x + 1 - 0.27*y - 8.31;
        double coverage = (boxIntegral(a) - boxIntegral(a-0.27)) / 0.27;
        double value = p[4*(y*width+x)] / 255.0;
        if (linear) value = linearLight(value);
        sum += (value-coverage) * (value-coverage);
    }
    return sum;
}

static NSData *referenceScale(NSData *source, int w, int h, int dw, int dh, BOOL nearest) {
    NSMutableData *result = [NSMutableData dataWithLength:(NSUInteger)dw * dh * 4];
    const uint8_t *in = source.bytes; uint8_t *out = result.mutableBytes;
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        double sx = fmax(0, fmin(w-1, (x+0.5)*w/dw-0.5));
        double sy = fmax(0, fmin(h-1, (y+0.5)*h/dh-0.5));
        int x0 = (int)floor(sx), y0 = (int)floor(sy), x1 = MIN(x0+1, w-1), y1 = MIN(y0+1, h-1);
        for (int c = 0; c < 4; c++) {
            double v;
            if (nearest) v = in[4*((int)floor(sy+0.5)*w+(int)floor(sx+0.5))+c];
            else {
                double a = in[4*(y0*w+x0)+c]*(1-(sx-x0)) + in[4*(y0*w+x1)+c]*(sx-x0);
                double b = in[4*(y1*w+x0)+c]*(1-(sx-x0)) + in[4*(y1*w+x1)+c]*(sx-x0);
                v = a*(1-(sy-y0)) + b*(sy-y0);
            }
            out[4*(y*dw+x)+c] = (uint8_t)lround(v);
        }
    }
    return result;
}

static void asynchronousBatch(void) {
    // No production grab or per-frame producer wait: three real presents may be
    // waiting on the producer event while later tickets are already queued.
    NSMutableArray<id<MTLTexture>> *sources = [NSMutableArray array];
    uint64_t serials[ASYNC_FRAMES], tickets[ASYNC_FRAMES];
    for (int i=0;i<ASYNC_FRAMES;i++) {
        id<MTLTexture> texture=renderPattern(67+4*i,53,3,i,0,67+4*i,53,0);
        [sources addObject:texture]; serials[i]=mtl_ext_tex((__bridge void *)texture);
    }
    dispatch_sync(dispatch_get_main_queue(), ^{ layer.drawableSize=CGSizeMake(134,106); [CATransaction flush]; });
    noteWindow(@"async-before", 0, nil);
    nvn_be_set_origin_lower_left(0);
    uint64_t firstTicket=lastTicket+1, delayedValue=++producerValue;
    pthread_mutex_lock(&drawableMutex);
    asyncDrawableBase=drawableCount; observeAsync=YES;
    for(int i=0;i<ASYNC_FRAMES;i++) asyncDrawables[i]=nil;
    pthread_mutex_unlock(&drawableMutex);
    dispatch_semaphore_t copied=dispatch_semaphore_create(0);
    NSMutableArray<NSData *> *pixels=[NSMutableArray array];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        @autoreleasepool {
            for(int i=0;i<ASYNC_FRAMES;i++) {
                // An independent consumer waits for true feedback, copies the
                // real drawable and releases it so the layer's three-drawable
                // pool can make progress. It never blocks the enqueue loop.
                nvn_be_present_wait(firstTicket+i);
                pthread_mutex_lock(&drawableMutex);
                id<CAMetalDrawable> drawable=asyncDrawables[i]; asyncDrawables[i]=nil;
                pthread_mutex_unlock(&drawableMutex);
                if(!drawable) break;
                NSData *image=readTexture(drawable.texture);
                notePresentation("async-readback", asyncDrawableBase+i+1, firstTicket+i, drawable);
                [pixels addObject:image];
                saveImage(image,drawable.texture.width,drawable.texture.height,[NSString stringWithFormat:@"async-%d",i]);
            }
            dispatch_semaphore_signal(copied);
        }
    });
    // 原先 300 ms 定时器置位生产者事件、判六帧入队在它之前返回：负载高时入队慢过 300 ms 就假红。
    //   现在生产者事件由测试在入队返回之后手动置位（放行线程等「入队已返回」信号；兜底 30 秒照放——入队若错误地等生产者事件，
    //   要到兜底才返回，此时事件已置位，判红）
    CFTimeInterval begin=CACurrentMediaTime();
    dispatch_semaphore_t enqueued=dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        dispatch_semaphore_wait(enqueued,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC)); producerEvent.signaledValue=delayedValue; });
    for(int i=0;i<ASYNC_FRAMES;i++) tickets[i]=mtl_present_ext(serials[i],0,(__bridge void *)producerEvent,delayedValue,NULL);
    BOOL beforeEvent=producerEvent.signaledValue<delayedValue;
    dispatch_semaphore_signal(enqueued);
    CHECK(tickets[0]==firstTicket && tickets[ASYNC_FRAMES-1]==firstTicket+ASYNC_FRAMES-1,
        "async batch enqueued six consecutive tickets %llu..%llu",tickets[0],tickets[ASYNC_FRAMES-1]);
    CHECK(beforeEvent,"async six-frame enqueue returned before the producer event was signaled (%.3f ms)",(CACurrentMediaTime()-begin)*1000);
    nvn_be_drain();
    BOOL complete=dispatch_semaphore_wait(copied,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC))==0;   // 兜底 30 秒（原 5 秒）
    CHECK(complete && pixels.count==ASYNC_FRAMES,"async true completion/drain and real drawable readback: %lu/6",(unsigned long)pixels.count);
    int actualShown=0;
    for(int i=0;i<ASYNC_FRAMES && complete && i<(int)pixels.count;i++) {
        NSData *image=pixels[i]; const uint8_t *p=image.bytes; int errors=0;
        if(image.length==134*106*4)
            for(int y=26;y<80;y++) for(int x=33;x<101;x++) {
                int k=4*(y*134+x);
                errors+=abs(p[k]-(153-17*i))>1 || abs(p[k+1]-(79+13*i))>1 || abs(p[k+2]-(29+23*i))>1 || p[k+3]!=255;
            }
        CHECK(image.length==134*106*4 && errors==0,"async ticket %llu independently expected GPU plane, stale/cross-frame pixels=%d",tickets[i],errors);
        actualShown+=checkPresentation(@"async-after-poll",asyncDrawableBase+i+1,tickets[i],nil);
    }
    CHECK(actualShown>0,"async batch actually displayed %d/6 drawables",actualShown);
    noteWindow(@"async-after", tickets[ASYNC_FRAMES-1], nil);
    pthread_mutex_lock(&drawableMutex); observeAsync=NO; pthread_mutex_unlock(&drawableMutex);
    lastTicket=tickets[ASYNC_FRAMES-1];
    printf("async batch: six frames, no production screenshot, producer event 300 ms, final GPU completion and %lu drawable images; busy-slot branch not instrumented\n",(unsigned long)pixels.count);
}

static void run(void) {
    @autoreleasepool {
        device = (__bridge id<MTLDevice>)mtl_device();
        CHECK(device != nil, "real Metal device: %s", device.name.UTF8String);
        producer = [device newCommandQueue]; producerEvent = [device newSharedEvent];
        NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 vs(uint i [[vertex_id]]) { float2 u=float2((i<<1)&2,i&2); return float4(u*float2(2,-2)+float2(-1,1),0,1); }\n"
            "struct P { int kind,ox,oy,w,h,flip; };\n"
            "fragment float4 fs(float4 p [[position]],constant P& a [[buffer(0)]]) {\n"
            " if(a.kind==1) return float4(29.0/255,79.0/255,153.0/255,1);\n"
            " if(a.kind==3) return float4(float3(29+23*a.ox,79+13*a.ox,153-17*a.ox)/255.0,1);\n"
            " float2 q=p.xy-float2(a.ox,a.oy); if(a.flip) q.y=a.h-q.y;\n"
            " if(a.kind==2) { if(any(q<0)||q.x>=a.w||q.y>=a.h) return float4(1,0,1,1);\n"
            " if(q.y<9) return float4(1,0.125,0.25,1); if(q.y>a.h-10) return float4(0.125,0.375,1,1);\n"
            " if(q.x<6) return float4(0.125,1,0.375,1); }\n"
            " float v=q.x>0.27*q.y+8.31?1:0; return float4(v,v,v,1); }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:msl options:nil error:&error];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"vs"]; pd.fragmentFunction = [library newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        patternPipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        CHECK(patternPipeline != nil, "GPU stimulus pipeline: %s", error ? error.description.UTF8String : "ready");
        if (!patternPipeline) exit(1);

        const int w = 67, h = 53;
        id<MTLTexture> edge = renderPattern(w, h, 0, 0, 0, w, h, 0);
        NSData *binary = readTexture(edge);
        int sourceErrors = 0; const uint8_t *bp = binary.bytes;
        for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
            int expected = x+0.5 > 0.27*(y+0.5)+8.31 ? 255 : 0;
            for (int c=0; c<3; c++) sourceErrors += bp[4*(y*w+x)+c] != expected;
        }
        CHECK(sourceErrors == 0, "GPU stimulus matches independent analytic edge (%d errors)", sourceErrors);
        NSData *native = capture(edge, NULL, NO, w, h, @"edge-native");
        const char *aa = getenv("S2A_ANTI_ALIASING"), *filter = getenv("S2A_SCALING_FILTER");
        BOOL none = !strcmp(aa, "None"), smaa = !strncmp(aa, "Smaa", 4), nearest = !strcmp(filter, "Nearest");
        const uint8_t *p = native.bytes; int fractional = 0, distant = 0, alpha = 0;
        for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
            int i=4*(y*w+x); fractional += p[i]>0 && p[i]<255; alpha += p[i+3]!=255;
            if (fabs(x+0.5-0.27*(y+0.5)-8.31)>4) distant += p[i]!=bp[i];
        }
        double before = coverageError(binary,w,h,smaa), after = coverageError(native,w,h,smaa);
        CHECK(distant == 0 && alpha == 0, "AA preserves flat regions and opacity: distant=%d alpha=%d", distant, alpha);
        if (none) { int maximum; CHECK(pixelDifference(native,binary,0,&maximum)==0, "None preserves exact GPU pixels"); }
        else CHECK(fractional>0 && after<before, "%s improves unit-box edge coverage: fractional=%d error %.9f < %.9f", aa,fractional,after,before);

        NSData *scaled = capture(edge,NULL,NO,2*w,2*h,@"edge-scaled");
        int maximum, different;
        if (!strcmp(filter, "MetalFX")) {
            // MetalFX 的核不公开，不能独立算出逐像素期望：核与算法无关的性质——离边 > 4 个源像素的平坦区（放大后 > 8 个目标像素）保持源值（± 2），不透明
            const uint8_t *q = scaled.bytes; int flatBad = 0, flatN = 0;
            for (int y=0; y<2*h; y++) for (int x=0; x<2*w; x++) {
                double sx = (x+0.5)/2, sy = (y+0.5)/2;
                if (fabs(sx-0.27*sy-8.31) <= 4) continue;
                int want = sx > 0.27*sy+8.31 ? 255 : 0, i = 4*(y*2*w+x); flatN++;
                for (int c=0; c<3; c++) flatBad += abs(q[i+c]-want) > 2;
                flatBad += q[i+3] != 255;
            }
            CHECK(flatN > 0 && flatBad == 0, "MetalFX keeps flat regions far from the edge: %d bad of %d", flatBad, flatN);
            NSData *bilinear = referenceScale(native,w,h,2*w,2*h,NO);
            different = pixelDifference(scaled,bilinear,1,&maximum);
            CHECK(different > 0, "MetalFX output differs from bilinear near the edge: different=%d max=%d", different, maximum);
        } else {
            NSData *expected = referenceScale(native,w,h,2*w,2*h,nearest);
            different = pixelDifference(scaled,expected,1,&maximum);
            CHECK(different==0, "%s matches independent pixel-center scale: different=%d max=%d",filter,different,maximum);
        }
        if (!strcmp(filter, "Fsr")) {
            // FSR 1 的画布（上面 scaled 是抓图，已按双线性参考核过）：离边 > 4 个源像素的平坦区保持源值（± 2：RCAS 的近似倒数让纯白落到 254）、不透明；
            //   边附近区别于双线性；整格里画布确实与抓图不同（FSR 真的上了屏）。EASU / RCAS 逐像素对 CPU 参考在 native-test-present_fsr
            const uint8_t *q = fsrDrawable.bytes; int flatBad = 0, flatN = 0;
            for (int y=0; y<2*h; y++) for (int x=0; x<2*w; x++) {
                double sx = (x+0.5)/2, sy = (y+0.5)/2;
                if (fabs(sx-0.27*sy-8.31) <= 4) continue;
                int want = sx > 0.27*sy+8.31 ? 255 : 0, i = 4*(y*2*w+x); flatN++;
                for (int c=0; c<3; c++) flatBad += abs(q[i+c]-want) > 2;
                flatBad += q[i+3] != 255;
            }
            CHECK(fsrDrawable.length == (NSUInteger)(2*w*2*h*4) && flatN > 0 && flatBad == 0, "Fsr drawable keeps flat regions far from the edge: %d bad of %d", flatBad, flatN);
            different = pixelDifference(fsrDrawable,referenceScale(native,w,h,2*w,2*h,NO),1,&maximum);
            CHECK(different > 0 && fsrChanged > 0, "Fsr drawable differs from bilinear near the edge: different=%d max=%d; drawable vs grab so far: %d pixels", different, maximum, fsrChanged);
        }

        id<MTLTexture> plane = renderPattern(64,36,1,0,0,64,36,0);
        id<MTLTexture> srgb = [plane newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
        NSData *flat = capture(srgb,NULL,NO,128,72,@"srgb-plane");
        p=flat.bytes; int planeErrors=0;
        for(NSUInteger i=0;i<flat.length;i+=4) planeErrors += abs(p[i]-153)>1 || abs(p[i+1]-79)>1 || abs(p[i+2]-29)>1 || p[i+3]!=255;
        CHECK(planeErrors==0,"sRGB source preserves encoded plane bytes (%d errors)",planeErrors);

        id<MTLTexture> cropped = renderPattern(79,61,2,5,4,67,49,0);
        int crop[4]={5,4,67,49};
        for(int flip=0;flip<2;flip++) {
            NSData *transformed = capture(cropped,crop,flip,134,98,flip?@"crop-flip":@"crop");
            id<MTLTexture> canonical=renderPattern(67,49,2,0,0,67,49,flip);
            NSData *reference=capture(canonical,NULL,NO,134,98,flip?@"canonical-flip":@"canonical");
            different=pixelDifference(transformed,reference,1,&maximum);
            CHECK(different==0,"crop then GL flip=%d then AA equals independently GPU-rendered canonical input: different=%d max=%d",flip,different,maximum);
        }
        CHECK(shown>0,"real drawables reached the visible display: %d",shown);
        asynchronousBatch();
        nvn_be_drain();
        int mismatches=0,finalSync=0,finalAsync=0,unconfirmed=0;
        for (uint64_t sequence=1;sequence<=drawableCount;sequence++) {
            pthread_mutex_lock(&drawableMutex);
            PresentObservation o=observations[sequence-1];
            logPresentationLocked("final", &o);
            pthread_mutex_unlock(&drawableMutex);
            double production=nvn_be_presented_time(o.ticket);
            BOOL consistent=presentationConsistent(&o,production);
            BOOL confirmed=consistent && production>0 && (o.platformTime>0 || o.sampledTime>0);
            mismatches+=!consistent;
            if(confirmed) { if(sequence<=asyncDrawableBase) finalSync++; else finalAsync++; }
            else unconfirmed++;
        }
        CHECK(mismatches==0 && finalSync>0 && finalAsync>0,
            "final independent platform/ring table: mismatches=%d confirmed sync=%d async=%d unconfirmed=%d",
            mismatches,finalSync,finalAsync,unconfirmed);
        printf("present settings: %d/%d passed; AA=%s filter=%s; pixels=actual CAMetalLayer drawable + production grab\n",checked-failed,checked,aa,filter);
        printf("%s t_present_settings\n", failed ? "✗" : "✓");   // 门禁结论行（t_result_lint）
        exit(failed?1:0);
    }
}

// 宿主显示环境探针：屏睡/无活动屏时 WindowServer 撤掉所有窗口的
// NSWindowOcclusionStateVisible 位（与焦点无关——无人值守跑门禁必中）。此时
// 「真 drawable 到达可见显示」本就不成立，格记「未证」退 77 而非红。
// S2A_PS_ENV 是测试缝：sleep=模拟屏睡（早探针、不建窗口），offspace=模拟窗口
// 不在活动 Space（走 500ms 起的轮询探针），awake=钉醒（配 S2A_PS_WINDOW 验红路仍通）。
static int displayEnvUnproven(void) {
    const char *sim = getenv("S2A_PS_ENV");
    if (sim) return !strcmp(sim, "sleep");
    uint32_t count = 0;
    CGGetActiveDisplayList(0, NULL, &count);
    return count == 0 || CGDisplayIsAsleep(CGMainDisplayID());
}

static void awaitWindow(int left) {   // 主队列上：每 50 ms 看一次持有生产图层的窗口，left 次后仍不可见判红
    NSWindow *window=nil;
    for(NSWindow *candidate in NSApp.windows) if(candidate.contentView.layer==layer) { window=candidate; break; }
    const char *sim=getenv("S2A_PS_ENV");
    BOOL offSpace = sim ? !strcmp(sim,"offspace") : (window && ![window isOnActiveSpace]);
    if(displayEnvUnproven() || offSpace) {
        printf("未证：宿主显示休眠或窗口不在活动 Space\n"); exit(77);
    }
    BOOL visible=window && (window.occlusionState & NSWindowOcclusionStateVisible);
    const char *simw=getenv("S2A_PS_WINDOW");      // 测试缝：occluded=钉真不可见，验原检查红路仍通（不等，当场判）
    BOOL pinned=simw && !strcmp(simw,"occluded");
    if(pinned) visible=NO;
    if(!(window && visible) && !pinned && left>0) { dispatch_after(dispatch_time(DISPATCH_TIME_NOW,50*NSEC_PER_MSEC),dispatch_get_main_queue(), ^{ awaitWindow(left-1); }); return; }
    CHECK(window && visible,"production window is visible");
    if(!window || !visible) exit(1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{ run(); });
}

int main(int argc, const char **argv) {
    setvbuf(stdout,NULL,_IONBF,0); alarm(60);
    if(argc!=2 || !getenv("S2A_ANTI_ALIASING") || !getenv("S2A_SCALING_FILTER")) return 2;
    outputDirectory=@(argv[1]);
    NSString *presentationPath=[outputDirectory stringByAppendingPathComponent:@"presentation-observations.tsv"];
    presentationLog=fopen(presentationPath.fileSystemRepresentation,"w");
    if(!presentationLog) { perror("presentation observations"); return 1; }
    fprintf(presentationLog,"event\tsequence\tticket\tdrawable_id\tdrawable_address\tcallbacks\tacquired_at\tcallback_at\tplatform_time\tsampled_time\tproduction_time\tnow\n");
    if(setenv("S2A_GFX","direct",1) || s2a_gfx_mode()!=S2A_GFX_DIRECT) return 1;
    if(displayEnvUnproven()) { printf("未证：宿主显示休眠（无活动屏或主屏睡眠），本格不给真上屏结论\n"); exit(77); }
    dispatch_async(dispatch_get_main_queue(), ^{
        layer=(__bridge CAMetalLayer *)s2a_mac_front_layer();
        CHECK(layer && front_metal_layer()==(__bridge void *)layer,"production Mac CAMetalLayer");
        if(!layer) exit(1);
        layer.framebufferOnly=NO;
        Class base=object_getClass(layer);
        originalNextDrawable=method_getImplementation(class_getInstanceMethod(base,@selector(nextDrawable)));
        Class observer=objc_allocateClassPair(base,"S2APresentSettingsDrawableObserver",0);
        if(!observer || !originalNextDrawable || !class_addMethod(observer,@selector(nextDrawable),(IMP)observeDrawable,"@@:")) exit(1);
        objc_registerClassPair(observer); object_setClass(layer,observer);
        [CATransaction flush];
        // 原先 500 ms 后只看一次窗口可见：负载高时窗口还没上屏就判红。现在 500 ms 起每 50 ms 看一次，等到可见为止（兜底 30 秒，等不到判红）
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),dispatch_get_main_queue(), ^{ awaitWindow(600); });
    });
    s2a_front_run(NULL); return 1;
}
