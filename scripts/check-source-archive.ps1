<#
.SYNOPSIS
    Builds a source archive of HEAD and fails if it holds anything a release should not.
.DESCRIPTION
    Runs `git archive` of HEAD into a temporary zip (the working tree and untracked files are
    not part of it, exactly like a release) and checks the archive's contents:
      - model weights (.th, .pt, .pth, .ckpt, .safetensors, .onnx, ...)
      - audio files outside testdata\, or in testdata\ but not named in testdata\README.md
      - build outputs, virtual environments, caches, binaries
      - logs and local settings
      - plan.md and decisions.md (local planning notes)
      - files over 5 MB
      - absolute user paths in text files: C:\Users\<name>\..., the profile or local app data
        folder of whoever runs this, or the user and computer name inside a path
      - strings that look like secrets: private keys, cloud and service tokens, quoted
        passwords or API keys (not looked for in tests\ and worker\tests\, which use fakes)
    Prints the archive size and file count. Exit 0 when clean, 1 with the list of findings.
    Read-only: the temporary zip is deleted afterwards.
.PARAMETER Ref
    The commit to archive (default HEAD).
.PARAMETER WorkingTree
    Archive what the next commit would hold instead of HEAD: the tracked files as they are on disk,
    plus new files that .gitignore does not exclude. Uses a temporary index and writes only loose
    objects; your index, branch and working tree are not touched. Use it to check before committing.
.PARAMETER Keep
    Keep the temporary zip and print its path.
#>
param(
    [string] $Ref = 'HEAD',
    [switch] $WorkingTree,
    [switch] $Keep
)

. "$PSScriptRoot\common.ps1"
trap { Write-Host "check-source-archive failed: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }

$git = Get-RequiredCommand 'git' 'Install Git for Windows.'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$maxFileBytes = 5MB
$maxTextBytes = 2MB

$weightExtensions = '.th', '.pt', '.pth', '.ckpt', '.safetensors', '.onnx', '.pb', '.h5', '.npz', '.npy'
$audioExtensions = '.wav', '.mp3', '.flac', '.ogg', '.oga', '.opus', '.m4a', '.aac', '.aif', '.aiff', '.wma', '.mid', '.midi'
$binaryExtensions = '.exe', '.dll', '.lib', '.obj', '.o', '.pdb', '.ilk', '.exp', '.pyc', '.pyo', '.pyd', '.so', '.dylib', '.zip', '.7z', '.tar', '.gz', '.whl'
$buildFolders = 'build', 'out', '.vs', '.venv', 'venv', '__pycache__', '.pytest_cache', '.ruff_cache', '.mypy_cache', 'CMakeFiles', 'node_modules', 'logs', 'jobs', 'cache', 'models'
$localFiles = 'plan.md', 'decisions.md', 'settings.json', 'originals.json', 'CMakeCache.txt', '.env', 'CMakeUserPresets.json'
$textExtensions = '.cpp', '.h', '.hpp', '.c', '.cmake', '.txt', '.md', '.ps1', '.py', '.toml', '.json', '.yml', '.yaml', '.lock', '.cfg', '.ini', '.in', '.gitignore', '.gitattributes', '.xml', '.html', '.css', '.js'

# The name after C:\Users\ that is a placeholder or a Windows built-in, not a person.
$placeholderUsers = 'Public', 'Default', 'Default User', 'All Users', 'you', 'your-name', 'yourname', 'name', 'user', 'username', 'me', 'Someone', 'Example', 'example', 'x', 'Pat', 'Patty'

$secretPatterns = @(
    @{ Name = 'private key block'; Pattern = '-----BEGIN [A-Z ]*PRIVATE KEY-----' },
    @{ Name = 'GitHub token'; Pattern = '\b(ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{30,}\b|github_pat_[A-Za-z0-9_]{30,}' },
    @{ Name = 'AWS access key id'; Pattern = '\bAKIA[0-9A-Z]{16}\b' },
    @{ Name = 'Slack token'; Pattern = '\bxox[abprs]-[A-Za-z0-9-]{10,}' },
    @{ Name = 'API secret key (sk-...)'; Pattern = '\bsk-[A-Za-z0-9]{24,}\b' },
    @{ Name = 'Hugging Face token'; Pattern = '\bhf_[A-Za-z0-9]{30,}\b' },
    @{ Name = 'bearer token'; Pattern = '(?i)\bbearer\s+[A-Za-z0-9._~+/=-]{24,}' },
    @{ Name = 'quoted password or key'; Pattern = '(?i)(passw(or)?d|passwd|secret|api[_-]?key|access[_-]?token|auth[_-]?token)["'']?\s*[:=]\s*["''][^"''\s$%{<]{8,}["'']'; SkipTests = $true }
)

$sw = [Diagnostics.Stopwatch]::StartNew()
$zipPath = Join-Path ([IO.Path]::GetTempPath()) ("snds-source-archive-{0}.zip" -f [Guid]::NewGuid().ToString('N'))
$findings = New-Object System.Collections.Generic.List[string]

function Add-Finding {
    param([string] $Category, [string] $Path, [string] $Detail = '')
    $findings.Add($(if ($Detail) { "[$Category] $Path : $Detail" } else { "[$Category] $Path" }))
}

try {
    if ($WorkingTree) {
        Write-Step 'Snapshot of the working tree (temporary index)'
        $tempIndex = Join-Path ([IO.Path]::GetTempPath()) ("snds-index-{0}" -f [Guid]::NewGuid().ToString('N'))
        $previousIndex = $env:GIT_INDEX_FILE
        try {
            $env:GIT_INDEX_FILE = $tempIndex
            Invoke-Native $git @('-C', $SndsRepoRoot, 'read-tree', 'HEAD')
            Invoke-Native $git @('-C', $SndsRepoRoot, 'add', '--all', '--', '.')
            $Ref = (@(& $git -C $SndsRepoRoot write-tree))[0].Trim()
            if ($LASTEXITCODE -ne 0 -or -not $Ref) { throw 'git write-tree failed.' }
        } finally {
            if ($null -eq $previousIndex) { Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue } else { $env:GIT_INDEX_FILE = $previousIndex }
            Remove-Item -LiteralPath $tempIndex -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Step "git archive $Ref"
    Invoke-Native $git @('-C', $SndsRepoRoot, 'archive', '--format=zip', "--output=$zipPath", $Ref)
    $archiveBytes = (Get-Item -LiteralPath $zipPath).Length

    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entries = @($zip.Entries | Where-Object { $_.FullName -and -not $_.FullName.EndsWith('/') })

        # The audio files testdata\README.md documents (by name, in backticks).
        $documented = @{}
        $readme = $zip.GetEntry('testdata/README.md')
        if ($readme) {
            $reader = New-Object IO.StreamReader($readme.Open())
            try { $readmeText = $reader.ReadToEnd() } finally { $reader.Dispose() }
            foreach ($match in [regex]::Matches($readmeText, '`([A-Za-z0-9_.-]+\.[A-Za-z0-9]+)`')) {
                $documented[$match.Groups[1].Value] = $true
            }
        }

        $userNames = @($env:USERNAME, (Split-Path -Leaf "$env:USERPROFILE")) | Where-Object { $_ } | Select-Object -Unique
        $literalPaths = @($env:USERPROFILE, $env:LOCALAPPDATA, $env:APPDATA, $env:TEMP, $SndsRepoRoot) | Where-Object { $_ -and $_.Length -gt 6 }
        $secretCount = 0
        $textScanned = 0

        Write-Step "Checking $($entries.Count) files"
        foreach ($entry in $entries) {
            $path = $entry.FullName
            $segments = $path -split '/'
            $name = $segments[-1]
            $extension = [IO.Path]::GetExtension($name).ToLowerInvariant()
            $isTest = $path -like 'tests/*' -or $path -like 'worker/tests/*' -or $path -like 'spikes/*/tests/*'

            if ($weightExtensions -contains $extension) {
                Add-Finding 'model weights' $path
            }
            if ($audioExtensions -contains $extension) {
                if ($segments[0] -ne 'testdata' -or $segments.Count -ne 2) {
                    Add-Finding 'audio outside testdata' $path
                } elseif (-not $documented.ContainsKey($name)) {
                    Add-Finding 'undocumented testdata audio' $path 'not named in testdata/README.md'
                }
            }
            if ($binaryExtensions -contains $extension) {
                Add-Finding 'binary or build output' $path
            }
            foreach ($segment in $segments[0..([Math]::Max(0, $segments.Count - 2))]) {
                if (($buildFolders -contains $segment -and $segments.Count -gt 1) -or $segment -like 'build-*' -or $segment -like '*.egg-info') {
                    Add-Finding 'build output or local folder' $path "folder '$segment'"
                    break
                }
            }
            if ($extension -eq '.log' -or $name -like '*.log.*') {
                Add-Finding 'log file' $path
            }
            if ($localFiles -contains $name) {
                Add-Finding 'local file' $path
            }
            if ($entry.Length -gt $maxFileBytes) {
                Add-Finding 'large file' $path ('{0:N1} MB' -f ($entry.Length / 1MB))
            }

            # Text content: absolute user paths and secrets.
            if ($entry.Length -eq 0 -or $entry.Length -gt $maxTextBytes) { continue }
            if (($textExtensions -notcontains $extension) -and $name -notmatch '^(LICENSE|\.git.*|.*file)$') { continue }

            $stream = $entry.Open()
            $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)
            try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
            if ($text.IndexOf([char]0) -ge 0) { continue }
            $textScanned++

            $lineNumber = 0
            foreach ($line in ($text -split "`n")) {
                $lineNumber++
                $where = "${path}:$lineNumber"

                foreach ($match in [regex]::Matches($line, '(?i)\b[A-Z]:[\\/]+Users[\\/]+([^\\/\s"''`<>*|?%$]+)')) {
                    $user = $match.Groups[1].Value.TrimEnd(".", ",", ";", ":", ")")
                    if ($placeholderUsers -notcontains $user) {
                        Add-Finding 'absolute user path' $where $match.Value
                    }
                }
                foreach ($match in [regex]::Matches($line, '(?i)/(?:c|d)/Users/([^/\s"''`<>*|?%$]+)')) {
                    if ($placeholderUsers -notcontains $match.Groups[1].Value.TrimEnd(".", ",", ";", ":", ")")) {
                        Add-Finding 'absolute user path' $where $match.Value
                    }
                }
                foreach ($literal in $literalPaths) {
                    $variants = @($literal, $literal.Replace('\', '/'), $literal.Replace('\', '\\'))
                    foreach ($variant in $variants) {
                        if ($line.IndexOf($variant, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                            Add-Finding 'absolute user path' $where "this machine's path $literal"
                            break
                        }
                    }
                }
                foreach ($user in $userNames) {
                    if ($line -match ('(?i)[\\/]' + [regex]::Escape($user) + '([\\/]|\b)') -and $line -match '(?i)[\\/](Users|home)[\\/]') {
                        Add-Finding 'user name in a path' $where $user
                    }
                }

                foreach ($rule in $secretPatterns) {
                    if ($rule.ContainsKey('SkipTests') -and $isTest) { continue }
                    if ($line -match $rule.Pattern) {
                        $secretCount++
                        Add-Finding 'possible secret' $where $rule.Name
                    }
                }
            }
        }
    } finally {
        $zip.Dispose()
    }

    $largest = $entries | Sort-Object Length -Descending | Select-Object -First 3
    Write-Host ''
    Write-Host ("Archive of {0}: {1} files, {2:N2} MB zipped, {3:N2} MB unpacked ({4} text files scanned, {5:N1} s)" -f `
            $Ref, $entries.Count, ($archiveBytes / 1MB), (($entries | Measure-Object Length -Sum).Sum / 1MB), $textScanned, $sw.Elapsed.TotalSeconds)
    foreach ($item in $largest) {
        Write-Host ("  largest: {0} ({1:N0} KB)" -f $item.FullName, ($item.Length / 1KB))
    }
    if ($Keep) {
        Write-Host "Kept: $zipPath"
    }
} finally {
    if (-not $Keep -and (Test-Path -LiteralPath $zipPath)) {
        Remove-Item -LiteralPath $zipPath -Force
    }
}

if ($findings.Count -gt 0) {
    Write-Host ''
    Write-Host "$($findings.Count) problem(s) in the source archive:" -ForegroundColor Red
    foreach ($finding in ($findings | Select-Object -First 60)) {
        Write-Host "  $finding" -ForegroundColor Red
    }
    if ($findings.Count -gt 60) {
        Write-Host "  ... and $($findings.Count - 60) more" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'The source archive is clean.' -ForegroundColor Green
exit 0
