#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class AExposureProbeActor;
class IDetailLayoutBuilder;

/**
 * Probe details: the shared body and units, and the probe's links to the other side with A and both shares,
 * each of which can be hidden from the viewport.
 */
class FExposureProbeDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual ~FExposureProbeDetails() override;

	virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override;
	virtual void CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& Builder) override;

private:
	TArray<FString> CollectPartners() const;
	AExposureProbeActor* FindPartner(const FString& Partner) const;
	/** Selected probes on the other side of Partner. */
	TArray<AExposureProbeActor*> Owners(const FString& Partner) const;
	void SetShown(const TArray<FString>& InPartners, bool bShown);
	void HandleReport();

	TArray<TWeakObjectPtr<AExposureProbeActor>> Probes;
	TArray<FString> Partners;
	TWeakPtr<IDetailLayoutBuilder> WeakBuilder;
	FDelegateHandle ReportHandle;
};
