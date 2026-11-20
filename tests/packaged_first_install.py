"""Exercise a first install from the release ZIP under a Chinese Windows path."""
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.request
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[1]
POWERSHELL = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"


def run(args, env, sql=None):
    result = subprocess.run(args, env=env, input=sql, capture_output=True, timeout=90,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise RuntimeError(f"{Path(args[0]).name} failed ({result.returncode}): "
                           + result.stderr.decode("utf-8", "backslashreplace")[-1800:])
    return result.stdout


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return str(probe.getsockname()[1])


def test_release(work):
    host = work / "软件" / "WMS仓库系统" / "WMS"
    with ZipFile(ROOT / "dist/WMS仓库系统.zip") as archive:
        for entry in archive.infolist():
            name = entry.filename.replace("\\", "/")
            if name.startswith(("WMS/scripts/", "WMS/build-lan-business/", "WMS/outputs/lan-dev/postgres/")):
                archive.extract(entry, host.parent)
    runtime = host / "outputs/lan-dev"
    pg_bin = runtime / "postgres/pgsql/bin"
    data = work / "Documents/WMS_OL"
    data.mkdir(parents=True)
    cluster = data / "pgdata"
    password = secrets.token_hex(32)
    credentials = data / "credentials.json"
    credentials.write_text(json.dumps({"postgres": password, "app": password, "admin": "123456"}), encoding="utf-8")
    before = credentials.read_bytes()
    env = dict(os.environ, WMS_TEST_DATA=str(data), WMS_TEST_RUNTIME=str(runtime), WMS_TEST_BIN=str(pg_bin),
               WMS_TEST_STORAGE=str(host / "scripts/lan-data-storage.ps1"),
               PATH=os.pathsep.join([str(pg_bin), str(POWERSHELL.parents[2]), os.environ["SystemRoot"]]))
    prefix = "$ErrorActionPreference='Stop'; . $env:WMS_TEST_STORAGE; "
    ps = [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command"]
    guard = "Assert-LanStorageLayout $env:WMS_TEST_RUNTIME $env:WMS_TEST_DATA -ConfirmFirstInstall"
    # Explicitly declining must preserve the credential-only residue from the old installer.
    refuse = "function Read-Host { '' }; try { " + guard + "; exit 1 } catch { exit 0 }"
    run(ps + [prefix + refuse], env)
    assert not cluster.exists() and credentials.read_bytes() == before
    cluster.mkdir()
    sentinel = cluster / "existing-data"
    sentinel.write_bytes(b"keep")
    run(ps + [prefix + "function Read-Host { '1' }; try { " + guard + "; exit 1 } catch { exit 0 }"], env)
    assert sentinel.read_bytes() == b"keep"
    sentinel.unlink()
    (data / "init-password.tmp").write_text(password + "\n", encoding="ascii")
    initialize = ("; Initialize-LanCluster $env:WMS_TEST_BIN (Join-Path $env:WMS_TEST_DATA pgdata) "
                  "(Join-Path $env:WMS_TEST_DATA init-password.tmp)")
    run(ps + [prefix + "function Read-Host { '1' }; " + guard + initialize], env)
    assert credentials.read_bytes() == before and (cluster / "global/pg_control").is_file()
    assert (cluster / "PG_VERSION").read_text().strip() == "17"
    run(ps + [prefix + "Assert-LanStorageLayout $env:WMS_TEST_RUNTIME $env:WMS_TEST_DATA"], env)

    pg_port = free_port()
    pg_ctl = [pg_bin / "pg_ctl.exe", "-D", cluster]
    env.update(PGPASSWORD=password, WMS_PG_HOST="127.0.0.1", WMS_PG_PORT=pg_port, WMS_PG_PASSWORD=password,
               WMS_DEMO_ADMIN_PASSWORD="123456", WMS_LAN_TEST_PASSWORDS="1", WMS_PG_BIN=str(pg_bin))
    try:
        # The postmaster may inherit pipe handles on Windows. Give its launcher real null handles.
        subprocess.run(pg_ctl + ["-l", work / "postgres.log", "-o", f"-h 127.0.0.1 -p {pg_port}", "-w", "start"],
                       env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=subprocess.CREATE_NO_WINDOW, check=True, timeout=60)
        server = host / "build-lan-business/WmsServer.exe"
        http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        for profile in ("ICE_BEAUTY", "MEI_JING_LING"):
            role = "wms_" + profile.lower()
            sql = f"CREATE ROLE {role} LOGIN PASSWORD '{password}';\nCREATE DATABASE {role} OWNER {role};"
            run([pg_bin / "psql.exe", "-X", "-w", "-h", "127.0.0.1", "-p", pg_port, "-U", "postgres",
                 "-d", "postgres", "-v", "ON_ERROR_STOP=1"], env, sql.encode("ascii"))
            env.update(WMS_PG_DATABASE=role, WMS_PG_USER=role)
            run([server, "--product", profile, "--init-lan-empty"], env)
            run([server, "--product", profile, "--init-business"], env)
            port = free_port()
            app = subprocess.Popen([server, "--product", profile, "--port", port], env=env,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                url = f"http://127.0.0.1:{port}"
                for _ in range(80):
                    if app.poll() is not None:
                        raise RuntimeError(f"{profile} exited early")
                    try:
                        with http.open(url + "/health", timeout=2) as response:
                            assert json.load(response)["product"] == profile
                        break
                    except OSError:
                        time.sleep(0.15)
                else:
                    raise RuntimeError("Health check timed out")
                body = json.dumps({"product": profile, "username": "admin", "password": "123456"}).encode()
                request = urllib.request.Request(url + "/api/v1/login", body, {"Content-Type": "application/json"})
                with http.open(request, timeout=10) as response:
                    assert json.load(response)["token"]
                print(profile, "fresh database, health and login OK", flush=True)
            finally:
                if app.poll() is None:
                    app.terminate()
                app.wait(timeout=10)
    finally:
        if (cluster / "postmaster.pid").is_file():
            run(pg_ctl + ["-m", "fast", "-w", "stop"], env)


if __name__ == "__main__":
    workspace = (ROOT / ".codex-work").resolve()
    workspace.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="first-install-", dir=workspace) as directory:
        work = Path(directory).resolve()
        assert work.parent == workspace
        test_release(work)
