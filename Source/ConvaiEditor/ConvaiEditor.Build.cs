using UnrealBuildTool;
using System.IO;

public class ConvaiEditor : ModuleRules
{
    public ConvaiEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Ensure this is editor-only module
        if (Target.Type != TargetRules.TargetType.Editor)
        {
            throw new BuildException("ConvaiEditor module should only be loaded in Editor builds!");
        }

        // Public dependencies
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "Slate",
            "SlateCore",
            "UnrealEd",
            "Json",
            "JsonUtilities",
            "WebBrowser",
            "HTTP",
            "Convai",
            "ApplicationCore",
            "UMGEditor",
            "Blutility"
        });

        // Private dependencies
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UMG",
            "EditorStyle",
            "LevelEditor",
            "Projects",
            "ToolMenus",
            "EditorFramework",
            "ImageWrapper",
            "HTTPServer",
            "Sockets",
            "Networking",
            "RHI",
            "RenderCore",
            "AssetTools",
            "EditorScriptingUtilities",
            "PropertyEditor",
            "DeveloperSettings",
            "ContentBrowser",
            "ContentBrowserData",
            "TextureEditor"
        });
        
        // NOTE: ConvaiEditor is an editor-only module and should NEVER be included in packaged builds.
        // RuntimeDependencies should NOT be added for editor modules as they are excluded from shipping builds.
    }
} 