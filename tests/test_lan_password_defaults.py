"""Test the LAN password policy against a disposable PostgreSQL database."""
from contextlib import contextmanager
import socket
import subprocess
import time
import uuid

import lan_integration as base


PRODUCT = "ICE_BEAUTY"
SERVER = base.BUILD / "WmsServer.exe"
DEFAULT = "123456"


@contextmanager
def running_server(env):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen([str(SERVER), "--product", PRODUCT, "--port", str(port)],
                               env=env, creationflags=base.FLAGS)
    try:
        for _ in range(60):
            if process.poll() is not None:
                raise RuntimeError("Test server exited during startup")
            try:
                status, health = base.api(port, "/health")
                if status == 200 and health["product"] == PRODUCT:
                    break
            except OSError:
                pass
            time.sleep(0.1)
        else:
            raise RuntimeError("Test server did not become ready")
        yield port
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)


def login(port, username, password, version=2):
    return base.api(port, f"/api/v{version}/login",
                    {"product": PRODUCT, "username": username, "password": password})


def rpc(port, token, method, **arguments):
    status, result = base.api(port, "/api/v2/data",
                              {"kind": "rpc", "method": "UserService." + method,
                               "arguments": arguments, "requestId": str(uuid.uuid4())}, token)
    assert status == 200, f"{method} returned HTTP {status}"
    return result


def test_lan_password_defaults():
    database = "wms_password_test_" + uuid.uuid4().hex
    role = "wms_icebeauty_business_demo"
    env = base.ENV | {"WMS_PG_HOST": "127.0.0.1", "WMS_PG_PORT": "55432",
                      "WMS_PG_DATABASE": database, "WMS_PG_USER": role,
                      "WMS_PG_PASSWORD": base.CREDS["app"], "WMS_DEMO_ADMIN_PASSWORD": DEFAULT}
    env.pop("WMS_LAN_TEST_PASSWORDS", None)
    test_env = env | {"WMS_LAN_TEST_PASSWORDS": "1"}
    base.sql(f"CREATE DATABASE {database} OWNER {role};")
    try:
        for schema_flag in ("--init-demo", "--init-business"):
            command = [SERVER, "--product", PRODUCT, schema_flag]
            assert base.run(command, env=env, check=False).returncode == 3, schema_flag
            base.run(command, env=test_env)

        with running_server(test_env) as port:
            assert login(port, "admin", DEFAULT, version=1)[0] == 200
            status, admin = login(port, "admin", DEFAULT)
            assert status == 200 and admin["testPasswordDefaults"] is True
            admin_token = admin["token"]
            created = rpc(port, admin_token, "createUser", username="viewer", displayName="Viewer",
                          roleCode="QUERY", password="")
            assert created.get("result") is True, "Create with blank password"
            status, viewer = login(port, "viewer", DEFAULT)
            assert status == 200
            for invalid in ("", "123457"):
                rejected = rpc(port, viewer["token"], "changeOwnPassword",
                               currentPassword=DEFAULT, newPassword=invalid)
                assert "error" in rejected, "Personal changes require an allowed nonempty password"
            custom = "ViewerPass123!"
            changed = rpc(port, viewer["token"], "changeOwnPassword",
                          currentPassword=DEFAULT, newPassword=custom)
            assert changed.get("result") is True
            assert login(port, "viewer", DEFAULT)[0] == 401
            assert login(port, "viewer", custom)[0] == 200
            reset = rpc(port, admin_token, "resetPassword", userId=created["userId"], newPassword="")
            assert reset.get("result") is True
            assert login(port, "viewer", DEFAULT)[0] == 200
            assert login(port, "viewer", custom)[0] == 401

        with running_server(env) as port:
            status, admin = login(port, "admin", DEFAULT)
            assert status == 200 and admin["testPasswordDefaults"] is False
            for invalid in ("", DEFAULT):
                result = rpc(port, admin["token"], "resetPassword", userId=created["userId"],
                             newPassword=invalid)
                assert "error" in result, "Regular server must enforce its existing password policy"
    finally:
        base.sql(f"DROP DATABASE {database} WITH (FORCE);")


if __name__ == "__main__":
    test_lan_password_defaults()
    print("PASS: LAN password defaults, both authentication APIs, and regular server policy")
