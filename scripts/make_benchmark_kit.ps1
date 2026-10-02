# Makes a folder (and a zip) that runs the benchmark on another PC without git,
# Visual Studio or a build: only what the benchmark needs, from this built checkout.
#
#   scripts\make_benchmark_kit.ps1                  # kit without the game
#   scripts\make_benchmark_kit.ps1 -IncludeGame     # with your copy of the game (large)
#   scripts\make_benchmark_kit.ps1 -UpdateOnly -Destination D:\sfr-benchmark-kit
#       # copies only the program and the scripts that changed, in place, over a kit an
#       # earlier run of this made (the shader pack, the DXC, the game and the results
#       # there are left alone, nothing is deleted, no zip)
#   scripts\make_benchmark_kit.ps1 -DxcDirectory D:\dxc   # use this folder's dxcompiler.dll and dxil.dll
#
# On the other PC, unzip it and double-click run_benchmark.bat. Without -IncludeGame
# the other PC needs the game's image and asset folders of its own, and the
# batch file asks where they are. The shader pack and the game folders come from
# your copy of the game: keep a kit that holds them on your own PCs.
param(
    [string]$Destination = '',
    [switch]$IncludeGame,
    [switch]$UpdateOnly,
    [string]$DxcDirectory = '',
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path (Split-Path -Parent $root) 'sfr-benchmark-kit' }
$host_dir = Join-Path $root 'out/build/host'
if (-not (Test-Path -LiteralPath (Join-Path $host_dir 'sfr_cpu_diagnostic.exe'))) {
    throw "Build first: scripts\build_tools.ps1 -Diagnostic (missing sfr_cpu_diagnostic.exe)"
}
if ($UpdateOnly) {
    if (-not (Test-Path -LiteralPath $Destination)) { throw "$Destination does not exist: make a whole kit first (without -UpdateOnly)." }
    $NoZip = $true
} elseif (Test-Path -LiteralPath $Destination) {
    # A kit that has been run holds its results: never delete those along with it.
    if (Test-Path -LiteralPath (Join-Path $Destination 'out/bench')) {
        throw "$Destination holds benchmark results (out\bench). Move them away, or choose another -Destination."
    }
    Remove-Item -Recurse -Force -LiteralPath $Destination
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$updated = [System.Collections.Generic.List[string]]::new()

function Copy-Into([string]$relative, [string[]]$exclude = @()) {
    $from = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $from)) { return $false }
    $to = Join-Path $Destination $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
    if ($UpdateOnly) {
        # In place: only the files that differ, and nothing the kit made while running
        $sources = if ((Get-Item -LiteralPath $from).PSIsContainer) { Get-ChildItem -LiteralPath $from -Recurse -File } else { @(Get-Item -LiteralPath $from) }
        foreach ($file in $sources) {
            $name = $file.Name
            if ($exclude | Where-Object { $name -like $_ }) { continue }
            if ($file.FullName -match '[\\/]save[\\/]') { continue }
            $target = Join-Path $Destination ($file.FullName.Substring($root.Length).TrimStart('\', '/'))
            # Same size and time: the copy there is this file (a hash would read it all over the network)
            if (Test-Path -LiteralPath $target) {
                $there = Get-Item -LiteralPath $target
                if ($there.Length -eq $file.Length -and [math]::Abs(($there.LastWriteTimeUtc - $file.LastWriteTimeUtc).TotalSeconds) -lt 3) { continue }
            }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item -Force -LiteralPath $file.FullName -Destination $target
            (Get-Item -LiteralPath $target).LastWriteTimeUtc = $file.LastWriteTimeUtc
            $updated.Add($target.Substring($Destination.Length).TrimStart('\', '/'))
        }
        return $true
    }
    if ((Get-Item -LiteralPath $from).PSIsContainer) {
        Copy-Item -Recurse -Force -LiteralPath $from -Destination $to -Exclude $exclude
        if ($exclude.Count) {
            foreach ($pattern in $exclude) { Get-ChildItem -LiteralPath $to -Recurse -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Force }
        }
    } else { Copy-Item -Force -LiteralPath $from -Destination $to }
    return $true
}

Copy-Into 'run_benchmark.bat' | Out-Null
try { (& git -C $root rev-parse --short HEAD 2>$null) | Set-Content -LiteralPath (Join-Path $Destination 'commit.txt') -Encoding ASCII } catch { }
foreach ($file in 'benchmark.ps1', 'benchmark_summary.py', 'profile_summary.py', 'anonymize_benchmark.py', 'benchmark_report.py', 'play.ps1') {
    Copy-Into "scripts/$file" | Out-Null
}
Copy-Into 'out/build/host' @('*.pdb', '*.ilk', '*.obj', '*.map', 'settings.ini') | Out-Null
# The shader pack and the recorded pipeline lists carry the shader ABI: a kit that keeps
# an older one translates every shader at the start (and needs the checkout's shader
# header, which a kit has not) or ignores the lists. So -UpdateOnly brings them along too,
# but not the runtime cache a checkout made beside them.
if ($UpdateOnly) { Copy-Into 'out/shaders' @('runtime') | Out-Null }
elseif (-not (Copy-Into 'out/shaders')) { Write-Warning 'out\shaders (the shader pack) was not found: the game may translate its shaders at the first start, which needs this checkout.' }
Copy-Into 'data/pipeline-manifests' | Out-Null
# The renderer links the pixel shaders of the D3D12 backend with a DXC (dxcompiler.dll
# and dxil.dll): the one SFR_DXC_LIBRARY names, else the checkout's dxc-bin, else any
# dxcompiler.dll under this checkout. The kit's run_benchmark.bat points the game at
# the dxc folder made here.
$dxc = $null
foreach ($candidate in @($DxcDirectory, $env:SFR_DXC_LIBRARY, (Join-Path $root 'tools/XenosRecomp/thirdparty/dxc-bin/bin/x64'))) {
    if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'dxcompiler.dll'))) { $dxc = $candidate; break }
}
if (-not $dxc -and -not $UpdateOnly) {
    $found = Get-ChildItem -LiteralPath $root -Recurse -Filter dxcompiler.dll -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.DirectoryName 'dxil.dll') } | Select-Object -First 1
    if ($found) { $dxc = $found.DirectoryName }
}
if ($UpdateOnly) { }
elseif ($dxc) {
    $to = Join-Path $Destination 'dxc'
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    foreach ($name in 'dxcompiler.dll', 'dxil.dll') { Copy-Item -Force -LiteralPath (Join-Path $dxc $name) -Destination $to }
    Write-Output "DXC from $dxc"
} else {
    Write-Warning 'dxcompiler.dll and dxil.dll were not found: the game stops at its first draw with dxcompiler.dll is unavailable.'
}

if ($IncludeGame -and -not $UpdateOnly) {
    $image = Join-Path $root 'out/recomp/image-loader'
    $assets = Join-Path $root 'private/assets'
    $ini = Join-Path $host_dir 'settings.ini'
    if (Test-Path -LiteralPath $ini) {
        foreach ($line in Get-Content -LiteralPath $ini -Encoding UTF8) {
            if ($line -match '^image_directory=(.+)$') { $image = $Matches[1] }
            if ($line -match '^asset_directory=(.+)$') { $assets = $Matches[1] }
        }
    }
    foreach ($pair in @(@($image, 'out/recomp/image-loader'), @($assets, 'private/assets'))) {
        if (-not (Test-Path -LiteralPath $pair[0])) { throw "Missing $($pair[0])" }
        $to = Join-Path $Destination $pair[1]
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
        Copy-Item -Recurse -Force -LiteralPath $pair[0] -Destination $to
    }
}

# What the kit needs to run on another PC, and which of it this one has.
$needs = @(
    @('the program', 'out/build/host/sfr_cpu_diagnostic.exe', $true),
    @('run_benchmark.bat', 'run_benchmark.bat', $true),
    @('benchmark script', 'scripts/benchmark.ps1', $true),
    @('DXC (dxcompiler.dll)', 'dxc/dxcompiler.dll', -not $UpdateOnly),
    @('shader pack', 'out/shaders/shaders.pack', $false),
    @('game image', 'out/recomp/image-loader', $false),
    @('game assets', 'private/assets', $false))
foreach ($need in $needs) {
    $there = Test-Path -LiteralPath (Join-Path $Destination $need[1])
    if ($UpdateOnly -and $need[1] -match '^(dxc|out/shaders|out/recomp|private)') { continue }
    $mark = if ($there) { 'ok     ' } elseif ($need[2]) { 'MISSING' } else { 'absent ' }
    Write-Output "  [$mark] $($need[0])"
}
$size = if ($UpdateOnly) { $null } else { [math]::Round((Get-ChildItem -Recurse -File -LiteralPath $Destination | Measure-Object Length -Sum).Sum / 1MB) }
if ($UpdateOnly) {
    if ($updated.Count) { Write-Output "Updated in ${Destination}:"; $updated | ForEach-Object { Write-Output "  $_" } } else { Write-Output "Already up to date: $Destination" }
}
if ($null -ne $size) { Write-Output "Kit folder: $Destination ($size MB)" }
if (-not $NoZip -and (Get-Command tar -ErrorAction SilentlyContinue)) {
    $zip = "$Destination.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -Force -LiteralPath $zip }
    & tar -a -c -f $zip -C $Destination .
    if (Test-Path -LiteralPath $zip) { Write-Output "Kit zip:    $zip" }
}
if (-not $UpdateOnly) { Write-Output 'On the other PC: unzip, then double-click run_benchmark.bat' }
