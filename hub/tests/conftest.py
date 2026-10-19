import base64
import socket
import sys
import time
from pathlib import Path

import pytest
from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import sim_device  # noqa: E402

from brain_drain_hub.app import create_app  # noqa: E402
from brain_drain_hub.config import Settings  # noqa: E402

TOKEN = "tok-test-123"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(cond, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


@pytest.fixture
def settings(tmp_path):
    return Settings(db_path=str(tmp_path / "hub.db"), secret_key=Fernet.generate_key().decode(),
                    admin_password="pw", poll_interval=60)


@pytest.fixture
def make_sim():
    servers = []

    def make(name="sim", hook_urls=None, device_id=None):
        port = free_port()
        s = sim_device.Server(sim_device.Sim(TOKEN, name, device_id, hook_urls), port).start()
        s.port = port
        s.address = f"127.0.0.1:{port}"
        servers.append(s)
        return s

    yield make
    for s in servers:
        s.stop()


@pytest.fixture
def sim(make_sim):
    return make_sim()


def authed(app) -> TestClient:
    c = TestClient(app)
    c.headers["Authorization"] = "Basic " + base64.b64encode(b"admin:pw").decode()
    return c


@pytest.fixture
def client(settings):
    with authed(create_app(settings)) as c:
        yield c


@pytest.fixture
def dev_id(client, sim):
    r = client.post("/api/v1/devices", json={"address": sim.address, "token": TOKEN})
    assert r.status_code == 200, r.text
    return r.json()["id"]
