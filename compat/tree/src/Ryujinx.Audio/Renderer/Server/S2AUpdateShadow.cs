using Ryujinx.Audio.Common;
using Ryujinx.Audio.Renderer.Common;
using Ryujinx.Audio.Renderer.Dsp.State;
using Ryujinx.Audio.Renderer.Parameter;
using Ryujinx.Audio.Renderer.Server.Effect;
using Ryujinx.Audio.Renderer.Server.MemoryPool;
using Ryujinx.Audio.Renderer.Server.Mix;
using Ryujinx.Audio.Renderer.Server.Sink;
using Ryujinx.Audio.Renderer.Server.Splitter;
using Ryujinx.Audio.Renderer.Server.Voice;
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using static Ryujinx.Audio.Renderer.Common.BehaviourParameter;
using CommonPlayState = Ryujinx.Audio.Renderer.Common.PlayState;

namespace Ryujinx.Audio.Renderer.Server
{
    // S2Arm (#12/#13 拆分 · 渲染拍不等 Update 的锁 · 维护方 #12 主方向:「Update 把解析结果写进影子状态、解析在锁外
    //   完成、只在交换/落账时短暂持锁」的载体,一个渲染器一份、一次构建、每更新复用(零稳态分配;效果/汇型变是
    //   罕见路径,与今天 ResetEffect/ResetSink 一样按需建新实例)。
    //
    //   三段流程(驱动在 AudioRenderSystem.S2AParseLocked / S2ACommitLocked,本类只持状态与拷贝件):
    //   ① 锁1(短):live→影子整组快照(结构体 memcpy + 效果/汇逐型字段拷贝 + 分频指针重绑),持锁只剩这一小段;
    //   ② 解析段(锁外):StateUpdater 各段跑在影子上下文上——声音段由驱动逐槽驱动(VoiceInfo.S2AUpdateWaveBuffer
    //      纯核),逐槽记录 isValid 读数与 hook 起点,重发错误按 (voiceId,波表) 暂存(不进任何活队列,处方③);
    //   ③ 锁2a(短):按字段矩阵落账 + 波表翻转重演(处方①②:基底=锁1 快照槽,重跑 isValid 分支与重发分支两者,
    //      hook 槽接序 [Unmap(A_old)→Map×2(A_new)]——强制映射路径本样双 Map,PoolMapper.AssignDspAddress)+ hook 日志
    //      按原序重放 + 暂存错误每槽恰一次按原序 AppendError。
    //
    //   字段矩阵(各拷贝方法逐处标注):游戏写→落账拷;拍写(渲染调度线程 UpdateForCommandGeneration / DSP 锁外独写)
    //   →保留 live;两写→按锁1 快照基底落(与波表重演基底同口径的今天可能序)。声音的播放态/IsNew/Flush 计数是
    //   三处例外:落账用纯函数对「活」值重放 = 「Update 在拍后串行」的今天语义(VoiceInfo.S2AUpdatePlayState 等)。
    //   原生块:池表与分频链表(影子槽里带指针的存储)必须不随 GC 移动——影子地址信息指向影子池表、影子链指向
    //   影子目的地数组,指针运算按下标平移落账重绑。
    internal sealed unsafe class S2AUpdateShadow
    {
        // ---------- 计数 ----------
        private readonly int _poolCount;
        private readonly int _voiceCount;
        private readonly int _effectCount;
        private readonly int _resultStateCount;
        private readonly int _sinkCount;
        private readonly int _mixCount;
        private readonly int _splitterCount;
        private readonly int _destinationCount;
        private readonly uint _processHandle;

        // ---------- live 侧(构建后不变)----------
        private readonly Memory<MemoryPoolInfo> _livePools;
        private readonly VoiceContext _liveVoiceContext;
        private readonly EffectContext _liveEffectContext;
        private readonly SinkContext _liveSinkContext;
        private readonly SplitterContext _liveSplitterContext;
        private readonly MixContext _liveMixContext;

        // ---------- 原生影子块(指针稳定)----------
        private byte* _nativeBlock;
        private readonly byte* _shadowPoolsBase;
        private Memory<MemoryPoolInfo> _shadowPools;
        private Memory<SplitterState> _shadowSplitters;
        private Memory<SplitterDestinationVersion1> _shadowDestinationsV1;
        private Memory<SplitterDestinationVersion2> _shadowDestinationsV2;
        private Memory<SplitterDestinationVersion1> _destinationBaseV1;
        private Memory<SplitterDestinationVersion2> _destinationBaseV2;

        // ---------- 影子上下文(真类实例,影子存储)----------
        internal readonly VoiceContext VoiceContext;
        internal readonly EffectContext EffectContext;
        internal readonly SinkContext SinkContext;
        internal readonly SplitterContext SplitterContext;
        internal readonly MixContext MixContext;

        // ---------- 影子数组(托管;结构体里的指针都指原生块,托管数组被 GC 移动不影响指针有效性)----------
        private readonly Memory<VoiceInfo> _shadowVoices;
        private readonly Memory<VoiceInfo> _voiceSnapshot;          // 波表重演基底(处方①)
        private readonly Memory<VoiceState> _shadowCpuStates;       // 解析段工作拷(读 isValid/写 IsNew 清位)
        private readonly Memory<VoiceChannelResource> _shadowResources;

        // ---------- 解析 BehaviourInfo(旗标/错误队列的私有拷贝;落账把新旗标拷回 live)----------
        internal BehaviourInfo ParseBehaviour = new();   // 非 readonly:纯核按 ref 收它(只读用法)

        // ---------- hook 日志(解析段记、落账段按原序重放)----------
        internal readonly S2AHookLog HookLog;
        internal int PoolsHookStart;      // 各段 hook 记录区间起点(落账按段位重放;声音段内再按槽细分)
        internal int VoicesHookStart;
        internal int EffectsHookStart;
        internal int SinksHookStart;

        // ---------- 声音段解析记录(落账要的参数标志 / 通道表 / 逐槽 isValid 与 hook 起点)----------
        internal readonly bool[] VoiceInUse;
        internal readonly bool[] VoiceIsNew;
        internal readonly CommonPlayState[] VoicePlayState;
        internal readonly byte[] VoiceFlushCount;
        internal readonly int[] VoiceChannelCount;
        internal readonly int[] VoiceChannelIds;        // voiceCount × 6 平铺(落账清位与复核用)
        internal readonly int[] VoiceHookStart;         // 每 voice 在日志里的起点(param 挂接 + 四槽;每更新每声都记,含不在用声——段界不留上一更新残值)
        internal readonly int[] PoolHookStart;          // 逐池 hook 起点(池段每池至多一条 attach/detach 钩;落账 detach 复核要用)

        // ---------- 逐槽记录 ----------
        internal struct VoiceSlotRecord
        {
            internal bool IsValid;            // 解析段读到的 CPU 侧 IsWaveBufferValid(锁2a 复核的对照基准)
            internal bool BufferUnmappedIn; // 进槽时的 voice 级 BufferInfoUnmapped(翻转重演的输入)
            internal SampleFormat SampleFormat;
            internal WaveBufferInternal Input;   // 输入波表(重发分支输入,翻转重演照重跑)
            internal int HookStart;              // 该槽 hook 记录起点(翻转重演的 Unmap 插它前面,处方②的时序)
        }

        internal readonly VoiceSlotRecord[] VoiceSlots;     // voiceCount × 4 平铺

        // ---------- 错误收集(处方③:重发错误按槽暂存、落账恰一次原序入队;上限与 BehaviourInfo.AppendError 同)----------
        internal readonly ErrorInfo[] Errors = new ErrorInfo[Constants.MaxErrorInfos];
        internal int ErrorCount;

        // ---------- 解析结果状态(驱动写、落账读)----------
        internal ResultCode ParseResult;
        internal int ParseStop;                // 落账走到第几段(0..9):失败点之前的前置段照落账;池/资源/分频/混音段
                                               // 失败时该段也整段落账——段内未跑到的部分影子=锁1 活值快照,而池表
                                               // (除 IsUsed)、声道资源(除 PreviousMix)、分频结构体、混音结构体没有
                                               // 拍写侧,整段落账与今天的前缀语义一致
        internal long ConsumedInput;            // 解析段末输入消费(CheckConsumedSize 口径,落账核算)
        internal int OutputCursorLength;        // 解析段末输出游标余长(消费口径)
        internal int OutputTotalLength;         // 输出总长(消费口径)
        internal int ErrorOutOffset;            // 错误段在输出里的字节位置(落账写入)
        internal int RendererInfoOutOffset;     // 信息段位置(落账写入)
        internal int EffectOutStatusSize;       // 效果段每条尺寸(V1/V2 按修订)
        internal bool ElapsedFrameCountSupported;
        internal bool PerfManagerPresent;
        internal int PerfTargetNodeId;
        internal bool IsActiveSnapshot;        // 锁1 快照的 _isActive(效果 StoreStatus 读)
        internal readonly bool[] EffectIsNew;   // 效果段 IsNew(解析段独立扫输入;落账对 live 重放 InitializeResultState)

        // NodeStates.Initialize 经 SpanMemoryManager<int>.Cast 把工作缓冲的地址存成裸指针（上游只给它不会搬的工作缓冲），
        //   所以这块必须固定：普通 new byte[] 会被 GC 压缩搬走，之后每次排序都写回旧地址——旧地址已解除提交时当场 SIGBUS，
        //   已被别的对象占用时悄悄写坏托管堆，等后台 GC 标记时才崩。分配在固定对象堆（POH），永不搬动
        internal static byte[] S2ANodeStatesWorkBuffer(int mixCount) => GC.AllocateArray<byte>(NodeStates.GetWorkBufferSize(mixCount), pinned: true);

        internal S2AUpdateShadow(
            uint processHandle,
            Memory<MemoryPoolInfo> livePools,
            VoiceContext liveVoiceContext,
            EffectContext liveEffectContext,
            SinkContext liveSinkContext,
            SplitterContext liveSplitterContext,
            MixContext liveMixContext,
            int poolCount,
            int voiceCount,
            int effectCount,
            int resultStateCount,
            int sinkCount,
            int mixCount,
            int hookLogCapacity)
        {
            _processHandle = processHandle;
            _livePools = livePools;
            _liveVoiceContext = liveVoiceContext;
            _liveEffectContext = liveEffectContext;
            _liveSinkContext = liveSinkContext;
            _liveSplitterContext = liveSplitterContext;
            _liveMixContext = liveMixContext;
            _poolCount = poolCount;
            _voiceCount = voiceCount;
            _effectCount = effectCount;
            _resultStateCount = resultStateCount;
            _sinkCount = sinkCount;
            _mixCount = mixCount;
            _splitterCount = liveSplitterContext.S2ASplitterCount;
            _destinationCount = liveSplitterContext.S2ADestinationCount;

            HookLog = new S2AHookLog(hookLogCapacity);

            // ---- 原生影子块:池表 + 分频结构体 + 目的地 + 目的地基底(Alignment 0x10 逐段对齐)----
            int poolSize = poolCount * sizeof(MemoryPoolInfo);
            int splitterSize = _splitterCount * sizeof(SplitterState);
            int destSize = _destinationCount * (liveSplitterContext.Version == 2 ? sizeof(SplitterDestinationVersion2) : sizeof(SplitterDestinationVersion1));
            int nativeSize = Align(poolSize + splitterSize + destSize * 2, 0x10);

            _nativeBlock = (byte*)System.Runtime.InteropServices.NativeMemory.AlignedAlloc((nuint)nativeSize, 0x10);
            NativeMemory.Fill(_nativeBlock, (nuint)nativeSize, 0);

            // 逐段按型包 Memory(原生块,指针稳定;池表/分频链表必须原生——影子槽里的池指针/链指针指着它们)
            _shadowPools = new Ryujinx.Memory.NativeMemoryManager<MemoryPoolInfo>((MemoryPoolInfo*)_nativeBlock, poolCount).Memory;
            _shadowPoolsBase = _nativeBlock;

            int offset = Align(poolSize, 0x10);
            _shadowSplitters = new Ryujinx.Memory.NativeMemoryManager<SplitterState>((SplitterState*)(_nativeBlock + offset), _splitterCount).Memory;

            offset = Align(offset + splitterSize, 0x10);
            if (liveSplitterContext.Version == 2)
            {
                _shadowDestinationsV2 = new Ryujinx.Memory.NativeMemoryManager<SplitterDestinationVersion2>((SplitterDestinationVersion2*)(_nativeBlock + offset), _destinationCount).Memory;
                offset = Align(offset + destSize, 0x10);
                _destinationBaseV2 = new Ryujinx.Memory.NativeMemoryManager<SplitterDestinationVersion2>((SplitterDestinationVersion2*)(_nativeBlock + offset), _destinationCount).Memory;
            }
            else
            {
                _shadowDestinationsV1 = new Ryujinx.Memory.NativeMemoryManager<SplitterDestinationVersion1>((SplitterDestinationVersion1*)(_nativeBlock + offset), _destinationCount).Memory;
                offset = Align(offset + destSize, 0x10);
                _destinationBaseV1 = new Ryujinx.Memory.NativeMemoryManager<SplitterDestinationVersion1>((SplitterDestinationVersion1*)(_nativeBlock + offset), _destinationCount).Memory;
            }

            // ---- 影子数组(托管)----
            _shadowVoices = new VoiceInfo[voiceCount];
            _voiceSnapshot = new VoiceInfo[voiceCount];
            _shadowCpuStates = new VoiceState[voiceCount];
            _shadowResources = new VoiceChannelResource[voiceCount];

            VoiceContext = new VoiceContext();
            VoiceContext.Initialize(
                new int[voiceCount],
                _shadowVoices,
                _shadowResources,
                _shadowCpuStates,
                liveVoiceContext.S2AVoiceStatesDsp,   // DSP 侧与 live 共享:解析段不写它(DSP 在锁外独写)
                (uint)voiceCount);

            EffectContext = new EffectContext();
            EffectContext.Initialize((uint)effectCount, (uint)resultStateCount);

            SinkContext = new SinkContext();
            SinkContext.Initialize((uint)sinkCount);

            SplitterContext = new SplitterContext();
            SplitterContext.S2AInitializeShadow(
                liveSplitterContext,
                _shadowSplitters,
                _shadowDestinationsV1,
                _shadowDestinationsV2,
                liveSplitterContext.S2ABqfStates);   // 双二阶滤波状态与 live 共享(游戏段只读)
            SplitterContext._s2aDestinationBaseV1 = _destinationBaseV1;   // 目的地锁1 基底(落账增量判据)
            SplitterContext._s2aDestinationBaseV2 = _destinationBaseV2;

            MixContext = new MixContext();
            MixContext.Initialize(
                new int[mixCount],
                new MixInfo[mixCount],
                S2ANodeStatesWorkBuffer(mixCount),
                new byte[EdgeMatrix.GetWorkBufferSize(mixCount)]);

            // ---- 声音段记录 ----
            VoiceInUse = new bool[voiceCount];
            VoiceIsNew = new bool[voiceCount];
            VoicePlayState = new CommonPlayState[voiceCount];
            VoiceFlushCount = new byte[voiceCount];
            VoiceChannelCount = new int[voiceCount];
            VoiceChannelIds = new int[voiceCount * Constants.VoiceChannelCountMax];
            VoiceHookStart = new int[voiceCount];
            PoolHookStart = new int[poolCount];
            VoiceSlots = new VoiceSlotRecord[voiceCount * Constants.VoiceWaveBufferCount];
            EffectIsNew = new bool[effectCount];
        }

        // S2Arm (#12/#13 拆分): 驱动与落账要的影子存储访问
        internal Span<MemoryPoolInfo> ShadowPoolsSpan => _shadowPools.Span;
        internal Memory<MemoryPoolInfo> ShadowPoolsMemory => _shadowPools;
        internal Span<VoiceInfo> VoiceSnapshotSpan => _voiceSnapshot.Span;

        internal static int Align(int size, int alignment) => (size + alignment - 1) & ~(alignment - 1);

        internal void DisposeNative()
        {
            if (_nativeBlock != null)
            {
                NativeMemory.AlignedFree(_nativeBlock);
                _nativeBlock = null;
            }
        }

        // 每更新的记录清零(快照前)
        internal void ResetRecords()
        {
            HookLog.Clear();
            ErrorCount = 0;
            PoolsHookStart = VoicesHookStart = EffectsHookStart = SinksHookStart = 0;
            ParseResult = ResultCode.Success;
            ParseStop = 0;
            ErrorOutOffset = RendererInfoOutOffset = 0;
        }

        internal void RecordError(in ErrorInfo error)
        {
            if (ErrorCount < Errors.Length)
            {
                Errors[ErrorCount++] = error;
            }
        }

        // 锁1:live→影子整组快照(在锁内调)。效果/汇逐型实例拷贝、分频指针重绑都在这里。
        internal void SnapshotFromLive(BehaviourInfo liveBehaviour, bool isActive)
        {
            ResetRecords();

            ParseBehaviour.SetUserRevision(liveBehaviour.UserRevision);
            ParseBehaviour.UpdateFlags(liveBehaviour.S2AFlags);
            ParseBehaviour.ClearError();

            IsActiveSnapshot = isActive;

            _livePools.Span.CopyTo(_shadowPools.Span);

            for (int i = 0; i < _voiceCount; i++)
            {
                ref VoiceInfo live = ref _liveVoiceContext.GetState(i);

                _shadowVoices.Span[i] = live;
                _voiceSnapshot.Span[i] = live;
                _shadowCpuStates.Span[i] = _liveVoiceContext.GetUpdateStateForCpu(i).Span[0];
                _shadowResources.Span[i] = _liveVoiceContext.GetChannelResource(i);
            }

            for (int i = 0; i < _effectCount; i++)
            {
                ref BaseEffect live = ref _liveEffectContext.GetEffect(i);
                ref BaseEffect shadow = ref EffectContext.GetEffect(i);

                if (shadow.TargetEffectType != live.TargetEffectType)
                {
                    // 影子实例型不符(上一拍型变刚落账,或首次构建):按 live 型补建(罕见路径,与今天 ResetEffect 同类)
                    shadow = BaseEffect.S2ACreate(live.TargetEffectType, live is BiquadFilterEffect biquad && biquad.BiquadFilterEffectVersion == 2);
                }

                shadow.S2AShadowCopyGameFieldsFrom(live);
            }

            for (int i = 0; i < _resultStateCount; i++)
            {
                EffectContext.GetState(i) = _liveEffectContext.GetState(i);
                EffectContext.GetDspState(i) = _liveEffectContext.GetDspState(i);
            }

            for (int i = 0; i < _sinkCount; i++)
            {
                ref BaseSink live = ref _liveSinkContext.GetSink(i);
                ref BaseSink shadow = ref SinkContext.GetSink(i);

                if (shadow.TargetSinkType != live.TargetSinkType)
                {
                    shadow = BaseSink.S2ACreate(live.TargetSinkType);
                }

                shadow.S2AShadowCopyGameFieldsFrom(live);
            }

            // 分频:结构体整拷 + 指针重绑(链头与每个目的地的 _next 都平移到影子数组)+ 目的地基底拷贝,见 SplitterContext.S2ASnapshotFrom
            SplitterContext.S2ASnapshotFrom(_liveSplitterContext);

            _liveMixContext.S2ASnapshotTo(MixContext);
        }

        // 落账重绑:影子槽里的池指针指影子池表(原生块)→ live 池表同下标;快照拷里没被重挂的地址信息仍指 live
        //   (工作缓冲原生块),保持原样即正确
        internal void S2ARebindAddressInfo(ref AddressInfo info)
        {
            nint ptr = info.S2AMemoryPoolPointer;

            if (ptr == 0)
            {
                return;
            }

            long offset = ptr - (nint)_shadowPoolsBase;

            if (offset >= 0 && offset < _poolCount * (long)sizeof(MemoryPoolInfo) && (offset % sizeof(MemoryPoolInfo)) == 0)
            {
                info.SetupMemoryPool(_livePools.Span.Slice((int)(offset / sizeof(MemoryPoolInfo))));
            }
        }
    }
}
