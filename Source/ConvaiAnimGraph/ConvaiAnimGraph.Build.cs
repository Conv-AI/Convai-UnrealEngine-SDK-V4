using UnrealBuildTool;

public class ConvaiAnimGraph : ModuleRules
{
    public ConvaiAnimGraph(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AnimGraph",
            "BlueprintGraph",
            "AnimGraphRuntime",
            "Convai"
        });
    }
}
