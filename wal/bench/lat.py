# lat.py LOGPREFIX: p50/p90/p99 of a pgbench -l run's transaction latencies, in ms.
import sys, glob
v = []
for f in glob.glob(sys.argv[1] + "*"):
    for line in open(f):
        p = line.split()
        if len(p) >= 3: v.append(int(p[2]) / 1000)
v.sort()
q = lambda x: v[min(len(v) - 1, int(len(v) * x))] if v else 0
print(f"n={len(v)} p50={q(.5):.1f}ms p90={q(.9):.1f}ms p99={q(.99):.1f}ms")
