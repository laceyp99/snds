# Runs every opengl-tier self-test scenario and summarises the JSON reports.
# Windows PowerShell 5.1 compatible; no VS developer shell needed.
param(
    [string]$BuildDir = "$env:LOCALAPPDATA\snds-spikes\opengl-tier-build",
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [double]$Seconds = 6
)

$ErrorActionPreference = 'Stop'
$reportDir = Join-Path $BuildDir "reports\$Config"
New-Item -ItemType Directory -Force $reportDir | Out-Null

$scenarios = @(
    @{ Name = 'normal';               Exe = 'SndsOpenGLTier';     Args = @() },
    @{ Name = 'normal-dgpu';          Exe = 'SndsOpenGLTierDgpu'; Args = @() },
    @{ Name = 'request-gl-default';   Exe = 'SndsOpenGLTier';     Args = @('--request-gl', 'default') },
    @{ Name = 'force-shader-failure'; Exe = 'SndsOpenGLTier';     Args = @('--force-shader-failure') },
    @{ Name = 'force-no-opengl';      Exe = 'SndsOpenGLTier';     Args = @('--force-no-opengl') },
    @{ Name = 'force-context-loss';   Exe = 'SndsOpenGLTier';     Args = @('--force-context-loss', '1500') },
    @{ Name = 'require-gl-9.9';       Exe = 'SndsOpenGLTier';     Args = @('--require-gl', '9.9', '--expect-tier', 'cpu') },
    @{ Name = 'force-cpu-failure';    Exe = 'SndsOpenGLTier';     Args = @('--force-no-opengl', '--force-cpu-failure') },
    # Informational: documents that a hung render thread blocks OpenGLContext::detach()
    # on the message thread. Expected to fail the stall check; not counted as a failure.
    @{ Name = 'force-gl-hang';        Exe = 'SndsOpenGLTier';     Args = @('--force-gl-hang', '4000'); Informational = $true }
)

$failed = 0
$gating = @($scenarios | Where-Object { -not $_.Informational }).Count
$rows = foreach ($s in $scenarios) {
    $exe = Join-Path $BuildDir "$($s.Exe)_artefacts\$Config\$($s.Exe).exe"
    $report = Join-Path $reportDir "$($s.Name).json"
    Remove-Item $report -ErrorAction SilentlyContinue

    $argList = @('--self-test', "$Seconds", '--report', "`"$report`"") + $s.Args
    $p = Start-Process -FilePath $exe -ArgumentList $argList -Wait -PassThru

    if (-not (Test-Path $report)) {
        if (-not $s.Informational) { $failed++ }
        [pscustomobject]@{ Scenario = $s.Name; Exit = $p.ExitCode; Pass = $false; Final = '(no report)' }
        continue
    }

    $r = Get-Content -Raw $report | ConvertFrom-Json
    if (-not $s.Informational -and ($p.ExitCode -ne 0 -or -not $r.pass)) { $failed++ }

    [pscustomobject]@{
        Scenario      = $s.Name + $(if ($s.Informational) { ' (informational)' } else { '' })
        Exit          = $p.ExitCode
        Pass          = $r.pass
        Final         = $r.fallback.finalTier
        Renderer      = $r.gl.renderer
        GL            = $r.gl.version
        Profile       = $r.gl.profile
        GLFrames      = $r.gl.frames
        GLFps         = [math]::Round($r.gl.framesPerSecond, 1)
        GLMaxFrameGap = [math]::Round($r.gl.maxFrameGapMs, 1)
        CPUFrames     = $r.fallback.cpuFrames
        AttachMs      = [math]::Round($r.gl.attachCallMs, 1)
        DetachMs      = (($r.fallback.transitions | ForEach-Object { [math]::Round($_.detachMs, 1) }) -join ',')
        WindowShownMs = [math]::Round($r.timer.windowShownMs, 0)
        FirstTickMs   = [math]::Round($r.timer.firstTickMs, 0)
        Ticks         = $r.timer.ticks
        GapSteadyMs   = [math]::Round($r.timer.maxGapSteadyMs, 1)
        GapAttachMs   = [math]::Round($r.timer.maxGapAttachMs, 1)
        GapTransMs    = [math]::Round($r.timer.maxGapTransitionMs, 1)
        Reason        = (($r.fallback.transitions | ForEach-Object { $_.reason }) -join ' -> ')
    }
}

$rows | Format-List
"Reports: $reportDir"
if ($failed -gt 0) { "FAILED: $failed scenario(s)"; exit 1 }
"All $gating gating scenarios passed."
