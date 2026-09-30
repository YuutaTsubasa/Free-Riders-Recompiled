param(
    [string]$PackageDirectory = $PSScriptRoot,
    [ValidateSet('on','off')][string]$Trace = 'on',
    [ValidateSet('notify','poll')][string]$Suspension = 'notify',
    [ValidateSet('on','off')][string]$Timer = 'off',
    [ValidateSet('on','off')][string]$ConstantReuse = 'off',
    [ValidateSet('on','off')][string]$HostTiming = 'off',
    [ValidateSet('on','off')][string]$Priority = 'off'
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$launcher = Join-Path $package 'FreeRidersRecompiled.exe'
$game = Join-Path $package 'sfr_cpu_diagnostic.exe'
if (!(Test-Path -LiteralPath $launcher) -or !(Test-Path -LiteralPath $game)) {
    throw 'Extract the full diagnostic package before running this script.'
}
if (Get-Process sfr_cpu_diagnostic -ErrorAction SilentlyContinue) {
    throw 'Close the running game first. Do not record two games at once.'
}
$power = Read-Host 'Power/TDP mode (optional)'
$plugged = Read-Host 'Plugged into power? (yes/no)'
$course = Read-Host 'Course to test (optional)'
$output = Join-Path $package ('benchmarks/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $Suspension + '-wait-' + $Trace + '-timer-' + $Timer + '-constants-' + $ConstantReuse + '-host-' + $HostTiming + '-priority-' + $Priority)
New-Item -ItemType Directory -Path $output | Out-Null
$variables = @('SFR_WAIT_TRACE','SFR_WAIT_GRAPH','SFR_FRAME_METRICS','SFR_TRACE_INPUT',
    'SFR_MAIN_PROFILE','SFR_HOST_PROFILE','SFR_SAMPLE_PROFILE','SFR_PROFILE_GUEST',
    'SFR_SUSPEND_NOTIFY','SFR_TIMER_RESOLUTION','SFR_TRACE_WAIT_RESULTS',
    'SFR_CONSTANT_UPLOAD_REUSE','SFR_CONSTANT_REUSE_TRACE',
    'SFR_HOST_TIMING','SFR_GUEST_SATURATED_PRIORITY')
$previous = @{}
foreach ($name in $variables) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
    [Environment]::SetEnvironmentVariable($name,$null,'Process')
}
$started = [DateTime]::UtcNow
try {
    [Environment]::SetEnvironmentVariable('SFR_WAIT_TRACE',$(if ($Trace -eq 'on') {'1'} else {'0'}),'Process')
    [Environment]::SetEnvironmentVariable('SFR_FRAME_METRICS','1','Process')
    [Environment]::SetEnvironmentVariable('SFR_TRACE_INPUT','0','Process')
    [Environment]::SetEnvironmentVariable('SFR_TRACE_WAIT_RESULTS','0','Process')
    [Environment]::SetEnvironmentVariable('SFR_SUSPEND_NOTIFY',$(if ($Suspension -eq 'notify') {'1'} else {'0'}),'Process')
    [Environment]::SetEnvironmentVariable('SFR_TIMER_RESOLUTION',$(if ($Timer -eq 'on') {'1'} else {'0'}),'Process')
    [Environment]::SetEnvironmentVariable('SFR_CONSTANT_UPLOAD_REUSE',$(if ($ConstantReuse -eq 'on') {'1'} else {'0'}),'Process')
    [Environment]::SetEnvironmentVariable('SFR_HOST_TIMING',$(if ($HostTiming -eq 'on') {'1'} else {'0'}),'Process')
    [Environment]::SetEnvironmentVariable('SFR_GUEST_SATURATED_PRIORITY',$(if ($Priority -eq 'on') {'1'} else {'0'}),'Process')
    Write-Host 'Keep the current graphics/input settings. Disable Skip movies for this capture.'
    Write-Host 'Watch the complete Intro (about 75 seconds), then play the slow course for about 2 minutes.'
    Write-Host 'Close BOTH game and launcher when done. Leave this console open until the ZIP is saved.'
    # The user needs this interactive launcher to configure and start the game.
    $process = Start-Process -FilePath $launcher -WorkingDirectory $package -WindowStyle Normal -PassThru -Wait
    $log = Join-Path $package 'game.log'
    if (!(Test-Path -LiteralPath $log) -or (Get-Item -LiteralPath $log).LastWriteTimeUtc -lt $started) {
        throw 'No new game.log was produced. Start the game before closing the launcher.'
    }
    Copy-Item -LiteralPath $log -Destination $output
    $settings = Join-Path $package 'settings.ini'
    if (Test-Path -LiteralPath $settings) { Copy-Item -LiteralPath $settings -Destination $output }
    $text = [IO.File]::ReadAllText($log)
    $metadata = [ordered]@{
        trace = $Trace
        suspension = $Suspension
        timerResolution = $Timer
        constantUploadReuse = $ConstantReuse
        hostTiming = $HostTiming
        saturatedGuestPriority = $Priority
        startedUtc = $started.ToString('o')
        endedUtc = [DateTime]::UtcNow.ToString('o')
        executableSha256 = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash
        launcherExitCode = $process.ExitCode
        processor = $env:PROCESSOR_IDENTIFIER
        logicalProcessors = $env:NUMBER_OF_PROCESSORS
        operatingSystem = [Environment]::OSVersion.VersionString
        powerMode = $power; pluggedIn = $plugged; course = $course
        frameRows = ([regex]::Matches($text,'(?m)^NATIVE_PRESENT ')).Count
        waitRows = ([regex]::Matches($text,'(?m)^WAIT_TRACE frame=')).Count
        movieRows = ([regex]::Matches($text,'(?m)^MOVIE_TRACE ')).Count
        closedNormally = $text.Contains('STOP window-closed')
        captureStatus = 'captured-unverified'
    }
    $metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'run.json') -Encoding UTF8
    $zip = $output + '.zip'
    Compress-Archive -LiteralPath $output -DestinationPath $zip
    Write-Host "Send this ZIP back: $zip"
    if (!$metadata.closedNormally -or !$metadata.frameRows -or ($Trace -eq 'on' -and !$metadata.waitRows)) {
        Write-Warning 'Capture is incomplete or lacks diagnostic rows; return the ZIP for diagnosis anyway.'
    }
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') }
}
