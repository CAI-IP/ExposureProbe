#include "Modules/ModuleManager.h"
#include "ExposureProbeText.h"
#include "ExposureProbeTypes.h"

DEFINE_LOG_CATEGORY(LogExposureProbe);

class FExposureProbeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if WITH_EDITOR
		ExposureProbeText::ApplyEditorMetadata();
#endif
	}
};

IMPLEMENT_MODULE(FExposureProbeModule, ExposureProbe)
