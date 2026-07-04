import asyncio, asyncpg, json
async def main():
    dsn="postgresql://u:p@127.0.0.1:55432/apgpool"  # DEFAULT statement cache
    # 1) reconnect to SAME tenant twice sequentially (the collision trigger)
    for i in (1,2):
        c=await asyncpg.connect(dsn, ssl=False)
        await c.execute("CREATE TABLE IF NOT EXISTS t(id serial primary key, name text, meta jsonb)")
        await c.execute("INSERT INTO t(name,meta) VALUES($1,$2::jsonb)", f"conn{i}", json.dumps({"i":i}))
        print(f"reconnect {i}: rows now", await c.fetchval("SELECT count(*) FROM t"))
        await c.close()
    # 2) POOL of 5 concurrent connections (the real service scenario, e.g. bootnode)
    pool=await asyncpg.create_pool(dsn, ssl=False, min_size=5, max_size=5)
    async def worker(k):
        async with pool.acquire() as c:
            for _ in range(3):
                await c.fetchval("SELECT $1::int + 1", k)  # same SQL -> cached named stmt per conn
            return await c.fetchval("SELECT count(*) FROM t")
    res=await asyncio.gather(*[worker(k) for k in range(5)])
    print("pool workers ok, counts:", res)
    await pool.close()
    print("ASYNCPG POOL+RECONNECT OK")
asyncio.run(main())
