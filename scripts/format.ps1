<#
.SYNOPSIS
    Formats SNDS-owned C++ and Python code, or checks it with -Check.
.DESCRIPTION
    C++ in src\ and tests\cpp\: clang-format at a pinned version, run through uvx so everyone
    uses the same binary. Python in worker\: ruff from the worker environment (the version
    pinned in worker\uv.lock), for formatting and lint. spikes\ is never touched.
.PARAMETER Check
    Change nothing. Exit 1 if any file needs formatting or ruff reports a lint problem.
#>
param(
    [switch] $Check
)

. "$PSScriptRoot\common.ps1"
trap { Write-Host "format failed: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }

$clangFormatVersion = '23.1.2'

$uvx = Get-RequiredCommand 'uvx' 'uvx comes with uv: install it from https://docs.astral.sh/uv/getting-started/installation/.'
if (-not (Test-Path -LiteralPath $SndsWorkerPython)) {
    throw "The worker environment is missing ($SndsWorkerPython). Run scripts\bootstrap.ps1."
}

$cppFolders = @((Join-Path $SndsRepoRoot 'src'), (Join-Path $SndsRepoRoot 'tests\cpp'))
$cppFiles = @(Get-ChildItem -LiteralPath $cppFolders -Recurse -File -Include '*.cpp', '*.h' | Sort-Object FullName | ForEach-Object { $_.FullName })
$failures = @()

Write-Step "C++: clang-format $clangFormatVersion on $($cppFiles.Count) files$(if ($Check) { ' (check only)' })"
$mode = if ($Check) { @('--dry-run', '--Werror') } else { @('-i') }
# Batches keep the command line well below the Windows limit as the code grows.
for ($start = 0; $start -lt $cppFiles.Count; $start += 50) {
    $batch = $cppFiles[$start..([Math]::Min($start + 49, $cppFiles.Count - 1))]
    try {
        Invoke-Native $uvx (@('--from', "clang-format==$clangFormatVersion", 'clang-format') + $mode + $batch)
    } catch {
        $failures += 'C++ formatting'
        break
    }
}

Write-Step "Python: ruff check$(if (-not $Check) { ' --fix' })"
try {
    Invoke-Native $SndsWorkerPython (@('-m', 'ruff', 'check') + $(if ($Check) { @() } else { @('--fix') }) + @($SndsWorkerRoot))
} catch {
    $failures += 'Python lint'
}

Write-Step "Python: ruff format$(if ($Check) { ' --check' })"
try {
    Invoke-Native $SndsWorkerPython (@('-m', 'ruff', 'format') + $(if ($Check) { @('--check') } else { @() }) + @($SndsWorkerRoot))
} catch {
    $failures += 'Python formatting'
}

if ($failures.Count -gt 0) {
    $hint = if ($Check) { ' Run scripts\format.ps1 to fix formatting; lint problems need code changes.' } else { '' }
    Write-Host "Failed: $($failures -join ', ').$hint" -ForegroundColor Red
    exit 1
}

Write-Host $(if ($Check) { 'Formatting and lint are clean.' } else { 'Formatted.' }) -ForegroundColor Green
exit 0
