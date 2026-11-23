#include "Modules/ModuleManager.h"

#include "ExposureProbeToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FExposureProbeMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UExposureProbeToolset::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized())
		{
			UToolsetRegistry::UnregisterToolsetClass(UExposureProbeToolset::StaticClass());
		}
	}
};

IMPLEMENT_MODULE(FExposureProbeMCPModule, ExposureProbeMCP)
