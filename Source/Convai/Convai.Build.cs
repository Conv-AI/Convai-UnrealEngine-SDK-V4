// Copyright 2022 Convai Inc. All Rights Reserved.

using UnrealBuildTool;
using System;
using System.IO;
using System.Collections.Generic;
using System.Reflection;
using EpicGames.Core;

// using Tools.DotNETCommon;



public class Convai : ModuleRules
{
    private static ConvaiPlatform _convaiPlatformInstance;
    
    // Make these class-level fields so they're accessible throughout the class
    private const bool BEnableConvaiHttp = false;

    private string ModulePath
	{
		get { return ModuleDirectory; }
	}

	private string ThirdPartyPath
	{
		get { return Path.GetFullPath(Path.Combine(ModulePath, "../ThirdParty/")); }
	}

    private string ConvaiWebRtcPath
    {
        get { return Path.GetFullPath(Path.Combine(ThirdPartyPath, "ConvaiWebRTC/")); }
    }

    private string BinariesPath
    {
        get { return Path.GetFullPath(Path.Combine(ModulePath, "../../Binaries/")); }
    }
    
    private static ConvaiPlatform GetConvaiPlatformInstance(ReadOnlyTargetRules target)
    {
        var convaiPlatformType = System.Type.GetType("ConvaiPlatform_" + target.Platform.ToString());
        if (convaiPlatformType == null)
        {
            throw new BuildException("Convai does not support platform " + target.Platform.ToString());
        }

        var platformInstance = Activator.CreateInstance(convaiPlatformType) as ConvaiPlatform;
        return platformInstance ?? throw new BuildException("Convai could not instantiate platform " + target.Platform.ToString());
    }

    private bool ConfigurePlatform(ReadOnlyTargetRules target, UnrealTargetConfiguration configuration)
    {
        // ConvaiWebRTC libraries path
        foreach (var arch in _convaiPlatformInstance.Architectures())
        {
            var useDebugThirdParty = target.Configuration == UnrealTargetConfiguration.Debug||
                                     Target.Configuration == UnrealTargetConfiguration.DebugGame;

            var buildConfig = useDebugThirdParty ? "release" : "release";

            var webRtcPath = Path.Combine(ConvaiWebRtcPath, "lib", buildConfig, _convaiPlatformInstance.LibrariesPath);
            Log.TraceInformation($"[ConvaiWebRTC] Using third-party config folder: {buildConfig}  ->  {webRtcPath}");

            var webRtcArchPath = webRtcPath + arch;

            if (target.Platform == UnrealTargetPlatform.Win64)
            {
                // Add all .lib files
                var libFiles = Directory.GetFiles(webRtcArchPath, "*.lib");
                PublicAdditionalLibraries.AddRange(libFiles);
                
                // Add all .dll files as runtime dependencies
                var dllFiles = Directory.GetFiles(webRtcArchPath, "*.dll");
                foreach (var dllFile in dllFiles)
                {
                    var dllName = Path.GetFileName(dllFile);
                    PublicDelayLoadDLLs.Add(dllName);
                    
                    // Copy DLLs to plugin's Binaries folder (for blueprint-only projects)
                    var pluginBinaryPath = Path.Combine(BinariesPath, "Win64", dllName);
                    RuntimeDependencies.Add(pluginBinaryPath, dllFile, StagedFileType.NonUFS);
                    
                    // Also copy to target output directory (for packaged builds)
                    RuntimeDependencies.Add("$(TargetOutputDir)/" + dllName, dllFile, StagedFileType.NonUFS);
                    
                    // And to project binaries (for C++ projects)
                    RuntimeDependencies.Add("$(ProjectDir)/Binaries/Win64/" + dllName, dllFile, StagedFileType.NonUFS);
                }
            }
            else if (target.Platform == UnrealTargetPlatform.Mac)
            {
                // Add all .a files
                var aFiles = Directory.GetFiles(webRtcArchPath, "*.a");
                PublicAdditionalLibraries.AddRange(aFiles);
                
                // Add all .dylib files as runtime dependencies
                var dylibFiles = Directory.GetFiles(webRtcArchPath, "*.dylib");
                foreach (var dylibFile in dylibFiles)
                {
                    var dylibName = Path.GetFileName(dylibFile);
                    PublicDelayLoadDLLs.Add(dylibName);
                    
                    // Copy dylibs to plugin's Binaries folder (for blueprint-only projects)
                    var pluginBinaryPath = Path.Combine(BinariesPath, "Mac", dylibName);
                    RuntimeDependencies.Add(pluginBinaryPath, dylibFile, StagedFileType.NonUFS);
                    
                    // Also copy to target output directory (for packaged builds)
                    RuntimeDependencies.Add("$(TargetOutputDir)/" + dylibName, dylibFile, StagedFileType.NonUFS);
                    
                    // And to project binaries (for C++ projects)
                    RuntimeDependencies.Add("$(ProjectDir)/Binaries/Mac/" + dylibName, dylibFile, StagedFileType.NonUFS);
                }
            }
            else if (target.Platform == UnrealTargetPlatform.Linux)
            {
                // Add all .a files (static libraries)
                var aFiles = Directory.GetFiles(webRtcArchPath, "*.a");
                PublicAdditionalLibraries.AddRange(aFiles);
                
                // Add all .so files for linking and as runtime dependencies
                var soFiles = Directory.GetFiles(webRtcArchPath, "*.so");
                foreach (var soFile in soFiles)
                {
                    var soName = Path.GetFileName(soFile);
                    // Add .so files for linking (this is the key fix)
                    PublicAdditionalLibraries.Add(soFile);
                    
                    // Copy .so files to plugin's Binaries folder (for blueprint-only projects)
                    var pluginBinaryPath = Path.Combine(BinariesPath, "Linux", soName);
                    RuntimeDependencies.Add(pluginBinaryPath, soFile, StagedFileType.NonUFS);
                    
                    // Also add as runtime dependencies
                    RuntimeDependencies.Add("$(TargetOutputDir)/" + soName, soFile, StagedFileType.NonUFS);
                    RuntimeDependencies.Add("$(ProjectDir)/Binaries/Linux/" + soName, soFile, StagedFileType.NonUFS);
                }
            }
            else if (target.Platform == UnrealTargetPlatform.IOS)
            {
                // Add all .a files for iOS
                string[] aFiles = Directory.GetFiles(webRtcArchPath, "*.a");
                PublicAdditionalLibraries.AddRange(aFiles);
                
                // Add all .dylib files if they exist
                string[] dylibFiles = Directory.GetFiles(webRtcArchPath, "*.dylib");
                foreach (string dylibFile in dylibFiles)
                {
                    string dylibName = Path.GetFileName(dylibFile);
                    PublicAdditionalLibraries.Add(dylibFile);
                    RuntimeDependencies.Add(dylibFile);
                }
            }
            else if (target.Platform == UnrealTargetPlatform.Android)
            {
                // Add all .a files
                string[] aFiles = Directory.GetFiles(webRtcArchPath, "*.a");
                PublicAdditionalLibraries.AddRange(aFiles);
                
                // Add all .so files for Android
                string[] soFiles = Directory.GetFiles(webRtcArchPath, "*.so");
                foreach (string soFile in soFiles)
                {
                    string soName = Path.GetFileName(soFile);
                    PublicAdditionalLibraries.Add(soFile);
                    RuntimeDependencies.Add("$(TargetOutputDir)/libs/" + arch + "/" + soName, soFile, StagedFileType.NonUFS);
                }
            }
        }
        
        return false;
    }

    public Convai(ReadOnlyTargetRules Target) : base(Target)
    {
        // Common Settings
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrecompileForTargets = PrecompileTargetsType.Any;
        _convaiPlatformInstance = GetConvaiPlatformInstance(Target);
        
        PublicDefinitions.AddRange(new string[] { "USE_CONVAI_HTTP=0" + (BEnableConvaiHttp ? "1" : "0")});

        PrivateIncludePaths.Add("Convai/Private");

        if (Target.Platform == UnrealTargetPlatform.Mac || Target.Platform == UnrealTargetPlatform.IOS)
        {
            // Include .mm file only for Mac
            PrivateIncludePaths.Add("Convai/Private/Mac");
        }

        if (BEnableConvaiHttp)
        {
            PublicDependencyModuleNames.AddRange(new string[] { "CONVAIHTTP", "HTTP" });
        }
        else
        {
            PublicDependencyModuleNames.AddRange(new string[] { "HTTP" });
        }

        // Add ConvaiWebRTC include path
        PrivateIncludePaths.AddRange(new string[] { Path.Combine(ConvaiWebRtcPath, "include") });

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "Json", "JsonUtilities", "AudioMixer", "AudioCaptureCore", "AudioCapture", "Voice", "SignalProcessing", "libOpus", "OpenSSL", "zlib", "SSL" });
        PrivateDependencyModuleNames.AddRange(new string[] {"Projects"});  
        PublicDefinitions.AddRange(new string[] { "ConvaiDebugMode=1" });

        // Target Platform Specific Settings
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            bUsePrecompiled = true;
        }

        if (Target.Platform == UnrealTargetPlatform.Android)
        {
            PublicDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "AndroidPermission" });
            string BuildPath = Utils.MakePathRelativeTo(ModuleDirectory, Target.RelativeEnginePath);
            AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(BuildPath, "Convai_AndroidAPL.xml"));
        }
        
        // ThirdParty Libraries
        AddEngineThirdPartyPrivateStaticDependencies(Target, "libOpus");
        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
        AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

        ConfigurePlatform(Target, Target.Configuration);
    }
}

public abstract class ConvaiPlatform
{
    public virtual string ConfigurationDir(UnrealTargetConfiguration Configuration)
    {
        if (Configuration == UnrealTargetConfiguration.Debug || Configuration == UnrealTargetConfiguration.DebugGame)
        {
            return "Debug/";
        }
        else
        {
            return "Release/";
        }
    }
    public abstract string LibrariesPath { get; }
    public abstract List<string> Architectures();
    public abstract string LibraryPrefixName { get; }
    public abstract string LibraryPostfixName { get; }
}

public class ConvaiPlatform_Win64 : ConvaiPlatform
{
    public override string LibrariesPath { get { return "win64/"; } }
    public override List<string> Architectures() { return new List<string> { "" }; }
    public override string LibraryPrefixName { get { return ""; } }
    public override string LibraryPostfixName { get { return ".lib"; } }
}

public class ConvaiPlatform_Android : ConvaiPlatform
{
    public override string LibrariesPath { get { return "android/"; } }
    public override List<string> Architectures() { return new List<string> { "armeabi-v7a/", "arm64-v8a/", "x86_64/" }; }
    public override string LibraryPrefixName { get { return "lib"; } }
    public override string LibraryPostfixName { get { return ".a"; } }
}

public class ConvaiPlatform_Mac : ConvaiPlatform
{
   public override string LibrariesPath { get { return "mac/"; } }
   public override List<string> Architectures() { return new List<string> { "" }; }
   public override string LibraryPrefixName { get { return "lib"; } }
   public override string LibraryPostfixName { get { return ".a"; } }
}


public class ConvaiPlatform_Linux : ConvaiPlatform
{
   public override string LibrariesPath { get { return "linux/"; } }
   public override List<string> Architectures() { return new List<string> { "" }; }
   public override string LibraryPrefixName { get { return "lib"; } }
   public override string LibraryPostfixName { get { return ".a"; } }
}

//public class ConvaiPlatform_PS5 : ConvaiPlatform
//{
//    public override string LibrariesPath { get { return "ps5/"; } }
//    public override List<string> Architectures() { return new List<string> { "" }; }
//    public override string LibraryPrefixName { get { return "lib"; } }
//    public override string LibraryPostfixName { get { return ".a"; } }
//}

//public class ConvaiPlatform_IOS : ConvaiPlatform
//{
//    public override string ConfigurationDir(UnrealTargetConfiguration Configuration)
//    {
//        return "";
//    }
//    public override string LibrariesPath { get { return "ios/"; } }
//    public override List<string> Architectures() { return new List<string> { "" }; }
//    public override string LibraryPrefixName { get { return "lib"; } }
//    public override string LibraryPostfixName { get { return ".a"; } }
//}
