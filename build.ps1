# Build script for lilhelpers (ASCII only - no BOM issues).
#
# Prefers MSVC (that is what CI releases are built with: MinGW-built binaries
# trip antivirus ML heuristics far more often). Falls back to a mingw-w64
# toolchain - g++/windres from PATH, or LLVM-MinGW installed via winget - so
# the project still builds on machines without Visual Studio.
#
# Force one or the other with -Toolchain msvc|mingw.
param(
    [ValidateSet("auto", "msvc", "mingw")]
    [string]$Toolchain = "auto"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$LIBS = @(
    "user32.lib", "gdi32.lib", "shell32.lib", "comctl32.lib",
    "shlwapi.lib", "ole32.lib", "oleaut32.lib", "gdiplus.lib", "taskschd.lib",
    "advapi32.lib",  # реєстр: mingw лінкує його сам, MSVC вимагає явно
    "wininet.lib", "version.lib",  # CAPS-7: геолокація за IP, версія з VERSIONINFO
    "uxtheme.lib", "dwmapi.lib",   # CAPS-8: темна тема вікна
    "bcrypt.lib",                  # CAPS-10: SHA-256 + ECDSA-перевірка оновлень
    "d2d1.lib", "windowscodecs.lib"   # CAPS-16: рендер SVG
)

function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $install = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if (-not $install) { return $null }
    $vcvars = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $vcvars) { return $vcvars }
    return $null
}

# vcvars64.bat only sets variables in its own cmd session; import them here.
function Import-VcEnv([string]$vcvars) {
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2]
        }
    }
}

function Build-Msvc([string]$vcvars) {
    Import-VcEnv $vcvars

    & rc.exe /nologo /fo lilhelpers.res lilhelpers.rc
    if ($LASTEXITCODE -ne 0) { throw "rc.exe failed" }

    # /utf-8: the source is UTF-8 without a BOM, and cl.exe would otherwise
    # read it in the system ANSI codepage and mangle every Cyrillic literal.
    # /MT: static CRT, no VC++ redistributable needed on the target machine.
    & cl.exe /nologo /std:c++17 /utf-8 /W3 /O2 /MT /DNDEBUG /DUNICODE /D_UNICODE `
        lilhelpers.cpp lilhelpers.res `
        /link /SUBSYSTEM:WINDOWS /OUT:lilhelpers.exe $LIBS
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed" }

    Remove-Item lilhelpers.obj, lilhelpers.res -ErrorAction SilentlyContinue
    Write-Host "OK: lilhelpers.exe built with MSVC"
}

function Resolve-MingwTool([string]$name) {
    $inPath = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($inPath) { return $inPath.Source }

    # winget package layout (its PATH links only appear in a fresh shell)
    $pkgRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    $bin = Get-ChildItem "$pkgRoot\MartinStorsjo.LLVM-MinGW*" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter "llvm-mingw-*" } |
        Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName "bin" }
    if ($bin -and (Test-Path "$bin\$name.exe")) { return "$bin\$name.exe" }

    throw "$name not found. Install a mingw-w64 toolchain, e.g. winget install MartinStorsjo.LLVM-MinGW.UCRT"
}

function Build-Mingw {
    $windres = Resolve-MingwTool "windres"
    $gxx     = Resolve-MingwTool "g++"

    & $windres -i lilhelpers.rc -o lilhelpers_res.o
    if ($LASTEXITCODE -ne 0) { throw "windres failed" }

    # -fno-exceptions/-fno-rtti: GDI+ and the Task Scheduler COM API report
    # status codes rather than throwing; this drops ~240 KB of C++ runtime.
    & $gxx lilhelpers.cpp lilhelpers_res.o -o lilhelpers.exe `
        -municode -mwindows -O2 -s -static -fno-exceptions -fno-rtti `
        -lshell32 -lgdi32 -lgdiplus -lshlwapi -lole32 -loleaut32 -lcomctl32 -ltaskschd -luuid -ladvapi32 `
        -lwininet -lversion -luxtheme -ldwmapi -lbcrypt -ld2d1 -lwindowscodecs -luuid
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }

    Remove-Item lilhelpers_res.o -ErrorAction SilentlyContinue
    Write-Host "OK: lilhelpers.exe built with mingw-w64"
}

$vcvars = if ($Toolchain -eq "mingw") { $null } else { Find-VcVars }

if ($vcvars) {
    Build-Msvc $vcvars
} elseif ($Toolchain -eq "msvc") {
    throw "MSVC requested but Visual Studio C++ tools were not found"
} else {
    Build-Mingw
}
