# SPIRV-Cross（收录子集，供 GAL Metal 渲染器 SPIR-V → MSL）

来源：KhronosGroup/SPIRV-Cross 标签 vulkan-sdk-1.4.363.0（Apache-2.0，见 LICENSE），与 Homebrew spirv-cross 1.4.363.0 同版。
下载：https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz，sha256 f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721（10-18 渲染线）。
收了：核心（spirv_cross / parsed_ir / parser / cfg）+ GLSL（MSL 的基类）+ MSL + C 接口，全部头文件；源码只有一处改动（见下「本仓改动」）。

本仓改动（唯一一处，spirv_msl.cpp 搜 `S2A:`）：NoContraction 的浮点辅助函数（spvFAdd / spvFSub / spvFMul / spvFMulVectorMatrix / spvFMulMatrixVector /
  spvFMulMatrixMatrix）去掉 `[[clang::optnone]]`、改 `inline`，fma 的 0 / 1 运算元取自函数内一次 `thread volatile float` 读。
  - 为什么：optnone 隐含 noinline，GPU 上每处使用都是一次真函数调用；降精度翻译下位置链整条标 NoContraction，一支顶点着色器可有几十处。
  - 舍入不变：每步仍是一次 fma，运算元顺序与原版相同；0 / 1 不是编译期常数，前端与后端都不能把 fma(l, r, 0) 化简成 fmul（nsz 下本来合法）再去融合。
  - 后果：带这类辅助函数的 MSL 与 Homebrew spirv-cross 1.4.363.0 命令行不再逐字节相同（数值逐位相同，见 tests/msl_nocontraction_test.sh）；
    不带的照旧逐字节相同（tests/spvc_inproc_test.sh）。直连出库（进程内库）与 GAL 运行时现编用本仓源码，走 Homebrew 命令行的 Vulkan / OpenGL 款离线库照旧。
  - 链锁的 spirv-cross 一行只记版本号；直连出库的着色器缓存键含本仓源码身份（spvc_inproc.identity），改动后照常重翻。
11-01 出包线补：命令行 main.cpp 与它链接要的 HLSL / C++ / 反射后端（spirv_hlsl / spirv_cpp / spirv_reflect / spirv_cross_util 的 .cpp），同一标签、同一源码包（sha256 同上），零改动（命令行入口这几个文件没动）；
  供 translator/spvc_inproc.py 把命令行编成动态库在进程内调用（直连出库翻译段，输出与 Homebrew spirv-cross 1.4.363.0 命令行逐字节相同）。
编译开关：SPIRV_CROSS_C_API_MSL=1 SPIRV_CROSS_C_API_GLSL=1（其余语言不收）。
用途：sw/gfx/gal/gal_prog.m 的 Mac 运行时现编、iOS 首遇现编、tools/gal_xc 离线出库——三处同一份源码，生成的 MSL 一致。
