// 首次进游戏的加载页：进度模型（平台无关；iOS 页面 ios/loadpage.m 只读它画，10-30 维护者 批准）。
// 三段：准备游戏数据（起局到预热开始）→ 为你的设备优化画面（直连管线预热，已编 / 总数）→ 进入游戏（预热完到首帧上屏）。
// 剩余时间按本机实测速率估：进度只进不退；显示的剩余时间每次至多按流逝时间往下走，实测持续变慢（新估计比显示值高 25% 以上连续 3 秒）才往上修正。
// 「什么时候不再等」由这里定（直连层 nvn_mtl.m 每 50 ms 调 s2a_front_warm_tick 问）：玩家点了「先开始玩」、长时间没有进展 → 返回 1，
//   剩下的管线转后台继续编（#800 B 近似顶替 + 后台编译）。默认编完直接进游戏，没有总时长上限（11-22 #502：编译还在往前走就等，等不等由玩家定）；无进展护栏只防异常，绝不卡死。
// 时间由调用方传（纳秒，单调时钟）；配置走环境变量，缺省值等真机实测后定：
//   S2A_LOAD_STALL_S（缺省 30）已编数多少秒没涨就自动进游戏
//   S2A_LOAD_UPGRADE_S（缺省 3）简短启动画面在优化画面里编了这么久还没编完，就升成完整加载页（first_run 0 → 1）
//   「先开始玩，后台继续优化」在完整加载页的优化画面段一直可点
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { S2A_LOAD_DATA = 0, S2A_LOAD_OPTIMIZE = 1, S2A_LOAD_ENTER = 2, S2A_LOAD_DONE = 3 };   // 阶段；DONE = 首帧已上屏，页面收起
enum { S2A_LOAD_GO_NONE = 0, S2A_LOAD_GO_DONE, S2A_LOAD_GO_USER, S2A_LOAD_GO_STALL, S2A_LOAD_GO_HOST };   // HOST（11-06）：直连层自己放行（s2a_front_warm_released，清单全部领走、只剩在编的几条）；为什么不再等预热
typedef struct {
    int stage;            // S2A_LOAD_*
    int first_run;        // 1 = 首次（完整加载页）；0 = 之后的启动（简短启动画面）。只会 0 → 1：之后的启动真要现编一批（S2A_LOAD_UPGRADE_S + S2A_LOAD_UPGRADE_COMPILED），
                          //   或只是从管线归档载入、却载入很久（S2A_LOAD_SLOW_S）时升成完整页
    int loading;          // 1 = 完整页显示「载入中」：之后的启动从管线归档载入为主，现编没到 S2A_LOAD_UPGRADE_COMPILED（不说「优化」）；现编够了转回 0
    long done, total;     // 预热已编 / 总数（阶段 OPTIMIZE 起有效）
    double frac;          // 总进度 0..1，只进不退（三段按 5% / 90% / 5% 摊）
    int eta_s;            // 剩余秒数；-1 = 还在估算 / 不显示
    int can_skip;         // 1 = 显示「先开始玩，后台继续优化」
    int go_reason;        // S2A_LOAD_GO_*
} s2a_load_view;
// 起局时调（iOS：场景连上之前）：first_run = 本包是否还没完整预热过（s2a_load_first_run）
void s2a_load_begin(uint64_t now, int first_run);
// 直连层预热节拍（任意线程，约每 50 ms；开始一次 (0, total)、结束一次 (total, total)）：返回 1 = 现在进游戏、剩下的转后台
int s2a_load_warm_tick(uint64_t now, long done, long total);
// 直连层报这一局预热至今「真现编」的管线条数（不是从管线归档 / 随包归档取的；归档关着时每条都算），与节拍一起约每 50 ms 一次
void s2a_load_warm_compiled(long n);
// 首帧上屏（ap_host.c）：阶段 → DONE；预热还在等（OPTIMIZE）时只记首帧已到，预热结束时才 → DONE（直连层先过首帧回调、后在首次提交里等预热）。「本包已完整预热」在收到 (total, total) 时记（首帧前编完或后台编完，直连层保证只发一次）
void s2a_load_first_frame(uint64_t now);
// 玩家点了「先开始玩」（主线程）
void s2a_load_user_skip(void);
// 页面读当前快照（主线程，约每 0.25 秒）
void s2a_load_snapshot(uint64_t now, s2a_load_view *out);
// 本包是否还没完整预热过：<Documents>/wire/warm_done 里记的出包指纹 ≠ pack_fp（或没有）= 1
int s2a_load_first_run(const char *pack_fp);
// 剩余时间文案（「约 1 分 20 秒」「约 40 秒」「不到 5 秒」「正在估算剩余时间」），写进 out
void s2a_load_eta_text(int eta_s, char *out, size_t n);
// 直连层调的入口（画面线 wire-c/warm-progress：开始一次 (0, total)、预热等待循环里周期调、全部编完时 (total, total) 一次，后者可能在后台队列线程）：
//   只回 0（继续等）/ 1（现在进游戏）；nvn_mtl.m 的弱定义回 -1 = 没链前端、直连层自己兜底。内部取单调时钟转给 s2a_load_warm_tick，加锁极短、不阻塞
int s2a_front_warm_tick(long done, long total);
// 直连层调的入口（同 s2a_front_warm_tick 的节拍）：没链前端时 nvn_mtl.m 的弱定义什么都不做
void s2a_front_warm_compiled(long n);
// 直连层自己不再等了（清单全部领走、只剩在编的几条；不是这里的节拍说的）：页面转「进入游戏」，首帧已到过就直接收起
void s2a_front_warm_released(void);
// 前台激活门（11-19 定因：系统弹框「允许使用无线数据？」「Apple 账户验证」盖在 App 上时，场景一直「前台但未激活」，宿主原先停在「等待前台」，
//   玩家应答弹框期间一点没加载）：场景进前台（未激活）就开始起宿主——C# 运行时、装模块、建内存、着色器预热这些不上屏的工作照做；
//   只有起游戏主线程（真正开跑、开始上屏）前等激活。状态：0 未知（门开：不走 UIKit 场景的路径不受影响）、1 前台未激活（门关）、2 已激活（门开）
void s2a_load_front_state(int st);        // app.m：sceneWillEnterForeground → 1，sceneDidBecomeActive → 2
int s2a_load_front_wait(void);            // host_main.c 起游戏主线程前：门关着就等到激活；返回等了多少毫秒（没等 = 0）
#ifdef __cplusplus
}

#endif
