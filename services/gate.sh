#!/bin/bash
# 门禁路由：services/ compat/ graphics/metal/cs graphics/loop graphics/widen graphics/metal/gal_abi.h graphics/metal/tools/abi_hash.py tests/visual/galrec platform/apple/cs/
# 测试数据：「01006F8002326000登记 + fs 读 RomFS」「系统数据按号」读 S2A_TEST_GAME（含 romfs/）/ S2A_TEST_SYSTEMDATA（含 0100000000000802/），门禁每台都配；设了却缺数据 = 红，不设（本地开发）记未覆盖
# 门禁钩子（测试出包线 tools/gate.sh 自动跑）：服务层单测（含零分配硬判据：高频命令每请求 / 声音线程每轮 / 空闲底噪 0 B）。
# 退出码 0 = 绿；最后一行是汇总（期望 → 真结果）。
cd "$(dirname "$0")/.." || exit 2
out=$(bash services/test.sh 2>&1); rc=$?
echo "$out" | grep -E "✗" | head -20
summary=$(echo "$out" | grep -E "^共 " | tail -1)
solo=$(echo "$out" | grep -E "^服务专项汇总：" | tail -1); solo=${solo#服务专项汇总：}
[ -n "$summary" ] || echo "$out" | tail -20 | sed 's/^/  | /'   # 没汇总行 = 编译或运行失败：打出末尾，门禁日志里就能看到原因
# 射程两段都报（11-27 维护者：加了专项「共 N 项」不变，报绿看不出专项跑了几个）：主测试一个进程的汇总 + 独立进程的专项个数。
#   专项先跑、有失败就不跑主测试：主测试写「没跑（专项先失败）」
if [ -z "$summary" ] && [ -n "$solo" ] && [ "${solo#*失败}" != "$solo" ]; then main="没跑（专项先失败）"; else main=${summary:-没有汇总行（编译或运行失败）}; fi
echo "服务层单测：期望 全部符合 → 真结果 主测试 ${main}；专项 ${solo:-没有汇总行（编译或运行失败）}"
exit $rc
