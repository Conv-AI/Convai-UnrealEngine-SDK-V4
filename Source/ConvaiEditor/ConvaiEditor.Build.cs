using UnrealBuildTool;
using System.IO;

public class ConvaiEditor : ModuleRules
{
    public ConvaiEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

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
        
        // Include all editor resources for runtime access
        string PluginContentDir = Path.Combine(PluginDirectory, "Resources", "ConvaiEditor");
        RuntimeDependencies.Add(Path.Combine(PluginContentDir, "**"), StagedFileType.UFS);
    }
} 