# Builds Cobalt Connector and its installer.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Clean] [-SkipInstaller]
#
# 1. CobaltConnector.exe: CMake + Ninja + MSVC (Visual Studio 2026, MSVC 14.51), Release, /W4 /WX, static CRT.
#    When Visual Studio has no Windows SDK installed, the SDK's official NuGet packages
#    (Microsoft.Windows.SDK.CPP and .x64) are downloaded into .cache\winsdk and used instead - no
#    administrator rights needed.
# 2. The embedded Python: the official python.org 3.12 embeddable zip (cached in .cache), its ._pth
#    pointed at its own stdlib and Lib\site-packages, the worker's third-party libraries
#    (requirements.txt, public PyPI wheels) installed into it with uv (wheels only).
# 3. The bootstrap (bootstrap\cobalt_bootstrap.py), staged and checked with the staged Python. The
#    installer carries NO worker code: the bootstrap downloads it by hash once a partner key is accepted.
# 4. Inno Setup 6 compiles setup.iss into dist\CobaltConnectorSetup.exe, with the version read from
#    app\CMakeLists.txt (the only place it is written).
#
# Everything it downloads or builds stays under .cache\, build\, stage\ and dist\, all gitignored.
param(
    [switch] $Clean,
    [switch] $SkipInstaller,
    [string] $PythonVersion = "3.12.10",
    [string] $SdkVersion = "10.0.26100.9169",
    [string] $VsRoot = "C:\Program Files\Microsoft Visual Studio\18\Professional",
    [string] $Iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cache = Join-Path $here ".cache"
$build = Join-Path $here "build"
$stage = Join-Path $here "stage"
$dist = Join-Path $here "dist"
function Say($t) { Write-Host "== $t" }

if ($Clean) { foreach ($d in $build, $stage, $dist) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } } }
New-Item -ItemType Directory -Force $cache, $build, $stage, $dist | Out-Null

# ------------------------------------------------------------------ 1. the exe
$vcvars = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $vcvars)) { throw "Visual Studio's vcvars64.bat is not at $vcvars (pass -VsRoot)" }

Say "MSVC environment"
if (-not ${env:ProgramFiles(x86)}) { ${env:ProgramFiles(x86)} = "C:\Program Files (x86)" }   # vcvars finds vswhere through it
$lines = cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($l in $lines) {
    $i = $l.IndexOf("=")
    if ($i -gt 0) { Set-Item -Path "env:$($l.Substring(0, $i))" -Value $l.Substring($i + 1) }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "cl.exe is not on the path after vcvars64.bat" }

if (-not (Get-Command rc.exe -ErrorAction SilentlyContinue)) {
    Say "No Windows SDK in Visual Studio: using the SDK's NuGet packages $SdkVersion"
    $sdk = Join-Path $cache "winsdk"
    New-Item -ItemType Directory -Force $sdk | Out-Null
    foreach ($p in "microsoft.windows.sdk.cpp", "microsoft.windows.sdk.cpp.x64") {
        $dir = Join-Path $sdk ($p -replace "^microsoft\.windows\.sdk\.", "")
        if (Test-Path (Join-Path $dir "c")) { continue }
        $pkg = Join-Path $sdk "$p.$SdkVersion.zip"
        if (-not (Test-Path $pkg)) {
            Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/$p/$SdkVersion/$p.$SdkVersion.nupkg" -OutFile $pkg
        }
        Expand-Archive -Path $pkg -DestinationPath $dir -Force
    }
    $inc = Get-ChildItem (Join-Path $sdk "cpp\c\Include") -Directory | Sort-Object Name | Select-Object -Last 1
    $bin = Join-Path $sdk "cpp\c\bin\$($inc.Name)\x64"
    $sdkInc = (@("ucrt", "um", "shared", "winrt", "cppwinrt") | ForEach-Object { Join-Path $inc.FullName $_ }) -join ";"
    $env:INCLUDE = "$sdkInc;$env:INCLUDE"
    $env:LIB = (Join-Path $sdk "cpp.x64\c\um\x64") + ";" + (Join-Path $sdk "cpp.x64\c\ucrt\x64") + ";" + $env:LIB
    $env:PATH = "$bin;$env:PATH"
}

Say "configure (Release)"
& $cmake -S (Join-Path $here "app") -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release | Out-Host
if ($LASTEXITCODE) { throw "cmake configure failed" }
Say "build"
$out = & $cmake --build $build --config Release --clean-first 2>&1
$out | Out-Host
if ($LASTEXITCODE) { throw "the build failed" }
$warnings = @($out | Select-String -Pattern "warning [A-Z]+\d+|: warning" )
Say "warnings: $($warnings.Count)"
if ($warnings.Count) { throw "the build has warnings (it must build clean at /W4)" }
$exe = Join-Path $build "CobaltConnector.exe"

# ------------------------------------------------------------------ 2. the embedded Python
$pyZip = Join-Path $cache "python-$PythonVersion-embed-amd64.zip"
if (-not (Test-Path $pyZip)) {
    Say "downloading the Python $PythonVersion embeddable zip from python.org"
    Invoke-WebRequest -Uri "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip" -OutFile $pyZip
}
$py = Join-Path $stage "python"
if (Test-Path $py) { Remove-Item -Recurse -Force $py }
Expand-Archive -Path $pyZip -DestinationPath $py -Force
$pth = Get-ChildItem $py -Filter "python3*._pth" | Select-Object -First 1
$zipName = (Get-ChildItem $py -Filter "python3*.zip" | Select-Object -First 1).Name
# The private Python's whole search path: its own stdlib and its packages. Nothing of the machine's (the
# ._pth file makes the interpreter ignore PYTHONPATH and the registry). The bootstrap puts the downloaded
# worker build on sys.path itself when it runs it.
$pthText = "$zipName`r`n.`r`nLib\site-packages`r`nimport site`r`n"
[IO.File]::WriteAllText($pth.FullName, $pthText, (New-Object Text.UTF8Encoding $false))

Say "installing the worker's third-party libraries into it (uv, wheels only)"
$uv = (Get-Command uv -ErrorAction SilentlyContinue).Source
if (-not $uv) { throw "uv is needed to install the libraries (https://docs.astral.sh/uv/)" }
$env:UV_CACHE_DIR = Join-Path $cache "uv"
& $uv pip install --quiet --target (Join-Path $py "Lib\site-packages") --python-version 3.12 `
    --python-platform x86_64-pc-windows-msvc --only-binary ":all:" --no-compile -r (Join-Path $here "requirements.txt")
if ($LASTEXITCODE) { throw "the libraries could not be installed" }
# the scripts uv puts beside the packages point at a Python that is not on this machine
$scripts = Join-Path $py "Lib\site-packages\bin"
if (Test-Path $scripts) { Remove-Item -Recurse -Force $scripts }

# ------------------------------------------------------------------ 3. the bootstrap
Say "staging the bootstrap"
$boot = Join-Path $stage "bootstrap"
if (Test-Path $boot) { Remove-Item -Recurse -Force $boot }
New-Item -ItemType Directory -Force $boot | Out-Null
Copy-Item (Join-Path $here "bootstrap\cobalt_bootstrap.py") $boot
$stagedPy = Join-Path $py "python.exe"
& $stagedPy -I -X utf8 (Join-Path $boot "cobalt_bootstrap.py") --version
if ($LASTEXITCODE) { throw "the staged bootstrap does not run" }
Say "checking the third-party libraries import with the staged Python"
& $stagedPy -I -X utf8 -c "import httpx, fastapi, pydantic, numpy, pandas, pyarrow, pytz, dateutil, dotenv, scipy.stats, numba; from smartmoneyconcepts import smc; print('libraries ok')"
if ($LASTEXITCODE) { throw "the staged libraries do not import" }
Get-ChildItem $stage -Recurse -Directory -Filter "__pycache__" | Remove-Item -Recurse -Force

Copy-Item $exe (Join-Path $stage "CobaltConnector.exe") -Force

# ------------------------------------------------------------------ 4. the installer
if ($SkipInstaller) { Say "installer skipped"; exit 0 }
if (-not (Test-Path $Iscc)) { throw "Inno Setup 6 is not at $Iscc (winget install JRSoftware.InnoSetup)" }
$cmakeLists = Get-Content (Join-Path $here "app\CMakeLists.txt") -Raw
if ($cmakeLists -notmatch "project\(CobaltConnector VERSION (\d+\.\d+\.\d+)") { throw "no version in app\CMakeLists.txt" }
$version = $Matches[1]
Say "compiling the installer (version $version)"
$isccOut = & $Iscc "/DStageDir=$stage" "/DOutDir=$dist" "/DAppVersion=$version" (Join-Path $here "setup.iss") 2>&1
if ($LASTEXITCODE) { $isccOut | Out-Host; throw "ISCC failed" }
$isccWarn = @($isccOut | Select-String -Pattern "^Warning")
$isccWarn | Out-Host
Say "installer compiler warnings: $($isccWarn.Count)"
$setup = Get-Item (Join-Path $dist "CobaltConnectorSetup.exe")
Say ("installer: {0} ({1:N1} MB)" -f $setup.FullName, ($setup.Length / 1MB))
# the release's SHA-256: it goes into the server's connector_release.json, which the app checks before it
# runs a downloaded installer
Say ("SHA-256: {0}" -f (Get-FileHash -Algorithm SHA256 $setup.FullName).Hash.ToLowerInvariant())
