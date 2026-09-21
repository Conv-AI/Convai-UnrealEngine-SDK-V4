// Copyright Convai Inc. All Rights Reserved.
using UnrealBuildTool;

public class ConvaiCloudProjectsEditor : ModuleRules
{
	public ConvaiCloudProjectsEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		// Packages the project through UAT and edits the .uproject: neither is valid at runtime.
		if (Target.Type != TargetType.Editor) throw new BuildException("Cloud Projects is editor-only.");
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "HTTP", "Json" });

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Convai",          // UConvaiURL / UConvaiUtils: one base URL and one auth header for the whole SDK
			"ConvaiEditor",    // ConvaiEditorVisual: the palette and button styles Cloud Avatars uses
			"InputCore", "UnrealEd", "ToolMenus", "Projects", "DesktopPlatform", "AssetRegistry",
			"UATHelper",       // BuildCookRun, with the editor's own packaging notification
			"FileUtilities",   // FZipArchiveWriter
			"TargetPlatform",  // packaging-platform availability, so a missing toolchain is reported before the cook
			"JsonUtilities",
		});
	}
}
