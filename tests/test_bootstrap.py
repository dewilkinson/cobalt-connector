"""Checks for bootstrap/cobalt_bootstrap.py that need no server: `python -m pytest tests`."""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def boot(monkeypatch, tmp_path):
    spec = importlib.util.spec_from_file_location("cobalt_bootstrap", ROOT / "bootstrap" / "cobalt_bootstrap.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["cobalt_bootstrap"] = mod
    spec.loader.exec_module(mod)
    monkeypatch.setenv("COBALT_CONTRIB_HOME", str(tmp_path))
    monkeypatch.setenv("COBALT_CONTRIB_KEY", "test-key")
    yield mod
    sys.modules.pop("cobalt_bootstrap", None)


class _Reply:
    def __init__(self, body: dict):
        self._body = json.dumps(body).encode("utf-8")

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def test_an_outdated_app_says_so_and_starts_no_loop(boot, monkeypatch, tmp_path):
    why = "This Cobalt Connector is too old to take work."
    monkeypatch.setattr(boot, "request", lambda *a, **k: _Reply({"code": "abc123", "state": "outdated", "why": why}))
    fetched = []
    monkeypatch.setattr(boot, "fetch", lambda want: fetched.append(want))

    class Stop(Exception):
        pass

    def wait(seconds, state, error="", code="", why=""):
        boot.write_status(state, error, code, why)
        raise Stop

    monkeypatch.setattr(boot, "wait", wait)
    with pytest.raises(Stop):
        boot.prepare()
    status = json.loads((tmp_path / "status.json").read_text(encoding="utf-8"))
    assert status["state"] == "outdated" and status["why"] == why
    assert fetched == [], "no build is fetched (and no loop started) for an app the fleet will not give work"


def test_a_current_app_fetches_the_build(boot, monkeypatch, tmp_path):
    monkeypatch.setattr(boot, "request", lambda *a, **k: _Reply({"code": "abc123", "state": "idle"}))
    monkeypatch.setattr(boot, "fetch", lambda want: tmp_path / want)
    assert boot.prepare() == tmp_path / "abc123"


# ---------------------------------------------------------------- the partner key never follows a redirect


import http.server  # noqa: E402
import threading  # noqa: E402


class _Server:
    """A local HTTP server: /moved answers 302 to /elsewhere on a second server; both record the headers they got."""

    def __init__(self, status: int = 200, location: str = ""):
        seen = self.seen = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def _answer(self):
                seen.append({"path": self.path, "key": self.headers.get("X-Cobalt-Contributor")})
                self.rfile.read(int(self.headers.get("Content-Length") or 0))
                if status in (301, 302, 303, 307, 308):
                    self.send_response(status)
                    self.send_header("Location", location)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                body = json.dumps({"code": "abc123", "state": "idle"}).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            do_GET = do_POST = _answer

            def log_message(self, *a):
                pass

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


@pytest.mark.parametrize("status", [301, 302, 303, 307, 308])
def test_a_redirect_is_an_error_and_the_key_never_follows_it(boot, monkeypatch, status):
    other = _Server()
    first = _Server(status, other.url + "/elsewhere")
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", first.url)
        with pytest.raises(boot.urllib.error.HTTPError) as got:
            boot.request("POST", "/api/fleet/hello", {"machine": "cc-000000000000"})
        assert got.value.code == status
        assert [s["key"] for s in first.seen] == ["test-key"]
        assert other.seen == [], "the request, and the partner key with it, went to the redirect's address"
        with pytest.raises(boot.urllib.error.HTTPError):
            boot.request("GET", "/api/fleet/build/abc123.zip")
        assert other.seen == []
    finally:
        first.close()
        other.close()


def test_a_plain_answer_still_comes_back(boot, monkeypatch):
    server = _Server()
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        with boot.request("POST", "/api/fleet/hello", {"machine": "cc-000000000000"}) as r:
            assert json.loads(r.read())["code"] == "abc123"
        assert server.seen[0]["key"] == "test-key"
    finally:
        server.close()
