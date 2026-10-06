// Copyright Convai Inc. All Rights Reserved.

using System.IO;
using EpicGames.Core;
using UnrealBuildTool;

public class ConvaiToolset : ModuleRules
{
	public ConvaiToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Editor-only module: it synthesizes Blueprint nodes, mutates SCS/CDOs, and
		// registers AI toolsets. None of that is valid in a packaged/runtime build.
		if (Target.Type != TargetRules.TargetType.Editor)
		{
			throw new BuildException("ConvaiToolset module should only be loaded in Editor builds!");
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"Kismet",
			"KismetCompiler",
			"BlueprintGraph",
			"Convai",
			"ConvaiEditor",
			"AIModule",
			"NavigationSystem",
			"EditorScriptingUtilities",
			"Slate",
			"SlateCore",
			"Projects",
		});

		// The engine ToolsetRegistry plugin (and the UToolsetDefinition base + the
		// AICallable reflection machinery) only exists on UE 5.8+. On 5.0-5.7 this
		// module links as an empty module, so we must NOT add the 5.8-only dependency
		// on older engines or the build will fail to resolve the module.
		bool bToolsets = (Target.Version.MajorVersion > 5)
			|| (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 8);
		if (bToolsets)
		{
			PrivateDependencyModuleNames.Add("ToolsetRegistry");
		}

		// The reflected toolset types (USTRUCT/UCLASS/UFUNCTION) derive from
		// UToolsetDefinition, which only exists on 5.8+. They CANNOT be hidden behind a
		// "#if CONVAI_TOOLSET_SUPPORTED" because UnrealHeaderTool rejects reflection macros
		// inside any preprocessor block other than WITH_EDITORONLY_DATA (enforced identically
		// on 5.0 through 5.8). So we gate them at the BUILD level instead: those files live in
		// the sibling "ConvaiToolsetGated" directory, which is OUTSIDE this module's auto-globbed
		// source tree and is therefore invisible to UBT/UHT by default. On 5.8+ we explicitly
		// fold it back into the module via ConditionalAddModuleDirectory, so the toolset compiles
		// normally; on 5.0-5.7 it is simply never seen and the module links empty. This is fully
		// deterministic (no build-time file mutation) and uniform across all engine versions.
		if (bToolsets)
		{
			DirectoryReference GatedDir = DirectoryReference.Combine(
				new DirectoryReference(ModuleDirectory), "..", "ConvaiToolsetGated");
			ConditionalAddModuleDirectory(GatedDir);
		}
	}
}
