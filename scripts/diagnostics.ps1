<#
.SYNOPSIS
    Prints a diagnostics report for bug reports: system, toolchain, worker, model, settings, cache, logs, and build.
.DESCRIPTION
    Read-only. User paths are redacted to %USERPROFILE% and %LOCALAPPDATA%. Each section reports
    its own failures, so the script always finishes. The sections System, Data directories, Model,
    Worker environment, Settings, Cache, and Logs match the app's "Export diagnostics..." zip
    (src/app/DiagnosticsExport.cpp); Source, Toolchain, and App build are script only, and
    Application, Renderer, and Audio device exist only in the app export (they need the running app).
    Settings show known fields only, never other values. Save the output with:
        powershell -NoProfile -ExecutionPolicy Bypass -File scripts\diagnostics.ps1 > diagnostics.txt
#>
param()

. "$PSScriptRoot\common.ps1"

function Write-Section {
    param([Parameter(Mandatory)][string] $Title)
    Write-Output ''
    Write-Output "## $Title"
}

function Write-Item {
    param([Parameter(Mandatory)][string] $Name, [AllowNull()][AllowEmptyString()][string] $Value)
    Write-Output (Protect-Path ("{0,-24} {1}" -f "${Name}:", $Value))
}

function Invoke-Section {
    # Runs one section; a failure is reported in place instead of ending the report.
    param([Parameter(Mandatory)][string] $Title, [Parameter(Mandatory)][scriptblock] $Body)
    Write-Section $Title
    try {
        & $Body
    } catch {
        Write-Output (Protect-Path "unavailable: $($_.Exception.Message)")
    }
}

function Get-NativeOutput {
    # First lines of a program's stdout, or a note if it failed. Never throws.
    param([Parameter(Mandatory)][string] $FilePath, [string[]] $ArgumentList = @(), [int] $Lines = 1)
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $FilePath @ArgumentList 2> $null
    } catch {
        return "failed: $($_.Exception.Message)"
    }
    if ($LASTEXITCODE -ne 0) {
        return "failed with exit code $LASTEXITCODE"
    }
    return (@($output) | Select-Object -First $Lines) -join '; '
}

function Format-Bytes {
    param([double] $Bytes)
    return '{0:N1} GB' -f ($Bytes / 1GB)
}

Write-Output "# SNDS diagnostics"
Write-Item 'Generated' (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')
Write-Item 'Repository' $SndsRepoRoot

Invoke-Section 'Source' {
    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $git) {
        Write-Item 'Commit' 'git not found'
        return
    }
    Write-Item 'Commit' (Get-NativeOutput $git.Source @('-C', $SndsRepoRoot, 'log', '-1', '--format=%h %s'))
    $ErrorActionPreference = 'Continue'
    $changes = @(& $git.Source -C $SndsRepoRoot status --porcelain 2> $null).Count
    Write-Item 'Uncommitted changes' "$changes"
}

Invoke-Section 'System' {
    $os = Get-CimInstance Win32_OperatingSystem
    Write-Item 'OS' "$($os.Caption) $($os.Version) ($($os.OSArchitecture))"
    foreach ($cpu in @(Get-CimInstance Win32_Processor)) {
        Write-Item 'CPU' "$($cpu.Name.Trim()) ($($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) threads)"
    }
    $computer = Get-CimInstance Win32_ComputerSystem
    Write-Item 'RAM' (Format-Bytes $computer.TotalPhysicalMemory)
    foreach ($gpu in @(Get-CimInstance Win32_VideoController)) {
        Write-Item 'GPU' "$($gpu.Name) (driver $($gpu.DriverVersion))"
    }
    $smi = Get-Command nvidia-smi -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($smi) {
        $query = Get-NativeOutput $smi.Source @('--query-gpu=name,driver_version,memory.total', '--format=csv,noheader') 4
        Write-Item 'nvidia-smi' $query
    } else {
        Write-Item 'nvidia-smi' 'not found'
    }
}

Invoke-Section 'Toolchain' {
    $uv = Get-Command uv -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    Write-Item 'uv' $(if ($uv) { Get-NativeOutput $uv.Source @('--version') } else { 'not found' })
    try {
        $cmake = Get-CMake
        Write-Item 'CMake' "$(Get-NativeOutput $cmake @('--version')) ($cmake)"
    } catch {
        Write-Item 'CMake' 'not found'
    }
    $vs = Get-VisualStudioPath
    if ($vs) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $name = Get-NativeOutput $vswhere @('-path', $vs, '-property', 'displayName')
        $version = Get-NativeOutput $vswhere @('-path', $vs, '-property', 'catalog_productDisplayVersion')
        Write-Item 'Visual Studio' "$name $version"
        $toolsFile = Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
        if (Test-Path -LiteralPath $toolsFile) {
            Write-Item 'MSVC toolset' (Get-Content -LiteralPath $toolsFile -TotalCount 1).Trim()
        }
    } else {
        Write-Item 'Visual Studio' 'no installation with the x64 C++ tools found'
    }
    Write-Item 'PowerShell' "$($PSVersionTable.PSVersion) ($($PSVersionTable.PSEdition))"
}

Invoke-Section 'Data directories' {
    $snds = Get-SndsHome
    $source = if ("$env:SNDS_HOME".Trim()) { 'SNDS_HOME' } else { 'default' }
    Write-Item 'SNDS home' "$snds ($source)"
    foreach ($entry in @('settings.json', 'cache', 'jobs', 'logs', 'models')) {
        $path = Join-Path $snds $entry
        Write-Item $entry $(if (Test-Path -LiteralPath $path) { 'present' } else { 'missing' })
    }
}

Invoke-Section 'Model' {
    $path = Join-Path (Get-SndsHome) 'models\htdemucs\955717e8-8726e21a.th'
    Write-Item 'Path' $path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-Item 'Status' 'missing (run scripts\bootstrap.ps1)'
        return
    }
    $size = (Get-Item -LiteralPath $path).Length
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $verified = ($size -eq 84141911) -and ($hash -eq '8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4')
    Write-Item 'Size' "$size bytes"
    Write-Item 'SHA-256' $hash
    Write-Item 'Status' $(if ($verified) { 'verified' } else { 'MISMATCH (run scripts\bootstrap.ps1 to download it again)' })
}

Invoke-Section 'Worker environment' {
    Write-Item 'Python' $SndsWorkerPython
    if (-not (Test-Path -LiteralPath $SndsWorkerPython)) {
        Write-Item 'Status' 'missing (run scripts\bootstrap.ps1)'
        return
    }
    $result = Invoke-WorkerJson 'readiness'
    Write-Item 'readiness exit code' "$($result.ExitCode)"
    if ($result.Payload) {
        Write-Output (Protect-Path ($result.Payload | ConvertTo-Json -Depth 6))
    } else {
        Write-Output (Protect-Path "no JSON report; stdout was: $($result.Text)")
    }
}

Invoke-Section 'Settings' {
    # Known fields only, like the app's export: values of any other key are never printed.
    $file = Join-Path (Get-SndsHome) 'settings.json'
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        Write-Item 'Status' 'no settings file (defaults)'
        return
    }
    try {
        $settings = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    } catch {
        # The parser's message quotes the text, so it is not shown.
        Write-Item 'Status' 'not valid JSON (contents not shown)'
        return
    }
    # Missing keys read as absent (the repo scripts run in strict mode).
    function Get-Field($Name) { $property = $settings.PSObject.Properties[$Name]; if ($property) { $property.Value } }
    Write-Item 'Use the CPU only' $(if ((Get-Field 'forceCpu') -eq $true) { 'yes' } else { 'no' })
    Write-Item 'Audio device' $(if ("$(Get-Field 'audioDevice')") { 'chosen by the user' } else { 'system default' })
    $recent = @(Get-Field 'recentSongs')
    Write-Item 'Recent songs' "$($recent.Count)"
    foreach ($song in $recent) {
        if ($song.key) {
            $key = "$($song.key)"
            Write-Output (Protect-Path ("  {0}  {1}  {2}" -f $key.Substring(0, [Math]::Min(12, $key.Length)), $song.title, $song.path))
        }
    }
    $known = 'format', 'version', 'forceCpu', 'audioDevice', 'recentSongs'
    $other = @($settings.PSObject.Properties.Name | Where-Object { $known -notcontains $_ })
    if ($other.Count) {
        Write-Item 'Other keys' "$($other -join ', ') (values not shown)"
    }
}

Invoke-Section 'Cache' {
    $cache = Join-Path (Get-SndsHome) 'cache'
    if (-not (Test-Path -LiteralPath $cache -PathType Container)) {
        Write-Item 'Songs' 'no cache folder'
        return
    }
    $folders = @(Get-ChildItem -LiteralPath $cache -Directory)
    $total = 0
    foreach ($folder in $folders) {
        $size = (Get-ChildItem -LiteralPath $folder.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        $total += $size
        $json = Join-Path $folder.FullName 'song.json'
        $status = 'no song.json'
        $name = ''
        if (Test-Path -LiteralPath $json -PathType Leaf) {
            try {
                $song = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
                $status = "$($song.status)"
                $name = "$($song.source.fileName)"
            } catch {
                $status = 'unreadable song.json'
            }
        }
        Write-Output (Protect-Path ("  {0}  {1,-12} {2,8}  {3}" -f $folder.Name.Substring(0, [Math]::Min(12, $folder.Name.Length)), $status, ('{0:N1} MB' -f ($size / 1MB)), $name))
    }
    Write-Item 'Songs' "$($folders.Count) folder(s)"
    Write-Item 'Cache size' ('{0:N1} MB' -f ($total / 1MB))
}

Invoke-Section 'Logs' {
    $logs = Join-Path (Get-SndsHome) 'logs'
    Write-Item 'Folder' $logs
    if (-not (Test-Path -LiteralPath $logs -PathType Container)) {
        Write-Item 'Included' 'no logs folder'
        return
    }
    $files = @(Get-ChildItem -LiteralPath $logs -File -Filter '*.log*' | Sort-Object LastWriteTime -Descending)
    Write-Item 'Included' "$($files.Count) file(s)"
    foreach ($file in $files) {
        Write-Output (Protect-Path ("  {0}  {1:N0} KB" -f $file.Name, ($file.Length / 1KB)))
    }
    # The app's export zips each log; here the end of the newest app log is enough for a report.
    $app = Join-Path $logs 'app.log'
    if (Test-Path -LiteralPath $app -PathType Leaf) {
        Write-Output ''
        Write-Output 'Last lines of app.log:'
        Get-Content -LiteralPath $app -Tail 20 | ForEach-Object { Write-Output (Protect-Path "  $_") }
    }
}

Invoke-Section 'App build' {
    foreach ($config in $SndsConfigurations) {
        foreach ($exe in @((Get-AppExecutable $config), (Join-Path $SndsBuildDir "snds_tests_artefacts\$config\snds_tests.exe"))) {
            $label = "$config $(Split-Path -Leaf $exe)"
            if (Test-Path -LiteralPath $exe) {
                Write-Item $label "built $((Get-Item -LiteralPath $exe).LastWriteTime.ToString('yyyy-MM-dd HH:mm'))"
            } else {
                Write-Item $label 'not built'
            }
        }
    }
}

exit 0
