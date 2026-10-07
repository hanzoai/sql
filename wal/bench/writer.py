# writer.py PORT THREADS OUT: insert rows until the server goes away; every id
# whose commit returned is written to OUT.
import sys, threading, psycopg2
port, n, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
acked, lock = [], threading.Lock()
def run():
    try:
        c = psycopg2.connect(host="/tmp", port=port, dbname="postgres", user="postgres")
        cur = c.cursor()
        while True:
            cur.execute("insert into acked default values returning id")
            i = cur.fetchone()[0]
            c.commit()
            with lock: acked.append(i)
    except Exception:
        return
ts = [threading.Thread(target=run) for _ in range(n)]
[t.start() for t in ts]; [t.join() for t in ts]
open(out, "w").write("\n".join(map(str, acked)) + "\n")
print(f"writer: {len(acked)} acknowledged, max {max(acked) if acked else 0}")
