# This helper owns only per-attempt outputs, never engine installs or shared caches.
Set-StrictMode -Version Latest
$script:ConvaiStorageOwner = 'ConvaiReleaseAttempt'

function Assert-ConvaiRunnerPath([string]$Path,[string]$Boundary) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd([char[]]'\/')
    $base = [IO.Path]::GetFullPath($Boundary).TrimEnd([char[]]'\/')
    if ($full -ne $base -and -not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Runner output path escapes its boundary.' }
    $cursor = $full
    while ($cursor) {
        try {
            $attributes = [IO.File]::GetAttributes($cursor)
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Runner output path is redirected: $cursor" }
        } catch [IO.FileNotFoundException] { }
          catch [IO.DirectoryNotFoundException] { }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $full
}

function Get-ConvaiRunnerRoot([string]$OutputParent,[string]$Repository,[string]$RunId,[string]$Attempt) {
    if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or ($Repository.Split('/') -contains '..') -or
        $RunId -notmatch '^[1-9][0-9]*$' -or $Attempt -notmatch '^[1-9][0-9]*$') { throw 'Invalid runner output identity.' }
    $base = Join-Path $OutputParent (($Repository.Split('/')[1]) + '_Binaries')
    return Assert-ConvaiRunnerPath (Join-Path $base "$RunId-$Attempt") $OutputParent
}

function Get-ConvaiRunnerEntries([string]$Root) {
    $full = Assert-ConvaiRunnerPath $Root $Root
    $queue = [Collections.Generic.Queue[string]]::new(); $queue.Enqueue($full)
    while ($queue.Count) {
        foreach ($entry in Get-ChildItem -LiteralPath $queue.Dequeue() -Force) {
            if (-not ([IO.Path]::GetFullPath($entry.FullName)).StartsWith($full + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
                ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Runner output contains an unsafe entry: $($entry.FullName)" }
            $entry
            if ($entry.PSIsContainer) { $queue.Enqueue($entry.FullName) }
        }
    }
}

function Read-ConvaiRunnerReceipt([string]$OutputParent,[string]$Repository,[string]$RunId,[string]$Attempt) {
    $root = Get-ConvaiRunnerRoot $OutputParent $Repository $RunId $Attempt
    $path = Assert-ConvaiRunnerPath (Join-Path $root '.convai-release-owner.json') $root
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Runner output has no ownership receipt: $root" }
    $receipt = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    if ($receipt.owner -ne $script:ConvaiStorageOwner -or $receipt.schema -ne 1 -or $receipt.repository -cne $Repository -or
        [string]$receipt.run_id -cne $RunId -or [string]$receipt.attempt -cne $Attempt -or $receipt.path -cne $root -or
        $receipt.state -notin @('building','ready_for_release')) { throw "Runner output ownership does not match this attempt: $root" }
    return $receipt
}

function Write-ConvaiRunnerReceipt([string]$Root,[string]$Repository,[string]$RunId,[string]$Attempt,[string]$State) {
    $receipt = @{schema=1;owner=$script:ConvaiStorageOwner;repository=$Repository;run_id=$RunId;attempt=$Attempt;path=$Root;state=$State}
    [IO.File]::WriteAllText((Join-Path $Root '.convai-release-owner.json'),($receipt | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
}

function Assert-ConvaiRunnerDiskBudget([long]$AvailableBytes,[int]$MinimumGiB,[string]$Path) {
    if ($MinimumGiB -lt 1 -or $MinimumGiB -gt 1024) { throw 'CONVAI_RELEASE_MIN_FREE_GIB must be between 1 and 1024.' }
    Write-Host ("Runner storage {0}: {1:N2} GiB available; configured floor {2} GiB." -f $Path,($AvailableBytes / 1GB),$MinimumGiB)
    if ($AvailableBytes -lt ([long]$MinimumGiB * 1GB)) { throw "Not enough free disk space at $Path. At least $MinimumGiB GiB is required before this build. No shared runner data will be deleted automatically." }
}

function Assert-ConvaiRunnerFreeSpace([string]$Path,[int]$MinimumGiB = 30) {
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path)))
    Assert-ConvaiRunnerDiskBudget $drive.AvailableFreeSpace $MinimumGiB $drive.Name
}

function Get-ConvaiRemoteRun([string]$Repository,[string]$RunId,[string]$Attempt) {
    $response = @(& gh api "repos/$Repository/actions/runs/$RunId/attempts/$Attempt")
    if ($LASTEXITCODE -ne 0) { throw "Cannot verify completed GitHub run $RunId attempt $Attempt; its output was not deleted." }
    return ($response -join "`n") | ConvertFrom-Json
}

function Assert-ConvaiRemoteRunIdentity($Run,[string]$Repository,[string]$RunId,[string]$Attempt,[switch]$LegacyFailure) {
    if ([string]$Run.id -cne $RunId -or [string]$Run.run_attempt -cne $Attempt -or $Run.repository.full_name -cne $Repository -or
        $Run.path -ne '.github/workflows/main.yml') { throw 'GitHub run identity does not match the owned release output.' }
    if ($Run.status -ne 'completed') { return $false }
    if ($LegacyFailure -and ($Run.conclusion -ne 'failure' -or $Run.head_branch -ne 'feat/avatar-studio-v1')) { throw 'The explicitly authorized historical output is not the expected failed feature run.' }
    return $true
}

function Remove-ConvaiRunnerTree([string]$Root,[string]$Target) {
    $full = Assert-ConvaiRunnerPath $Target $Root
    # A cancelled runner may leave UAT/UBT alive. Never delete files it still owns.
    foreach ($process in Get-CimInstance Win32_Process -ErrorAction Stop) {
        if ($process.CommandLine -and $process.CommandLine.Replace('/','\').IndexOf($full.Replace('/','\'),[StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "A process still references this generated output (PID $($process.ProcessId)); cleanup is deferred: $full"
        }
    }
    $entries = @(Get-ConvaiRunnerEntries $full)
    $bytes = [long]0
    foreach ($entry in $entries) { if (-not $entry.PSIsContainer) { $bytes += $entry.Length } }
    Write-Host ("Removing verified generated output: {0} ({1:N2} GiB)." -f $full,($bytes / 1GB))
    # Keep the receipt until every other entry is gone. A locked file or an
    # interrupted cleanup must leave authorization for the next attempt.
    $ownerFile = Join-Path $Root '.convai-release-owner.json'
    foreach ($entry in $entries | Where-Object { -not $_.PSIsContainer -and $_.FullName -ne $ownerFile } | Sort-Object FullName) {
        Remove-Item -LiteralPath $entry.FullName -Force -ErrorAction Stop
    }
    foreach ($entry in $entries | Where-Object { $_.PSIsContainer } | Sort-Object { $_.FullName.Length } -Descending) {
        if (@(Get-ChildItem -LiteralPath $entry.FullName -Force).Count) { throw 'Generated output changed during cleanup; its receipt was preserved.' }
        Remove-Item -LiteralPath $entry.FullName -Force -ErrorAction Stop
    }
    if ($full -eq $Root -and (Test-Path -LiteralPath $ownerFile)) {
        if (@(Get-ChildItem -LiteralPath $full -Force | Where-Object { $_.FullName -ne $ownerFile }).Count) { throw 'Generated output changed during cleanup; its receipt was preserved.' }
        Remove-Item -LiteralPath $ownerFile -Force -ErrorAction Stop
    }
    if (@(Get-ChildItem -LiteralPath $full -Force).Count) { throw 'Generated output changed during cleanup.' }
    Remove-Item -LiteralPath $full -Force -ErrorAction Stop
    if (Test-Path -LiteralPath $full) { throw "Generated output cleanup did not finish: $full" }
}

function Save-ConvaiRunnerDiagnostics([string]$Root,[string]$DiagnosticsRoot,[string]$Repository,[string]$RunId,[string]$Attempt) {
    if (-not $DiagnosticsRoot) { return }
    $destination = Assert-ConvaiRunnerPath (Join-Path $DiagnosticsRoot "$($Repository.Replace('/','_'))/$RunId-$Attempt") $DiagnosticsRoot
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    $total = [long]0
    foreach ($entry in @(Get-ConvaiRunnerEntries $Root)) {
        if ($entry.PSIsContainer) { continue }
        $relative = $entry.FullName.Substring($Root.Length + 1).Replace('\','/')
        $isSummary = $relative -in @('.convai-release-owner.json','release-plan.json')
        $isLog = $entry.Extension -in @('.log','.txt','.xml') -and $relative -match '(^|/)(Saved|EditorOpenSmoke)/' -and $relative -notmatch '/Plugins/'
        if (-not ($isSummary -or $isLog) -or $entry.Length -gt 16MB -or $total + $entry.Length -gt 64MB) { continue }
        $target = Assert-ConvaiRunnerPath (Join-Path $destination $relative) $destination
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        [IO.File]::Copy($entry.FullName,$target,$true)
        $total += $entry.Length
    }
    Write-Host "Preserved small build diagnostics in $destination. Compiler output is also retained in the GitHub job log."
}

function Initialize-ConvaiRunnerStorage([string]$OutputParent,[string]$Repository,[string]$RunId,[string]$Attempt,
    [string]$PlanJson,[int]$MinimumGiB = 30,[string]$DiagnosticsRoot,
    [scriptblock]$RunLookup = { param($Repository,$RunId,$Attempt) Get-ConvaiRemoteRun $Repository $RunId $Attempt }) {
    $root = Get-ConvaiRunnerRoot $OutputParent $Repository $RunId $Attempt
    $base = [IO.Path]::GetDirectoryName($root)
    [IO.Directory]::CreateDirectory($base) | Out-Null
    $candidates = @{}
    # Only these two pre-receipt directories were explicitly authorized from CI logs.
    if ($Repository -ceq 'Conv-AI/Convai-UnrealEngine-SDK-Dev') {
        foreach ($oldId in @('34347999294','34357116920')) { $candidates["$oldId-1"] = $true }
    }
    foreach ($directory in Get-ChildItem -LiteralPath $base -Directory -Force) {
        if ($directory.Name -notmatch '^([1-9][0-9]*)-([1-9][0-9]*)$' -or $directory.FullName -eq $root) { continue }
        Assert-ConvaiRunnerPath $directory.FullName $base | Out-Null
        if (Test-Path -LiteralPath (Join-Path $directory.FullName '.convai-release-owner.json') -PathType Leaf) { $candidates[$directory.Name] = $false }
    }
    foreach ($name in @($candidates.Keys)) {
        $parts = $name.Split('-'); $oldRoot = Get-ConvaiRunnerRoot $OutputParent $Repository $parts[0] $parts[1]
        if ($oldRoot -eq $root -or -not (Test-Path -LiteralPath $oldRoot)) { continue }
        $legacy = [bool]$candidates[$name]
        if (-not $legacy) {
            $receipt = Read-ConvaiRunnerReceipt $OutputParent $Repository $parts[0] $parts[1]
            if ($receipt.state -eq 'ready_for_release') { Write-Host "Preserving release ZIPs and plan: $oldRoot"; continue }
        }
        $run = & $RunLookup $Repository $parts[0] $parts[1]
        if (-not (Assert-ConvaiRemoteRunIdentity $run $Repository $parts[0] $parts[1] -LegacyFailure:$legacy)) { Write-Host "Run is still active; preserving $oldRoot"; continue }
        # Inspect the complete tree before adopting either known historical directory.
        @(Get-ConvaiRunnerEntries $oldRoot) | Out-Null
        # The two exact legacy paths were verified from original CI logs. That
        # authorization survives partial deletion; no new receipt write or intact
        # HostProject layout is required when E: is completely full.
        try { Save-ConvaiRunnerDiagnostics $oldRoot $DiagnosticsRoot $Repository $parts[0] $parts[1] }
        catch { Write-Warning "Diagnostic copy unavailable; the GitHub job log remains available. $($_.Exception.Message)" }
        if (-not $legacy) { Read-ConvaiRunnerReceipt $OutputParent $Repository $parts[0] $parts[1] | Out-Null }
        Remove-ConvaiRunnerTree $oldRoot $oldRoot
    }
    Assert-ConvaiRunnerFreeSpace $base $MinimumGiB
    if (Test-Path -LiteralPath $root) { throw "This output attempt already exists: $root. Use a new GitHub run attempt." }
    [IO.Directory]::CreateDirectory($root) | Out-Null
    Write-ConvaiRunnerReceipt $root $Repository $RunId $Attempt 'building'
    [IO.File]::WriteAllText((Join-Path $root 'release-plan.json'),$PlanJson,[Text.UTF8Encoding]::new($false))
    return $root
}

function Complete-ConvaiRunnerBuild([string]$OutputParent,[string]$Repository,[string]$RunId,[string]$Attempt,[bool]$Succeeded,[string]$DiagnosticsRoot) {
    $receipt = Read-ConvaiRunnerReceipt $OutputParent $Repository $RunId $Attempt
    $root = [string]$receipt.path
    try { Save-ConvaiRunnerDiagnostics $root $DiagnosticsRoot $Repository $RunId $Attempt }
    catch { Write-Warning "Diagnostic copy unavailable; the GitHub job log remains available. $($_.Exception.Message)" }
    if ($receipt.state -ne 'ready_for_release') {
        if ($Succeeded) { throw 'Successful build cleanup requires the validated-archives checkpoint.' }
        Remove-ConvaiRunnerTree $root $root; return
    }
    # ZIP paths, release-plan.json and receipt remain for the separate publishing job.
    foreach ($name in @('V5.3','V5.4','V5.5','V5.6','V5.7','V5.8','EditorOpenSmoke','NoBinaries')) {
        $target = Join-Path $root $name
        if (Test-Path -LiteralPath $target) { Remove-ConvaiRunnerTree $root $target }
    }
}

function Set-ConvaiRunnerArchivesReady([string]$OutputParent,[string]$Repository,[string]$RunId,[string]$Attempt,[string[]]$Archives) {
    $receipt = Read-ConvaiRunnerReceipt $OutputParent $Repository $RunId $Attempt
    if (-not $Archives.Count) { throw 'No validated archives were supplied.' }
    foreach ($archive in $Archives) {
        $path = Assert-ConvaiRunnerPath $archive $receipt.path
        if ([IO.Path]::GetDirectoryName($path) -ne $receipt.path -or [IO.Path]::GetFileName($path) -notmatch '^Convai-UE5\.[3-8](?:-(?:Win64|Android))?(?:-marketplace-no-binaries)?\.zip$' -or
            -not (Test-Path -LiteralPath $path -PathType Leaf) -or ([IO.FileInfo]::new($path)).Length -eq 0) { throw 'An expected release archive is absent, empty, or outside this attempt.' }
    }
    Write-ConvaiRunnerReceipt $receipt.path $Repository $RunId $Attempt 'ready_for_release'
}

function Complete-ConvaiRunnerRelease([string]$OutputParent,[string]$Repository,[string]$RunId,[string]$Attempt) {
    $receipt = Read-ConvaiRunnerReceipt $OutputParent $Repository $RunId $Attempt
    if ($receipt.state -ne 'ready_for_release') { throw 'Release cleanup requires a completed build receipt.' }
    Remove-ConvaiRunnerTree $receipt.path $receipt.path
}

function Complete-ConvaiPublishedOutput([string]$OutputParent,[string]$Repository,[string]$RunId,[string]$BuildOutputRoot) {
    # A retry of only the publishing job can consume a prior build attempt.
    $name = [IO.Path]::GetFileName([IO.Path]::GetFullPath($BuildOutputRoot).TrimEnd([char[]]'\/'))
    if ($name -notmatch '^([1-9][0-9]*)-([1-9][0-9]*)$' -or $Matches[1] -cne $RunId) { throw 'Published output does not belong to this GitHub run.' }
    $buildAttempt = $Matches[2]
    $expected = Get-ConvaiRunnerRoot $OutputParent $Repository $RunId $buildAttempt
    if ([IO.Path]::GetFullPath($BuildOutputRoot).TrimEnd([char[]]'\/') -ne $expected) { throw 'Published output root is outside the exact repository attempt.' }
    Complete-ConvaiRunnerRelease $OutputParent $Repository $RunId $buildAttempt
}
