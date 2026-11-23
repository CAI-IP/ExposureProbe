#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;
class STextBlock;
struct FExposureEdge;
struct FExposureEdgeChange;

/** Dockable panel: layer toggles, baseline comparison and report, changed pairs, floor analysis, all pairs. */
class SExposureProbePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SExposureProbePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SExposureProbePanel() override;

private:
	void Refresh();
	TSharedRef<SWidget> MakeToolbar();
	TSharedRef<SWidget> MakeBaselineBar();
	TSharedRef<SWidget> MakeBaselineMenu();
	TSharedRef<SWidget> MakeHeatSection();
	TSharedRef<SWidget> MakePairRow(const FExposureEdge& Edge, const FExposureEdgeChange* Change);
	TSharedRef<SWidget> MakeShareBar(const FExposureEdge& Edge);
	void RefreshHeat();

	TSharedPtr<STextBlock> SummaryText;
	TSharedPtr<STextBlock> StatsText;
	TSharedPtr<SVerticalBox> WarningBox;
	TSharedPtr<SVerticalBox> ChangedBox;
	TSharedPtr<SVerticalBox> EvidenceBox;
	TSharedPtr<SVerticalBox> AllBox;
	TSharedPtr<STextBlock> HeatStatus;
	TSharedPtr<SVerticalBox> HeatSpots;
	FDelegateHandle ReportHandle;
	FString NoteDraft;
	FString BaselineDraft;
	FString HeatMessage;
	FString ReportPath;
	FString SmokeMessage;
	TSharedRef<SWidget> MakeSmokeSection();
};
