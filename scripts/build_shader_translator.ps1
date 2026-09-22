param([string]$OutputDirectory = 'out/tools/shader-translator')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $repoRoot 'config/dependencies.lock.json') -Raw | ConvertFrom-Json
$dependency = $lock.dependencies | Where-Object name -eq 'XenosRecomp'
if ($dependency.commit -ne 'fb32631ee398e46f2a113d8f9103201dbaa000b4') { throw 'Unsupported XenosRecomp pin.' }
$checkout = Join-Path $repoRoot $dependency.path
$head = & git -C $checkout rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $dependency.commit) { throw 'XenosRecomp checkout differs from dependency pin. Run bootstrap.py.' }
$fmtDependency = ($lock.dependencies | Where-Object name -eq 'XenonRecomp').submodules | Where-Object path -eq 'thirdparty/fmt'
$fmt = Join-Path $repoRoot 'tools/XenonRecomp/thirdparty/fmt'
$fmtHead = & git -C $fmt rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $fmtHead -ne $fmtDependency.commit) { throw 'fmt checkout differs from dependency pin.' }
& git -C $fmt diff --quiet HEAD -- include
if ($LASTEXITCODE -ne 0) { throw 'fmt headers must be unmodified.' }
$names = @('constant_table.h', 'shader.h', 'shader_code.h', 'shader_common.h', 'shader_recompiler.h', 'shader_recompiler.cpp', 'pch.h')
$paths = $names | ForEach-Object { 'XenosRecomp/' + $_ }
& git -C $checkout diff --quiet HEAD -- @paths
if ($LASTEXITCODE -ne 0) { throw 'Translator upstream sources must be byte-identical to the pinned checkout.' }
$output = [IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDirectory))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$source = Join-Path $checkout 'XenosRecomp'
# Copy no implementation: compile the byte-identical upstream source directly.
# Its build-wide PCH includes unused compiler/cache libraries. Keep its endian
# helpers verbatim, preceded only by the standard/fmt headers used in translation.
$pch = Get-Content -LiteralPath (Join-Path $source 'pch.h') -Raw
$marker = $pch.IndexOf('template<typename T>')
if ($marker -lt 0) { throw 'Pinned endian helpers missing.' }
$minimalPch = @'
#pragma once
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/core.h>

'@ + $pch.Substring($marker)
$pchPath = Join-Path $output 'translator_pch.h'
[IO.File]::WriteAllText($pchPath, $minimalPch)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ desktop tools are required.' }
$vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$environmentLines = & $env:COMSPEC /d /s /c "`"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'vcvars64 failed.' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$compiler = Join-Path $env:ProgramFiles 'LLVM/bin/clang++.exe'
$executable = Join-Path $output 'shader_translate.exe'
$pending = Join-Path $output 'shader_translate.pending.exe'
$arguments = @('-std=c++20', '-fms-extensions', '-DFMT_HEADER_ONLY', '-Wno-null-arithmetic', '-Wno-switch', '-Wno-unused-variable', '-include', $pchPath, '-I', $source, '-I', (Join-Path $fmt 'include'), (Join-Path $source 'shader_recompiler.cpp'), (Join-Path $repoRoot 'src/shader_translate_main.cpp'), '-o', $pending)
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Shader translator build failed.' }
Move-Item -LiteralPath $pending -Destination $executable -Force
$hashes = [ordered]@{}
foreach ($name in $names) {
    $relative = 'tools/XenosRecomp/XenosRecomp/' + $name
    $hashes[$relative] = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repoRoot $relative)).Hash.ToLowerInvariant()
}
$hashes['src/shader_translate_main.cpp'] = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repoRoot 'src/shader_translate_main.cpp')).Hash.ToLowerInvariant()
$provenance = [ordered]@{ commit = $dependency.commit; fmt_commit = $fmtHead; sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash.ToLowerInvariant(); compiler = $compiler; compiler_version = (& $compiler --version | Out-String).Trim(); arguments = $arguments; sources = $hashes }
$provenance | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'shader_translate.json') -Encoding utf8
Write-Output "Built pinned shader translator: $executable"
