#!/usr/bin/env python3
# 门禁手动：要私有游戏数据：Ryujinx 真值截图序列（要传 shots 目录 / 游戏 / 检查点 / 局名）
"""从一局 Ryujinx 真值截图序列里按「到达」挑帧，缩到 960×540 存进 <S2A_TRUTH>/<游戏>/<检查点>/ryu_<局>_<秒>.png（自旧库 497552d 迁入，真值根目录见 truth_cmp.py）。

用法：truth_pick.py <shots 目录> <游戏> <检查点> <局名> --after <秒> --offsets 20,40 [--dark 20] [--bright 50]
  截图名 t<秒>.png（compat/truth/run_truth.sh 时间表里的 SHOT:t<秒>，写在 <输出目录>/shots/）。
  到达 = --after 秒之后最后一次「暗帧（平均亮度 < --dark）→ 亮帧（≥ --bright）」跳变的那张亮帧（读盘黑场结束、进入场景）；
  --anchor <秒>：以该秒为到达（不找跳变）。取到达后 --offsets 各秒（取不到恰好那秒就取之后最近一张）。到达点找不到就报红退出，不出真值。
"""
import argparse, glob, os, re, sys
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('shots'); ap.add_argument('game'); ap.add_argument('point'); ap.add_argument('run')
ap.add_argument('--after', type=int, default=0); ap.add_argument('--offsets', default='20'); ap.add_argument('--dark', type=float, default=20); ap.add_argument('--bright', type=float, default=50); ap.add_argument('--bright-max', type=float, default=255)
ap.add_argument('--anchor', type=int, help='不找暗→亮跳变，直接以该秒为到达（如真值局在确定状态下按的复位键时刻）')
a = ap.parse_args()
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import truth_cmp as T   # 定到达的规则与我方序列共用（truth_cmp.arrival）
fs, lum = T.seq_lum(a.shots); ts = sorted(fs)
arr = a.anchor if a.anchor is not None else T.arrival(lum, a.after, a.dark, a.bright, a.bright_max)
if arr is None: sys.exit(f'🔴 {a.shots}：{a.after} 秒后没找到暗→亮跳变（亮度序列 {[round(lum[t]) for t in ts]}）')
d = os.path.join(T.TRUTH, a.game, a.point); os.makedirs(d, exist_ok=True)
for off in map(int, a.offsets.split(',')):
    t = T.pick_after(fs, arr, off)
    if t is None: sys.exit(f'🔴 到达 t{arr} 后 {off} 秒没有截图（最后一张 t{ts[-1]}）')
    out = os.path.join(d, f'ryu_{a.run}_{off}.png')
    Image.open(fs[t]).convert('RGB').resize((960, 540), Image.LANCZOS).save(out)
    print(f'到达 t{arr}（亮度 {lum.get(arr, float("nan")):.0f}）+{off}s → t{t} → {out}')
