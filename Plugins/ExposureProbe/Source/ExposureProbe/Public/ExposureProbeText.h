#pragma once

#include "CoreMinimal.h"

/** Chinese / English UI text. Chinese follows the editor language unless the settings force a language. */
namespace ExposureProbeText
{
	EXPOSUREPROBE_API bool IsChinese();
	EXPOSUREPROBE_API FText Get(const TCHAR* Zh, const TCHAR* En);
	EXPOSUREPROBE_API FString Str(const TCHAR* Zh, const TCHAR* En);
	EXPOSUREPROBE_API FText Format(const TCHAR* Zh, const TCHAR* En, const FFormatOrderedArguments& Args);

#if WITH_EDITOR
	/** Chinese display names, tooltips and categories for the plugin's classes, properties and enums. */
	EXPOSUREPROBE_API void ApplyEditorMetadata();
#endif
}

#define EPT(Zh, En) ExposureProbeText::Get(TEXT(Zh), TEXT(En))
#define EPS(Zh, En) ExposureProbeText::Str(TEXT(Zh), TEXT(En))
