"""
THỰC NGHIỆM THUẬT TOÁN LỌC TIẾNG ỒN (mô phỏng trên máy tính)
- Thuật toán lọc giống hệt chương trình ESP32: trung vị 5 mẫu + EMA (alpha = 0.2), lấy mẫu 200 ms
- Tín hiệu mô phỏng giống chế độ SIM_MODE của ESP32: mức nền + dao động ±3 dB + 8 % xung va đập +10..25 dB
- So sánh 4 phương án: không lọc, chỉ EMA, chỉ trung vị, trung vị + EMA
"""
import math, random, statistics, csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

random.seed(2026)
DT = 0.2                     # 200 ms
MEDIAN_N, ALPHA = 5, 0.2
LIMIT, WARN = 85.0, 80.0

# --- Kịch bản 10 phút: mức ồn thật (true) thay đổi theo bậc như các giai đoạn vận hành máy ---
segments = [(60, 72), (120, 88), (60, 95), (90, 82), (60, 70), (120, 87), (90, 78)]  # (giây, dB)
true = []
for dur, lvl in segments:
    true += [lvl] * int(dur / DT)
N = len(true)

def gen_raw(t):
    x = t + random.uniform(-3, 3)
    if random.random() < 0.08:
        x += random.uniform(10, 25)
    return min(max(x, 30), 130)

raw = [gen_raw(t) for t in true]

def median_filter(seq, n=MEDIAN_N):
    out, buf = [], []
    for x in seq:
        buf.append(x)
        if len(buf) > n: buf.pop(0)
        out.append(sorted(buf)[len(buf) // 2])
    return out

def ema_filter(seq, a=ALPHA):
    out, e = [], None
    for x in seq:
        e = x if e is None else a * x + (1 - a) * e
        out.append(e)
    return out

variants = {
    "Không lọc (raw)": raw,
    "Chỉ EMA": ema_filter(raw),
    "Chỉ trung vị": median_filter(raw),
    "Trung vị + EMA (đề xuất)": ema_filter(median_filter(raw)),
}

def leq(vals):
    return 10 * math.log10(sum(10 ** (v / 10) for v in vals) / len(vals))

def metrics(sig):
    err = [s - t for s, t in zip(sig, true)]
    rmse = math.sqrt(sum(e * e for e in err) / N)
    maxe = max(abs(e) for e in err)
    # báo động giả: tín hiệu >= 85 trong khi mức thật < 85
    false_alarm = sum(1 for s, t in zip(sig, true) if s >= LIMIT and t < LIMIT)
    # sai số LAeq theo cửa sổ 10 s (50 mẫu) - đúng như ESP32 gửi lên Firebase
    w = int(10 / DT)
    leq_err = [abs(leq(sig[i:i + w]) - leq(true[i:i + w])) for i in range(0, N, w)]
    zone = lambda L: 2 if L >= LIMIT else (1 if L >= WARN else 0)
    wrong_zone = sum(1 for i in range(0, N, w) if zone(leq(sig[i:i + w])) != zone(leq(true[i:i + w])))
    # độ trễ đáp ứng bậc 72 -> 88 dB (đạt 90 % bước nhảy)
    s0 = int(60 / DT); target = 72 + 0.9 * 16
    lag = next((k for k in range(s0, s0 + 300) if sig[k] >= target), None)
    lag_s = (lag - s0) * DT if lag is not None else float("nan")
    return dict(rmse=rmse, maxe=maxe, false_alarm=false_alarm,
                leq_mae=statistics.mean(leq_err), leq_max=max(leq_err), lag=lag_s, wrong=wrong_zone)

rows = []
for name, sig in variants.items():
    m = metrics(sig)
    rows.append([name, f"{m['rmse']:.2f}", f"{m['maxe']:.1f}", m["false_alarm"],
                 f"{m['false_alarm'] * DT:.1f}", f"{m['leq_mae']:.2f}", f"{m['leq_max']:.2f}", f"{m['lag']:.1f}", m["wrong"]])
    print(f"{name:28s} RMSE={m['rmse']:.2f} dB  MaxErr={m['maxe']:.1f}  FalseAlarm={m['false_alarm']} mẫu "
          f"({m['false_alarm']*DT:.1f}s)  LAeq MAE={m['leq_mae']:.2f}  LAeq max={m['leq_max']:.2f}  Lag={m['lag']:.1f}s  WrongZone={m['wrong']}/60")

with open("filter_results.csv", "w", newline="", encoding="utf-8-sig") as f:
    w = csv.writer(f)
    w.writerow(["Phương án", "RMSE (dB)", "Sai số lớn nhất (dB)", "Số mẫu báo động giả",
                "Thời gian báo động giả (s)", "Sai số LAeq TB (dB)", "Sai số LAeq lớn nhất (dB)", "Độ trễ đáp ứng (s)", "Số cửa sổ 10 s phân loại sai (/60)"])
    w.writerows(rows)
print(f"Tổng số mẫu: {N} ({N*DT/60:.0f} phút); số xung va đập: {sum(1 for r,t in zip(raw,true) if r-t>6)}")

# --- Hình 1: toàn bộ 10 phút ---
tm = [i * DT / 60 for i in range(N)]
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
fig, ax = plt.subplots(figsize=(12, 4.6), dpi=150)
ax.plot(tm, raw, color="#94a3b8", lw=0.6, label="Tín hiệu thô (có nhiễu + xung va đập)")
ax.plot(tm, variants["Chỉ EMA"], color="#f59e0b", lw=1.1, label="Chỉ EMA")
ax.plot(tm, variants["Trung vị + EMA (đề xuất)"], color="#2563eb", lw=1.8, label="Trung vị + EMA (đề xuất)")
ax.plot(tm, true, color="#111827", lw=1.4, ls="--", label="Mức ồn thật")
ax.axhline(LIMIT, color="#dc2626", ls=":", lw=1.2); ax.text(0.1, LIMIT + 0.6, "85 dBA", color="#dc2626")
ax.set_xlabel("Thời gian (phút)"); ax.set_ylabel("Mức âm (dB)"); ax.set_ylim(60, 125)
ax.set_title("Thực nghiệm bộ lọc tiếng ồn: tín hiệu thô và sau lọc (10 phút, lấy mẫu 200 ms)")
ax.legend(loc="upper right", ncol=2, fontsize=9); ax.grid(alpha=.3)
fig.tight_layout(); fig.savefig("filter_overview.png")

# --- Hình 2: phóng to 40 giây quanh bước nhảy 72 -> 88 dB ---
a, b = int(50 / DT), int(90 / DT)
ts = [i * DT for i in range(a, b)]
fig, ax = plt.subplots(figsize=(12, 4.2), dpi=150)
ax.plot(ts, raw[a:b], "o-", color="#94a3b8", ms=2.5, lw=0.7, label="Tín hiệu thô")
ax.plot(ts, variants["Chỉ trung vị"][a:b], color="#16a34a", lw=1.3, label="Chỉ trung vị")
ax.plot(ts, variants["Chỉ EMA"][a:b], color="#f59e0b", lw=1.3, label="Chỉ EMA")
ax.plot(ts, variants["Trung vị + EMA (đề xuất)"][a:b], color="#2563eb", lw=2.2, label="Trung vị + EMA")
ax.plot(ts, true[a:b], color="#111827", ls="--", lw=1.3, label="Mức ồn thật")
ax.axhline(LIMIT, color="#dc2626", ls=":", lw=1.2)
ax.set_xlabel("Thời gian (giây)"); ax.set_ylabel("Mức âm (dB)")
ax.set_title("Phóng to: khả năng loại xung va đập và độ trễ khi mức ồn tăng 72 → 88 dB")
ax.legend(loc="upper left", ncol=3, fontsize=9); ax.grid(alpha=.3)
fig.tight_layout(); fig.savefig("filter_zoom.png")
