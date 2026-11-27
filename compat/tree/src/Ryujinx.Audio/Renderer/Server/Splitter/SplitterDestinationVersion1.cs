using Ryujinx.Audio.Renderer.Parameter;
using Ryujinx.Common.Utilities;
using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Ryujinx.Audio.Renderer.Server.Splitter
{
    /// <summary>
    /// Server state for a splitter destination (version 1).
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Size = 0xE0, Pack = Alignment)]
    public struct SplitterDestinationVersion1
    {
        public const int Alignment = 0x10;

        /// <summary>
        /// The unique id of this <see cref="SplitterDestinationVersion1"/>.
        /// </summary>
        public int Id;

        /// <summary>
        /// The mix to output the result of the splitter.
        /// </summary>
        public int DestinationId;

        /// <summary>
        /// Mix buffer volumes storage.
        /// </summary>
        private MixArray _mix;
        private MixArray _previousMix;

        /// <summary>
        /// Pointer to the next linked element.
        /// </summary>
        private unsafe SplitterDestinationVersion1* _next;

        /// <summary>
        /// Set to true if in use.
        /// </summary>
        [MarshalAs(UnmanagedType.I1)]
        public bool IsUsed;

        /// <summary>
        /// Set to true if the internal state need to be updated.
        /// </summary>
        [MarshalAs(UnmanagedType.I1)]
        public bool NeedToUpdateInternalState;

        [StructLayout(LayoutKind.Sequential, Size = sizeof(float) * Constants.MixBufferCountMax, Pack = 1)]
        private struct MixArray { }

        /// <summary>
        /// Mix buffer volumes.
        /// </summary>
        /// <remarks>Used when a splitter id is specified in the mix.</remarks>
        public Span<float> MixBufferVolume => SpanHelpers.AsSpan<MixArray, float>(ref _mix);

        /// <summary>
        /// Previous mix buffer volumes.
        /// </summary>
        /// <remarks>Used when a splitter id is specified in the mix.</remarks>
        public Span<float> PreviousMixBufferVolume => SpanHelpers.AsSpan<MixArray, float>(ref _previousMix);

        /// <summary>
        /// Get the reference of the next element or null if not present.
        /// </summary>
        public readonly ref SplitterDestinationVersion1 Next
        {
            get
            {
                unsafe
                {
                    return ref Unsafe.AsRef<SplitterDestinationVersion1>(_next);
                }
            }
        }

        /// <summary>
        /// Create a new <see cref="SplitterDestinationVersion1"/>.
        /// </summary>
        /// <param name="id">The unique id of this <see cref="SplitterDestinationVersion1"/>.</param>
        public SplitterDestinationVersion1(int id) : this()
        {
            Id = id;
            DestinationId = Constants.UnusedMixId;

            ClearVolumes();
        }

        /// <summary>
        /// Update the <see cref="SplitterDestinationVersion1"/> from user parameter.
        /// </summary>
        /// <param name="parameter">The user parameter.</param>
        /// <param name="isPrevVolumeResetSupported">Indicates that the audio renderer revision in use supports explicitly resetting the volume.</param>
        public void Update<T>(in T parameter, bool isPrevVolumeResetSupported) where T : ISplitterDestinationInParameter
        {
            Debug.Assert(Id == parameter.Id);

            if (parameter.IsMagicValid() && Id == parameter.Id)
            {
                DestinationId = parameter.DestinationId;

                parameter.MixBufferVolume.CopyTo(MixBufferVolume);

                bool resetPrevVolume = isPrevVolumeResetSupported ? parameter.ResetPrevVolume : !IsUsed && parameter.IsUsed;
                if (resetPrevVolume)
                {
                    MixBufferVolume.CopyTo(PreviousMixBufferVolume);

                    NeedToUpdateInternalState = false;
                }

                IsUsed = parameter.IsUsed;
            }
        }

        /// <summary>
        /// Update the internal state of the instance.
        /// </summary>
        public void UpdateInternalState()
        {
            if (IsUsed && NeedToUpdateInternalState)
            {
                MixBufferVolume.CopyTo(PreviousMixBufferVolume);
            }

            NeedToUpdateInternalState = false;
        }

        /// <summary>
        /// Set the update internal state marker.
        /// </summary>
        public void MarkAsNeedToUpdateInternalState()
        {
            NeedToUpdateInternalState = true;
        }

        // S2Arm (#12/#13 拆分 · 渲染拍不等 Update 的锁): 目的地游戏字段落账拷贝(影子→live)。字段矩阵:
        //   游戏每更新都写的(DestinationId/音量/IsUsed)整拷;游戏只在 ResetPrevVolume
        //   路径写的(_previousMix、NeedToUpdateInternalState 清位)按「影子相对锁1 基底(sourceBase)的增量」落——
        //   没写过就保留 live,中拍的标记/内态刷新不抹;拍写的(_isPreviousBiquadFilterEnabled)
        //   不拷。链表指针 _next 不在这里拷,由 S2ACommitLinkFrom 按下标原样平移
        internal void S2ACommitGameFieldsFrom(in SplitterDestinationVersion1 source, in SplitterDestinationVersion1 sourceBase)
        {
            DestinationId = source.DestinationId;

            source.MixBufferVolume.CopyTo(MixBufferVolume);

            IsUsed = source.IsUsed;

            if (source.NeedToUpdateInternalState != sourceBase.NeedToUpdateInternalState)
            {
                NeedToUpdateInternalState = source.NeedToUpdateInternalState;
            }

            if (!source.PreviousMixBufferVolume.SequenceEqual(sourceBase.PreviousMixBufferVolume))
            {
                source.PreviousMixBufferVolume.CopyTo(PreviousMixBufferVolume);
            }
        }

        /// <summary>
        /// Return true if the <see cref="SplitterDestinationVersion1"/> is used and has a destination.
        /// </summary>
        /// <returns>True if the <see cref="SplitterDestinationVersion1"/> is used and has a destination.</returns>
        public readonly bool IsConfigured()
        {
            return IsUsed && DestinationId != Constants.UnusedMixId;
        }

        /// <summary>
        /// Get the volume for a given destination.
        /// </summary>
        /// <param name="destinationIndex">The destination index to use.</param>
        /// <returns>The volume for the given destination.</returns>
        public float GetMixVolume(int destinationIndex)
        {
            Debug.Assert(destinationIndex is >= 0 and < Constants.MixBufferCountMax);

            return MixBufferVolume[destinationIndex];
        }

        /// <summary>
        /// Get the previous volume for a given destination.
        /// </summary>
        /// <param name="destinationIndex">The destination index to use.</param>
        /// <returns>The volume for the given destination.</returns>
        public float GetMixVolumePrev(int destinationIndex)
        {
            Debug.Assert(destinationIndex is >= 0 and < Constants.MixBufferCountMax);

            return PreviousMixBufferVolume[destinationIndex];
        }

        /// <summary>
        /// Clear the volumes.
        /// </summary>
        public void ClearVolumes()
        {
            MixBufferVolume.Clear();
            PreviousMixBufferVolume.Clear();
        }

        /// <summary>
        /// Link the next element to the given <see cref="SplitterDestinationVersion1"/>.
        /// </summary>
        /// <param name="next">The given <see cref="SplitterDestinationVersion1"/> to link.</param>
        public void Link(ref SplitterDestinationVersion1 next)
        {
            unsafe
            {
                fixed (SplitterDestinationVersion1* nextPtr = &next)
                {
                    _next = nextPtr;
                }
            }
        }

        /// <summary>
        /// Remove the link to the next element.
        /// </summary>

        // S2Arm (#12/#13 拆分): 落账时链指针照影子原样——取影子同下标目的地的 _next,从影子数组平移回 live 数组
        internal unsafe void S2ACommitLinkFrom(in SplitterDestinationVersion1 source, SplitterDestinationVersion1* shadowBase, SplitterDestinationVersion1* liveBase)
        {
            _next = source._next;
            S2ARebindSnapshotPointers(shadowBase, liveBase);
        }

        // S2Arm (#12/#13 拆分 · 渲染拍不等 Update 的锁): 锁1 快照拷贝的重绑(理由见 SplitterState.S2ARebindSnapshotPointers:
        //   拷来的 _next 指着 live,影子段 ClearLinks 顺着写会指进 live;按下标平移重绑到影子数组)
        internal unsafe void S2ARebindSnapshotPointers(SplitterDestinationVersion1* liveBase, SplitterDestinationVersion1* shadowBase)
        {
            if (_next != null)
            {
                _next = shadowBase + (_next - liveBase);
            }
        }

        public void Unlink()
        {
            unsafe
            {
                _next = null;
            }
        }
    }
}
