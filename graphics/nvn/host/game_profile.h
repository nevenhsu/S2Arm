// 上游的 game_profile.h 在它那边按款编译期生成（make GAME=<程序号>）；S2Arm 一个 App 跑任意款：起局读随包的 shaders/direct/game.tsv（host/game_tsv.h），
// 不再把手写档编进宿主、不按程序号查表（10-25：手写档只当测试夹具，零配置推导档同样随包）。宏名与语义不变，图形层源码照原样读这些宏。
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include "game_tsv.h"
typedef struct {
    s2a_game_tsv t;                 // 随包 game.tsv 的图形层各项（没有文件 / 没写的键按 Switch 真实语义的缺省，见 game_tsv.h）
} s2a_nvn_profile;
const s2a_nvn_profile *s2a_nvn_prof(void);   // host_adapt.m：本次运行这一款的参数（起局读一次）
const char *s2a_nvn_game_id(void);   // host_adapt.m：16 位十六进制程序号，每次现取 NPDM（iOS 上每款参数在重定位期就读了，那时程序号还是 0）
#define DM_GAME_ID               (s2a_nvn_game_id())
#define DM_GAME_APP_POOL         (s2a_nvn_prof()->t.app_pool)
// 60 帧开关在读时套（每局现读 S2A_FPS60）：每款参数一进程只读一次，设置页切 60 帧后进程内重启的新局要按新开关走
//   （11-02 维护者：关了 60 帧游戏还是 60 帧——档里的 display 决策连同当时的开关一起缓存，重启后照旧放开出图间隔）
//   来源两处：每款参数的决策（推导 / 显式 / L3），和包内能力清单（runtime/src/caps.c s2a_fps60_display_source：fps60_display 1 或未验证款手动开）——
//   审计 P1-80：清单原先 setenv DM_PRESENT_INTERVAL=display，压过这道闸
int s2a_fps60_display_source(void);
#define DM_GAME_PRESENT_DISPLAY  (s2a_present_fps60_gate(s2a_nvn_prof()->t.present_display || s2a_fps60_display_source(), getenv("S2A_FPS60")))
#define DM_GAME_FPS_HOLD         (s2a_nvn_prof()->t.fps_hold)
#define DM_GAME_RENDER_COND      (s2a_nvn_prof()->t.render_cond)
#define DM_GAME_TEX_FLUSH_UPLOAD (s2a_nvn_prof()->t.tex_flush_upload)
#define DM_GAME_TEX_DIRTY_UPLOAD (s2a_nvn_prof()->t.tex_dirty_upload)
#define DM_GAME_TEX_DYN_SLOTS    (s2a_nvn_prof()->t.tex_dyn_slots)
#define DM_GAME_GPU_TS_SEGMENT   (s2a_nvn_prof()->t.gpu_ts_segment)
#define DM_GAME_VCONV_RANGE      (s2a_nvn_prof()->t.vconv_range)
#define DM_GAME_DRAW_BARRIERS_GAME (s2a_nvn_prof()->t.draw_barriers_game)
#define DM_GAME_ALPHA_FROM_RED   (s2a_nvn_prof()->t.alpha_from_red)
#define DM_GAME_FEEDBACK_SNAP    (s2a_nvn_prof()->t.feedback_snap)
#define DM_GAME_ZERO_SKIP        (s2a_nvn_prof()->t.zero_skip)
// DM_GAME_NVN_API_MAJOR / _MINOR 不定义：走按 SDK 版本的分支（hle_nvn.c nvn_api）
