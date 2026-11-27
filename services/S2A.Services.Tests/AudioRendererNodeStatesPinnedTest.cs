using Ryujinx.Audio.Renderer.Common;
using Ryujinx.Audio.Renderer.Server;
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace S2A.Services.Tests
{
    // 影子混音器的 NodeStates 工作缓冲必须不随 GC 搬动（compat/tree/.../Renderer/Server/S2AUpdateShadow.cs S2ANodeStatesWorkBuffer）：
    //   NodeStates.Initialize 经 SpanMemoryManager<int>.Cast 把缓冲地址存成裸指针，之后每次混音排序都按这个地址写。
    //   做法：先垫一批垃圾数组，再分配工作缓冲和一块同样大小的普通数组（负对照），丢掉垃圾后强制做三次压缩式完整 GC。
    //   期望：负对照的地址变了（证明这次 GC 真的压缩、搬动了后面的对象）；工作缓冲的地址不变。
    //   改前（普通 new byte[]）工作缓冲也会被搬走，本测红
    public static unsafe partial class Program
    {
        [SoloTest("--audio-renderer-nodestates-pinned")]
        private static int CheckNodeStatesPinned()
        {
            int bad = 0;

            void Row(string name, string expect, string actual, bool ok)
            {
                bad += ok ? 0 : 1;

                Console.WriteLine($"{(ok ? "✓" : "✗")} NodeStates 工作缓冲 {name}:{expect} → {actual}");
            }

            object[] junk = new object[4096];

            for (int i = 0; i < junk.Length; i++)
            {
                junk[i] = new byte[256];
            }

            byte[] work = S2AUpdateShadow.S2ANodeStatesWorkBuffer(64);
            byte[] control = new byte[work.Length];
            long work0 = NodeStatesPinnedAddr(work), control0 = NodeStatesPinnedAddr(control);

            junk = null;

            for (int i = 0; i < 3; i++)
            {
                GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            }

            long work1 = NodeStatesPinnedAddr(work), control1 = NodeStatesPinnedAddr(control);

            bool controlMoved = control0 != control1, workStayed = work0 == work1;

            Row("负对照（普通数组）被压缩搬动", "地址改变", $"{control0:x} → {control1:x}", controlMoved);
            Row("压缩式 GC 后不搬动", "地址不变", $"{work0:x} → {work1:x}", workStayed);
            Row("大小", NodeStates.GetWorkBufferSize(64).ToString(), work.Length.ToString(), work.Length == (int)NodeStates.GetWorkBufferSize(64));

            GC.KeepAlive(work);
            GC.KeepAlive(control);

            return bad;
        }

        // 不内联：每次调用都重新取当时的数组地址，不让 JIT 把 GC 前后的两次读合并
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static long NodeStatesPinnedAddr(byte[] a) => (long)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(a));
    }
}
