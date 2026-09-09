# =============================================================================
# XA DevHub API Control
# Version: 1.10
# Description: Safe localhost-only PowerShell client for XA DevHub automation.
# Features: Token-aware health preflight, structured JSON payloads, WhatIf mutation
#           preview, UTF-8 output, bounded queries, exact reads, and mutation readback.
# Important: Never edits devhub.db. DevHub must own all database writes.
# Metadata: Author=XA/Codex; Created=2026-07-15; PowerShell=5.1+
# Release notes: 1.10 - Add optimistic title-only updates and verified canonical ticket merges.
#                1.09 - Add strict project-bound work creation with exact readback.
#                1.08 - Resolve exact-instance LocalAppData rendezvous records and fail closed on stale discovery.
#                1.07 - Remove machine-local token defaults and discover the running DevHub token portably.
#                1.06 - Add exact project reads and complete create/update with direct readback.
#                1.05 - Add additive historical work import with per-item direct readback.
#                1.04 - Add exact work-item reads and validated status mutation with direct readback.
#                1.03 - Verify mutations through direct or owning aggregate readback; label source-only writes provisional.
#                1.02 - Report context now explicitly captures approval evidence; added read-only report-preview.
#                1.01 - Added exact workflow-resume targeting and shared packet routes.
#                1.00 - Initial knowledge, report, and workflow control surface.
# =============================================================================

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory)]
    [ValidateSet(
        'health', 'stats', 'project-list', 'project-get', 'project-save',
        'work-list', 'work-get', 'work-save', 'work-title', 'work-merge', 'work-status',
        'work-import',
        'review-context', 'release-context',
        'knowledge-list', 'knowledge-get', 'knowledge-save', 'source-save',
        'knowledge-add-claim', 'knowledge-link', 'knowledge-health',
        'knowledge-context', 'conflict-list', 'conflict-save',
        'conflict-resolve', 'report-list', 'report-get', 'report-save',
        'report-status', 'report-preview', 'report-context', 'workflow-list', 'workflow-get',
        'workflow-save', 'workflow-status', 'workflow-artifact',
        'workflow-evidence', 'workflow-resume', 'learning-list',
        'learning-save', 'processing-list', 'processing-save',
        'retrieval-list', 'retrieval-save'
    )]
    [string]$Action,

    [string]$BaseUrl,
    [string]$TokenPath,
    [long]$ProjectId = 0,
    [long]$Id = 0,
    [string]$Query = '',
    [string]$Kind = '',
    [string]$Status = '',
    [string]$InputJson = '',
    [string]$OutputPath = '',
    [ValidateRange(0, 3)][int]$Level = 2,
    [ValidateRange(1, 200)][int]$Limit = 50,
    [ValidateRange(1, 3650)][int]$Days = 30,
    [ValidateRange(2, 120)][int]$TimeoutSec = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-Config {
    $configPath = Join-Path $PSScriptRoot 'devhub.config.json'
    if (-not (Test-Path -LiteralPath $configPath)) { return $null }
    try { return Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json }
    catch { throw "Invalid config JSON at ${configPath}: $($_.Exception.Message)" }
}

function Get-CanonicalLoopbackOrigin([string]$Url) {
    try { $parsed = [Uri]$Url }
    catch { throw 'BaseUrl must be a valid HTTP loopback URL.' }
    $hostAllowed = $parsed.DnsSafeHost -ieq 'localhost'
    $hostAddress = $null
    if ([Net.IPAddress]::TryParse($parsed.DnsSafeHost, [ref]$hostAddress)) {
        $hostAllowed = $hostAllowed -or
            $hostAddress.Equals([Net.IPAddress]::Loopback) -or
            $hostAddress.Equals([Net.IPAddress]::IPv6Loopback)
    }
    if (-not $parsed.IsAbsoluteUri -or $parsed.Scheme -ne 'http' -or
        -not $hostAllowed -or
        -not [string]::IsNullOrEmpty($parsed.UserInfo) -or
        -not [string]::IsNullOrEmpty($parsed.Query) -or
        -not [string]::IsNullOrEmpty($parsed.Fragment) -or
        $parsed.AbsolutePath -notin @('', '/')) {
        throw 'BaseUrl must be an origin-only HTTP loopback address (127.0.0.1, localhost, or ::1).'
    }
    $effectivePort = if ($parsed.IsDefaultPort) { 80 } else { $parsed.Port }
    return "http://127.0.0.1:$effectivePort"
}

function Get-DefaultRendezvousPath([string]$CanonicalOrigin) {
    $localAppData = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        throw 'LocalApplicationData is unavailable for DevHub API discovery.'
    }
    $effectivePort = ([Uri]$CanonicalOrigin).Port
    return Join-Path $localAppData "XA DevHub\api\api-token-v1-$effectivePort.json"
}

function Read-RendezvousToken([string]$Path, [string]$CanonicalOrigin) {
    try { $length = (Get-Item -LiteralPath $Path -Force).Length }
    catch { throw "Could not inspect the DevHub API rendezvous at ${Path}: $($_.Exception.Message)" }
    if ($length -le 0 -or $length -gt 4096) {
        throw "DevHub API rendezvous at ${Path} must be between 1 byte and 4 KiB."
    }
    try { $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 }
    catch { throw "Could not read the DevHub API rendezvous at ${Path}: $($_.Exception.Message)" }
    if ([Text.Encoding]::UTF8.GetByteCount($raw) -gt 4096) {
        throw "DevHub API rendezvous at ${Path} changed while it was being read."
    }
    try { $record = $raw | ConvertFrom-Json }
    catch { throw "DevHub API rendezvous at ${Path} is not valid JSON." }
    foreach ($property in @('schema', 'origin', 'pid', 'token')) {
        if ($record.PSObject.Properties.Name -notcontains $property) {
            throw "DevHub API rendezvous at ${Path} is missing required metadata."
        }
    }
    if ([string]$record.schema -cne 'xa-devhub.api-rendezvous/v1') {
        throw "DevHub API rendezvous at ${Path} uses an unsupported schema."
    }
    if ([string]$record.origin -cne $CanonicalOrigin) {
        throw "DevHub API rendezvous at ${Path} does not match $CanonicalOrigin."
    }
    $recordProcessId = 0L
    if (-not [long]::TryParse([string]$record.pid, [ref]$recordProcessId) -or
        $recordProcessId -le 0) {
        throw "DevHub API rendezvous at ${Path} has invalid process metadata."
    }
    try { $process = Get-Process -Id $recordProcessId -ErrorAction Stop }
    catch { throw "DevHub API rendezvous at ${Path} is stale; its DevHub process is not running." }
    if ($process.ProcessName -ine 'devhub') {
        throw "DevHub API rendezvous at ${Path} is stale; its process identity is not DevHub."
    }
    $token = [string]$record.token
    if ($token -cnotmatch '^[0-9a-f]{64}$') {
        throw "DevHub API rendezvous at ${Path} has an invalid token shape."
    }
    return $token
}

function Find-LegacyRunningDevHubTokenPath {
    $processes = @(Get-Process -Name 'devhub' -ErrorAction SilentlyContinue)
    if ($processes.Count -ne 1) { return '' }

    $process = $processes[0]
    try {
        $executablePath = [string]$process.Path
        $versionText = [string]$process.FileVersion
    } catch { return '' }
    $version = $null
    if ([string]::IsNullOrWhiteSpace($executablePath) -or
        -not [Version]::TryParse($versionText, [ref]$version) -or
        $version.Major -ne 1 -or $version.Minor -ne 0 -or
        $version.Build -ne 5 -or $version.Revision -notin @(-1, 0)) {
        return ''
    }
    return Join-Path (Split-Path -Parent $executablePath) 'data\api-token'
}

$config = Read-Config
if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    if ($null -ne $config -and $config.PSObject.Properties.Name -contains 'base_url') {
        $BaseUrl = [string]$config.base_url
    } else {
        $BaseUrl = 'http://127.0.0.1:21100'
    }
}
$BaseUrl = Get-CanonicalLoopbackOrigin $BaseUrl

$explicitTokenPath = -not [string]::IsNullOrWhiteSpace($TokenPath)
if (-not $explicitTokenPath) {
    if ($null -ne $config -and $config.PSObject.Properties.Name -contains 'token_path' -and
        -not [string]::IsNullOrWhiteSpace([string]$config.token_path)) {
        $TokenPath = [string]$config.token_path
        $explicitTokenPath = $true
    } elseif (-not [string]::IsNullOrWhiteSpace([string]$env:DEVHUB_TOKEN_PATH)) {
        $TokenPath = [string]$env:DEVHUB_TOKEN_PATH
        $explicitTokenPath = $true
    }
}

if ($explicitTokenPath) {
    if (-not (Test-Path -LiteralPath $TokenPath -PathType Leaf)) {
        throw "Configured DevHub rendezvous path does not name a readable file: $TokenPath"
    }
    $ApiToken = Read-RendezvousToken $TokenPath $BaseUrl
} else {
    $TokenPath = Get-DefaultRendezvousPath $BaseUrl
    if (Test-Path -LiteralPath $TokenPath -PathType Leaf) {
        $ApiToken = Read-RendezvousToken $TokenPath $BaseUrl
    } elseif ($BaseUrl -ceq 'http://127.0.0.1:21100') {
        # Compatibility only for the already-deployed v1.0.5 binary. Rebuilt
        # servers publish the exact-instance LocalAppData rendezvous above.
        $legacyPath = Find-LegacyRunningDevHubTokenPath
        if (-not [string]::IsNullOrWhiteSpace($legacyPath) -and
            (Test-Path -LiteralPath $legacyPath -PathType Leaf)) {
            $ApiToken = (Get-Content -LiteralPath $legacyPath -Raw -Encoding Ascii).Trim()
            if ($ApiToken -cnotmatch '^[0-9a-f]{64}$') {
                throw 'The deployed DevHub legacy token file has an invalid shape.'
            }
        } else {
            throw "No DevHub API rendezvous exists for $BaseUrl. Start the rebuilt DevHub instance."
        }
    } else {
        throw "No DevHub API rendezvous exists for $BaseUrl. Start that exact DevHub instance."
    }
}

function Encode([string]$Value) { return [Uri]::EscapeDataString($Value) }

function Add-Query([string]$Path, [hashtable]$Values) {
    $parts = @(foreach ($entry in $Values.GetEnumerator() | Sort-Object Key) {
        if ($null -eq $entry.Value) { continue }
        $text = [string]$entry.Value
        if ([string]::IsNullOrWhiteSpace($text) -or $text -eq '0') { continue }
        '{0}={1}' -f (Encode $entry.Key), (Encode $text)
    })
    if ($parts.Count -eq 0) { return $Path }
    return $Path + '?' + ($parts -join '&')
}

function Resolve-Body {
    if ([string]::IsNullOrWhiteSpace($InputJson)) { return '{}' }
    $raw = if (Test-Path -LiteralPath $InputJson) {
        Get-Content -LiteralPath $InputJson -Raw
    } else { $InputJson }
    try { return (($raw | ConvertFrom-Json) | ConvertTo-Json -Depth 40 -Compress) }
    catch { throw "InputJson is not valid JSON or a readable file: $($_.Exception.Message)" }
}

function Invoke-DevHub([string]$Method, [string]$Path, [string]$Body = '') {
    $uri = $BaseUrl + $Path
    $params = @{
        Uri = $uri
        Method = $Method
        TimeoutSec = $TimeoutSec
        UseBasicParsing = $true
    }
    if (-not [string]::IsNullOrEmpty($ApiToken)) {
        $params.Headers = @{ 'X-DevHub-Token' = $ApiToken }
    }
    if ($Method -ne 'GET') {
        $params.ContentType = 'application/json; charset=utf-8'
        $params.Body = $Body
    }
    try {
        $response = Invoke-WebRequest @params
    } catch {
        $detail = [string]$_.Exception.Message
        if ($null -ne $_.ErrorDetails -and
            -not [string]::IsNullOrWhiteSpace([string]$_.ErrorDetails.Message)) {
            $detail = [string]$_.ErrorDetails.Message
        }
        throw "DevHub $Method $Path failed: $detail"
    }
    $contentType = [string]$response.Headers.'Content-Type'
    if ($contentType -like 'application/json*') {
        return ($response.Content | ConvertFrom-Json)
    }
    return [string]$response.Content
}

function Require-Id {
    if ($Id -le 0) { throw "Action '$Action' requires -Id." }
}

function Get-CollectionItems($Collection) {
    if ($null -eq $Collection) { return @() }
    if ($Collection -is [array]) { return @($Collection) }
    if ($Collection.PSObject.Properties.Name -contains 'items') {
        return @($Collection.items)
    }
    return @($Collection)
}

function Find-ReadbackRecord($Collection, [long]$RecordId, [string]$Label) {
    if ($RecordId -le 0) { throw "$Label mutation did not return a positive record ID." }
    $record = @(Get-CollectionItems $Collection | Where-Object {
        $_.PSObject.Properties.Name -contains 'id' -and [long]$_.id -eq $RecordId
    } | Select-Object -First 1)
    if ($record.Count -ne 1) {
        throw "$Label mutation returned ID $RecordId, but owning-route readback did not find it."
    }
    return $record[0]
}

if ($Action -ne 'health') {
    $health = Invoke-DevHub 'GET' '/api/health'
    if ($null -eq $health -or -not $health.ok) { throw 'DevHub health preflight did not return ok=true.' }
}

$method = 'GET'
$path = ''
$body = ''
$readback = ''

switch ($Action) {
    'health' { $path = '/api/health' }
    'stats' { $path = '/api/stats' }
    'project-list' { $path = '/api/projects' }
    'project-get' { Require-Id; $path = "/api/projects/$Id" }
    'project-save' {
        $method = 'POST'; $body = Resolve-Body
        if ($Id -gt 0) {
            $priorProject = Invoke-DevHub 'GET' "/api/projects/$Id"
            if ($null -eq $priorProject -or -not $priorProject.ok -or
                [long]$priorProject.id -ne $Id) {
                throw "Project $Id pre-write readback did not return the exact target."
            }
            $path = "/api/projects/$Id"
        } else {
            $path = '/api/projects'
        }
        $readback = 'project'
    }
    'work-list' { $path = '/api/work' }
    'work-get' { Require-Id; $path = "/api/work/$Id" }
    'work-save' {
        $method = 'POST'; $body = Resolve-Body
        $workPayload = $body | ConvertFrom-Json
        if ($workPayload.PSObject.Properties.Name -notcontains 'project_id' -or
            [long]$workPayload.project_id -le 0) {
            throw 'work-save requires a positive project_id in -InputJson.'
        }
        $targetProjectId = [long]$workPayload.project_id
        $priorProject = Invoke-DevHub 'GET' "/api/projects/$targetProjectId"
        if ($null -eq $priorProject -or -not $priorProject.ok -or
            [long]$priorProject.id -ne $targetProjectId) {
            throw "Project $targetProjectId pre-write readback did not return the exact target."
        }
        $path = '/api/work'; $readback = 'work'
    }
    'work-title' {
        Require-Id
        $method = 'POST'; $body = Resolve-Body
        $workTitlePayload = $body | ConvertFrom-Json
        $titleFields = @($workTitlePayload.PSObject.Properties.Name)
        if ($titleFields.Count -ne 2 -or
            $titleFields -notcontains 'title' -or
            $titleFields -notcontains 'expected_updated_at' -or
            [string]::IsNullOrWhiteSpace([string]$workTitlePayload.title) -or
            [string]::IsNullOrWhiteSpace([string]$workTitlePayload.expected_updated_at)) {
            throw 'work-title requires only nonblank title and expected_updated_at fields.'
        }
        $priorWorkTitle = Invoke-DevHub 'GET' "/api/work/$Id"
        if ($null -eq $priorWorkTitle -or -not $priorWorkTitle.ok -or
            [long]$priorWorkTitle.id -ne $Id) {
            throw "Work item $Id pre-write readback did not return the exact target."
        }
        if ([string]$priorWorkTitle.status -ceq 'merged') {
            throw 'work-title cannot modify a merged audit record.'
        }
        if ([string]$priorWorkTitle.updated_at -cne
            [string]$workTitlePayload.expected_updated_at) {
            throw "Work item $Id changed after review; refresh the approved title plan."
        }
        $approvedWorkTitle = ([string]$workTitlePayload.title).Trim()
        $path = "/api/work/$Id/title"; $readback = 'work-title'
    }
    'work-merge' {
        Require-Id
        $method = 'POST'; $body = Resolve-Body
        $workMergePayload = $body | ConvertFrom-Json
        $mergeFields = @($workMergePayload.PSObject.Properties.Name)
        if ($mergeFields.Count -ne 2 -or
            $mergeFields -notcontains 'expected_updated_at' -or
            $mergeFields -notcontains 'sources' -or
            [string]::IsNullOrWhiteSpace([string]$workMergePayload.expected_updated_at)) {
            throw 'work-merge requires only expected_updated_at and sources fields.'
        }
        $mergeSourcePayloads = @($workMergePayload.sources)
        if ($mergeSourcePayloads.Count -lt 1 -or $mergeSourcePayloads.Count -gt 200) {
            throw 'work-merge requires between 1 and 200 source entries.'
        }
        $priorMergeTarget = Invoke-DevHub 'GET' "/api/work/$Id"
        if ($null -eq $priorMergeTarget -or -not $priorMergeTarget.ok -or
            [long]$priorMergeTarget.id -ne $Id) {
            throw "Merge target $Id pre-write readback did not return the exact item."
        }
        if ([string]$priorMergeTarget.status -ceq 'merged') {
            throw 'work-merge cannot retain a merged audit record.'
        }
        if ([string]$priorMergeTarget.updated_at -cne
            [string]$workMergePayload.expected_updated_at) {
            throw "Merge target $Id changed after review; refresh the approved merge plan."
        }
        $mergeIds = New-Object 'System.Collections.Generic.HashSet[long]'
        [void]$mergeIds.Add($Id)
        $priorMergeSources = @()
        foreach ($sourcePayload in $mergeSourcePayloads) {
            $sourceFields = @($sourcePayload.PSObject.Properties.Name)
            if ($sourceFields.Count -ne 2 -or
                $sourceFields -notcontains 'id' -or
                $sourceFields -notcontains 'expected_updated_at' -or
                [long]$sourcePayload.id -le 0 -or
                [string]::IsNullOrWhiteSpace([string]$sourcePayload.expected_updated_at) -or
                -not $mergeIds.Add([long]$sourcePayload.id)) {
                throw 'Each work-merge source requires one distinct positive id and expected_updated_at.'
            }
            $sourceId = [long]$sourcePayload.id
            $priorSource = Invoke-DevHub 'GET' "/api/work/$sourceId"
            if ($null -eq $priorSource -or -not $priorSource.ok -or
                [long]$priorSource.id -ne $sourceId) {
                throw "Merge source $sourceId pre-write readback did not return the exact item."
            }
            if ([string]$priorSource.status -ceq 'merged' -or
                [long]$priorSource.project_id -ne [long]$priorMergeTarget.project_id) {
                throw "Merge source $sourceId is merged already or belongs to another project."
            }
            if ([string]$priorSource.updated_at -cne
                [string]$sourcePayload.expected_updated_at) {
                throw "Merge source $sourceId changed after review; refresh the approved merge plan."
            }
            $priorMergeSources += $priorSource
        }
        $path = "/api/work/$Id/merge"; $readback = 'work-merge'
    }
    'work-status' {
        Require-Id
        if ($Status -notin @('open', 'in_progress', 'blocked', 'completed', 'wont_do')) {
            throw 'work-status requires -Status open, in_progress, blocked, completed, or wont_do.'
        }
        $method = 'POST'; $body = @{ status = $Status } | ConvertTo-Json -Compress
        $path = "/api/work/$Id/status"; $readback = 'work'
    }
    'work-import' {
        $method = 'POST'; $body = Resolve-Body
        $path = '/api/work/import'; $readback = 'work-import'
    }
    'review-context' {
        if ($ProjectId -le 0) { throw 'review-context requires -ProjectId.' }
        $path = Add-Query '/api/export/review' @{ project_id = $ProjectId }
    }
    'release-context' {
        if ($ProjectId -le 0) { throw 'release-context requires -ProjectId.' }
        $path = Add-Query '/api/export/release' @{ project_id = $ProjectId; days = $Days }
    }
    'knowledge-list' {
        $path = Add-Query '/api/knowledge' @{ project_id = $ProjectId; q = $Query; kind = $Kind; status = $Status; limit = $Limit }
    }
    'knowledge-get' { Require-Id; $path = "/api/knowledge/$Id" }
    'knowledge-save' {
        $method = 'POST'; $body = Resolve-Body
        $path = if ($Id -gt 0) { "/api/knowledge/$Id" } else { '/api/knowledge' }
        $readback = 'knowledge'
    }
    'source-save' { $method = 'POST'; $body = Resolve-Body; $path = '/api/knowledge/sources'; $readback = 'source-provisional' }
    'knowledge-add-claim' { Require-Id; $method = 'POST'; $body = Resolve-Body; $path = "/api/knowledge/$Id/claims"; $readback = 'knowledge-claim' }
    'knowledge-link' { $method = 'POST'; $body = Resolve-Body; $path = '/api/knowledge/links'; $readback = 'knowledge-link' }
    'knowledge-health' { $path = Add-Query '/api/knowledge/health' @{ project_id = $ProjectId } }
    'knowledge-context' {
        $path = Add-Query '/api/knowledge/context' @{ project_id = $ProjectId; q = $Query; level = $Level; limit = $Limit }
    }
    'conflict-list' { $path = Add-Query '/api/knowledge/conflicts' @{ project_id = $ProjectId; status = $Status; limit = $Limit } }
    'conflict-save' { $method = 'POST'; $body = Resolve-Body; $path = '/api/knowledge/conflicts'; $readback = 'conflict' }
    'conflict-resolve' { Require-Id; $method = 'POST'; $body = Resolve-Body; $path = "/api/knowledge/conflicts/$Id/resolve"; $readback = 'conflict' }
    'report-list' { $path = Add-Query '/api/reports' @{ project_id = $ProjectId; type = $Kind; status = $Status; limit = $Limit } }
    'report-get' { Require-Id; $path = "/api/reports/$Id" }
    'report-save' { $method = 'POST'; $body = Resolve-Body; $path = '/api/reports'; $readback = 'report' }
    'report-status' {
        Require-Id
        if ([string]::IsNullOrWhiteSpace($Status)) { throw 'report-status requires -Status.' }
        $method = 'POST'; $body = @{ status = $Status } | ConvertTo-Json -Compress
        $path = "/api/reports/$Id/status"; $readback = 'report'
    }
    'report-preview' { $path = Add-Query '/api/reports/context' @{ project_id = $ProjectId; type = $Kind; days = $Days } }
    'report-context' {
        $method = 'POST'; $body = '{}'
        $path = Add-Query '/api/reports/context' @{ project_id = $ProjectId; type = $Kind; days = $Days }
    }
    'workflow-list' { $path = Add-Query '/api/workflows' @{ project_id = $ProjectId; status = $Status; limit = $Limit } }
    'workflow-get' { Require-Id; $path = "/api/workflows/$Id" }
    'workflow-save' { $method = 'POST'; $body = Resolve-Body; $path = '/api/workflows'; $readback = 'workflow' }
    'workflow-status' {
        Require-Id
        if ([string]::IsNullOrWhiteSpace($Status)) { throw 'workflow-status requires -Status.' }
        $method = 'POST'; $body = @{ status = $Status } | ConvertTo-Json -Compress
        $path = "/api/workflows/$Id/status"; $readback = 'workflow'
    }
    'workflow-artifact' { Require-Id; $method = 'POST'; $body = Resolve-Body; $path = "/api/workflows/$Id/artifacts"; $readback = 'workflow-artifact' }
    'workflow-evidence' { Require-Id; $method = 'POST'; $body = Resolve-Body; $path = "/api/workflows/$Id/evidence"; $readback = 'workflow-evidence' }
    'workflow-resume' {
        $path = if ($Id -gt 0) {
            "/api/workflows/$Id/resume"
        } else {
            Add-Query '/api/workflows/resume' @{ project_id = $ProjectId }
        }
    }
    'learning-list' { $path = Add-Query '/api/learnings' @{ project_id = $ProjectId; q = $Query; limit = $Limit } }
    'learning-save' { $method = 'POST'; $body = Resolve-Body; $path = '/api/learnings'; $readback = 'learning' }
    'processing-list' { $path = Add-Query '/api/processing-runs' @{ status = $Status; kind = $Kind; limit = $Limit } }
    'processing-save' { $method = 'POST'; $body = Resolve-Body; $path = '/api/processing-runs'; $readback = 'processing' }
    'retrieval-list' { $path = Add-Query '/api/retrieval-evaluations' @{ engine = $Kind; limit = $Limit } }
    'retrieval-save' { $method = 'POST'; $body = Resolve-Body; $path = '/api/retrieval-evaluations'; $readback = 'retrieval' }
}

if ($method -ne 'GET' -and -not $PSCmdlet.ShouldProcess($BaseUrl + $path, "$method DevHub mutation")) {
    return
}

$result = Invoke-DevHub $method $path $body
$mutationResult = $result

if ($Action -eq 'work-list' -and $result -is [array]) {
    if ($ProjectId -gt 0) { $result = @($result | Where-Object project_id -eq $ProjectId) }
    if ($Status) { $result = @($result | Where-Object status -eq $Status) }
}

if ($readback) {
    $savedId = if ($Id -gt 0) {
        $Id
    } elseif ($mutationResult -isnot [string] -and
              $mutationResult.PSObject.Properties.Name -contains 'id') {
        [long]$mutationResult.id
    } else { 0 }

    switch ($readback) {
        'project' {
            $result = Invoke-DevHub 'GET' "/api/projects/$savedId"
            if ($null -eq $result -or -not $result.ok -or
                [long]$result.id -ne $savedId) {
                throw "Project mutation returned ID $savedId, but exact readback did not match."
            }
        }
        'work' { $result = Invoke-DevHub 'GET' "/api/work/$savedId" }
        'work-title' {
            $result = Invoke-DevHub 'GET' "/api/work/$Id"
            if ($null -eq $result -or -not $result.ok -or
                [long]$result.id -ne $Id -or
                [string]$result.title -cne $approvedWorkTitle) {
                throw "work-title readback did not contain the approved title for I$Id."
            }
            $preservedTitleFields = @(
                'id', 'project_id', 'project_name', 'type', 'body', 'status',
                'priority', 'due_date', 'review_date', 'blocked_reason',
                'created_at', 'completed_at', 'source_id', 'credited', 'origin',
                'tags', 'contributors', 'contributor_link_count', 'event_count',
                'discord_message_count', 'notification_card_count',
                'attachment_count', 'notification_acknowledgement_count',
                'merged_into_id', 'merged_source_count'
            )
            foreach ($field in $preservedTitleFields) {
                $before = $priorWorkTitle.$field | ConvertTo-Json -Compress
                $after = $result.$field | ConvertTo-Json -Compress
                if ($before -cne $after) {
                    throw "work-title changed preserved field '$field' for I$Id."
                }
            }
            if ([string]$priorWorkTitle.title -cne $approvedWorkTitle -and
                [string]$result.updated_at -ceq [string]$priorWorkTitle.updated_at) {
                throw "work-title did not advance the concurrency identity for I$Id."
            }
        }
        'work-merge' {
            if ($mutationResult -is [string] -or
                -not ($mutationResult.PSObject.Properties.Name -contains 'ok') -or
                -not $mutationResult.ok -or
                -not ($mutationResult.PSObject.Properties.Name -contains 'target') -or
                -not ($mutationResult.PSObject.Properties.Name -contains 'sources') -or
                [long]$mutationResult.target.id -ne $Id -or
                @($mutationResult.sources).Count -ne $priorMergeSources.Count) {
                throw 'work-merge did not return the exact target and source identities.'
            }
            $targetReadback = Invoke-DevHub 'GET' "/api/work/$Id"
            if ($null -eq $targetReadback -or -not $targetReadback.ok -or
                [long]$targetReadback.id -ne $Id) {
                throw "work-merge target I$Id was not directly readable after mutation."
            }
            $expectedBody = [string]$priorMergeTarget.body
            foreach ($source in $priorMergeSources) {
                if (-not [string]::IsNullOrEmpty([string]$source.title) -or
                    -not [string]::IsNullOrEmpty([string]$source.body)) {
                    if (-not [string]::IsNullOrEmpty($expectedBody)) {
                        $expectedBody += "`n`n---`n"
                    }
                    $expectedBody += 'Merged feedback: ' + [string]$source.title
                    if (-not [string]::IsNullOrEmpty([string]$source.body)) {
                        $expectedBody += "`n`n" + [string]$source.body
                    }
                }
            }
            $stableTargetFields = @(
                'id', 'project_id', 'project_name', 'type', 'title', 'status',
                'priority', 'due_date', 'review_date', 'blocked_reason',
                'created_at', 'completed_at', 'origin', 'tags', 'event_count',
                'merged_into_id'
            )
            foreach ($field in $stableTargetFields) {
                $before = $priorMergeTarget.$field | ConvertTo-Json -Compress
                $after = $targetReadback.$field | ConvertTo-Json -Compress
                if ($before -cne $after) {
                    throw "work-merge changed preserved target field '$field' for I$Id."
                }
            }
            if ([string]$targetReadback.body -cne $expectedBody -or
                [long]$targetReadback.merged_source_count -ne
                    ([long]$priorMergeTarget.merged_source_count +
                     $priorMergeSources.Count)) {
                throw "work-merge target I$Id failed exact body or audit readback."
            }
            $sourceReadbacks = @()
            foreach ($source in $priorMergeSources) {
                $sourceId = [long]$source.id
                $readSource = Invoke-DevHub 'GET' "/api/work/$sourceId"
                if ($null -eq $readSource -or -not $readSource.ok -or
                    [long]$readSource.id -ne $sourceId -or
                    [long]$readSource.project_id -ne [long]$source.project_id -or
                    [string]$readSource.title -cne [string]$source.title -or
                    [string]$readSource.body -cne [string]$source.body -or
                    [string]$readSource.status -cne 'merged' -or
                    [long]$readSource.merged_into_id -ne $Id -or
                    [long]$readSource.contributor_link_count -ne 0 -or
                    [long]$readSource.discord_message_count -ne 0 -or
                    [long]$readSource.notification_card_count -ne 0 -or
                    [long]$readSource.attachment_count -ne 0 -or
                    -not [string]::IsNullOrEmpty([string]$readSource.completed_at)) {
                    throw "work-merge source I$sourceId failed immutable audit readback."
                }
                $sourceReadbacks += $readSource
            }
            if ([long]$targetReadback.discord_message_count -ne
                    ([long]$priorMergeTarget.discord_message_count +
                     ($priorMergeSources | Measure-Object -Property discord_message_count -Sum).Sum) -or
                [long]$targetReadback.notification_card_count -ne
                    ([long]$priorMergeTarget.notification_card_count +
                     ($priorMergeSources | Measure-Object -Property notification_card_count -Sum).Sum) -or
                [long]$targetReadback.attachment_count -ne
                    ([long]$priorMergeTarget.attachment_count +
                     ($priorMergeSources | Measure-Object -Property attachment_count -Sum).Sum) -or
                [long]$targetReadback.notification_acknowledgement_count -ne
                    ([long]$priorMergeTarget.notification_acknowledgement_count +
                     ($priorMergeSources | Measure-Object -Property notification_acknowledgement_count -Sum).Sum)) {
                throw "work-merge target I$Id did not preserve linked evidence counts."
            }
            $result = [pscustomobject]@{
                target = $targetReadback
                sources = $sourceReadbacks
            }
        }
        'work-import' {
            if ($mutationResult -is [string] -or
                -not ($mutationResult.PSObject.Properties.Name -contains 'ok') -or
                -not $mutationResult.ok -or
                -not ($mutationResult.PSObject.Properties.Name -contains 'items')) {
                throw 'work import did not return an accepted item manifest.'
            }
            $entries = @($mutationResult.items)
            if ($entries.Count -le 0) {
                throw 'work import returned an empty item manifest.'
            }
            if (-not ($mutationResult.PSObject.Properties.Name -contains 'created_count') -or
                -not ($mutationResult.PSObject.Properties.Name -contains 'duplicate_count') -or
                [long]$mutationResult.created_count + [long]$mutationResult.duplicate_count -ne
                    $entries.Count) {
                throw 'work import item counts do not match its returned manifest.'
            }
            $returnedIds = New-Object 'System.Collections.Generic.HashSet[long]'
            $readItems = @()
            foreach ($entry in $entries) {
                if (-not ($entry.PSObject.Properties.Name -contains 'id') -or
                    -not ($entry.PSObject.Properties.Name -contains 'source_id') -or
                    -not ($entry.PSObject.Properties.Name -contains 'result')) {
                    throw 'work import returned an incomplete item entry.'
                }
                $entryId = [long]$entry.id
                if ($entryId -le 0 -or [long]$entry.source_id -ne $entryId -or
                    [string]$entry.result -notin @('created', 'duplicate') -or
                    -not $returnedIds.Add($entryId)) {
                    throw 'work import returned an invalid or repeated item identity.'
                }
                $item = Invoke-DevHub 'GET' "/api/work/$entryId"
                if ($null -eq $item -or -not $item.ok -or
                    [long]$item.id -ne $entryId) {
                    throw "work import readback failed for item $entryId."
                }
                $readItems += $item
            }
            if ($readItems.Count -ne $entries.Count) {
                throw 'work import did not read back every returned item.'
            }

            if (-not ($mutationResult.PSObject.Properties.Name -contains 'verified_graph')) {
                throw 'work import did not return its verified graph counts.'
            }
            $graph = $mutationResult.verified_graph
            $graphFields = @(
                @{ Graph = 'contributor_links'; Readback = 'contributor_link_count' },
                @{ Graph = 'events'; Readback = 'event_count' },
                @{ Graph = 'discord_messages'; Readback = 'discord_message_count' },
                @{ Graph = 'notification_cards'; Readback = 'notification_card_count' },
                @{ Graph = 'notification_acknowledgements'; Readback = 'notification_acknowledgement_count' },
                @{ Graph = 'attachments'; Readback = 'attachment_count' }
            )
            foreach ($mapping in $graphFields) {
                if (-not ($graph.PSObject.Properties.Name -contains $mapping.Graph)) {
                    throw "work import graph is missing $($mapping.Graph)."
                }
                [long]$readbackTotal = 0
                foreach ($item in $readItems) {
                    if (-not ($item.PSObject.Properties.Name -contains $mapping.Readback)) {
                        throw "work import readback is missing $($mapping.Readback)."
                    }
                    $readbackTotal += [long]$item.($mapping.Readback)
                }
                if ($readbackTotal -ne [long]$graph.($mapping.Graph)) {
                    throw "work import readback count disagrees for $($mapping.Graph)."
                }
            }

            if (-not ($mutationResult.PSObject.Properties.Name -contains 'attachments') -or
                -not ($mutationResult.PSObject.Properties.Name -contains 'attachments_restored')) {
                throw 'work import did not return its attachment verification manifest.'
            }
            $attachmentEntries = @($mutationResult.attachments)
            [long]$restoredCount = 0
            foreach ($attachment in $attachmentEntries) {
                if (-not ($attachment.PSObject.Properties.Name -contains 'item_id') -or
                    -not ($attachment.PSObject.Properties.Name -contains 'relative_path') -or
                    -not ($attachment.PSObject.Properties.Name -contains 'actual_size') -or
                    -not ($attachment.PSObject.Properties.Name -contains 'sha256') -or
                    -not ($attachment.PSObject.Properties.Name -contains 'result')) {
                    throw 'work import returned an incomplete attachment entry.'
                }
                $relativePath = [string]$attachment.relative_path
                if (-not $returnedIds.Contains([long]$attachment.item_id) -or
                    [long]$attachment.actual_size -le 0 -or
                    [string]$attachment.sha256 -notmatch '^[0-9a-f]{64}$' -or
                    $relativePath -notmatch '^(ticket-images|ticket-files)[\\/]' -or
                    $relativePath -match '(^|[\\/])\.\.([\\/]|$)' -or
                    [string]$attachment.result -notin @('restored', 'duplicate')) {
                    throw 'work import returned an invalid attachment verification entry.'
                }
                if ([string]$attachment.result -eq 'restored') { $restoredCount++ }
            }
            if ($attachmentEntries.Count -ne [long]$graph.attachments -or
                $restoredCount -ne [long]$mutationResult.attachments_restored) {
                throw 'work import attachment counts disagree with its verified graph.'
            }
            $result = [pscustomobject]@{
                import = $mutationResult
                readback = $readItems
            }
        }
        'knowledge' { $result = Invoke-DevHub 'GET' "/api/knowledge/$savedId" }
        'report' { $result = Invoke-DevHub 'GET' "/api/reports/$savedId" }
        'workflow' { $result = Invoke-DevHub 'GET' "/api/workflows/$savedId" }
        'knowledge-claim' {
            $owner = Invoke-DevHub 'GET' "/api/knowledge/$Id"
            $result = Find-ReadbackRecord @($owner.claims) ([long]$mutationResult.id) 'knowledge claim'
        }
        'knowledge-link' {
            $owner = Invoke-DevHub 'GET' "/api/knowledge/$([long]$mutationResult.from_node_id)"
            $matches = @($owner.links | Where-Object {
                [long]$_.from_node_id -eq [long]$mutationResult.from_node_id -and
                [long]$_.to_node_id -eq [long]$mutationResult.to_node_id -and
                [string]$_.relation -eq [string]$mutationResult.relation
            } | Select-Object -First 1)
            if ($matches.Count -ne 1) { throw 'knowledge link owning-node readback did not find the saved relation.' }
            $result = $matches[0]
        }
        'conflict' {
            $collection = Invoke-DevHub 'GET' (Add-Query '/api/knowledge/conflicts' @{ limit = 200 })
            $result = Find-ReadbackRecord $collection $savedId 'knowledge conflict'
        }
        'workflow-artifact' {
            $owner = Invoke-DevHub 'GET' "/api/workflows/$Id"
            $result = Find-ReadbackRecord @($owner.artifacts) ([long]$mutationResult.id) 'workflow artifact'
        }
        'workflow-evidence' {
            $owner = Invoke-DevHub 'GET' "/api/workflows/$Id"
            $result = Find-ReadbackRecord @($owner.evidence) ([long]$mutationResult.id) 'workflow evidence'
        }
        'learning' {
            $collection = Invoke-DevHub 'GET' (Add-Query '/api/learnings' @{ limit = 200 })
            $result = Find-ReadbackRecord $collection $savedId 'learning'
        }
        'processing' {
            $collection = Invoke-DevHub 'GET' (Add-Query '/api/processing-runs' @{ limit = 200 })
            $result = Find-ReadbackRecord $collection $savedId 'processing run'
        }
        'retrieval' {
            $collection = Invoke-DevHub 'GET' (Add-Query '/api/retrieval-evaluations' @{ limit = 200 })
            $result = Find-ReadbackRecord $collection $savedId 'retrieval evaluation'
        }
        'source-provisional' {
            if ($savedId -le 0 -or -not $mutationResult.ok) {
                throw 'knowledge source mutation did not return an accepted positive ID.'
            }
            $result = $mutationResult
            $result | Add-Member -NotePropertyName '_verification' -Force `
                -NotePropertyValue ([pscustomobject]@{
                    verified = $false
                    mode = 'mutation-response-only'
                    reason = 'The API has no standalone source read route; verify the source through its owning knowledge node after attachment.'
                })
        }
    }

    if ($readback -ne 'source-provisional' -and $result -isnot [string]) {
        $result | Add-Member -NotePropertyName '_mutation' `
            -NotePropertyValue $mutationResult -Force
    }
}

$rendered = if ($result -is [string]) { $result } else { $result | ConvertTo-Json -Depth 40 }
if ($OutputPath) {
    $resolved = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
    $parent = Split-Path -Parent $resolved
    if ($parent) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
    [IO.File]::WriteAllText($resolved, $rendered, [Text.UTF8Encoding]::new($false))
    Write-Output $resolved
} else {
    Write-Output $rendered
}
