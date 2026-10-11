# Cobalt Connector

Cobalt Connector lets a **Cobalt partner** use Cobalt with **their own Anthropic API key, safely**. Your research
questions, note replies and Ask about Cobalt questions are answered on your own machine, on your own key. **The key
is never sent to Cobalt**: it is stored encrypted for your Windows user and sent only to Anthropic. Only the answers
and their token counts (or, when one fails, its error message) go back to Cobalt. Exactly what does leave your machine, and where it goes, is listed in
[What leaves your machine](#what-leaves-your-machine).

**Optional: contribute cores.** The app can also lend your machine's spare processor cores to the **Cobalt training
fleet**. It runs the fleet's jobs inside the limits you set (cores, memory, hours), at below-normal priority, and you can
pause it at any time from the tray icon.

A partner key is required. Without one the installer will not finish and the app will not run. Keys
are issued by whoever runs the fleet you are joining.

This repository holds the app shell only: the C++ app, the Python bootstrap, the installer script and
the build script. **The installer carries no worker code.** The worker code is downloaded from the
fleet server, by its version hash, only after your partner key has been accepted.

Cobalt Connector was called **Cobalt Worker** up to version 1.1.0. Installing 1.2.0 over it upgrades it in place
(see [Upgrading from Cobalt Worker](#upgrading-from-cobalt-worker)).

## Requirements

- Windows 10 (version 2004, build 19041) or newer, 64-bit
- At least 4 logical cores and 8 GB of memory, and about 20 GB free disk for cached market data
- An internet connection to the fleet server
- A partner key

No administrator rights are needed. Cobalt Connector installs for your Windows user only.

## Install

1. Download `CobaltConnectorSetup.exe` from the [Releases](https://github.com/dewilkinson/cobalt-connector/releases/latest)
   page, or build it yourself (see below).
2. Run it. The installer is **not code-signed yet**, so Windows SmartScreen says "Windows protected your
   PC" and "unknown publisher". Choose **More info**, then **Run anyway**.
3. Go through the pages:
   - **Licence, what it runs and what it holds**: the MIT licence and the privacy notes below.
   - **Install folder**: `%LOCALAPPDATA%\Programs\Cobalt Connector` by default.
   - **Your limits**: cores (all but two by default), memory (all but 4 GB), hours (empty means any
     hour, or for example `22:00-07:00`), and optionally the **name** Cobalt shows for this machine (empty
     means other partners see it as "<your name>'s machine 1"; you and the fleet's owner see the computer's name).
   - **Your partner key** (required): Next stays disabled until a key is entered, and pressing Next
     checks it with the fleet server. A refused key stays on the page. If the server cannot be reached
     you can continue offline: the key is stored and checked when the app first connects, and the tray
     icon turns red if it is refused then.
   - **Anthropic API key** (optional): most people skip this; the button reads **Skip** while the box is
     empty. If you give one: it is used only for your own research questions and note replies, never for
     the fleet owner's work; everything it is used for is billed to your Anthropic account; it is stored
     encrypted for your Windows user only and sent only to Anthropic; create a key just for Cobalt in the
     Anthropic Console with a monthly spend limit; a Claude chat subscription (Pro or Max) cannot be used,
     only an API key.
   - **Starting**: "Start Cobalt Connector now" and "Start Cobalt Connector when I sign in to Windows".

**Running the installer again** on a machine that already has a partner key keeps everything: the limits page is
skipped and your limits stay as they are, and the key pages are optional (leave a box empty to keep the stored key).
Paste a new partner key there if yours was refused.

### Unattended install

```powershell
CobaltConnectorSetup.exe /VERYSILENT /SUPPRESSMSGBOXES /DIR="C:\Cobalt Connector" /PARTNERKEYFILE=C:\path\partner.txt `
    [/ANTHROPICKEYFILE=C:\path\anthropic.txt] [/CORES=6] [/MEMORYGB=12] [/HOURS=22:00-07:00] `
    [/MACHINENAME="Gaming PC"] [/RELAY=https://...] [/STARTNOW=0|1] [/RUNATLOGON=0|1] [/SKIPKEYCHECK=1]
```

A `*FILE` parameter names a file holding just the key. `/PARTNERKEY=...` and `/ANTHROPICKEY=...` work as
well, but a key on the command line is written into Setup's log when `/LOG` is used, so prefer the file
forms. A silent install without a partner key, on a machine that has none stored, installs nothing.
`/RELAY=` points the app at a fleet server other than the default (`https://api.bluesec.ai`). The address must
start with `https://`: the app refuses a plain `http://` address, because your partner key would travel in clear
text (the key check fails and nothing is stored).
`/MACHINENAME=` sets the name Cobalt shows for the machine (see [Settings](#settings)).

**Update mode.** `/UPDATE`, or a silent install on a machine that already holds a partner key and is given none,
skips the key pages, leaves the stored keys exactly as they are, and keeps every setting in `limits.json` (only
settings it lacks are filled in). This is how the app updates itself.

### Upgrading from Cobalt Worker

Version 1.2.0 replaces Cobalt Worker 1.1.0 in place (the same entry in Apps & features):

- the installer quits the old app, installs to `%LOCALAPPDATA%\Programs\Cobalt Connector`, and removes the old
  program folder, the old Start-menu entry and the old "run at logon" entry (it adds the new one when the old
  one was on);
- on its first start the app moves your settings folder from `%LOCALAPPDATA%\CobaltWorker` to
  `%LOCALAPPDATA%\CobaltConnector`. If the folder cannot be moved whole, it copies your keys, the machine's id and
  your limits, and deletes the old folder only once a partner key that opens is in the new one. Your keys, your
  limits and the machine's place on the fleet page (its id and history) all carry over.

## Settings

The window (double-click the tray icon) holds everything you can change:

- **Your limits**: cores, memory and hours, with **Save limits**, and **Pause** / **Resume**.
- **This machine**:
  - **Machine name**: the name Cobalt shows for this machine on the fleet page, for example "Gaming PC". Up to 32
    letters, digits, spaces, `-`, `_` and `.`; empty means other partners see it as "<your name>'s machine 1" (you and
    the fleet's owner see the computer's name). **Save name** sends it at once
    (it restarts the app's work process so it says hello with the new name). Renaming never changes the machine's
    id, so its history stays.
  - **Updates**: **Install automatically** (the default) or **Ask me first**, with the version line and an
    **Update now** button (see [Updates](#updates)).
- **Start Cobalt Connector when I sign in to Windows**.
- **Your Anthropic key**: the model-work option, and Save key / Clear key (see below).

The name and the update choice are kept in `limits.json` as `"name"` and `"updates"` (`"auto"` or `"ask"`).

## Updates

Cobalt Connector keeps up with the Cobalt server by itself:

- A minute after it starts, and every 6 hours, it asks the server which version it wants
  (`GET /api/fleet/connector?machine=<id>&version=<this version>`, with your partner key). The answer names the newest
  release, the oldest one the server still sends work to, the installer's address and its SHA-256.
- **Install automatically**: a newer version is downloaded, checked and installed. You get a notification ("A new
  version of Cobalt Connector (1.2.1) is ready. It will install itself now and start again in a minute."), the
  installer runs silently in update mode, and the new version starts in the tray.
- **Ask me first**: you get one notification per new version; click it, or **Update now** in the window, to
  install it. A version the server no longer accepts is installed at once in either mode, since the server sends it
  no work until it is updated.
- **What it trusts**: the installer is downloaded only from this repository's releases
  (`https://github.com/dewilkinson/cobalt-connector/releases/download/...`); every redirect is checked and must stay
  on `github.com`, `objects.githubusercontent.com` or `release-assets.githubusercontent.com` over HTTPS; it is at
  most 300 MB; and it runs only when its SHA-256 matches the one the server gave. A mismatch deletes it. It never
  installs an older or equal version.
- **When something fails**: network errors (and a download sent somewhere other than GitHub's download hosts, or
  cut short) are retried after 1, 2, 4, 8 and 16 hours, then daily. A release whose download does not match its
  SHA-256, or is over 300 MB, is never tried again. A release that does not install (the installer would not start,
  or the app came back on its old version, as after a reboot mid-install) is tried again at the next check, and
  given up only after its third failure; only a newer release (or a corrected one) is then tried. So a bad release
  cannot loop. If an update stops halfway, the installer starts the version that is still there again, and that
  version keeps its own Python, so it still works.
- Notifications are a courtesy (Windows Focus Assist may hold them back): the window always shows the same state.
  The app also notifies you when Cobalt refuses your partner key, stops sending this machine work, or its work
  process keeps stopping - once per change, at most one of a kind an hour.

## How it works

```
CobaltConnector.exe (tray app)
  ├─ python\python.exe -I bootstrap\cobalt_bootstrap.py --pull     (private embedded Python 3.12)
  │    1. POST /api/fleet/hello            partner key in X-Cobalt-Contributor, the app's version and the
  │                                         machine's name; the answer names the code hash
  │    2. GET  /api/fleet/build/<hash>.zip  only if that build is not already here and verified
  │    3. checks it, unpacks it to %LOCALAPPDATA%\CobaltConnector\build\<hash>\
  │    4. runs the build's pull loop: asks for a job, runs it, posts the result back
  │       (with "Run my model work on this machine": also your model jobs, answered with your key
  │        by POST https://api.anthropic.com/v1/messages - the answer text and token counts go back)
  └─ every 6 hours: GET /api/fleet/connector   the version the server wants (see Updates)
```

- **The app** (`app/`, C++20, Win32, WinHTTP, no runtime to install) supervises one Python process. It
  starts it at below-normal priority with a minimal environment built from scratch: nothing of your own
  environment beyond what Windows needs, so no Python setting or model key of yours leaks in. The partner
  key is decrypted from its DPAPI file and passed through that process's environment only. The optional
  Anthropic key is decrypted and passed the same way only while "Run my model work on this machine" is on;
  with it off the key is not read, and the process's environment has no Anthropic key at all. The process and every process it starts run in a Windows job object that
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
- **Pulling, not listening**: the app asks the server for work. It opens no port on your machine and
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
| red (!) | a problem: no key, key refused, offline, benched, too old to take work, or the work process keeps stopping |

The window shows your limits (editable), the machine's name and updates, your Anthropic key, the jobs in flight,
today's totals and the log, with Pause / Resume and "Run at logon".

### Your model work on your own machine

In the window, under **Your Anthropic key**:

- **Run my model work on this machine (my Anthropic key stays here)**. When it is on and a key is stored, the
  app tells the Cobalt server so when it signs in (a yes/no flag, never the key). From then on your research
  questions, note replies, Dreamer questions and Ask about Cobalt questions come to this machine as text. The
  app calls Anthropic's API with your key, and only the answer text and its token counts (or the error, if it
  fails) go back. A question
  is one call; a research topic is a short series of calls (a few turns at most, no tools, no files, nothing on
  the network but Anthropic). While your computer is off the questions wait on the server; after 24 hours an
  unanswered one is closed with a plain message.
- **Save key / Clear key** change or remove the stored key. Paste a new key into the box and press **Save key**;
  the box is emptied at once and the key is stored encrypted, as at install. Changing the key or the option
  restarts the app's work process so it takes effect.
- Pause and your hours limit only the cores you lend: your own model work still runs while you are paused.
- The jobs box shows **Answering a question...** (or **Researching a topic...**) while it works, and today's
  model work: answers, tokens in and tokens out. Everything it spends is billed to your Anthropic account.
- If Anthropic refuses the key, the app stops asking for model work and the window says so; save a new key
  to start again.

## What leaves your machine

This list comes from the code, not from memory: every network call in this repository (`app/src/check_key.cpp`,
`app/src/update.cpp`, `bootstrap/cobalt_bootstrap.py`) and in the worker code the app downloads and runs (its pull
loop, `src/training/contributor.py`, and the server routes it talks to). If you find a call that isn't listed here,
that's a bug - please open an issue.

**The short version**

- **Your Anthropic key goes only to Anthropic** (`https://api.anthropic.com`), and only if you've turned on "Run my
  model work on this machine". It is never sent to Cobalt. With that option off, the work process is never given the
  key: it stays encrypted on your machine.
- **Your partner key is sent to the Cobalt server** with every request the app makes there, always over HTTPS (the
  app refuses a plain `http://` server address). That's how the machine signs in, the same way a password is. The server keeps only a SHA-256 hash
  of it, and the code never logs it. No request that carries it follows a redirect: a redirect is treated as an
  error, so the key can't be passed on to another address.
- The Cobalt server also gets a **short description of the machine** (a random id, the computer's name, the name you
  chose, cores, memory, your limits, versions), the **results of the jobs** it runs, and, if you use model work, **the
  answers Claude wrote, with their token counts**.
- **Other partners can see your machine** on Cobalt's fleet page: its name (the one you chose, or else
  "<your name>'s machine 1", numbered by when each of your machines first joined), the name on your partner key, its
  cores, memory and hours, and what it's doing. They never receive the computer's name or the machine id, not even in
  the data behind the page. Only you (signed in with your own partner key) and the fleet's owner see those.
- **GitHub** gets a plain download request when the app updates itself. No key and no id go with it.
- Nothing goes anywhere else. Market data only ever comes **down**, from the Cobalt server. (If Windows is set to use
  a proxy, the app's requests go through it, as your browser's do.)
- Your files, your environment variables, your Windows user name and your logs are not sent. One caveat: when a job
  fails, its error message is sent back. A Python error about a file names that file's path, which can include your
  Windows user folder (for example `C:\Users\<you>\AppData\Local\CobaltConnector\cache\...`), and a failed model job's
  error can carry Anthropic's reason for refusing (for example a spending limit).
- The worker code comes from the Cobalt server and changes over time. This list describes it as of this release of
  the app.

### To the Cobalt server

The address is `https://api.bluesec.ai` unless you installed with `/RELAY=`. Every request below carries your
**partner key** in the `X-Cobalt-Contributor` header, and goes over HTTPS when the address starts with `https://`
(the app refuses any other address: a plain `http://` one would send your key in clear text. The only exception is
`http://127.0.0.1:<port>` or `http://localhost:<port>`, this machine itself, which the tests use. An address edited
into `contributor.json` that breaks this rule is ignored, and the default is used). None of these
requests follows a redirect: the app, the bootstrap and the worker code all treat a redirect as an error, so the key
goes to this address and nowhere else.

| When | Request | What's sent | What comes back |
|---|---|---|---|
| The installer checks your key | `POST /api/fleet/hello` | machine id, computer name, number of cores, app version, the name you chose, "paused" | accepted or refused, the name your key belongs to, the machine's fleet id |
| Each time the work process starts | `POST /api/fleet/hello` | machine id, computer name, number of cores, the code hash it holds, the bootstrap's version, app version, the name you chose, "paused" | the code hash to run; whether this app is too old to get work |
| About every 30 s between jobs (not while you're paused or outside your hours, unless model work is on) | `POST /api/fleet/hello` | machine id, computer name, cores (your limit, or all of them), total memory, free memory (capped at your limit), job slots, your hours, code hash, the loop's version, jobs running now, paused or outside your hours, the names of the market-data files it holds, and `local_model` (yes/no: model work is on here) | the code hash to run, whether the machine is benched, how many jobs are queued, the update on offer |
| When new worker code is needed | `GET /api/fleet/build/<hash>.zip` | nothing but the request | the worker code (a zip, checked against its hash) |
| When a job needs market data it doesn't have | `GET /api/fleet/shard/<name>` | nothing but the request | one public market-data file (0.5-1 GB), kept |
| Asking for work | `POST /api/fleet/claim` | machine id, code hash, free job slots, free memory, and the kinds of job it takes (only when model work is on) | a job, or nothing |
| After every job | `POST /api/fleet/result` | machine id, job number, seconds it took, and either the result or the error message (cut at 2,000 characters; see the caveat above) | stored, or refused |
| A minute after the app starts, then every 6 hours (less often after a failure), and again right before it downloads an update | `GET /api/fleet/connector?machine=<id>&version=<app version>` | machine id, app version | the newest release, the oldest one still given work, the installer's address and SHA-256 |

- **The machine id** is `cc-` and 12 random hex digits, made once at install. Nothing of the computer's name is in it.
  A machine first installed as Cobalt Worker keeps its older id, which starts with the computer's name.
- **The computer's name** is always sent, even when you choose a name for the machine: the chosen name changes what
  the fleet page shows, not what's sent. Only you and the fleet's owner can read the computer's name back; every other
  partner gets the chosen name, or "<your name>'s machine N".
- **What's in a job result**: for a replay, the list of trades the engine *simulated* on public market data (never
  trades of yours), the symbol, the seconds it took and counts of the steps that turned trades away; for a study,
  the numbers the research function returned; for a model job, the answer text, tokens in, tokens out, the model's
  name and the number of turns.
- **No logs are uploaded.** Nothing in the app or the worker code sends a log file; the only text that goes back
  besides results is a failed job's error message.

**What the server keeps**

- One row per machine: its fleet id (your name on the key plus the machine id), your name, everything in the
  30-second hello above (the computer's name cut at 80 characters), the name you chose, the app's version, when it
  was first and last seen, and how many jobs it did, failed, and how long they took. The rows stay until your partner
  key is revoked, which deletes them.
- Each job's result or error, in the job queue. The code has a one-day clean-up for finished jobs, but nothing runs
  it yet, so assume they're kept.
- A model job's answer (cut at 200,000 characters) and its token counts, filed where the answer belongs (your note,
  your research topic, your Ask about Cobalt question), exactly as if Cobalt had written it, and what it cost at list
  prices, on your own spend record. A failed model job's error is shown there instead.
- Your partner key as a SHA-256 hash, and when it was last used.
- Your IP address isn't stored by any of this code. Like any website, the server and its host can see it, and their
  ordinary access logs may record it.

### To Anthropic

Only when "Run my model work on this machine" is on and an Anthropic key is stored.

| When | Request | What's sent | What comes back |
|---|---|---|---|
| For each model job: one call for a question, up to 12 for a research topic | `POST https://api.anthropic.com/v1/messages` | your **Anthropic key** in `x-api-key`, `anthropic-version`, and the request: the model, the token limit, the system prompt and the messages (the text of your own question, which the Cobalt server built and sent down as the job) | the answer text, tokens in and out, the model's name |

- The address is fixed in the code. The only thing that can replace it is a test stand-in on this same machine
  (`127.0.0.1` or `localhost`); any other address is ignored.
- These calls use their own connection, with no Cobalt header on it, so the partner key never goes to Anthropic.
- Text only: no tools, no files. What it costs is billed to your Anthropic account, never to Cobalt's Admin Key.

### To GitHub

| When | Request | What's sent | What comes back |
|---|---|---|---|
| An update is downloaded | `GET https://github.com/dewilkinson/cobalt-connector/releases/download/v<version>/<file>.exe`, and its redirects, which must stay on `github.com`, `objects.githubusercontent.com` or `release-assets.githubusercontent.com` over HTTPS | nothing but the request (the app's user agent, `CobaltConnector/<version>`); no key, no machine id | the installer, run only if its SHA-256 matches what the Cobalt server said |

Clicking a "download the new version" notification opens the releases page in your own browser.

### Market-data sources and anything else

None. The machine never talks to a market-data provider: all the market data it uses comes down from the Cobalt
server (the shard downloads above). The app opens no port and listens for nothing, and the work process talks to the
Cobalt server and, with model work on, to Anthropic, and nowhere else.

### What never leaves your machine

- **Your Anthropic key, to Cobalt.** The hello only says yes or no to "model work runs here".
- **Your files.** Nothing reads your documents or sends a file. The app and the worker code work in their own folders
  (the install folder and `%LOCALAPPDATA%\CobaltConnector`, plus Windows' temp folder). No file is ever uploaded:
  all that goes up is the request bodies listed above.
- **Your environment.** The work process starts with an environment built from scratch, with only what Windows and
  Python need. Your Windows user name is in it (Windows needs it), but no code sends it; see the error-message caveat
  above.
- **Your logs** (`app.log`, `python.log`, `worker.log`): kept on your machine only.
- **Any other hardware detail**: no processor model, no disk, no network addresses, no MAC address. Just the number of
  cores and the amount of memory.
- **The text of questions and answers, in a log**: never written to one, here or by the loop.

### Keys

- **Your Anthropic key** is stored encrypted for your Windows user (DPAPI) and handed to the work process only through
  its environment, and only while "Run my model work on this machine" is on. With the option off the app doesn't
  decrypt it and the work process never has it. It is sent only to Anthropic, over HTTPS, in the `x-api-key` header, and only for your own model
  work. It is never sent to Cobalt.
- **Your partner key is sent to the Cobalt server**, in the `X-Cobalt-Contributor` header of every request listed
  above (always over HTTPS): the installer's check, each hello, each job request and result, each
  download from the server, and the update check. It has to be: it's how the machine signs in. None of those requests
  follows a redirect, so it can't be forwarded to another address. The server stores only
  its SHA-256 hash and the code never logs it. It is never sent to GitHub or Anthropic. On disk it's kept only
  encrypted, apart from the moment the installer hands it to the app (a plain file in Setup's own temp folder,
  deleted once read), and it's never logged on your machine unless you put it on the installer's command line and
  use `/LOG`.
- In short: two keys, two destinations. The partner key goes to the Cobalt server and the Anthropic key goes to
  Anthropic. Neither goes anywhere else, and no other key is ever sent.

## What it downloads and stores

It downloads:

- the worker code the fleet runs, by its version hash, and only with an accepted partner key;
- public market data only: the market-data files the jobs name, 0.5-1 GB each, from the Cobalt server, downloaded
  once and kept;
- new versions of Cobalt Connector itself, from this repository's releases (see [Updates](#updates)).

A compute job carries nothing of anyone's accounts, money, trades or keys. A model job carries the text of your own
question, built by the Cobalt server from what you can already see there.

| Where | What |
|---|---|
| `%LOCALAPPDATA%\Programs\Cobalt Connector\CobaltConnector.exe` | the app |
| `...\python\` | a private Python 3.12 (python.org's embeddable build) and the worker's third-party libraries (`requirements.txt`, public PyPI packages) - never on PATH |
| `...\bootstrap\cobalt_bootstrap.py` | the bootstrap |
| `%LOCALAPPDATA%\CobaltConnector\` | `limits.json` (your limits, Pause, `local_model`: the model-work option, `name`: the machine's name, `updates`: `auto` or `ask`), `contributor.json` (the machine's id and the server address), `partner.key` and `anthropic.key` (DPAPI-encrypted), `status.json`, `update_state.json` (the update in progress, the back-off and releases that failed), `updates\` (a downloaded installer, deleted once it is installed), `build\<hash>\` (the downloaded worker code), `cache\` (market data), `logs\` |
| `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\CobaltConnector` | only when "Run at logon" is on |

## Your keys

- Both keys are encrypted with **Windows DPAPI** for your Windows user only (user scope, with a per-key
  entropy value) and stored as `partner.key` and `anthropic.key` in `%LOCALAPPDATA%\CobaltConnector`. Another
  Windows user on the same machine, or a copy of the files on another machine, cannot decrypt them.
- The installer hands keys to the app through files in its own temporary folder, which are deleted once
  read; no key is put on a command line unless you choose the `/PARTNERKEY=` or `/ANTHROPICKEY=` form.
- No key is ever written to a log (unless you use one of those command-line forms with `/LOG`), and neither is the
  text of a question or an answer.
- The window's **Save key** and **Clear key** change or remove the Anthropic key later; it is stored the same way.
- An update never touches the keys.
- The partner key is sent only to the fleet server (with every request, always over HTTPS: it's
  how the machine signs in);
  an Anthropic key is sent only to Anthropic. See [Keys](#keys).
- The fleet's owner can revoke a partner key at any time, which ends the machine's access at once.

## Uninstall

Windows Settings > Apps > Cobalt Connector > Uninstall, or silently:

```powershell
& "$env:LOCALAPPDATA\Programs\Cobalt Connector\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
```

It stops the app and its work process and removes the app folder, the private Python, the downloaded worker code,
the cached market data, the logs, both keys, the settings and the "Run at logon" entry. Nothing is left in
`%LOCALAPPDATA%\CobaltConnector` (or an old `%LOCALAPPDATA%\CobaltWorker`) or the registry.

## Command line

```
CobaltConnector.exe [--background]                  the tray app (with its window, or tray only)
CobaltConnector.exe --check-key <key> [--relay URL] check a partner key: exit 0 accepted, 1 refused, 2 could not check
CobaltConnector.exe --check-key-file <file>         the same, the key read from a file (deleted after)
CobaltConnector.exe --set-keys <file>               store partner=/anthropic=/relay= lines with DPAPI (file wiped after)
CobaltConnector.exe --set-limits <file>             save key=value lines into limits.json (?key= only when not set yet)
CobaltConnector.exe --run-at-logon on|off
CobaltConnector.exe --quit                          stop the running app and its work process
CobaltConnector.exe --version
```

A `--relay` address, or a `relay=` line, must start with `https://` (or be `http://127.0.0.1:<port>` or
`http://localhost:<port>`, for tests); any other is refused with exit 1 and nothing is sent or stored.

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

It builds `CobaltConnector.exe` (CMake + Ninja, Release, `/W4 /WX`, static CRT: any warning fails the build),
downloads the Python 3.12 embeddable zip from python.org, installs `requirements.txt` into it (wheels only),
stages the bootstrap and checks it and the libraries with the staged Python, and compiles `setup.iss` into
`dist\CobaltConnectorSetup.exe` (about 85 MB), printing its SHA-256. Everything downloaded or built goes to
`.cache\`, `build\`, `stage\` and `dist\`, which are gitignored. Build with `-Clean` after pulling a change that
renames files (1.2.0 did).

**The version** is written in one place only: `project(CobaltConnector VERSION x.y.z)` in `app/CMakeLists.txt`.
CMake makes `version.h` from it for the code and the exe's version resource, and `build.ps1` passes it to the
installer.

### Publishing a release

Upload `CobaltConnectorSetup.exe` to a GitHub release tagged `v<version>`. The app updates to it only once the Cobalt
server names it (its address and the SHA-256 the build printed), so a release is never picked up by itself.

### Unsigned installer

The installer and the app are not code-signed, so SmartScreen warns about an "unknown publisher". Signing
needs a code-signing certificate (an OV or EV certificate, or Azure Trusted Signing); with one, sign both
`CobaltConnector.exe` and `CobaltConnectorSetup.exe` (`signtool sign /fd sha256 /tr <timestamp-url> /td sha256 ...`,
or Inno Setup's `SignTool=` directive), and the warning goes away once the certificate has reputation.

## Licence

MIT - see [LICENSE](LICENSE). Copyright (c) 2026 Dave Wilkinson.

The licence covers Cobalt Connector itself (the app, the bootstrap, the installer and build scripts). The
worker code the bootstrap downloads with a partner key is not part of this repository and is not covered
by this licence. There is no warranty.
