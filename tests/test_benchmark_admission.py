"""Exercise the target-idle admission function without running the suite."""
import json
import os
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PS = shutil.which('powershell') if os.name == 'nt' else None


@unittest.skipUnless(PS, 'Windows PowerShell required')
class BenchmarkAdmissionTest(unittest.TestCase):
    def test_suite_and_per_run_admission_wait_for_two_consecutive_safe_samples(self):
        script_path = str(ROOT / 'scripts' / 'benchmark_codex_i5.ps1').replace("'", "''")
        runner_path = str(ROOT / 'scripts' / 'benchmark.ps1').replace("'", "''")
        command = r'''
$sourcePath = '__SCRIPT_PATH__'
$runnerPath = '__RUNNER_PATH__'
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$definition = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-SemanticIdle' }, $true)
if (-not $definition) { throw 'Assert-SemanticIdle function was not found.' }
. ([scriptblock]::Create($definition.Extent.Text))
$runnerText = Get-Content -LiteralPath $runnerPath -Raw
$runnerAst = [System.Management.Automation.Language.Parser]::ParseFile($runnerPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$startRun = $runnerAst.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Start-Run' }, $true)
if (-not $startRun) { throw 'Start-Run function was not found.' }
$runStatements = $startRun.Body.EndBlock.Statements
$launchCleanup = @($runStatements | Where-Object { $_.Extent.Text -match '^Get-ChildItem Env:SFR_\*' }) | Select-Object -First 1
if (-not $launchCleanup) { throw 'Start-Run admission prefix boundary was not found.' }
$prefixStart = $runStatements[0].Extent.StartOffset
$runnerPrefix = $runnerText.Substring($prefixStart, $launchCleanup.Extent.StartOffset - $prefixStart)
$script:cpuSamples = @(); $script:gameSamples = @(); $script:sampleIndex = 0; $script:sleepCalls = 0; $script:stopCalls = 0
function Get-CimInstance { param($ClassName, $Filter); $n = [Math]::Min($script:sampleIndex, $script:cpuSamples.Count - 1); $value = $script:cpuSamples[$n]; if ($value -eq '__NULL_CPU__') { $value = $null }; [pscustomobject]@{ PercentProcessorTime = $value } }
function Get-Process { param($Name, $ErrorAction); $n = [Math]::Min($script:sampleIndex, $script:gameSamples.Count - 1); if ($script:gameSamples[$n]) { [pscustomobject]@{ Name = 'sfr_cpu_diagnostic' } } }
function Start-Sleep { param([int]$Seconds); $script:sleepCalls += $Seconds; $script:sampleIndex++ }
function Save-State { }
function Stop-Process { $script:stopCalls++; throw 'Stop-Process must never be called.' }
function Invoke-RunPrefixCase([string]$Name, [object[]]$CpuSequence, [bool[]]$GameSequence) {
  $script:cpuSamples = $CpuSequence; $script:gameSamples = $GameSequence; $script:sampleIndex = 0; $script:sleepCalls = 0; $script:stopCalls = 0
  $GameplayTicks = 5400; $TargetAdmission = $true; $Out = 'mock-out'; $admission = $null; $errorText = $null
  try { $pipeline = @(Invoke-Expression $runnerPrefix) } catch { $errorText = $_.ToString() + ' ' + $_.InvocationInfo.PositionMessage + ' ' + $_.ScriptStackTrace }
  [pscustomobject]@{ path='runner'; name=$Name; admission=$admission; pipeline_count=@($pipeline).Count; waits=$script:sleepCalls; stop_calls=$script:stopCalls; error=$errorText }
}
function Invoke-Case([string]$Name, [object[]]$CpuSequence, [bool[]]$GameSequence) {
  $script:cpuSamples = $CpuSequence; $script:gameSamples = $GameSequence; $script:sampleIndex = 0; $script:sleepCalls = 0; $script:stopCalls = 0
  $script:state = [ordered]@{ status = 'running'; preflight = @() }
  $errorText = $null
  try { Assert-SemanticIdle $Name } catch { $errorText = $_.Exception.Message }
  [pscustomobject]@{ path='suite'; name=$Name; status=$script:state.status; snapshots=@($script:state.preflight).Count; admitted=(@($script:state.preflight).Count -gt 0 -and $script:state.preflight[-1].admitted); wait_recorded=(@($script:state.preflight).Count -gt 0 -and $script:state.preflight[-1].Contains('waited_seconds')); waiting_fields_removed=(-not $script:state.Contains('waiting_stage') -and -not $script:state.Contains('waiting_load')); waits=$script:sleepCalls; stop_calls=$script:stopCalls; error=$errorText }
}
$cases = @(
  @{name='busy_then_idle'; cpu=@(41,8,7); games=@($false,$false,$false); waits=10},
  @{name='game_exit_then_idle'; cpu=@(8,8,7); games=@($true,$false,$false); waits=10},
  @{name='unknown_cpu'; cpu=@('__NULL_CPU__'); games=@($false); waits=0},
  @{name='spike_resets_idle_streak'; cpu=@(8,21,8,7); games=@($false,$false,$false,$false); waits=15}
)
$results = @()
foreach ($case in $cases) {
  $results += Invoke-Case $case.name $case.cpu $case.games
  $results += Invoke-RunPrefixCase $case.name $case.cpu $case.games
}
Write-Output ('@@ADMISSION@@' + (ConvertTo-Json -InputObject $results -Depth 5 -Compress))
'''.replace('__SCRIPT_PATH__', script_path).replace('__RUNNER_PATH__', runner_path)
        completed = subprocess.run(
            [PS, '-NoProfile', '-Command', command], cwd=ROOT,
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        payload = next((line[len('@@ADMISSION@@'):] for line in completed.stdout.splitlines()
                        if line.startswith('@@ADMISSION@@')), None)
        self.assertIsNotNone(payload, completed.stdout + completed.stderr)
        results = {(row['path'], row['name']): row for row in json.loads(payload)}

        for path in ('suite', 'runner'):
            for name, sleeps in (('busy_then_idle', 10), ('game_exit_then_idle', 10),
                                 ('spike_resets_idle_streak', 15)):
                with self.subTest(path=path, name=name):
                    row = results[(path, name)]
                    if path == 'suite':
                        self.assertTrue(row['admitted'])
                        self.assertEqual(row['snapshots'], 1)
                        self.assertEqual(row['status'], 'running')
                        self.assertTrue(row['wait_recorded'])
                        self.assertTrue(row['waiting_fields_removed'])
                    else:
                        self.assertIsNotNone(row['admission'], row)
                        self.assertTrue(row['admission']['admitted'])
                        self.assertEqual(row['admission']['game_processes'], 0)
                        self.assertEqual(row['pipeline_count'], 0)
                    self.assertEqual(row['waits'], sleeps)
                    self.assertEqual(row['stop_calls'], 0)
                    self.assertIsNone(row['error'])
        for path in ('suite', 'runner'):
            row = results[(path, 'unknown_cpu')]
            self.assertIsNotNone(row['error'])
            self.assertEqual(row['stop_calls'], 0)
            if path == 'suite':
                self.assertFalse(row['admitted'])
                self.assertEqual(row['status'], 'running')
            else:
                self.assertIsNone(row['admission'])


if __name__ == '__main__':
    unittest.main()
