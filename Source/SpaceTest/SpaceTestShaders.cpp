#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"      // хедер, НЕ модуль
#include "Interfaces/IPluginManager.h"

class FSpaceTestShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const FString ShaderDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Project"), ShaderDir);
	}

	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FSpaceTestShadersModule, SpaceTestShaders);
