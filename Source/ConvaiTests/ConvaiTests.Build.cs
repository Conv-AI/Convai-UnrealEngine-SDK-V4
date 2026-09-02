// Copyright 2022 Convai Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

// The test framework, as a module a customer never receives. Per ADR-0005 it
// reaches the plugin only through extension points the plugin already offers
// third parties, so the shipping Convai binary is byte-identical to the one
// under test. push_to_public_v4.bat deletes this folder and its .uplugin entry
// at release (issue 12).
public class ConvaiTests : ModuleRules
{
    public ConvaiTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // ConvaiSubsystem.h reaches ConvaiConnectionSessionProxy.h, which
        // includes <convai/convai_client.h>. The Convai module resolves that
        // through its own third-party path rather than a public include path,
        // so a consumer of its public headers has to add the same one.
        PrivateIncludePaths.Add(
            Path.GetFullPath(Path.Combine(ModuleDirectory, "../ThirdParty/ConvaiWebRTC/include")));

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "Convai",
            }
            );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                // Reference Audio is captured off the master submix, so the
                // harness needs the mixer device directly rather than the
                // Blueprint wrapper alone.
                "AudioMixer",
                "AudioMixerCore",
                "SignalProcessing",
                // report.json, the JSONL event traces, and STT.json.
                "Json",
                // Locating Source/ConvaiTests/Data through the plugin's own
                // base dir, rather than guessing a path relative to the project.
                "Projects",
            }
            );
    }
}
