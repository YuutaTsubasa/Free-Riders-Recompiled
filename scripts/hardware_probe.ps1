# What this PC is, for a performance report (docs/benchmark.md): what can change
# the numbers (cores, memory, the GPUs and their drivers, the power mode, the
# display, Windows' own switches) and whether the driver is what the renderer
# needs. benchmark.ps1 dot-sources this and writes the lines into info.txt; run
# by itself it prints the same block, to paste into an issue:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\hardware_probe.ps1
#
# Windows PowerShell 5.1, no administrator rights. Every probe stands alone: one
# that fails says "unknown" and the rest go on, so the benchmark never fails for
# the probe. Nothing here says who the PC belongs to: no computer or user name,
# no serial numbers, no MAC addresses, no paths.
param(
    [string]$HostDirectory = ''   # where the program is (its D3D12\ folder): out\build\host of this checkout by default
)

function Get-HardwareProbe([string]$HostDirectory = '', [switch]$Basics) {
    # key=value lines, like the rest of info.txt (benchmark_report.py reads them
    # too). -Basics adds the model, CPU, GPU, OS and power lines benchmark.ps1
    # writes itself.
    $lines = New-Object System.Collections.Generic.List[string]
    $probes = New-Object System.Collections.Generic.List[object]
    function Add-Probe([string]$key, [scriptblock]$probe) { $probes.Add(@($key, $probe)) }

    if ($Basics) {
        Add-Probe 'model' { $system = Get-CimInstance Win32_ComputerSystem; "$($system.Manufacturer) $($system.Model)" }
        Add-Probe 'cpu' { (Get-CimInstance Win32_Processor | Select-Object -First 1).Name }
        Add-Probe 'gpu' { (Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name }) -join '; ' }
        Add-Probe 'driver' { (Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) $($_.DriverVersion)" }) -join '; ' }
        Add-Probe 'power' {
            $battery = Get-CimInstance Win32_Battery -ErrorAction Stop
            $scheme = (powercfg /getactivescheme) -join ' '
            # Windows' own plans by GUID, so the report reads the same in every language;
            # a maker's or a user's plan keeps the name Windows gives it.
            $known = @{ '381b4222-f694-41f0-9685-ff5bb260df2e' = 'Balanced'; '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c' = 'High performance'
                        'a1841308-3541-4fab-bc81-f71556f20b4a' = 'Power saver'; 'e9a42b02-d5df-448d-aa00-03f14749eb61' = 'Ultimate Performance' }
            $guid = if ($scheme -match '([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})') { $Matches[1].ToLower() } else { '' }
            $plan = if ($known.ContainsKey($guid)) { $known[$guid] } else { $scheme -replace '^.*\(([^)]*)\).*$', '$1' }
            $state = if (-not $battery) { 'no battery (desktop)' } elseif ($battery.BatteryStatus -in 2, 6, 7, 8, 9) { 'on mains' } else { 'ON BATTERY' }
            "$state plan=$plan"
        }
        Add-Probe 'os' { [Environment]::OSVersion.VersionString }
    }

    # Cores and threads, the clock the maker states, and a hybrid CPU's split:
    # Windows does not say which cores are which, but on a 12th to 14th
    # generation Core only the P-cores have two threads, so threads minus
    # cores is the P-cores and the rest are E-cores.
    Add-Probe 'cpu_cores' {
        $processors = @(Get-CimInstance Win32_Processor)
        $cores = ($processors | Measure-Object NumberOfCores -Sum).Sum
        $threads = ($processors | Measure-Object NumberOfLogicalProcessors -Sum).Sum
        $text = "$cores cores, $threads threads, max $($processors[0].MaxClockSpeed) MHz"
        if ($processors.Count -gt 1) { $text += ", $($processors.Count) sockets" }
        $name = "$($processors[0].Name)"
        if ($name -match '1[2-4]th Gen Intel' -and $threads -gt $cores -and $threads -lt 2 * $cores) {
            $text += " (hybrid: about $($threads - $cores) P-cores and $(2 * $cores - $threads) E-cores, read from the counts)"
        } elseif ($name -match 'Core\(TM\) Ultra|Core Ultra') { $text += ' (hybrid P/E cores; the split is not readable here)' }
        $text
    }
    # The instruction sets Windows reports present. The game is built for
    # Sandy Bridge (-march=sandybridge, CMakeLists.txt): it needs AVX, not
    # AVX2. Windows older than 10 2004 does not know the AVX questions and
    # answers no: a "no" there says nothing.
    Add-Probe 'cpu_features' {
        if (-not ('SfrProbe.ProcessorFeatures' -as [type])) {
            Add-Type -Namespace SfrProbe -Name ProcessorFeatures -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint feature);
'@ | Out-Null
        }
        $features = 'SfrProbe.ProcessorFeatures' -as [type]
        $answer = foreach ($pair in @(@('avx', 39), @('avx2', 40), @('avx512f', 41))) {
            "$($pair[0])=$(if ($features::IsProcessorFeaturePresent([uint32]$pair[1])) { 'yes' } else { 'no' })"
        }
        ($answer -join ' ') + ' (the game needs AVX; "no" on Windows before 10 2004 says nothing)'
    }
    # The memory, and its speed when the firmware tells it: an integrated GPU
    # draws from the same memory, so its speed is the GPU's.
    Add-Probe 'memory' {
        $total = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
        $text = "$total GB"
        $modules = @(Get-CimInstance Win32_PhysicalMemory -ErrorAction SilentlyContinue)
        if ($modules.Count) {
            $speed = ($modules | ForEach-Object { if ($_.ConfiguredClockSpeed) { $_.ConfiguredClockSpeed } else { $_.Speed } } |
                Where-Object { $_ } | Sort-Object -Unique) -join '/'
            $text += ", $($modules.Count) module$(if ($modules.Count -ne 1) { 's' })"
            if ($speed) { $text += " at $speed MT/s" }
        }
        $text
    }

    # Every GPU Windows knows: its driver's version and date, its memory (the
    # driver's own figure, from the registry; CIM's AdapterRAM stops at 4 GB)
    # and the display it drives. Which of them the game used is on the game's
    # own NATIVE_GRAPHICS line (the summary reads it).
    # @() outside the try: one controller comes out of it as itself, and a CIM instance
    # has no Count in Windows PowerShell 5.1 (gpu0 was then also written as unknown).
    $controllers = @(try { Get-CimInstance Win32_VideoController } catch { })
    $registered = try {
        Get-ChildItem -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -ErrorAction SilentlyContinue |
            Where-Object { $_.PSChildName -match '^\d{4}$' } |
            ForEach-Object { Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue }
    } catch { @() }
    $index = 0
    foreach ($controller in $controllers) {
        $current = $controller
        $key = "gpu$index"
        ++$index
        # A script block reads its variables when it runs: this one keeps its own controller.
        Add-Probe $key ({
            $text = "$($current.Name) driver=$($current.DriverVersion)"
            if ($current.DriverDate) { $text += " ($($current.DriverDate.ToString('yyyy-MM-dd')))" }
            $entry = $registered | Where-Object { $_.DriverDesc -eq $current.Name } | Select-Object -First 1
            # A driver that writes the size as bytes rather than a number costs only the size.
            $exact = try { if ($entry -and $entry.'HardwareInformation.qwMemorySize') { [int64]$entry.'HardwareInformation.qwMemorySize' } else { 0 } } catch { 0 }
            $capped = try { if ($current.AdapterRAM) { [int64][uint32]$current.AdapterRAM } else { 0 } } catch { 0 }
            if ($exact -gt 0) { $text += " vram_mb=$([math]::Round($exact / 1MB))" }
            elseif ($capped -ge 4GB - 1MB) { $text += ' vram_mb=4096+ (Windows tells no more than 4 GB here: see the game''s own line)' }
            elseif ($capped -gt 0) { $text += " vram_mb=$([math]::Round($capped / 1MB))" }
            if ($current.CurrentRefreshRate -and $current.CurrentHorizontalResolution) {
                $text += " display=$($current.CurrentHorizontalResolution)x$($current.CurrentVerticalResolution)@$($current.CurrentRefreshRate)"
            }
            $text
        }.GetNewClosure())
    }
    if (-not $controllers.Count) { Add-Probe 'gpu0' { 'unknown' } }

    # A laptop runs on what its power mode allows: the chassis, the battery,
    # and Windows 11's power mode (an overlay on the plan) for mains and for
    # battery. A maker's own mode (Turbo, Silent) is not readable here.
    Add-Probe 'chassis' {
        $types = @((Get-CimInstance Win32_SystemEnclosure).ChassisTypes)
        $portable = @(8, 9, 10, 11, 12, 14, 18, 21, 30, 31, 32)
        $battery = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)
        $kind = if ($types | Where-Object { $_ -in $portable }) { 'laptop or tablet' } else { 'desktop' }
        # A desktop with a battery is usually one on a UPS that reports itself as one.
        $extra = if ($kind -eq 'desktop' -and $battery.Count) { ', a battery is reported (a UPS?)' } else { '' }
        "$kind (chassis type $($types -join ','))$extra"
    }
    Add-Probe 'power_mode' {
        $overlays = @{
            'ded574b5-45a0-4f42-8737-46345c09c238' = 'best performance'
            '3af9b8d9-7c97-431d-ad78-34a8bfea439f' = 'better performance'
            '961cc777-3547-4f9d-8174-7d86181b8a7a' = 'best power efficiency'
            '00000000-0000-0000-0000-000000000000' = 'balanced'
        }
        $schemes = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes' -ErrorAction Stop
        $answer = foreach ($pair in @(@('mains', 'ActiveOverlayAcPowerScheme'), @('battery', 'ActiveOverlayDcPowerScheme'))) {
            $guid = "$($schemes.($pair[1]))".ToLowerInvariant()
            if ($guid) { "$($pair[0])=$(if ($overlays.ContainsKey($guid)) { $overlays[$guid] } else { $guid })" }
        }
        if ($answer) { $answer -join ' ' } else { 'no power mode overlay (Windows 10, or a maker''s own mode)' }
    }

    # Windows' own switches that move frame times: hardware-accelerated GPU
    # scheduling, Game Mode, and virtualization-based security with memory
    # integrity (HVCI), which costs some CPU time.
    Add-Probe 'gpu_scheduling' {
        $mode = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -ErrorAction Stop).HwSchMode
        switch ($mode) { 2 { 'on' } 1 { 'off' } default { 'not set (off, or the driver does not offer it)' } }
    }
    Add-Probe 'game_mode' {
        $bar = Get-ItemProperty 'HKCU:\Software\Microsoft\GameBar' -ErrorAction SilentlyContinue
        if ($bar -and $null -ne $bar.AutoGameModeEnabled) { if ($bar.AutoGameModeEnabled) { 'on' } else { 'off' } } else { 'default (on)' }
    }
    Add-Probe 'vbs' {
        $guard = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction Stop
        $states = @('off', 'configured', 'running')
        $vbs = $states[[int]$guard.VirtualizationBasedSecurityStatus]
        $integrity = if (@($guard.SecurityServicesRunning) -contains 2) { 'on' } else { 'off' }
        "$vbs memory_integrity=$integrity"
    }
    Add-Probe 'os_build' {
        $version = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction Stop
        $product = "$($version.ProductName)"
        if ([int]$version.CurrentBuild -ge 22000) { $product = $product -replace 'Windows 10', 'Windows 11' }
        $release = if ($version.DisplayVersion) { $version.DisplayVersion } else { $version.ReleaseId }
        "$product $release build $($version.CurrentBuild).$($version.UBR)"
    }

    # What the renderer asks of the drivers. D3D12: Plume records into
    # ID3D12GraphicsCommandList7, which Windows' own runtime has from 11 24H2
    # and the Agility SDK (D3D12\D3D12Core.dll beside the program, SDK 619,
    # scripts/fetch_d3d12_agility.py) gives on Windows 10; without either the
    # game falls back to Vulkan (NATIVE_GRAPHICS_FALLBACK in the log). Vulkan:
    # Plume asks for API 1.2 with VK_KHR_buffer_device_address and
    # VK_EXT_scalar_block_layout; the loader here is vulkan-1.dll.
    $hostDir = if ($HostDirectory) { $HostDirectory } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'out/build/host' }
    Add-Probe 'd3d12_runtime' {
        $system = Join-Path $env:SystemRoot 'System32\d3d12.dll'
        $text = "system d3d12.dll $((Get-Item -LiteralPath $system -ErrorAction Stop).VersionInfo.FileVersion)"
        $core = Join-Path $hostDir 'D3D12\D3D12Core.dll'
        $text += $(if (Test-Path -LiteralPath $core) { ", Agility SDK beside the program $((Get-Item -LiteralPath $core).VersionInfo.FileVersion)" }
                   else { ', no Agility SDK beside the program (D3D12\D3D12Core.dll)' })
        $text
    }
    Add-Probe 'vulkan_loader' {
        $loader = Join-Path $env:SystemRoot 'System32\vulkan-1.dll'
        if (-not (Test-Path -LiteralPath $loader)) { 'none (no vulkan-1.dll)' }
        else {
            $text = "vulkan-1.dll $((Get-Item -LiteralPath $loader).VersionInfo.FileVersion)"
            $drivers = Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers' -ErrorAction SilentlyContinue
            if ($drivers) {
                $names = @($drivers.PSObject.Properties | Where-Object { $_.Name -match '\.json$' } | ForEach-Object { Split-Path -Leaf $_.Name })
                if ($names.Count) { $text += ", drivers: $($names -join ', ')" }
            }
            $text
        }
    }

    foreach ($pair in $probes) {
        $key, $probe = $pair
        $value = try { & $probe } catch { 'unknown' }
        if ($null -eq $value -or "$value" -eq '') { $value = 'unknown' }
        # One line a key: a value that broke into lines is joined.
        $lines.Add("$key=$(("$value" -split '\r?\n') -join ' ')")
    }
    return $lines
}

# Run by itself (not dot-sourced): print the block.
if ($MyInvocation.InvocationName -ne '.' -and $MyInvocation.Line -notmatch '^\s*\.\s') {
    Get-HardwareProbe -HostDirectory $HostDirectory -Basics | ForEach-Object { Write-Output $_ }
}
