using Ryujinx.Graphics.Shader.IntermediateRepresentation;

using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class Simplification
    {
        private const int AllOnes = ~0;

        public static void RunPass(Operation operation)
        {
            switch (operation.Inst)
            {
                case Instruction.Add:
                    TryEliminateBinaryOpCommutative(operation, 0);
                    break;

                case Instruction.BitwiseAnd:
                    TryEliminateBitwiseAnd(operation);
                    break;

                case Instruction.BitwiseExclusiveOr:
                    if (!TryEliminateXorSwap(operation))
                    {
                        TryEliminateBinaryOpCommutative(operation, 0);
                    }

                    break;

                case Instruction.BitwiseOr:
                    TryEliminateBitwiseOr(operation);
                    break;

                case Instruction.CompareNotEqual:
                    TryEliminateCompareNotEqual(operation);
                    break;

                case Instruction.ConditionalSelect:
                    TryEliminateConditionalSelect(operation);
                    break;

                case Instruction.Divide:
                    TryEliminateBinaryOpY(operation, 1);
                    break;

                case Instruction.Multiply:
                    TryEliminateBinaryOpCommutative(operation, 1);
                    break;

                case Instruction.MultiplyHighU32:
                    TryRewriteMultiplyHighU32ByPowerOfTwo(operation);
                    break;

                case Instruction.ShiftLeft:
                case Instruction.ShiftRightS32:
                case Instruction.ShiftRightU32:
                case Instruction.Subtract:
                    TryEliminateBinaryOpY(operation, 0);
                    break;
            }
        }

        private static void TryEliminateBitwiseAnd(Operation operation)
        {
            // Try to recognize and optimize those 3 patterns (in order):
            //  x & 0xFFFFFFFF == x,          0xFFFFFFFF & y == y,
            //  x & 0x00000000 == 0x00000000, 0x00000000 & y == 0x00000000

            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (IsConstEqual(x, AllOnes))
            {
                operation.TurnIntoCopy(y);
            }
            else if (IsConstEqual(y, AllOnes))
            {
                operation.TurnIntoCopy(x);
            }
            else if (IsConstEqual(x, 0) || IsConstEqual(y, 0))
            {
                operation.TurnIntoCopy(Const(0));
            }
        }

        private static bool TryEliminateXorSwap(Operation xCopyOp)
        {
            // Try to recognize XOR swap pattern:
            //  x = x ^ y
            //  y = x ^ y
            //  x = x ^ y
            // Or, in SSA:
            //  x2 = x ^ y
            //  y2 = x2 ^ y
            //  x3 = x2 ^ y2
            // Transform it into something more sane:
            //  temp = y
            //  y = x
            //  x = temp

            // Note that because XOR is commutative, there are actually
            // multiple possible combinations of this pattern, for
            // simplicity this only catches one of them.

            Operand x = xCopyOp.GetSource(0);
            Operand y = xCopyOp.GetSource(1);

            if (x.AsgOp is not Operation tCopyOp || tCopyOp.Inst != Instruction.BitwiseExclusiveOr ||
                y.AsgOp is not Operation yCopyOp || yCopyOp.Inst != Instruction.BitwiseExclusiveOr)
            {
                return false;
            }

            if (tCopyOp == yCopyOp)
            {
                return false;
            }

            if (yCopyOp.GetSource(0) != x ||
                yCopyOp.GetSource(1) != tCopyOp.GetSource(1) ||
                x.UseOps.Count != 2)
            {
                return false;
            }

            x = tCopyOp.GetSource(0);
            y = tCopyOp.GetSource(1);

            tCopyOp.TurnIntoCopy(y); // Temp = Y
            yCopyOp.TurnIntoCopy(x); // Y = X
            xCopyOp.TurnIntoCopy(tCopyOp.Dest); // X = Temp

            return true;
        }

        private static void TryEliminateBitwiseOr(Operation operation)
        {
            // Try to recognize and optimize those 3 patterns (in order):
            //  x | 0x00000000 == x,          0x00000000 | y == y,
            //  x | 0xFFFFFFFF == 0xFFFFFFFF, 0xFFFFFFFF | y == 0xFFFFFFFF

            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (IsConstEqual(x, 0))
            {
                operation.TurnIntoCopy(y);
            }
            else if (IsConstEqual(y, 0))
            {
                operation.TurnIntoCopy(x);
            }
            else if (IsConstEqual(x, AllOnes) || IsConstEqual(y, AllOnes))
            {
                operation.TurnIntoCopy(Const(AllOnes));
            }
        }

        private static void TryEliminateBinaryOpY(Operation operation, int comparand)
        {
            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (IsConstEqual(y, comparand))
            {
                operation.TurnIntoCopy(x);
            }
        }

        private static void TryEliminateBinaryOpCommutative(Operation operation, int comparand)
        {
            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (IsConstEqual(x, comparand))
            {
                operation.TurnIntoCopy(y);
            }
            else if (IsConstEqual(y, comparand))
            {
                operation.TurnIntoCopy(x);
            }
        }

        // S2A: the high word of an unsigned 32 x 32 product by a constant 2^k (1 <= k <= 31) is exactly x >> (32 - k).
        // Address arithmetic (a 64-bit index * 16 split into words) hits this dozens of times per shader; as MultiplyHighU32 it
        // becomes SPIR-V OpUMulExtended, which the MSL backend wraps in an optnone helper: a real function call per use on the GPU.
        private static void TryRewriteMultiplyHighU32ByPowerOfTwo(Operation operation)
        {
            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (!TryGetPowerOfTwoShift(y, out int shift))
            {
                if (!TryGetPowerOfTwoShift(x, out shift))
                {
                    return;
                }

                x = y;
            }

            operation.TurnInto(Instruction.ShiftRightU32, x);
            operation.AppendSources([Const(32 - shift)]);
        }

        private static bool TryGetPowerOfTwoShift(Operand operand, out int shift)
        {
            shift = 0;

            if (operand.Type != OperandType.Constant)
            {
                return false;
            }

            uint value = (uint)operand.Value;

            if (value < 2 || (value & (value - 1)) != 0)
            {
                return false;
            }

            shift = System.Numerics.BitOperations.TrailingZeroCount(value);

            return true;
        }

        private static void TryEliminateCompareNotEqual(Operation operation)
        {
            // Comparison instruction returns 0 if the result is false, and -1 if true.
            // Doing a not equal zero comparison on the result is redundant, so we can just copy the first result in this case.

            Operand lhs = operation.GetSource(0);
            Operand rhs = operation.GetSource(1);

            if (lhs.Type == OperandType.Constant)
            {
                (lhs, rhs) = (rhs, lhs);
            }

            if (rhs.Type != OperandType.Constant || rhs.Value != 0)
            {
                return;
            }

            if (lhs.AsgOp is not Operation compareOp || !compareOp.Inst.IsComparison())
            {
                return;
            }

            operation.TurnIntoCopy(lhs);
        }

        private static void TryEliminateConditionalSelect(Operation operation)
        {
            Operand cond = operation.GetSource(0);

            if (cond.Type != OperandType.Constant)
            {
                return;
            }

            // The condition is constant, we can turn it into a copy, and select
            // the source based on the condition value.
            int srcIndex = cond.Value != 0 ? 1 : 2;

            Operand source = operation.GetSource(srcIndex);

            operation.TurnIntoCopy(source);
        }

        private static bool IsConstEqual(Operand operand, int comparand)
        {
            if (operand.Type != OperandType.Constant)
            {
                return false;
            }

            return operand.Value == comparand;
        }
    }
}
