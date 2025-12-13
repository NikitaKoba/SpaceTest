// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class SpaceTest : ModuleRules
{
	public SpaceTest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"ReplicationGraph",
			"IrisCore",
			"Renderer",
			"PhysicsCore",
			"ProceduralMeshComponent"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"NetCore",
			"RHI",
			"RenderCore",
			"Renderer",
			"Projects"
		});

		// Чтобы шейдеры попали в сборку/пакедж (важно для Shipping)
		RuntimeDependencies.Add("$(ProjectDir)/Shaders/PlanetHeightCS.usf");
		RuntimeDependencies.Add("$(ProjectDir)/Shaders/PlanetHeightMipCS.usf");
	}
}