# make_icon.ps1 - rebuild <name>.ico and <name>.png from an SVG (default lilhelpers.svg).
# ASCII only - no BOM issues.
#
# This script NEVER edits the artwork. It rasterizes the SVG at each target
# size (vector -> pixels, no downscaling of a raster) and packs those frames
# into a multi-size .ico. Edit the SVG, rerun this, rebuild the exe.
#   .\make_icon.ps1                                   -> lilhelpers.ico + lilhelpers.png here
#   .\make_icon.ps1 -Svg lh_icon.svg -OutDir preview -KeepPngs
#                                                     -> preview\lh_icon.{ico,png} + one png per size
#
# Sizes cover what Windows actually asks for, at every common DPI scaling:
#   tray / small icon : 16 (100%), 20 (125%), 24 (150%), 28 (175%), 32 (200%)
#   shell large icon  : 32 (100%), 40 (125%), 48 (150%), 56 (175%), 64 (200%)
#   Explorer / jumbo  : 48, 96, 128, 256
#
# Rasterizing needs a Chromium browser in headless mode - Chrome or Edge,
# whichever is installed.
#
# Small frames are stored as 32-bit DIBs (the format every icon consumer has
# always understood); large ones as PNG, because an uncompressed 128px DIB
# alone costs 67 KB and all of this ends up inside the exe. Windows has read
# PNG frames since Vista, and this app targets Windows 10+ anyway.

param(
    [int[]]$Sizes    = @(16, 20, 24, 28, 32, 40, 48, 56, 64, 96, 128, 256),
    [int]  $LogoSize = 256,  # <Name>.png, drawn inside the settings window
    [int]  $PngFrom  = 96,   # frames >= this size are stored PNG-compressed
    [string]$Svg     = "lilhelpers.svg",  # source artwork (relative to this folder)
    [string]$OutDir  = "",              # where <Name>.ico/.png land; default: this folder
    [string]$Name    = "",              # base name of the outputs; default: SVG's base name
    [switch]$KeepPngs                   # also save every rendered frame as <Name>_<size>.png
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
Add-Type -AssemblyName System.Drawing

$svgPath = Join-Path $PSScriptRoot $Svg
if (-not (Test-Path $svgPath)) { throw "$Svg not found" }
if (-not $Name)   { $Name = [System.IO.Path]::GetFileNameWithoutExtension($Svg) }
if (-not $OutDir) { $OutDir = $PSScriptRoot } else { $OutDir = Join-Path $PSScriptRoot $OutDir }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

function Find-Browser {
    $candidates = @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    throw "No Chromium browser found (needed to rasterize the SVG)"
}

# Renders the SVG into a size x size PNG on a transparent background.
function Render-Png([string]$browser, [string]$work, [int]$size, [string]$outFile) {
    # UTF8 explicitly: a BOM-less SVG with non-ASCII ids (Illustrator exports
    # gradient ids in the UI language) would otherwise be read as ANSI.
    $svg  = Get-Content $svgPath -Raw -Encoding UTF8
    $page = Join-Path $work "page_$size.html"
    $html = "<html><head><meta charset='utf-8'><style>html,body{margin:0;padding:0;" +
            "background:transparent}svg{display:block;width:${size}px;height:${size}px}" +
            "</style></head><body>$svg</body></html>"
    Set-Content -Path $page -Value $html -Encoding UTF8

    $url = "file:///" + ($page -replace '\\', '/')
    $a = @("--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
           "--disable-background-networking", "--disable-sync", "--disable-extensions",
           "--user-data-dir=$work\profile", "--hide-scrollbars",
           "--default-background-color=00000000",
           "--screenshot=$outFile", "--window-size=$size,$size", $url)

    $p = Start-Process $browser -ArgumentList $a -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) {
        $p | Stop-Process -Force
        throw "headless render timed out at ${size}px"
    }
    if (-not (Test-Path $outFile)) { throw "render produced no file at ${size}px" }
}

# 32-bit BGRA bottom-up DIB + empty AND mask, as icon directories expect.
function ConvertTo-IcoDib([string]$pngPath) {
    $bmp = New-Object System.Drawing.Bitmap($pngPath)
    try {
        $w = $bmp.Width; $h = $bmp.Height
        $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
        $locked = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                                [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $pixels = New-Object byte[] ($locked.Stride * $h)
        [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $pixels, 0, $pixels.Length)
        $stride = $locked.Stride
        $bmp.UnlockBits($locked)

        $maskStride = [math]::Floor(($w + 31) / 32) * 4
        $xorSize = $w * 4 * $h
        $andSize = $maskStride * $h

        $ms = New-Object System.IO.MemoryStream
        $bw = New-Object System.IO.BinaryWriter($ms)
        # BITMAPINFOHEADER; height is doubled because it covers XOR + AND masks
        $bw.Write([uint32]40); $bw.Write([int32]$w); $bw.Write([int32]($h * 2))
        $bw.Write([uint16]1);  $bw.Write([uint16]32)
        $bw.Write([uint32]0);  $bw.Write([uint32]($xorSize + $andSize))
        $bw.Write([int32]0);   $bw.Write([int32]0)
        $bw.Write([uint32]0);  $bw.Write([uint32]0)

        for ($y = $h - 1; $y -ge 0; $y--) { $bw.Write($pixels, $y * $stride, $w * 4) }
        $bw.Write((New-Object byte[] $andSize), 0, $andSize)

        $bw.Flush()
        return $ms.ToArray()
    } finally { $bmp.Dispose() }
}

function Write-Ico([hashtable]$frames, [string]$outFile) {
    $ordered = $frames.Keys | Sort-Object
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)

    $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$ordered.Count)
    $offset = 6 + 16 * $ordered.Count
    foreach ($s in $ordered) {
        $data = $frames[$s]
        $dim = if ($s -ge 256) { 0 } else { $s }   # 0 means 256 in the directory
        $bw.Write([byte]$dim); $bw.Write([byte]$dim)
        $bw.Write([byte]0);    $bw.Write([byte]0)
        $bw.Write([uint16]1);  $bw.Write([uint16]32)
        $bw.Write([uint32]$data.Length); $bw.Write([uint32]$offset)
        $offset += $data.Length
    }
    foreach ($s in $ordered) { $bw.Write($frames[$s], 0, $frames[$s].Length) }

    $bw.Flush()
    [System.IO.File]::WriteAllBytes($outFile, $ms.ToArray())
}

$browser = Find-Browser
Write-Host "rasterizing with $(Split-Path $browser -Leaf)"

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("${Name}_icon_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work -Force | Out-Null

try {
    $frames = @{}
    foreach ($s in ($Sizes | Sort-Object -Unique)) {
        $png = Join-Path $work "icon_$s.png"
        Render-Png $browser $work $s $png
        if ($KeepPngs) { Copy-Item $png (Join-Path $OutDir "${Name}_$s.png") -Force }
        $frames[$s] = if ($s -ge $PngFrom) { [System.IO.File]::ReadAllBytes($png) }
                      else { ConvertTo-IcoDib $png }
        Write-Host ("  {0,3}px -> {1,7} bytes" -f $s, $frames[$s].Length)
    }

    Write-Ico $frames (Join-Path $OutDir "$Name.ico")

    $logo = Join-Path $work "logo.png"
    Render-Png $browser $work $LogoSize $logo
    Copy-Item $logo (Join-Path $OutDir "$Name.png") -Force

    Write-Host "OK: $OutDir\$Name.ico ($($frames.Count) frames) + $Name.png (${LogoSize}px)"
} finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
