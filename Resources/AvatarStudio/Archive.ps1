# Copyright 2026 Convai Inc. All Rights Reserved.
# Called by Avatar Studio with local JSON request/result files. Never execute archive content.
param([Parameter(Mandatory=$true)][string]$RequestPath, [Parameter(Mandatory=$true)][string]$ResultPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$script:Utf8 = New-Object System.Text.UTF8Encoding($false, $true)
$script:MaxEntries = 100000
$script:MaxEntryBytes = [long]17179869184 # 16 GiB per uncompressed entry
$script:MaxExpandedBytes = [long]68719476736 # 64 GiB per archive
$script:MaxZipBytes = [long]10485760000 # The existing Assets signed-upload limit

function Assert-PluginName([string]$Name) {
    if ($Name -notmatch '^[A-Za-z][A-Za-z0-9_]{0,99}$') { throw 'The avatar plugin name is invalid.' }
}

function Read-AcknowledgedMissingPackages($Value, [string]$PluginName) {
    # Do not coerce scalar strings, nulls, or JSON objects into an approval list.
    if ($Value -isnot [Array] -or $Value.Count -gt 15000) { throw 'The missing-reference review must be a bounded list of exact asset package paths.' }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $Value) {
        if ($path -isnot [string] -or $path.Length -gt 1023 -or $path -notmatch '^/[^/]+/[^/]' -or
            $path.EndsWith('/') -or $path.Contains('//') -or $path -match '[\\:*?"<>|'' ,.&!~@#\x00-\x1f\x7f]' -or
            $path.StartsWith('/Script/', [System.StringComparison]::OrdinalIgnoreCase) -or
            $path.StartsWith('/' + $PluginName + '/', [System.StringComparison]::OrdinalIgnoreCase) -or -not $seen.Add($path)) {
            throw 'The missing-reference review is invalid. List exact external asset package paths; missing files inside the avatar plugin cannot be approved.'
        }
    }
    return ,$Value
}

function Get-SafeEntryPath([string]$Name) {
    if ([string]::IsNullOrEmpty($Name)) { throw 'The source archive contains an empty filename.' }
    $normalized = $Name.Replace('\','/')
    if ($normalized.StartsWith('/') -or $normalized.Contains(':')) { throw 'The source archive contains an absolute path or alternate data stream.' }
    $trimmed = $normalized.TrimEnd('/')
    if ($trimmed.Length -gt 2048) { throw 'The source archive contains an excessively long path.' }
    foreach ($segment in $trimmed.Split('/')) {
        if ([string]::IsNullOrEmpty($segment) -or $segment -eq '.' -or $segment -eq '..' -or
            $segment.EndsWith('.') -or $segment.EndsWith(' ') -or $segment.Length -gt 255 -or
            $segment -match '[\x00-\x1f<>"|?*]' -or
            $segment -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw 'The source archive contains an unsafe Windows filename.'
        }
    }
    return $trimmed
}

function Read-SourceCopyMap($Value, [string]$PluginName, $AvailableRelativeFiles) {
    if ($Value -isnot [System.Management.Automation.PSCustomObject] -or @($Value.PSObject.Properties).Count -gt 15000) {
        throw 'The copied-asset map must be a bounded object of exact package paths.'
    }
    $sources = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $targets = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $map = @{}
    foreach ($pair in $Value.PSObject.Properties) {
        $source = [string]$pair.Name
        $target = $pair.Value
        foreach ($path in @($source, $target)) {
            if ($path -isnot [string] -or $path.Length -gt 1023 -or $path -notmatch '^/[^/]+/[^/]' -or
                $path.EndsWith('/') -or $path.Contains('//') -or $path -match '[\\:*?"<>|'' ,.&!~@#\x00-\x1f\x7f]') {
                throw 'The copied-asset map contains an invalid package path.'
            }
        }
        if ($source.StartsWith('/Script/', [StringComparison]::OrdinalIgnoreCase) -or
            $source.StartsWith('/' + $PluginName + '/', [StringComparison]::OrdinalIgnoreCase) -or
            $target -cne ('/' + $PluginName + $source) -or -not $sources.Add($source) -or -not $targets.Add($target)) {
            throw 'Each copied asset must identify its exact external source and unique destination inside this avatar plugin.'
        }
        $relative = Get-SafeEntryPath ('Content/' + $target.Substring($PluginName.Length + 2) + '.uasset')
        if (-not $AvailableRelativeFiles.Contains($relative)) { throw 'A copied asset recorded by the source manifest is missing from the archive.' }
        $map[$source] = $target
    }
    return $map
}

function Assert-NoReparseAncestors([string]$Path) {
    $current = [System.IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($current)) {
        if ([System.IO.File]::Exists($current) -or [System.IO.Directory]::Exists($current)) {
            if (([System.IO.File]::GetAttributes($current) -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'The archive source or destination uses a symbolic link or junction. Use the canonical avatar folder.'
            }
        }
        $parent = [System.IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
}

function Get-PluginFiles([string]$Root) {
    Assert-NoReparseAncestors $Root
    if (-not [System.IO.Directory]::Exists($Root)) { throw 'The prepared avatar plugin folder is missing.' }
    $files = New-Object 'System.Collections.Generic.List[string]'
    $stack = New-Object 'System.Collections.Generic.Stack[string]'
    $stack.Push($Root)
    while ($stack.Count -gt 0) {
        $directory = $stack.Pop()
        foreach ($item in [System.IO.Directory]::EnumerateFileSystemEntries($directory)) {
            $attributes = [System.IO.File]::GetAttributes($item)
            if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'The prepared avatar contains a symbolic link or junction.' }
            if (($attributes -band [System.IO.FileAttributes]::Directory) -ne 0) { $stack.Push($item) }
            else { $files.Add($item) }
            if (($files.Count + $stack.Count) -gt $script:MaxEntries) { throw 'The avatar contains too many files or folders.' }
        }
    }
    return ,$files
}

function Test-PortablePluginFile([string]$Relative, [string]$PluginName) {
    if ($Relative -ieq 'ConvaiAvatarStudio.json' -or $Relative -ieq 'ConvaiAvatarPendingInstall.json' -or $Relative -ieq 'ConvaiAvatarCloudBinding.json') { return $false } # Local ownership, pending installs, and cloud-binding recovery IDs are not portable.
    if ($Relative -ieq ($PluginName + '.uplugin')) { return $true }
    if ($Relative -match '^Content/' -and $Relative -match '(?i)\.(uasset|umap|uexp|ubulk|uptnl)$') { return $true }
    if ($Relative -match '^Resources/' -and $Relative -match '(?i)\.(png|jpg|jpeg|txt|md)$') { return $true }
    if ($Relative -notmatch '/' -and $Relative -match '(?i)\.(txt|md)$') { return $true }
    throw 'The selected avatar plugin contains source code, binaries, configuration, or unsupported files. Only a content-only avatar plugin can be imported.'
}

function Read-Descriptor([string]$Text) {
    $descriptor = $Text | ConvertFrom-Json
    if ($descriptor.CanContainContent -ne $true -or @($descriptor.Modules).Where({ $null -ne $_ }).Count -gt 0 -or
        $null -ne $descriptor.PreBuildSteps -or $null -ne $descriptor.PostBuildSteps) {
        throw 'The selected avatar is not a content-only plugin. Install code dependencies separately.'
    }
    foreach ($plugin in @($descriptor.Plugins)) { if ($null -ne $plugin) { Assert-PluginName ([string]$plugin.Name) } }
    return $descriptor
}

function Get-RequiredPlugins($Descriptor, $Project, $Additional, [string]$AvatarName) {
    $names = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($owner in @($Descriptor, $Project)) {
        foreach ($plugin in @($owner.Plugins)) {
            if ($null -ne $plugin -and $plugin.Enabled -eq $true -and $plugin.Name -ine $AvatarName) {
                Assert-PluginName ([string]$plugin.Name)
                [void]$names.Add([string]$plugin.Name)
            }
        }
    }
    foreach ($name in @($Additional)) {
        if (-not [string]::IsNullOrEmpty([string]$name) -and $name -ine $AvatarName) { Assert-PluginName ([string]$name); [void]$names.Add([string]$name) }
    }
    return @($names | Sort-Object)
}

function Assert-EntryPoint([string]$EntryPoint, [string]$PluginName, $AvailableRelativeFiles) {
    $mount = '/' + $PluginName + '/'
    if (-not $EntryPoint.StartsWith($mount, [System.StringComparison]::Ordinal)) { throw 'The avatar Blueprint path does not belong to the selected plugin.' }
    $relative = $EntryPoint.Substring($mount.Length).Split('.')[0]
    [void](Get-SafeEntryPath $relative)
    $expected = 'Content/' + $relative + '.uasset'
    if (-not $AvailableRelativeFiles.Contains($expected)) { throw 'The source archive does not contain the selected avatar Blueprint.' }
}

function Add-ZipBytes($Zip, [string]$Name, [byte[]]$Bytes) {
    $entry = $Zip.CreateEntry($Name, [System.IO.Compression.CompressionLevel]::Optimal)
    $stream = $entry.Open()
    try { $stream.Write($Bytes, 0, $Bytes.Length) } finally { $stream.Dispose() }
}

function Add-ZipText($Zip, [string]$Name, [string]$Text) { Add-ZipBytes $Zip $Name ($script:Utf8.GetBytes($Text)) }

function Copy-StreamChecked($InputStream, $OutputStream, [long]$ExpectedLength, [string]$ExpectedMd5 = '') {
    $buffer = New-Object byte[] 1048576
    [long]$copied = 0
    $md5 = $null
    if ($ExpectedMd5) { $md5 = [System.Security.Cryptography.MD5]::Create() }
    try {
        while (($read = $InputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            if ($read -gt ($ExpectedLength - $copied)) { throw 'A source file expands beyond its declared archive size.' }
            if ($null -ne $md5) { [void]$md5.TransformBlock($buffer, 0, $read, $buffer, 0) }
            $OutputStream.Write($buffer, 0, $read)
            $copied += $read
        }
        if ($copied -ne $ExpectedLength) { throw 'A source file does not match its declared archive size.' }
        if ($null -ne $md5) {
            [void]$md5.TransformFinalBlock([byte[]]@(), 0, 0)
            if ([BitConverter]::ToString($md5.Hash).Replace('-','') -ine $ExpectedMd5) {
                throw 'Shared avatar support changed after packaging. Package this avatar again before uploading its source.'
            }
        }
    } finally { if ($null -ne $md5) { $md5.Dispose() } }
}

function Get-Md5([byte[]]$Bytes) {
    $md5 = [System.Security.Cryptography.MD5]::Create()
    try { return [BitConverter]::ToString($md5.ComputeHash($Bytes)).Replace('-','').ToLowerInvariant() }
    finally { $md5.Dispose() }
}

function Read-BaseContext($Request, [string]$ProjectDirectory, [string]$PluginName) {
    $path = [System.IO.Path]::Combine($ProjectDirectory, 'ConvaiAvatarBaseContent.json')
    $expected = [string]$Request.expected_base_manifest_md5
    if (-not [System.IO.File]::Exists($path) -and [string]::IsNullOrEmpty($expected)) { return $null } # Legacy/standalone source fixture.
    if ($expected -notmatch '^[A-Fa-f0-9]{32}$' -or -not [System.IO.File]::Exists($path)) {
        throw 'The completed packaging context is missing. Package this avatar again before uploading its source.'
    }
    Assert-NoReparseAncestors $path
    if ((New-Object System.IO.FileInfo($path)).Length -gt 16777216) { throw 'The shared support manifest exceeds its size limit.' }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ((Get-Md5 $bytes) -ine $expected) { throw 'The uploader context changed after packaging. Package this avatar again before uploading its source.' }
    $manifest = $script:Utf8.GetString($bytes) | ConvertFrom-Json
    if ($manifest.schema_version -ne 1 -or $manifest.complete -ne $true -or $manifest.selected_plugin -cne $PluginName -or
        $null -eq $manifest.files -or $manifest.files -isnot [System.Management.Automation.PSCustomObject]) {
        throw 'The shared support snapshot is incomplete or belongs to a different avatar. Package this avatar again.'
    }
    $files = New-Object 'System.Collections.Generic.List[object]'
    $relativeFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $portableFiles = @{}
    foreach ($property in $manifest.files.PSObject.Properties) {
        $relative = Get-SafeEntryPath ([string]$property.Name)
        if ($relative -notmatch '(?i)\.(uasset|uexp|ubulk|uptnl)$' -or -not $relativeFiles.Add($relative) -or
            [string]$property.Value -notmatch '^[A-Fa-f0-9]{32}$') { throw 'The shared support manifest contains an invalid file record.' }
        if ($files.Count -ge $script:MaxEntries) { throw 'The shared support snapshot contains too many files.' }
        $file = [System.IO.Path]::Combine($ProjectDirectory, 'Content', $relative)
        Assert-NoReparseAncestors $file
        if (-not [System.IO.File]::Exists($file)) { throw 'A shared support snapshot file is missing. Package this avatar again.' }
        $length = (New-Object System.IO.FileInfo($file)).Length
        if ($length -gt $script:MaxEntryBytes) { throw 'A shared support file exceeds the archive size limit.' }
        $files.Add(@{ Path=$file; Relative=$relative; Length=$length; Md5=[string]$property.Value })
        $portableFiles[$relative] = [string]$property.Value
    }
    foreach ($relative in @($portableFiles.Keys)) {
        if ([System.IO.Path]::GetExtension($relative) -ine '.uasset' -and -not $portableFiles.ContainsKey([System.IO.Path]::ChangeExtension($relative, '.uasset'))) {
            throw 'A shared support sidecar is missing its owning asset package.'
        }
    }
    return @{ Path=$path; ExpectedMd5=$expected; Files=$files; ProfileJson=[string]$manifest.project_profile_json; Configuration=[string]$manifest.project_configuration; Portable=@{schema_version=1; selected_plugin=$PluginName; complete=$true; files=$portableFiles} }
}

function Get-PortableProfileConfig($Context, [string]$PluginName) {
    if ($null -eq $Context -or -not $Context.ProfileJson) { return $null } # Legacy source context.
    if ($Context.ProfileJson.Length -gt 65536) { throw 'The captured project profile is too large.' }
    $profile = $Context.ProfileJson | ConvertFrom-Json
    if ($profile.schema_version -ne 1 -or @($profile.settings).Count -lt 1 -or @($profile.settings).Count -gt 43) { throw 'The captured project profile is invalid.' }
    if ($Context.Configuration -notin @('Shipping','Development','Test','Debug','DebugGame')) { throw 'The captured runtime configuration is invalid.' }
    $rendererKeys = @('r.GenerateMeshDistanceFields','r.DynamicGlobalIlluminationMethod','r.ReflectionMethod','r.RayTracing','r.Shadow.Virtual.Enable','r.GPUSkin.Support16BitBoneIndex','r.GPUSkin.UnlimitedBoneInfluences','SkeletalMesh.UseExperimentalChunking','r.VirtualTextures','r.SkinCache.CompileShaders','r.SkinCache.BlendUsingVertexColorForRecomputeTangents','r.SkinCache.SceneMemoryLimitInMB','r.SkinCache.DefaultBehavior','r.HairStrands.SkyLighting','r.HairStrands.SkyAO','r.HairStrands.Visibility.MaterialPass','r.HairStrands.Visibility.FullCoverageThreshold','r.HairStrands.Visibility.MSAA.MeanSamplePerPixel','r.HairStrands.RasterizationScale','r.HairStrands.Voxelization.DensityScale','r.HairStrands.Voxelization.DepthBiasScale_Shadow','r.HairStrands.Voxelization.DepthBiasScale_Light','r.HairStrands.Voxelization.DepthBiasScale_Environment','r.HairStrands.Voxelization.GPUDriven','r.HairStrands.Voxelization.Virtual.VoxelWorldSize','r.HairStrands.Voxelization.Virtual.VoxelPageCountPerDim','r.HairStrands.Voxelization.Raymarching.SteppingScale','r.HairStrands.Voxelization.Raymarching.SteppingScale.Shadow','r.HairStrands.DeepShadow.RandomType','r.HairStrands.ComposeAfterTranslucency','r.HairStrands.AsyncLoad','r.HairStrands.BindingAsyncLoad','r.HairStrands.SimulationRestUpdate','r.HairStrands.MaxSimulatedLOD','r.HairStrands.LODMode','r.ReflectionCaptureResolution','r.AllowStaticLighting')
    $packaging = @{bUseIoStore='False';bGenerateChunks='True';bShareMaterialShaderCode='False';UsePakFile='True'}
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $engine = ''; $lastSection = ''
    foreach ($setting in @($profile.settings)) {
        $file=[string]$setting.file; $section=[string]$setting.section; $key=[string]$setting.key; $values=@($setting.values)
        if (-not $seen.Add($file+'|'+$section+'|'+$key)) { throw 'The captured profile contains a duplicate key.' }
        if ($file -ceq 'DefaultEngine.ini' -and $section -ceq '/Script/Engine.RendererSettings' -and $rendererKeys -ccontains $key) {
            if ($values.Count -ne 1 -or $values[0] -isnot [string] -or -not $values[0].StartsWith($key+'=',[StringComparison]::Ordinal)) { throw 'The captured renderer profile contains an invalid value.' }
            $value=$values[0].Substring($key.Length+1)
            if ($value -cnotin @('True','False') -and ($value -notmatch '^(?:\d+(?:\.\d*)?|\.\d+)$' -or [double]::Parse($value,[Globalization.CultureInfo]::InvariantCulture) -gt 65536)) { throw 'The captured renderer profile contains an unsafe value.' }
        } elseif ($file -ceq 'DefaultEngine.ini' -and $section -ceq '/Script/WindowsTargetPlatform.WindowsTargetSettings') {
            if ($key -ceq 'DefaultGraphicsRHI') { if ($values.Count -ne 1 -or $values[0] -cne 'DefaultGraphicsRHI=DefaultGraphicsRHI_DX12') { throw 'The captured RHI value is invalid.' } }
            elseif ($key -ceq 'D3D12TargetedShaderFormats') { if ($values.Count -ne 2 -or $values[0] -cne '-D3D12TargetedShaderFormats=PCD3D_SM5' -or $values[1] -cne '+D3D12TargetedShaderFormats=PCD3D_SM6') { throw 'The captured shader formats are invalid.' } }
            else { throw 'The captured profile contains an unsupported RHI key.' }
        } elseif ($file -ceq 'DefaultGame.ini' -and $section -ceq '/Script/UnrealEd.ProjectPackagingSettings' -and $packaging.ContainsKey($key)) {
            if ($values.Count -ne 1 -or $values[0] -cne ($key+'='+$packaging[$key])) { throw 'The captured profile changes a required pak setting.' }
            continue # These fixed V1 pak values are emitted below.
        } else { throw 'The captured profile contains an unsupported configuration section or key.' }
        if ($lastSection -cne $section) { $engine += '['+$section+"]`r`n"; $lastSection=$section }
        $engine += ($values -join "`r`n")+"`r`n"
    }
    $game="[/Script/UnrealEd.ProjectPackagingSettings]`r`nUsePakFile=True`r`nbUseIoStore=False`r`nbGenerateChunks=True`r`nbShareMaterialShaderCode=False`r`nbCookAll=False`r`nBuildConfiguration=PPBC_$($Context.Configuration)`r`n+DirectoriesToAlwaysCook=(Path=`"/$PluginName`")`r`nbCompressed=True`r`n"
    if ($profile.compression.format -ceq 'Zlib') { $game += "PackageCompressionFormat=Zlib`r`n" }
    elseif ($profile.compression.format -ceq 'Oodle' -and $profile.compression.method -cin @('Kraken','Mermaid','Selkie','Leviathan') -and
        [string]$profile.compression.development_level -match '^[1-9]$' -and [string]$profile.compression.shipping_level -match '^[1-9]$') {
        $game += "PackageCompressionFormat=Oodle`r`nPackageCompressionMethod=$($profile.compression.method)`r`nPackageCompressionLevel_DebugDevelopment=$($profile.compression.development_level)`r`nPackageCompressionLevel_TestShipping=$($profile.compression.shipping_level)`r`n"
    } else { throw 'The captured profile compression settings are invalid.' }
    return @{Engine=$engine;Game=$game}
}

function Read-ZipJson($Entry, [long]$MaxBytes = 1048576) {
    if ($Entry.Length -gt $MaxBytes) { throw 'An archive descriptor is too large.' }
    $reader = $Entry.Open()
    $buffer = New-Object System.IO.MemoryStream
    try {
        Copy-StreamChecked $reader $buffer $Entry.Length
        return ($script:Utf8.GetString($buffer.ToArray()) | ConvertFrom-Json)
    } finally { $reader.Dispose(); $buffer.Dispose() }
}

function New-SourceArchive($Request) {
    $name = [string]$Request.plugin_name
    Assert-PluginName $name
    $source = [System.IO.Path]::GetFullPath([string]$Request.plugin_directory).TrimEnd('\','/')
    $output = [System.IO.Path]::GetFullPath([string]$Request.output_zip)
    Assert-NoReparseAncestors $source
    Assert-NoReparseAncestors $output
    if ([System.IO.File]::Exists($output) -or [System.IO.Directory]::Exists($output)) { throw 'A source archive already exists at this destination. Choose a new output filename.' }
    $descriptorPath = [System.IO.Path]::Combine($source, $name + '.uplugin')
    if (-not [System.IO.File]::Exists($descriptorPath)) { throw 'The avatar plugin descriptor is missing.' }
    $descriptor = Read-Descriptor ([System.IO.File]::ReadAllText($descriptorPath, $script:Utf8))
    $projectPath = [System.IO.Path]::GetFullPath([string]$Request.proxy_uproject)
    Assert-NoReparseAncestors $projectPath
    $project = [System.IO.File]::ReadAllText($projectPath, $script:Utf8) | ConvertFrom-Json
    $projectName = [System.IO.Path]::GetFileNameWithoutExtension($projectPath)
    # The local shared project can use a short name while the source wrapper
    # retains the persisted runtime/project_name identity of existing uploads.
    if ($Request.PSObject.Properties['portable_project_name']) { $projectName = [string]$Request.portable_project_name }
    Assert-PluginName $projectName
    $required = @(Get-RequiredPlugins $descriptor $null $Request.required_plugins $name)
    $portablePlugins = @(@{ Name=$name; Enabled=$true })
    foreach ($dependency in $required) { $portablePlugins += @{ Name=$dependency; Enabled=$true } }
    $portableProject = @{ FileVersion=3; EngineAssociation=[string]$project.EngineAssociation; Description='Avatar Studio editable source'; Plugins=$portablePlugins }
    $baseContext = Read-BaseContext $Request ([System.IO.Path]::GetDirectoryName($projectPath)) $name
    $files = Get-PluginFiles $source
    $relativeFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $selected = New-Object 'System.Collections.Generic.List[object]'
    [long]$total = 0
    foreach ($file in $files) {
        $relative = Get-SafeEntryPath ($file.Substring($source.Length + 1))
        if (-not (Test-PortablePluginFile $relative $name)) { continue }
        if (-not $relativeFiles.Add($relative)) { throw 'The avatar contains duplicate filenames.' }
        $length = (New-Object System.IO.FileInfo($file)).Length
        if ($length -gt $script:MaxEntryBytes -or $length -gt ($script:MaxExpandedBytes - $total)) { throw 'The avatar source exceeds the archive size limit.' }
        $total += $length
        $selected.Add(@{ Path=$file; Relative=$relative; Length=$length })
    }
    Assert-EntryPoint ([string]$Request.entry_point) $name $relativeFiles
    $sourceMap = $null
    if ($Request.PSObject.Properties['source_to_destination_packages']) {
        $sourceMap = Read-SourceCopyMap $Request.source_to_destination_packages $name $relativeFiles
    }
    if ($null -ne $baseContext) {
        if (($selected.Count + $baseContext.Files.Count + 6) -gt $script:MaxEntries) { throw 'The avatar source contains too many archive entries.' }
        foreach ($item in $baseContext.Files) {
            if ($item.Length -gt ($script:MaxExpandedBytes - $total)) { throw 'The avatar source exceeds the archive size limit.' }
            $total += $item.Length
        }
    }
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($output))
    $temporary = $output + '.' + [guid]::NewGuid().ToString('N') + '.part'
    $fileStream = $null
    $zip = $null
    try {
        $fileStream = [System.IO.File]::Open($temporary, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        $zip = New-Object System.IO.Compression.ZipArchive($fileStream, [System.IO.Compression.ZipArchiveMode]::Create, $true)
        Add-ZipText $zip ($projectName + '/' + $projectName + '.uproject') ($portableProject | ConvertTo-Json -Depth 30)
        $manifest = @{ schema_version=1; plugin_name=$name; entry_point=[string]$Request.entry_point; engine_association=[string]$project.EngineAssociation; required_plugins=$required; source_package_count=$selected.Count }
        if ($null -ne $sourceMap) { $manifest.source_to_destination_packages = $sourceMap }
        if ($Request.PSObject.Properties['is_metahuman']) {
            if ($Request.is_metahuman -isnot [bool]) { throw 'The selected MetaHuman choice must be a Boolean.' }
            $manifest.is_metahuman=$Request.is_metahuman
        }
        if ($Request.PSObject.Properties['include_convai_content']) {
            if ($Request.include_convai_content -isnot [bool]) { throw 'The selected Convai content choice must be a Boolean.' }
            $manifest.include_convai_content=$Request.include_convai_content
        }
        if ($Request.PSObject.Properties['acknowledged_missing_packages']) {
            $manifest.acknowledged_missing_packages=Read-AcknowledgedMissingPackages $Request.acknowledged_missing_packages $name
        }
        if ($null -ne $baseContext) { $manifest.base_context_md5=$baseContext.ExpectedMd5; $manifest.base_content_file_count=$baseContext.Files.Count }
        Add-ZipText $zip ($projectName + '/AvatarStudioSource.json') ($manifest | ConvertTo-Json -Depth 30)
        $gameIni = "[/Script/UnrealEd.ProjectPackagingSettings]`r`nUsePakFile=True`r`nbUseIoStore=False`r`nbGenerateChunks=True`r`nbCookAll=False`r`n+DirectoriesToAlwaysCook=(Path=`"/$name`")`r`n"
        # Copy only renderer keys relevant to MetaHuman assets, never account/configuration secrets.
        $engineIni = "[/Script/Engine.RendererSettings]`r`nr.SkinCache.CompileShaders=True`r`n"
        $originalEngineIni = [System.IO.Path]::Combine([System.IO.Path]::GetDirectoryName($projectPath), 'Config', 'DefaultEngine.ini')
        if ([System.IO.File]::Exists($originalEngineIni)) {
            $inRenderer = $false
            foreach ($line in [System.IO.File]::ReadAllLines($originalEngineIni)) {
                if ($line -match '^\s*\[(.+)\]') { $inRenderer = $Matches[1] -eq '/Script/Engine.RendererSettings'; continue }
                if ($inRenderer -and $line -match '^\s*(r\.(?:SkinCache\.[A-Za-z0-9_]+|GPUSkin\.[A-Za-z0-9_]+|Support16BitBoneIndex))\s*=\s*(True|False|[0-9]+)\s*$') { $engineIni += $Matches[1] + '=' + $Matches[2] + "`r`n" }
            }
        }
        $profileConfig = Get-PortableProfileConfig $baseContext $name
        if ($null -ne $profileConfig) { $gameIni=$profileConfig.Game; $engineIni=$profileConfig.Engine }
        Add-ZipText $zip ($projectName + '/Config/DefaultGame.ini') $gameIni
        Add-ZipText $zip ($projectName + '/Config/DefaultEngine.ini') $engineIni
        foreach ($item in $selected) {
            $entry = $zip.CreateEntry($projectName + '/Plugins/ConvaiAvatars/' + $name + '/' + $item.Relative, [System.IO.Compression.CompressionLevel]::Optimal)
            $destination = $entry.Open()
            $input = [System.IO.File]::Open($item.Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
            try { Copy-StreamChecked $input $destination $item.Length } finally { $input.Dispose(); $destination.Dispose() }
        }
        if ($null -ne $baseContext) {
            Add-ZipText $zip ($projectName + '/ConvaiAvatarBaseContent.json') ($baseContext.Portable | ConvertTo-Json -Depth 10)
            foreach ($item in $baseContext.Files) {
                Assert-NoReparseAncestors $item.Path
                $entry = $zip.CreateEntry($projectName + '/Content/' + $item.Relative, [System.IO.Compression.CompressionLevel]::Optimal)
                $destination = $entry.Open()
                $input = $null
                try {
                    # Deny writes/deletes while streaming and hash the actual archived bytes against the completed cook.
                    $input = [System.IO.File]::Open($item.Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
                    Copy-StreamChecked $input $destination $item.Length $item.Md5
                } finally { if ($null -ne $input) { $input.Dispose() }; $destination.Dispose() }
            }
            Assert-NoReparseAncestors $baseContext.Path
            if ((New-Object System.IO.FileInfo($baseContext.Path)).Length -gt 16777216 -or
                (Get-Md5 ([System.IO.File]::ReadAllBytes($baseContext.Path))) -ine $baseContext.ExpectedMd5) {
                throw 'The uploader context changed while archiving. Package this avatar again before uploading its source.'
            }
        }
        $zip.Dispose(); $zip = $null
        $fileStream.Dispose(); $fileStream = $null
        if ((New-Object System.IO.FileInfo($temporary)).Length -gt $script:MaxZipBytes) { throw 'The source ZIP exceeds the Assets upload limit of 10,000 MiB.' }
        [System.IO.File]::Move($temporary, $output)
        return @{ ok=$true; output_zip=$output; required_plugins=$required; expanded_bytes=$total }
    } finally {
        if ($null -ne $zip) { $zip.Dispose() }
        if ($null -ne $fileStream) { $fileStream.Dispose() }
        if ([System.IO.File]::Exists($temporary)) { [System.IO.File]::Delete($temporary) }
    }
}

function Install-SourceArchive($Request) {
    $name = [string]$Request.plugin_name
    Assert-PluginName $name
    $inputPath = [System.IO.Path]::GetFullPath([string]$Request.zip)
    Assert-NoReparseAncestors $inputPath
    if (-not [System.IO.File]::Exists($inputPath) -or (New-Object System.IO.FileInfo($inputPath)).Length -gt $script:MaxZipBytes) { throw 'The source ZIP is missing or exceeds the supported download size.' }
    $destinationRoot = [System.IO.Path]::GetFullPath([string]$Request.destination_root).TrimEnd('\','/')
    Assert-NoReparseAncestors $destinationRoot
    $target = [System.IO.Path]::Combine($destinationRoot, $name)
    if ([System.IO.Directory]::Exists($target) -or [System.IO.File]::Exists($target)) { throw 'This avatar plugin already exists locally. Open the existing avatar or use a separate project to import a fresh copy.' }
    $zip = [System.IO.Compression.ZipFile]::OpenRead($inputPath)
    $staging = $null
    $supportStaging = $null
    $supportTarget = [System.IO.Path]::Combine($destinationRoot, 'BaseContent')
    try {
        if ($zip.Entries.Count -gt $script:MaxEntries) { throw 'The source archive contains too many entries.' }
        $entries = @{}
        $fileNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
        $directoryNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
        [long]$total = 0
        foreach ($entry in $zip.Entries) {
            $path = Get-SafeEntryPath $entry.FullName
            if ($entries.ContainsKey($path)) { throw 'The source archive contains duplicate filenames, including case-only duplicates.' }
            # Unix file type is stored in the high word; low word includes Windows reparse attributes.
            $attributes = [long]$entry.ExternalAttributes -band 4294967295
            $fileType = ($attributes -shr 16) -band 61440
            if (($attributes -band 1024) -ne 0 -or ($fileType -ne 0 -and $fileType -ne 32768 -and $fileType -ne 16384)) { throw 'The source archive contains a symbolic link, reparse point, or special device entry.' }
            if ($entry.Length -gt $script:MaxEntryBytes -or $entry.Length -gt ($script:MaxExpandedBytes - $total)) { throw 'The expanded source archive exceeds the supported size.' }
            $total += $entry.Length
            $directory = $entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')
            if ($directory) { [void]$directoryNames.Add($path) } else { [void]$fileNames.Add($path) }
            $entries[$path] = @{ Entry=$entry; Directory=$directory }
        }
        # File/folder collisions are checked across the entire archive before any extraction.
        foreach ($path in $entries.Keys) {
            $parent = $path
            while ($parent.Contains('/')) {
                $parent = $parent.Substring(0, $parent.LastIndexOf('/'))
                if ($fileNames.Contains($parent)) { throw 'The source archive contains a file/folder path collision.' }
            }
        }
        $descriptors = @($entries.Keys | Where-Object { -not $entries[$_].Directory -and ($_ -ieq ($name + '.uplugin') -or $_.EndsWith('/' + $name + '.uplugin', [System.StringComparison]::OrdinalIgnoreCase)) })
        if ($descriptors.Count -ne 1) { throw 'The source archive must contain exactly one descriptor for the selected avatar plugin.' }
        $descriptorEntry = [string]$descriptors[0]
        $prefix = $descriptorEntry.Substring(0, $descriptorEntry.Length - ($name + '.uplugin').Length)
        if ($prefix.Length -gt 0 -and $prefix.TrimEnd('/').Split('/')[-1] -ine $name) { throw 'The selected plugin descriptor is in a folder with a different name.' }
        $descriptorObject = Read-ZipJson $entries[$descriptorEntry].Entry
        $descriptor = Read-Descriptor ($descriptorObject | ConvertTo-Json -Depth 60)
        $selected = New-Object 'System.Collections.Generic.List[object]'
        $relativeFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
        [long]$selectedBytes = 0
        foreach ($path in $entries.Keys) {
            if (-not $path.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) -or $entries[$path].Directory) { continue }
            $relative = $path.Substring($prefix.Length)
            if (-not (Test-PortablePluginFile $relative $name)) { continue }
            [void]$relativeFiles.Add($relative)
            $selected.Add(@{ Entry=$entries[$path].Entry; Relative=$relative })
            $selectedBytes += $entries[$path].Entry.Length
        }
        Assert-EntryPoint ([string]$Request.entry_point) $name $relativeFiles
        $containingProject = $null
        $projectCandidates = @($entries.Keys | Where-Object { $_ -match '(?i)\.uproject$' -and -not $entries[$_].Directory } | Where-Object {
            $slash = $_.LastIndexOf('/')
            $projectPrefix = if ($slash -ge 0) { $_.Substring(0, $slash + 1) } else { '' }
            $prefix.StartsWith($projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)
        } | Sort-Object Length -Descending)
        if ($projectCandidates.Count -gt 0) { $containingProject = Read-ZipJson $entries[$projectCandidates[0]].Entry }
        if ([string]$containingProject.EngineAssociation -match '^(\d+)\.(\d+)(?:\.\d+)?$' -and $Request.engine_major) {
            if ([int]$Matches[1] -gt [int]$Request.engine_major -or ([int]$Matches[1] -eq [int]$Request.engine_major -and [int]$Matches[2] -gt [int]$Request.engine_minor)) {
                throw 'This avatar source was saved in a newer Unreal Engine. Open it in that engine version or newer.'
            }
        }
        # A legacy wrapper's .uproject can enable unrelated uploader/editor utilities. Do not
        # turn those into avatar requirements. Use the selected descriptor and portable manifest.
        $manifestRequired = @()
        $metaHumanChoice = $null
        $includeConvaiContent = $null
        $sourceMap = $null
        $hasAcknowledgedMissingPackages = $false
        $acknowledgedMissingPackages = $null
        $support = New-Object 'System.Collections.Generic.List[object]'
        $supportFiles = @{}
        if ($projectCandidates.Count -gt 0) {
            $projectEntry = [string]$projectCandidates[0]
            $lastSlash = $projectEntry.LastIndexOf('/')
            $projectPrefix = if ($lastSlash -ge 0) { $projectEntry.Substring(0, $lastSlash + 1) } else { '' }
            $manifestPath = $projectPrefix + 'AvatarStudioSource.json'
            if ($entries.ContainsKey($manifestPath)) {
                $manifest = Read-ZipJson $entries[$manifestPath].Entry
                if ($manifest.plugin_name -ine $name) { throw 'The portable source manifest describes a different avatar plugin.' }
                if ($manifest.PSObject.Properties['is_metahuman']) {
                    if ($manifest.is_metahuman -isnot [bool]) { throw 'The portable MetaHuman choice must be a Boolean.' }
                    $metaHumanChoice=$manifest.is_metahuman
                }
                if ($manifest.PSObject.Properties['include_convai_content']) {
                    if ($manifest.include_convai_content -isnot [bool]) { throw 'The portable Convai content choice must be a Boolean.' }
                    $includeConvaiContent=$manifest.include_convai_content
                }
                if ($manifest.PSObject.Properties['source_to_destination_packages']) {
                    $sourceMap = Read-SourceCopyMap $manifest.source_to_destination_packages $name $relativeFiles
                }
                if ($manifest.PSObject.Properties['acknowledged_missing_packages']) {
                    $acknowledgedMissingPackages=Read-AcknowledgedMissingPackages $manifest.acknowledged_missing_packages $name
                    $hasAcknowledgedMissingPackages=$true
                }
                $manifestRequired = @($manifest.required_plugins)
            }
            $supportManifestPath = $projectPrefix + 'ConvaiAvatarBaseContent.json'
            if ($Request.stage_support -eq $true -and $entries.ContainsKey($supportManifestPath)) {
                $supportManifest = Read-ZipJson $entries[$supportManifestPath].Entry 16777216
                if ($supportManifest.schema_version -ne 1 -or $supportManifest.complete -ne $true -or
                    $supportManifest.selected_plugin -cne $name -or $null -eq $supportManifest.files -or
                    $supportManifest.files -isnot [System.Management.Automation.PSCustomObject]) {
                    throw 'The downloaded shared support manifest is incomplete or describes a different avatar.'
                }
                foreach ($property in $supportManifest.files.PSObject.Properties) {
                    $relative = Get-SafeEntryPath ([string]$property.Name)
                    if ($relative -notmatch '(?i)\.(uasset|uexp|ubulk|uptnl)$' -or
                        [string]$property.Value -notmatch '^[A-Fa-f0-9]{32}$' -or $supportFiles.ContainsKey($relative)) {
                        throw 'The downloaded support manifest contains an invalid or duplicate file record.'
                    }
                    $entryPath = $projectPrefix + 'Content/' + $relative
                    if (-not $entries.ContainsKey($entryPath) -or $entries[$entryPath].Directory) {
                        throw 'A required shared support file is missing from the downloaded source.'
                    }
                    $supportFiles[$relative] = [string]$property.Value
                    $support.Add(@{ Entry=$entries[$entryPath].Entry; Relative=$relative; Md5=[string]$property.Value })
                    $selectedBytes += $entries[$entryPath].Entry.Length
                }
                foreach ($relative in @($supportFiles.Keys)) {
                    if ([System.IO.Path]::GetExtension($relative) -ine '.uasset' -and -not $supportFiles.ContainsKey([System.IO.Path]::ChangeExtension($relative, '.uasset'))) {
                        throw 'A downloaded shared support sidecar is missing its owning asset package.'
                    }
                }
            }
        }
        $required = @(Get-RequiredPlugins $descriptor $null $manifestRequired $name)
        $rootDrive = New-Object System.IO.DriveInfo([System.IO.Path]::GetPathRoot($destinationRoot))
        if ($rootDrive.AvailableFreeSpace -lt ($selectedBytes + 1073741824)) { throw 'There is not enough free disk space to extract this avatar safely.' }
        [void][System.IO.Directory]::CreateDirectory($destinationRoot)
        Assert-NoReparseAncestors $destinationRoot
        $staging = [System.IO.Path]::Combine($destinationRoot, '.' + $name + '-import-' + [guid]::NewGuid().ToString('N'))
        [void][System.IO.Directory]::CreateDirectory($staging)
        foreach ($item in $selected) {
            $output = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($staging, $item.Relative.Replace('/', '\')))
            if (-not $output.StartsWith($staging + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'An extracted path would leave the avatar staging folder.' }
            [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($output))
            Assert-NoReparseAncestors $output
            $reader = $item.Entry.Open()
            $writer = [System.IO.File]::Open($output, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
            try { Copy-StreamChecked $reader $writer $item.Entry.Length } finally { $reader.Dispose(); $writer.Dispose() }
            if ((New-Object System.IO.FileInfo($output)).Length -ne $item.Entry.Length) { throw 'An extracted file has a different size than its archive entry.' }
        }
        if ($support.Count -gt 0) {
            Assert-NoReparseAncestors $supportTarget
            if ([System.IO.Directory]::Exists($supportTarget) -or [System.IO.File]::Exists($supportTarget)) { throw 'Shared support is already staged for this download. Resume the pending download or create a fresh download job.' }
            $supportStaging = [System.IO.Path]::Combine($destinationRoot, '.base-import-' + [guid]::NewGuid().ToString('N'))
            [void][System.IO.Directory]::CreateDirectory($supportStaging)
            foreach ($item in $support) {
                $output = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($supportStaging, $item.Relative.Replace('/', '\')))
                if (-not $output.StartsWith($supportStaging + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'An extracted support path would leave its staging folder.' }
                [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($output))
                Assert-NoReparseAncestors $output
                $reader = $item.Entry.Open()
                $writer = $null
                try {
                    $writer = [System.IO.File]::Open($output, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
                    Copy-StreamChecked $reader $writer $item.Entry.Length $item.Md5
                } finally { $reader.Dispose(); if ($null -ne $writer) { $writer.Dispose() } }
            }
            [System.IO.Directory]::Move($supportStaging, $supportTarget)
            $supportStaging = $null
        }
        Assert-NoReparseAncestors $destinationRoot
        [System.IO.Directory]::Move($staging, $target) # Refuses any existing destination, including races.
        $staging = $null
        $result=@{ ok=$true; installed_directory=$target; required_plugins=$required; support_files=$supportFiles; engine_association=[string]$containingProject.EngineAssociation }
        if ($null -ne $metaHumanChoice) { $result.is_metahuman=$metaHumanChoice }
        if ($null -ne $includeConvaiContent) { $result.include_convai_content=$includeConvaiContent }
        if ($null -ne $sourceMap) { $result.source_to_destination_packages=$sourceMap }
        if ($hasAcknowledgedMissingPackages) { $result.acknowledged_missing_packages=$acknowledgedMissingPackages }
        return $result
    } finally {
        $zip.Dispose()
        if ($null -ne $staging -and [System.IO.Directory]::Exists($staging)) {
            $fullStaging = [System.IO.Path]::GetFullPath($staging)
            if ($fullStaging.StartsWith($destinationRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
                Assert-NoReparseAncestors $fullStaging
                [System.IO.Directory]::Delete($fullStaging, $true)
            }
        }
        if ($null -ne $supportStaging -and [System.IO.Directory]::Exists($supportStaging)) {
            $fullStaging = [System.IO.Path]::GetFullPath($supportStaging)
            if ($fullStaging.StartsWith($destinationRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
                Assert-NoReparseAncestors $fullStaging
                [System.IO.Directory]::Delete($fullStaging, $true)
            }
        }
    }
}

try {
    $request = [System.IO.File]::ReadAllText([System.IO.Path]::GetFullPath($RequestPath), $script:Utf8) | ConvertFrom-Json
    $result = switch ([string]$request.operation) {
        'create' { New-SourceArchive $request }
        'import' { Install-SourceArchive $request }
        default { throw 'Unknown Avatar Studio archive operation.' }
    }
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($ResultPath), ($result | ConvertTo-Json -Depth 30), $script:Utf8)
    exit 0
} catch {
    $result = @{ ok=$false; error=$_.Exception.Message }
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($ResultPath), ($result | ConvertTo-Json -Depth 10), $script:Utf8)
    exit 1
}
