#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h" // это хедер, НЕ модуль

class FSpaceTestModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();

		const FString ShaderDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Project"), ShaderDir);
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FSpaceTestModule, SpaceTest, "SpaceTest");
