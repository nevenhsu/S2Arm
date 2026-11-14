// Windows 上编 spirv-cross 命令行（spvc_inproc.cli_path，#1100 W6）时预包含进 main.cpp：命令行按 "w" 开输出文件、往 stdout 打印，
//   Windows 文本模式会把 \n 写成 \r\n；这里把输出一律改成二进制，MSL / GLSL 与 Mac 的 Homebrew 版逐字节同。main.cpp 一字不动（SPIRV-Cross 源码只有 spirv_msl.cpp 一处本仓改动，见 graphics/third_party/spirv_cross/README.md）。
#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
inline FILE *spvc_cli_fopen_bin(const char *path, const char *mode) { return ::fopen(path, mode && !strcmp(mode, "w") ? "wb" : mode); }
static const int spvc_cli_stdout_bin = (_setmode(_fileno(stdout), _O_BINARY), 0);   // 进 main 之前把 stdout 设成二进制
#define fopen spvc_cli_fopen_bin
