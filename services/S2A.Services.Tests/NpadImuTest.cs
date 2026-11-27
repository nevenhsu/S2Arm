using Ryujinx.HLE.HOS.Services.Hid;
using System;
using System.Numerics;
using System.Text;

namespace S2A.Services.Tests
{
    // hid 320 EnableNpadImu / 321 DisableNpadImu（Switch 2 SDK 的 Npad IMU 缓冲，格式见 compat/tree/.../Hid/HidDevices/NpadImu.cs）。
    //   期望手写，按 Switch Sports Resort 随游戏 sdk 的 GetNpadImuSamples / UpdateSensorFusion 读法推出：
    //   ① 320 交 0x4000 字节传输内存（缓冲先填 0xEE）→ 结果 0、Imu 在用、各格写入位置清 0
    //   ② Player1 Pro 手柄喂两笔（时间戳 1000 / 6000 µs，加速度 (0, 0, 1) g、角速度 (90, 0, 0) 度/秒）→ 格 0 的样本 0 / 1：
    //      序号 1 / 2、纳秒差 5,000,000 / 5,000,000（第一笔按 200 Hz 补）、加速度 Z 9.80665 米/秒²、角速度 X π/2 弧度/秒；写入位置 2
    //   ③ 改双 Joy-Con：传感器 1 的格（0x1950）也写；掌机写 0x32a0
    //   ④ 再喂 14 笔 → 格 0 写入位置 (3 + 14) mod 16 = 1，序号接着涨到 17
    //   ⑤ 缓冲太小（0x1000 < 0x3528）→ InvalidBufferSize（模块 202、描述 131：131 << 9 | 202 = 0x106CA）、报红 +1
    //   ⑥ 321 → Imu 不在用；之后喂数不再写（格 0 写入位置仍 1）
    //   ⑧ 玩家 1 左只、玩家 2 右只：左只写传感器 0，右只写传感器 1
    //   ⑩ 时间戳没往前的读数不重写（双 Joy-Con 每拍重推同一份时，游戏不会把同一份角速度积分好几遍）
    //   ⑨ Joy-Con（单只 / 一对⑦）写 IMU 前加速度三轴取反（原始是比力，IMU 要重力方向）、角速度不动；Pro 不动（②）
    public static unsafe partial class Program
    {
        [SoloTest("--npad-imu", Booted = true)]
        private static void NpadImuChannel()
        {
            byte* port = _game;
            Encoding.ASCII.GetBytes("sm:\0").CopyTo(new Span<byte>(port, 4));
            uint sm = (uint)Svc(0x1F, 0, 0, (ulong)port).Regs[1];
            Msg init = Msg.Cmif(0, Bytes(0L));
            init.SendPid = true;
            Call(sm, init);
            Reply gh = Call(sm, Msg.Cmif(1, Name8("hid")));
            uint hid = gh.Move.Count == 1 ? gh.Move[0] : 0;
            NpadDevices npads = Host.Device.Hid.Npads;

            const ulong Size = 0x4000;
            byte* buf = _game + 0xA000;
            new Span<byte>(buf, (int)Size).Fill(0xEE);

            Msg Enable(ulong size)
            {
                uint tm = FakeNative.TmemCreate((ulong)buf, size, 1);
                Msg m = Msg.Cmif(320, Bytes(1L, size));
                m.SendPid = true;
                m.Copy.Add(tm);
                return m;
            }

            ulong U64(ulong off) => *(ulong*)(buf + off);
            float F32(ulong off) => *(float*)(buf + off);
            ulong Wi(ulong slot) => U64(slot + 0x280);

            // ①
            Reply en = Call(hid, Enable(Size));
            Check("NpadImu ① 320 交 0x4000 字节传输内存", "结果 0、在用、格 0 / 传感器 1 格 / 掌机格写入位置 0 / 0 / 0",
                $"结果 {en.Result}、{(npads.Imu.Active ? "在用" : "不在用")}、格 0 / 传感器 1 格 / 掌机格写入位置 {Wi(0)} / {Wi(0x1950)} / {Wi(0x32a0)}");

            // ②
            npads.Configure(new ControllerConfig { Player = PlayerIndex.Player1, Type = ControllerType.ProController });
            npads.Update(new System.Collections.Generic.List<GamepadInput> { new() { PlayerId = PlayerIndex.Player1 } });
            Vector3 a = new(0, 0, 1), g = new(90, 0, 0);
            npads.PushImu(PlayerIndex.Player1, a, g, 1000);
            npads.PushImu(PlayerIndex.Player1, a, g, 6000);
            Check("NpadImu ② Pro 手柄两笔：序号、纳秒差", "1 / 2、5000000 / 5000000", $"{U64(0x00)} / {U64(0x28)}、{U64(0x08)} / {U64(0x30)}");
            Check("NpadImu ② 加速度 Z（米/秒²）、角速度 X（弧度/秒）、写入位置", "9.80665、1.5707964、2", $"{F32(0x18)}、{F32(0x1c)}、{Wi(0)}");
            Check("NpadImu ② Pro 手柄不写传感器 1 的格", "写入位置 0", $"写入位置 {Wi(0x1950)}");

            // ③
            npads.Configure(new ControllerConfig { Player = PlayerIndex.Player1, Type = ControllerType.JoyconPair });
            npads.Update(new System.Collections.Generic.List<GamepadInput> { new() { PlayerId = PlayerIndex.Player1 } });
            npads.PushImu(PlayerIndex.Player1, a, g, 11000);
            npads.PushImu(PlayerIndex.Handheld, a, g, 11000);
            Check("NpadImu ③ 双 Joy-Con 写传感器 0 / 1 两格、掌机写 0x32a0", "格 0 写入位置 3、传感器 1 格写入位置 1 序号 1、掌机格写入位置 1 加速度 Z 9.80665",
                $"格 0 写入位置 {Wi(0)}、传感器 1 格写入位置 {Wi(0x1950)} 序号 {U64(0x1950)}、掌机格写入位置 {Wi(0x32a0)} 加速度 Z {F32(0x32a0 + 0x18)}");

            // ④
            for (ulong i = 0; i < 14; i++) npads.PushImu(PlayerIndex.Player1, a, g, 16000 + i * 5000);
            Check("NpadImu ④ 绕圈：格 0 写入位置、槽 0 序号", "1、17", $"{Wi(0)}、{U64(0x00)}");

            // ⑤
            long red0 = Red.Total;
            Reply small = Call(hid, Enable(0x1000));
            Check("NpadImu ⑤ 缓冲 0x1000 太小", "结果 0x106CA、报红 +1", $"结果 0x{small.Result:X}、报红 +{Red.Total - red0}");

            // ⑥
            Msg dis = Msg.Cmif(321, Bytes(1L));
            dis.SendPid = true;
            Reply dr = Call(hid, dis);
            npads.PushImu(PlayerIndex.Player1, a, g, 200000);
            Check("NpadImu ⑥ 321 后不在用、不再写", "结果 0、不在用、格 0 写入位置 1",
                $"结果 {dr.Result}、{(npads.Imu.Active ? "在用" : "不在用")}、格 0 写入位置 {Wi(0)}");

            // ⑦ 双 Joy-Con 左右各有读数（Joy-Con 2 拼对，ns2_relay.h motion_left）：左只只写传感器 0、右只只写传感器 1。
            //   当车把左右一起倾斜时两只的角速度 X 相反：原先单源左右同一份，游戏读到的两只就不再相反
            Reply en2 = Call(hid, Enable(Size));
            npads.Configure(new ControllerConfig { Player = PlayerIndex.Player1, Type = ControllerType.JoyconPair });
            npads.Update(new System.Collections.Generic.List<GamepadInput> { new() { PlayerId = PlayerIndex.Player1 } });
            npads.PushImu(PlayerIndex.Player1, a, new Vector3(90, 0, 0), 300000, 0);
            npads.PushImu(PlayerIndex.Player1, a, new Vector3(-90, 0, 0), 300000, 1);
            Check("NpadImu ⑦ 双 Joy-Con 左右各写各的（加速度取反、角速度不动）：格 0 / 传感器 1 格写入位置、角速度 X、加速度 Z",
                "结果 0、1 / 1、1.5707964 / -1.5707964、-9.80665",
                $"结果 {en2.Result}、{Wi(0)} / {Wi(0x1950)}、{F32(0x1c)} / {F32(0x1950 + 0x1c)}、{F32(0x18)}");
            npads.Configure(new ControllerConfig { Player = PlayerIndex.Player1, Type = ControllerType.ProController });
            npads.Update(new System.Collections.Generic.List<GamepadInput> { new() { PlayerId = PlayerIndex.Player1 } });
            npads.PushImu(PlayerIndex.Player1, a, new Vector3(-90, 0, 0), 305000, 1);
            Check("NpadImu ⑦ 不是双 Joy-Con 样式时右只那路不写（不在一格里交错两只）", "格 0 写入位置 1、传感器 1 格写入位置 1", $"格 0 写入位置 {Wi(0)}、传感器 1 格写入位置 {Wi(0x1950)}");

            // ⑧ 两人各拿一只（玩家 1 左只、玩家 2 右只，单一体感源）：左只写自己那格的传感器 0，右只写传感器 1（同真机，游戏按样式读右只那格）；
            //   改前右只也写传感器 0，传感器 1 一直空着，拿右只的玩家体感校准等不到读数
            Reply en3 = Call(hid, Enable(Size));
            npads.Configure(new ControllerConfig { Player = PlayerIndex.Player1, Type = ControllerType.JoyconLeft },
                            new ControllerConfig { Player = PlayerIndex.Player2, Type = ControllerType.JoyconRight });
            npads.Update(new System.Collections.Generic.List<GamepadInput> { new() { PlayerId = PlayerIndex.Player1 }, new() { PlayerId = PlayerIndex.Player2 } });
            npads.PushImu(PlayerIndex.Player1, a, new Vector3(90, 0, 0), 400000);
            npads.PushImu(PlayerIndex.Player2, a, new Vector3(-90, 0, 0), 400000);
            Check("NpadImu ⑧ 左只 / 右只各一人：玩家 1 传感器 0 / 1、玩家 2 传感器 0 / 1 写入位置，玩家 2 传感器 1 角速度 X",
                "结果 0、1 / 0、0 / 1、-1.5707964",
                $"结果 {en3.Result}、{Wi(0)} / {Wi(0x1950)}、{Wi(0x288)} / {Wi(0x1950 + 0x288)}、{F32(0x1950 + 0x288 + 0x1c)}");
            // ⑨ 单只样式：加速度 (0.5, 0.25, 1) g、角速度 (10, 20, 30) 度/秒 → 加速度三轴取反、角速度不动
            npads.PushImu(PlayerIndex.Player1, new Vector3(0.5f, 0.25f, 1f), new Vector3(10, 20, 30), 405000);
            const ulong s9 = 1 * 0x28;   // 玩家 1 传感器 0 的第 2 笔
            Check("NpadImu ⑨ 单只样式加速度取反、角速度不动：加速度 X / Y / Z、角速度 X / Y / Z",
                "-4.903325 / -2.4516625 / -9.80665、0.17453292 / 0.34906584 / 0.5235988",   // −0.5 / −0.25 / −1 g × 9.80665；10 / 20 / 30 度 × π/180（float）
                $"{F32(s9 + 0x10)} / {F32(s9 + 0x14)} / {F32(s9 + 0x18)}、{F32(s9 + 0x1c)} / {F32(s9 + 0x20)} / {F32(s9 + 0x24)}");
            // ⑩ 同一笔读数（时间戳没往前）不重写：同格同时间戳再推一次，写入位置不动
            npads.PushImu(PlayerIndex.Player1, new Vector3(0.5f, 0.25f, 1f), new Vector3(10, 20, 30), 405000);
            npads.PushImu(PlayerIndex.Player1, new Vector3(0.5f, 0.25f, 1f), new Vector3(10, 20, 30), 404000);
            Check("NpadImu ⑩ 时间戳相同 / 倒退的读数不重写：玩家 1 传感器 0 写入位置", "2", $"{Wi(0)}");
            // ⑪ 往回跳 1 秒以上 = 换了时钟（这一格换了体感源）：照新样本写。先推到 3 秒（写入位置 3），再推 1 秒（往回 2 秒）→ 写入位置 4
            npads.PushImu(PlayerIndex.Player1, new Vector3(0.5f, 0.25f, 1f), new Vector3(10, 20, 30), 3000000);
            npads.PushImu(PlayerIndex.Player1, new Vector3(0.5f, 0.25f, 1f), new Vector3(10, 20, 30), 1000000);
            Check("NpadImu ⑪ 时间戳往回跳 1 秒以上（换了时钟）照写：玩家 1 传感器 0 写入位置", "4", $"{Wi(0)}");
            npads.Configure();
        }
    }
}
