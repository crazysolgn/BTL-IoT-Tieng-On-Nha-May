"""Lặp lại thực nghiệm với 20 hạt giống ngẫu nhiên để kết quả không phụ thuộc 1 lần chạy."""
import importlib.util, random, statistics, sys, io, contextlib
import matplotlib; matplotlib.use("Agg")
src = open("filter_experiment.py", encoding="utf-8").read().split("rows = []")[0]
res = {}
for seed in range(1, 21):
    g = {}
    code = src.replace("random.seed(2026)", f"random.seed({seed})")
    exec(code, g)
    for name, sig in g["variants"].items():
        m = g["metrics"](sig)
        res.setdefault(name, []).append(m)
print("Phương án | RMSE TB | Sai số LAeq TB | Báo động giả TB (s) | Cửa sổ sai TB /60 | Trễ TB (s)")
with open("filter_results_20seeds.csv", "w", encoding="utf-8-sig") as f:
    f.write("Phương án,RMSE TB (dB),Sai số LAeq TB (dB),Báo động giả TB (s),Cửa sổ 10s phân loại sai TB (/60),Độ trễ TB (s)\n")
    for name, ms in res.items():
        r = [statistics.mean(m[k] for m in ms) for k in ("rmse", "leq_mae", "false_alarm", "wrong", "lag")]
        r[2] *= 0.2
        print(f"{name:26s} | {r[0]:.2f} | {r[1]:.2f} | {r[2]:.1f} | {r[3]:.1f} | {r[4]:.1f}")
        f.write(f"{name},{r[0]:.2f},{r[1]:.2f},{r[2]:.1f},{r[3]:.1f},{r[4]:.1f}\n")
