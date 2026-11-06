# Runner-side Unreal build helpers: the per-engine MSVC pin, and a generated
# C++ host project that packages the released plugin as a Win64 game.
Set-StrictMode -Version Latest

function Set-ConvaiMsvcPin([string]$Engine, [string]$ConfigPath) {
    # UE <=5.7 require MSVC <=14.39 (14.40+ breaks engine code, e.g. ConcurrentLinearAllocator).
    # UE 5.8 requires >=14.44 (14.38 fails compiling its container headers). Both toolchains
    # must be installed on the runner. BuildPlugin ignores the -CompilerVersion arg, so pin via
    # the user BuildConfiguration.xml; a surgical regex swap preserves other settings.
    $msvcVersion = if ($Engine -eq '5.8') { '14.44.35207' } else { '14.38.33130' }
    New-Item -ItemType Directory -Force -Path (Split-Path $ConfigPath -Parent) | Out-Null
    if (Test-Path -LiteralPath $ConfigPath) { $cfgXml = Get-Content -LiteralPath $ConfigPath -Raw } else { $cfgXml = "<?xml version=`"1.0`" encoding=`"utf-8`"?>`r`n<Configuration xmlns=`"https://www.unrealengine.com/BuildConfiguration`">`r`n</Configuration>" }
    if ($cfgXml -match '<CompilerVersion>') {
        $cfgXml = $cfgXml -replace '<CompilerVersion>[^<]*</CompilerVersion>', "<CompilerVersion>$msvcVersion</CompilerVersion>"
        $cfgXml = $cfgXml -replace '<ToolchainVersion>[^<]*</ToolchainVersion>', "<ToolchainVersion>$msvcVersion</ToolchainVersion>"
    } else {
        $cfgXml = $cfgXml -replace '</Configuration>', "  <WindowsPlatform>`r`n    <CompilerVersion>$msvcVersion</CompilerVersion>`r`n    <ToolchainVersion>$msvcVersion</ToolchainVersion>`r`n  </WindowsPlatform>`r`n</Configuration>"
    }
    Set-Content -LiteralPath $ConfigPath -Value $cfgXml -Encoding UTF8
    Write-Host "Pinned MSVC $msvcVersion for UE $Engine"
}

function New-ConvaiPackagingSmokeProject([string]$Root, [string]$Engine, [string]$Plugin) {
    if (-not (Test-Path -LiteralPath (Join-Path $Plugin 'ConvAI.uplugin') -PathType Leaf)) { throw "Packaged plugin not found for UE ${Engine}: $Plugin" }
    # Each release attempt has its own output root, so an existing project is never ours to reuse.
    if (Test-Path -LiteralPath $Root) { throw "The packaging smoke project already exists: $Root" }
    $Root = [IO.Path]::GetFullPath($Root)
    $name = 'PackagingSmoke'
    $files = [ordered]@{}
    $files["$name.uproject"] = [ordered]@{
        FileVersion = 3; EngineAssociation = $Engine; Category = ''; Description = 'CI packaging smoke test'
        Modules = @([ordered]@{ Name = $name; Type = 'Runtime'; LoadingPhase = 'Default' })
        Plugins = @([ordered]@{ Name = 'ConvAI'; Enabled = $true })
    } | ConvertTo-Json -Depth 5
    foreach ($kind in @('Game', 'Editor')) {
        $target = if ($kind -eq 'Game') { $name } else { "${name}Editor" }
        $files["Source/${target}.Target.cs"] = @"
using UnrealBuildTool;

public class ${target}Target : TargetRules
{
	public ${target}Target(TargetInfo Target) : base(Target)
	{
		Type = TargetType.$kind;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("$name");
	}
}
"@
    }
    $files["Source/$name/$name.Build.cs"] = @"
using UnrealBuildTool;

public class $name : ModuleRules
{
	public $name(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}
"@
    $files["Source/$name/$name.cpp"] = "#include `"Modules/ModuleManager.h`"`n`nIMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, $name, `"$name`");`n"
    # The plugin's Blueprint components load its classes in the cooker, without the MetaHuman content.
    $files['Config/DefaultGame.ini'] = "[/Script/UnrealEd.ProjectPackagingSettings]`n+DirectoriesToAlwaysCook=(Path=`"/ConvAI/ConvaiConveniencePack`")`n"
    foreach ($relative in $files.Keys) {
        $path = Join-Path $Root $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
        [IO.File]::WriteAllText($path, $files[$relative], [Text.UTF8Encoding]::new($false))
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $Root 'Plugins') | Out-Null
    Copy-Item -LiteralPath $Plugin -Destination (Join-Path $Root 'Plugins/Convai') -Recurse -Force
    return (Join-Path $Root "$name.uproject")
}

function Assert-ConvaiStagedNativeLibraries([string]$UProject) {
    $root = Split-Path $UProject -Parent
    $name = [IO.Path]::GetFileNameWithoutExtension($UProject)
    $game = Join-Path $root "Saved/StagedBuilds/Windows/$name"
    # A packaged game loads convai_client.dll by name from beside its executable (Convai.cpp).
    # The Scene Auto Tagger adapter loads the plugin's Source/ThirdParty core, the only copy
    # staged with its HTTP helper (SceneAutoTaggerNativeCoreAdapter.cpp, ConvaiSceneAutoTaggerCore.Build.cs).
    $layout = [ordered]@{
        'Source/ThirdParty/ConvaiWebRTC/lib/release/win64' = 'Binaries/Win64'
        'Source/ThirdParty/ConvaiSceneAutoTaggerCore/Win64/bin' = 'Plugins/Convai/Source/ThirdParty/ConvaiSceneAutoTaggerCore/Win64/bin'
    }
    $staged = 0
    $missing = @()
    foreach ($source in $layout.Keys) {
        $libraries = @(Get-ChildItem -LiteralPath (Join-Path $root "Plugins/Convai/$source") -Filter '*.dll' -File -ErrorAction SilentlyContinue)
        if (-not $libraries.Count) { throw "The packaged plugin ships no native libraries in $source." }
        foreach ($library in $libraries) {
            $relative = "$($layout[$source])/$($library.Name)"
            if (Test-Path -LiteralPath (Join-Path $game $relative) -PathType Leaf) { $staged++ } else { $missing += $relative }
        }
    }
    if ($missing.Count) { throw "The packaged $name game is missing native libraries where the runtime loads them: $($missing -join ', ')" }
    Write-Host "Packaged $name stages all $staged native libraries where the runtime loads them."
}

function Assert-ConvaiReceiptShipsPluginFiles([string]$UProject, [string]$PackagedPlugin) {
    # UBT never writes runtime dependencies into an installed engine, so an engine-installed
    # plugin packages only if every plugin file a game stages is already in the package.
    # The host project's own plugin copy hides this: UBT fills it in during the build.
    $root = Split-Path $UProject -Parent
    $prefix = '$(ProjectDir)/Plugins/Convai/'
    $receipts = @(Get-ChildItem -LiteralPath (Join-Path $root 'Binaries/Win64') -Filter '*.target' -File -ErrorAction SilentlyContinue |
        ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json } | Where-Object { $_.TargetType -eq 'Game' })
    if (-not $receipts.Count) { throw "No game receipt found under $root/Binaries/Win64." }
    $missing = @(foreach ($receipt in $receipts) {
        foreach ($dependency in @($receipt.RuntimeDependencies)) {
            if (-not $dependency.Path.StartsWith($prefix, [StringComparison]::Ordinal)) { continue }
            $relative = $dependency.Path.Substring($prefix.Length)
            if (-not (Test-Path -LiteralPath (Join-Path $PackagedPlugin $relative) -PathType Leaf)) { $relative }
        }
    })
    if ($missing.Count) { throw "Games stage plugin files the package does not ship, so packaging fails with the plugin installed in the engine: $(($missing | Sort-Object -Unique) -join ', ')" }
    Write-Host "Every plugin file the game receipts stage ships in the package."
}
