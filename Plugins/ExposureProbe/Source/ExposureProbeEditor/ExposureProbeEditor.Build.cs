using UnrealBuildTool;

public class ExposureProbeEditor : ModuleRules
{
	public ExposureProbeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EditorSubsystem",
			"ExposureProbe",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"Slate",
			"SlateCore",
			"InputCore",
			"WorkspaceMenuStructure",
			"PlacementMode",
			"NavigationSystem",
			"RenderCore",
			"RHI",
			"PropertyEditor",
			"Settings",
			"Json",
			"JsonUtilities",
			"Projects",
			"ToolMenus",
			"LevelEditor",
			"MaterialEditor",
		});
	}
}
