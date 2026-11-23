using UnrealBuildTool;

public class ExposureProbeMCP : ModuleRules
{
	public ExposureProbeMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"Json",
			"JsonUtilities",
			"ToolsetRegistry",
			"ExposureProbe",
			"ExposureProbeEditor",
		});
	}
}
