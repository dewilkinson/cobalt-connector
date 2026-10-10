# Cobalt Worker

Cobalt Worker lends a Windows machine's spare processor cores to the **Cobalt training fleet** of a
partner who holds a **partner key**. It is a small native Windows app with a tray icon: it runs the
fleet's jobs inside the limits you set (cores, memory, hours), at below-normal priority, and you can
pause it at any time.

A partner key is required. Without one the installer will not finish and the app will not run. Keys
are issued by whoever runs the fleet you are joining.

This repository holds the app shell only: the C++ app, the Python bootstrap, the installer script and
the build script. **The installer carries no worker code.** The worker code is downloaded from the
fleet server, by its version hash, only after your partner key has been accepted.

## Requirements

- Windows 10 (version 2004, build 19041) or newer, 64-bit
- At least 4 logical cores and 8 GB of memory, and about 20 GB free disk for cached market data
- An internet connection to the fleet server
- A partner key

No administrator rights are needed. Cobalt Worker installs for your Windows user only.

## Install

1. Download `CobaltWorkerSetup.exe` from the [Releases](../../releases) page, or build it yourself (see
   below).
2. Run it. The installer is **not code-signed yet**, so Windows SmartScreen says "Windows protected your
   PC" and "unknown publisher". Choose **More info**, then **Run anyway**.
3. Go through the pages:
   - **Licence, what it runs and what it holds**: the MIT licence and the privacy notes below.
   - **Install folder**: `%LOCALAPPDATA%\Programs\Cobalt Worker` by default.
   - **Your limits**: cores (all but two by default), memory (all but 4 GB), hours (empty means any
     hour, or for example `22:00-07:00`).
   - **Your partner key** (required): Next stays disabled until a key is entered, and pressing Next
     checks it with the fleet server. A refused key stays on the page. If the server cannot be reached
     you can continue offline: the key is stored and checked when the worker first connects, and the tray
     icon turns red if it is refused then.
   - **Anthropic API key** (optional): most people skip this; the button reads **Skip** while the box is
     empty. If you give one: it is used only for your own research questions and note replies, never for
     the fleet owner's work; everything it is used for is billed to your Anthropic account; it is stored
     encrypted for your Windows user only and sent only to Anthropic; create a key just for Cobalt in the
     Anthropic Console with a monthly spend limit; a Claude chat subscription (Pro or Max) cannot be used,
     only an API key.
   - **Starting**: "Start Cobalt Worker now" and "Start Cobalt Worker when I sign in to Windows".

### Unattended install

```powershell
CobaltWorkerSetup.exe /VERYSILENT /SUPPRESSMSGBOXES /DIR="C:\Cobalt Worker" /PARTNERKEYFILE=C:\path\partner.txt `
    [/ANTHROPICKEYFILE=C:\path\anthropic.txt] [/CORES=6] [/MEMORYGB=12] [/HOURS=22:00-07:00] `
    [/RELAY=https://...] [/STARTNOW=0|1] [/RUNATLOGON=0|1] [/SKIPKEYCHECK=1]
```

A `*FILE` parameter names a file holding just the key. `/PARTNERKEY=...` and `/ANTHROPICKEY=...` work as
well, but a key on the command line is written into Setup's log when `/LOG` is used, so prefer the file
forms. A silent install without a partner key installs nothing. `/RELAY=` points the worker at a fleet
server other than the default (`https://api.bluesec.ai`).

## How it works

```
CobaltWorker.exe (tray app)
  └─ python\python.exe -I bootstrap\cobalt_bootstrap.py --pull     (private embedded Python 3.12)
       1. POST /api/fleet/hello            partner key in X-Cobalt-Contributor; the answer names the code hash
       2. GET  /api/fleet/build/<hash>.zip  only if that build is not already here and verified
       3. checks it, unpacks it to %LOCALAPPDATA%\CobaltWorker\build\<hash>\
       4. runs the build's pull loop: asks for a job, runs it, posts the result back
```

- **The app** (`app/`, C++20, Win32, WinHTTP, no runtime to install) supervises one Python process. It
  starts it at below-normal priority with a minimal environment built from scratch: nothing of your own
  environment beyond what Windows needs, so no Python setting or model key of yours leaks in. The partner
  key and the optional Anthropic key are decrypted from their DPAPI files and passed through that
  process's environment only. The process and every process it starts run in a Windows job object that
  ends with the app, so Quit leaves nothing running. If it exits it is started again with backoff (2 s
  after a clean exit, doubling to 60 s while it keeps failing).
- **The bootstrap** (`bootstrap/cobalt_bootstrap.py`, standard library only) says hello to the fleet with
  your partner key. A refused key stops there: nothing is downloaded. Otherwise it downloads the build of
  the code hash the fleet runs, checks that the server labelled it with that hash, that no file in it
  points outside its folder, unpacks it into a temporary folder, recomputes the code hash from the
  unpacked files, and moves it into place only if the hash matches. It then runs that build's pull loop
  (`python -I -m src.training.contributor --pull`, with the build folder as the working directory). When
  the fleet moves to new code, the loop stops itself and the bootstrap fetches the new build the same way.
  If the server cannot be reached and a verified build is already here, it runs that one.
- **Pulling, not listening**: the worker asks the server for work. It opens no port on your machine and
  nothing is forwarded through your router.
- **Checking**: a share of the jobs a machine finishes is replayed on the fleet owner's own machine and the
  results compared. A replay is deterministic, so an honest machine always matches; one that does not
  stops being sent work.

### The tray icon

| Tray | Means |
|---|---|
| blue (ring) | idle: waiting for work, starting, or downloading the worker code |
| green (dot) | working: jobs in flight |
| amber (bars) | paused by you, or outside your hours |
| red (!) | a problem: no key, key refused, offline, benched, or the worker keeps stopping |

The window shows your limits (editable), the jobs in flight, today's totals and the log, with Pause /
Resume and "Run at logon".

## What it downloads and stores

It downloads:

- the worker code the fleet runs, by its version hash, and only with an accepted partner key;
- public market data only: price bars and the 15-second tape of futures markets, 0.5-1 GB per data file,
  downloaded once and kept.

Nothing of anyone's accounts, money, trades or keys is ever on the job queue, so none of it can reach your
machine. The fleet server sees your computer's name, its number of cores and memory, and your limits, and
shows the machine on its fleet page under your first name.

| Where | What |
|---|---|
| `%LOCALAPPDATA%\Programs\Cobalt Worker\CobaltWorker.exe` | the app |
| `...\python\` | a private Python 3.12 (python.org's embeddable build) and the worker's third-party libraries (`requirements.txt`, public PyPI packages) - never on PATH |
| `...\bootstrap\cobalt_bootstrap.py` | the bootstrap |
| `%LOCALAPPDATA%\CobaltWorker\` | `limits.json`, `contributor.json` (the machine's id and the server address), `partner.key` and `anthropic.key` (DPAPI-encrypted), `status.json`, `build\<hash>\` (the downloaded worker code), `cache\` (market data), `logs\` |
| `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\CobaltWorker` | only when "Run at logon" is on |

## Your keys

- Both keys are encrypted with **Windows DPAPI** for your Windows user only (user scope, with a per-key
  entropy value) and stored as `partner.key` and `anthropic.key` in `%LOCALAPPDATA%\CobaltWorker`. Another
  Windows user on the same machine, or a copy of the files on another machine, cannot decrypt them.
- The installer hands keys to the app through files in its own temporary folder, which are overwritten
  and deleted once read; no key is put on a command line unless you choose the `/PARTNERKEY=` form.
- No key is ever written to a log.
- The partner key is sent only to the fleet server; an Anthropic key is sent only to Anthropic.
- The fleet's owner can revoke a partner key at any time, which ends the machine's access at once.

## Uninstall

Windows Settings > Apps > Cobalt Worker > Uninstall, or silently:

```powershell
& "$env:LOCALAPPDATA\Programs\Cobalt Worker\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
```

It stops the app and the worker and removes the app folder, the private Python, the downloaded worker code,
the cached market data, the logs, both keys, the settings and the "Run at logon" entry. Nothing is left in
`%LOCALAPPDATA%\CobaltWorker` or the registry.

## Command line

```
CobaltWorker.exe [--background]                  the tray app (with its window, or tray only)
CobaltWorker.exe --check-key <key> [--relay URL] check a partner key: exit 0 accepted, 1 refused, 2 could not check
CobaltWorker.exe --check-key-file <file>         the same, the key read from a file (deleted after)
CobaltWorker.exe --set-keys <file>               store partner=/anthropic=/relay= lines with DPAPI (file wiped after)
CobaltWorker.exe --run-at-logon on|off
CobaltWorker.exe --quit                          stop the running app and its worker
CobaltWorker.exe --version
```

## Build from source

You need:

- Visual Studio 2026 (MSVC 14.51) with its bundled CMake and Ninja. If Visual Studio has no Windows SDK
  component, the build script downloads the SDK's official NuGet packages (`Microsoft.Windows.SDK.CPP` and
  `.x64`) into `.cache\winsdk` and builds against those, without administrator rights.
- [uv](https://docs.astral.sh/uv/), to install the third-party wheels into the embedded Python.
- [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`), in
  `%LOCALAPPDATA%\Programs\Inno Setup 6` by default.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 [-Clean] [-SkipInstaller] [-VsRoot <Visual Studio folder>] [-Iscc <path to ISCC.exe>]
```

It builds `CobaltWorker.exe` (CMake + Ninja, Release, `/W4 /WX`, static CRT: any warning fails the build),
downloads the Python 3.12 embeddable zip from python.org, installs `requirements.txt` into it (wheels only),
stages the bootstrap and checks it and the libraries with the staged Python, and compiles `setup.iss` into
`dist\CobaltWorkerSetup.exe` (about 85 MB). Everything downloaded or built goes to `.cache\`, `build\`,
`stage\` and `dist\`, which are gitignored.

### Unsigned installer

The installer and the app are not code-signed, so SmartScreen warns about an "unknown publisher". Signing
needs a code-signing certificate (an OV or EV certificate, or Azure Trusted Signing); with one, sign both
`CobaltWorker.exe` and `CobaltWorkerSetup.exe` (`signtool sign /fd sha256 /tr <timestamp-url> /td sha256 ...`,
or Inno Setup's `SignTool=` directive), and the warning goes away once the certificate has reputation.

## Licence

MIT - see [LICENSE](LICENSE). Copyright (c) 2026 Dave Wilkinson.

The licence covers Cobalt Worker itself (the app, the bootstrap, the installer and build scripts). The
worker code the bootstrap downloads with a partner key is not part of this repository and is not covered
by this licence. There is no warranty.
