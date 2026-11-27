using Ryujinx.HLE.HOS.Services.Hid.Types.SharedMemory.Npad;
using Ryujinx.Memory;
using System;
using System.Numerics;
using System.Threading;

namespace Ryujinx.HLE.HOS.Services.Hid
{
    // S2Arm：Switch 2 的 Npad IMU 通道（hid 320 EnableNpadImu / 321 DisableNpadImu，21.0.0+）。
    // 游戏 SDK 的 nn::hid::InitializeSensorFusion → EnableNpadImu(void* buffer, size_t size) 把一块缓冲做成传输内存交给 hid，
    //   hid 往里写 IMU 样本，SDK 的 GetNpadImuSamples → UpdateSensorFusion → nn::imu 融合出姿态（GetSensorFusionOrientations）。
    // 格式是读 Switch Sports Resort（04008E001F098000）随游戏的 sdk 反汇编得来（Switchbrew 只有命令名）：
    //   每支传感器一格 0x288 字节 = 16 笔 × 0x28 字节环形样本 + 0x280 处 u64 写入位置（下一笔要写的槽，读方 ldar 读，读到等于它为止）。
    //   格子位置：NpadId 0–9 的传感器 0 在 npadId × 0x288，传感器 1 在 0x1950 + npadId × 0x288；掌机（NpadId 0x20）在 0x32a0。
    //   一笔样本：u64 取样序号（逐笔 +1；差 1–99 按掉样本补、倒退或差更多则融合重置）、u64 距上一笔的纳秒数、
    //   f32×3 加速度（米/秒²）、f32×3 角速度（弧度/秒）。SDK 掉样本时每笔按 5 ms 补，即 200 Hz。
    // 宿主没有真的传感器芯片：读数与六轴同源（手柄 / 手机的 NS2 原始坐标读数），每来一笔新读数写一笔。
    public sealed class NpadImu
    {
        public const int SlotSize = 0x288;
        public const int RingLength = 16;
        public const int SampleSize = 0x28;
        public const int WriteIndexOffset = RingLength * SampleSize; // 0x280
        public const int NpadSlotCount = 10;
        public const int SecondSensorBase = NpadSlotCount * SlotSize; // 0x1950
        public const int HandheldBase = 2 * SecondSensorBase;          // 0x32a0
        public const ulong MinimumSize = HandheldBase + SlotSize;      // 0x3528
        public const int HandheldSlot = 2 * NpadSlotCount;
        private const int SlotTotal = HandheldSlot + 1;
        private const ulong DefaultDeltaNs = 5_000_000;
        private const float StandardGravity = 9.80665f;
        private const float DegreesToRadians = MathF.PI / 180f;

        private readonly object _lock = new();
        private IVirtualMemoryManager _memory;
        private ulong _address;
        private long _appletResourceUserId;
        private readonly ulong[] _samplingNumber = new ulong[SlotTotal];
        private readonly ulong[] _writeIndex = new ulong[SlotTotal];
        private readonly ulong[] _lastTimestampUs = new ulong[SlotTotal];
        private const ulong ClockJumpUs = 1_000_000;   // 时间戳往回跳这么多（微秒）以上 = 换了时钟，不当重推（见 Write）

        public bool Active { get { lock (_lock) return _memory != null; } }

        public void Enable(IVirtualMemoryManager memory, ulong address, long appletResourceUserId)
        {
            lock (_lock)
            {
                _memory = memory;
                _address = address;
                _appletResourceUserId = appletResourceUserId;
                Array.Clear(_samplingNumber);
                Array.Clear(_writeIndex);
                Array.Clear(_lastTimestampUs);
                // 真机 hid 接手时缓冲里没有样本：读方游标从 0 起，写入位置也得是 0
                Span<byte> zero = stackalloc byte[SlotSize];
                for (int slot = 0; slot < SlotTotal; slot++)
                {
                    memory.Write(SlotAddress(slot), zero);
                }
            }
        }

        public bool Disable(long appletResourceUserId)
        {
            lock (_lock)
            {
                if (_memory == null || appletResourceUserId != _appletResourceUserId)
                {
                    return false;
                }

                _memory = null;
                _address = 0;

                return true;
            }
        }

        // accelerationG：加速度（g），angularVelocityDegrees：角速度（度/秒），都是 NS2 原始坐标；timestampUs：读数的单调时间戳（微秒）
        // sensor：双 Joy-Con 左右各有读数时 0 = 左只、1 = 右只各写各的；-1 = 单一体感源（双 Joy-Con 时左右写同一份，同改前）
        // style：这一格的样式（不含 Joy-Con 2 位）。Joy-Con 2 原始坐标走六轴（SixAxis）是对的，写进 IMU 要先换口径（只动 IMU 这一路）：
        //   - Joy-Con（单只 / 一对，左右两只同一口径）：加速度三轴取反、角速度不动。原始坐标是右手系（x 右、y 前、z 上，陀螺绕同一组轴、右手定则），
        //     加速度是比力（平放 z = +1 g）；IMU 样本要的是重力方向（平放 z = −1 g，SDK 的融合读样本时再取反成比力），角速度同原始
        //   - 改前单只按 Y 轴镜像、一对绕 X 轴转 180°：两者差一个镜射，一个项目对、另一个就反（实机：一对拿其中一只单手挥拍时左右反，
        //     双手立握的项目左右闪躲反，只能二选一）。换成上面这个后两边都对：左手 / 右手单手挥拍，双手立握的立起检查与左右闪躲
        //   - 单只右 Joy-Con 写右只那支传感器（传感器 1），同真机：游戏按样式读右只那格（改前一律写传感器 0，拿右只的玩家体感校准永远等不到读数）
        internal void Push(PlayerIndex player, NpadStyleTag style, Vector3 accelerationG, Vector3 angularVelocityDegrees, ulong timestampUs, int sensor = -1)
        {
            bool joyDual = style == NpadStyleTag.JoyDual;
            if (joyDual || style is NpadStyleTag.JoyLeft or NpadStyleTag.JoyRight)
            {
                accelerationG = -accelerationG;
            }

            lock (_lock)
            {
                if (_memory == null)
                {
                    return;
                }

                if (player == PlayerIndex.Handheld)
                {
                    if (sensor != 1) Write(HandheldSlot, accelerationG, angularVelocityDegrees, timestampUs);
                }
                else if (player >= PlayerIndex.Player1 && player <= PlayerIndex.Player8)
                {
                    // 双 Joy-Con：左右各一支传感器（传感器 0 / 1）。左右各有读数时各写各的；单一体感源时左右同一份读数（同 NpadDevices.UpdateSixAxis 的 #55 做法）。
                    //   左右同一份在手柄当车把一起倾斜时会被游戏当成两只反向转（左右两只坐标相反）、互相抵消
                    if (style == NpadStyleTag.JoyRight)   // 单只右 Joy-Con：只有一路，写右只那支传感器
                    {
                        if (sensor != 1) Write(NpadSlotCount + (int)player, accelerationG, angularVelocityDegrees, timestampUs);
                        return;
                    }

                    if (sensor != 1)   // 不是双 Joy-Con 样式时只认第一路（左只 / 单源），右只那份不写，免得一格里交错两只的读数
                    {
                        Write((int)player, accelerationG, angularVelocityDegrees, timestampUs);
                    }

                    if (joyDual && sensor != 0)
                    {
                        Write(NpadSlotCount + (int)player, accelerationG, angularVelocityDegrees, timestampUs);
                    }
                }
            }
        }

        public static ulong SlotOffset(int slot) => slot == HandheldSlot ? HandheldBase : (ulong)slot * SlotSize;

        private ulong SlotAddress(int slot) => _address + SlotOffset(slot);

        private void Write(int slot, Vector3 accelerationG, Vector3 angularVelocityDegrees, ulong timestampUs)
        {
            ulong last = _lastTimestampUs[slot];
            // 同一笔读数不重写（时间戳没往前）：服务层双 Joy-Con 每拍都推左右两路、不论有没有新样本，改前每次都当成隔 5 ms 的新样本写，
            //   游戏的融合把同一份角速度积分好几遍，手一动就放大成好几倍（实机：一人拿一对的项目没挥也自己出拳）
            //   往回跳 1 秒以上不算重推，是换了时钟（这一格换了体感源，比如转成局域网另一台机器转发的读数）：照新样本写，否则这一格的体感从此一直写不进去
            if (last != 0 && timestampUs <= last && last - timestampUs < ClockJumpUs)
            {
                return;
            }

            ulong deltaNs = last != 0 && timestampUs > last ? (timestampUs - last) * 1000 : DefaultDeltaNs;
            _lastTimestampUs[slot] = timestampUs;

            ulong index = _writeIndex[slot];
            ulong sample = SlotAddress(slot) + index * SampleSize;

            Span<byte> data = stackalloc byte[SampleSize];
            BitConverter.TryWriteBytes(data[0x00..], ++_samplingNumber[slot]);
            BitConverter.TryWriteBytes(data[0x08..], deltaNs);
            BitConverter.TryWriteBytes(data[0x10..], accelerationG.X * StandardGravity);
            BitConverter.TryWriteBytes(data[0x14..], accelerationG.Y * StandardGravity);
            BitConverter.TryWriteBytes(data[0x18..], accelerationG.Z * StandardGravity);
            BitConverter.TryWriteBytes(data[0x1c..], angularVelocityDegrees.X * DegreesToRadians);
            BitConverter.TryWriteBytes(data[0x20..], angularVelocityDegrees.Y * DegreesToRadians);
            BitConverter.TryWriteBytes(data[0x24..], angularVelocityDegrees.Z * DegreesToRadians);
            _memory.Write(sample, data);

            // 先写样本、再推进写入位置（读方 ldar 读写入位置后才读样本）
            index = (index + 1) % RingLength;
            _writeIndex[slot] = index;
            Thread.MemoryBarrier();
            _memory.Write(SlotAddress(slot) + WriteIndexOffset, index);
        }
    }
}
