[CmdletBinding()]
param(
    [string]$RepoRoot = ''
)

# XA DevHub Public Repository Audit v1.02
# Verifies the exact tracked/unignored candidate tree before public release.
# Created by: XA DevHub contributors
# Last Updated: 2026-09-08 16:30:00
# v1.02 - Keep operator release helpers private and reject nested tests/docs and linked candidates.
#
# This check is intentionally static. It does not build, package, upload, push,
# change repository visibility, or inspect ignored runtime/backup content.

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
}
$repo = (Resolve-Path -LiteralPath $RepoRoot).Path
$allowlistPath = Join-Path $repo '.public-repo-allowlist'
$failures = New-Object 'System.Collections.Generic.List[string]'

function Add-Failure([string]$Message) {
    $failures.Add($Message)
}

function Test-SyntheticDiscordId([string]$Value) {
    if ($Value -in @(
        '111111111111111111', '987654321098765432',
        '18446744073709551616', '9223372036854770000',
        '9223456789012345678'
    )) { return $true }
    foreach ($prefix in @(
        '123456789012345', '223456789012345', '423456789012345',
        '523456789012345', '623456789012345', '723456789012345',
        '823456789012345', '923456789012345', '700000000000000',
        '710000000000000'
    )) {
        if ($Value.StartsWith($prefix, [System.StringComparison]::Ordinal)) {
            return $true
        }
    }
    return $false
}

function Invoke-Git([string[]]$Arguments) {
    $output = @(& git -C $repo @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed: $($output -join [Environment]::NewLine)"
    }
    return $output
}

if (-not (Test-Path -LiteralPath $allowlistPath -PathType Leaf)) {
    throw "Missing public allow-list: $allowlistPath"
}

$null = Invoke-Git @('rev-parse', '--show-toplevel')
$deleted = @(Invoke-Git @('ls-files', '--deleted') | ForEach-Object {
    $_.Replace('\', '/')
})
$deletedSet = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($path in $deleted) { $null = $deletedSet.Add($path) }

$candidates = @(Invoke-Git @(
    'ls-files', '--cached', '--others', '--exclude-standard'
) | ForEach-Object { $_.Replace('\', '/') } |
    Where-Object { $_ -and -not $deletedSet.Contains($_) } |
    Sort-Object -Unique)

$ignoredTracked = @(Invoke-Git @('ls-files', '-ci', '--exclude-standard'))
foreach ($path in $ignoredTracked) {
    Add-Failure "Tracked file is now ignored: $path"
}

$exact = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
$prefixes = New-Object 'System.Collections.Generic.List[string]'
foreach ($line in Get-Content -LiteralPath $allowlistPath) {
    $entry = $line.Trim()
    if (-not $entry -or $entry.StartsWith('#')) { continue }
    if (-not $entry.StartsWith('/')) {
        Add-Failure "Allow-list entry is not repository-anchored: $entry"
        continue
    }
    if ($entry.EndsWith('/')) { $prefixes.Add($entry) }
    else { $null = $exact.Add($entry) }
}

$required = @(
    '.gitattributes', '.gitignore', '.public-repo-allowlist',
    '1. build.py', 'tools/release_receipt.py',
    'CHANGELOG.md', 'CONTRIBUTING.md', 'LICENSE', 'NOTICE',
    'PRIVACY.md', 'README.md', 'SECURITY.md', 'THIRD_PARTY_NOTICES.md'
)
foreach ($path in $required) {
    if ($candidates -notcontains $path) {
        Add-Failure "Required public file is absent: $path"
    }
}

$forbiddenPathPattern = [regex]::new(
    '(?i)(^|/)(?:data|docs|tests|xapr|release|github_release|ftp-upload|backups?|build(?:-[^/]*)?|vcpkg_installed|\.vs|\.vscode|\.idea|\.claude-octopus|local-only)(/|$)|' +
    '(?:^|/)(?:api-token|credentials\.json|secrets(?:\.[^/]*)?\.json)$|' +
    '\.(?:db|db-wal|db-shm|sqlite|sqlite3|log|dmp|dump|pem|key|pfx|p12|user|suo|pyc)$'
)
$binaryExtensions = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($extension in @('.ico', '.png', '.jpg', '.jpeg', '.gif', '.webp', '.zip')) {
    $null = $binaryExtensions.Add($extension)
}

$userPathExpression = '(?i)(?:[A-Z]:[\\/]Users[\\/]|/' + 'Users/|/' +
    'home/)[^\\/\s"'']+'
$userPathPattern = [regex]::new($userPathExpression)
$workspacePathPattern = [regex]::new('(?i)(?:C|D):[\\/]+\.AI(?:[\\/]|\b)')
$snowflakePattern = [regex]::new('(?<!\d)\d{17,20}(?!\d)')
$quotedSnowflakePattern = [regex]::new('["''](?<id>\d{17,20})["'']')
$emailPattern = [regex]::new(
    '(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b')
$secretPatterns = @(
    [regex]::new('\bgh[pousr]_[A-Za-z0-9]{20,}\b'),
    [regex]::new('\bgithub_pat_[A-Za-z0-9_]{20,}\b'),
    [regex]::new('\bsk-[A-Za-z0-9_-]{20,}\b'),
    [regex]::new('\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    [regex]::new('\bxox[baprs]-[A-Za-z0-9-]{10,}\b'),
    [regex]::new('(?<![A-Za-z0-9_-])(?:mfa\.[A-Za-z0-9_-]{20,}|[A-Za-z0-9_-]{23,28}\.[A-Za-z0-9_-]{6}\.[A-Za-z0-9_-]{25,})(?![A-Za-z0-9_-])'),
    [regex]::new(('-----BE' + 'GIN (?:[A-Z ]+ )?PRIVATE KEY-----'))
)

$syntheticSecretFile = 'src/app/AppSelftest.cpp'
$syntheticPemMarker = '-----BE' + 'GIN PRIVATE KEY-----'
$syntheticValues = @(
    ('sk-' + 'live-not-a-real-secret-1234567890'),
    ('MTIzNDU2Nzg5MDEyMzQ1Njc4' + '.GAbcDe.FgHiJkLmNoPqRsTuVwXyZ')
)

$manifest = New-Object 'System.Collections.Generic.List[string]'
foreach ($path in $candidates) {
    $anchored = '/' + $path
    $allowed = $exact.Contains($anchored)
    if (-not $allowed) {
        foreach ($prefix in $prefixes) {
            if ($anchored.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $allowed = $true
                break
            }
        }
    }
    if (-not $allowed) { Add-Failure "Path is outside the public allow-list: $path" }
    if ($forbiddenPathPattern.IsMatch($path)) {
        Add-Failure "Runtime, secret, or generated path is public: $path"
    }

    $absolute = Join-Path $repo ($path.Replace('/', '\'))
    $cursorPath = $absolute
    $linked = $false
    while ($cursorPath -and $cursorPath -ne $repo) {
        $cursor = Get-Item -LiteralPath $cursorPath -Force -ErrorAction SilentlyContinue
        if ($null -eq $cursor) { break }
        if ($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            Add-Failure "Linked/reparse candidate is public: $path"
            $linked = $true
            break
        }
        $cursorPath = Split-Path -Parent $cursorPath
    }
    if ($linked) { continue }
    if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) {
        Add-Failure "Candidate file is missing from the worktree: $path"
        continue
    }
    $hash = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest.Add("$path`t$hash")

    if ($binaryExtensions.Contains([System.IO.Path]::GetExtension($path))) { continue }
    $content = [System.IO.File]::ReadAllText($absolute)
    if ($userPathPattern.IsMatch($content)) {
        Add-Failure "Absolute user-profile path found in: $path"
    }
    if ($workspacePathPattern.IsMatch($content)) {
        Add-Failure "Original developer workspace path found in: $path"
    }

    $secretScan = $content.Replace("`r`n", "`n")
    if ($path -eq $syntheticSecretFile) {
        foreach ($value in $syntheticValues) { $secretScan = $secretScan.Replace($value, '') }
        if ([regex]::Matches($secretScan, [regex]::Escape($syntheticPemMarker)).Count -eq 2) {
            $secretScan = $secretScan.Replace($syntheticPemMarker, '')
        }
    }
    foreach ($pattern in $secretPatterns) {
        if ($pattern.IsMatch($secretScan)) {
            Add-Failure "Credential-shaped content found in: $path"
            break
        }
    }

    foreach ($match in $quotedSnowflakePattern.Matches($content)) {
        if (-not (Test-SyntheticDiscordId $match.Groups['id'].Value)) {
            Add-Failure "Non-synthetic Discord-style identifier found in: $path"
            break
        }
    }

    if (-not $path.StartsWith(
            'third_party/licenses/', [System.StringComparison]::OrdinalIgnoreCase)) {
        $emailScan = $content.Replace('x@y.com', '').Replace(
            'not-a-real-user-token@github.com', '')
        if ($emailPattern.IsMatch($emailScan)) {
            Add-Failure "Email address found outside a preserved license notice: $path"
        }
    }

    if (($path -eq 'README.md' -or $path -eq 'CHANGELOG.md' -or
         $path.StartsWith('docs/', [System.StringComparison]::OrdinalIgnoreCase)) -and
        $snowflakePattern.IsMatch($content)) {
        Add-Failure "Discord-style numeric identifier found in public documentation: $path"
    }
}

$ignoreProbes = @(
    'changelog.txt',
    'data/devhub.db', 'data/devhub.db-wal', 'data/api-token',
    'build/Release/devhub.exe', 'build-ci/Debug/devhub.pdb',
    'vcpkg_installed/vcpkg/vcpkg-running.lock', 'backups/source.cpp',
    'src/app/backups/source.cpp', '.env', 'local-only/operator.md',
    '.github/workflows/ci.yml', 'docs/RELEASE-WORKFLOW.md',
    'tests/test_core.cpp',
    'tools/devhub-control/tests/private.ps1', 'tools/docs/operator.md',
    '2. Prepare_release.py', '3. Push_release.py', 'xapr/config.json',
    'ftp-upload/downloads/xa-devhub/latest.json', 'github_release/receipt.json',
    'devhub-latest.json', 'devhub-export-20260812.md',
    'ticket-files/I1/private.bin', 'release-package.zip'
)
foreach ($probe in $ignoreProbes) {
    & git -C $repo check-ignore --no-index --quiet -- $probe
    if ($LASTEXITCODE -ne 0) { Add-Failure "Expected ignore rule is missing for: $probe" }
}
& git -C $repo check-ignore --no-index --quiet -- '.env.example'
if ($LASTEXITCODE -eq 0) { Add-Failure '.env.example must remain shareable' }

$manifestText = ($manifest | Sort-Object) -join "`n"
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $manifestHash = ([BitConverter]::ToString(
        $sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($manifestText))
    )).Replace('-', '').ToLowerInvariant()
} finally {
    $sha256.Dispose()
}

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Host "FAIL: $failure" }
    Write-Host "PUBLIC_REPO_AUDIT=FAIL failures=$($failures.Count) candidates=$($candidates.Count)"
    exit 1
}

Write-Host "PUBLIC_REPO_AUDIT=PASS candidates=$($candidates.Count) manifest_sha256=$manifestHash"
exit 0
