# Copyright Convai Inc. All Rights Reserved.
# Builds the resolved HTTP Editor module in the single managed uploader project.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$TransportDirectory,
    [Parameter(Mandatory=$true)][string]$EngineDirectory,
    [Parameter(Mandatory=$true)][string]$ResultFile,
    [Parameter(Mandatory=$true)][string]$ResolutionFile,
    [switch]$PrepareOnly,
    # Internal contract: parent holds packaging.lock through bootstrap AND transfer.
    # Not a security boundary; standalone callers acquire that same lock here.
    [switch]$WorkspaceLockHeld
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
$utf8 = New-Object Text.UTF8Encoding($false)
function Assert-Local([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.StartsWith('\\') -or $full.Contains('"')) { throw 'The uploader requires an ordinary local directory.' }
    $part = $full
    while ($part) {
        if ([IO.File]::Exists($part) -or [IO.Directory]::Exists($part)) {
            if (([IO.File]::GetAttributes($part) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'An uploader cache path is redirected.' }
        }
        $parent = [IO.Path]::GetDirectoryName($part)
        if ($parent -eq $part) { break }; $part = $parent
    }
    return $full
}
function Below([string]$Child, [string]$Parent) {
    return [IO.Path]::GetFullPath($Child).StartsWith([IO.Path]::GetFullPath($Parent).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}
function Write-Text([string]$Text, [string]$Path) {
    [void](Assert-Local $Path)
    $temporary = $Path + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($temporary, $Text, $utf8)
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $Path) }
    } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}
function Write-Json($Value, [string]$Path) { Write-Text (ConvertTo-Json $Value -Depth 12) $Path }
function Read-Json([string]$Path) {
    [void](Assert-Local $Path)
    if (([IO.FileInfo]$Path).Length -gt 4194304) { throw 'An uploader metadata file is too large.' }
    return [IO.File]::ReadAllText($Path) | ConvertFrom-Json
}
function Local-Files([string]$Directory) {
    if (-not [IO.Directory]::Exists($Directory)) { return }
    $queue = New-Object 'Collections.Generic.Queue[string]'; $queue.Enqueue((Assert-Local $Directory))
    while ($queue.Count) {
        $next = $queue.Dequeue()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($next)) {
            [void](Assert-Local $entry)
            if ([IO.Directory]::Exists($entry)) { $queue.Enqueue($entry) } else { $entry }
        }
    }
}
function Hash-Stream($Stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Stream))).Replace('-', '') } finally { $sha.Dispose() }
}
function Hash-File([string]$Path) {
    # Native Unreal launch may inherit PS7's PSModulePath: no Get-FileHash dependency.
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { return Hash-Stream $stream } finally { $stream.Dispose() }
}
function Hash-Text([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() } finally { $sha.Dispose() }
}
function Assert-BuildPath([string]$Path) {
    $length = [IO.Path]::GetFullPath($Path).Length
    if ($length -ge 250) { throw ('The transfer workspace exceeds the Windows build path limit (' + $length + ' characters; maximum 249 with a 10-character Unreal build margin). Shorten the project location by at least ' + ($length - 249) + ' characters and retry.') }
}
function Assert-ManagedProject([string]$Text) {
    $value = $Text | ConvertFrom-Json
    if ($value.FileVersion -ne 3 -or $value.Description -ne 'Managed Cloud Avatars uploader workspace.') { throw 'Open Cloud Avatars in the host project to initialize the shared uploader, then retry.' }
    return $value
}
function Assert-UploaderOwner {
    try {
        $marker=Read-Json (Join-Path $script:workspace '.convai-workspace.json')
        if($marker.owner -ne 'ConvaiAvatarUploader' -or $marker.version -ne 2){throw 'Unrecognized owner'}
    } catch { throw 'Open Cloud Avatars in the host project to initialize the shared uploader, then retry. No unowned uploader files were changed.' }
}
function Restore-Project {
    if (-not [IO.File]::Exists($script:backup)) { return }
    $receipt = Read-Json $script:backup
    if ($receipt.owner -ne 'ConvaiAvatarUploader' -or $receipt.version -ne 1 -or $receipt.project_json -isnot [string]) { throw 'The uploader recovery record is invalid. Keep it and restore the managed uploader project before retrying.' }
    $null = Assert-ManagedProject $receipt.project_json
    Write-Text $receipt.project_json $script:project
    [IO.File]::Delete($script:backup)
}
function Retire-LegacyProjects {
    # Historical binaries/logs stay in place. Only exact owned project descriptors lose .uproject.
    foreach($layout in @('B','Builds')) {
        $parent=Join-Path $script:transport $layout
        if(-not [IO.Directory]::Exists($parent) -or (([IO.File]::GetAttributes($parent) -band [IO.FileAttributes]::ReparsePoint) -ne 0)){continue}
        foreach($directory in [IO.Directory]::EnumerateDirectories($parent)) {
            if(([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint) -ne 0){continue}
            $intentPath=Join-Path $directory 'ConvaiAvatarTransport.intent.json'
            if(-not [IO.File]::Exists($intentPath)){continue}
            try {
                $intent=Read-Json $intentPath
                if($intent.owner -ne 'ConvaiAvatarTransport' -or $intent.fingerprint -notmatch '^[A-Fa-f0-9]{64}$'){continue}
                $expected=if($layout -eq 'B'){$intent.fingerprint.Substring(0,16)}else{$intent.fingerprint}
                if([IO.Path]::GetFileName($directory) -ne $expected){continue}
            } catch {continue}
            foreach($name in @('CA','ConvaiAvatarTransport')) {
                $old=Join-Path $directory ($name+'.uproject');$retired=Join-Path $directory ($name+'.retired-project.json')
                if(-not [IO.File]::Exists($old) -or [IO.File]::Exists($retired)){continue}
                try {
                    $descriptor=Read-Json $old
                    if($descriptor.FileVersion -ne 3 -or @($descriptor.Modules).Count -ne 1 -or $descriptor.Modules[0].Name -ne $name -or $descriptor.Modules[0].Type -ne 'Editor' -or
                       @($descriptor.Plugins).Count -ne 1 -or $descriptor.Plugins[0].Name -ne 'ConvaiHTTP' -or $descriptor.Plugins[0].Enabled -ne $true){continue}
                } catch {continue}
                [void](Assert-Local $retired)
                [IO.File]::Move($old,$retired)
            }
        }
    }
}
function Source-Hashes([string]$Plugin) {
    $hashes = @{}
    foreach ($file in @((Join-Path $Plugin 'ConvaiHTTP.uplugin')) + @(Local-Files (Join-Path $Plugin 'Source')) + @(Local-Files (Join-Path $Plugin 'Resources'))) {
        [void](Assert-Local $file)
        $hashes[$file.Substring($script:workspace.Length).TrimStart('\','/').Replace('\','/')] = Hash-File $file
    }
    return $hashes
}
function Assert-HttpPlugin([string]$Plugin) {
    $descriptor = Read-Json (Join-Path $Plugin 'ConvaiHTTP.uplugin')
    $names = @($descriptor.Modules | ForEach-Object { $_.Name })
    $manifest = Read-Json (Join-Path $Plugin 'Resources/Transfer/manifest.json')
    if ($names -notcontains 'CONVAIHTTP' -or $names -notcontains 'ConvaiHTTPTransfer' -or
        @($descriptor.Modules | Where-Object { $_.Name -eq 'ConvaiHTTPTransfer' -and $_.Type -eq 'Editor' }).Count -ne 1 -or
        $manifest.transport_identity -ne 'CONVAIHTTP-transfer-v3' -or $manifest.schema_version -ne 1 -or
        $manifest.commandlet -ne 'ConvaiAvatarTransport' -or $manifest.request_argument -ne 'AvatarTransferRequest' -or
        $manifest.result_schema -ne 1 -or $manifest.max_file_bytes -ne '10485760000' -or
        -not [IO.File]::Exists((Join-Path $Plugin 'Source/ConvaiHTTPTransfer/ConvaiHTTPTransfer.Build.cs'))) { throw 'The resolved HTTP plugin does not contain the compatible transfer Editor module.' }
}
function Disabled-Plugins($FullProject) {
    $names = @{'ConvAI'=$true}
    if ($FullProject.PSObject.Properties['Plugins']) { foreach ($entry in $FullProject.Plugins) { $names[[string]$entry.Name] = $true } }
    $plugins = Join-Path $script:workspace 'Plugins'
    if ([IO.Directory]::Exists($plugins)) {
        foreach ($folder in [IO.Directory]::EnumerateDirectories($plugins)) {
            $name = [IO.Path]::GetFileName($folder)
            if ($name -eq 'ConvaiAvatars') {
                [void](Assert-Local $folder)
                # Names only: never follow avatar shortcuts into their content.
                foreach ($avatar in [IO.Directory]::EnumerateDirectories($folder)) { $names[[IO.Path]::GetFileName($avatar)] = $true }
            } else {
                [void](Assert-Local $folder)
                foreach ($descriptor in [IO.Directory]::EnumerateFiles($folder, '*.uplugin')) { [void](Assert-Local $descriptor); $names[[IO.Path]::GetFileNameWithoutExtension($descriptor)] = $true }
            }
        }
    }
    foreach ($name in @($names.Keys | Sort-Object)) {
        if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { throw 'The uploader contains an invalid plugin name.' }
        if ($name -ne 'ConvaiHTTP') { @{Name=$name; Enabled=$false} }
    }
}
function Read-ArchiveJson($Entry) {
    if($Entry.Length -gt 4194304){throw 'An HTTP archive metadata file is too large.'}
    $reader=New-Object IO.StreamReader($Entry.Open(),[Text.Encoding]::UTF8)
    try{return $reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}
}
function Validate-Prebuilt($Zip,[string]$Prefix) {
    $result=@{available=$false;matches=$false;files=@{};status='This HTTP release contains source. The uploader will build its two HTTP modules.'}
    $manifestEntries=@($Zip.Entries|Where-Object{$_.FullName.Replace('\','/') -ceq ($Prefix+'Resources/Transfer/prebuilt.json')})
    if($manifestEntries.Count -eq 0){return $result}
    if($manifestEntries.Count -ne 1){throw 'The HTTP archive has duplicate precompiled manifests.'}
    $manifest=Read-ArchiveJson $manifestEntries[0]
    $required=@('Binaries/Win64/UnrealEditor-CONVAIHTTP.dll','Binaries/Win64/UnrealEditor-ConvaiHTTPTransfer.dll','Binaries/Win64/UnrealEditor.modules')
    if($manifest.schema_version -ne 1 -or $manifest.engine_version -isnot [string] -or $manifest.engine_version -notmatch '^\d+\.\d+$' -or
       $manifest.engine_build_id -isnot [string] -or [string]::IsNullOrWhiteSpace($manifest.engine_build_id) -or
       $manifest.platform -cne 'Win64' -or $manifest.configuration -cne 'Development' -or @($manifest.files.PSObject.Properties).Count -ne 3){throw 'The HTTP archive has an invalid precompiled manifest. Check for uploader updates and retry.'}
    foreach($name in $required){
        $property=$manifest.files.PSObject.Properties[$name]
        if(-not $property -or $property.Name -cne $name -or $property.Value -isnot [string] -or $property.Value -notmatch '^[A-Fa-f0-9]{64}$'){throw 'The HTTP precompiled manifest does not identify its exact required module files.'}
        $entries=@($Zip.Entries|Where-Object{$_.FullName.Replace('\','/') -ceq ($Prefix+$name)})
        if($entries.Count -ne 1 -or $entries[0].Length -le 0){throw 'The HTTP archive is missing a required precompiled module file.'}
        $stream=$entries[0].Open();try{$hash=Hash-Stream $stream}finally{$stream.Dispose()}
        if($hash -ne $property.Value){throw 'A precompiled HTTP module checksum is invalid. Check for uploader updates and retry.'}
        $result.files[$name]=$hash
        if($name.EndsWith('.modules')){$modules=Read-ArchiveJson $entries[0]}
    }
    if($modules.BuildId -cne $manifest.engine_build_id -or @($modules.Modules.PSObject.Properties).Count -ne 2 -or
       $modules.Modules.CONVAIHTTP -cne 'UnrealEditor-CONVAIHTTP.dll' -or $modules.Modules.ConvaiHTTPTransfer -cne 'UnrealEditor-ConvaiHTTPTransfer.dll'){throw 'The HTTP precompiled loader manifest does not match its module files and engine build.'}
    $result.available=$true
    $result.matches=$manifest.engine_version -ceq $script:engineVersion -and $manifest.engine_build_id -ceq $script:buildId
    $result.status=if($result.matches){'Using the precompiled HTTP modules for this Unreal editor; no compilation is needed.'}else{'The precompiled HTTP modules target a different Unreal editor build. The uploader will build compatible modules from the included source.'}
    return $result
}
$lock = $null; $safeResult = $null; $backup = $null; $exitCode = 1
try {
    $transport = (Assert-Local $TransportDirectory).TrimEnd('\','/')
    if ([IO.Path]::GetFileName($transport) -ne 'Transport') { throw 'Transfer data must use the managed uploader Transport folder.' }
    $workspace = Assert-Local ([IO.Path]::GetDirectoryName($transport))
    $engine = (Assert-Local $EngineDirectory).TrimEnd('\','/')
    $result = Assert-Local $ResultFile
    if (-not (Below $result $transport)) { throw 'The bootstrap result must stay inside the transport data folder.' }
    # Ownership is checked before creating transfer data, lock files, or diagnostic output.
    Assert-UploaderOwner
    $safeResult = $result; [IO.Directory]::CreateDirectory($transport) | Out-Null
    $lockPath = Assert-Local (Join-Path $workspace 'packaging.lock')
    if (-not $WorkspaceLockHeld) {
        try { $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
        catch { throw 'Another editor is using the uploader for packaging or a transfer. Wait for it to finish and retry.' }
    }
    Assert-UploaderOwner
    $project = Join-Path $workspace 'CA.uproject'; $backup = Join-Path $workspace 'CA.bootstrap-backup.json'
    Restore-Project
    $fullProjectText = [IO.File]::ReadAllText((Assert-Local $project)); $fullProject = Assert-ManagedProject $fullProjectText
    Retire-LegacyProjects
    $resolvedPath=Assert-Local $ResolutionFile
    if(-not (Below $resolvedPath $transport)){throw 'The dependency resolution must stay inside this uploader transfer data folder.'}
    $resolution = Read-Json $resolvedPath
    $engineBuild=Read-Json (Join-Path $engine 'Build/Build.version')
    if(($engineBuild.MajorVersion -isnot [int] -and $engineBuild.MajorVersion -isnot [long]) -or ($engineBuild.MinorVersion -isnot [int] -and $engineBuild.MinorVersion -isnot [long]) -or $engineBuild.MajorVersion -lt 1 -or $engineBuild.MinorVersion -lt 0){throw 'The installed Unreal Engine version could not be verified. Check the engine installation and retry.'}
    $engineVersion=[string]$engineBuild.MajorVersion+'.'+[string]$engineBuild.MinorVersion
    if ($resolution.success -ne $true -or $resolution.schema_version -ne 1 -or
        $resolution.protocol -ne 'CONVAIHTTP-transfer-v3' -or $resolution.engine_version -ne $engineVersion -or
        $resolution.archive_sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or $resolution.resolution_id -notmatch '^[A-Fa-f0-9]{64}$') { throw 'The dependency resolution is invalid or incompatible. Check for uploader updates and retry.' }
    $archive = Assert-Local $resolution.archive_path
    if (-not (Below $archive (Join-Path $transport 'Downloads'))) { throw 'The resolved HTTP source is outside the uploader download cache. Check for uploader updates and retry.' }
    if (-not [IO.File]::Exists($archive)) { throw 'The resolved HTTP archive is missing from the uploader cache. Check for uploader updates again to download it, then retry.' }
    if ((Hash-File $archive) -ne $resolution.archive_sha256) { throw 'The resolved HTTP source checksum is invalid. Resolve the dependency again and retry.' }
    $buildId = [string](Read-Json (Join-Path $engine 'Binaries/Win64/UnrealEditor.modules')).BuildId
    if (-not $buildId) { throw 'The selected Unreal Engine has no editor build identity.' }
    # Full project JSON / avatar selection never changes the HTTP build fingerprint.
    $fingerprint = Hash-Text ($engine.ToLowerInvariant() + [Environment]::NewLine + $buildId + [Environment]::NewLine + [IO.File]::ReadAllText((Join-Path $engine 'Build/Build.version')) + [Environment]::NewLine + $resolution.archive_sha256 + [Environment]::NewLine + (Hash-File $PSCommandPath))
    $plugin = Assert-Local (Join-Path $workspace 'Plugins/ConvaiHTTP')
    $intentPath = Join-Path $transport 'ConvaiAvatarTransport.intent.json'; $readyPath = Join-Path $transport 'ConvaiAvatarTransport.ready.json'
    $previousFiles = @{}
    if ([IO.Directory]::Exists($plugin)) {
        try {
            $intent = Read-Json $intentPath
            if ($intent.owner -ne 'ConvaiAvatarTransport' -or $intent.project_relative -ne 'CA.uproject') { throw 'Unrecognized HTTP owner' }
        } catch { throw 'The uploader HTTP plugin ownership record is missing or unreadable. Keep the plugin folder and restore its Transport/ConvaiAvatarTransport.intent.json record, or move only that HTTP cache folder to a backup location before retrying.' }
        foreach ($field in @('source_files','previous_files')) {
            if ($intent.PSObject.Properties[$field]) { foreach ($property in $intent.$field.PSObject.Properties) { if (-not $previousFiles.ContainsKey($property.Name)) { $previousFiles[$property.Name]=@() }; $previousFiles[$property.Name] += [string]$property.Value } }
        }
    }
    $validCache = $false
    # The previous verified build also owns these three outputs, including source-built revisions.
    if([IO.File]::Exists($readyPath)) {
        try {
            $previousReady=Read-Json $readyPath
            foreach($name in @('Binaries/Win64/UnrealEditor-CONVAIHTTP.dll','Binaries/Win64/UnrealEditor-ConvaiHTTPTransfer.dll','Binaries/Win64/UnrealEditor.modules')) {
                $key='Plugins/ConvaiHTTP/'+$name;$property=$previousReady.files.PSObject.Properties[$key]
                if($property -and $property.Value -match '^[A-Fa-f0-9]{64}$') { $previousFiles[$name]=@($previousFiles[$name])+[string]$property.Value }
            }
        } catch { } # Invalid readiness never authorizes an overwrite.
    }
    if ([IO.File]::Exists($readyPath)) {
        try {
            $ready = Read-Json $readyPath; $validCache = $ready.fingerprint -eq $fingerprint -and $ready.build_id -eq $buildId
            Assert-HttpPlugin $plugin
            $sourceHashes = Source-Hashes $plugin
            foreach ($key in $sourceHashes.Keys) { if (-not $ready.files.PSObject.Properties[$key] -or $ready.files.$key -ne $sourceHashes[$key]) { $validCache = $false } }
            foreach ($required in @('Plugins/ConvaiHTTP/ConvaiHTTP.uplugin','Plugins/ConvaiHTTP/Binaries/Win64/UnrealEditor.modules','Plugins/ConvaiHTTP/Binaries/Win64/UnrealEditor-CONVAIHTTP.dll','Plugins/ConvaiHTTP/Binaries/Win64/UnrealEditor-ConvaiHTTPTransfer.dll')) { if (-not $ready.files.PSObject.Properties[$required]) { $validCache = $false } }
            foreach ($property in $ready.files.PSObject.Properties) {
                $file = Assert-Local (Join-Path $workspace $property.Name)
                if (-not (Below $file $plugin) -or -not [IO.File]::Exists($file) -or (Hash-File $file) -ne $property.Value) { $validCache = $false; break }
            }
            $modules = Read-Json (Join-Path $plugin 'Binaries/Win64/UnrealEditor.modules')
            if ($modules.BuildId -ne $buildId -or -not $modules.Modules.PSObject.Properties['CONVAIHTTP'] -or -not $modules.Modules.PSObject.Properties['ConvaiHTTPTransfer']) { $validCache = $false }
        } catch { $validCache = $false }
    }
    if (-not $validCache) {
        $zip = [IO.Compression.ZipFile]::OpenRead($archive)
        try {
            $descriptors = @($zip.Entries | Where-Object { $_.FullName.Replace('\','/') -eq 'ConvaiHTTP.uplugin' -or $_.FullName.Replace('\','/').EndsWith('/ConvaiHTTP.uplugin') })
            if ($descriptors.Count -ne 1) { throw 'The HTTP archive must contain exactly one plugin descriptor.' }
            $prefix = $descriptors[0].FullName.Replace('\','/'); $prefix = $prefix.Substring(0,$prefix.Length-'ConvaiHTTP.uplugin'.Length)
            if ($prefix.TrimEnd('/').Contains('/')) { throw 'The HTTP archive has more than one wrapper folder.' }
            $prebuilt=Validate-Prebuilt $zip $prefix
            if(-not $prebuilt.matches) {
                foreach ($relative in @(
                    'Intermediate/Build/Win64/x64/UnrealEditor/Development/UnrealEd/SharedPCH.UnrealEd.Project.ValApi.ValExpApi.Cpp20.h.dep.json',
                    'Plugins/ConvaiHTTP/Intermediate/Build/Win64/x64/UnrealEditor/Development/ConvaiHTTPTransfer/ConvaiAvatarTransportCommandlet.cpp.obj.rsp',
                    'Plugins/ConvaiHTTP/Intermediate/Build/Win64/x64/UnrealEditor/Development/CONVAIHTTP/CurlConvaihttpThread.cpp.obj.rsp'
                )) { Assert-BuildPath (Join-Path $workspace $relative) }
            }
            $seen=@{}; $entries=@(); $desired=@{}; $allNames=@{}
            foreach ($entry in $zip.Entries) {
                $raw=$entry.FullName.Replace('\','/')
                if ($raw.StartsWith('/') -or $raw.Contains(':') -or @($raw.Split('/') | Where-Object { $_ -eq '..' }).Count -or (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'The HTTP source has an unsafe archive entry.' }
                if ($raw.EndsWith('/')) { continue }
                if ($allNames.ContainsKey($raw)) { throw 'The HTTP archive has duplicate entries.' }; $allNames[$raw]=$true
                if (-not $raw.StartsWith($prefix,[StringComparison]::Ordinal)) { continue }
                $name=$raw.Substring($prefix.Length)
                if ($name -ne 'ConvaiHTTP.uplugin' -and -not $name.StartsWith('Source/') -and -not $name.StartsWith('Resources/') -and -not ($prebuilt.matches -and $prebuilt.files.ContainsKey($name))) { continue }
                if ($seen.ContainsKey($name)) { throw 'The HTTP source has duplicate entries.' }; $seen[$name]=$true
                $target=Assert-Local (Join-Path $plugin $name)
                if (-not (Below $target $plugin)) { throw 'An HTTP source file escapes its plugin.' }; if(-not $prebuilt.matches){Assert-BuildPath $target}
                $input=$entry.Open(); try { $desired[$name]=Hash-Stream $input } finally { $input.Dispose() }
                $entries+=@{entry=$entry; target=$target; name=$name}
            }
            # Only prior manifest-owned unchanged files may be replaced or removed on a version update.
            $oldFiles=@(Local-Files (Join-Path $plugin 'Source'))+@(Local-Files (Join-Path $plugin 'Resources'))
            if ([IO.File]::Exists((Join-Path $plugin 'ConvaiHTTP.uplugin'))) { $oldFiles+=Join-Path $plugin 'ConvaiHTTP.uplugin' }
            foreach($name in @('Binaries/Win64/UnrealEditor-CONVAIHTTP.dll','Binaries/Win64/UnrealEditor-ConvaiHTTPTransfer.dll','Binaries/Win64/UnrealEditor.modules')) {
                $binary=Assert-Local (Join-Path $plugin $name)
                if([IO.File]::Exists($binary) -and ($prebuilt.matches -or $previousFiles.ContainsKey($name))){$oldFiles+=$binary}
            }
            foreach ($old in $oldFiles) {
                $name=$old.Substring($plugin.Length).TrimStart('\','/').Replace('\','/'); $hash=Hash-File $old
                if (($previousFiles[$name] -notcontains $hash) -and $desired[$name] -ne $hash) { throw 'The uploader HTTP source contains local changes or unowned files. Preserve that plugin folder elsewhere before retrying.' }
            }
            $oldManifest=@{}; foreach($old in $oldFiles) { $oldManifest[$old.Substring($plugin.Length).TrimStart('\','/').Replace('\','/')]=Hash-File $old }
            Write-Json @{owner='ConvaiAvatarTransport'; project_relative='CA.uproject'; source_files=$desired; previous_files=$oldManifest; resolution_id=$resolution.resolution_id} $intentPath
            $installRoot=Assert-Local (Join-Path $transport ('Install/'+$resolution.resolution_id))
            $installOwner=Join-Path $installRoot 'owner.json'
            if([IO.Directory]::Exists($installRoot)) {
                $owner=Read-Json $installOwner
                if($owner.owner -ne 'ConvaiAvatarTransportInstaller' -or $owner.resolution_id -ne $resolution.resolution_id) { throw 'The HTTP source staging folder belongs to another operation.' }
            } else {
                [IO.Directory]::CreateDirectory($installRoot)|Out-Null
                Write-Json @{owner='ConvaiAvatarTransportInstaller';resolution_id=$resolution.resolution_id} $installOwner
            }
            foreach($old in $oldFiles) { $name=$old.Substring($plugin.Length).TrimStart('\','/').Replace('\','/'); if(-not $desired.ContainsKey($name)) { [IO.File]::Delete($old) } }
            $entryIndex=0
            foreach($item in $entries) {
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($item.target)) | Out-Null
                $temporary=Assert-Local (Join-Path $installRoot ($entryIndex.ToString('D6')+'.tmp'));$entryIndex++
                # Stage complete bytes separately; cancellation must not partly overwrite previously valid source.
                try {
                    $input=$item.entry.Open()
                    try { $output=[IO.File]::Open($temporary,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None); try{$input.CopyTo($output)}finally{$output.Dispose()} } finally{$input.Dispose()}
                    if((Hash-File $temporary) -ne $desired[$item.name]) { throw 'The staged HTTP source checksum changed.' }
                    [void](Assert-Local $item.target)
                    if([IO.File]::Exists($item.target)) {
                        $currentHash=Hash-File $item.target
                        if($currentHash -ne $oldManifest[$item.name] -and $currentHash -ne $desired[$item.name]) { throw 'The uploader HTTP source changed during its update. Its file was kept; retry after resolving local changes.' }
                        [IO.File]::Replace($temporary,$item.target,[NullString]::Value)
                    } else { [IO.File]::Move($temporary,$item.target) }
                } finally { if([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
            }
            Write-Json @{owner='ConvaiAvatarTransport'; project_relative='CA.uproject'; source_files=$desired; previous_files=@{}; resolution_id=$resolution.resolution_id} $intentPath
        } finally { $zip.Dispose() }
        Assert-HttpPlugin $plugin
        if ($prebuilt.matches) {
            $hashes=Source-Hashes $plugin
            foreach($name in $prebuilt.files.Keys) {
                $file=Assert-Local (Join-Path $plugin $name)
                if((Hash-File $file) -ne $prebuilt.files[$name]){throw 'A precompiled HTTP module changed during installation. Retry after resolving the cache changes.'}
                $hashes['Plugins/ConvaiHTTP/'+$name]=$prebuilt.files[$name]
            }
            Write-Json @{fingerprint=$fingerprint;build_id=$buildId;files=$hashes;precompiled=$true} $readyPath
            Write-Json @{success=$true;prepared_only=$false;precompiled=$true;status=$prebuilt.status;project=$project;fingerprint=$fingerprint;build_id=$buildId;resolution_id=$resolution.resolution_id} $result
        }
        elseif ($PrepareOnly) { Write-Json @{success=$true; prepared_only=$true; precompiled=$false;status=$prebuilt.status;project=$project; fingerprint=$fingerprint; build_id=$buildId; resolution_id=$resolution.resolution_id} $result }
        else {
            $sdkHashes=@{}; foreach($file in @(Local-Files (Join-Path $workspace 'Plugins/Convai/Binaries'))) { $sdkHashes[$file]=Hash-File $file }
            Write-Json @{owner='ConvaiAvatarUploader'; version=1; project_json=$fullProjectText} $backup
            Write-Json @{FileVersion=3; Description='Managed Cloud Avatars uploader workspace.'; DisableEnginePluginsByDefault=$true; Plugins=@(@{Name='ConvaiHTTP'; Enabled=$true})+@(Disabled-Plugins $fullProject)} $project
            $buildLog=Assert-Local (Join-Path $transport 'build.log')
            # The installed UnrealEditor target has bBuildAllModules=true, even for disabled plugins.
            # UBT's OnlyModuleNames filter limits output actions to these modules and their prerequisites;
            # neither HTTP module depends on the SDK. Keep both explicit so a generic target cannot rebuild it.
            & (Join-Path $engine 'Build/BatchFiles/Build.bat') UnrealEditor Win64 Development "-Project=$project" -Module=CONVAIHTTP -Module=ConvaiHTTPTransfer -WaitMutex -NoHotReloadFromIDE 2>&1 | Out-File -LiteralPath $buildLog -Encoding utf8
            if($LASTEXITCODE -ne 0) { throw ('The transfer module did not build. Check its build details, resolve the error, then retry: '+$buildLog) }
            foreach($file in $sdkHashes.Keys) { if(-not [IO.File]::Exists($file) -or (Hash-File $file) -ne $sdkHashes[$file]) { throw 'Building the HTTP module changed an existing SDK binary. Upload stopped; restore the uploader SDK copy before retrying.' } }
            # -Module filters out UBT's target-wide metadata action as well as unrelated binaries.
            # The successful installed-engine build above supplies compatibility; write only this
            # owned plugin's loader manifest after verifying both fixed outputs, never a global receipt.
            $moduleFiles=@{CONVAIHTTP='UnrealEditor-CONVAIHTTP.dll';ConvaiHTTPTransfer='UnrealEditor-ConvaiHTTPTransfer.dll'}
            foreach($filename in $moduleFiles.Values) {
                $builtDll=Assert-Local (Join-Path $plugin ('Binaries/Win64/'+$filename))
                if(-not [IO.File]::Exists($builtDll) -or (Get-Item -LiteralPath $builtDll).Length -eq 0) { throw 'The HTTP build did not produce both required module files. Open the build details and retry after resolving the error.' }
            }
            $modulesPath=Assert-Local (Join-Path $plugin 'Binaries/Win64/UnrealEditor.modules')
            Write-Json @{BuildId=$buildId;Modules=$moduleFiles} $modulesPath
            $hashes=Source-Hashes $plugin; $modules=Read-Json $modulesPath
            if($modules.BuildId -ne $buildId) { throw 'The HTTP module was built for a different Unreal editor.' }
            $hashes['Plugins/ConvaiHTTP/Binaries/Win64/UnrealEditor.modules']=Hash-File $modulesPath
            foreach($name in @('CONVAIHTTP','ConvaiHTTPTransfer')) {
                if(-not $modules.Modules.PSObject.Properties[$name]) { throw 'A required HTTP module was not produced by the build.' }
                $binary=Assert-Local (Join-Path ([IO.Path]::GetDirectoryName($modulesPath)) ([string]$modules.Modules.$name))
                if(-not (Below $binary $plugin)) { throw 'An HTTP module points outside its managed plugin.' }
                $hashes[$binary.Substring($workspace.Length).TrimStart('\','/').Replace('\','/')]=Hash-File $binary
            }
            Write-Json @{fingerprint=$fingerprint; build_id=$buildId; files=$hashes;precompiled=$false} $readyPath
            Write-Json @{success=$true; prepared_only=$false;precompiled=$false;status=$prebuilt.status; project=$project; fingerprint=$fingerprint; build_id=$buildId; resolution_id=$resolution.resolution_id} $result
        }
    } else { Write-Json @{success=$true; prepared_only=$false;precompiled=($ready.PSObject.Properties['precompiled'] -and $ready.precompiled); project=$project; fingerprint=$fingerprint; build_id=$buildId; resolution_id=$resolution.resolution_id} $result }
    $exitCode=0
} catch {
    if($safeResult -and [IO.Directory]::Exists([IO.Path]::GetDirectoryName($safeResult))) { Write-Json @{success=$false; error=$_.Exception.Message} $safeResult }
    elseif(-not $safeResult){[Console]::Error.WriteLine($_.Exception.Message)}
} finally {
    # A terminated parent/child leaves the full descriptor for EnsureProject or the next bootstrap.
    try { if($backup) { Restore-Project } }
    catch { $exitCode=1; if($safeResult) { Write-Json @{success=$false; error='The uploader project could not be restored. Its recovery record was kept; close other uploader processes and retry.'} $safeResult } }
    if($lock) { $lock.Dispose() }
}
exit $exitCode
