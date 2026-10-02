# Measures a Free Race without anyone playing: the game starts, says the menu
# words that reach a race (SFR_SAY, docs/race-controls.md), races with nobody
# at the controls and stops itself after -PresentLimit frames. Each setting
# in -Configs runs -Repeats times, taking turns, and the race frames of every
# run are summed up by scripts/benchmark_summary.py (docs/benchmark.md).
#
#   scripts\benchmark.ps1                      # the default comparison
#   scripts\benchmark.ps1 -Configs baseline -Repeats 1
#
# Build first: scripts\build_tools.ps1 -Diagnostic. The game runs with the
# launcher's defaults and nothing of yours is touched: no Kinect, no sound,
# and a copy of your save in the run's own directory.
param(
    [string[]]$Configs = @('baseline', 'sequential', 'serial', 'skip-draws'),
    [int]$Repeats = 2,
    # The menu words and the presents they are said at: title, main menu,
    # one turn of the ring (the ring wraps: Free Race is left of World Grand Prix;
    # if `left` does nothing, five `right` do it), then rules, course, character and
    # gear. By present 12000 the race is running.
    [string]$Say = 'ok@580,ok@930,ok@1280,start@1630,ok@1830,left@2030,ok@2230,ok@2530,ok@2830,ok@3130,right@3430,ok@3730',
    [int]$PresentLimit = 15600,
    [switch]$NoStretch,               # the words by presents alone and the run ends at -PresentLimit (it cannot finish on a PC that presents fast)
    [double]$ReferenceFps = 100,
    [int]$AfterSay = 4200,
    [int]$Skip = 600,                 # race frames left out at the start
    [int]$TimeoutMinutes = 25,        # a run that takes longer is stopped
    [int]$ScreenshotEvery = 0,         # a screenshot every N presents in every run (a diagnostic: it slows the run)
    [switch]$NoWarmup,                # the first run fills the shader caches
    [switch]$SkipBuildCheck,          # a copied folder (no git): its files' times say nothing about the build
    [switch]$Capped,                  # 60 fps as when playing, not as fast as it goes
    [switch]$ColdPipelines,           # every run starts without pipeline-cache: a new player's first race
    [string]$ImageDirectory = '',     # the game folders, when they are not where this checkout keeps them
    [string]$AssetDirectory = '',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$Stretch = -not $NoStretch
$root = Split-Path -Parent $PSScriptRoot
# A kit keeps its own DXC next to the scripts (run_benchmark.bat sets this too); without it nothing is drawn
if (-not $env:SFR_DXC_LIBRARY -and (Test-Path -LiteralPath (Join-Path $root 'dxc\dxcompiler.dll'))) { $env:SFR_DXC_LIBRARY = Join-Path $root 'dxc' }
$host_dir = Join-Path $root 'out/build/host'
$exe = Join-Path $host_dir 'sfr_cpu_diagnostic.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build first: scripts\build_tools.ps1 -Diagnostic (missing $exe)" }
if ($Stretch -and -not [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($exe)).Contains('SFR_PRESENT_LIMIT_AFTER_SAY')) {
    throw "$exe is older than -Stretch (no SFR_PRESENT_LIMIT_AFTER_SAY): build again, or take the program from a new kit"
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
    'no-vertex-cache' = @{ SFR_VERTEX_CACHE = '0' }
    'no-gpu-pipeline' = @{ SFR_GPU_PIPELINE = '0' }
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
    # Two executables of different builds in out/build/host, compared in the same
    # rounds: copy each build's sfr_cpu_diagnostic.exe to sfr_cpu_diagnostic_a.exe
    # and sfr_cpu_diagnostic_b.exe. SFR_EXE names the file and is not passed on to the game.
    'exe-a'        = @{ SFR_EXE = 'sfr_cpu_diagnostic_a.exe' }
    'exe-b'        = @{ SFR_EXE = 'sfr_cpu_diagnostic_b.exe' }
    # samples the main thread every millisecond during the race (from present
    # 12200); profile.md names the functions (scripts/profile_summary.py)
    'profile'      = @{ SFR_MAIN_PROFILE = '1'; SFR_PROFILE_AFTER = '12200' }
}
# "a,b" arrives as one string through powershell -File.
$Configs = @($Configs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($name in $Configs) {
    if (-not $settings.ContainsKey($name)) { throw "Unknown setting '$name'; known: $($settings.Keys -join ', ')" }
}

# A setting that names another executable must find it before anything runs.
foreach ($name in $Configs) {
    if ($settings[$name].ContainsKey('SFR_EXE') -and -not (Test-Path -LiteralPath (Join-Path $host_dir $settings[$name]['SFR_EXE']))) {
        throw "Setting '$name' needs $($settings[$name]['SFR_EXE']) in $host_dir (copy that build's sfr_cpu_diagnostic.exe to it)"
    }
}

# The launcher's defaults (launcher_settings.cpp game_environment), then what
# makes a run unattended and alike.
$base = [ordered]@{
    SFR_CALL_BUDGET = '18446744073709551615'; SFR_WATCHDOG_SECONDS = '31536000'
    SFR_ALLOW_RENDER_TARGETS = '1'; SFR_TRACE_GRAPHICS = '0'; SFR_DIAGNOSTIC_ENTRIES = '0'; SFR_TRACE_IMPORTS = '0'
    SFR_FRAME_LIMIT = $(if ($Capped) { '60' } else { '0' })
    SFR_RENDER_EVERY = '1'; SFR_PARALLEL_WORKER = 'cores'; SFR_VERTEX_CACHE = '1'; SFR_GPU_PIPELINE = '1'
    SFR_AUDIO = '0'; SFR_PROFILE = '1'; SFR_SKIP_MOVIES = '1'
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
    # may come before the race. Each word waits for P/ReferenceFps seconds; the run ends AfterSay
    # presents after the last word, with the present limit left as a backstop.
    $base['SFR_SAY_REFERENCE_FPS'] = "$ReferenceFps"
    $base['SFR_PRESENT_LIMIT_AFTER_SAY'] = "$AfterSay"
    if (-not $PSBoundParameters.ContainsKey('PresentLimit')) { $base['SFR_PRESENT_LIMIT'] = '60000' }
}
# Left unset, whatever this console has: the Kinect, the voice, a second
# player, the per-setting switches and the investigation aids.
$cleared = @('SFR_CAMERA', 'SFR_VOICE', 'SFR_INPUT_SCRIPT', 'SFR_INPUT_SCRIPT_2', 'SFR_GRAPHICS', 'SFR_HOST_PROCESSORS',
             'SFR_SKIP_DRAWS', 'SFR_SCREENSHOT', 'SFR_SCREENSHOT_EVERY', 'SFR_SAMPLE_PROFILE', 'SFR_HOST_PROFILE',
             'SFR_MAIN_PROFILE', 'SFR_AVATAR_MODEL', 'SFR_TWO_PLAYERS', 'SFR_PLAYER2_INPUT', 'SFR_PIPELINE_MANIFEST')
# Every setting's switch too, so one run's never carries into the next.
$cleared = @($cleared + @($settings.Values | ForEach-Object { $_.Keys }) | Sort-Object -Unique)
$touched = @($base.Keys) + $cleared
$saved = @{}
foreach ($name in ($touched | Sort-Object -Unique)) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }

if (-not $Out) { $Out = Join-Path $root ('out/bench/' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$save_source = Join-Path $host_dir 'save'

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
$plan = try { ((powercfg /getactivescheme) -join ' ') -replace '^.*\(([^)]*)\).*$', '$1' } catch { 'unknown' }
$battery = try { Get-CimInstance Win32_Battery -ErrorAction Stop } catch { $null }
$power = if (-not $battery) { 'no battery (desktop)' } elseif ($battery.BatteryStatus -in 2, 6, 7, 8, 9) { 'on mains' } else { 'ON BATTERY' }
$samples = @(1..5 | ForEach-Object { Start-Sleep -Seconds 1; try { (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average } catch { 0 } })
$idle = [math]::Round(($samples | Measure-Object -Average).Average)
if ($idle -gt 15) { Write-Warning "This PC is $idle% busy before the benchmark starts: close what is running, or the numbers will carry it." }
if ($power -eq 'ON BATTERY') { Write-Warning 'On battery: plug in, or the CPU and GPU will not run at full speed.' }
Write-Output "PC: $power, power plan '$plan', $idle% busy before the start"
@("commit=$commit", "generated=$generated", "model=$model", "cpu=$cpu", "gpu=$gpu", "driver=$driver", "power=$power plan=$plan idle_cpu_percent=$idle",
  "cold_pipelines=$([bool]$ColdPipelines)", "os=$([Environment]::OSVersion.VersionString)",
  "configs=$($Configs -join ',') repeats=$Repeats present_limit=$PresentLimit stretch=$([bool]$Stretch) capped=$([bool]$Capped)",
  "say=$Say") | Set-Content -LiteralPath (Join-Path $Out 'info.txt') -Encoding UTF8
# The executables an exe-a / exe-b comparison ran: which file, how big, when it was built.
foreach ($name in $Configs) {
    if ($settings[$name].ContainsKey('SFR_EXE')) {
        $file = Get-Item -LiteralPath (Join-Path $host_dir $settings[$name]['SFR_EXE'])
        Add-Content -LiteralPath (Join-Path $Out 'info.txt') -Encoding UTF8 -Value "$name=$($file.Name) bytes=$($file.Length) built=$($file.LastWriteTime.ToString('s'))"
    }
}


$runs = New-Object System.Collections.Generic.List[object]
if (-not $NoWarmup) { $runs.Add(@('warmup', 1)) }
for ($r = 1; $r -le $Repeats; ++$r) { foreach ($name in $Configs) { $runs.Add(@($name, $r)) } }

function Start-Run([string]$name, [int]$repeat) {
    foreach ($key in $cleared) { Remove-Item "Env:$key" -ErrorAction SilentlyContinue }
    foreach ($key in $base.Keys) { Set-Item "Env:$key" $base[$key] }
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
    if ($ColdPipelines) { Remove-Item -Recurse -Force -LiteralPath (Join-Path $root 'pipeline-cache') -ErrorAction SilentlyContinue }
    $label = "$name-$repeat"
    # A fresh copy of the save each run: the same menus every time, and the
    # player's own save is never written.
    $save = Join-Path $Out "save-$label"
    if (Test-Path -LiteralPath $save_source) { Copy-Item -Recurse -LiteralPath $save_source -Destination $save }
    else { New-Item -ItemType Directory -Force -Path $save | Out-Null }
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
    $arguments = @("`"$image`"", "`"$assets`"", '--game-region=ntsc-us')
    return Start-Process -FilePath $program -ArgumentList $arguments -WorkingDirectory $root -NoNewWindow -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput (Join-Path $Out "$label.out")
}

try {
    $started = Get-Date
    $index = 0
    foreach ($run in $runs) {
        ++$index
        $name, $repeat = $run
        $elapsed = (Get-Date) - $started
        $eta = if ($index -gt 1) { ' (left: about ' + [int](($elapsed.TotalMinutes / ($index - 1)) * ($runs.Count - $index + 1)) + ' min)' } else { '' }
        Write-Output ("[{0}/{1}] {2}, run {3}{4}" -f $index, $runs.Count, $name, $repeat, $eta)
        $process = Start-Run $name $repeat
        if (-not $process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
            Write-Output "  took over $TimeoutMinutes minutes: stopped"
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit()
        }
        Remove-Item -Recurse -Force -LiteralPath (Join-Path $Out "save-$name-$repeat") -ErrorAction SilentlyContinue
    }
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
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
