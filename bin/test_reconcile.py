"""Tests of bin/reconcile against a throwaway Patroni cluster: a leader and a standby.

Needs PostgreSQL server binaries (pg_config on PATH, else the newest
/usr/lib/postgresql/*/bin), `patroni` with its raft extra on PATH, and the Python
that runs them with psycopg2 and PyYAML:

    python3 bin/test_reconcile.py

Nothing runs it in CI: the runners carry neither Patroni nor a server, and the
image has no raft extra. It is run by hand against the PostgreSQL major the
image ships (18).
"""
import argparse
import contextlib
import glob
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

import psycopg2
import psycopg2.extras
import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
RECONCILE = os.path.join(HERE, "reconcile")
spec = importlib.util.spec_from_loader("reconcile", importlib.machinery.SourceFileLoader("reconcile", RECONCILE))
reconcile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reconcile)

REST = ("patroni", "restpw")
REPLICATION = ("replicator", "replpw")
PINS = {}  # declared file -> the SHA-256 its pin names, as a pod spec would hold it
HBA = [
    "local all all trust",
    "host replication replicator 127.0.0.1/32 scram-sha-256",
    "host all all 127.0.0.1/32 scram-sha-256",
]


def free_ports(count):
    held = [socket.socket() for _ in range(count)]
    for s in held:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in held]
    for s in held:
        s.close()
    return ports


def bindir():
    found = shutil.which("pg_config")
    if found:
        return subprocess.check_output([found, "--bindir"], text=True).strip()
    versions = sorted(glob.glob("/usr/lib/postgresql/*/bin"), key=lambda p: int(p.split("/")[-2]))
    if not versions:
        raise RuntimeError("no PostgreSQL server binaries: put pg_config on PATH")
    return versions[-1]


class Member:
    def __init__(self, root, name, pg, rest, raft, partner):
        self.name, self.pg, self.rest, self.raft, self.partner = name, pg, rest, raft, partner
        self.dir = os.path.join(root, name)
        self.sock = os.path.join(self.dir, "sock")
        self.url = "http://127.0.0.1:%d" % rest
        self.process = None

    def config(self, dcs):
        return {
            "scope": "test", "name": self.name, "log": {"level": "INFO"},
            "raft": {"data_dir": os.path.join(self.dir, "raft"), "self_addr": "127.0.0.1:%d" % self.raft,
                     "partner_addrs": ["127.0.0.1:%d" % self.partner]},
            "restapi": {"listen": "127.0.0.1:%d" % self.rest, "connect_address": "127.0.0.1:%d" % self.rest,
                        "authentication": {"username": REST[0], "password": REST[1]}},
            "bootstrap": {"dcs": dcs, "initdb": [{"encoding": "UTF8"}, "data-checksums"]},
            "postgresql": {
                "listen": "127.0.0.1:%d" % self.pg, "connect_address": "127.0.0.1:%d" % self.pg,
                "data_dir": os.path.join(self.dir, "data"), "bin_dir": bindir(),
                "pgpass": os.path.join(self.dir, "pgpass"),
                "authentication": {"superuser": {"username": "hanzo", "password": "superpw"},
                                   "replication": {"username": REPLICATION[0], "password": REPLICATION[1]}},
                "parameters": {"unix_socket_directories": self.sock},
            },
        }

    def start(self, dcs):
        os.makedirs(self.sock)
        path = os.path.join(self.dir, "patroni.yml")
        with open(path, "w") as file:
            yaml.safe_dump(self.config(dcs), file)
        self.log = open(os.path.join(self.dir, "patroni.log"), "w")
        self.process = subprocess.Popen(["patroni", path], stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True)

    def stop(self):
        if self.process and self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(30)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
        if self.process:
            self.log.close()
        data = os.path.join(self.dir, "data")
        if os.path.exists(os.path.join(data, "postmaster.pid")):
            subprocess.run([os.path.join(bindir(), "pg_ctl"), "-D", data, "-m", "immediate", "stop"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def ready(self, path):
        try:
            return urllib.request.urlopen(self.url + path, timeout=2).status == 200
        except OSError:
            return False

    def connect(self, user="hanzo", database="postgres"):
        conn = psycopg2.connect(host=self.sock, port=self.pg, user=user, dbname=database, connect_timeout=5)
        conn.autocommit = True
        return conn

    def rows(self, query, args=None, user="hanzo", database="postgres"):
        conn = self.connect(user, database)
        try:
            with conn.cursor() as cur:
                cur.execute(query, args)
                return cur.fetchall() if cur.description else []
        finally:
            conn.close()

    def config_get(self):
        return json.load(urllib.request.urlopen(self.url + "/config", timeout=5))

    def config_patch(self, body):
        request = urllib.request.Request(self.url + "/config", method="PATCH", data=json.dumps(body).encode(),
                                         headers={"Content-Type": "application/json",
                                                  "Authorization": "Basic cGF0cm9uaTpyZXN0cHc="})
        return json.load(urllib.request.urlopen(request, timeout=10))


SNAPSHOT = (
    "SELECT r.rolname, r.rolcanlogin, r.rolsuper, r.rolreplication, r.rolbypassrls, r.rolcreatedb, r.rolcreaterole,"
    " r.rolconfig, a.rolpassword IS NULL FROM pg_roles r JOIN pg_authid a ON a.oid = r.oid ORDER BY 1",
    "SELECT g.rolname, m.rolname, a.admin_option FROM pg_auth_members a JOIN pg_roles g ON g.oid = a.roleid"
    " JOIN pg_roles m ON m.oid = a.member ORDER BY 1, 2",
    "SELECT datname, pg_get_userbyid(datdba), datacl::text FROM pg_database ORDER BY 1",
    "SELECT classoid::regclass::text, objoid, description FROM pg_shdescription ORDER BY 1, 2",
)


def start_cluster():
    """Bring up two members, whichever wins the lease; (root, leader, standby, DCS at start)."""
    root = tempfile.mkdtemp(prefix="q2.")
    ports = free_ports(6)
    a = Member(root, "a", ports[0], ports[1], ports[2], ports[3])
    b = Member(root, "b", ports[4], ports[5], ports[3], ports[2])
    dcs = {"ttl": 30, "loop_wait": 5, "retry_timeout": 10, "maximum_lag_on_failover": 1048576,
           "postgresql": {"use_pg_rewind": False, "use_slots": True, "pg_hba": HBA,
                          "parameters": {"max_connections": 100, "wal_level": "replica", "hot_standby": "on",
                                         "max_wal_senders": 10, "max_replication_slots": 10}}}

    def stop():
        for member in (b, a):
            member.stop()
        shutil.rmtree(root, ignore_errors=True)

    try:
        a.start(dcs)
        b.start(dcs)
        deadline = time.monotonic() + 120
        while not (a.ready("/primary") and b.ready("/replica") or b.ready("/primary") and a.ready("/replica")):
            if time.monotonic() > deadline:
                raise RuntimeError("the cluster did not come up:\n" + open(a.log.name).read()[-2000:]
                                   + open(b.log.name).read()[-2000:])
            time.sleep(1)
        leader, standby = (a, b) if a.ready("/primary") else (b, a)
        base = leader.config_get()
    except BaseException:
        stop()
        raise
    unittest.addModuleCleanup(stop)
    return root, leader, standby, base


CLUSTER = []


class Cluster(unittest.TestCase):
    """The two-member Patroni cluster the tests of this module share."""

    @classmethod
    def setUpClass(cls):
        if not CLUSTER:
            CLUSTER.extend(start_cluster())
        cls.root, cls.a, cls.b, cls.base = CLUSTER
        cls.count = 0

    def declare(self, doc):
        """Write a declared file and record its pin; the path."""
        type(self).count += 1
        path = os.path.join(self.root, "declared-%s-%d.yml" % (type(self).__name__, type(self).count))
        raw = yaml.safe_dump(doc).encode()
        with open(path, "wb") as file:
            file.write(raw)
        PINS[path] = hashlib.sha256(raw).hexdigest()
        return path

    def argv(self, path, member, *more, pin=None):
        return [sys.executable, RECONCILE, *more, "--declared", path, "--sha256", pin or PINS[path],
                "--socket", member.sock, "--port", str(member.pg), "--patroni", member.url]

    @staticmethod
    def env():
        return dict(os.environ, SQL_USER="hanzo", SQL_REPLICATION_USER=REPLICATION[0],
                    PATRONI_RESTAPI_USERNAME=REST[0], PATRONI_RESTAPI_PASSWORD=REST[1])

    def run_reconcile(self, path, member=None):
        """One pass as the sidecar runs it: (exit status, log lines)."""
        member = member or self.a
        done = subprocess.run(self.argv(path, member, "--once"), capture_output=True, text=True, env=self.env(),
                              timeout=180)
        self.assertEqual(done.stderr, "")
        return done.returncode, done.stdout.splitlines()

    def snapshot(self):
        return [self.a.rows(query) for query in SNAPSHOT] + [self.a.config_get()]

    def settle(self, want):
        """Patroni shows a PATCHed config within a loop: wait until /config is want."""
        deadline = time.monotonic() + 60
        while reconcile.diff(self.a.config_get(), want):
            self.assertLess(time.monotonic(), deadline, "/config never equalled the config patched in")
            time.sleep(1)

    def restore_dcs(self):
        deadline = time.monotonic() + 60
        patch = reconcile.diff(self.a.config_get(), self.base)
        while patch and time.monotonic() < deadline:
            self.a.config_patch(patch)
            time.sleep(6)
            patch = reconcile.diff(self.a.config_get(), self.base)

    @staticmethod
    def writes(lines):
        return [line for line in lines if " sql write " in line or " patroni PATCH " in line]

    def denied(self, user, database):
        with self.assertRaises(psycopg2.OperationalError) as caught:
            self.a.connect(user, database).close()
        self.assertIn("permission denied for database", str(caught.exception))


class Roles(Cluster):
    def test_roles_are_narrow_synchronous_and_enrolled(self):
        path = self.declare({"roles": {
            "r_oauth": {"oauth": True},
            "r_maker": {"oauth": True, "createdb": True, "createrole": True, "enroll": True},
            "r_scram": {},
        }})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        rows = {r[0]: r for r in self.a.rows(
            "SELECT r.rolname, r.rolcanlogin, r.rolsuper, r.rolreplication, r.rolbypassrls, r.rolcreatedb,"
            " r.rolcreaterole, a.rolpassword IS NULL, r.rolconfig FROM pg_roles r JOIN pg_authid a ON a.oid = r.oid"
            " WHERE r.rolname LIKE 'r\\_%'")}
        self.assertEqual(set(rows), {"r_oauth", "r_maker", "r_scram"})
        for name, row in rows.items():
            self.assertEqual(row[1:3], (True, False), name)            # LOGIN, NOSUPERUSER
            self.assertEqual(row[3:5], (False, False), name)           # NOREPLICATION, NOBYPASSRLS
            self.assertTrue(row[7], name)                              # no password
            self.assertIn("synchronous_commit=on", row[8], name)
        self.assertEqual(rows["r_oauth"][5:7], (False, False))
        self.assertEqual(rows["r_scram"][5:7], (False, False))
        self.assertEqual(rows["r_maker"][5:7], (True, True))
        members = dict(self.a.rows(
            "SELECT m.rolname, a.admin_option FROM pg_auth_members a JOIN pg_roles g ON g.oid = a.roleid"
            " JOIN pg_roles m ON m.oid = a.member WHERE g.rolname = 'sql_iam' AND m.rolname LIKE 'r\\_%'"))
        self.assertEqual(members, {"r_oauth": False, "r_maker": True})
        group = self.a.rows("SELECT rolcanlogin, rolsuper FROM pg_roles WHERE rolname = 'sql_iam'")
        self.assertEqual(group, [(False, False)])

    def test_a_role_leaving_oauth_leaves_the_group_and_attributes_converge(self):
        path = self.declare({"roles": {"c_role": {"oauth": True, "enroll": True, "createdb": True}}})
        self.assertEqual(self.run_reconcile(path)[0], 0)
        # Out-of-band drift: a superuser grant, then every attribute the declaration forbids.
        self.a.rows("ALTER ROLE c_role SUPERUSER REPLICATION BYPASSRLS NOLOGIN")
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.a.rows("SELECT rolsuper, rolreplication, rolbypassrls, rolcanlogin, rolcreatedb"
                                     " FROM pg_roles WHERE rolname = 'c_role'"), [(False, False, False, True, True)])
        path = self.declare({"roles": {"c_role": {"createdb": True}}})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.a.rows("SELECT count(*) FROM pg_auth_members a JOIN pg_roles m ON m.oid = a.member"
                                     " WHERE m.rolname = 'c_role'"), [(0,)])

    def test_identifiers_are_quoted(self):
        names = ['Mixed Case "Quote"', "semi;colon--x", 'x"; DROP ROLE hanzo; --']
        database = 'db-with "dash"; DROP DATABASE postgres; --'
        path = self.declare({
            "roles": {names[0]: {"oauth": True, "connect": [database]}, names[1]: {}, names[2]: {"createdb": True}},
            "databases": {database: {"owner": names[2]}},
        })
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        have = {r[0] for r in self.a.rows("SELECT rolname FROM pg_roles")}
        self.assertTrue(set(names) <= have)
        self.assertIn("hanzo", have)
        self.assertEqual(self.a.rows("SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname = %s", (database,)),
                         [(names[2],)])
        self.assertEqual(self.a.rows("SELECT count(*) FROM pg_database WHERE datname = 'postgres'"), [(1,)])
        before = self.snapshot()
        status, lines = self.run_reconcile(path)
        self.assertEqual((status, self.writes(lines)), (0, []))
        self.assertEqual(self.snapshot(), before)

    def test_a_role_that_sets_its_own_password_is_reported(self):
        path = self.declare({"roles": {"p_oauth": {"oauth": True}, "p_scram": {}}})
        self.assertEqual(self.run_reconcile(path)[0], 0)
        self.a.rows("ALTER ROLE p_oauth PASSWORD 'hunter2-s3cret'", user="p_oauth")
        self.a.rows("ALTER ROLE p_scram PASSWORD 'another-s3cret'")
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 1)
        reported = [line for line in lines if "ERROR" in line]
        self.assertEqual(len(reported), 1, reported)
        self.assertIn("sql_iam member p_oauth has a password", reported[0])
        text = "\n".join(lines)
        for secret in ("hunter2", "another-s3cret", "SCRAM-SHA-256$"):
            self.assertNotIn(secret, text)
        self.a.rows("ALTER ROLE p_oauth PASSWORD NULL")
        self.assertEqual(self.run_reconcile(path)[0], 0)


    def test_dropping_enroll_keeps_the_membership_and_takes_the_admin_option(self):
        admin = ("SELECT a.admin_option FROM pg_auth_members a JOIN pg_roles g ON g.oid = a.roleid"
                 " JOIN pg_roles m ON m.oid = a.member WHERE g.rolname = 'sql_iam' AND m.rolname = 'e_role'")
        self.assertEqual(self.run_reconcile(self.declare({"roles": {"e_role": {"oauth": True, "enroll": True}}}))[0], 0)
        self.assertEqual(self.a.rows(admin), [(True,)])
        path = self.declare({"roles": {"e_role": {"oauth": True}}})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertTrue([line for line in lines if 'REVOKE ADMIN OPTION FOR "sql_iam" FROM "e_role"' in line], lines)
        self.assertEqual(self.a.rows(admin), [(False,)])
        status, lines = self.run_reconcile(path)
        self.assertEqual((status, self.writes(lines)), (0, []))

    def test_a_role_the_file_does_not_declare_is_never_altered(self):
        self.a.rows("CREATE ROLE u_repl LOGIN REPLICATION PASSWORD 'u-repl-secret'")
        self.a.rows("CREATE ROLE u_bypass LOGIN BYPASSRLS CREATEDB")
        self.a.rows("CREATE ROLE u_reader LOGIN IN ROLE pg_read_all_data")
        self.a.rows("ALTER ROLE u_reader SET work_mem = '3MB'")
        names = ["u_repl", "u_bypass", "u_reader", REPLICATION[0]]
        held = (
            "SELECT a.rolname, a.rolsuper, a.rolinherit, a.rolcreaterole, a.rolcreatedb, a.rolcanlogin,"
            " a.rolreplication, a.rolbypassrls, a.rolconnlimit, a.rolpassword, a.rolvaliduntil, r.rolconfig"
            " FROM pg_authid a JOIN pg_roles r ON r.oid = a.oid WHERE a.rolname = ANY(%s) ORDER BY 1",
            "SELECT g.rolname, m.rolname, a.admin_option FROM pg_auth_members a JOIN pg_roles g ON g.oid = a.roleid"
            " JOIN pg_roles m ON m.oid = a.member WHERE m.rolname = ANY(%s) OR g.rolname = ANY(%s) ORDER BY 1, 2")
        def read():
            return [self.a.rows(held[0], (names,)), self.a.rows(held[1], (names, names))]
        before = read()
        status, lines = self.run_reconcile(self.declare({"roles": {"u_declared": {"oauth": True}}}))
        self.assertEqual(status, 0, lines)
        self.assertEqual(read(), before)
        self.assertTrue(self.writes(lines))
        for line in lines:
            for name in names:
                self.assertNotIn(name, line)

    def test_the_replication_user_cannot_be_declared(self):
        before = self.snapshot()
        status, lines = self.run_reconcile(self.declare({"roles": {REPLICATION[0]: {}}}))
        self.assertEqual(status, 1, lines)
        self.assertTrue([line for line in lines if "role %s is reserved" % REPLICATION[0] in line], lines)
        self.assertEqual(self.writes(lines), [])
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.a.rows("SELECT rolreplication FROM pg_roles WHERE rolname = %s", (REPLICATION[0],)),
                         [(True,)])
        # The connection Patroni's standbys make with it still opens.
        conn = psycopg2.connect(host="127.0.0.1", port=self.a.pg, user=REPLICATION[0], password=REPLICATION[1],
                                connection_factory=psycopg2.extras.PhysicalReplicationConnection)
        try:
            with conn.cursor() as cur:
                cur.execute("IDENTIFY_SYSTEM")
                self.assertEqual(len(cur.fetchall()), 1)
        finally:
            conn.close()


class Databases(Cluster):
    def test_a_declared_role_cannot_connect_to_another_database(self):
        doc = {"roles": {"k_alice": {"oauth": True}, "k_bob": {"oauth": True}},
               "databases": {"k_adb": {"owner": "k_alice"}, "k_bdb": {"owner": "k_bob"}}}
        status, lines = self.run_reconcile(self.declare(doc))
        self.assertEqual(status, 0, lines)
        self.a.connect("k_alice", "k_adb").close()
        self.a.connect("k_bob", "k_bdb").close()
        self.denied("k_alice", "k_bdb")
        self.denied("k_bob", "k_adb")
        # An undeclared database keeps its PUBLIC CONNECT.
        self.a.connect("k_alice", "postgres").close()
        # A listed database opens, and an unlisted grant made by hand closes again.
        doc["roles"]["k_alice"]["connect"] = ["k_bdb"]
        self.assertEqual(self.run_reconcile(self.declare(doc))[0], 0)
        self.a.connect("k_alice", "k_bdb").close()
        del doc["roles"]["k_alice"]["connect"]
        self.a.rows('GRANT CONNECT ON DATABASE k_adb TO k_bob')
        status, lines = self.run_reconcile(self.declare(doc))
        self.assertEqual(status, 0, lines)
        self.denied("k_alice", "k_bdb")
        self.denied("k_bob", "k_adb")
        self.a.connect("k_alice", "k_adb").close()
        # A grant to a role the file does not declare is left alone.
        self.a.rows("CREATE ROLE k_outsider LOGIN")
        self.a.rows("GRANT CONNECT ON DATABASE k_adb TO k_outsider")
        self.assertEqual(self.run_reconcile(self.declare(doc))[0], 0)
        self.a.connect("k_outsider", "k_adb").close()
        # PUBLIC given back by hand is taken away again.
        self.a.rows("GRANT CONNECT ON DATABASE k_adb TO PUBLIC")
        self.assertEqual(self.run_reconcile(self.declare(doc))[0], 0)
        self.denied("k_bob", "k_adb")

    def test_an_adopted_databases_objects_are_owned_by_the_app_role(self):
        self.a.rows("CREATE DATABASE legacy")
        self.a.rows("CREATE DATABASE legacy_keep")
        self.a.rows("CREATE ROLE legacy_third LOGIN")
        self.a.rows("CREATE TABLE keep_t (id int)", database="legacy_keep")
        # A member of an extension stays with it: install one, if the build has any contrib.
        available = {r[0] for r in self.a.rows("SELECT name FROM pg_available_extensions")}
        contrib = next((name for name in ("citext", "hstore", "pg_trgm") if name in available), None)
        if contrib:
            self.a.rows("CREATE EXTENSION %s" % contrib, database="legacy")
        self.a.rows("""
            CREATE SCHEMA s1;
            CREATE TABLE s1.t (id serial PRIMARY KEY, name text UNIQUE);
            CREATE TABLE public.u (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v int);
            CREATE INDEX u_v ON public.u (v);
            CREATE VIEW public.vw AS SELECT * FROM public.u;
            CREATE MATERIALIZED VIEW public.mv AS SELECT * FROM public.u;
            CREATE SEQUENCE public.sq;
            CREATE FUNCTION public.f(a int, b text) RETURNS int LANGUAGE sql AS 'SELECT 1';
            CREATE PROCEDURE public.pr(a int) LANGUAGE sql AS 'SELECT 1';
            CREATE AGGREGATE public.ag(int) (sfunc = int4pl, stype = int4);
            CREATE TYPE public.ty AS ENUM ('a', 'b');
            CREATE TYPE public.ct AS (x int, y int);
            CREATE DOMAIN public.dm AS int CHECK (value > 0);
            CREATE COLLATION public.co (provider = libc, locale = 'C');
            CREATE TEXT SEARCH DICTIONARY public.tsd (template = simple);
            CREATE STATISTICS public.st ON id, v FROM public.u;
            CREATE OPERATOR public.=== (leftarg = int, rightarg = int, function = int4eq);
            CREATE TABLE public.third_t (id int);
            ALTER TABLE public.third_t OWNER TO legacy_third;
            SELECT lo_create(0);
        """, database="legacy")
        def member(catalog):
            return ("EXISTS (SELECT 1 FROM pg_depend d WHERE d.deptype = 'e' AND d.classid = '%s'::regclass"
                    " AND d.objid = t.oid)" % catalog)
        owners = (
            "SELECT 'class', relname::text, pg_get_userbyid(relowner), %s FROM pg_class t WHERE relnamespace IN"
            " ('public'::regnamespace, 's1'::regnamespace) AND relkind <> 't' UNION ALL"
            " SELECT 'proc', proname::text, pg_get_userbyid(proowner), %s FROM pg_proc t WHERE oid >= 16384 UNION ALL"
            " SELECT 'type', typname::text, pg_get_userbyid(typowner), %s FROM pg_type t WHERE oid >= 16384"
            " AND typcategory <> 'A' AND (typtype <> 'c' OR typname = 'ct') UNION ALL"
            " SELECT 'schema', nspname::text, pg_get_userbyid(nspowner), %s FROM pg_namespace t WHERE oid >= 16384 UNION ALL"
            " SELECT 'collation', collname::text, pg_get_userbyid(collowner), %s FROM pg_collation t WHERE oid >= 16384 UNION ALL"
            " SELECT 'tsdict', dictname::text, pg_get_userbyid(dictowner), %s FROM pg_ts_dict t WHERE oid >= 16384 UNION ALL"
            " SELECT 'stat', stxname::text, pg_get_userbyid(stxowner), %s FROM pg_statistic_ext t WHERE oid >= 16384 UNION ALL"
            " SELECT 'operator', oprname::text, pg_get_userbyid(oprowner), %s FROM pg_operator t WHERE oid >= 16384 UNION ALL"
            " SELECT 'lo', oid::text, pg_get_userbyid(lomowner), false FROM pg_largeobject_metadata t"
        ) % tuple(member(c) for c in ("pg_class", "pg_proc", "pg_type", "pg_namespace", "pg_collation", "pg_ts_dict",
                                      "pg_statistic_ext", "pg_operator"))
        path = self.declare({"roles": {"legacy_app": {"oauth": True}, "legacy_third": {}},
                             "databases": {"legacy": {"owner": "legacy_app", "adopt": "hanzo"}}})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.a.rows("SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname = 'legacy'"),
                         [("legacy_app",)])
        seen = set()
        extension = 0
        for kind, name, owner, in_extension in self.a.rows(owners, database="legacy"):
            seen.add((kind, name))
            if in_extension:
                extension += 1
                self.assertEqual(owner, "hanzo", (kind, name))    # a member of an extension stays with it
            elif (kind, name) == ("class", "third_t"):
                self.assertEqual(owner, "legacy_third")           # another role's object is not taken
            else:
                self.assertEqual(owner, "legacy_app", (kind, name))
        for expected in (("class", "u"), ("class", "u_v"), ("class", "u_id_seq"), ("class", "t_id_seq"), ("class", "mv"),
                         ("class", "vw"), ("class", "sq"), ("proc", "f"), ("proc", "pr"), ("proc", "ag"), ("type", "ty"),
                         ("type", "dm"), ("schema", "s1"), ("collation", "co"), ("tsdict", "tsd"), ("stat", "st"),
                         ("operator", "==="), ("class", "third_t")):
            self.assertIn(expected, seen)
        self.assertEqual(len([s for s in seen if s[0] == "lo"]), 1)
        if contrib:
            self.assertGreater(extension, 3)
        # The app role can now migrate what it adopted.
        self.a.rows("ALTER TABLE public.u ADD COLUMN w int", user="legacy_app", database="legacy")
        # Nothing hanzo owns elsewhere changed hands, and no REASSIGN ran.
        self.assertEqual(self.a.rows("SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname = 'legacy_keep'"),
                         [("hanzo",)])
        self.assertEqual(self.a.rows("SELECT pg_get_userbyid(relowner) FROM pg_class WHERE relname = 'keep_t'",
                                     database="legacy_keep"), [("hanzo",)])
        self.assertFalse([line for line in lines if "REASSIGN" in line])
        # And a second pass changes nothing.
        before = self.snapshot()
        status, lines = self.run_reconcile(path)
        self.assertEqual((status, self.writes(lines)), (0, []), lines)
        self.assertEqual(self.snapshot(), before)


    def test_adoption_covers_the_rest_of_what_an_owner_can_hold(self):
        self.a.rows("CREATE DATABASE more")
        available = {r[0] for r in self.a.rows("SELECT name FROM pg_available_extensions")}
        script = """
            CREATE TABLE public.u (id int);
            CREATE PUBLICATION more_pub FOR TABLE public.u;
            CREATE OPERATOR FAMILY public.more_fam USING btree;
            CREATE OPERATOR CLASS public.more_cls FOR TYPE int4 USING hash AS OPERATOR 1 =, FUNCTION 1 hashint4(int4);
            CREATE TRUSTED LANGUAGE more_lang HANDLER plpgsql_call_handler INLINE plpgsql_inline_handler
                VALIDATOR plpgsql_validator;
        """
        server = "postgres_fdw" in available
        if server:
            # A server follows once its new owner may use its wrapper; the wrapper, a member of its extension, stays.
            script += """
                CREATE EXTENSION postgres_fdw;
                GRANT USAGE ON FOREIGN DATA WRAPPER postgres_fdw TO PUBLIC;
                CREATE SERVER more_srv FOREIGN DATA WRAPPER postgres_fdw OPTIONS (host 'nowhere');
            """
        self.a.rows(script, database="more")
        path = self.declare({"roles": {"more_app": {"oauth": True}},
                             "databases": {"more": {"owner": "more_app", "adopt": "hanzo"}}})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        owners = self.a.rows(
            "SELECT 'publication', pg_get_userbyid(pubowner) FROM pg_publication WHERE pubname = 'more_pub' UNION ALL"
            " SELECT 'family', pg_get_userbyid(opfowner) FROM pg_opfamily WHERE opfname = 'more_fam' UNION ALL"
            " SELECT 'class', pg_get_userbyid(opcowner) FROM pg_opclass WHERE opcname = 'more_cls' UNION ALL"
            " SELECT 'language', pg_get_userbyid(lanowner) FROM pg_language WHERE lanname = 'more_lang' UNION ALL"
            " SELECT 'server', pg_get_userbyid(srvowner) FROM pg_foreign_server WHERE srvname = 'more_srv'",
            database="more")
        self.assertEqual(sorted(owners), sorted(
            [(kind, "more_app") for kind in ("publication", "family", "class", "language") + (("server",) if server else ())]))
        if server:
            self.assertEqual(self.a.rows("SELECT pg_get_userbyid(fdwowner) FROM pg_foreign_data_wrapper"
                                         " WHERE fdwname = 'postgres_fdw'", database="more"), [("hanzo",)])
        before = self.snapshot()
        status, lines = self.run_reconcile(path)
        self.assertEqual((status, self.writes(lines)), (0, []), lines)
        self.assertEqual(self.snapshot(), before)

    def test_what_cannot_change_hands_is_reported(self):
        self.a.rows("CREATE DATABASE stuck")
        self.a.rows("""
            CREATE TABLE public.moves (id int);
            CREATE FOREIGN DATA WRAPPER stuck_fdw;
            CREATE FUNCTION public.stuck_fn() RETURNS event_trigger LANGUAGE plpgsql AS 'BEGIN END';
            CREATE EVENT TRIGGER stuck_et ON ddl_command_end EXECUTE FUNCTION public.stuck_fn();
            CREATE SUBSCRIPTION stuck_sub CONNECTION 'host=nowhere dbname=none' PUBLICATION nope WITH (connect = false);
        """, database="stuck")
        path = self.declare({"roles": {"stuck_app": {"oauth": True}},
                             "databases": {"stuck": {"owner": "stuck_app", "adopt": "hanzo"}}})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 1, lines)
        reported = sorted(line.split(" ERROR ", 1)[1] for line in lines if " ERROR " in line)
        self.assertEqual(reported, [
            "adopt event trigger stuck_et: still owned by hanzo",
            "adopt foreign data wrapper stuck_fdw: still owned by hanzo",
            "adopt subscription stuck_sub: still owned by hanzo",
        ])
        self.assertEqual(self.a.rows("SELECT pg_get_userbyid(relowner) FROM pg_class WHERE relname = 'moves'",
                                     database="stuck"), [("stuck_app",)])
        # Reported on every pass until it is resolved, and nothing is written meanwhile.
        status, lines = self.run_reconcile(path)
        self.assertEqual((status, self.writes(lines)), (1, []), lines)

    def test_an_adopt_role_that_does_not_exist_is_reported(self):
        path = self.declare({"roles": {"g_app": {}}, "databases": {"g_db": {"owner": "g_app", "adopt": "nobody_here"}}})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 1, lines)
        self.assertTrue([line for line in lines if "adopt g_db: role nobody_here does not exist" in line], lines)
        self.assertFalse([line for line in lines if " ALTER " in line and "OWNER TO" in line and "g_db" in line])
        self.assertEqual(self.a.rows("SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname = 'g_db'"),
                         [("g_app",)])


class Patroni(Cluster):
    def tearDown(self):
        self.restore_dcs()

    def dcs(self, **changes):
        doc = json.loads(json.dumps(self.base))
        doc.update(changes)
        return doc

    def test_a_second_run_changes_nothing(self):
        doc = {"roles": {"s_app": {"oauth": True, "createdb": True, "enroll": True, "connect": ["s_db"]}},
               "databases": {"s_db": {"owner": "s_app"}},
               "dcs": self.dcs(ttl=31)}
        doc["dcs"]["postgresql"]["pg_hba"] = HBA + ["host all s_nobody 127.0.0.1/32 reject"]
        path = self.declare(doc)
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertTrue(self.writes(lines))
        self.assertEqual(self.a.config_get()["ttl"], 31)
        before = self.snapshot()
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.writes(lines), [])
        self.assertIn("pass: 0 writes, 0 problems", lines[-1])
        self.assertEqual(self.snapshot(), before)

    def test_every_statement_is_logged(self):
        path = self.declare({"roles": {"l_role": {"oauth": True}}, "databases": {"l_db": {"owner": "l_role"}}})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        sent = [line.split(": ", 1)[1] for line in lines if " sql write postgres: " in line]
        for statement in ('CREATE ROLE "l_role" LOGIN NOSUPERUSER NOREPLICATION NOBYPASSRLS NOCREATEDB NOCREATEROLE',
                          'ALTER ROLE "l_role" SET synchronous_commit = on', 'GRANT "sql_iam" TO "l_role"',
                          'CREATE DATABASE "l_db" OWNER "l_role"', 'REVOKE CONNECT ON DATABASE "l_db" FROM PUBLIC'):
            self.assertIn(statement, sent)
        self.assertTrue([line for line in lines if " sql read postgres: SELECT pg_is_in_recovery()" in line])

    def test_a_standby_does_nothing(self):
        before = self.snapshot()
        doc = {"roles": {"y_role": {"oauth": True}}, "databases": {"y_db": {"owner": "y_role"}},
               "dcs": self.dcs(ttl=33)}
        status, lines = self.run_reconcile(self.declare(doc), self.b)
        self.assertEqual(status, 0, lines)
        self.assertEqual(len(lines), 2, lines)
        self.assertIn("standby, nothing to do", lines[1])
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.a.rows("SELECT count(*) FROM pg_roles WHERE rolname = 'y_role'"), [(0,)])

    def test_a_dcs_drift_is_patched_back(self):
        want = self.dcs(ttl=34)
        path = self.declare({"dcs": want})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.a.config_get(), want)
        drifted = json.loads(json.dumps(want))
        drifted.update(ttl=40, loop_wait=6)
        drifted["postgresql"]["parameters"]["work_mem"] = "2MB"
        self.a.config_patch({"ttl": 40, "loop_wait": 6, "postgresql": {"parameters": {"work_mem": "2MB"}}})
        self.settle(drifted)
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        patched = [line for line in lines if " patroni PATCH " in line]
        self.assertEqual(len(patched), 1, lines)
        self.assertIn('"work_mem": null', patched[0])
        self.assertEqual(self.a.config_get(), want)
        self.assertEqual(self.writes(self.run_reconcile(path)[1]), [])

    def test_pg_hba_reloads_when_its_hash_changes(self):
        want = self.dcs()
        want["postgresql"]["pg_hba"] = HBA + ["host all h_one 127.0.0.1/32 reject"]
        path = self.declare({"dcs": want})
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertTrue([line for line in lines if "pg_reload_conf" in line])
        rules = [(r[0], r[1]) for r in self.a.rows("SELECT database, user_name FROM pg_hba_file_rules")]
        self.assertIn((["all"], ["h_one"]), rules)
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertFalse([line for line in lines if "pg_reload_conf" in line])
        want["postgresql"]["pg_hba"] = HBA + ["host all h_two 127.0.0.1/32 reject"]
        status, lines = self.run_reconcile(self.declare({"dcs": want}))
        self.assertEqual(status, 0, lines)
        self.assertTrue([line for line in lines if "pg_reload_conf" in line])
        rules = [(r[0], r[1]) for r in self.a.rows("SELECT database, user_name FROM pg_hba_file_rules")]
        self.assertIn((["all"], ["h_two"]), rules)
        self.assertNotIn((["all"], ["h_one"]), rules)
        # The standby holds the same file: Patroni writes it on every member.
        deadline = time.monotonic() + 60
        while "h_two" not in self.b.rows("SELECT pg_read_file(current_setting('hba_file'))")[0][0]:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(1)

    def test_a_pg_hba_line_the_server_refuses_is_reported(self):
        want = self.dcs()
        want["postgresql"]["pg_hba"] = HBA + ["host all all 127.0.0.1/32 nosuchmethod"]
        status, lines = self.run_reconcile(self.declare({"dcs": want}))
        self.assertEqual(status, 1, lines)
        self.assertTrue([line for line in lines if "ERROR pg_hba.conf line" in line and "nosuchmethod" in line], lines)
        self.assertFalse([line for line in lines if "COMMENT ON ROLE" in line])
        want["postgresql"]["pg_hba"] = HBA
        status, lines = self.run_reconcile(self.declare({"dcs": want}))
        self.assertEqual(status, 0, lines)


class Timeouts(Cluster):
    def test_every_connection_carries_its_timeouts(self):
        run = reconcile.Pass(argparse.Namespace(socket=self.a.sock, port=self.a.pg), "hanzo")
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                rows = run.sql("postgres", "SELECT name, setting FROM pg_settings"
                                           " WHERE name IN ('lock_timeout', 'statement_timeout')")
        finally:
            for conn in run.conns.values():
                conn.close()
        self.assertEqual(dict(rows), {"lock_timeout": str(reconcile.LOCK_TIMEOUT),
                                      "statement_timeout": str(reconcile.STATEMENT_TIMEOUT)})

    def test_a_held_lock_fails_one_step_and_does_not_freeze_the_table(self):
        self.a.rows("CREATE DATABASE lk_db")
        self.a.rows("CREATE TABLE t (id int)", database="lk_db")
        doc = {"roles": {"lk_app": {"oauth": True}}}
        self.assertEqual(self.run_reconcile(self.declare(doc))[0], 0)
        self.a.rows("ALTER ROLE lk_app PASSWORD 'lk-secret'")
        doc["databases"] = {"lk_db": {"owner": "lk_app", "adopt": "hanzo"}}
        path = self.declare(doc)
        limit = reconcile.LOCK_TIMEOUT / 1000
        # A session idle in a transaction that has read the table: ALTER OWNER waits for its ACCESS EXCLUSIVE lock.
        holder = self.a.connect("hanzo", "lk_db")
        holder.autocommit = False
        proc = None
        try:
            with holder.cursor() as cur:
                cur.execute("SELECT * FROM t")
            started = time.monotonic()
            proc = subprocess.Popen(self.argv(path, self.a, "--once"), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, env=self.env())
            deadline = time.monotonic() + 30
            while not self.a.rows("SELECT 1 FROM pg_stat_activity WHERE application_name = 'reconcile'"
                                  " AND wait_event_type = 'Lock'"):
                self.assertLess(time.monotonic(), deadline, "the pass never waited for the table's lock")
                time.sleep(0.1)
            # A reader of the table queues behind the pass's request; it is released when the pass gives up.
            reader = self.a.connect("hanzo", "lk_db")
            try:
                with reader.cursor() as cur:
                    cur.execute("SET statement_timeout = %d" % ((limit + 3) * 1000))
                    try:
                        cur.execute("SELECT count(*) FROM t")
                    except psycopg2.errors.QueryCanceled:
                        self.fail("a reader of the table was held past the lock timeout")
            finally:
                reader.close()
            out, err = proc.communicate(timeout=60)
            took = time.monotonic() - started
        finally:
            holder.rollback()
            holder.close()
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.communicate()
        lines = out.splitlines()
        self.assertEqual(err, "")
        self.assertEqual(proc.returncode, 1, lines)
        self.assertLess(took, limit + 25)
        self.assertTrue([line for line in lines if "ERROR adopt table public.t" in line and "lock timeout" in line], lines)
        # The step failed; the rest of the pass ran.
        self.assertTrue([line for line in lines if "sql_iam member lk_app has a password" in line], lines)
        self.assertEqual(self.a.rows("SELECT pg_get_userbyid(relowner) FROM pg_class WHERE relname = 't'",
                                     database="lk_db"), [("hanzo",)])
        # The next pass, with the lock gone, finishes the adoption.
        self.a.rows("ALTER ROLE lk_app PASSWORD NULL")
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.a.rows("SELECT pg_get_userbyid(relowner) FROM pg_class WHERE relname = 't'",
                                     database="lk_db"), [("lk_app",)])


class Daemon(Cluster):
    def test_it_passes_until_told_to_stop(self):
        path = self.declare({"roles": {"d_role": {"oauth": True}}})
        proc = subprocess.Popen(self.argv(path, self.a, "--interval", "300"), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, env=self.env())
        watchdog = threading.Timer(120, proc.kill)
        watchdog.start()
        try:
            lines = []
            for line in proc.stdout:
                lines.append(line)
                if "problems" in line:
                    break
            self.assertRegex(lines[-1], r"pass: [1-9]\d* writes, 0 problems", lines)
            proc.send_signal(signal.SIGTERM)  # the daemon is now asleep until its next pass
            status = proc.wait(timeout=10)
            err = proc.stderr.read()
        finally:
            watchdog.cancel()
            if proc.poll() is None:
                proc.kill()
            proc.stdout.close()
            proc.stderr.close()
        self.assertEqual((status, err), (0, ""))
        self.assertEqual(self.a.rows("SELECT count(*) FROM pg_roles WHERE rolname = 'd_role'"), [(1,)])


class Declared(Cluster):
    def test_a_file_that_does_not_match_its_pin_is_not_applied(self):
        path = self.declare({"roles": {"n_role": {"oauth": True}}})
        with open(path, "a") as file:
            file.write("  n_other: {}\n")
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 1)
        self.assertTrue([line for line in lines if "does not match its pin" in line], lines)
        self.assertEqual(self.writes(lines), [])
        # The pin is the process's, not the mount's: a pin written beside the edited file names nothing.
        with open(path, "rb") as file:
            edited = hashlib.sha256(file.read()).hexdigest()
        with open(path + ".sha256", "w") as file:
            file.write("%s  declared.yml\n" % edited)
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 1)
        self.assertTrue([line for line in lines if "does not match its pin" in line], lines)
        self.assertEqual(self.writes(lines), [])
        self.assertEqual(self.a.rows("SELECT count(*) FROM pg_roles WHERE rolname LIKE 'n\\_%'"), [(0,)])
        # Started with the edited file's digest, it applies.
        PINS[path] = edited
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.a.rows("SELECT count(*) FROM pg_roles WHERE rolname LIKE 'n\\_%'"), [(2,)])

    def test_a_missing_file_is_not_applied(self):
        path = self.declare({"roles": {"m_role": {}}})
        os.remove(path)
        status, lines = self.run_reconcile(path)
        self.assertEqual(status, 1)
        self.assertTrue([line for line in lines if "cannot read the declared file" in line], lines)
        self.assertEqual(self.writes(lines), [])

    def test_it_starts_only_with_a_pin_and_both_users(self):
        path = self.declare({})
        def start(argv, env):
            done = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=30)
            self.assertEqual(done.stdout, "")
            return done
        argv = self.argv(path, self.a, "--once")
        index = argv.index("--sha256")
        done = start(argv[:index] + argv[index + 2:], self.env())
        self.assertEqual(done.returncode, 2)
        self.assertIn("--sha256", done.stderr)
        done = start(argv[:index + 1] + ["not-a-digest"] + argv[index + 2:], self.env())
        self.assertEqual(done.returncode, 2)
        self.assertIn("64 hex digits", done.stderr)
        for missing in ("SQL_USER", "SQL_REPLICATION_USER"):
            env = self.env()
            del env[missing]
            done = start(argv, env)
            self.assertEqual(done.returncode, 1)
            self.assertIn(missing, done.stderr)


class Validate(unittest.TestCase):
    def refused(self, doc, message):
        with self.assertRaises(reconcile.Invalid) as caught:
            reconcile.validate(doc, "hanzo", REPLICATION[0])
        self.assertIn(message, str(caught.exception))

    def test_the_declared_file_is_strict(self):
        reconcile.validate({"roles": {"a": {"oauth": True, "enroll": True, "connect": ["d"]}},
                            "databases": {"d": {"owner": "a", "adopt": "hanzo"}}}, "hanzo", REPLICATION[0])
        reconcile.validate(None, "hanzo", REPLICATION[0])
        self.refused({"role": {}}, "unknown key role")
        self.refused({"roles": {"a": {"superuser": True}}}, "unknown key superuser")
        self.refused({"roles": {"a": {"enroll": True}}}, "enroll needs oauth")
        self.refused({"roles": {"a": {"oauth": "yes"}}}, "oauth must be true or false")
        self.refused({"roles": {"hanzo": {}}}, "reserved")
        self.refused({"roles": {REPLICATION[0]: {"createdb": True}}}, "reserved")
        self.refused({"roles": {"sql_iam": {}}}, "reserved")
        self.refused({"roles": {"pg_monitor": {}}}, "reserved")
        self.refused({"roles": {"a": {"connect": ["d"]}}}, "undeclared database d")
        self.refused({"databases": {"d": {"owner": "ghost"}}}, "not a declared role")
        self.refused({"databases": {"d": {}}}, "owner name None")
        self.refused({"databases": {"postgres": {"owner": "hanzo"}}}, "reserved")
        self.refused({"roles": {"a": {}}, "databases": {"d": {"owner": "a", "adopt": "a"}}}, "adopt names its owner")
        self.refused({"dcs": {"ttl": None}}, "null is not a value")
        self.refused({"dcs": {"postgresql": {"pg_hba": "local all all trust"}}}, "list of lines")
        self.refused({"roles": {"x" * 64: {}}}, "1 to 63 bytes")

    def test_a_repeated_key_is_refused(self):
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "declared.yml")
            raw = b"roles:\n  a: {oauth: true}\n  a: {}\n"
            with open(path, "wb") as file:
                file.write(raw)
            with self.assertRaises(reconcile.Invalid) as caught:
                reconcile.load(path, hashlib.sha256(raw).hexdigest(), "hanzo", REPLICATION[0])
            self.assertIn("declared twice", str(caught.exception))

    def test_diff_is_the_merge_patch(self):
        have = {"a": 1, "b": {"c": 1, "d": 2}, "e": [1, 2], "gone": 1}
        want = {"a": 1, "b": {"c": 1, "d": 3}, "e": [1, 3], "new": {"x": 1}}
        self.assertEqual(reconcile.diff(have, want), {"b": {"d": 3}, "e": [1, 3], "new": {"x": 1}, "gone": None})
        self.assertEqual(reconcile.diff(want, want), {})


if __name__ == "__main__":
    unittest.main(verbosity=2)
