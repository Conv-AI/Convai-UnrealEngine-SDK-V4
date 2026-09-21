// Copyright Convai Inc. All Rights Reserved.
using UnrealBuildTool;

public class ConvaiAvatarStudioEditor : ModuleRules
{
    public ConvaiAvatarStudioEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        if (Target.Type != TargetType.Editor) throw new BuildException("Avatar Studio is editor-only.");
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "HTTP", "Json" });
        if (Target.Platform == UnrealTargetPlatform.Win64) PublicSystemLibraries.Add("Advapi32.lib");
        PrivateDependencyModuleNames.AddRange(new[] { "BlueprintGraph", "NavigationSystem" });
        PrivateDependencyModuleNames.AddRange(new[] { "Convai", "ConvaiEditor", "InputCore", "UnrealEd", "ToolMenus", "LevelEditor", "Projects", "AssetTools", "AssetRegistry", "ContentBrowser", "DeveloperSettings", "DeveloperToolSettings", "JsonUtilities", "PropertyEditor", "DesktopPlatform", "ImageWrapper", "ImageCore", "FreeImage", "RenderCore", "RHI", "FileUtilities", "Settings", "Niagara", "TargetPlatform" });
    }
}
