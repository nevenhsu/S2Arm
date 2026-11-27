// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using Ryujinx.Audio.Renderer.Server.Splitter;
using Ryujinx.Memory;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace S2A.Services.Tests
{
    // 分频器链落账（SplitterContext.S2ACommitFrom，#12/#13 拆分「渲染一拍不等 Update 的锁」）：影子上游戏改过的分频器链落回 live 后，
    //   live 每条链必须与影子同序、且不成环——渲染拍在会话锁里顺链走（SplitterState.UpdateInternalState / ForEachDestination），
    //   成环就永远不出来，会话锁不放，声音全停、即时存档拿不到锁卡死（时之笛 2/2：工作线程停在 SplitterDestinationVersion2.UpdateInternalState）。
    //   口径：真类型（SplitterContext / SplitterState / SplitterDestinationVersion2 跑在原生块上，同生产工作缓冲），
    //   穷举 4 个目的地 × 2 个分频器的全部「旧分配 → 新分配」（每个分频器一条互不相交的有序链，含空链）。
    //   期望值不依赖被测实现：落账后 live 链 = 新分配（影子）原样。最小成环例：旧 [0] / [1→2→3] → 新 [1→0] / [3→2]
    static class AudioSplitterCommitLinksTest
    {
        private const int Dests = 4;
        private const int Splitters = 2;

        private sealed unsafe class Ctx : IDisposable
        {
            public readonly SplitterContext Context = new();
            private readonly SplitterState* _s;
            private readonly SplitterDestinationVersion2* _d, _b;

            public Ctx()
            {
                _s = (SplitterState*)NativeMemory.AlignedAlloc((nuint)(sizeof(SplitterState) * Splitters), 0x10);
                _d = (SplitterDestinationVersion2*)NativeMemory.AlignedAlloc((nuint)(sizeof(SplitterDestinationVersion2) * Dests), 0x10);
                _b = (SplitterDestinationVersion2*)NativeMemory.AlignedAlloc((nuint)(sizeof(SplitterDestinationVersion2) * Dests), 0x10);
                Reset();
                Context.S2AInitializeShadow(new SplitterContext(),
                    new NativeMemoryManager<SplitterState>(_s, Splitters).Memory,
                    Memory<SplitterDestinationVersion1>.Empty,
                    new NativeMemoryManager<SplitterDestinationVersion2>(_d, Dests).Memory,
                    Memory<Ryujinx.Audio.Renderer.Dsp.State.BiquadFilterState>.Empty);
                Context._s2aDestinationBaseV2 = new NativeMemoryManager<SplitterDestinationVersion2>(_b, Dests).Memory;
            }

            public void Reset()
            {
                for (int i = 0; i < Splitters; i++) _s[i] = new SplitterState(i);
                for (int i = 0; i < Dests; i++) { _d[i] = new SplitterDestinationVersion2(i); _b[i] = new SplitterDestinationVersion2(i); }
            }

            // 按游戏解析段的语义摆链：链头 + 依次 Link，链尾 _next 为空（新内存）
            public void Lay(int[][] chains)
            {
                Reset();
                for (int s = 0; s < Splitters; s++)
                {
                    ref SplitterState st = ref Context.GetState(s);
                    int[] ch = chains[s];
                    st.DestinationCount = ch.Length;
                    if (ch.Length == 0) continue;
                    st.SetDestination(Context.GetDestination(ch[0]));
                    for (int k = 1; k < ch.Length; k++) Context.GetDestination(ch[k - 1]).Link(Context.GetDestination(ch[k]));
                }
            }

            // 照 Ryujinx SplitterState.Update 的链操作改一个分频器（ClearLinks 顺旧链清 → 链头 → 依次 Link），不 Reset 别的
            public void UpdateSplitter(int s, int[] ch)
            {
                ref SplitterState st = ref Context.GetState(s);
                st.ClearLinks();
                st.DestinationCount = ch.Length;
                if (ch.Length == 0) return;
                st.SetDestination(Context.GetDestination(ch[0]));
                for (int k = 1; k < ch.Length; k++) Context.GetDestination(ch[k - 1]).Link(Context.GetDestination(ch[k]));
            }

            public byte[] Bytes() =>
                new ReadOnlySpan<byte>(_s, sizeof(SplitterState) * Splitters).ToArray().Concat(new ReadOnlySpan<byte>(_d, sizeof(SplitterDestinationVersion2) * Dests).ToArray()).ToArray();

            // 顺链走（同 ForEachDestination），超过目的地总数即成环，返回 null
            public List<int> Walk(int s)
            {
                var ids = new List<int>();
                SplitterDestination t = Context.GetState(s).Destination;
                while (!t.IsNull)
                {
                    ids.Add(t.Id);
                    if (ids.Count > Dests) return null;
                    t = t.Next;
                }
                return ids;
            }

            public void Dispose() { NativeMemory.AlignedFree(_s); NativeMemory.AlignedFree(_d); NativeMemory.AlignedFree(_b); }
        }

        private static List<int[][]> Assignments()
        {
            var res = new List<int[][]>();
            int total = (int)Math.Pow(Splitters + 1, Dests);
            for (int code = 0; code < total; code++)
            {
                var groups = new List<int>[Splitters];
                for (int s = 0; s < Splitters; s++) groups[s] = new List<int>();
                for (int d = 0, c = code; d < Dests; d++, c /= Splitters + 1) if (c % (Splitters + 1) < Splitters) groups[c % (Splitters + 1)].Add(d);
                IEnumerable<int[][]> acc = new[] { Array.Empty<int[]>() };
                foreach (List<int> g in groups) acc = acc.SelectMany(a => Perms(g).Select(p => a.Append(p).ToArray())).ToList();
                res.AddRange(acc);
            }
            return res;
        }

        private static IEnumerable<int[]> Perms(List<int> g) =>
            g.Count == 0 ? new[] { Array.Empty<int>() } : g.SelectMany(x => Perms(g.Where(y => y != x).ToList()).Select(r => new[] { x }.Concat(r).ToArray()));

        private static string Text(int[][] c) => string.Join(" / ", c.Select(ch => "[" + string.Join("→", ch) + "]"));

        [SoloTest("--audio-splitter-commit-links")]
        private static int Check()
        {
            List<int[][]> all = Assignments();
            int pairs = 0, cycles = 0, wrong = 0; string first = null;
            using var live = new Ctx();
            using var shadow = new Ctx();
            foreach (int[][] oldC in all)
            {
                foreach (int[][] newC in all)
                {
                    pairs++;
                    live.Lay(oldC);
                    shadow.Lay(newC);
                    live.Context.S2ACommitFrom(shadow.Context);
                    bool cyc = false, bad = false; var got = new List<string>();
                    for (int s = 0; s < Splitters; s++)
                    {
                        List<int> w = live.Walk(s);
                        if (w == null) { cyc = true; got.Add("[成环]"); continue; }
                        got.Add("[" + string.Join("→", w) + "]");
                        if (!w.SequenceEqual(newC[s])) bad = true;
                    }
                    if (cyc) cycles++; else if (bad) wrong++;
                    if ((cyc || bad) && first == null) first = $"旧 {Text(oldC)} → 新 {Text(newC)}：live 落账后 {string.Join(" / ", got)}";
                }
            }
            // 钉住最小成环例：旧 [0] / [1→2→3] → 新 [1→0] / [3→2]（修前落账得 3→2→3 成环，渲染拍顺链走不出来）
            {
                int[][] oldC = { new[] { 0 }, new[] { 1, 2, 3 } }, newC = { new[] { 1, 0 }, new[] { 3, 2 } };
                live.Lay(oldC);
                shadow.Lay(newC);
                live.Context.S2ACommitFrom(shadow.Context);
                List<int> a = live.Walk(0), b = live.Walk(1);
                string got = $"{(a == null ? "[成环]" : "[" + string.Join("→", a) + "]")} / {(b == null ? "[成环]" : "[" + string.Join("→", b) + "]")}";
                bool pin = a != null && b != null && a.SequenceEqual(newC[0]) && b.SequenceEqual(newC[1]);
                Console.WriteLine($"{(pin ? "✓" : "✗")} 分频器链落账最小成环例：旧 {Text(oldC)} → 新 {Text(newC)}；期望 live [1→0] / [3→2] → 真结果 {got}");
                if (!pin) return 1;
            }
            // 整条拆分路径：live 摆旧链 → 影子快照（S2ASnapshotFrom）→ 影子上按游戏语义改有变的分频器 → 落账。
            //   oracle = 不拆分的今天语义：同一份更新用 Ryujinx 原版链操作就地跑在 live 上（不经被测的快照 / 落账）。
            //   判：① 影子阶段 live 字节不变（快照没重绑目的地 _next 时，影子 ClearLinks 顺链走进 live 在锁外拆链）；
            //   ② 落账后 live 每条链 = oracle（oracle 自己成环的配对——跨分频器挪动，原版也走不通——不计，单列个数）
            int sp = 0, spLiveWritten = 0, spWrong = 0, oracleCyc = 0; string spFirst = null;
            using var oracle = new Ctx();
            foreach (int[][] oldC in all)
            {
                foreach (int[][] newC in all)
                {
                    oracle.Lay(oldC);
                    for (int s = 0; s < Splitters; s++) if (!newC[s].SequenceEqual(oldC[s])) oracle.UpdateSplitter(s, newC[s]);
                    var want = Enumerable.Range(0, Splitters).Select(oracle.Walk).ToList();
                    if (want.Any(w => w == null)) { oracleCyc++; continue; }
                    sp++;
                    live.Lay(oldC);
                    byte[] before = live.Bytes();
                    shadow.Context.S2ASnapshotFrom(live.Context);
                    for (int s = 0; s < Splitters; s++) if (!newC[s].SequenceEqual(oldC[s])) shadow.UpdateSplitter(s, newC[s]);
                    bool written = !before.SequenceEqual(live.Bytes());
                    live.Context.S2ACommitFrom(shadow.Context);
                    var got = Enumerable.Range(0, Splitters).Select(live.Walk).ToList();
                    bool same = got.Zip(want, (g, w) => g != null && g.SequenceEqual(w)).All(x => x);
                    if (written) spLiveWritten++;
                    if (!same) spWrong++;
                    if ((written || !same) && spFirst == null)
                        spFirst = $"旧 {Text(oldC)} → 新 {Text(newC)}：影子阶段写了 live {written}；落账后 {string.Join(" / ", got.Select(g => g == null ? "[成环]" : "[" + string.Join("→", g) + "]"))}，oracle {string.Join(" / ", want.Select(w => "[" + string.Join("→", w) + "]"))}";
                }
            }
            bool spOk = spLiveWritten == 0 && spWrong == 0 && sp > 0;
            Console.WriteLine($"{(spOk ? "✓" : "✗")} 快照 → 影子更新 → 落账：{sp} 对（另 {oracleCyc} 对原版就地更新自己成环，不计）；期望 影子阶段写 live 0、落账与原版不符 0 → 真结果 写 live {spLiveWritten}、不符 {spWrong}" +
                              (spFirst != null ? $"；首例 {spFirst}" : ""));
            bool ok = cycles == 0 && wrong == 0 && spOk;
            Console.WriteLine($"{(cycles == 0 && wrong == 0 ? "✓" : "✗")} 分频器链落账：穷举 {pairs} 对旧 / 新分配（{Dests} 个目的地 × {Splitters} 个分频器）；期望 成环 0、链与影子不符 0 → 真结果 成环 {cycles}、不符 {wrong}" +
                              (first != null ? $"；首例 {first}" : ""));
            return ok ? 0 : 1;
        }
    }
}
