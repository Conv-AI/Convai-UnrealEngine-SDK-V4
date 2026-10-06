// Copyright Convai. All Rights Reserved.

using UnrealBuildTool;

public class ConvaiSceneTagging : ModuleRules
{
	public ConvaiSceneTagging(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		DefaultBuildSettings = BuildSettingsVersion.Latest;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"DeveloperSettings",
				"Engine",
				"ConvaiSceneAutoTaggerCore"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Convai",
				"ImageCore",
				"ImageWrapper",
				"Json",
				"Projects",
				"RenderCore",
				"RHI"
			}
			);

		if (Target.bBuildEditor)
		{
			// Authored thumbnail-orbit presentation hints only exist as editor data.
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "LevelEditor" });
		}
	}
}
