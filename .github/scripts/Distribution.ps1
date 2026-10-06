# Shared by release staging and the public committed-source exporter.
# Only explicit development-test locations/naming conventions are removed.
Set-StrictMode -Version Latest

function Test-ConvaiDistributionTestPath([string]$RelativePath) {
    $path = $RelativePath.Replace('\','/').TrimEnd('/')
    if (-not $path -or $path.StartsWith('/') -or $path.Contains(':') -or $path -match '(^|/)\.\.?(/|$)|[\x00-\x1f]') { throw 'Invalid distribution-relative path.' }
    $segments = $path.Split('/')
    if ($segments[0] -eq 'Source') {
        if ($segments.Count -gt 1 -and $segments[1] -eq 'ThirdParty') { return $false }
        if ($segments.Count -gt 1 -and $segments[1] -eq 'ConvaiTests') { return $true }
        if ($segments -contains 'Tests') { return $true }
        return $segments[-1] -cmatch '(?:Test|Tests)\.(?:cpp|cc|cxx|h|hpp|inl)$'
    }
    if ($segments[0] -in @('Resources','.github')) {
        return ($segments -contains 'Tests') -or $segments[-1] -match '\.Tests\.(?:ps1|py)$'
    }
    if ($segments[0] -eq 'Binaries') {
        return $segments[-1] -match '(?:^|[-_])ConvaiTests(?:[-.]|$)'
    }
    return $false
}

function Assert-ConvaiDistributionPath([string]$Path,[string]$Root) {
    $base = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    $full = [IO.Path]::GetFullPath($Path).TrimEnd([char[]]'\/')
    if ($full -ne $base -and -not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Distribution path escapes its plugin root.' }
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Distribution paths cannot be redirected: $cursor" }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $full
}

function Get-ConvaiDistributionEntries([string]$PluginRoot) {
    $root = Assert-ConvaiDistributionPath $PluginRoot $PluginRoot
    if (-not (Test-Path -LiteralPath (Join-Path $root 'ConvAI.uplugin') -PathType Leaf)) { throw 'Distribution staging requires the ConvAI.uplugin root.' }
    $queue = [Collections.Generic.Queue[string]]::new()
    foreach ($name in @('Source','Resources','Binaries','.github')) {
        $directory = Join-Path $root $name
        if (Test-Path -LiteralPath $directory) { Assert-ConvaiDistributionPath $directory $root | Out-Null; $queue.Enqueue($directory) }
    }
    # Enumerate one level at a time: reject a link before considering its children.
    while ($queue.Count) {
        foreach ($entry in Get-ChildItem -LiteralPath $queue.Dequeue() -Force) {
            # Its parent was validated before enqueueing; inspect this entry before
            # traversal without rewalking every ancestor for every source file.
            $full = [IO.Path]::GetFullPath($entry.FullName)
            if (-not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Distribution entry escapes its plugin root.' }
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Distribution paths cannot be redirected: $full" }
            $entry
            # Vendored native bundles have their own checksum/packaging contract;
            # this policy never edits or searches their implementation files.
            $relative = $entry.FullName.Substring($root.Length + 1).Replace('\','/')
            if ($entry.PSIsContainer -and $relative -ne 'Source/ThirdParty') { $queue.Enqueue($entry.FullName) }
        }
    }
}

function Assert-ConvaiDistributionSource([string]$PluginRoot,[object[]]$Entries,[string[]]$RemovedHeaders = @(),[switch]$BeforeRemoval) {
    $root = [IO.Path]::GetFullPath($PluginRoot).TrimEnd([char[]]'\/')
    $headers = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($header in $RemovedHeaders) { [void]$headers.Add($header) }
    foreach ($entry in $Entries) {
        $relative = $entry.FullName.Substring($root.Length + 1).Replace('\','/')
        if (Test-ConvaiDistributionTestPath $relative) {
            if ($BeforeRemoval) { continue }
            throw "Internal test file or directory remains in the distribution: $relative"
        }
        if ($entry.PSIsContainer -or -not $relative.StartsWith('Source/',[StringComparison]::OrdinalIgnoreCase) -or
            $relative.StartsWith('Source/ThirdParty/',[StringComparison]::OrdinalIgnoreCase) -or $entry.Extension -notin @('.cpp','.cc','.cxx','.h','.hpp','.inl','.cs')) { continue }
        $source = [IO.File]::ReadAllText($entry.FullName)
        if ($source -match '\b(?:IMPLEMENT_[A-Z_]*AUTOMATION_TEST|BEGIN_DEFINE_SPEC|DEFINE_SPEC)\s*\(') {
            throw "Automation code remains in production source: $relative. Move its tests into a Tests directory or a *Tests.cpp file; production files are never deleted automatically."
        }
        foreach ($include in [regex]::Matches($source,'(?m)^\s*#\s*include\s*[<"]([^>"\r\n]+)[>"]')) {
            $name = $include.Groups[1].Value.Replace('\','/')
            if ($headers.Contains(($name.Split('/'))[-1]) -or $name -match '(?:^|/)Tests/' -or $name -eq 'Misc/AutomationTest.h') {
                throw "Production source $relative includes a removed test fixture: $name"
            }
        }
    }
}

function Assert-ConvaiDistribution([string]$PluginRoot,[string[]]$RemovedHeaders = @()) {
    $root = Assert-ConvaiDistributionPath $PluginRoot $PluginRoot
    $descriptorPath = Assert-ConvaiDistributionPath (Join-Path $root 'ConvAI.uplugin') $root
    $descriptor = [IO.File]::ReadAllText($descriptorPath) | ConvertFrom-Json
    if (@($descriptor.Modules | Where-Object { $_.Name -eq 'ConvaiTests' }).Count) { throw 'ConvaiTests is still enabled in the distribution descriptor.' }
    $entries = @(Get-ConvaiDistributionEntries $root)
    Assert-ConvaiDistributionSource $root $entries $RemovedHeaders
    foreach ($entry in $entries | Where-Object { -not $_.PSIsContainer -and $_.Extension -eq '.modules' }) {
        $modules = [IO.File]::ReadAllText($entry.FullName) | ConvertFrom-Json
        if ($modules.PSObject.Properties['Modules'] -and $modules.Modules.PSObject.Properties['ConvaiTests']) { throw 'A binary module manifest still references ConvaiTests.' }
    }
}

function Remove-ConvaiDistributionTests([string]$PluginRoot) {
    $root = Assert-ConvaiDistributionPath $PluginRoot $PluginRoot
    $descriptorPath = Assert-ConvaiDistributionPath (Join-Path $root 'ConvAI.uplugin') $root
    $descriptor = [IO.File]::ReadAllText($descriptorPath) | ConvertFrom-Json
    $entries = @(Get-ConvaiDistributionEntries $root)
    $remove = @($entries | Where-Object { Test-ConvaiDistributionTestPath $_.FullName.Substring($root.Length + 1) })
    $headers = @($remove | Where-Object { -not $_.PSIsContainer -and $_.Extension -in @('.h','.hpp','.inl') } | ForEach-Object { $_.Name })
    # Refuse dangling includes or mixed production/test code before deleting anything.
    Assert-ConvaiDistributionSource $root $entries $headers -BeforeRemoval
    foreach ($entry in $remove | Where-Object { -not $_.PSIsContainer }) {
        $path = Assert-ConvaiDistributionPath $entry.FullName $root
        Remove-Item -LiteralPath $path -Force
    }
    foreach ($entry in $remove | Where-Object { $_.PSIsContainer } | Sort-Object { $_.FullName.Length } -Descending) {
        $path = Assert-ConvaiDistributionPath $entry.FullName $root
        if (@(Get-ChildItem -LiteralPath $path -Force).Count) { throw "The test directory changed during staging: $path" }
        Remove-Item -LiteralPath $path -Force
    }
    if (@($descriptor.Modules | Where-Object { $_.Name -eq 'ConvaiTests' }).Count) {
        $descriptor.Modules = @($descriptor.Modules | Where-Object { $_.Name -ne 'ConvaiTests' })
        [IO.File]::WriteAllText($descriptorPath,($descriptor | ConvertTo-Json -Depth 50),[Text.UTF8Encoding]::new($false))
    }
    Assert-ConvaiDistribution $root $headers
    Write-Host "Distribution checked: removed $($remove.Count) internal test files/directories; production source has no test fixture dependencies."
}
