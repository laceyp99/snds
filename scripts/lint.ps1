<#
.SYNOPSIS
    Checks formatting and lint: clang-format for SNDS C++ sources, ruff for the worker.

.DESCRIPTION
    clang-format is the pinned PyPI build run through "uv tool run" (no global install). ruff runs from
    worker\.venv (installed by scripts\bootstrap.ps1) and is skipped with a message when the worker
    environment does not exist yet.

.PARAMETER Fix
    Rewrite files instead of only checking them.
#>
[CmdletBinding()]
param(
    [switch]$Fix,
    [switch]$SkipCpp,
    [switch]$SkipPython
)

. (Join-Path $PSScriptRoot '_common.ps1')

function Get-SndsCppSources {
    $roots = @('src', 'tests\cpp', 'spikes\cpp') | ForEach-Object { Join-Path $SndsRepoRoot $_ } |
        Where-Object { Test-Path -LiteralPath $_ }
    # Filter on Extension: Windows PowerShell 5.1 ignores -Include when combined with -LiteralPath.
    $extensions = @('.h', '.hpp', '.cpp')
    return @(Get-ChildItem -LiteralPath $roots -Recurse -File |
            Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
            Sort-Object FullName | ForEach-Object { $_.FullName })
}

Invoke-SndsScript {
    $failed = @()

    if (-not $SkipCpp) {
        $uv = Assert-SndsCommand -Name 'uv' -Hint 'Install uv from https://docs.astral.sh/uv/getting-started/installation/.'
        $files = @(Get-SndsCppSources)
        $mode = if ($Fix) { @('-i') } else { @('--dry-run', '--Werror') }
        Write-SndsStep "clang-format $SndsClangFormatVersion ($($files.Count) files)"
        $arguments = @('tool', 'run', '--from', "clang-format==$SndsClangFormatVersion", 'clang-format') + $mode + $files
        $exitCode = Invoke-SndsNative -FilePath $uv -ArgumentList $arguments -AllowFailure
        if ($exitCode -ne 0) {
            $failed += 'clang-format (run scripts\lint.ps1 -Fix)'
        }
    }

    if (-not $SkipPython) {
        if (-not (Test-Path -LiteralPath $SndsWorkerPython)) {
            Write-SndsWarning "Skipping ruff: $SndsWorkerPython does not exist. Run scripts\bootstrap.ps1 first."
        }
        else {
            Push-Location -LiteralPath $SndsWorkerDir
            try {
                $checkArguments = @('-m', 'ruff', 'check', '.')
                $formatArguments = @('-m', 'ruff', 'format', '.')
                if ($Fix) {
                    $checkArguments += '--fix'
                }
                else {
                    $formatArguments += '--check'
                }
                Write-SndsStep 'ruff check'
                if ((Invoke-SndsNative -FilePath $SndsWorkerPython -ArgumentList $checkArguments -AllowFailure) -ne 0) {
                    $failed += 'ruff check'
                }
                Write-SndsStep 'ruff format'
                if ((Invoke-SndsNative -FilePath $SndsWorkerPython -ArgumentList $formatArguments -AllowFailure) -ne 0) {
                    $failed += 'ruff format'
                }
            }
            finally {
                Pop-Location
            }
        }
    }

    if ($failed.Count -gt 0) {
        throw "Failed: $($failed -join ', ')"
    }
    Write-SndsStep 'Formatting and lint checks passed'
}
