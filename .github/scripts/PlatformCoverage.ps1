# BuildPlugin compiles the host UnrealEditor independently of selected UnrealGame
# targets. UBT emits .precompiled OutputFiles for monolithic game modules and
# BuildPlugin packages both the manifest and its objects (UE5.3-5.8).
. (Join-Path $PSScriptRoot 'Distribution.ps1')

function Get-ConvaiCoverageFiles([string]$Directory,[string]$PluginRoot) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return }
    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue((Assert-ConvaiDistributionPath $Directory $PluginRoot))
    while ($queue.Count) {
        foreach ($entry in Get-ChildItem -LiteralPath $queue.Dequeue() -Force) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Platform coverage cannot traverse redirected paths.' }
            if ($entry.PSIsContainer) { $queue.Enqueue($entry.FullName) }
            elseif ($entry.Extension -eq '.precompiled') { $entry }
        }
    }
}

function Read-ConvaiCoverageJson([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -gt 1048576) { throw "Missing or oversized platform receipt: $Path" }
    try { return [IO.File]::ReadAllText($Path) | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "Invalid platform receipt JSON: $Path" }
}

function Assert-ConvaiPlatformCoverage([string]$PluginRoot,[string[]]$Platforms) {
    $root = Assert-ConvaiDistributionPath $PluginRoot $PluginRoot
    if (-not (Test-Path -LiteralPath (Join-Path $root 'ConvAI.uplugin') -PathType Leaf)) { throw 'Platform coverage requires a packaged ConvAI.uplugin root.' }
    if ($Platforms.Count -eq 0 -or @($Platforms | Select-Object -Unique).Count -ne $Platforms.Count) { throw 'Select at least one unique game platform.' }
    foreach ($platform in $Platforms) { if ($platform -cnotin @('Win64','Android')) { throw "Unsupported game platform: $platform" } }

    # An Android-only package still includes the Windows host editor. Its DLLs
    # never satisfy the independent Windows game-manifest requirement below.
    $hostPath = Assert-ConvaiDistributionPath (Join-Path $root 'Binaries/Win64/UnrealEditor.modules') $root
    $hostReceipt = Read-ConvaiCoverageJson $hostPath
    if (-not $hostReceipt.PSObject.Properties['BuildId'] -or [string]::IsNullOrWhiteSpace([string]$hostReceipt.BuildId) -or
        -not $hostReceipt.PSObject.Properties['Modules'] -or -not $hostReceipt.Modules.PSObject.Properties['Convai']) { throw 'The Windows host editor module receipt is incomplete.' }
    $dllName = $hostReceipt.Modules.Convai
    if ($dllName -isnot [string] -or $dllName -notmatch '^UnrealEditor-Convai(?:-[A-Za-z0-9_-]+)?\.dll$') { throw 'The Windows host editor receipt must identify its Convai DLL.' }
    $dll = Assert-ConvaiDistributionPath (Join-Path ([IO.Path]::GetDirectoryName($hostPath)) $dllName) $root
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf) -or (Get-Item -LiteralPath $dll).Length -eq 0) { throw 'The Windows host editor Convai DLL is missing or empty.' }

    $gameReceipts = [Collections.Generic.List[object]]::new()
    foreach ($file in Get-ConvaiCoverageFiles (Join-Path $root 'Intermediate/Build') $root) {
        $relative = $file.FullName.Substring($root.Length + 1).Replace('\','/')
        $parts = $relative.Split('/')
        $target = [Array]::IndexOf($parts, 'UnrealGame')
        if ($target -lt 0) { continue }
        # Architecture-free Android manifests aggregate a/x objects in UE5.8;
        # Win64 normally uses x64. Older engine layouts omit the architecture.
        if ($target -notin @(3,4) -or $parts.Count -ne $target + 4) { throw "Unexpected game receipt layout: $relative" }
        $platform = $parts[2]
        if ($platform -cnotin $Platforms) { throw "Package contains an unrequested game platform: $platform" }
        if ($parts[-1] -ieq 'Convai.precompiled' -and $parts[-2] -ieq 'Convai') {
            $gameReceipts.Add(@{ File=$file.FullName; Platform=$platform; Configuration=$parts[$target + 1] })
        }
    }
    foreach ($platform in $Platforms) {
        foreach ($configuration in @('Development','Shipping')) {
            $receipts = @($gameReceipts | Where-Object { $_.Platform -ceq $platform -and $_.Configuration -ceq $configuration })
            if ($receipts.Count -eq 0) { throw "Missing $platform $configuration game precompiled receipt for Convai. Host editor DLLs are not game coverage." }
            foreach ($receipt in $receipts) {
                $manifest = Read-ConvaiCoverageJson $receipt.File
                if (-not $manifest.PSObject.Properties['OutputFiles'] -or $manifest.OutputFiles -is [string] -or @($manifest.OutputFiles).Count -eq 0) { throw "Game receipt has no compiled outputs: $($receipt.File)" }
                foreach ($output in $manifest.OutputFiles) {
                    if ($output -isnot [string] -or [string]::IsNullOrWhiteSpace($output) -or [IO.Path]::IsPathRooted($output) -or $output -match '[:\x00-\x1f]') { throw 'Game receipt output must be a relative compiled-file path.' }
                    # Parent components are valid: Android aggregate receipts point
                    # into sibling a/x architecture directories. Resolve then bound.
                    $full = Assert-ConvaiDistributionPath (Join-Path ([IO.Path]::GetDirectoryName($receipt.File)) $output) $root
                    $outputParts = $full.Substring($root.Length + 1).Replace('\','/').Split('/')
                    $outputTarget = [Array]::IndexOf($outputParts, 'UnrealGame')
                    if ($outputParts.Count -lt 6 -or $outputParts[0] -cne 'Intermediate' -or $outputParts[1] -cne 'Build' -or $outputParts[2] -cne $platform -or
                        $outputTarget -notin @(3,4) -or $outputParts[$outputTarget + 1] -cne $configuration) { throw 'Game receipt output points to a different platform or configuration.' }
                    if (-not (Test-Path -LiteralPath $full -PathType Leaf) -or (Get-Item -LiteralPath $full).Length -eq 0) { throw "Game receipt compiled output is missing or empty: $full" }
                }
            }
        }
    }
    Write-Host "Platform coverage verified: Windows host editor; $($Platforms -join ', ') games in Development and Shipping."
}
