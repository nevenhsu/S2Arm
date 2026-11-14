// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 无号 32 位乘以常数 2^k 取高位（MultiplyHighU32）改写成右移：x * 2^k 的高 32 位 = x >> (32 - k)，1 <= k <= 31。
// 跑生产函数 Simplification.RunPass（Optimizer 每轮对每个运算都调它），断言改写后的指令、来源与使用登记；
// 期望不跑被测实现：数值对照直接按 64 位乘积算（((ulong)x * c) >> 32），与改写后的右移逐个比。
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation.Optimizations;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class Program
{
    private static int _passed;
    private static int _failed;

    private static void Check(string name, bool ok)
    {
        Console.WriteLine($"{(ok ? "通过" : "失败")}  {name}");
        if (ok) _passed++; else _failed++;
    }

    private static readonly uint[] Xs =
    [
        0, 1, 2, 3, 15, 16, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff,
        0x0fffffff, 0x10000000, 0xf0000000, 0x55555555, 0xaaaaaaaa, 0x12345678, 0xdeadbeef,
    ];

    // 一个 MultiplyHighU32 运算：x 是局部变量（另一侧是常数）或常数本身
    private static (Operation Op, Operand X) Make(bool constLeft, uint c)
    {
        Operand x = Local();
        Operation op = constLeft
            ? new Operation(Instruction.MultiplyHighU32, Local(), Const((int)c), x)
            : new Operation(Instruction.MultiplyHighU32, Local(), x, Const((int)c));
        return (op, x);
    }

    // 改写后的运算按自己的语义求值（只认 ShiftRightU32 / MultiplyHighU32），与 64 位乘积比
    private static bool ValuesMatch(Operation op, uint c)
    {
        foreach (uint x in Xs)
        {
            ulong want = ((ulong)x * c) >> 32;
            ulong got = op.Inst switch
            {
                Instruction.ShiftRightU32 => x >> op.GetSource(1).Value,
                Instruction.MultiplyHighU32 => ((ulong)x * c) >> 32,
                _ => ulong.MaxValue,
            };
            if (got != want) return false;
        }
        return true;
    }

    private static void Rewritten(uint c, bool constLeft, int wantShift)
    {
        (Operation op, Operand x) = Make(constLeft, c);
        Simplification.RunPass(op);
        string side = constLeft ? "常数在左" : "常数在右";
        Check($"x * 0x{c:x} 取高位（{side}）→ ShiftRightU32(x, {wantShift})",
            op.Inst == Instruction.ShiftRightU32 && op.SourcesCount == 2 && op.GetSource(0) == x &&
            op.GetSource(1).Type == OperandType.Constant && op.GetSource(1).Value == wantShift);
        Check($"x * 0x{c:x}（{side}）：x 的使用登记只剩改写后的这一个运算", x.UseOps.Count == 1 && x.UseOps.Contains(op));
        Check($"x * 0x{c:x}（{side}）：{Xs.Length} 个边界值与 64 位乘积高位逐个相同", ValuesMatch(op, c));
    }

    private static void Untouched(string name, Operation op)
    {
        Instruction before = op.Inst;
        Simplification.RunPass(op);
        Check(name, op.Inst == before && op.SourcesCount == 2);
    }

    public static int Main()
    {
        Rewritten(16, constLeft: false, wantShift: 28);
        Rewritten(16, constLeft: true, wantShift: 28);
        Rewritten(2, constLeft: false, wantShift: 31);          // k = 1
        Rewritten(4, constLeft: false, wantShift: 30);
        Rewritten(65536, constLeft: false, wantShift: 16);
        Rewritten(0x80000000, constLeft: false, wantShift: 1);  // k = 31（常数的位型是 int.MinValue）
        Rewritten(0x80000000, constLeft: true, wantShift: 1);

        Untouched("x * 1 取高位：不改（2^0，高位恒 0，留给常量折叠以外的现有路径）", Make(false, 1).Op);
        Untouched("x * 0 取高位：不改", Make(false, 0).Op);
        Untouched("x * 3 取高位：不是 2 的幂，不改", Make(false, 3).Op);
        Untouched("x * 0xfffffffe 取高位：不是 2 的幂，不改", Make(false, 0xfffffffe).Op);
        Untouched("x * y（两侧都不是常数）：不改", new Operation(Instruction.MultiplyHighU32, Local(), Local(), Local()));
        Untouched("有号 MultiplyHighS32(x, 16)：不改（有号高位不是逻辑右移）",
            new Operation(Instruction.MultiplyHighS32, Local(), Local(), Const(16)));

        Console.WriteLine($"\n{_passed} 通过 / {_failed} 失败");
        return _failed == 0 ? 0 : 1;
    }
}
