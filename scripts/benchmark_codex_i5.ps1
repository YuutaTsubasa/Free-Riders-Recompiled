# Run this from the kit on the TARGET PC. Only preparation/export touches X:.
param(
    [string]$SettingsFile = '',
    [string]$LocalRoot = '',
    [string]$ResultsRoot = '',
    [ValidateRange(1,100)][int]$Repeats = 6,
    [ValidateRange(1,100)][int]$ControlRepeats = 2,
    [ValidateSet(0,5400)][int]$GameplayTicks = 0,
    [string]$ResumeFrom = '',
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
if ($GameplayTicks -and ($Repeats -lt 6 -or $ControlRepeats -lt 2)) {
    throw 'Semantic grouped comparison requires at least six pairs and two control pairs.'
}
function Get-I5FileHash([string]$Path) {
    # Same fallback as benchmark.ps1: Get-FileHash is absent on some PS5 hosts.
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}
$kit = Split-Path -Parent $PSScriptRoot
if (-not $SettingsFile) { $SettingsFile = Join-Path $kit 'play/settings.ini' }
if (-not $LocalRoot) { $LocalRoot = Join-Path $env:LOCALAPPDATA 'FreeRidersCodexBench' }
if (-not $ResultsRoot) { $ResultsRoot = Join-Path $kit 'i5-results' }
$SettingsFile = [IO.Path]::GetFullPath($SettingsFile)
$LocalRoot = [IO.Path]::GetFullPath($LocalRoot)
$ResultsRoot = [IO.Path]::GetFullPath($ResultsRoot)
if ($ResumeFrom) {
    if (-not $GameplayTicks) { throw 'ResumeFrom requires the semantic gameplay protocol.' }
    $ResumeFrom=(Resolve-Path -LiteralPath $ResumeFrom -ErrorAction Stop).Path
    if (-not $ResumeFrom.StartsWith($LocalRoot.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Resume source must be an existing session under LocalRoot.'
    }
}
$drive = New-Object IO.DriveInfo([IO.Path]::GetPathRoot($LocalRoot))
if ($drive.DriveType -ne [IO.DriveType]::Fixed) { throw 'LocalRoot must be on a local fixed disk, not X: or a network share.' }
if (-not (Test-Path -LiteralPath $SettingsFile -PathType Leaf)) { throw "Missing launcher settings: $SettingsFile" }
$paths = @{}
foreach ($line in Get-Content -LiteralPath $SettingsFile -Encoding UTF8) {
    if ($line -match '^\s*(image_directory|asset_directory)\s*=(.*)$') { $paths[$Matches[1]] = $Matches[2].Trim() }
}
foreach ($key in 'image_directory','asset_directory') {
    if (-not $paths[$key]) { throw "Fill $key in $SettingsFile first." }
    if (-not [IO.Path]::IsPathRooted($paths[$key])) { $paths[$key] = Join-Path (Split-Path -Parent $SettingsFile) $paths[$key] }
    $paths[$key] = (Resolve-Path -LiteralPath $paths[$key]).Path
    if (-not (Test-Path -LiteralPath $paths[$key] -PathType Container)) { throw "Missing game folder: $($paths[$key])" }
}
$pythonCommand = $null; $pythonPrefix = @()
foreach ($candidate in @(@('py','-3'), @('python'))) {
    if (-not (Get-Command $candidate[0] -ErrorAction SilentlyContinue)) { continue }
    $prefix = @($candidate | Select-Object -Skip 1)
    try { $version = & $candidate[0] @prefix -c 'import sys; print(sys.version_info.major*100+sys.version_info.minor)' 2>$null }
    catch { continue }
    if ($LASTEXITCODE -eq 0 -and "$version" -match '^\d+$' -and [int]$version -ge 308) { $pythonCommand = $candidate[0]; $pythonPrefix = $prefix; break }
}
if (-not $pythonCommand) { throw 'Python 3.8+ is required to check results. Install Python 3 on this PC, then run again.' }
$manifest = Get-Content -LiteralPath (Join-Path $kit 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$sourceFiles = @{}
foreach ($entry in $manifest.files) {
    $source = [IO.Path]::GetFullPath((Join-Path $kit $entry.path))
    if (-not $source.StartsWith($kit.TrimEnd('\','/') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid path in kit manifest.' }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Incomplete kit: $source" }
    $sourceFiles[$entry.path] = $source
}
$copyBytes = 0L
foreach ($source in $sourceFiles.Values) { $copyBytes += (Get-Item -LiteralPath $source).Length }
$copyGame = @{}
foreach ($key in $paths.Keys) {
    $copyGame[$key] = $true
    if (-not $paths[$key].StartsWith('\\')) {
        $sourceDrive = New-Object IO.DriveInfo([IO.Path]::GetPathRoot($paths[$key]))
        $copyGame[$key] = $sourceDrive.DriveType -ne [IO.DriveType]::Fixed
    }
    if ($copyGame[$key]) { $copyBytes += (Get-ChildItem -LiteralPath $paths[$key] -Recurse -File | Measure-Object Length -Sum).Sum }
}
if ($drive.AvailableFreeSpace -lt $copyBytes + 4GB) { throw 'Not enough local free space for the kit, game data and 4 GiB of result headroom.' }
$id = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$session = Join-Path $LocalRoot $id
$runtime = Join-Path $session 'runtime'
$results = Join-Path $session 'results'
$export = Join-Path $ResultsRoot $id
if ((Test-Path -LiteralPath $session) -or (Test-Path -LiteralPath $export)) { throw 'Session already exists; run again to use a new session.' }
New-Item -ItemType Directory -Path $runtime,$results | Out-Null
$state = [ordered]@{
    id=$id; status='preparing'; started=(Get-Date).ToString('o'); completed=$null; error=$null
    source_kit=$kit; settings_file=$SettingsFile; source_game_paths=$paths
    game_paths=@{}; game_copied=$copyGame
    local_runtime=$runtime; local_results=$results; export_path=$export
    candidate_commit=$manifest.candidate_commit; candidate_sha256=$manifest.candidate_sha256
    baseline_sha256=$manifest.baseline_sha256; repeats=$Repeats; control_repeats=$ControlRepeats
    race_frames=5400; skip=1200; window_end=4500; stages=@(); managed_texture_state=$true; managed_stencil_state=$true
}
if ($GameplayTicks) {
    $state['protocol']='gameplay_v1'
    $state['gameplay_ticks']=$GameplayTicks
    $state['race_frames']=0
    $state['expected_game_config']=@(1,2,1,0,3,60,0)
    $state['route_say']='ok@580,ok@930,ok@1280,start@1630,ok@1830,right@2030,ok@2230,ok@2530,ok@2830,right@3130,ok@3430'
    $state['preflight']=@()
    $state['fixture_files']=@()
    $state['baseline_commit']=$manifest.baseline_commit
    $state['candidate_runtime_commit']=$manifest.candidate_runtime_commit
}
function Save-State {
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $results 'suite.json') -Encoding UTF8
}
$previousLocation = Get-Location
$savedDxc = $env:SFR_DXC_LIBRARY; $savedPack = $env:SFR_SHADER_PACK
$failure = $null; $transcriptStarted = $false; $suiteMutex=$null; $mutexOwned=$false
function Assert-SemanticIdle([string]$Stage) {
    $prior=$state.status; $waitStarted=Get-Date; $quietSamples=0
    try {
        while ($quietSamples -lt 2) {
            $games=@(Get-Process -Name 'sfr_cpu_diagnostic*','FreeRidersRecompiled' -ErrorAction SilentlyContinue)
            $cpu=(Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'" -ErrorAction Stop).PercentProcessorTime
            if ($null -eq $cpu) { throw 'Cannot verify target load before stage.' }
            if (-not $games.Count -and $cpu -le 20) { $quietSamples++ } else { $quietSamples=0 }
            if ($quietSamples -lt 2) {
                $state.status='waiting_for_idle'; $state['waiting_stage']=$Stage
                $state['waiting_load']=@{cpu_percent=$cpu; game_processes=$games.Count}
                Save-State
                Start-Sleep -Seconds 5
            }
        }
    } finally {
        $state.status=$prior; $state.Remove('waiting_stage'); $state.Remove('waiting_load')
    }
    $state.preflight+=@{stage=$Stage; cpu_percent=[int]$cpu; game_processes=0; admitted=$true; waited_seconds=((Get-Date)-$waitStarted).TotalSeconds}
    Save-State
}
try {
    if ($GameplayTicks -and -not $PrepareOnly) {
        $suiteMutex=New-Object Threading.Mutex($false,'Global\FreeRidersCodexSemanticSuite')
        try {$mutexOwned=$suiteMutex.WaitOne(0)} catch [Threading.AbandonedMutexException] {$mutexOwned=$true}
        if (-not $mutexOwned) { throw 'Another semantic suite owns the target admission lock.' }
        Assert-SemanticIdle 'prepare'
    }
    Start-Transcript -LiteralPath (Join-Path $results 'console.txt') | Out-Null
    $transcriptStarted = $true
    Save-State
    Write-Host "Local session: $session"
    Write-Host ('Copying and checking {0:N2} GiB. No benchmark runs on the network share.' -f ($copyBytes / 1GB))
    foreach ($entry in $manifest.files) {
        $target = Join-Path $runtime $entry.path
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $sourceFiles[$entry.path] -Destination $target
        # Launcher settings are intentionally editable after packaging.
        if (($entry.path -replace '\\','/') -ne 'play/settings.ini') {
            if ((Get-I5FileHash $target) -ne $entry.sha256) { throw "Kit checksum mismatch: $($entry.path)" }
        }
    }
    Copy-Item -LiteralPath (Join-Path $kit 'manifest.json') -Destination $runtime
    Copy-Item -LiteralPath (Join-Path $kit 'manifest.json') -Destination $results
    foreach ($name in 'image','assets') {
        $key = if ($name -eq 'image') { 'image_directory' } else { 'asset_directory' }
        if ($copyGame[$key]) {
            New-Item -ItemType Directory -Force -Path (Join-Path $runtime 'game') | Out-Null
            $state.game_paths[$key] = Join-Path $runtime "game/$name"
            Copy-Item -LiteralPath $paths[$key] -Destination $state.game_paths[$key] -Recurse
        } else {
            $state.game_paths[$key] = $paths[$key]
            Write-Host "Using existing local game data (read only): $($paths[$key])"
        }
    }
    if ($GameplayTicks) {
        foreach ($binary in @(
            @{path='out/build/host/sfr_cpu_diagnostic.exe'; hash=$state.candidate_sha256},
            @{path='out/build/host/sfr_cpu_diagnostic_before.exe'; hash=$state.baseline_sha256}
        )) {
            if ((Get-I5FileHash (Join-Path $runtime $binary.path)) -ne $binary.hash) { throw "Staged executable differs from suite identity: $($binary.path)" }
        }
        foreach ($required in 'benchmark_gameplay.py','benchmark_codex_gameplay_report.py','benchmark_summary.py') {
            if (-not (Test-Path -LiteralPath (Join-Path $runtime "scripts/$required"))) { throw "Missing semantic validator: $required" }
        }
        $fixture=Join-Path $runtime 'out/build/host/save'
        $state.fixture_files=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object {
            @{path=($_.FullName.Substring($fixture.Length+1) -replace '\\','/'); sha256=(Get-I5FileHash $_.FullName)}
        })
        if (-not $state.fixture_files.Count) { throw 'Semantic benchmark requires a nonempty save fixture.' }
    }
    if ($ResumeFrom) {
        $sourceResults=Join-Path $ResumeFrom 'results'
        $checkText=& $pythonCommand @pythonPrefix (Join-Path $runtime 'scripts/benchmark_codex_gameplay_report.py') --resume-check $sourceResults
        if ($LASTEXITCODE -ne 0) { throw "Source resume integrity failed: $checkText" }
        $check=($checkText -join "`n") | ConvertFrom-Json
        if (-not $check.resume_valid) { throw 'Source resume integrity did not pass.' }
        foreach ($key in 'baseline_sha256','candidate_sha256','gameplay_ticks','skip','window_end','repeats','control_repeats','route_say','baseline_commit','candidate_runtime_commit') {
            if ([string]$check.source_params.$key -cne [string]$state[$key]) { throw "Resume parameter differs: $key" }
        }
        if (($check.source_params.expected_game_config -join ',') -ne ($state.expected_game_config -join ',')) { throw 'Resume native configuration differs.' }
        $oldFixture=@($check.source_params.fixture_files | ForEach-Object { ($_.path -replace '\\','/').ToLowerInvariant()+':'+$_.sha256.ToUpperInvariant() } | Sort-Object)
        $newFixture=@($state.fixture_files | ForEach-Object { ($_.path -replace '\\','/').ToLowerInvariant()+':'+$_.sha256.ToUpperInvariant() } | Sort-Object)
        if (($oldFixture -join "`n") -cne ($newFixture -join "`n")) { throw 'Resume save fixture differs.' }
        $state['resumed_from']=$ResumeFrom
        $state['resume_source_suite']=$check.source_suite
        $state['import_inventory']=@()
        foreach ($name in 'verify','control') {
            $sourceStage=Join-Path $sourceResults $name
            $targetStage=Join-Path $results $name
            if (Test-Path -LiteralPath $targetStage) { throw 'Resume destination must be fresh.' }
            Copy-Item -LiteralPath $sourceStage -Destination $targetStage -Recurse
            foreach ($file in Get-ChildItem -LiteralPath $sourceStage -File -Recurse) {
                $relative=$file.FullName.Substring($sourceStage.Length+1)
                $hash=Get-I5FileHash $file.FullName
                if ((Get-I5FileHash (Join-Path $targetStage $relative)) -ne $hash) { throw "Resume copy hash differs: $name/$relative" }
                $state.import_inventory+=@{path="$name/$relative"; sha256=$hash}
            }
        }
        $state.stages=@($check.stages)
        $state.preflight+=@($check.preflight | Where-Object { $_.stage -in 'verify','control' })
        Save-State
    }
    # The fixed benchmark save is separate from the user's play/save.
    $env:SFR_DXC_LIBRARY = Join-Path $runtime 'dxc'
    $env:SFR_SHADER_PACK = Join-Path $runtime 'out/shaders/shaders.pack'
    Set-Location -LiteralPath $runtime
    $state.status = 'prepared'; Save-State
    if (-not $PrepareOnly) {
        $launches = 3 + 2 * $ControlRepeats + $(if($GameplayTicks){2}else{3}) * $Repeats
        if ($ResumeFrom) { $launches=1+2*$Repeats }
        Write-Host "$launches remaining game launches (including warm-ups)."
        Write-Host 'Game windows are hidden; this console shows progress. Ctrl+C stops the suite. Avoid other heavy work.'
        $stages = @(
            @{Name='verify'; Configs=@('verify-buffers'); Count=1; Verify=$true},
            @{Name='control'; Configs=@('codex-before','codex-control'); Count=$ControlRepeats; Verify=$false},
            @{Name='compare'; Configs=@('codex-before','codex-after','managed-buffers'); Count=$Repeats; Verify=$false}
        )
        if ($GameplayTicks) {
            $stages=@(
                @{Name='verify'; Configs=@('semantic-verify'); Count=1; Verify=$true},
                @{Name='control'; Configs=@('semantic-before','semantic-control'); Count=$ControlRepeats; Verify=$false},
                @{Name='compare'; Configs=@('semantic-before','semantic-after'); Count=$Repeats; Verify=$false}
            )
        }
        foreach ($stage in $stages) {
            if ($ResumeFrom -and $stage.Name -in 'verify','control') { continue }
            if ($GameplayTicks) { Assert-SemanticIdle $stage.Name }
            $record = [ordered]@{name=$stage.Name; configs=$stage.Configs; repeats=$stage.Count; status='running'}
            $state.stages += $record; $state.status='running'; Save-State
            Write-Host "=== $($stage.Name) ==="
            $arguments = @{
                Configs=$stage.Configs; Repeats=$stage.Count; Scenario='solo'; FixedStep=$true
                RaceFrames=5400; Skip=1200; ReferenceFps=40; TimeoutMinutes=25; Backend='d3d12'
                SkipBuildCheck=$true; StopOnFailure=$true; NoShareArchive=$true
                ImageDirectory=$state.game_paths.image_directory; AssetDirectory=$state.game_paths.asset_directory
                SaveDirectory=(Join-Path $runtime 'out/build/host/save'); Out=(Join-Path $results $stage.Name)
            }
            if ($stage.Verify) { $arguments.NoWarmup=$true; $arguments.ScreenshotEvery=1500 }
            if ($GameplayTicks) {
                $arguments.Remove('RaceFrames')
                $arguments.GameplayTicks=$GameplayTicks
                $arguments.TargetAdmission=$true
                $arguments.Say=$state.route_say
            }
            & (Join-Path $runtime 'scripts/benchmark.ps1') @arguments
            $record.status='passed'; Save-State
        }
        $state.status='completed'
    }
} catch {
    $failure = $_.Exception.Message
    $state.status='failed'; $state.error=$failure
    foreach ($stageRecord in $state.stages) {
        if ($stageRecord.status -eq 'running') { $stageRecord.status='failed'; $stageRecord.error=$failure }
    }
    Write-Warning $failure
} finally {
    if ($state.status -in 'preparing','running') { $state.status='interrupted'; $state.error='Suite interrupted before completion.' }
    $state.completed=(Get-Date).ToString('o'); Save-State
    $reportScript=if($GameplayTicks){'benchmark_codex_gameplay_report.py'}else{'benchmark_codex_i5_report.py'}
    if (-not $PrepareOnly -and (Test-Path -LiteralPath (Join-Path $runtime "scripts/$reportScript"))) {
        try {
            & $pythonCommand @pythonPrefix (Join-Path $runtime "scripts/$reportScript") $results
            if ($LASTEXITCODE -ne 0) { throw 'Result validation failed; see report.md and suite.json.' }
        } catch {
            $validationError=$_.Exception.Message
            if (-not $failure) { $failure=$validationError; $state.error=$failure }
            $state.validation_error=$validationError; $state.status='failed'; Save-State
            Write-Warning $validationError
        }
    }
    Set-Location -LiteralPath $previousLocation
    $env:SFR_DXC_LIBRARY=$savedDxc; $env:SFR_SHADER_PACK=$savedPack
    if ($transcriptStarted) { Stop-Transcript | Out-Null }
    try {
        New-Item -ItemType Directory -Force -Path $ResultsRoot | Out-Null
        if (Test-Path -LiteralPath $export) { throw 'Export folder already exists; local results preserved.' }
        Copy-Item -LiteralPath $results -Destination $export -Recurse
        Write-Host "Results saved without compression: $export"
    } catch {
        $failure="Export failed: $($_.Exception.Message)"
        Write-Warning "$failure Local results remain at $results"
    }
    Write-Host "Local copy retained: $session"
    if($mutexOwned){$suiteMutex.ReleaseMutex()}
    if($suiteMutex){$suiteMutex.Dispose()}
}
if ($failure -or $state.status -notin 'prepared','completed') { throw "The i5 suite did not complete successfully. $failure" }
