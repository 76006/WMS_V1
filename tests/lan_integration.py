"""Integration checks against real PostgreSQL and the compiled Qt executables.

Uses unique disposable databases. Never opens or migrates a desktop SQLite database.
Run scripts/start-lan-dev.ps1 first to provision the local PostgreSQL development cluster.
"""
import concurrent.futures
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "outputs" / "lan-dev"
BUILD = ROOT / "build-lan"
PG_BIN = RUNTIME / "postgres" / "pgsql" / "bin"
QT_BIN = Path(os.environ.get("WMS_TEST_QT_BIN", "D:/Qt/6.5.3/mingw_64/bin"))
CREDS = json.loads((RUNTIME / "credentials.json").read_text(encoding="utf-8-sig"))
ENV = os.environ.copy()
ENV["PATH"] = os.pathsep.join([str(PG_BIN), str(QT_BIN), ENV["PATH"]])
ENV["QT_QPA_PLATFORM"] = "offscreen"
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))
CHECKS = 0


def run(args, env=ENV, check=True, timeout=30, input_text=None):
    return subprocess.run([str(a) for a in args], env=env, input=input_text,
                          capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout, creationflags=FLAGS, check=check)


def sql(statement, database="postgres"):
    env = ENV | {"PGPASSWORD": CREDS["postgres"]}
    return run([PG_BIN / "psql.exe", "-X", "-h", "127.0.0.1", "-p", "55432",
                "-U", "postgres", "-d", database, "-v", "ON_ERROR_STOP=1", "-t", "-A"],
               env=env, input_text=statement).stdout.strip()


def api(port, path, body=None, token=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}",
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers=headers)
    try:
        response = HTTP.open(req, timeout=15)
    except urllib.error.HTTPError as exc:
        response = exc
    with response:
        return response.status, json.load(response)


def expect(condition, description):
    global CHECKS
    if not condition:
        raise AssertionError(description)
    CHECKS += 1
    print("PASS:", description, flush=True)


def main():
    processes = []
    databases = []
    clients = []
    test_password = secrets.token_urlsafe(24)
    try:
        for profile, executable in [("ICE_BEAUTY", "IceBeautyWms.exe"),
                                    ("MEI_JING_LING", "MeiJingLingWms.exe")]:
            database = "wms_lan_test_" + uuid.uuid4().hex
            sql(f"CREATE DATABASE {database} OWNER wms_icebeauty_demo;")
            databases.append(database)
            env = ENV | {"WMS_PG_HOST": "127.0.0.1", "WMS_PG_PORT": "55432",
                         "WMS_PG_DATABASE": database, "WMS_PG_USER": "wms_icebeauty_demo",
                         "WMS_PG_PASSWORD": CREDS["app"], "WMS_DEMO_ADMIN_PASSWORD": test_password}
            server = BUILD / "WmsServer.exe"
            run([server, "--product", profile, "--init-demo"], env=env)
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            proc = subprocess.Popen([str(server), "--product", profile, "--port", str(port)],
                                    env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    creationflags=FLAGS)
            processes.append(proc)
            for _ in range(100):
                if proc.poll() is not None:
                    raise RuntimeError("Test server exited")
                try:
                    status, health = api(port, "/health")
                    if status == 200:
                        break
                except OSError:
                    time.sleep(0.1)
            else:
                raise RuntimeError("Test server did not become ready")
            expect(health["product"] == profile, f"{profile}: database identity")
            expect(api(port, "/api/v1/inventory")[0] == 401, "unauthenticated inventory is rejected")
            body = {"product": profile, "username": "admin", "password": test_password}
            other = "MEI_JING_LING" if profile == "ICE_BEAUTY" else "ICE_BEAUTY"
            expect(api(port, "/api/v1/login", body | {"product": other})[0] == 409, "wrong product rejected")
            expect(api(port, "/api/v1/login", body | {"password": "incorrect"})[0] == 401, "wrong password rejected")
            expect(api(port, "/api/v1/login", body | {"username": "' OR 1=1 --"})[0] == 401, "login SQL injection rejected")
            first = api(port, "/api/v1/login", body)[1]["token"]
            second = api(port, "/api/v1/login", body)[1]["token"]
            expect(first != second, "clients have independent sessions")
            inventory = api(port, "/api/v1/inventory", token=first)
            expect(inventory[0] == 200 and inventory[1]["items"][0]["quantity"] == "100.000000", "PostgreSQL inventory quantity is exact")
            expect(api(port, "/api/v1/inventory?limit=0", token=first)[0] == 400, "invalid page size rejected")
            expect(api(port, "/api/v1/inventory?offset=-1", token=first)[0] == 400, "negative offset rejected")
            expect(api(port, "/api/v1/inventory?q=%25", token=first)[1]["items"] == [], "search wildcard is treated literally")
            keyword = urllib.parse.quote("测试物料")
            expect(len(api(port, f"/api/v1/inventory?q={keyword}", token=first)[1]["items"]) == 1, "Chinese keyword search")
            sql("UPDATE wms_lan.stock_balances SET quantity=73.125000;", database)
            with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
                reads = list(pool.map(lambda _: api(port, "/api/v1/inventory", token=second), range(5)))
            expect(all(r[0] == 200 and r[1]["items"][0]["quantity"] == "73.125000" for r in reads), "five clients see committed data")
            api(port, "/api/v1/logout", {}, first)
            expect(api(port, "/api/v1/inventory", token=first)[0] == 401 and api(port, "/api/v1/inventory", token=second)[0] == 200, "logout revokes only the intended session")
            sql("UPDATE wms_lan.users SET can_view_inventory=false;", database)
            expect(api(port, "/api/v1/inventory", token=second)[0] == 403, "permission revocation is effective for existing sessions")
            sql("UPDATE wms_lan.users SET can_view_inventory=true,is_active=false;", database)
            expect(api(port, "/api/v1/inventory", token=second)[0] == 401, "disabled account loses access")
            sql("UPDATE wms_lan.users SET is_active=true;", database)
            expect(api(port, "/api/v1/inventory", token=second)[0] == 401, "reenabling account does not resurrect revoked session")
            token = api(port, "/api/v1/login", body)[1]["token"]
            sql(f"SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='{database}' AND application_name='WmsServer';")
            expect(api(port, "/api/v1/inventory", token=token)[0] == 200, "database reconnect after terminated connection")
            sql(f"UPDATE wms_lan.system_identity SET product='{other}';", database)
            expect(api(port, "/health")[0] == 503 and api(port, "/api/v1/inventory", token=token)[0] == 503, "mismatched database remains unavailable")
            sql(f"UPDATE wms_lan.system_identity SET product='{profile}';", database)
            expect(api(port, "/health")[0] == 200, "database identity recovery")
            expect(run([server, "--product", profile, "--init-demo"], env=env, check=False).returncode != 0, "repeat initialization refuses overwrite")
            expect(run([server, "--product", other], env=env, check=False).returncode == 4, "server refuses another product database at startup")
            expect(run([server, "--product", profile, "--listen", "0.0.0.0"], env=env, check=False).returncode == 6, "LAN HTTP requires explicit test opt-in")
            client_env = env | {"WMS_TEST_PASSWORD": test_password}
            result = run([BUILD / executable, "--server", f"http://127.0.0.1:{port}", "--network-smoke-test"], env=client_env, check=False, timeout=45)
            expect(result.returncode == 0, f"{profile}: actual desktop logs in and renders PostgreSQL rows ({result.returncode})")
            clients.append((port, token, executable, client_env))
            # Local smoke checks use only fresh files below the ignored output directory.
            local_file = RUNTIME / (database + ".db")
            local = run([BUILD / executable, "--database", local_file, "--smoke-test"], env=ENV, check=False)
            expect(local.returncode == 0, f"{profile}: standalone SQLite initialization still works")
        # Deliberately point IceBeauty at MeiJingLing using the real desktop network path.
        port, _, _, env = clients[1]
        wrong = run([BUILD / "IceBeautyWms.exe", "--server", f"http://127.0.0.1:{port}", "--network-smoke-test"], env=env, check=False, timeout=45)
        expect(wrong.returncode == 3, "desktop rejects wrong product endpoint")
        print(f"Completed {CHECKS} integration checks.", flush=True)
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        for database in databases:
            sql(f"DROP DATABASE {database} WITH (FORCE);")


if __name__ == "__main__":
    main()
