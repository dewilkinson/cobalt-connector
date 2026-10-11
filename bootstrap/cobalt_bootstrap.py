# What leaves your machine (full list, from the code: README.md, section "What leaves your machine"):
# - To the Cobalt server (always HTTPS; a plain-http address is refused): a one-hour pass in X-Cobalt-Device on every
#   request (Cobalt Connector 1.3.0 gets it by signing with a key of its own that never leaves the machine; your
#   partner key was sent once, when the app registered that key, and the server keeps only SHA-256 hashes of the key
#   and of each pass - a 1.2.x app or an older server: the partner key in X-Cobalt-Contributor instead), the
#   machine id ("cc-" + 12 random hex), the
#   computer's name, the name you chose, cores, memory, your limits, versions, job results or error messages, and
#   model answers with token counts. Other partners see the machine only by its label (the name you chose, or
#   "<owner>'s machine N"), never the computer's name or the machine id. No request with a pass or a key follows a
#   redirect.
# - To Anthropic, only with model work on: your Anthropic key and your own question's text. Never to Cobalt. With
#   model work off the work process is never given the key.
# - To GitHub: a plain download request for an app update. No key, no id.
# - Never: your files, your environment, your Windows user name, your logs. Market data only comes down.
#   (A failed job's error text goes back, and a file path in it can include your user folder's name.)
"""Cobalt Connector bootstrap: fetch the worker code the fleet runs, verify it, and run it.

The installer ships no worker code. CobaltConnector.exe starts this script in the embedded Python:

    python.exe -I -X utf8 <install>\\bootstrap\\cobalt_bootstrap.py --pull

with a minimal environment that carries:

    COBALT_FLEET_URL     the fleet server (default https://api.bluesec.ai); only an https:// address is used
                         (or plain http to this machine, for tests) - any other is ignored for the default
    COBALT_TOKEN_FILE    Cobalt Connector 1.3.0: the token.json the app keeps fresh - a one-hour pass the app gets by
                         signing with the machine's own device key. Read again whenever it changes; sent in
                         X-Cobalt-Device. The work process is then never given the partner key.
    COBALT_CONTRIB_KEY   only when there is no token file (a 1.2.x app, or a server that does not offer passes yet):
                         the owner's partner key (decrypted from DPAPI by the app, never written to disk), sent in
                         X-Cobalt-Contributor
    COBALT_CONTRIB_HOME  %LOCALAPPDATA%\\CobaltConnector - settings, limits, status, cache, builds
    COBALT_APP_VERSION   the app's own version (e.g. 1.2.0), sent in the hello so the server knows it
    COBALT_MACHINE_NAME  the name the owner chose for this machine ("" = its computer name), sent in the hello

What it does, every time it is started:

1. Says hello to the fleet (POST /api/fleet/hello, with the pass - or, from an older app, the partner key),
   with the app's version and the machine's chosen name. The answer names the code hash the fleet runs and
   the hash of its pull loop (`loop`). A refused key or a removed machine stops here: nothing is downloaded.
   A pass the server no longer takes leaves `token.stale` beside token.json, so the app renews it at once,
   and is treated as an outage meanwhile. An app the fleet calls too old to take work ("outdated") stops
   here too: status.json says so, with the fleet's reason, and it asks again later.
2. If build\\<hash>\\ is not already here and verified, or its pull loop hashes differently from the hello's
   `loop`, downloads GET /api/fleet/build/<hash>.zip (same header), checks that the server labelled it with that hash (X-Cobalt-Code), that no file in it
   points outside its folder, unpacks it into a temporary folder, and recomputes the code hash from
   the unpacked files. Only a build whose hash matches is moved into place; older builds are removed.
3. Runs that build's pull loop in this process - the equivalent of
   `python -I -m src.training.contributor --pull` with the build folder as the working directory.
   When the fleet moves to new code the loop stops itself, the app starts this script again, and the
   new build is fetched by step 2.

If the fleet cannot be reached and a verified build is already here, the newest one is run (the loop
waits for the server itself). This script uses the Python standard library only and holds nothing of
the worker code. It never prints, logs or stores the partner key or a pass.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import runpy
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

VERSION = "bootstrap-3"
DEFAULT_RELAY = "https://api.bluesec.ai"
CONTRIB_HEADER = "X-Cobalt-Contributor"
DEVICE_HEADER = "X-Cobalt-Device"          # Cobalt Connector 1.3.0's one-hour pass
REASON_HEADER = "X-Cobalt-Reason"          # why the server refused a request (a code, never a secret)
STALE_NAME = "token.stale"                 # left beside token.json when the pass was refused: the app renews it
CODE_HEADER = "X-Cobalt-Code"
HASH_RE = re.compile(r"^[0-9a-f]{4,40}$")
APP_VERSION_RE = re.compile(r"^\d{1,6}\.\d{1,6}\.\d{1,6}$")
NAME_RE = re.compile(r"^[A-Za-z0-9 ._-]{0,32}$")
LOOP_MODULE = "src.training.contributor"
MARKER = ".cobalt-build.json"
STATUS_EVERY_S = 10.0          # the app treats a status older than 30 s as "not reporting"
REFUSED_WAIT_S = 300.0         # a refused key is asked about again every 5 minutes
OUTDATED_WAIT_S = 600.0        # an app too old to take work asks again every 10 minutes (it updates meanwhile)
BUILD_ATTEMPTS = 4


class Refused(Exception):
    """The fleet refused the partner key (401 or 403)."""


class BadBuild(Exception):
    """A downloaded build failed its check (wrong label, unsafe paths, or its files hash differently)."""


class Moved(Exception):
    """The fleet moved to another code hash between hello and download (409)."""


class Outdated(Exception):
    """The fleet sends this app no work until it is updated (hello state "outdated"); the text says why."""


class NotSignedIn(Exception):
    """Cobalt Connector 1.3.0 has no pass the fleet accepts yet (it is signing in, or renewing an expired one).
    Nothing is sent without one; it is treated as an outage until the app writes a new token.json."""


def log(text: str) -> None:
    print(time.strftime("%Y-%m-%d %H:%M:%S") + " bootstrap: " + text, flush=True)


# ---------------------------------------------------------------- where things live


def home() -> Path:
    h = os.environ.get("COBALT_CONTRIB_HOME") or os.path.join(os.environ.get("LOCALAPPDATA", "."), "CobaltConnector")
    p = Path(h)
    p.mkdir(parents=True, exist_ok=True)
    return p


def builds_dir() -> Path:
    p = home() / "build"
    p.mkdir(parents=True, exist_ok=True)
    return p


LOOPBACK_RE = re.compile(r"^http://(127\.0\.0\.1|localhost):\d{1,5}$")
_refused_relay_logged = False


def secure_relay(url: str) -> bool:
    """A server address the partner key may be sent to: https://, or plain http only to this machine
    (http://127.0.0.1:<port> or http://localhost:<port>, which the tests use). Anything else would send
    the key in clear text."""
    u = (url or "").strip().rstrip("/")
    if u[:8].lower() == "https://" and len(u) > 8:
        return True
    return bool(LOOPBACK_RE.match(u))


def relay() -> str:
    global _refused_relay_logged
    r = (os.environ.get("COBALT_FLEET_URL") or "").strip().rstrip("/")
    if not r:
        return DEFAULT_RELAY
    if not secure_relay(r):
        if not _refused_relay_logged:
            _refused_relay_logged = True
            log(f"the server address {r} is not secure: the connector only talks to a server over https, "
                f"so it uses {DEFAULT_RELAY} instead")
        return DEFAULT_RELAY
    return r


def partner_key() -> str:
    return (os.environ.get("COBALT_CONTRIB_KEY") or "").strip()


class TokenFile:
    """The pass Cobalt Connector writes to token.json, read again only when the file changes (one stat per request,
    so a renewal reaches the running loop without a restart). The pass is never printed or logged."""

    def __init__(self, path: str):
        self.path = Path(path)
        self._stamp: tuple | None = None
        self._token = ""

    def current(self) -> str:
        try:
            st = self.path.stat()
        except OSError:
            self._stamp, self._token = None, ""
            return ""
        stamp = (st.st_mtime_ns, st.st_size)
        if stamp != self._stamp:
            try:
                got = json.loads(self.path.read_text(encoding="utf-8-sig"))
                self._token = str(got.get("token") or "").strip() if isinstance(got, dict) else ""
                self._stamp = stamp
            except (OSError, ValueError):            # caught mid-write: read again on the next request
                self._token = ""
        return self._token

    def mark_stale(self) -> None:
        """Tell the app the pass it wrote was refused: it renews it at once (at most once a minute)."""
        try:
            (self.path.parent / STALE_NAME).touch()
        except OSError as e:
            log(f"could not tell Cobalt Connector its sign-in was refused ({e})")


_token_file: TokenFile | None = None


def token_file() -> TokenFile | None:
    """The app's token.json when it gave one (COBALT_TOKEN_FILE), else None: then the partner key is used."""
    global _token_file
    path = (os.environ.get("COBALT_TOKEN_FILE") or "").strip()
    if not path:
        return None
    if _token_file is None or str(_token_file.path) != str(Path(path)):
        _token_file = TokenFile(path)
    return _token_file


def reset_credentials() -> None:
    """Forget the token file read so far (the tests change the environment between cases)."""
    global _token_file
    _token_file = None


def read_json(path: Path) -> dict:
    try:
        v = json.loads(path.read_text(encoding="utf-8-sig"))
        return v if isinstance(v, dict) else {}
    except (OSError, ValueError):
        return {}


def write_json(path: Path, obj: dict) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(obj, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def machine_id() -> str:
    """The id the app made at install (contributor.json "machine"); made here only if it is missing,
    in the same form: "cc-" and 12 random hex digits (nothing of the computer's name). An id made by an
    older version (the computer's name and eight hex digits) is kept, so the machine keeps its history."""
    path = home() / "contributor.json"
    cfg = read_json(path)
    mid = str(cfg.get("machine") or "")
    if not mid:
        mid = f"cc-{secrets.token_hex(6)}"
        cfg["machine"] = mid
        write_json(path, cfg)
    return mid


def write_status(state: str, error: str = "", code: str = "", why: str = "") -> None:
    """status.json in the shape the app reads (the pull loop overwrites it once it runs)."""
    try:
        write_json(home() / "status.json", {
            "state": state, "machine": machine_id(), "relay": relay(), "in_flight": 0, "jobs": [],
            "last_error": error, "code": code, "why": why, "version": VERSION, "at": time.time()})
    except OSError:
        pass


def wait(seconds: float, state: str, error: str = "", code: str = "", why: str = "") -> None:
    """Sleep, keeping status.json fresh so the tray does not say "not reporting"."""
    end = time.monotonic() + seconds
    while True:
        write_status(state, error, code, why)
        left = end - time.monotonic()
        if left <= 0:
            return
        time.sleep(min(STATUS_EVERY_S, left))


# ---------------------------------------------------------------- the fleet


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Every request here carries the partner key, and urllib's own redirect handler would resend that header to
    whatever address a 3xx names. This one follows nothing: the 3xx comes back as an HTTPError, as any other refusal."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


# the only opener the fleet's requests use: no redirect is ever followed, so the key goes to COBALT_FLEET_URL alone
_OPENER = urllib.request.build_opener(_NoRedirect)


def _open(method: str, path: str, data: bytes | None, timeout: float, header: str, value: str):
    req = urllib.request.Request(relay() + path, data=data, method=method)
    req.add_header(header, value)
    req.add_header("User-Agent", "CobaltConnector/" + VERSION)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    return _OPENER.open(req, timeout=timeout)


def request(method: str, path: str, body: dict | None = None, timeout: float = 30.0):
    """A request to the fleet with the app's pass (X-Cobalt-Device), or - only when the app gave no token file - the
    partner key. A redirect is never followed (it raises HTTPError with its 3xx code), so the credential is sent to
    the fleet's own address and nowhere else.

    With a token file: no pass yet means nothing is sent (NotSignedIn). A 401 is retried once when the app has written
    a new pass meanwhile; otherwise `token.stale` asks the app to renew it and NotSignedIn is raised."""
    data = json.dumps(body).encode("utf-8") if body is not None else None
    tf = token_file()
    if tf is None:
        return _open(method, path, data, timeout, CONTRIB_HEADER, partner_key())
    sent = tf.current()
    if not sent:
        raise NotSignedIn("Cobalt Connector has not signed this machine in yet")
    try:
        return _open(method, path, data, timeout, DEVICE_HEADER, sent)
    except urllib.error.HTTPError as e:
        if e.code != 401:
            raise
        refused = e
    renewed = tf.current()
    if renewed and renewed != sent:
        try:
            return _open(method, path, data, timeout, DEVICE_HEADER, renewed)
        except urllib.error.HTTPError as e:
            if e.code != 401:
                raise
            refused = e
    reason = (refused.headers.get(REASON_HEADER) or "").strip()[:40] if refused.headers else ""
    refused.close()
    tf.mark_stale()
    raise NotSignedIn("the server did not accept this machine's sign-in"
                      + (f" ({reason})" if reason else "") + "; Cobalt Connector is renewing it")


def why(e: urllib.error.HTTPError) -> str:
    try:
        detail = json.loads(e.read().decode("utf-8", "replace")).get("detail")
        return f"{e.code} {detail}" if detail else str(e.code)
    except Exception:
        return str(e.code)


def app_version() -> str | None:
    """The app's version from its environment, when it is a plain N.N.N."""
    v = (os.environ.get("COBALT_APP_VERSION") or "").strip()
    return v if APP_VERSION_RE.match(v) else None


def machine_name() -> str | None:
    """The owner's chosen name for this machine: "" clears it (the server then shows the computer's name);
    None (an app too old to set it, or a name that is not allowed) leaves the server's record alone."""
    v = os.environ.get("COBALT_MACHINE_NAME")
    if v is None:
        return None
    v = v.strip()
    return v if NAME_RE.match(v) else None


def hello(current: str) -> dict:
    body = {"machine": machine_id(), "host": socket.gethostname(), "cores": os.cpu_count() or 1,
            "code": current, "version": VERSION, "paused": True}
    if (v := app_version()) is not None:
        body["app_version"] = v
    if (n := machine_name()) is not None:
        body["name"] = n
    try:
        with request("POST", "/api/fleet/hello", body) as r:
            reply = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise Refused(why(e)) from None
        raise
    if not isinstance(reply, dict) or not HASH_RE.match(str(reply.get("code") or "")):
        raise ValueError("the fleet's hello answer names no code hash")
    if reply.get("state") == "outdated":
        # the fleet gives this machine no work: say so (the app shows "Update needed" and tells the owner)
        # rather than start a loop that would only sit idle
        raise Outdated(str(reply.get("why") or "")[:300])
    return reply


def download(want: str, dest: Path) -> str:
    """GET /api/fleet/build/<hash>.zip into dest; returns the zip's SHA-256 (for the record)."""
    delay = 2.0
    for attempt in range(1, BUILD_ATTEMPTS + 1):
        try:
            with request("GET", f"/api/fleet/build/{want}.zip", timeout=900.0) as r:
                labelled = (r.headers.get(CODE_HEADER) or "").strip()
                if labelled and labelled != want:
                    raise BadBuild(f"the server labelled the build {labelled}, not {want}")
                h = hashlib.sha256()
                with open(dest, "wb") as f:
                    while chunk := r.read(1 << 20):
                        h.update(chunk)
                        f.write(chunk)
                return h.hexdigest()
        except urllib.error.HTTPError as e:
            if e.code in (401, 403):
                raise Refused(why(e)) from None
            if e.code == 409:
                raise Moved(why(e)) from None
            if e.code < 500 or attempt == BUILD_ATTEMPTS:
                raise
            log(f"the build download failed ({why(e)}); trying again in {delay:.0f} s")
        except (urllib.error.URLError, OSError) as e:
            if attempt == BUILD_ATTEMPTS:
                raise
            log(f"the build download failed ({e}); trying again in {delay:.0f} s")
        time.sleep(delay)
        delay *= 2
    raise RuntimeError("unreachable")


def safe_names(z: zipfile.ZipFile) -> bool:
    for n in z.namelist():
        p = n.replace("\\", "/")
        if p.startswith("/") or ":" in p or any(part == ".." for part in p.split("/")):
            return False
    return True


def code_hash_of(folder: Path) -> str:
    """The code hash, computed by the build's own definition from its unpacked files, in a separate
    process so nothing of the build stays loaded here if it does not match."""
    probe = ("import sys; sys.path.insert(0, sys.argv[1]); "
             "from src.training import worker as wk; print(wk.code_hash())")
    out = subprocess.run([sys.executable, "-I", "-X", "utf8", "-c", probe, str(folder)], cwd=str(folder),
                         capture_output=True, text=True, timeout=600, creationflags=subprocess.CREATE_NO_WINDOW)
    if out.returncode != 0:
        raise BadBuild("the build's code hash could not be computed: " + (out.stderr or "").strip()[-400:])
    return out.stdout.strip().splitlines()[-1] if out.stdout.strip() else ""


def loop_file(folder: Path) -> Path:
    return folder / "src" / "training" / "contributor.py"


def verified(folder: Path, want: str) -> bool:
    m = read_json(folder / MARKER)
    return m.get("code") == want and loop_file(folder).is_file()


def loop_of(folder: Path) -> str:
    """The pull loop's own hash, as the server's hello names it (`loop`): the first 16 hex of the sha256 of
    contributor.py with its line endings as LF. It is not part of the code hash, so a build of the same hash can hold
    an older loop."""
    try:
        raw = loop_file(folder).read_bytes().replace(b"\r\n", b"\n")
    except OSError:
        return ""
    return hashlib.sha256(raw).hexdigest()[:16]


def knows_tokens(folder: Path) -> bool:
    """Whether this build's pull loop can sign in with the app's token file (an older one knows the partner key
    only, which a 1.3.0 app no longer gives the work process)."""
    try:
        return b"COBALT_TOKEN_FILE" in loop_file(folder).read_bytes()
    except OSError:
        return False


def fetch(want: str, loop: str | None = None) -> Path:
    """build\\<want>\\, downloaded and verified if it is not already here - or if its pull loop is not the one the
    fleet's hello named (`loop`; an older server names none, and then the held build stands)."""
    base = builds_dir()
    final = base / want
    if verified(final, want) and (not loop or loop_of(final) == loop):
        return final
    if verified(final, want):
        log(f"the build {want} here has an older pull loop; downloading it again")
    write_status("code", code=want)
    log(f"downloading the worker build {want}")
    work = Path(tempfile.mkdtemp(prefix=f".tmp-{want}-", dir=base))
    try:
        zpath = work / "build.zip"
        sha = download(want, zpath)
        unpacked = work / "build"
        try:
            with zipfile.ZipFile(zpath) as z:
                if not safe_names(z):
                    raise BadBuild("the build names files outside its folder: nothing unpacked")
                z.extractall(unpacked)
        except zipfile.BadZipFile as e:
            raise BadBuild(f"the download is not a zip ({e})") from None
        zpath.unlink()
        got = code_hash_of(unpacked)
        if got != want:
            raise BadBuild(f"the build's files hash to {got or 'nothing'}, not {want}: discarded")
        write_json(unpacked / MARKER, {"code": want, "zip_sha256": sha, "verified_at": time.time(),
                                       "relay": relay(), "loop": loop_of(unpacked)})
        if final.exists():
            shutil.rmtree(final, ignore_errors=True)
        os.replace(unpacked, final)
        log(f"the build {want} is verified and in place")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for old in base.iterdir():                  # one build is kept: the one the fleet runs
        if old.is_dir() and old.name != want:
            shutil.rmtree(old, ignore_errors=True)
    return final


def newest_verified() -> Path | None:
    """The newest verified build here; with a token file, only one whose pull loop can use it."""
    best: tuple[float, Path] | None = None
    tokens = token_file() is not None
    for d in builds_dir().iterdir():
        if d.is_dir() and HASH_RE.match(d.name) and verified(d, d.name) and (not tokens or knows_tokens(d)):
            t = float(read_json(d / MARKER).get("verified_at") or 0)
            if best is None or t > best[0]:
                best = (t, d)
    return best[1] if best else None


# ---------------------------------------------------------------- running the loop


def run_loop(folder: Path, args: list[str]) -> None:
    """`python -I -m src.training.contributor <args>` with `folder` as the working directory, in this
    process (the embedded Python's ._pth fixes sys.path, so the build folder is put on it here; its
    process pools inherit sys.path and the -I flag)."""
    log(f"starting the pull loop from build {folder.name}")
    os.chdir(folder)
    sys.path.insert(0, str(folder))
    sys.argv = [str(folder / "src" / "training" / "contributor.py"), *args]
    runpy.run_module(LOOP_MODULE, run_name="__main__", alter_sys=True)


def prepare() -> Path:
    """The verified build to run: what the fleet runs now, or (offline) the newest one already here."""
    backoff = 5.0
    while True:
        have = newest_verified()
        try:
            reply = hello(have.name if have else "")
            want = str(reply["code"])
            loop = str(reply.get("loop") or "").strip().lower()
            try:
                return fetch(want, loop=loop if HASH_RE.match(loop) else None)
            except Moved as e:
                log(f"the fleet moved to new code while downloading ({e}); asking again")
                continue
        except Outdated as e:
            log("the fleet says this Cobalt Connector is too old to take work until it is updated; "
                f"asking again in {OUTDATED_WAIT_S / 60:.0f} minutes")
            wait(OUTDATED_WAIT_S, "outdated", why=str(e))
            continue
        except NotSignedIn as e:
            if have is not None:
                log(f"{e}; running the build already here, {have.name}")
                return have
            log(f"{e}; trying again in {backoff:.0f} s")
            wait(backoff, "offline", "Cobalt Connector is signing this machine in to the Cobalt server")
            backoff = min(60.0, backoff * 2)
        except Refused as e:
            log(f"the fleet refused this machine ({e}); nothing is downloaded")
            wait(REFUSED_WAIT_S, "refused", f"the partner key was refused: {e}")
            continue
        except BadBuild as e:
            log(f"the downloaded build failed its check and was discarded ({e})")
            if have is not None:
                log(f"running the verified build already here, {have.name}")
                return have
            wait(backoff, "offline", f"the downloaded worker build failed its check: {e}")
            backoff = min(60.0, backoff * 2)
        except Exception as e:
            if have is not None:
                log(f"the fleet could not be reached ({e}); running the build already here, {have.name}")
                return have
            log(f"the fleet could not be reached and no build is here yet ({e}); trying again in {backoff:.0f} s")
            wait(backoff, "offline", f"the Cobalt server could not be reached: {e}")
            backoff = min(60.0, backoff * 2)


def main(argv: list[str]) -> int:
    if "--version" in argv:
        print(VERSION)
        return 0
    if "--pull" not in argv:
        print(__doc__)
        return 1
    if token_file() is None and not partner_key():
        log("no sign-in was given (COBALT_TOKEN_FILE or COBALT_CONTRIB_KEY): start Cobalt Connector from its app")
        write_status("refused", "no partner key is stored")
        return 2
    write_status("starting")
    folder = prepare()
    run_loop(folder, ["--pull"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
