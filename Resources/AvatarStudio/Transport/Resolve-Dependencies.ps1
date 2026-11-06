# Copyright Convai Inc. All Rights Reserved.
# Caller holds the common uploader lease. This resolves/downloads only; it never builds or installs a plugin.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$UploaderDirectory,
    [Parameter(Mandatory=$true)][string]$EngineDirectory,
    [Parameter(Mandatory=$true)][string]$ResultFile,
    # Loads functions for offline fixtures. The normal host never passes this switch.
    [switch]$FunctionsOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$script:DependencyUtf8 = New-Object Text.UTF8Encoding($false, $true)
$script:DependencyRepository = 'Conv-AI/Convai-UnrealEngine-HTTP'
$script:DependencyProtocol = 'CONVAIHTTP-transfer-v3'

function Stop-Dependency([string]$Message) { throw [InvalidOperationException]::new('Dependency resolver: ' + $Message) }
function Get-DependencyField($Object, [string]$Name, $Default = $null) {
    if ($null -ne $Object -and $Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $Default
}
function Get-DependencyHash([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create(); $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally { if ($stream) { $stream.Dispose() }; $sha.Dispose() }
}
function Get-DependencyTextHash([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($script:DependencyUtf8.GetBytes($Text))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Assert-DependencyPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathRooted($Path) -or $Path.StartsWith('\') -or $Path.Contains('"') -or $Path -match '[\x00-\x1f]') {
        Stop-Dependency 'Use an ordinary absolute local path for the uploader and its results.'
    }
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Substring(2).Contains(':')) { Stop-Dependency 'A dependency path is not an ordinary local file.' }
    $part = $full
    while ($part) {
        try {
            $attributes = [IO.File]::GetAttributes($part)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { Stop-Dependency 'An uploader, engine, or configuration path points elsewhere. Restore a normal local folder and retry.' }
        } catch [IO.FileNotFoundException] { } catch [IO.DirectoryNotFoundException] { }
        $parent = [IO.Path]::GetDirectoryName($part)
        if ($parent -eq $part) { break }; $part = $parent
    }
    return $full
}
function Test-DependencyBelow([string]$Child, [string]$Parent) {
    return $Child.StartsWith($Parent.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}
function Assert-DependencyUri([string]$Value) {
    $uri = $null
    if (-not [Uri]::TryCreate($Value, [UriKind]::Absolute, [ref]$uri) -or $uri.Scheme -ne 'https' -or $uri.Port -ne 443 -or $uri.UserInfo -or $uri.Fragment -or $Value -match '[\x00-\x20]') {
        Stop-Dependency 'The published dependency address is not a supported HTTPS GitHub address.'
    }
    if ($uri.DnsSafeHost.ToLowerInvariant() -notin @('raw.githubusercontent.com','api.github.com','codeload.github.com','github.com','objects.githubusercontent.com','release-assets.githubusercontent.com')) {
        Stop-Dependency 'The dependency download points outside the approved GitHub hosts.'
    }
    return $uri
}
function Copy-DependencyBounded($InputStream, $OutputStream, [long]$MaxBytes) {
    $buffer = New-Object byte[] 32768; [long]$total = 0
    while (($count = $InputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $total += $count
        if ($total -gt $MaxBytes) { Stop-Dependency 'A dependency response is larger than the supported limit. Check the published configuration and retry.' }
        $OutputStream.Write($buffer, 0, $count)
    }
}
function Receive-DependencyStream([string]$Url, $OutputStream, [long]$MaxBytes) {
    $uri = Assert-DependencyUri $Url
    $watch = [Diagnostics.Stopwatch]::StartNew()
    for ($hop = 0; $hop -le 5; $hop++) {
        $remaining = 30000 - [int]$watch.ElapsedMilliseconds
        if ($remaining -le 0) { Stop-Dependency 'GitHub took too long to respond. Check the connection and retry.' }
        $request = [Net.HttpWebRequest]::Create($uri)
        $request.Method = 'GET'; $request.AllowAutoRedirect = $false
        $request.Timeout = $remaining; $request.ReadWriteTimeout = $remaining
        $request.UseDefaultCredentials = $false; $request.Credentials = $null
        $request.UserAgent = 'ConvaiCloudAvatarsDependencyResolver/1'
        $request.Headers['Cache-Control'] = 'no-cache'
        $request.Accept = 'application/json, application/octet-stream'
        $response = $null; $input = $null
        try {
            try { $response = $request.GetResponse() }
            catch [Net.WebException] {
                if ($_.Exception.Response) { $_.Exception.Response.Dispose() }
                Stop-Dependency 'GitHub could not provide the dependency. Check the connection, release availability, or rate limit and retry.'
            }
            $status = [int]$response.StatusCode
            if ($status -in @(301,302,303,307,308)) {
                if ($hop -eq 5) { Stop-Dependency 'The dependency download redirected too many times.' }
                $location = [string]$response.Headers['Location']
                if (-not $location) { Stop-Dependency 'GitHub returned a redirect without a download address.' }
                $uri = Assert-DependencyUri ([Uri]::new($uri, $location).AbsoluteUri)
                continue
            }
            if ($status -ne 200) { Stop-Dependency 'GitHub returned an unexpected dependency response. Check the release configuration and retry.' }
            if ($response.ContentLength -gt $MaxBytes) { Stop-Dependency 'The dependency download exceeds its supported size limit.' }
            $input = $response.GetResponseStream()
            $buffer = New-Object byte[] 32768; [long]$total = 0
            while ($true) {
                $remaining = 30000 - [int]$watch.ElapsedMilliseconds
                if ($remaining -le 0) { Stop-Dependency 'The dependency download timed out. Check the connection and retry.' }
                if ($input.CanTimeout) { $input.ReadTimeout = $remaining }
                $count = $input.Read($buffer, 0, $buffer.Length)
                if ($count -eq 0) { break }
                $total += $count
                if ($total -gt $MaxBytes) { Stop-Dependency 'The dependency download exceeds its supported size limit.' }
                $OutputStream.Write($buffer, 0, $count)
            }
            if ($response.ContentLength -ge 0 -and $total -ne $response.ContentLength) { Stop-Dependency 'The dependency download ended early. Check the connection and retry.' }
            return
        } finally { if ($input) { $input.Dispose() }; if ($response) { $response.Dispose() }; $request.Abort() }
    }
}
function Read-DependencyRemoteJson([string]$Url, [long]$MaxBytes) {
    [void](Assert-DependencyUri $Url)
    $memory = New-Object IO.MemoryStream
    try {
        Receive-DependencyStream $Url $memory $MaxBytes
        $text = $script:DependencyUtf8.GetString($memory.ToArray())
        return @{value=($text | ConvertFrom-Json); text=$text; sha256=(Get-DependencyTextHash $text); source_url=$Url}
    } finally { $memory.Dispose() }
}
function Read-DependencyLocalJson([string]$Path, [long]$MaxBytes) {
    $safe = Assert-DependencyPath $Path
    if (-not [IO.File]::Exists($safe) -or ([IO.FileInfo]$safe).Length -gt $MaxBytes) { Stop-Dependency 'A dependency configuration or ownership file is missing or too large.' }
    return ([IO.File]::ReadAllText($safe, $script:DependencyUtf8) | ConvertFrom-Json)
}
function Read-DependencyConfiguration([string]$Relative, [string]$ConfigDirectory, [string]$BaseUrl, [string]$ExplicitUrl = '') {
    if ($ConfigDirectory) {
        $path = Join-Path $ConfigDirectory $Relative
        # Accept the historical flat offline fixture layout, while matching V0's checkout-root override.
        if (-not [IO.File]::Exists($path) -and $Relative.StartsWith('resources/')) { $path = Join-Path $ConfigDirectory ([IO.Path]::GetFileName($Relative)) }
        $value = Read-DependencyLocalJson $path 1048576
        $text = [IO.File]::ReadAllText($path, $script:DependencyUtf8)
        return @{value=$value;text=$text;sha256=(Get-DependencyTextHash $text);source_url='Local configuration override'}
    }
    $url = if ($ExplicitUrl) { $ExplicitUrl } else { $BaseUrl + $Relative }
    $uri = Assert-DependencyUri $url
    if ($uri.DnsSafeHost -ne 'raw.githubusercontent.com' -or -not $uri.AbsolutePath.StartsWith('/Conv-AI/Convai-UnrealEngine-ModdingTool/', [StringComparison]::Ordinal)) {
        Stop-Dependency 'The project profile must come from the public Convai Modding Tool configuration repository.'
    }
    return Read-DependencyRemoteJson $url 1048576
}
function Write-DependencySnapshot($Document, [string]$Directory) {
    $path = Assert-DependencyPath (Join-Path $Directory ($Document.sha256 + '.json'))
    if ([IO.File]::Exists($path)) {
        if ((Get-DependencyHash $path) -ne $Document.sha256) { Stop-Dependency 'A saved dependency configuration has changed. Its files were kept; resolve the local conflict and retry.' }
    } else {
        # Content-addressed snapshots belong to the already verified managed uploader.
        $temporary = $path + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
        try {
            [IO.File]::WriteAllText($temporary, $Document.text, $script:DependencyUtf8)
            [IO.File]::Move($temporary, $path)
        } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
    }
    return $path
}
function Write-DependencyResult($Value, [string]$Path) {
    [void](Assert-DependencyPath $Path)
    $temp = $Path + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($temp, (ConvertTo-Json $Value -Depth 8), $script:DependencyUtf8)
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temp, $Path, [NullString]::Value) } else { [IO.File]::Move($temp, $Path) }
    } finally { if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) } }
}
function Read-DependencyZipJson($Entry, [long]$Limit) {
    if ($Entry.Length -le 0 -or $Entry.Length -gt $Limit) { Stop-Dependency 'The HTTP archive has an invalid or oversized compatibility file.' }
    $input = $Entry.Open(); $memory = New-Object IO.MemoryStream
    try { Copy-DependencyBounded $input $memory $Limit; return ($script:DependencyUtf8.GetString($memory.ToArray()) | ConvertFrom-Json) }
    finally { $input.Dispose(); $memory.Dispose() }
}
function Get-DependencyEntryHash($Entry, [long]$Limit) {
    if ($Entry.Length -le 0 -or $Entry.Length -gt $Limit) { Stop-Dependency 'A precompiled HTTP file is empty or exceeds its supported size.' }
    $input = $Entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $buffer = New-Object byte[] 32768; [long]$total = 0
        while (($count = $input.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $total += $count
            if ($total -gt $Limit) { Stop-Dependency 'A precompiled HTTP file exceeds its supported size.' }
            [void]$sha.TransformBlock($buffer, 0, $count, $buffer, 0)
        }
        if ($total -ne $Entry.Length) { Stop-Dependency 'A precompiled HTTP file ended early.' }
        [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        return [BitConverter]::ToString($sha.Hash).Replace('-', '').ToLowerInvariant()
    } finally { $input.Dispose(); $sha.Dispose() }
}
function Assert-DependencyArchive([string]$Path, [string]$Protocol, [string]$EngineVersion, [string]$EngineBuildId = '') {
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        if ($archive.Entries.Count -gt 50000) { Stop-Dependency 'The HTTP archive has too many entries.' }
        $entries = @{}; $roots = @()
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\','/')
            if ($name.StartsWith('/') -or $name.Contains(':') -or $name -match '[\x00-\x1f]' -or @($name.Split('/') | Where-Object { $_ -eq '..' -or $_ -eq '.' }).Count) { Stop-Dependency 'The HTTP archive contains an unsafe path.' }
            if ((($entry.ExternalAttributes -shr 16) -band 61440) -eq 40960) { Stop-Dependency 'The HTTP archive contains a redirected file.' }
            if ($entries.ContainsKey($name)) { Stop-Dependency 'The HTTP archive contains duplicate paths.' }
            $entries[$name] = $entry
            if ($name -eq 'ConvaiHTTP.uplugin' -or $name.EndsWith('/ConvaiHTTP.uplugin', [StringComparison]::OrdinalIgnoreCase)) { $roots += $name.Substring(0, $name.Length - 'ConvaiHTTP.uplugin'.Length) }
        }
        if ($roots.Count -ne 1) { Stop-Dependency 'The release must contain exactly one ConvaiHTTP plugin.' }
        $root = $roots[0]
        foreach ($relative in @('ConvaiHTTP.uplugin','Resources/Transfer/manifest.json','Source/ConvaiHTTPTransfer/ConvaiHTTPTransfer.Build.cs','Source/CONVAIHTTP/CONVAIHTTP.Build.cs')) {
            if (-not $entries.ContainsKey($root + $relative)) { Stop-Dependency 'This HTTP release does not include the Cloud Avatars transfer module. Update the published dependency configuration.' }
        }
        $descriptor = Read-DependencyZipJson $entries[($root + 'ConvaiHTTP.uplugin')] 131072
        $manifest = Read-DependencyZipJson $entries[($root + 'Resources/Transfer/manifest.json')] 65536
        $modules = @(Get-DependencyField $descriptor 'Modules' @())
        $transfer = @($modules | Where-Object { (Get-DependencyField $_ 'Name') -eq 'ConvaiHTTPTransfer' })
        if ($transfer.Count -ne 1 -or (Get-DependencyField $transfer[0] 'Type') -ne 'Editor' -or (Get-DependencyField $transfer[0] 'LoadingPhase') -ne 'Default' -or @((Get-DependencyField $transfer[0] 'PlatformAllowList' @())) -notcontains 'Win64') {
            Stop-Dependency 'The HTTP release does not declare a compatible Win64 editor transfer module.'
        }
        if ((Get-DependencyField $manifest 'schema_version') -ne 1 -or (Get-DependencyField $manifest 'module') -ne 'ConvaiHTTPTransfer' -or
            (Get-DependencyField $manifest 'commandlet') -ne 'ConvaiAvatarTransport' -or (Get-DependencyField $manifest 'transport_identity') -ne $Protocol -or
            (Get-DependencyField $manifest 'result_schema') -ne 1 -or (Get-DependencyField $manifest 'plugin_version') -ne (Get-DependencyField $descriptor 'VersionName')) {
            Stop-Dependency 'The HTTP release uses an incompatible transfer protocol. Update the published dependency configuration.'
        }
        $versions = @(Get-DependencyField $manifest 'engine_versions' @())
        if ($versions.Count -and $versions -notcontains $EngineVersion) { Stop-Dependency 'This HTTP release does not support the selected Unreal Engine version.' }
        $prebuilt = @{available=$false;matches_engine=$false;engine_version='';engine_build_id='';status='This HTTP archive contains source only. The transfer module will be built for this Unreal installation.'}
        if ($entries.ContainsKey($root + 'Resources/Transfer/prebuilt.json')) {
            $binaryManifest = Read-DependencyZipJson $entries[($root + 'Resources/Transfer/prebuilt.json')] 65536
            $binaryVersion = [string](Get-DependencyField $binaryManifest 'engine_version' '')
            $binaryBuild = [string](Get-DependencyField $binaryManifest 'engine_build_id' '')
            if ((Get-DependencyField $binaryManifest 'schema_version') -ne 1 -or $binaryVersion -notmatch '^\d{1,2}\.\d{1,2}$' -or
                $binaryBuild -notmatch '^[A-Za-z0-9._-]{1,128}$' -or (Get-DependencyField $binaryManifest 'platform') -ne 'Win64' -or
                (Get-DependencyField $binaryManifest 'configuration') -ne 'Development') { Stop-Dependency 'The precompiled HTTP compatibility manifest is invalid.' }
            $binaryFiles = Get-DependencyField $binaryManifest 'files'
            $requiredBinaries = @('Binaries/Win64/UnrealEditor-CONVAIHTTP.dll','Binaries/Win64/UnrealEditor-ConvaiHTTPTransfer.dll','Binaries/Win64/UnrealEditor.modules')
            if (-not $binaryFiles -or @($binaryFiles.PSObject.Properties).Count -ne $requiredBinaries.Count) { Stop-Dependency 'The precompiled HTTP manifest must identify exactly two module DLLs and their Unreal module receipt.' }
            foreach ($relative in $requiredBinaries) {
                $expected = [string](Get-DependencyField $binaryFiles $relative '')
                if ($expected -notmatch '^[A-Fa-f0-9]{64}$' -or -not $entries.ContainsKey($root + $relative)) { Stop-Dependency 'A required precompiled HTTP file or checksum is missing.' }
                if ((Get-DependencyEntryHash $entries[($root + $relative)] 268435456) -ne $expected.ToLowerInvariant()) { Stop-Dependency 'A precompiled HTTP file does not match its release checksum.' }
            }
            $receipt = Read-DependencyZipJson $entries[($root + 'Binaries/Win64/UnrealEditor.modules')] 65536
            $receiptModules = Get-DependencyField $receipt 'Modules'
            if ([string](Get-DependencyField $receipt 'BuildId' '') -ne $binaryBuild -or
                [string](Get-DependencyField $receiptModules 'CONVAIHTTP' '') -cne 'UnrealEditor-CONVAIHTTP.dll' -or
                [string](Get-DependencyField $receiptModules 'ConvaiHTTPTransfer' '') -cne 'UnrealEditor-ConvaiHTTPTransfer.dll' -or
                @($receiptModules.PSObject.Properties).Count -ne 2) { Stop-Dependency 'The precompiled HTTP module receipt does not match its compatibility manifest.' }
            $matchesEngine = $EngineBuildId -and $binaryBuild -ceq $EngineBuildId -and $binaryVersion -eq $EngineVersion
            $prebuilt = @{available=$true;matches_engine=[bool]$matchesEngine;engine_version=$binaryVersion;engine_build_id=$binaryBuild;
                status=$(if ($matchesEngine) {'Precompiled HTTP modules match this Unreal installation. A local transfer-module build is not required.'} else {'The precompiled HTTP modules target a different Unreal build. Source compilation is required for this installation.'})}
        }
        return $prebuilt
    } finally { $archive.Dispose() }
}

function Invoke-DependencyResolution([string]$UploaderDirectory, [string]$EngineDirectory, [string]$ResultFile) {
    $safeResult = $null; $partial = $null
    try {
        $uploader = (Assert-DependencyPath $UploaderDirectory).TrimEnd('\','/')
        $engine = (Assert-DependencyPath $EngineDirectory).TrimEnd('\','/')
        $transport = Assert-DependencyPath (Join-Path $uploader 'Transport')
        $result = Assert-DependencyPath $ResultFile
        if (-not (Test-DependencyBelow $result $transport)) { Stop-Dependency 'The dependency result must stay inside the uploader Transport folder.' }
        $marker = Read-DependencyLocalJson (Join-Path $uploader '.convai-workspace.json') 65536
        if ((Get-DependencyField $marker 'version') -ne 2 -or (Get-DependencyField $marker 'owner') -ne 'ConvaiAvatarUploader') { Stop-Dependency 'The uploader ownership record could not be verified. Prepare the managed uploader before resolving dependencies.' }
        $safeResult = $result
        $engineInfo = Read-DependencyLocalJson (Join-Path $engine 'Build/Build.version') 131072
        $major = Get-DependencyField $engineInfo 'MajorVersion'; $minor = Get-DependencyField $engineInfo 'MinorVersion'
        if ($major -notmatch '^\d{1,2}$' -or $minor -notmatch '^\d{1,2}$') { Stop-Dependency 'The selected Unreal Engine version could not be read.' }
        $engineVersion = [string]$major + '.' + [string]$minor
        $engineBuildId = ''
        $engineModulesPath = Join-Path $engine 'Binaries/Win64/UnrealEditor.modules'
        if ([IO.File]::Exists($engineModulesPath)) { $engineBuildId = [string](Get-DependencyField (Read-DependencyLocalJson $engineModulesPath 1048576) 'BuildId' '') }
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($result)) | Out-Null
        $configDir = [Environment]::GetEnvironmentVariable('CONVAI_MODDING_CONFIG_DIR')
        if ($configDir) { $configDir = Assert-DependencyPath $configDir }
        $branch = [Environment]::GetEnvironmentVariable('CONVAI_MODDING_CONFIG_BRANCH')
        # This V1 development line follows V0 staging. Stable/main publication is a separate release decision.
        if (-not $branch) { $branch = 'staging' }
        if ($branch -notmatch '^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$' -or $branch.Contains('..') -or $branch.EndsWith('/')) { Stop-Dependency 'The dependency configuration branch is invalid.' }
        $escapedBranch = ($branch.Split('/') | ForEach-Object { [Uri]::EscapeDataString($_) }) -join '/'
        $baseUrl = 'https://raw.githubusercontent.com/Conv-AI/Convai-UnrealEngine-ModdingTool/' + $escapedBranch + '/'
        $configDocument = Read-DependencyConfiguration 'resources/modding_tool_config.json' $configDir $baseUrl
        $config = $configDocument.value; $configHash = $configDocument.sha256
        $github = Get-DependencyField $config 'github'; $http = Get-DependencyField $github 'convai_http_plugin'
        $repository = [string](Get-DependencyField $http 'repo')
        if ($repository -cne $script:DependencyRepository) { Stop-Dependency 'The dependency configuration must name the public Convai HTTP repository.' }
        $policy = Get-DependencyField $http 'cloud_avatars'; $override = Get-DependencyField $http 'override'
        $protocol = [string](Get-DependencyField $policy 'protocol' $script:DependencyProtocol)
        if ($protocol -ne $script:DependencyProtocol) { Stop-Dependency 'The published dependency protocol is not supported by this Cloud Avatars version.' }
        $versions = @(Get-DependencyField $policy 'engine_versions' @())
        if ($versions.Count -and $versions -notcontains $engineVersion) { Stop-Dependency 'The published HTTP dependency does not support this Unreal Engine version.' }
        $configuredHash = [string](Get-DependencyField $policy 'sha256' '')
        if ($configuredHash -and $configuredHash -notmatch '^[a-fA-F0-9]{64}$') { Stop-Dependency 'The published HTTP checksum is invalid.' }
        $commit = [string](Get-DependencyField $policy 'source_commit' '')
        if ($commit) {
            if ($commit -notmatch '^[a-fA-F0-9]{40}$' -or -not $configuredHash) { Stop-Dependency 'A source candidate requires an immutable commit and a SHA-256 checksum in the published configuration.' }
            $version = 'source-' + $commit.ToLowerInvariant(); $sha = $configuredHash.ToLowerInvariant()
            $url = 'https://codeload.github.com/' + $repository + '/zip/' + $commit.ToLowerInvariant()
            $artifact = 'commit:' + $commit.ToLowerInvariant()
        } else {
            $versionOverride = [string](Get-DependencyField $override 'version' '')
            if ($versionOverride -and ($versionOverride.Length -gt 128 -or $versionOverride -match '[\x00-\x20]')) { Stop-Dependency 'The configured HTTP release version is invalid.' }
            $releaseUrl = 'https://api.github.com/repos/' + $repository + '/releases/'
            if ($versionOverride) { $releaseUrl += 'tags/' + [Uri]::EscapeDataString($versionOverride) } else { $releaseUrl += 'latest' }
            $release = (Read-DependencyRemoteJson $releaseUrl 2097152).value
            $version = [string](Get-DependencyField $release 'tag_name' '')
            if (-not $version -or $version.Length -gt 128 -or $version -match '[\x00-\x20]' -or (Get-DependencyField $release 'draft' $false)) { Stop-Dependency 'GitHub did not return a usable HTTP release.' }
            $assets = @(Get-DependencyField $release 'assets' @())
            $assetOverride = [string](Get-DependencyField $override 'asset' '')
            if ($assetOverride) {
                $candidateAssets = @($assets | Where-Object { (Get-DependencyField $_ 'name') -ceq $assetOverride })
            } else {
                $patterns = @(Get-DependencyField $http 'asset_patterns' @('.zip'))
                $best = -1; $candidateAssets = @()
                foreach ($asset in $assets) {
                    $name = [string](Get-DependencyField $asset 'name' '')
                    if (-not $name.EndsWith('.zip', [StringComparison]::OrdinalIgnoreCase)) { continue }
                    $score = 0
                    foreach ($pattern in $patterns) {
                        if ($pattern -isnot [string] -or -not $pattern -or $pattern.Length -gt 128) { Stop-Dependency 'The configured HTTP release asset patterns are invalid.' }
                        if ($name.IndexOf($pattern, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $score++ }
                    }
                    if ($score -eq 0) { continue }
                    if ($score -gt $best) { $candidateAssets = @($asset); $best = $score } elseif ($score -eq $best) { $candidateAssets += $asset }
                }
            }
            if ($candidateAssets.Count -ne 1) { Stop-Dependency 'The HTTP release asset is missing or ambiguous. Set an exact ZIP asset name in the published configuration.' }
            $asset = $candidateAssets[0]; $name = [string](Get-DependencyField $asset 'name' '')
            if (-not $name.EndsWith('.zip', [StringComparison]::OrdinalIgnoreCase)) { Stop-Dependency 'The configured HTTP release asset must be a ZIP archive.' }
            $digest = [string](Get-DependencyField $asset 'digest' '')
            $releaseHash = ''
            if ($digest -match '^sha256:([a-fA-F0-9]{64})$') { $releaseHash = $Matches[1].ToLowerInvariant() }
            if ($configuredHash -and $releaseHash -and $configuredHash.ToLowerInvariant() -ne $releaseHash) { Stop-Dependency 'The release checksum does not match the published configuration. Keep the current cache and check the release.' }
            $sha = if ($configuredHash) { $configuredHash.ToLowerInvariant() } else { $releaseHash }
            if (-not $sha) { Stop-Dependency 'This HTTP release has no trusted SHA-256 checksum. Update its release digest or published configuration.' }
            $url = [string](Get-DependencyField $asset 'browser_download_url' '')
            $assetId = [string](Get-DependencyField $asset 'id' '')
            if ($assetId -notmatch '^\d+$') { Stop-Dependency 'The HTTP release asset has no stable identity.' }
            $artifact = 'release:' + $version + ':asset:' + $assetId
            $size = Get-DependencyField $asset 'size' 0
            if ($size -le 0 -or $size -gt 268435456) { Stop-Dependency 'The HTTP release archive has an unsupported size.' }
        }
        [void](Assert-DependencyUri $url)
        # One operation uses these exact reviewed documents, even if remote policy changes later.
        $cloudConfig = Get-DependencyField $config 'cloud_avatars'
        $profileDocument = Read-DependencyConfiguration 'resources/avatar_studio_project_profile.json' $configDir $baseUrl ([string](Get-DependencyField $cloudConfig 'project_profile_url' ''))
        $uploaderPolicy = Read-DependencyConfiguration 'resources/asset_uploader_config.json' $configDir $baseUrl
        $versionDocument = Read-DependencyConfiguration 'Version.json' $configDir $baseUrl
        if ((Get-DependencyField $profileDocument.value 'schema_version') -ne 1 -or @(Get-DependencyField $profileDocument.value 'settings' @()).Count -eq 0) {
            Stop-Dependency 'The published project profile is missing its supported declarative settings.'
        }
        $currentVersion = [string](Get-DependencyField $versionDocument.value 'current-ue-version' '')
        $migrationVersion = [string](Get-DependencyField $versionDocument.value 'target-ue-version' '')
        if ($currentVersion -notmatch '^\d{1,2}\.\d{1,2}$' -or $migrationVersion -notmatch '^\d{1,2}\.\d{1,2}$') { Stop-Dependency 'The published current and migration Unreal Engine versions could not be read.' }
        $uploadEngine = Get-DependencyField $uploaderPolicy.value 'unreal-engine'
        if ((Get-DependencyField $uploaderPolicy.value 'raw-project-upload') -isnot [bool] -or -not $uploadEngine) { Stop-Dependency 'The published upload policy is incomplete.' }
        foreach ($platformName in @('windows','linux')) {
            $platformPolicy = Get-DependencyField $uploadEngine $platformName
            if ((Get-DependencyField $platformPolicy 'should-package') -isnot [bool] -or (Get-DependencyField $platformPolicy 'configuration') -notin @('Shipping','Development')) { Stop-Dependency 'A published packaging platform has unsupported settings.' }
        }
        $downloads = Assert-DependencyPath (Join-Path $transport 'Downloads')
        [IO.Directory]::CreateDirectory($downloads) | Out-Null
        $archivePath = Assert-DependencyPath (Join-Path $downloads ($sha + '.zip'))
        if ([IO.File]::Exists($archivePath)) {
            if (([IO.FileInfo]$archivePath).Length -gt 268435456 -or (Get-DependencyHash $archivePath) -ne $sha) { Stop-Dependency 'The cached HTTP archive has changed. Its files were kept; remove only this cached download and retry.' }
            $prebuilt = Assert-DependencyArchive $archivePath $protocol $engineVersion $engineBuildId
        } else {
            $partial = Join-Path $downloads ($sha + '.' + [guid]::NewGuid().ToString('N') + '.partial')
            [void](Assert-DependencyPath $partial)
            $file = [IO.File]::Open($partial, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { Receive-DependencyStream $url $file 268435456; $file.Flush() } finally { $file.Dispose() }
            if ((Get-DependencyHash $partial) -ne $sha) { Stop-Dependency 'The downloaded HTTP archive did not match its published checksum. The current cache was kept; check the release and retry.' }
            $prebuilt = Assert-DependencyArchive $partial $protocol $engineVersion $engineBuildId
            [IO.File]::Move($partial, $archivePath); $partial = $null
        }
        $snapshots = Assert-DependencyPath (Join-Path $transport 'Configuration')
        [IO.Directory]::CreateDirectory($snapshots) | Out-Null
        $configSnapshot = Write-DependencySnapshot $configDocument $snapshots
        $profileSnapshot = Write-DependencySnapshot $profileDocument $snapshots
        $policySnapshot = Write-DependencySnapshot $uploaderPolicy $snapshots
        $versionSnapshot = Write-DependencySnapshot $versionDocument $snapshots
        $projectSettings = Get-DependencyField $config 'project_settings'
        $crossCompilation = Get-DependencyField $config 'cross_compilation'
        $toolchainVersion = [string](Get-DependencyField (Get-DependencyField $crossCompilation 'toolchain_versions') $engineVersion.Replace('.','_') '')
        $toolchainUrl = [string](Get-DependencyField (Get-DependencyField $crossCompilation 'toolchain_download_urls') $toolchainVersion '')
        $identity = $repository + "`n" + $artifact + "`n" + $sha + "`n" + $protocol + "`n" + $engineVersion + "`n" + $engineBuildId + "`n" + $configHash + "`n" + $profileDocument.sha256 + "`n" + $uploaderPolicy.sha256 + "`n" + $versionDocument.sha256
        Write-DependencyResult @{success=$true; schema_version=1; repository=$repository; version=$version; archive_path=$archivePath; archive_sha256=$sha; protocol=$protocol; engine_version=$engineVersion; resolution_id=(Get-DependencyTextHash $identity); config_sha256=$configHash;
            modding_config_path=$configSnapshot; modding_config_sha256=$configHash; modding_config_source_url=$configDocument.source_url;
            project_profile_path=$profileSnapshot; project_profile_sha256=$profileDocument.sha256; project_profile_source_url=$profileDocument.source_url;
            asset_uploader_policy_path=$policySnapshot; asset_uploader_policy_sha256=$uploaderPolicy.sha256;
            version_metadata_path=$versionSnapshot; version_metadata_sha256=$versionDocument.sha256; current_supported_engine_version=$currentVersion; migration_target_engine_version=$migrationVersion;
            required_plugins=@(Get-DependencyField $projectSettings 'required_plugins' @()); metahuman_plugins=@(Get-DependencyField $projectSettings 'metahuman_plugins' @());
            linux_toolchain_version=$toolchainVersion; linux_toolchain_url=$toolchainUrl; reallusion_content_id=[string](Get-DependencyField (Get-DependencyField $config 'google_drive') 'convai_reallusion_content' '');
            engine_build_id=$engineBuildId;prebuilt_available=$prebuilt.available;prebuilt_matches_engine=$prebuilt.matches_engine;
            prebuilt_engine_version=$prebuilt.engine_version;prebuilt_engine_build_id=$prebuilt.engine_build_id;prebuilt_status=$prebuilt.status} $result
        return 0
    } catch {
        $message = 'Could not resolve the HTTP dependency. Check the configuration, connection, and local folder permissions, then retry.'
        if ($_.Exception.Message.StartsWith('Dependency resolver: ')) { $message = $_.Exception.Message.Substring('Dependency resolver: '.Length) }
        if ($safeResult -and [IO.Directory]::Exists([IO.Path]::GetDirectoryName($safeResult))) {
            try { Write-DependencyResult @{success=$false; schema_version=1; error=$message} $safeResult } catch { }
        }
        [Console]::Error.WriteLine($message)
        return 1
    } finally {
        if ($partial -and [IO.File]::Exists($partial)) { [void](Assert-DependencyPath $partial); [IO.File]::Delete($partial) }
    }
}
if (-not $FunctionsOnly) { exit (Invoke-DependencyResolution $UploaderDirectory $EngineDirectory $ResultFile) }
