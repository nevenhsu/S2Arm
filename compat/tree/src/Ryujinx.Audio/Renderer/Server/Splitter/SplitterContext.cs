using Ryujinx.Audio.Renderer.Common;
using Ryujinx.Audio.Renderer.Dsp.State;
using Ryujinx.Audio.Renderer.Parameter;
using Ryujinx.Audio.Renderer.Utils;
using Ryujinx.Common;
using Ryujinx.Common.Extensions;
using System;
using System.Buffers;
using System.Diagnostics;
using System.Runtime.CompilerServices;

namespace Ryujinx.Audio.Renderer.Server.Splitter
{
    /// <summary>
    /// Splitter context.
    /// </summary>
    public class SplitterContext
    {
        /// <summary>
        /// Amount of biquad filter states per splitter destination.
        /// </summary>
        public const int BqfStatesPerDestination = 4;

        /// <summary>
        /// Storage for <see cref="SplitterState"/>.
        /// </summary>
        private Memory<SplitterState> _splitters;

        /// <summary>
        /// Storage for <see cref="SplitterDestinationVersion1"/>.
        /// </summary>
        private Memory<SplitterDestinationVersion1> _splitterDestinationsV1;

        /// <summary>
        /// Storage for <see cref="SplitterDestinationVersion2"/>.
        /// </summary>
        private Memory<SplitterDestinationVersion2> _splitterDestinationsV2;

        /// <summary>
        /// Splitter biquad filtering states.
        /// </summary>
        private Memory<BiquadFilterState> _splitterBqfStates;

        /// <summary>
        /// Version of the splitter context that is being used, currently can be 1 or 2.
        /// </summary>
        public int Version { get; private set; }

        /// <summary>
        /// If set to true, trust the user destination count in <see cref="SplitterState.Update(SplitterContext, in SplitterInParameter, ref SequenceReader{byte})"/>.
        /// </summary>
        public bool IsBugFixed { get; private set; }

        /// <summary>
        /// If set to true, the previous mix volume is explicitly resetted using the input parameter, instead of implicitly on first use.
        /// </summary>
        public bool IsSplitterPrevVolumeResetSupported { get; private set; }
        
        /// <summary>
        /// If set to true, the previous mix volume is explicitly resetted using the input parameter, instead of implicitly on first use.
        /// </summary>
        public bool IsBiquadFilterParameterFloatSupported { get; private set; }

        /// <summary>
        /// Initialize <see cref="SplitterContext"/>.
        /// </summary>
        /// <param name="behaviourInfo">The behaviour context.</param>
        /// <param name="parameter">The audio renderer configuration.</param>
        /// <param name="workBufferAllocator">The <see cref="WorkBufferAllocator"/>.</param>
        /// <param name="splitterBqfStates">Memory to store the biquad filtering state for splitters during processing.</param>
        /// <returns>Return true if the initialization was successful.</returns>
        public bool Initialize(
            ref BehaviourInfo behaviourInfo,
            ref AudioRendererConfiguration parameter,
            WorkBufferAllocator workBufferAllocator,
            Memory<BiquadFilterState> splitterBqfStates)
        {
            if (!behaviourInfo.IsSplitterSupported() || parameter.SplitterCount <= 0 || parameter.SplitterDestinationCount <= 0)
            {
                Setup(Memory<SplitterState>.Empty, Memory<SplitterDestinationVersion1>.Empty, Memory<SplitterDestinationVersion2>.Empty, false);

                return true;
            }

            Memory<SplitterState> splitters = workBufferAllocator.Allocate<SplitterState>(parameter.SplitterCount, SplitterState.Alignment);

            if (splitters.IsEmpty)
            {
                return false;
            }

            int splitterId = 0;

            foreach (ref SplitterState splitter in splitters.Span)
            {
                splitter = new SplitterState(splitterId++);
            }

            Memory<SplitterDestinationVersion1> splitterDestinationsV1 = Memory<SplitterDestinationVersion1>.Empty;
            Memory<SplitterDestinationVersion2> splitterDestinationsV2 = Memory<SplitterDestinationVersion2>.Empty;

            if (!behaviourInfo.IsBiquadFilterParameterForSplitterEnabled())
            {
                Version = 1;

                splitterDestinationsV1 = workBufferAllocator.Allocate<SplitterDestinationVersion1>(parameter.SplitterDestinationCount,
                    SplitterDestinationVersion1.Alignment);

                if (splitterDestinationsV1.IsEmpty)
                {
                    return false;
                }

                int splitterDestinationId = 0;
                foreach (ref SplitterDestinationVersion1 data in splitterDestinationsV1.Span)
                {
                    data = new SplitterDestinationVersion1(splitterDestinationId++);
                }
            }
            else
            {
                Version = 2;

                splitterDestinationsV2 = workBufferAllocator.Allocate<SplitterDestinationVersion2>(parameter.SplitterDestinationCount,
                    SplitterDestinationVersion2.Alignment);

                if (splitterDestinationsV2.IsEmpty)
                {
                    return false;
                }

                int splitterDestinationId = 0;
                foreach (ref SplitterDestinationVersion2 data in splitterDestinationsV2.Span)
                {
                    data = new SplitterDestinationVersion2(splitterDestinationId++);
                }

                if (parameter.SplitterDestinationCount > 0)
                {
                    // Official code stores it in the SplitterDestinationVersion2 struct,
                    // but we don't to avoid using unsafe code.

                    splitterBqfStates.Span.Clear();
                    _splitterBqfStates = splitterBqfStates;
                }
                else
                {
                    _splitterBqfStates = Memory<BiquadFilterState>.Empty;
                }
            }

            IsSplitterPrevVolumeResetSupported = behaviourInfo.IsSplitterPrevVolumeResetSupported();
            IsBiquadFilterParameterFloatSupported = behaviourInfo.IsBiquadFilterParameterFloatSupported();

            SplitterState.InitializeSplitters(splitters.Span);

            Setup(splitters, splitterDestinationsV1, splitterDestinationsV2, behaviourInfo.IsSplitterBugFixed());

            return true;
        }

        /// <summary>
        /// Get the work buffer size while adding the size needed for splitter to operate.
        /// </summary>
        /// <param name="size">The current size.</param>
        /// <param name="behaviourInfo">The behaviour context.</param>
        /// <param name="parameter">The renderer configuration.</param>
        /// <returns>Return the new size taking splitter into account.</returns>
        public static ulong GetWorkBufferSize(ulong size, ref BehaviourInfo behaviourInfo, ref AudioRendererConfiguration parameter)
        {
            if (behaviourInfo.IsSplitterSupported())
            {
                size = WorkBufferAllocator.GetTargetSize<SplitterState>(size, parameter.SplitterCount, SplitterState.Alignment);

                if (behaviourInfo.IsBiquadFilterParameterForSplitterEnabled())
                {
                    size = WorkBufferAllocator.GetTargetSize<SplitterDestinationVersion2>(size, parameter.SplitterDestinationCount, SplitterDestinationVersion2.Alignment);
                }
                else
                {
                    size = WorkBufferAllocator.GetTargetSize<SplitterDestinationVersion1>(size, parameter.SplitterDestinationCount, SplitterDestinationVersion1.Alignment);
                }

                if (behaviourInfo.IsSplitterBugFixed())
                {
                    size = WorkBufferAllocator.GetTargetSize<int>(size, parameter.SplitterDestinationCount, 0x10);
                }
            }

            return size;
        }

        /// <summary>
        /// Setup the <see cref="SplitterContext"/> instance.
        /// </summary>
        /// <param name="splitters">The <see cref="SplitterState"/> storage.</param>
        /// <param name="splitterDestinationsV1">The <see cref="SplitterDestinationVersion1"/> storage.</param>
        /// <param name="splitterDestinationsV2">The <see cref="SplitterDestinationVersion2"/> storage.</param>
        /// <param name="isBugFixed">If set to true, trust the user destination count in <see cref="SplitterState.Update(SplitterContext, in SplitterInParameter, ref SequenceReader{byte})"/>.</param>
        private void Setup(
            Memory<SplitterState> splitters,
            Memory<SplitterDestinationVersion1> splitterDestinationsV1,
            Memory<SplitterDestinationVersion2> splitterDestinationsV2,
            bool isBugFixed)
        {
            _splitters = splitters;
            _splitterDestinationsV1 = splitterDestinationsV1;
            _splitterDestinationsV2 = splitterDestinationsV2;
            IsBugFixed = isBugFixed;
        }

        // S2Arm (#12/#13 拆分 · 渲染拍不等 Update 的锁): 影子构建——解析段(锁外)在影子上跑 SplitterContext.Update。
        //   各存储来自 S2AUpdateShadow 的原生影子块(链表指针必须指向稳定的影子目的地数组,原生块不随 GC 移动);
        //   标量按 live 现值快照;双二阶滤波状态与 live 共享(游戏段只读,DSP 在锁外独写)
        internal void S2AInitializeShadow(
            SplitterContext live,
            Memory<SplitterState> splitters,
            Memory<SplitterDestinationVersion1> splitterDestinationsV1,
            Memory<SplitterDestinationVersion2> splitterDestinationsV2,
            Memory<BiquadFilterState> splitterBqfStates)
        {
            Version = live.Version;
            IsSplitterPrevVolumeResetSupported = live.IsSplitterPrevVolumeResetSupported;
            IsBiquadFilterParameterFloatSupported = live.IsBiquadFilterParameterFloatSupported;
            _splitterBqfStates = splitterBqfStates;

            Setup(splitters, splitterDestinationsV1, splitterDestinationsV2, live.IsBugFixed);
        }

        // S2Arm (#12/#13 拆分 · 渲染拍不等 Update 的锁): 锁1 快照——live 分频器与目的地整拷进本(影子)上下文,
        //   链头和每个目的地的 _next 都按下标平移到影子数组,目的地另拷一份基底(落账增量判据)。
        //   目的地 _next 必须重绑:拷来的指着 live,影子段 SplitterState.Update 的 ClearLinks 顺链走会走进 live、
        //   在锁外把 live 的链拆掉(渲染拍正在读),落账顺影子链重建时也会穿进 live,连出环(渲染拍在会话锁里
        //   顺链走不出来:声音全停、即时存档拿不到会话锁)
        internal unsafe void S2ASnapshotFrom(SplitterContext live)
        {
            SplitterDestinationVersion1* liveBaseV1 = live.S2ADestinationBaseV1;
            SplitterDestinationVersion2* liveBaseV2 = live.S2ADestinationBaseV2;
            SplitterDestinationVersion1* shadowBaseV1 = S2ADestinationBaseV1;
            SplitterDestinationVersion2* shadowBaseV2 = S2ADestinationBaseV2;

            for (int i = 0; i < _splitters.Length; i++)
            {
                _splitters.Span[i] = live._splitters.Span[i];
                _splitters.Span[i].S2ARebindSnapshotPointers(liveBaseV1, liveBaseV2, shadowBaseV1, shadowBaseV2);
            }

            for (int i = 0; i < _splitterDestinationsV2.Length; i++)
            {
                _splitterDestinationsV2.Span[i] = live._splitterDestinationsV2.Span[i];
                _splitterDestinationsV2.Span[i].S2ARebindSnapshotPointers(liveBaseV2, shadowBaseV2);
                _s2aDestinationBaseV2.Span[i] = live._splitterDestinationsV2.Span[i];
            }

            for (int i = 0; i < _splitterDestinationsV1.Length; i++)
            {
                _splitterDestinationsV1.Span[i] = live._splitterDestinationsV1.Span[i];
                _splitterDestinationsV1.Span[i].S2ARebindSnapshotPointers(liveBaseV1, shadowBaseV1);
                _s2aDestinationBaseV1.Span[i] = live._splitterDestinationsV1.Span[i];
            }
        }

        // S2Arm (#12/#13 拆分 · 渲染拍不等 Update 的锁): 影子分频段→live 的落账。目的地按字段矩阵落账
        //   (S2ACommitGameFieldsFrom,基底=锁1 快照拷贝);链结构(链头、_next)按下标原样平移回 live
        internal void S2ACommitFrom(SplitterContext shadow)
        {
            if (shadow._splitterDestinationsV2.IsEmpty && shadow._splitterDestinationsV1.IsEmpty)
            {
                return;
            }

            if (shadow._splitterDestinationsV2.IsEmpty)
            {
                for (int i = 0; i < shadow._splitterDestinationsV1.Length; i++)
                {
                    _splitterDestinationsV1.Span[i].S2ACommitGameFieldsFrom(
                        in shadow._splitterDestinationsV1.Span[i],
                        in shadow._s2aDestinationBaseV1.Span[i]);
                }
            }
            else
            {
                for (int i = 0; i < shadow._splitterDestinationsV2.Length; i++)
                {
                    _splitterDestinationsV2.Span[i].S2ACommitGameFieldsFrom(
                        in shadow._splitterDestinationsV2.Span[i],
                        in shadow._s2aDestinationBaseV2.Span[i]);
                }
            }

            // 链结构原样照影子:链头与每个目的地的 _next 按下标从影子数组平移回 live(拆分路径 = 原版就地更新的同构拷贝)
            unsafe
            {
                SplitterDestinationVersion1* shadowBaseV1 = shadow.S2ADestinationBaseV1, liveBaseV1 = S2ADestinationBaseV1;
                SplitterDestinationVersion2* shadowBaseV2 = shadow.S2ADestinationBaseV2, liveBaseV2 = S2ADestinationBaseV2;

                for (int i = 0; i < _splitterDestinationsV1.Length; i++)
                {
                    _splitterDestinationsV1.Span[i].S2ACommitLinkFrom(in shadow._splitterDestinationsV1.Span[i], shadowBaseV1, liveBaseV1);
                }

                for (int i = 0; i < _splitterDestinationsV2.Length; i++)
                {
                    _splitterDestinationsV2.Span[i].S2ACommitLinkFrom(in shadow._splitterDestinationsV2.Span[i], shadowBaseV2, liveBaseV2);
                }

                for (int i = 0; i < _splitters.Length; i++)
                {
                    _splitters.Span[i].S2ACommitGameFieldsFrom(in shadow._splitters.Span[i], shadowBaseV1, shadowBaseV2, liveBaseV1, liveBaseV2);
                }
            }
        }

        // S2Arm (#12/#13 拆分): 目的地数组的锁1 基底拷贝(落账的增量判据;影子块内、随影子复用)
        internal Memory<SplitterDestinationVersion1> _s2aDestinationBaseV1;
        internal Memory<SplitterDestinationVersion2> _s2aDestinationBaseV2;
        // S2Arm (#12/#13 拆分): 影子构建/落账要的逐项访问与原生基址(工作缓冲原生块,指针稳定;
        //   快照拷贝的重绑、落账的链头重建都要按下标平移指针)
        internal ref SplitterState S2AGetState(int id) => ref _splitters.Span[id];
        internal ref SplitterDestinationVersion1 S2AGetDestinationV1(int id) => ref _splitterDestinationsV1.Span[id];
        internal ref SplitterDestinationVersion2 S2AGetDestinationV2(int id) => ref _splitterDestinationsV2.Span[id];
        internal unsafe SplitterDestinationVersion1* S2ADestinationBaseV1
        {
            get
            {
                fixed (SplitterDestinationVersion1* p = _splitterDestinationsV1.Span) { return p; }
            }
        }
        internal unsafe SplitterDestinationVersion2* S2ADestinationBaseV2
        {
            get
            {
                fixed (SplitterDestinationVersion2* p = _splitterDestinationsV2.Span) { return p; }
            }
        }

        // S2Arm (#12/#13 拆分): 影子上下文共享 live 的双二阶滤波状态(游戏段只读,DSP 在锁外独写)
        internal Memory<BiquadFilterState> S2ABqfStates => _splitterBqfStates;

        // S2Arm (#12/#13 拆分): 影子构建要的计数(分频器数/目的地数)
        internal int S2ASplitterCount => _splitters.Length;
        internal int S2ADestinationCount => _splitterDestinationsV2.IsEmpty ? _splitterDestinationsV1.Length : _splitterDestinationsV2.Length;

        /// <summary>
        /// Clear the new connection flag.
        /// </summary>
        private void ClearAllNewConnectionFlag()
        {
            foreach (ref SplitterState splitter in _splitters.Span)
            {
                splitter.ClearNewConnectionFlag();
            }
        }

        /// <summary>
        /// Get the destination count using the count of splitter.
        /// </summary>
        /// <returns>The destination count using the count of splitter.</returns>
        public int GetDestinationCountPerStateForCompatibility()
        {
            if (_splitters.IsEmpty)
            {
                return 0;
            }

            int length;

            if (_splitterDestinationsV2.IsEmpty)
            {
                length = _splitterDestinationsV1.Length;
            }
            else
            {
                length = _splitterDestinationsV2.Length;
            }

            return length / _splitters.Length;
        }

        /// <summary>
        /// Update one or multiple <see cref="SplitterState"/> from user parameters.
        /// </summary>
        /// <param name="inputHeader">The splitter header.</param>
        /// <param name="input">The raw data after the splitter header.</param>
        private void UpdateState(in SplitterInParameterHeader inputHeader, ref SequenceReader<byte> input)
        {
            for (int i = 0; i < inputHeader.SplitterCount; i++)
            {
                ref readonly SplitterInParameter parameter = ref input.GetRefOrRefToCopy<SplitterInParameter>(out _);

                Debug.Assert(parameter.IsMagicValid());

                if (parameter.IsMagicValid())
                {
                    if (parameter.Id >= 0 && parameter.Id < _splitters.Length)
                    {
                        ref SplitterState splitter = ref GetState(parameter.Id);

                        splitter.Update(this, in parameter, ref input);
                    }

                    // NOTE: there are 12 bytes of unused/unknown data after the destination IDs array.
                    input.Advance(0xC);
                }
                else
                {
                    input.Rewind(Unsafe.SizeOf<SplitterInParameter>());
                    break;
                }
            }
        }

        /// <summary>
        /// Update one splitter destination data from user parameters.
        /// </summary>
        /// <param name="input">The raw data after the splitter header.</param>
        /// <returns>True if the update was successful, false otherwise</returns>
        private bool UpdateData<T>(ref SequenceReader<byte> input) where T : unmanaged, ISplitterDestinationInParameter
        {
            ref readonly T parameter = ref input.GetRefOrRefToCopy<T>(out _);

            Debug.Assert(parameter.IsMagicValid());

            if (parameter.IsMagicValid())
            {
                int length;

                if (_splitterDestinationsV2.IsEmpty)
                {
                    length = _splitterDestinationsV1.Length;
                }
                else
                {
                    length = _splitterDestinationsV2.Length;
                }
                
                if (parameter.Id >= 0 && parameter.Id < length)
                {
                    SplitterDestination destination = GetDestination(parameter.Id);

                    destination.Update(parameter, IsSplitterPrevVolumeResetSupported);
                }

                return true;
            }
            else
            {
                input.Rewind(Unsafe.SizeOf<T>());

                return false;
            }
        }

        /// <summary>
        /// Update one or multiple splitter destination data from user parameters.
        /// </summary>
        /// <param name="inputHeader">The splitter header.</param>
        /// <param name="input">The raw data after the splitter header.</param>
        private void UpdateData(in SplitterInParameterHeader inputHeader, ref SequenceReader<byte> input)
        {
            for (int i = 0; i < inputHeader.SplitterDestinationCount; i++)
            {
                if (Version == 1)
                {
                    if (!UpdateData<SplitterDestinationInParameterVersion1>(ref input))
                    {
                        break;
                    }
                }
                else if (Version == 2)
                {
                    if (IsBiquadFilterParameterFloatSupported)
                    {
                        if (!UpdateData<SplitterDestinationInParameterVersion2b>(ref input))
                        {
                            break;
                        }
                    }
                    else
                    {
                        if (!UpdateData<SplitterDestinationInParameterVersion2a>(ref input))
                        {
                            break;
                        }
                    }
                }
                else
                {
                    Debug.Fail($"Invalid splitter context version {Version}.");
                }
            }
        }

        /// <summary>
        /// Update splitter from user parameters.
        /// </summary>
        /// <param name="input">The input raw user data.</param>
        /// <returns>Return true if the update was successful.</returns>
        public bool Update(ref SequenceReader<byte> input)
        {
            if (!UsingSplitter())
            {
                return true;
            }

            ref readonly SplitterInParameterHeader header = ref input.GetRefOrRefToCopy<SplitterInParameterHeader>(out _);

            if (header.IsMagicValid())
            {
                ClearAllNewConnectionFlag();

                UpdateState(in header, ref input);
                UpdateData(in header, ref input);

                input.SetConsumed(BitUtils.AlignUp(input.Consumed, 0x10));

                return true;
            }
            else
            {
                input.Rewind(Unsafe.SizeOf<SplitterInParameterHeader>());

                return false;
            }
        }

        /// <summary>
        /// Get a reference to a <see cref="SplitterState"/> at the given <paramref name="id"/>.
        /// </summary>
        /// <param name="id">The index to use.</param>
        /// <returns>A reference to a <see cref="SplitterState"/> at the given <paramref name="id"/>.</returns>
        public ref SplitterState GetState(int id)
        {
            return ref SpanIOHelper.GetFromMemory(_splitters, id, (uint)_splitters.Length);
        }

        /// <summary>
        /// Get a reference to the splitter destination data at the given <paramref name="id"/>.
        /// </summary>
        /// <param name="id">The index to use.</param>
        /// <returns>A reference to the splitter destination data at the given <paramref name="id"/>.</returns>
        public SplitterDestination GetDestination(int id)
        {
            if (_splitterDestinationsV2.IsEmpty)
            {
                return new SplitterDestination(ref SpanIOHelper.GetFromMemory(_splitterDestinationsV1, id, (uint)_splitterDestinationsV1.Length));
            }
                
            return new SplitterDestination(ref SpanIOHelper.GetFromMemory(_splitterDestinationsV2, id, (uint)_splitterDestinationsV2.Length));
        }

        /// <summary>
        /// Get a <see cref="SplitterDestination"/> in the <see cref="SplitterState"/> at <paramref name="id"/> and pass <paramref name="destinationId"/> to <see cref="SplitterState.GetData(int)"/>.
        /// </summary>
        /// <param name="id">The index to use to get the <see cref="SplitterState"/>.</param>
        /// <param name="destinationId">The index of the <see cref="SplitterDestination"/>.</param>
        /// <returns>A <see cref="SplitterDestination"/>.</returns>
        public SplitterDestination GetDestination(int id, int destinationId)
        {
            ref SplitterState splitter = ref GetState(id);

            return splitter.GetData(destinationId);
        }

        /// <summary>
        /// Gets the biquad filter state for a given splitter destination.
        /// </summary>
        /// <param name="destination">The splitter destination.</param>
        /// <returns>Biquad filter state for the specified destination.</returns>
        public Memory<BiquadFilterState> GetBiquadFilterState(SplitterDestination destination)
        {
            return _splitterBqfStates.Slice(destination.Id * BqfStatesPerDestination, BqfStatesPerDestination);
        }

        /// <summary>
        /// Return true if the audio renderer has any splitters.
        /// </summary>
        /// <returns>True if the audio renderer has any splitters.</returns>
        public bool UsingSplitter()
        {
            return !_splitters.IsEmpty && (!_splitterDestinationsV1.IsEmpty || !_splitterDestinationsV2.IsEmpty);
        }

        /// <summary>
        /// Update the internal state of all splitters.
        /// </summary>
        public void UpdateInternalState()
        {
            foreach (ref SplitterState splitter in _splitters.Span)
            {
                splitter.UpdateInternalState();
            }
        }
    }
}
