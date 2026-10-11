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
    monkeypatch.setattr(boot, "fetch", lambda want, loop=None: tmp_path / want)
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


@pytest.mark.parametrize("url", [
    "http://api.bluesec.ai",
    "http://192.168.1.145:47101",
    "http://127.0.0.2:8000",
    "http://localhost.evil.com:80",
    "http://127.0.0.1:80@evil.com",
    "api.bluesec.ai",
    "ftp://api.bluesec.ai",
    "https://",
])
def test_a_plain_http_server_address_is_never_used(boot, monkeypatch, url):
    monkeypatch.setenv("COBALT_FLEET_URL", url)
    assert not boot.secure_relay(url)
    assert boot.relay() == boot.DEFAULT_RELAY


@pytest.mark.parametrize("url", [
    "https://api.bluesec.ai",
    "https://api.bluesec.ai/",
    "HTTPS://fleet.example.com:8443",
    "http://127.0.0.1:8000",
    "http://localhost:47101/",
])
def test_an_https_or_loopback_server_address_is_used(boot, monkeypatch, url):
    monkeypatch.setenv("COBALT_FLEET_URL", url)
    assert boot.secure_relay(url)
    assert boot.relay() == url.rstrip("/")


def test_no_server_address_is_the_default(boot, monkeypatch):
    monkeypatch.delenv("COBALT_FLEET_URL", raising=False)
    assert boot.relay() == boot.DEFAULT_RELAY


def test_a_refused_address_is_logged_once_without_the_key(boot, monkeypatch, capsys):
    monkeypatch.setenv("COBALT_FLEET_URL", "http://fleet.example.com")
    assert boot.relay() == boot.DEFAULT_RELAY
    assert boot.relay() == boot.DEFAULT_RELAY
    out = capsys.readouterr().out
    assert out.count("is not secure") == 1
    assert "test-key" not in out


# ---------------------------------------------------------------- 1.3.0: the device token instead of the partner key


class _FleetServer:
    """A local fleet stand-in: `answer(path, headers)` returns (status, body dict, extra headers); every request's
    path and credential headers are recorded."""

    def __init__(self, answer):
        seen = self.seen = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def _answer(self):
                self.rfile.read(int(self.headers.get("Content-Length") or 0))
                seen.append({"path": self.path, "key": self.headers.get("X-Cobalt-Contributor"),
                             "token": self.headers.get("X-Cobalt-Device")})
                status, body, extra = answer(self.path, self.headers)
                raw = json.dumps(body).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                for k, v in (extra or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(raw)

            do_GET = do_POST = _answer

            def log_message(self, *a):
                pass

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def _write_token(path: Path, token: str) -> None:
    """token.json as the app writes it. A rewrite inside the same clock tick is still seen as a change."""
    import os
    old = path.stat().st_mtime_ns if path.exists() else 0
    path.write_text(json.dumps({"token": token, "expires": 4102444800, "device": "dv_0123456789abcdef",
                                "machine": "cc-000000000000"}), encoding="utf-8")
    if path.stat().st_mtime_ns <= old:
        os.utime(path, ns=(old + 1_000_000, old + 1_000_000))


def _held_build(boot, tmp_path, code: str, token_aware: bool = True, loop_text: str | None = None) -> Path:
    """A verified build already here; its pull loop knows the token file or only the partner key."""
    folder = tmp_path / "build" / code
    (folder / "src" / "training").mkdir(parents=True, exist_ok=True)
    text = loop_text if loop_text is not None else (
        "TOKEN = 'COBALT_TOKEN_FILE'\n" if token_aware else "KEY = 'COBALT_CONTRIB_KEY'\n")
    (folder / "src" / "training" / "contributor.py").write_bytes(text.encode("utf-8"))
    (folder / boot.MARKER).write_text(json.dumps({"code": code, "verified_at": 1.0}), encoding="utf-8")
    return folder


@pytest.fixture
def token_boot(boot, monkeypatch, tmp_path):
    """The bootstrap as Cobalt Connector 1.3.0 starts it: a token file, and no partner key in its environment."""
    monkeypatch.delenv("COBALT_CONTRIB_KEY", raising=False)
    path = tmp_path / "token.json"
    monkeypatch.setenv("COBALT_TOKEN_FILE", str(path))
    boot.reset_credentials()
    return boot, path


def test_the_token_is_sent_and_the_partner_key_is_not(token_boot, monkeypatch):
    boot, path = token_boot
    _write_token(path, "cdt_first")
    server = _FleetServer(lambda p, h: (200, {"code": "abc123", "state": "idle", "loop": "0" * 16}, {}))
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        with boot.request("POST", "/api/fleet/hello", {"machine": "cc-000000000000"}) as r:
            assert json.loads(r.read())["code"] == "abc123"
        assert server.seen == [{"path": "/api/fleet/hello", "key": None, "token": "cdt_first"}]
    finally:
        server.close()


def test_a_new_token_file_is_read_on_the_next_request(token_boot, monkeypatch):
    boot, path = token_boot
    _write_token(path, "cdt_first")
    server = _FleetServer(lambda p, h: (200, {"code": "abc123"}, {}))
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        boot.request("GET", "/api/fleet/build/abc123.zip").close()
        _write_token(path, "cdt_second")
        boot.request("GET", "/api/fleet/build/abc123.zip").close()
        assert [s["token"] for s in server.seen] == ["cdt_first", "cdt_second"]
    finally:
        server.close()


def test_a_401_is_retried_once_when_the_app_has_written_a_new_token(token_boot, monkeypatch, tmp_path):
    boot, path = token_boot
    _write_token(path, "cdt_old")

    def answer(p, h):
        if h.get("X-Cobalt-Device") == "cdt_old":
            _write_token(path, "cdt_new")          # the app refreshed meanwhile
            return 401, {"detail": "This sign-in has expired."}, {"X-Cobalt-Reason": "token_expired"}
        return 200, {"code": "abc123", "state": "idle"}, {}

    server = _FleetServer(answer)
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        with boot.request("POST", "/api/fleet/hello", {"machine": "cc-000000000000"}) as r:
            assert json.loads(r.read())["code"] == "abc123"
        assert [s["token"] for s in server.seen] == ["cdt_old", "cdt_new"]
        assert not (tmp_path / "token.stale").exists()
    finally:
        server.close()


def test_a_401_with_no_new_token_leaves_token_stale_and_is_not_a_refused_key(token_boot, monkeypatch, tmp_path, capsys):
    boot, path = token_boot
    _write_token(path, "cdt_old")
    server = _FleetServer(lambda p, h: (401, {"detail": "This sign-in has expired."},
                                        {"X-Cobalt-Reason": "token_expired"}))
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        with pytest.raises(boot.NotSignedIn):
            boot.hello("")
        assert len(server.seen) == 1, "no retry with the same token"
        assert (tmp_path / "token.stale").exists(), "the app is told to refresh at once"
        assert "cdt_old" not in capsys.readouterr().out
    finally:
        server.close()


def test_a_stale_sign_in_runs_the_build_already_here(token_boot, monkeypatch, tmp_path):
    boot, path = token_boot
    _write_token(path, "cdt_old")
    held = _held_build(boot, tmp_path, "abc123")
    server = _FleetServer(lambda p, h: (401, {"detail": "This sign-in has expired."},
                                        {"X-Cobalt-Reason": "token_expired"}))
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        assert boot.prepare() == held
    finally:
        server.close()


def test_a_removed_machine_is_refused(token_boot, monkeypatch):
    boot, path = token_boot
    _write_token(path, "cdt_x")
    server = _FleetServer(lambda p, h: (403, {"detail": "Cobalt's owner has removed this machine from the fleet."},
                                        {"X-Cobalt-Reason": "device_revoked"}))
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        with pytest.raises(boot.Refused) as got:
            boot.hello("")
        assert "removed this machine" in str(got.value)
    finally:
        server.close()


def test_no_token_yet_sends_nothing_and_runs_the_build_already_here(token_boot, monkeypatch, tmp_path):
    boot, path = token_boot                         # no token.json: the app could not sign in yet
    server = _FleetServer(lambda p, h: (200, {"code": "abc123"}, {}))
    held = _held_build(boot, tmp_path, "abc123")
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        with pytest.raises(boot.NotSignedIn):
            boot.request("POST", "/api/fleet/hello", {})
        assert server.seen == [], "nothing goes out without a credential"
        assert boot.prepare() == held
    finally:
        server.close()


def test_offline_with_a_token_file_a_build_whose_loop_knows_no_tokens_is_not_run(token_boot, monkeypatch, tmp_path):
    boot, path = token_boot
    _held_build(boot, tmp_path, "abc123", token_aware=False)
    assert boot.newest_verified() is None
    monkeypatch.delenv("COBALT_TOKEN_FILE")
    monkeypatch.setenv("COBALT_CONTRIB_KEY", "test-key")
    boot.reset_credentials()
    assert boot.newest_verified() is not None, "with the partner key (a 1.2.x app, or an older server) it runs"


def test_with_only_the_partner_key_it_is_sent_as_before(boot, monkeypatch):
    monkeypatch.delenv("COBALT_TOKEN_FILE", raising=False)
    boot.reset_credentials()
    server = _FleetServer(lambda p, h: (200, {"code": "abc123", "state": "idle"}, {}))
    try:
        monkeypatch.setenv("COBALT_FLEET_URL", server.url)
        assert boot.hello("")["code"] == "abc123"
        assert server.seen == [{"path": "/api/fleet/hello", "key": "test-key", "token": None}]
    finally:
        server.close()


def test_main_needs_a_token_file_or_a_partner_key(boot, monkeypatch, tmp_path):
    monkeypatch.delenv("COBALT_CONTRIB_KEY", raising=False)
    monkeypatch.delenv("COBALT_TOKEN_FILE", raising=False)
    boot.reset_credentials()
    assert boot.main(["--pull"]) == 2
    monkeypatch.setenv("COBALT_TOKEN_FILE", str(tmp_path / "token.json"))
    boot.reset_credentials()
    started = []
    monkeypatch.setattr(boot, "prepare", lambda: tmp_path)
    monkeypatch.setattr(boot, "run_loop", lambda folder, args: started.append(folder))
    assert boot.main(["--pull"]) == 0 and started == [tmp_path]


# ---------------------------------------------------------------- the pull loop's own hash (`loop` in the hello)


def test_the_loop_hash_is_the_servers(boot, tmp_path):
    import hashlib
    folder = _held_build(boot, tmp_path, "abc123", loop_text="a = 1\r\nb = 2\r\n")
    assert boot.loop_of(folder) == hashlib.sha256(b"a = 1\nb = 2\n").hexdigest()[:16]


def test_a_held_build_whose_loop_differs_is_fetched_again(boot, monkeypatch, tmp_path):
    folder = _held_build(boot, tmp_path, "abc123")
    got = []

    def download(want, dest):
        got.append(want)
        raise boot.BadBuild("stop here")

    monkeypatch.setattr(boot, "download", download)
    with pytest.raises(boot.BadBuild):
        boot.fetch("abc123", loop="ffffffffffffffff")
    assert got == ["abc123"]
    assert boot.fetch("abc123", loop=boot.loop_of(folder)) == folder, "the same loop: nothing is downloaded"
    assert boot.fetch("abc123") == folder, "no loop in the hello (an older server): the held build stands"
    assert got == ["abc123"]


def test_prepare_passes_the_hellos_loop_on(boot, monkeypatch, tmp_path):
    monkeypatch.setattr(boot, "request", lambda *a, **k: _Reply({"code": "abc123", "state": "idle", "loop": "1" * 16}))
    seen = []
    monkeypatch.setattr(boot, "fetch", lambda want, loop=None: seen.append((want, loop)) or tmp_path)
    assert boot.prepare() == tmp_path
    assert seen == [("abc123", "1" * 16)]
