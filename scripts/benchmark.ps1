# Measures a Free Race without anyone playing: the game starts, says the menu
# words that reach a race (SFR_SAY, docs/race-controls.md), races with nobody
# at the controls and stops itself after -PresentLimit frames. Each setting
# in -Configs runs -Repeats times, taking turns, and the race frames of every
# run are summed up by scripts/benchmark_summary.py (docs/benchmark.md).
#
#   scripts\benchmark.ps1                      # the default comparison
#   scripts\benchmark.ps1 -Configs baseline -Repeats 1
#   scripts\benchmark.ps1 -Scenario solo -FixedStep   # whether a change helps: no rivals, the same frames every run
#
# -Scenario race (the default) is a Free Race with rivals and items: the frame rate a
# player gets. -Scenario solo is a Time Attack, the player alone on the course, so no
# rival or item makes one run heavier than another. -FixedStep steps the race one
# original frame (1/60 s) per present, as the desktop launcher does (SFR_REALTIME_RACE=0),
# so every run draws the same race frames whatever the PC's speed (docs/benchmark.md).
#
# Build first: scripts\build_tools.ps1 -Diagnostic. The game runs with the
# explicit elapsed-time defaults: no Kinect, normal sound,
# and a copy of your save in the run's own directory.
param(
    [string[]]$Configs = @('baseline', 'no-suspend-notify'),
    [ValidateRange(1, 100)][int]$Repeats = 6,
    # The menu words and the presents they are said at: title, main menu,
    # one turn of the ring (the ring wraps: Free Race is left of World Grand Prix;
    # if `left` does nothing, five `right` do it), then rules, course, character and
    # gear. By present 12000 the race is running.
    # -Scenario solo turns `right` to Time Attack instead (docs/race-controls.md: the ring
    # is World Grand Prix, Time Attack, ...), then course, character and gear: it has no rules page.
    [string]$Say = 'ok@580,ok@930,ok@1280,start@1630,ok@1830,left@2030,ok@2230,ok@2530,ok@2830,ok@3130,right@3430,ok@3730',
    [ValidateSet('race', 'solo')][string]$Scenario = 'race',
    [switch]$FixedStep,               # one original frame per present (SFR_REALTIME_RACE=0): every run the same race frames
    [ValidateRange(0, 10000000)][int]$RaceFrames = 0,  # race presents a run (SFR_PRESENT_LIMIT_RACING); 0: from -RaceSeconds
    [int]$PresentLimit = 15600,
    [switch]$NoStretch,               # the words by presents alone and the run ends at -PresentLimit (it cannot finish on a PC that presents fast)
    [ValidateRange(1.0, 10000.0)][double]$ReferenceFps = 100,
    [ValidateRange(1, 10000000)][int]$AfterSay = 4200,
    [ValidateRange(0, 3600)][int]$RaceSeconds = 90,   # every counted run races at least this long, judged from the warm-up (0: -AfterSay as given)
    [ValidateRange(0, 10000000)][int]$Skip = 600,
    [ValidateRange(1, 120)][int]$TimeoutMinutes = 25,
    [ValidateSet('d3d12', 'vulkan')][string]$Backend = 'd3d12',
    [switch]$NoAudio,                 # explicit diagnostic, recorded in metadata
    [switch]$AllowDiagnosticRendering, # required for omitted-draw comparisons
    [int]$ScreenshotEvery = 0,         # a screenshot every N presents in every run (a diagnostic: it slows the run)
    [switch]$NoWarmup,                # the first run fills the shader caches
    [switch]$FixedOrder,              # the settings in the same order every round (the default turns the order each round)
    [switch]$SkipBuildCheck,          # a copied folder (no git): its files' times say nothing about the build
    [switch]$Capped,                  # 60 fps as when playing, not as fast as it goes
    [switch]$ColdPipelines,           # every run starts without pipeline-cache: a new player's first race
    [switch]$KeepSaves,               # keep each run's copy of the save (save-<run>; 23 MB each), to see what the game wrote
    [string]$ImageDirectory = '',     # the game folders, when they are not where this checkout keeps them
    [string]$AssetDirectory = '',
    [string]$SaveDirectory = '',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
function Get-BenchmarkSha256([string]$Path) {
    # Windows PowerShell hosts can expose Utility without its Get-FileHash
    # helper. Use the same streaming SHA-256 implementation directly.
    $stream = [System.IO.File]::OpenRead($Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return [System.BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}
$originalEnvironment = @{}
Get-ChildItem Env:SFR_* | ForEach-Object { $originalEnvironment[$_.Name] = $_.Value }
$process = $null
try {
$Stretch = -not $NoStretch
$SoloSay = 'ok@580,ok@930,ok@1280,start@1630,ok@1830,right@2030,ok@2230,ok@2530,ok@2830,right@3130,ok@3430'
if ($Scenario -eq 'solo' -and -not $PSBoundParameters.ContainsKey('Say')) { $Say = $SoloSay }
$root = Split-Path -Parent $PSScriptRoot
# A kit keeps its own DXC next to the scripts (run_benchmark.bat sets this too); without it nothing is drawn
if (-not $env:SFR_DXC_LIBRARY -and (Test-Path -LiteralPath (Join-Path $root 'dxc\dxcompiler.dll'))) { $env:SFR_DXC_LIBRARY = Join-Path $root 'dxc' }
$host_dir = Join-Path $root 'out/build/host'
$exe = Join-Path $host_dir 'sfr_cpu_diagnostic.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build first: scripts\build_tools.ps1 -Diagnostic (missing $exe)" }
if ($Stretch -and -not [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($exe)).Contains('SFR_PRESENT_LIMIT_AFTER_SAY')) {
    throw "$exe is older than -Stretch (no SFR_PRESENT_LIMIT_AFTER_SAY): build again, or take the program from a new kit"
}
function Test-ProgramText([string]$Path, [string]$Text) {
    return [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($Path)).Contains($Text)
}
# A build that failed leaves the last good executable behind, and a benchmark of
# it would be taken for one of the sources checked out now.
$src = Join-Path $root 'src'
$newest = if (-not $SkipBuildCheck -and (Test-Path -LiteralPath $src)) {
    Get-ChildItem -LiteralPath $src -Include *.cpp, *.h -Recurse | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if ($newest -and $newest.LastWriteTime -gt (Get-Item -LiteralPath $exe).LastWriteTime) {
    throw "$exe is older than src\$($newest.Name): the last build did not finish. Build again: scripts\build_tools.ps1 -Diagnostic"
}

# Where the launcher found the game, else where a checkout keeps it.
$image = Join-Path $root 'out/recomp/image-loader'
$assets = Join-Path $root 'private/assets'
$ini = Join-Path $host_dir 'settings.ini'
if (Test-Path -LiteralPath $ini) {
    foreach ($line in Get-Content -LiteralPath $ini -Encoding UTF8) {
        if ($line -match '^image_directory=(.+)$') { $image = $Matches[1] }
        if ($line -match '^asset_directory=(.+)$') { $assets = $Matches[1] }
    }
}
if ($ImageDirectory) { $image = $ImageDirectory }
if ($AssetDirectory) { $assets = $AssetDirectory }
# Paths copied from another PC's settings.ini lead nowhere: this folder's own
# are used then.
if (-not (Test-Path -LiteralPath $image)) { $image = Join-Path $root 'out/recomp/image-loader' }
if (-not (Test-Path -LiteralPath $assets)) { $assets = Join-Path $root 'private/assets' }
foreach ($path in $image, $assets) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing $path (install the game with the launcher first)" }
}

# What each setting changes from the launcher's defaults.
$settings = @{
    'baseline'     = @{}
    'sequential'   = @{ SFR_HOST_PROCESSORS = 'sequential' }   # the old processor order
    'serial'       = @{ SFR_PARALLEL_WORKER = '0' }            # guest threads one at a time
    'all'          = @{ SFR_PARALLEL_WORKER = 'all' }          # every guest thread in parallel (experimental)
    'skip-draws'   = @{ SFR_SKIP_DRAWS = '1' }                 # the game without its drawing: the ceiling
    'render-every-2' = @{ SFR_RENDER_EVERY = '2' }             # a race drawn every other frame
    'render-50'    = @{ SFR_RENDER_SCALE = '50' }              # half the rendering resolution: how much is the GPU's (run_benchmark.bat report)
    'no-vertex-cache' = @{ SFR_VERTEX_CACHE = '0' }
    'no-gpu-pipeline' = @{ SFR_GPU_PIPELINE = '0' }
    'no-audio'     = @{ SFR_AUDIO = '0' }                      # no sound sent to the PC's output (the game still mixes it): what the output costs
    # the main thread left to Windows instead of pinned to the first core, and the other way round.
    # The default pins it with six or more logical processors and leaves it unpinned with fewer
    # (pin_main_thread_by_default in native_thread.h), so one of the two is the baseline on any PC.
    'main-unpinned' = @{ SFR_MAIN_AFFINITY = '0' }
    'main-pinned'   = @{ SFR_MAIN_AFFINITY = '1' }
    # a self-suspended guest is woken by polling every 1 ms again, as before
    # (baseline wakes it by notification: GuestThreads::suspension_waiter)
    'no-suspend-notify' = @{ SFR_SUSPEND_NOTIFY = '0' }
    # pipelines are only built when a draw first needs them, as before (the
    # manifest is still written: NativeRenderer::Impl::load_manifest)
    'no-prewarm'   = @{ SFR_PIPELINE_PREWARM = '0' }
    'vulkan'       = @{ SFR_GRAPHICS = 'vulkan' }
    # opt-in or default-on experiments whose effect on frame time is not established
    # (docs/handheld-performance-2026-09-30.md): each differs from baseline by one switch
    'constant-reuse' = @{ SFR_CONSTANT_UPLOAD_REUSE = '1' }    # exact 4 KiB constant uploads reused (off by default)
    'priority'       = @{ SFR_GUEST_SATURATED_PRIORITY = '1' } # saturated guests' host priority raised (off by default)
    'no-host-timing' = @{ SFR_HOST_TIMING = '0' }              # the host timer and speed policy left alone (on by default)
    'audio-parallel' = @{ SFR_AUDIO_PARALLEL = '5' }          # the audio pump beside the main thread on console processor 5 (off by default)
    # Two executables of different builds in out/build/host, compared in the same
    # rounds: copy each build's sfr_cpu_diagnostic.exe to sfr_cpu_diagnostic_a.exe
    # and sfr_cpu_diagnostic_b.exe. SFR_EXE names the file and is not passed on to the game.
    'exe-a'        = @{ SFR_EXE = 'sfr_cpu_diagnostic_a.exe' }
    'exe-b'        = @{ SFR_EXE = 'sfr_cpu_diagnostic_b.exe' }
    # the same file as exe-b under another name: a control that shows what comparing a program with itself gives
    'exe-b-again'  = @{ SFR_EXE = 'sfr_cpu_diagnostic_b.exe' }
    # samples the main thread every millisecond during the race (from present
    # 12200); profile.md names the functions (scripts/profile_summary.py)
    'profile'      = @{ SFR_MAIN_PROFILE = '1'; SFR_PROFILE_AFTER = '12200' }
}
# "a,b" arrives as one string through powershell -File.
$Configs = @($Configs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Configs.Count -or @($Configs | Select-Object -Unique).Count -ne $Configs.Count) { throw 'Provide distinct configurations.' }
if (-not $AllowDiagnosticRendering -and @($Configs | Where-Object { $_ -in 'skip-draws','render-every-2' }).Count) {
    throw 'Omitted drawing is a diagnostic ceiling, not gameplay performance; use -AllowDiagnosticRendering explicitly.'
}
foreach ($name in $Configs) {
    if (-not $settings.ContainsKey($name)) { throw "Unknown setting '$name'; known: $($settings.Keys -join ', ')" }
}

# A setting that names another executable must find it before anything runs.
foreach ($name in $Configs) {
    if ($settings[$name].ContainsKey('SFR_EXE') -and -not (Test-Path -LiteralPath (Join-Path $host_dir $settings[$name]['SFR_EXE']))) {
        throw "Setting '$name' needs $($settings[$name]['SFR_EXE']) in $host_dir (copy that build's sfr_cpu_diagnostic.exe to it)"
    }
}

# Two different executables must differ: copying the same build under both names compares
# a program with itself (a whole benchmark wasted before anyone notices).
$exeFiles = @{}
foreach ($name in $Configs) { if ($settings[$name].ContainsKey('SFR_EXE')) { $exeFiles[$settings[$name]['SFR_EXE']] = $true } }
if ($exeFiles.Count -ge 2) {
    $seen = @{}
    foreach ($file in $exeFiles.Keys) {
        $hash = Get-BenchmarkSha256 (Join-Path $host_dir $file)
        if ($seen.ContainsKey($hash)) { throw "$file and $($seen[$hash]) are the same program (same SHA-256): build the second one before copying it" }
        $seen[$hash] = $file
    }
}

# The launcher's defaults (launcher_settings.cpp game_environment), then what
# makes a run unattended and alike.
$base = [ordered]@{
    SFR_CALL_BUDGET = '18446744073709551615'; SFR_WATCHDOG_SECONDS = '31536000'
    SFR_ALLOW_RENDER_TARGETS = '1'; SFR_TRACE_GRAPHICS = '0'; SFR_DIAGNOSTIC_ENTRIES = '0'; SFR_TRACE_IMPORTS = '0'
    SFR_FRAME_LIMIT = $(if ($Capped) { '60' } else { '0' })
    SFR_RENDER_EVERY = '1'; SFR_PARALLEL_WORKER = 'all'; SFR_VERTEX_CACHE = '1'; SFR_GPU_PIPELINE = '1'
    SFR_AUDIO = $(if ($NoAudio) { '0' } else { '1' }); SFR_PROFILE = '1'; SFR_SKIP_MOVIES = '1'
    SFR_REALTIME_RACE = $(if ($FixedStep) { '0' } else { '1' }); SFR_REALTIME_UI = '1'; SFR_GRAPHICS = $Backend
    # No controller may take the run over: player 1 is the keyboard nobody touches and player 2 is off
    # (native_input.cpp knows both/gamepad/keyboard/off; 'none' fell back to both, so a pad that was
    # switched on took player 1 and the Kinect hand, and the menu words went unheard).
    SFR_GAME_LANGUAGE = 'en'; SFR_PLAYER1_INPUT = 'keyboard'; SFR_PLAYER2_INPUT = 'off'
    # Since v0.4.3 the per-frame NATIVE_PRESENT line is only written when asked for (tracing is off here)
    SFR_FRAME_METRICS = '1'
    SFR_WINDOW_WIDTH = '1280'; SFR_WINDOW_HEIGHT = '720'; SFR_FULLSCREEN = '0'; SFR_VSYNC = '0'
    SFR_NUI_HAND_CENTRED = '1'; SFR_SAY = $Say; SFR_PRESENT_LIMIT = "$PresentLimit"
    # The menus take time on the wall clock: on a fast PC a script by presents alone speaks too early
    # and a run may never reach the race (8 of 19 on a Ryzen AI MAX+ 395).
    SFR_SAY_MIN_SECONDS = '2'
}
if ($Stretch) {
    # Loading takes seconds, not presents (a PC presents hundreds of frames a second while the title
    # loads), so a word keyed by presents may be said before the menu listens, and a limit by presents
    # may come before the race. Each word waits for P/ReferenceFps seconds (and the title asks for
    # none while it loads); the run ends AfterSay presents after the last word, so it is counted
    # from the race. No limit counts from the start: a slow load presents its loading screen over
    # a thousand times a second, and on an i5-3470 one build used up 60000 presents before the
    # race in 2 of 6 runs. The backstop is the wall clock instead (10 minutes unless
    # -TimeoutMinutes says otherwise; a run takes 3 or 4). A fast PC plays AfterSay presents
    # in a short race, so after the warm-up the counted runs get as many presents as
    # -RaceSeconds of its race, when that is more (below).
    $base['SFR_SAY_REFERENCE_FPS'] = "$ReferenceFps"
    $base['SFR_PRESENT_LIMIT_AFTER_SAY'] = "$AfterSay"
    if (-not $PSBoundParameters.ContainsKey('PresentLimit')) { $base['SFR_PRESENT_LIMIT'] = '0' }
    if (-not $PSBoundParameters.ContainsKey('TimeoutMinutes')) { $TimeoutMinutes = 10 }
}
if (-not $Out) { $Out = Join-Path $root ('out/bench/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$Out = [IO.Path]::GetFullPath($Out)
if (Test-Path -LiteralPath $Out) { throw "Output already exists: $Out. Choose a new directory." }
$save_source = if ($SaveDirectory) { [IO.Path]::GetFullPath($SaveDirectory) } else { Join-Path $host_dir 'save' }
if ($SaveDirectory -and -not (Test-Path -LiteralPath $save_source -PathType Container)) { throw 'SaveDirectory does not exist.' }
if ($Out.Equals($save_source, [StringComparison]::OrdinalIgnoreCase) -or $Out.StartsWith($save_source.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output must not be inside the source save directory.'
}
New-Item -ItemType Directory -Path $Out | Out-Null
$fixture = Join-Path $Out 'fixture'
if (Test-Path -LiteralPath $save_source) { Copy-Item -Recurse -LiteralPath $save_source -Destination $fixture }
else {
    # A new player is asked at the title whether Omochao should teach them, a dialog only the
    # hand answers (docs/pad-menus.md): the menu words go unheard and no run reaches the race.
    Write-Warning "No save in $save_source`: the game will ask a new player about its tutorial, which the menu words cannot answer. Give -SaveDirectory a save that has been past the title once."
    New-Item -ItemType Directory -Path $fixture | Out-Null
}
Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($fixture.Length); sha256 = (Get-BenchmarkSha256 $_.FullName) }
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Out 'fixture-hashes.json') -Encoding UTF8

# A build that counts the race's own presents (SFR_PRESENT_LIMIT_RACING) ends every run after
# as many of them, the loading screen not counted (the limit after the last word counts it,
# and it presents hundreds of times a second on a fast PC). Every program of the comparison
# must know it, or a run would never end; -AfterSay given keeps the old limit.
$programs = @($exe) + @($Configs | Where-Object { $settings.Contains($_) -and $settings[$_].ContainsKey('SFR_EXE') } |
    ForEach-Object { Join-Path $host_dir $settings[$_]['SFR_EXE'] } | Where-Object { Test-Path -LiteralPath $_ })
$RacingLimit = $Stretch -and -not $PSBoundParameters.ContainsKey('AfterSay') -and
    -not @($programs | Where-Object { -not (Test-ProgramText $_ 'SFR_PRESENT_LIMIT_RACING') }).Count
if ($RacingLimit) {
    # With -FixedStep a race second is 60 presents on any PC; otherwise the warm-up says how many
    # presents -RaceSeconds are on this one (below), and it runs 60 a second until then.
    $frames = if ($RaceFrames -gt 0) { $RaceFrames } else { [math]::Max(1, $RaceSeconds) * 60 }
    $base.Remove('SFR_PRESENT_LIMIT_AFTER_SAY')
    $base['SFR_PRESENT_LIMIT_RACING'] = "$frames"
} elseif ($RaceFrames -gt 0) {
    Write-Warning '-RaceFrames needs a build with SFR_PRESENT_LIMIT_RACING (and no -AfterSay): the runs end -AfterSay presents after the last word instead.'
}

# What the numbers belong to.
$cpu = try { (Get-CimInstance Win32_Processor | Select-Object -First 1).Name } catch { 'unknown' }
$gpu = try { (Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name) -join '; ' } catch { 'unknown' }
# A kit has no git: make_benchmark_kit.ps1 leaves the commit it was made at in commit.txt.
$commit = try { (& git -C $root rev-parse --short HEAD 2>$null) } catch { $null }
if (-not $commit) {
    $recorded = Join-Path $root 'commit.txt'
    $commit = if (Test-Path -LiteralPath $recorded) { (Get-Content -LiteralPath $recorded -TotalCount 1) } else { 'unknown' }
}
# Which generated code the build compiled (SFR_DIAGNOSTIC_DIR in its CMake cache).
$generated = ''
$cache = Join-Path $host_dir 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    $line = Select-String -LiteralPath $cache -Pattern '^SFR_DIAGNOSTIC_DIR:[A-Z]+=(.*)$' | Select-Object -First 1
    if ($line) { $generated = $line.Matches[0].Groups[1].Value }
}
# What else decides the numbers on this PC: the driver, the power plan, mains or battery,
# and whether something else is busy before the first run starts.
$model = try { $system = Get-CimInstance Win32_ComputerSystem; "$($system.Manufacturer) $($system.Model)" } catch { 'unknown' }
$driver = try { (Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) $($_.DriverVersion)" }) -join '; ' } catch { 'unknown' }
# Windows' own plans by GUID, so info.txt reads the same in every language (hardware_probe.ps1 does
# the same); a maker's or a user's plan keeps the name Windows gives it.
$plan = try {
    $scheme = (powercfg /getactivescheme) -join ' '
    $known = @{ '381b4222-f694-41f0-9685-ff5bb260df2e' = 'Balanced'; '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c' = 'High performance'
                'a1841308-3541-4fab-bc81-f71556f20b4a' = 'Power saver'; 'e9a42b02-d5df-448d-aa00-03f14749eb61' = 'Ultimate Performance' }
    $guid = if ($scheme -match '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})') { $Matches[1].ToLower() } else { '' }
    if ($known.ContainsKey($guid)) { $known[$guid] } else { $scheme -replace '^.*\(([^)]*)\).*$', '$1' }
} catch { 'unknown' }
$battery = try { Get-CimInstance Win32_Battery -ErrorAction Stop } catch { $null }
$power = if (-not $battery) { 'no battery (desktop)' } elseif ($battery.BatteryStatus -in 2, 6, 7, 8, 9) { 'on mains' } else { 'ON BATTERY' }
$samples = @(1..5 | ForEach-Object { Start-Sleep -Seconds 1; try { (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average } catch { 0 } })
$idle = [math]::Round(($samples | Measure-Object -Average).Average)
if ($idle -gt 15) { Write-Warning "This PC is $idle% busy before the benchmark starts: close what is running, or the numbers will carry it." }
if ($power -eq 'ON BATTERY') { Write-Warning 'On battery: plug in, or the CPU and GPU will not run at full speed.' }
Write-Output "PC: $power, power plan '$plan', $idle% busy before the start"
@("commit=$commit", "generated=$generated", "model=$model", "cpu=$cpu", "gpu=$gpu", "driver=$driver", "power=$power plan=$plan idle_cpu_percent=$idle",
  "cold_pipelines=$([bool]$ColdPipelines)", "os=$([Environment]::OSVersion.VersionString)",
  "configs=$($Configs -join ',') repeats=$Repeats present_limit=$($base.SFR_PRESENT_LIMIT) after_say=$($base.SFR_PRESENT_LIMIT_AFTER_SAY) race_frames=$($base.SFR_PRESENT_LIMIT_RACING) scenario=$Scenario fixed_step=$([bool]$FixedStep) reference_fps=$($base.SFR_SAY_REFERENCE_FPS) stretch=$([bool]$Stretch) capped=$([bool]$Capped)",
  "say=$Say", "order=$(if ($FixedOrder) { 'fixed' } else { 'turning' })", "realtime_race=$($base.SFR_REALTIME_RACE)", "realtime_ui=1",
  "audio=$($base.SFR_AUDIO)", "backend=$Backend", "diagnostic_rendering=$([bool]$AllowDiagnosticRendering)") | Set-Content -LiteralPath (Join-Path $Out 'info.txt') -Encoding UTF8
# The executables an exe-a / exe-b comparison ran: which file, how big, when it was built.
foreach ($name in $Configs) {
    if ($settings[$name].ContainsKey('SFR_EXE')) {
        $file = Get-Item -LiteralPath (Join-Path $host_dir $settings[$name]['SFR_EXE'])
        Add-Content -LiteralPath (Join-Path $Out 'info.txt') -Encoding UTF8 -Value "$name=$($file.Name) bytes=$($file.Length) built=$($file.LastWriteTime.ToString('s')) sha256=$(Get-BenchmarkSha256 $file.FullName)"
    }
}
# What else about this PC can change the numbers, and what its drivers support
# (scripts\hardware_probe.ps1, which prints the same block by itself; the summary
# puts it first). A probe that fails says "unknown"; an older kit has none.
$probe = Join-Path $PSScriptRoot 'hardware_probe.ps1'
if (Test-Path -LiteralPath $probe) {
    $probed = try { . $probe; @(Get-HardwareProbe -HostDirectory $host_dir) } catch { @("probe=failed ($($_.Exception.Message))") }
    Add-Content -LiteralPath (Join-Path $Out 'info.txt') -Encoding UTF8 -Value $probed
}


$runs = New-Object System.Collections.Generic.List[object]
if (-not $NoWarmup) { $runs.Add(@('warmup', 1)) }
# The order turns by one place each round (a b c, then b c a, then c a b), so no setting is
# always the first or the last of its round: a run that comes later in a round was measured
# slower than the same program earlier in it (2.4% on one PC, same executable twice), and
# with a fixed order that would be charged to the setting that happens to come later.
for ($r = 1; $r -le $Repeats; ++$r) {
    $shift = if ($FixedOrder) { 0 } else { ($r - 1) % $Configs.Count }
    $order = if ($shift) { @($Configs | Select-Object -Skip $shift) + @($Configs | Select-Object -First $shift) } else { @($Configs) }
    foreach ($name in $order) { $runs.Add(@($name, $r)) }
}

function Start-Run([string]$name, [int]$repeat) {
    Get-ChildItem Env:SFR_* | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, $null) }
    foreach ($key in $base.Keys) { Set-Item "Env:$key" $base[$key] }
    foreach ($key in 'SFR_DXC_LIBRARY','SFR_SHADER_PACK') {
        if ($originalEnvironment.ContainsKey($key)) { Set-Item "Env:$key" $originalEnvironment[$key] }
    }
    if (-not $env:SFR_DXC_LIBRARY -and (Test-Path -LiteralPath (Join-Path $root 'dxc/dxcompiler.dll'))) { $env:SFR_DXC_LIBRARY = Join-Path $root 'dxc' }
    $changes = if ($name -eq 'warmup') { @{} } else { $settings[$name] }
    $program = $exe
    foreach ($key in $changes.Keys) {
        if ($key -eq 'SFR_EXE') {
            $program = Join-Path $host_dir $changes[$key]
            if (-not (Test-Path -LiteralPath $program)) { throw "Missing $program (copy that build's sfr_cpu_diagnostic.exe to it)" }
        } else { Set-Item "Env:$key" $changes[$key] }
    }
    # What a player gets: the recorded pipeline list shipped in data\pipeline-manifests,
    # prepared before the first frame. A checkout does not put it beside the shader
    # pack the way the packagers do, so it is named here (unless one is already there).
    $backend = if ($env:SFR_GRAPHICS -eq 'vulkan') { 'vulkan' } else { 'd3d12' }
    $shipped = Join-Path $root "data/pipeline-manifests/pipelines-$backend.manifest"
    if ((Test-Path -LiteralPath $shipped) -and -not (Test-Path -LiteralPath (Join-Path $root "out/shaders/pipelines-$backend.manifest"))) {
        $env:SFR_PIPELINE_MANIFEST = $shipped
    }
    $label = "$name-$repeat"
    $cacheLabel = if ($ColdPipelines) { $label } else { "warm-$backend" }
    $cacheRoot = Join-Path $Out "cache/$cacheLabel"
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
    $env:SFR_PIPELINE_CACHE_PATH = Join-Path $cacheRoot 'pipelines'
    $env:SFR_PIPELINE_MANIFEST_LOCAL = Join-Path $cacheRoot 'learned.manifest'
    $env:SFR_RUNTIME_SHADER_CACHE = Join-Path $cacheRoot 'shaders'
    # A fresh copy of the save each run: the same menus every time, and the
    # player's own save is never written.
    $save = Join-Path $Out "save-$label"
    Copy-Item -Recurse -LiteralPath $fixture -Destination $save
    $env:SFR_SAVE_DIRECTORY = $save
    # The warm-up shows where the menus went, should a run not reach the race.
    if ($name -eq 'warmup') {
        $env:SFR_SCREENSHOT = Join-Path $Out 'warmup-%d.bmp'
        $env:SFR_SCREENSHOT_EVERY = '1500'
    }
    if ($ScreenshotEvery -gt 0 -and $name -ne 'warmup') {
        $env:SFR_SCREENSHOT = Join-Path $Out "$label-%d.bmp"
        $env:SFR_SCREENSHOT_EVERY = "$ScreenshotEvery"
    }
    $log = Join-Path $Out "$label.log"
    $effective = [ordered]@{}
    Get-ChildItem Env:SFR_* | Sort-Object Name | ForEach-Object { $effective[$_.Name] = $_.Value }
    [ordered]@{ executable = $program; sha256 = (Get-BenchmarkSha256 $program); settings = $effective } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out "$label.json") -Encoding UTF8
    $arguments = @("`"$image`"", "`"$assets`"", '--game-region=ntsc-us')
    return Start-Process -FilePath $program -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput (Join-Path $Out "$label.out")
}

function Get-RaceFps([string]$logPath) {
    # A run's race frames a second: its racing=1 presents, first to last, by their seconds= clock.
    if (-not (Test-Path -LiteralPath $logPath)) { return $null }
    $reader = New-Object System.IO.StreamReader($logPath)
    try {
        $first = $null; $last = $null; $count = 0
        while ($null -ne ($line = $reader.ReadLine())) {
            if (-not $line.StartsWith('NATIVE_PRESENT ') -or -not $line.Contains(' racing=1 ')) { continue }
            if ($line -match ' seconds=([0-9.]+)') {
                $seconds = [double]$Matches[1]
                if ($null -eq $first) { $first = $seconds }
                $last = $seconds
                ++$count
            }
        }
        if ($count -lt 60 -or $last -le $first) { return $null }
        return ($count - 1) / ($last - $first)
    } finally { $reader.Dispose() }
}

    $started = Get-Date
    $index = 0
    $dead = @{}
    foreach ($run in $runs) {
        ++$index
        $name, $repeat = $run
        if ($dead.ContainsKey($name)) {
            # Its first run never reached the race: the others would sit there as long.
            Write-Output ("[{0}/{1}] {2}, run {3}: skipped ({4} never reached the race in run {5})" -f $index, $runs.Count, $name, $repeat, $name, $dead[$name])
            Set-Content -LiteralPath (Join-Path $Out "$name-$repeat.log") -Encoding ASCII -Value "STOP skipped: $name never reached the race in run $($dead[$name])"
            continue
        }
        $elapsed = (Get-Date) - $started
        $eta = if ($index -gt 1) { ' (left: about ' + [int](($elapsed.TotalMinutes / ($index - 1)) * ($runs.Count - $index + 1)) + ' min)' } else { '' }
        Write-Output ("[{0}/{1}] {2}, run {3}{4}" -f $index, $runs.Count, $name, $repeat, $eta)
        $process = Start-Run $name $repeat
        if ($index -eq 1) { Write-Output '  (the game runs hidden; Ctrl+C here stops it and the benchmark)' }
        # A second at a time: one long WaitForExit holds Ctrl+C until it returns.
        $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
        while (-not $process.WaitForExit(1000) -and (Get-Date) -lt $deadline) { }
        if (-not $process.HasExited) {
            Write-Output "  took over $TimeoutMinutes minutes: stopped"
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit()
            Add-Content -LiteralPath (Join-Path $Out "$name-$repeat.log") -Value 'STOP benchmark-timeout: host deadline reached'
        }
        # The run's own copy of the save has done its job (the next run starts from the
        # fixture again); 25 of them would be 600 MB. fixture\ keeps what every run started from.
        if (-not $KeepSaves) {
            $used = Join-Path $Out "save-$name-$repeat"
            try { if (Test-Path -LiteralPath $used) { Remove-Item -Recurse -Force -LiteralPath $used } }
            catch { Write-Warning "Could not remove ${used}: $($_.Exception.Message)" }
        }
        $raced = Select-String -LiteralPath (Join-Path $Out "$name-$repeat.log") -SimpleMatch ' racing=1 ' -Quiet
        if ($name -ne 'warmup' -and -not $raced) {
            Write-Warning "$name never reached the race in run $repeat (the log is $name-$repeat.log): its other runs are skipped."
            $dead[$name] = $repeat
        }
        if ($name -eq 'warmup' -and -not $raced) {
            # Every other run would sit on the same page until its timeout.
            throw ("The warm-up never reached a race, so the benchmark stops here: the warmup-*.bmp screenshots in $Out show where the menus stopped. " +
                "Omochao asking to teach you how to play means the save is a new player's: give -SaveDirectory a save that has been past the title once.")
        }
        if ($name -eq 'warmup' -and $RacingLimit -and -not $FixedStep -and $RaceFrames -eq 0 -and $RaceSeconds -gt 0) {
            # Racing by the clock, -RaceSeconds of race are as many presents as this PC draws in them.
            $fps = Get-RaceFps (Join-Path $Out 'warmup-1.log')
            if ($fps) {
                $needed = [int][math]::Ceiling($fps * $RaceSeconds)
                $base['SFR_PRESENT_LIMIT_RACING'] = "$needed"
                Write-Output ("  the warm-up raced at {0:0.#} fps: every run gets {1} race presents ({2} seconds)" -f $fps, $needed, $RaceSeconds)
                Add-Content -LiteralPath (Join-Path $Out 'info.txt') -Encoding UTF8 -Value "race_frames_from_warmup=$needed warmup_fps=$([math]::Round($fps, 1))"
            }
        } elseif ($name -eq 'warmup' -and -not $RacingLimit -and $Stretch -and $RaceSeconds -gt 0 -and -not $PSBoundParameters.ContainsKey('AfterSay')) {
            # A fast PC plays -AfterSay presents in a short race (4200 at 300 fps are 14 seconds), too
            # short for the summary's common stretch of race (docs/benchmark.md): the counted runs get
            # as many presents as -RaceSeconds of the warm-up's race, when that is more.
            $fps = Get-RaceFps (Join-Path $Out 'warmup-1.log')
            $needed = if ($fps) { [int][math]::Ceiling($fps * $RaceSeconds) } else { 0 }
            if ($needed -gt $AfterSay) {
                $base['SFR_PRESENT_LIMIT_AFTER_SAY'] = "$needed"
                Write-Output ("  the warm-up raced at {0:0.#} fps: every run gets {1} race presents ({2} seconds)" -f $fps, $needed, $RaceSeconds)
                Add-Content -LiteralPath (Join-Path $Out 'info.txt') -Encoding UTF8 -Value "after_say_from_warmup=$needed warmup_fps=$([math]::Round($fps, 1))"
            }
        }
    }

# The profile's addresses are named by the linker's map of this build.
$map = Join-Path $host_dir 'sfr_cpu_diagnostic.map'
if ($Configs -contains 'profile' -and (Test-Path -LiteralPath $map)) { Copy-Item -LiteralPath $map -Destination $Out }

# The py launcher first: 'python' may be the Microsoft Store's stand-in,
# which runs nothing.
$summary = Join-Path $PSScriptRoot 'benchmark_summary.py'
foreach ($python in @(@('py', '-3'), @('python'))) {
    if (-not (Get-Command $python[0] -ErrorAction SilentlyContinue)) { continue }
    $rest = @($python | Select-Object -Skip 1)
    & $python[0] @rest $summary $Out --skip $Skip
    if ($Configs -contains 'profile') { & $python[0] @rest (Join-Path $PSScriptRoot 'profile_summary.py') $Out }
    if (Test-Path -LiteralPath (Join-Path $Out 'summary.md')) { break }
}
# What goes to someone else is a copy without the user name, the language and
# country, the profile id and the screenshots (scripts\anonymize_benchmark.py);
# the folder itself stays as it is, for you.
foreach ($python in @(@('py', '-3'), @('python'))) {
    if (-not (Get-Command $python[0] -ErrorAction SilentlyContinue)) { continue }
    $rest = @($python | Select-Object -Skip 1)
    & $python[0] @rest (Join-Path $PSScriptRoot 'anonymize_benchmark.py') $Out --also $env:COMPUTERNAME --also $env:USERNAME
    if (Get-ChildItem -Path "$Out-shareable*.zip" -ErrorAction SilentlyContinue) { break }
}
if (Test-Path -LiteralPath (Join-Path $Out 'summary.md')) { Write-Output "Logs and summary.md are in $Out" }
else { Write-Output "No Python ran the summary; the logs are in $Out (python scripts\benchmark_summary.py `"$Out`")" }
} finally {
    if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    Get-ChildItem Env:SFR_* | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, $null) }
    foreach ($key in $originalEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key, $originalEnvironment[$key]) }
}
