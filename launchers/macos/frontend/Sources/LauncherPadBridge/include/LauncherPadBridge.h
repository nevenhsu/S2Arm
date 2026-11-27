// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#import <AppKit/AppKit.h>
#include <stdint.h>

// SwiftUI accessibility elements implement these public selectors without always
// declaring conformance to the complete NSAccessibility protocol.
@interface S2ALauncherAX : NSObject
+ (NSArray *)children:(id)element;
+ (NSString *)role:(id)element;
+ (NSString *)identifier:(id)element;
+ (NSString *)label:(id)element;
+ (NSRect)frame:(id)element;
+ (BOOL)enabled:(id)element;
+ (id)value:(id)element;
+ (BOOL)secure:(id)element;
+ (void)setValue:(id)value element:(id)element;
+ (void)focus:(id)element;
+ (BOOL)press:(id)element;
+ (BOOL)adjust:(id)element step:(NSInteger)step;
+ (BOOL)pick:(id)element;
+ (BOOL)cancel:(id)element;
@end

typedef struct s2a_launcher_pad_client s2a_launcher_pad_client;
typedef struct {
    uint64_t buttons, epoch;
    float x, y, right_y;
    uint8_t ready;
} s2a_launcher_pad_sample;
s2a_launcher_pad_client *s2a_launcher_pad_open(const char *path);
// Nonblocking. 1 = sample, 0 = no new packet, -1 = connection ended/invalid.
int s2a_launcher_pad_poll(s2a_launcher_pad_client *client, s2a_launcher_pad_sample *sample);
void s2a_launcher_pad_close(s2a_launcher_pad_client *client);
int s2a_launcher_pad_decode(const void *bytes, size_t length, s2a_launcher_pad_sample *sample);
// 手柄测试面板（开发工具）：同一条连接顺带要体感读数（连接程序一次只服务一个客户端，不另开连接）。
//   开了体感后每个 INPUT 包里的六轴原样排进队列（每位玩家都有；一对时 motion 是右只、motion_left 是左只），面板取走
typedef struct {
    uint8_t player, kind, side;   // kind = ns2_kind（1 Pro、2 左只、3 右只、4 一对）；side 0 = motion（单只 / Pro / 一对的右只）、1 = motion_left（一对的左只）
    uint64_t host_timestamp_us;   // 0 = 没有时间戳
    float ax, ay, az, gx, gy, gz;   // g、度/秒，设备轴序，未转换
} s2a_launcher_pad_motion;
// 送 NS2_RELAY_MOTION（enabled 0 / 1）：0 成功，-1 送不出
int s2a_launcher_pad_set_motion(s2a_launcher_pad_client *client, int enabled);
// 取走排着的六轴（最旧的先）：返回取到几笔
int s2a_launcher_pad_motion_drain(s2a_launcher_pad_client *client, s2a_launcher_pad_motion *out, int max);
// 一个 INPUT 包里的六轴（纯函数，单测）：返回几笔（0–2），-1 = 包不对
int s2a_launcher_pad_decode_motion(const void *bytes, size_t length, s2a_launcher_pad_motion out[2]);
// 单测用：按 ns2_relay_packet 的字段摆一个 INPUT 包（m[0] 进 motion，n == 2 时 m[1] 进 motion_left、kind 记成一对）；返回包长
size_t s2a_launcher_pad_encode_motion_for_test(const s2a_launcher_pad_motion *m, int n, void *out, size_t capacity);
int s2a_launcher_face_valid(int map);
int s2a_launcher_face_source(int map, int target);
uint64_t s2a_launcher_face_apply(uint64_t buttons, int map);

// 启动器常驻独占 Xbox 有线手柄（USB GIP）并通过本机 Unix 域套接字转发给 s2arm-host。
typedef void (*s2a_xbox_gip_status_cb)(const char *name, int connected, const char *blocked_by, void *ctx);
void s2a_xbox_gip_relay_set_log_path(const char *log_path);
int s2a_xbox_gip_relay_start(const char *socket_path, s2a_xbox_gip_status_cb cb, void *ctx);
int s2a_xbox_gip_relay_start_ex(const char *socket_path, int enable_usb, s2a_xbox_gip_status_cb cb, void *ctx);
int s2a_xbox_gip_relay_publish_for_test(const void *raw, size_t len);
void s2a_xbox_gip_relay_stop(void);

// 启动器读 HID 兜底手柄（#1151，HIDPads.m）：GameController 认不出的手柄（PowerA「Lic Pro Controller」等），认法 / 解析同宿主 macos/hid_pad.m。
//   buttons 是 Npad 位（第 0 位 A … 第 12–15 位 左上右下），摇杆 −32767..32767、Y 向上为正。只读、共享打开，启动器失焦时关
typedef struct { uint32_t buttons; int16_t lx, ly, rx, ry; char name[96]; int pro; } s2a_launcher_hid_pad;
void s2a_launcher_hid_open(void);
void s2a_launcher_hid_close(void);
int s2a_launcher_hid_read(s2a_launcher_hid_pad *out, int max);
// 单只手柄的解析状态（纯函数，单测直接喂元素值 / 原始报告）
typedef struct s2a_launcher_hid_state s2a_launcher_hid_state;
s2a_launcher_hid_state *s2a_launcher_hid_state_new(int pro);
void s2a_launcher_hid_state_free(s2a_launcher_hid_state *s);
void s2a_launcher_hid_state_init(s2a_launcher_hid_state *s, int pro);
void s2a_launcher_hid_has_axis(s2a_launcher_hid_state *s, uint32_t page, uint32_t usage);
void s2a_launcher_hid_value(s2a_launcher_hid_state *s, uint32_t page, uint32_t usage, long v, long min, long max);
int s2a_launcher_hid_report(s2a_launcher_hid_state *s, const uint8_t *r, size_t n);
void s2a_launcher_hid_state_pad(const s2a_launcher_hid_state *s, s2a_launcher_hid_pad *out);
