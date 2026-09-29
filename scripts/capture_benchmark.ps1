param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [ValidateSet('fast', 'legacy')][string]$Mode = 'fast'
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$launcher = Join-Path $package 'FreeRidersRecompiled.exe'
$game = Join-Path $package 'sfr_cpu_diagnostic.exe'
if (!(Test-Path -LiteralPath $launcher) -or !(Test-Path -LiteralPath $game)) {
    throw 'Select an extracted Windows package containing both executables.'
}
$output = Join-Path $package ('benchmarks/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $Mode)
New-Item -ItemType Directory -Path $output | Out-Null
$inherited = @{}
Get-ChildItem Env:SFR_* | ForEach-Object { $inherited[$_.Name] = $_.Value }
$variables = @('SFR_GPU_WAIT_FAST', 'SFR_MAIN_AFFINITY', 'SFR_WORKER_AFFINITY', 'SFR_FRAME_METRICS', 'SFR_TRACE_INPUT', 'SFR_MAIN_PROFILE', 'SFR_HOST_PROFILE', 'SFR_SAMPLE_PROFILE', 'SFR_PROFILE_GUEST')
$previous = @{}
foreach ($name in $variables) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
$start = [DateTime]::UtcNow
try {
    [Environment]::SetEnvironmentVariable('SFR_FRAME_METRICS', '1', 'Process')
    [Environment]::SetEnvironmentVariable('SFR_TRACE_INPUT', '0', 'Process')
    foreach ($name in @('SFR_GPU_WAIT_FAST', 'SFR_MAIN_AFFINITY', 'SFR_WORKER_AFFINITY')) {
        [Environment]::SetEnvironmentVariable($name, $(if ($Mode -eq 'legacy') { '0' } else { '1' }), 'Process')
    }
    Write-Host 'Use the same backend, settings, character, gear and course for each run. Close the game and launcher when done.'
    Write-Host 'Normal 60 FPS pacing is retained. Run legacy / fast / fast / legacy after warming the course once.'
    # This is the interactive launcher the player needs to use, not a helper window.
    $process = Start-Process -FilePath $launcher -WorkingDirectory $package -WindowStyle Normal -PassThru -Wait
    $log = Join-Path $package 'game.log'
    if (!(Test-Path -LiteralPath $log) -or (Get-Item -LiteralPath $log).LastWriteTimeUtc -lt $start) {
        throw 'No new game.log was produced; this run is not a measurement.'
    }
    Copy-Item -LiteralPath $log -Destination (Join-Path $output 'game.log')
    $settings = Join-Path $package 'settings.ini'
    if (Test-Path -LiteralPath $settings) { Copy-Item -LiteralPath $settings -Destination $output }
    $metadata = [ordered]@{
        mode = $Mode
        startedUtc = $start.ToString('o')
        endedUtc = [DateTime]::UtcNow.ToString('o')
        executableSha256 = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash
        launcherExitCode = $process.ExitCode
        processor = $env:PROCESSOR_IDENTIFIER
        logicalProcessors = $env:NUMBER_OF_PROCESSORS
        operatingSystem = [Environment]::OSVersion.VersionString
        inheritedDiagnosticEnvironment = $inherited
    }
    $metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'run.json') -Encoding UTF8
    Write-Host "Saved: $output"
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}
