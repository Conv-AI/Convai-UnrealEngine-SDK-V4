// Copyright Convai. All Rights Reserved.

using UnrealBuildTool;

public class ConvaiSceneTaggingEditor : ModuleRules
{
	public ConvaiSceneTaggingEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		if (Target.Type != TargetType.Editor)
		{
			throw new BuildException("ConvaiSceneTaggingEditor is editor-only.");
		}

		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		DefaultBuildSettings = BuildSettingsVersion.Latest;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"DeveloperSettings"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Convai",
				"ConvaiEditor",
				"ConvaiSceneTagging",
				"Engine",
				"FreeImage",
				"HTTP",
				"ImageCore",
				"ImageWrapper",
				"InputCore",
				"Json",
				"LevelEditor",
				"Projects",
				"RenderCore",
				"RHI",
				"Settings",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd"
			}
			);
	}
}
