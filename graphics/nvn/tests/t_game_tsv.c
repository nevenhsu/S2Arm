// 随包 game.tsv 解析（graphics/nvn/host/game_tsv.h）单测：期望值按 game.py 的换算与 10-25 定的真实语义缺省手写，不由被测实现推出。
//   t_game_tsv          手写用例
//   t_game_tsv <文件>…  每个文件打一行解析结果（tests/game_tsv_test.py 拿它与 game.py header() 对拍全部手写档）
#include "../host/game_tsv.h"

static int fails;
#define CK(c, ...) do { if (!(c)) { fails++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void dump(const char *path) {
    s2a_game_tsv g; char err[200];
    int got = s2a_game_tsv_load(path, &g, err, sizeof err);
    printf("%s\t%d\t%#llx\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\n", path, got, (unsigned long long)g.app_pool, g.present_display, g.fps_hold, g.render_cond,
           g.tex_flush_upload, g.tex_dirty_upload, g.tex_dyn_slots, g.gpu_ts_segment, g.vconv_range, g.alpha_from_red, g.feedback_snap, g.zero_skip, err);   // draw_barriers 不在这里：game.py 还没有它（冷窗口补）
}

int main(int argc, char **argv) {
    if (argc > 1) { for (int i = 1; i < argc; i++) dump(argv[i]); return 0; }
    s2a_game_tsv g; char err[200];

    s2a_game_tsv_parse("", &g, err, sizeof err);   // 空档：全按缺省
    CK(g.app_pool == 0xCD500000ull && !g.present_display && g.fps_hold && g.render_cond && g.tex_flush_upload && g.tex_dirty_upload &&
       g.tex_dyn_slots && g.gpu_ts_segment && !g.vconv_range && !g.draw_barriers_game && !*g.alpha_from_red && !*g.feedback_snap && !*g.zero_skip && !*err,
       "空档缺省：app_pool 0xCD500000、上屏按游戏、帧保持开、条件渲染开、贴图上传 / 脏页开、动态句柄开、GPU 起止计时、按绘制转顶点关、绘制遍屏障 full、名单空");
    s2a_game_tsv_parse("draw_barriers\tgame\n", &g, err, sizeof err);
    CK(g.draw_barriers_game && !*err, "draw_barriers game：只按游戏自己的屏障排序");
    s2a_game_tsv_parse("draw_barriers\tsome\n", &g, err, sizeof err);
    CK(!g.draw_barriers_game && *err, "draw_barriers 值看不懂：报错、按缺省 full（实得 %d「%s」）", g.draw_barriers_game, err);

    s2a_game_tsv_parse("gpu_timestamps\tbad\n", &g, err, sizeof err);
    CK(g.gpu_ts_segment == 1 && *err, "gpu_timestamps 值看不懂：报错、按缺省 segment（实得 %d「%s」）", g.gpu_ts_segment, err);

    s2a_game_tsv_parse(NULL, &g, err, sizeof err);
    CK(g.render_cond && g.gpu_ts_segment && !*err, "没有文件（NULL）同空档");

    s2a_game_tsv_parse("render_cond\toff\ntex_flush_upload\toff\ntex_dirty_upload\ton\ngpu_timestamps\tsubmission\ntex_dyn_slots\toff\nfps_hold\toff\nvconv_range\ton\n", &g, err, sizeof err);
    CK(!g.render_cond && !g.tex_flush_upload && !g.tex_dirty_upload && !g.gpu_ts_segment && !g.tex_dyn_slots && !g.fps_hold && g.vconv_range && !*err,
       "显式值优先（含 off）；tex_dirty_upload 要 tex_flush_upload 开，否则按关（同 game.py）");
    s2a_game_tsv_parse("tex_flush_upload\toff\n", &g, err, sizeof err);
    CK(!g.tex_flush_upload && !g.tex_dirty_upload && !*err, "档里只写 tex_flush_upload off：tex_dirty_upload 随它关（同 game.py）");

    s2a_game_tsv_parse("  # 注释 render_cond\ton\n\n render_cond \t off \r\nrender_cond\ton\npresent_interval\tdisplay\n", &g, err, sizeof err);
    CK(!g.render_cond && g.present_display && !*err, "注释 / 空行跳过，键值去空白与回车，同一键取第一行；present_interval display");

    s2a_game_tsv_parse("alpha_from_red\t10bc9dd3825aec43 b9f0ca8022fbf9c2\nfeedback_snap\t66a71a3081b91b9a\t30e3ccb0bd304ead\napp_pool\t0x40000000\n", &g, err, sizeof err);
    CK(!strcmp(g.alpha_from_red, "10bc9dd3825aec43 b9f0ca8022fbf9c2") && !strcmp(g.feedback_snap, "66a71a3081b91b9a 30e3ccb0bd304ead") && g.app_pool == 0x40000000ull && !*err,
       "名单键空白分隔（值间 TAB 换成空格）；app_pool 十六进制（实得「%s」「%s」%#llx）", g.alpha_from_red, g.feedback_snap, (unsigned long long)g.app_pool);

    s2a_game_tsv_parse("zero_skip\t2bcd0a940ff3f011:4:1:0\t0123456789abcdef:-1:-1:-1\n", &g, err, sizeof err);
    CK(!strcmp(g.zero_skip, "2bcd0a940ff3f011:4:1:0 0123456789abcdef:-1:-1:-1") && !*err, "zero_skip 名单键：值间 TAB 换成空格、原样交给 nvn_mtl.m（实得「%s」）", g.zero_skip);

    s2a_game_tsv_parse("render_cond\tmaybe\n", &g, err, sizeof err);
    CK(g.render_cond == 1 && strstr(err, "render_cond") != NULL, "看不懂的值：报一行、保持缺省（实得 %d「%s」）", g.render_cond, err);
    s2a_game_tsv_parse("app_pool\tzz\n", &g, err, sizeof err);
    CK(g.app_pool == 0xCD500000ull && strstr(err, "app_pool") != NULL, "app_pool 看不懂：报一行、保持缺省");

    s2a_game_tsv_parse("render_condx\toff\nxrender_cond\toff\n", &g, err, sizeof err);
    CK(g.render_cond == 1 && !*err, "键名整词匹配（前后缀不算）");

    CK(s2a_game_tsv_load("/nonexistent/game.tsv", &g, err, sizeof err) == 0 && g.render_cond == 1, "文件不在：返回 0、全按缺省");

    if (fails) { printf("✗ t_game_tsv：%d 处不符\n", fails); return 1; }
    printf("✓ t_game_tsv：13 组用例全符（随包 game.tsv 解析、真实语义缺省、显式值优先）\n");
    return 0;
}
