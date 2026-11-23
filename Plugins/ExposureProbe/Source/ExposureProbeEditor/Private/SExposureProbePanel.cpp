#include "SExposureProbePanel.h"

#include "ExposureProbeSettings.h"
#include "ExposureProbeSubsystem.h"
#include "ExposureProbeText.h"
#include "ExposureProbeTypes.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ISettingsModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "UObject/UnrealType.h"

namespace ExposureProbePanel
{
	static FLinearColor TCol() { return GetDefault<UExposureProbeSettings>()->TColor; }
	static FLinearColor CTCol() { return GetDefault<UExposureProbeSettings>()->CTColor; }
	static const FLinearColor Alert = UExposureProbeSettings::AlertColor();

	static FString Regions(const TArray<int32>& Vis, const TArray<int32>& Valid)
	{
		if (Vis.Num() != 3 || Valid.Num() != 3) { return FString(); }
		return FString::Format(*EPS("头 {0}/{1} · 躯干 {2}/{3} · 腿 {4}/{5}", "head {0}/{1} · torso {2}/{3} · legs {4}/{5}"),
			{ Vis[2], Valid[2], Vis[1], Valid[1], Vis[0], Valid[0] });
	}

	static FString Lead(const FExposureEdge& E)
	{
		return FString::Printf(TEXT("A %+.2f"), E.A);
	}

	static TSharedRef<SWidget> IconButton(const FName& Icon, const FText& Tip, TFunction<void()> OnClick)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(Tip)
			.ContentPadding(FMargin(3, 2))
			.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
			[
				SNew(SImage).Image(FAppStyle::GetBrush(Icon)).ColorAndOpacity(FSlateColor::UseForeground())
			];
	}

	static TSharedRef<SWidget> TextButton(const FText& Label, TFunction<void()> OnClick, const FText& Tip = FText::GetEmpty())
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(Tip)
			.ContentPadding(FMargin(6, 2))
			.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(Label).Font(FAppStyle::GetFontStyle("SmallFont"))
			];
	}

	static TSharedRef<SWidget> PrimaryButton(const FText& Label, TFunction<void()> OnClick, const FText& Tip = FText::GetEmpty())
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.ToolTipText(Tip)
			.ContentPadding(FMargin(8, 2))
			.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(Label).Font(FAppStyle::GetFontStyle("SmallFont"))
			];
	}

	/** Toggle button bound to a getter and a setter, for mutually exclusive choices. */
	template <typename T>
	static TSharedRef<SWidget> Choice(const FText& Label, const FText& Tip, T Value, TFunction<T()> Get, TFunction<void(T)> Set, TFunction<bool()> Enabled = nullptr)
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.Padding(FMargin(8, 2))
			.ToolTipText(Tip)
			.IsEnabled_Lambda([Enabled]() { return !Enabled || Enabled(); })
			.IsChecked_Lambda([Get, Value]() { return Get() == Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Set, Value](ECheckBoxState) { Set(Value); })
			[
				SNew(STextBlock).Text(Label).Font(FAppStyle::GetFontStyle("SmallFont"))
			];
	}

	static UExposureProbeSubsystem* Subsys() { return UExposureProbeSubsystem::Get(); }
}

void SExposureProbePanel::Construct(const FArguments& InArgs)
{
	using namespace ExposureProbePanel;
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(0)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ MakeToolbar() ]
			+ SVerticalBox::Slot().AutoHeight()[ SNew(SSeparator).Thickness(1.f) ]
			+ SVerticalBox::Slot().AutoHeight()[ MakeBaselineBar() ]
			+ SVerticalBox::Slot().AutoHeight()[ SNew(SSeparator).Thickness(1.f) ]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot().Padding(FMargin(10, 8))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
					[
						SAssignNew(SummaryText, STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SAssignNew(StatsText, STextBlock)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(WarningBox, SVerticalBox) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
					[
						SNew(SExpandableArea)
						.AreaTitle_Lambda([]()
						{
							const UExposureProbeSubsystem* S = Subsys();
							return S && S->HasBaseline()
								? FText::Format(EPT("相对基线「{0}」变化的探针对", "Pairs changed against \"{0}\""), FText::FromString(UExposureProbeSubsystem::BaselineDisplayName(S->GetBaseline().Name)))
								: EPT("这次改动影响的探针对", "Pairs changed by the last edit");
						})
						.BorderImage(FAppStyle::GetBrush("NoBorder"))
						.BodyContent()[ SAssignNew(ChangedBox, SVerticalBox) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
					[
						SNew(SExpandableArea)
						.AreaTitle(EPT("地面分析：全图 A 与单向视线", "Floor analysis: full-map A and one-way sightlines"))
						.BorderImage(FAppStyle::GetBrush("NoBorder"))
						.BodyContent()[ MakeHeatSection() ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
					[
						SNew(SExpandableArea)
						.AreaTitle(EPT("烟雾：切断代价 CC 与恢复时间 RT", "Smoke: cut cost CC and recovery time RT"))
						.BorderImage(FAppStyle::GetBrush("NoBorder"))
						.BodyContent()[ MakeSmokeSection() ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
					[
						SNew(SExpandableArea)
						.AreaTitle(EPT("翻转的射线", "Flipped Rays"))
						.BorderImage(FAppStyle::GetBrush("NoBorder"))
						.InitiallyCollapsed(true)
						.BodyContent()[ SAssignNew(EvidenceBox, SVerticalBox) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
					[
						SNew(SExpandableArea)
						.AreaTitle(EPT("全部探针对", "All Pairs"))
						.BorderImage(FAppStyle::GetBrush("NoBorder"))
						.InitiallyCollapsed(true)
						.BodyContent()[ SAssignNew(AllBox, SVerticalBox) ]
					]
				]
			]
		]
	];

	if (UExposureProbeSubsystem* S = Subsys())
	{
		ReportHandle = S->OnReportUpdated.AddSP(this, &SExposureProbePanel::Refresh);
	}
	Refresh();
}

SExposureProbePanel::~SExposureProbePanel()
{
	if (UExposureProbeSubsystem* S = ExposureProbePanel::Subsys())
	{
		S->OnReportUpdated.Remove(ReportHandle);
	}
}

TSharedRef<SWidget> SExposureProbePanel::MakeToolbar()
{
	using namespace ExposureProbePanel;
	const TFunction<EExposureDrawStyle()> GetStyle = []() { const UExposureProbeSubsystem* S = Subsys(); return S ? S->GetDrawStyle() : EExposureDrawStyle::Body; };
	const TFunction<void(EExposureDrawStyle)> SetStyle = [](EExposureDrawStyle V) { if (UExposureProbeSubsystem* S = Subsys()) { S->SetDrawStyle(V); } };
	const TFunction<EExposureDrawMode()> GetMode = []() { const UExposureProbeSubsystem* S = Subsys(); return S ? S->GetDrawMode() : EExposureDrawMode::Changed; };
	const TFunction<void(EExposureDrawMode)> SetMode = [](EExposureDrawMode V) { if (UExposureProbeSubsystem* S = Subsys()) { S->SetFocusEdge(FString()); S->SetDrawMode(V); } };
	const TFunction<EExposureHeatShow()> GetShow = []() { const UExposureProbeSubsystem* S = Subsys(); return S ? S->GetHeatShow() : EExposureHeatShow::A; };
	const TFunction<void(EExposureHeatShow)> SetShow = [](EExposureHeatShow V) { if (UExposureProbeSubsystem* S = Subsys()) { S->SetHeatShow(V); } };
	const TFunction<bool()> ProbesOn = []() { const UExposureProbeSubsystem* S = Subsys(); return S && S->IsLayerOn(EExposureLayer::Probes); };
	const TFunction<bool()> HeatOn = []() { const UExposureProbeSubsystem* S = Subsys(); return S && S->IsLayerOn(EExposureLayer::Heatmap); };

	// Layers are independent: any of them can be on at the same time. Turning on a floor layer analyses the floor if that has not been done.
	auto Layer = [this](const FText& Label, const FText& Tip, EExposureLayer L, const FLinearColor& Dot)
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.Padding(FMargin(8, 2))
			.ToolTipText(Tip)
			.IsChecked_Lambda([L]() { const UExposureProbeSubsystem* S = Subsys(); return S && S->IsLayerOn(L) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, L](ECheckBoxState St)
			{
				UExposureProbeSubsystem* S = Subsys();
				if (!S) { return; }
				const bool bOn = St == ECheckBoxState::Checked;
				S->SetLayer(L, bOn);
				if (bOn && L != EExposureLayer::Probes && !S->HasHeat())
				{
					S->ComputeHeatmap(HeatMessage);
					if (!S->HasHeat()) { S->SetLayer(L, false); }
				}
				RefreshHeat();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 5, 0)
				[
					SNew(SBox).WidthOverride(8.f).HeightOverride(8.f)[ SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(Dot) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(Label).Font(FAppStyle::GetFontStyle("SmallFont"))
				]
			];
	};
	auto RowLabel = [](const FText& Label)
	{
		return SNew(SBox).MinDesiredWidth(34.f)
			[
				SNew(STextBlock).Text(Label).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
	};
	const UExposureProbeSettings* Set = GetDefault<UExposureProbeSettings>();

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
		.Padding(FMargin(6, 4))
		[
			SNew(SVerticalBox)
			// Layers.
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ RowLabel(EPT("显示", "Show")) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 2, 0)
				[
					Layer(EPT("探针", "Probes"), EPT("成对探针：谁看见谁、改动影响了哪些对", "Paired probes: who sees whom, and which pairs an edit changed"), EExposureLayer::Probes, Set->TColor)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 2, 0)
				[
					Layer(EPT("热力图", "Heatmap"), EPT("全图 A：蓝色看见对方更多，红色被看见更多", "Full-map A: blue sees more, red is seen more"), EExposureLayer::Heatmap, Set->HeatSeesColor)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					Layer(EPT("单向视线", "One-way"), EPT("地面格子：黄色格子单向看见其他位置（看得见对方，对方看不见自己）。选中探针时只显示与这些探针有关的格子",
						"Floor cells: yellow cells see other spots one-way (they see and are not seen). With probes selected, only the cells related to those probes"), EExposureLayer::OneWay, UExposureProbeSettings::PeekColor())
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					IconButton("Icons.FolderOpen", EPT("打开日志、基线与报告文件夹", "Open the log, baseline and report folder"), []()
					{
						const FString Dir = FPaths::ConvertRelativePathToFull(UExposureProbeSubsystem::LogDir());
						IFileManager::Get().MakeDirectory(*Dir, true);
						FPlatformProcess::ExploreFolder(*Dir);
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					IconButton("Icons.Settings", EPT("设置", "Settings"), []()
					{
						FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Project", "Plugins", "ExposureProbeSettings");
					})
				]
			]
			// Probes: compute, update, auto; style; what to draw.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ RowLabel(EPT("探针", "Probes")) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					PrimaryButton(EPT("全量计算", "Recompute"), []() { if (UExposureProbeSubsystem* S = Subsys()) { S->Rebuild(true); } })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
				[
					TextButton(EPT("立即更新", "Update"), []() { if (UExposureProbeSubsystem* S = Subsys()) { S->UpdateNow(); } })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([]() { const UExposureProbeSubsystem* S = Subsys(); return S && S->IsAutoUpdate() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([](ECheckBoxState St) { if (UExposureProbeSubsystem* S = Subsys()) { S->SetAutoUpdate(St == ECheckBoxState::Checked); } })
					[
						SNew(STextBlock).Text(EPT("自动", "Auto")).Font(FAppStyle::GetFontStyle("SmallFont"))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 2, 0)
				[
					Choice<EExposureDrawStyle>(EPT("身体", "Body"), EPT("每对一条连线，被看见的身体部位按可见比例着色", "One link per pair; seen body regions filled by visible share"), EExposureDrawStyle::Body, GetStyle, SetStyle, ProbesOn)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
				[
					Choice<EExposureDrawStyle>(EPT("射线", "Rays"), EPT("画出每条眼位到身体采样点的射线", "Every ray from the eye to the body sample points"), EExposureDrawStyle::Rays, GetStyle, SetStyle, ProbesOn)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 2, 0)
				[
					Choice<EExposureDrawMode>(EPT("关", "Off"), FText::GetEmpty(), EExposureDrawMode::Off, GetMode, SetMode, ProbesOn)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 2, 0)
				[
					Choice<EExposureDrawMode>(EPT("变化", "Changed"), EPT("只画这次改动（或相对基线）变化的探针对", "Only the pairs the last edit (or the baseline) changed"), EExposureDrawMode::Changed, GetMode, SetMode, ProbesOn)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					Choice<EExposureDrawMode>(EPT("全部", "All"), FText::GetEmpty(), EExposureDrawMode::All, GetMode, SetMode, ProbesOn)
				]
			]
			// Floor: analyse, clear; A or the change against the baseline.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ RowLabel(EPT("地面", "Floor")) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					PrimaryButton(EPT("分析地面", "Analyse Floor"), [this]()
					{
						if (UExposureProbeSubsystem* S = Subsys())
						{
							S->ComputeHeatmap(HeatMessage);
							RefreshHeat();
						}
					}, EPT("在可站立的地面上取样，一次计算全图 A 与单向视线", "Sample the standable floor and compute the full-map A and the one-way sightlines in one pass"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
				[
					TextButton(EPT("清除", "Clear"), [this]() { if (UExposureProbeSubsystem* S = Subsys()) { S->ClearHeatmap(); HeatMessage.Reset(); RefreshHeat(); } })
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 2, 0)
				[
					Choice<EExposureHeatShow>(EPT("A", "A"), EPT("每个位置的暴露不对称度", "Exposure asymmetry at each spot"), EExposureHeatShow::A, GetShow, SetShow, HeatOn)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					Choice<EExposureHeatShow>(EPT("ΔA", "ΔA"), EPT("相对基线的 A 变化；基线需保存有相同设置的地面分析", "Change of A against the baseline; the baseline needs a floor analysis on the same settings"),
						EExposureHeatShow::Delta, GetShow, SetShow, []() { const UExposureProbeSubsystem* S = Subsys(); return S && S->IsLayerOn(EExposureLayer::Heatmap) && S->CanShowHeatDelta(); })
				]
			]
		];
}

TSharedRef<SWidget> SExposureProbePanel::MakeBaselineMenu()
{
	using namespace ExposureProbePanel;
	FMenuBuilder Menu(true, nullptr);
	UExposureProbeSubsystem* S = Subsys();
	const TArray<FString> Names = S ? S->ListBaselines() : TArray<FString>();
	Menu.BeginSection(NAME_None, EPT("对比", "Compare"));
	Menu.AddMenuEntry(EPT("不对比", "None"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]() { FString E; if (UExposureProbeSubsystem* X = Subsys()) { X->SetActiveBaseline(FString(), E); } }),
			FCanExecuteAction(), FIsActionChecked::CreateLambda([]() { const UExposureProbeSubsystem* X = Subsys(); return X && !X->HasBaseline(); })),
		NAME_None, EUserInterfaceActionType::RadioButton);
	for (const FString& Name : Names)
	{
		Menu.AddMenuEntry(FText::FromString(UExposureProbeSubsystem::BaselineDisplayName(Name)), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Name]()
				{
					FString E;
					if (UExposureProbeSubsystem* X = Subsys()) { if (!X->SetActiveBaseline(Name, E)) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *E); } }
				}),
				FCanExecuteAction(), FIsActionChecked::CreateLambda([Name]() { const UExposureProbeSubsystem* X = Subsys(); return X && X->HasBaseline() && X->GetBaseline().Name == Name; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	}
	Menu.EndSection();
	if (Names.Num() > 0)
	{
		Menu.BeginSection(NAME_None, EPT("删除", "Delete"));
		for (const FString& Name : Names)
		{
			Menu.AddMenuEntry(FText::FromString(UExposureProbeSubsystem::BaselineDisplayName(Name)), EPT("删除这个基线文件", "Delete this baseline file"), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
				FUIAction(FExecuteAction::CreateLambda([Name]() { if (UExposureProbeSubsystem* X = Subsys()) { X->DeleteBaseline(Name); } })));
		}
		Menu.EndSection();
	}
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SExposureProbePanel::MakeBaselineBar()
{
	using namespace ExposureProbePanel;
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.Padding(FMargin(10, 6))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(STextBlock).Text(EPT("基线", "Baseline")).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SComboButton)
					.ToolTipText(EPT("选择一个已保存的版本与当前关卡对比。每次保存关卡时自动保存「上次保存」", "Pick a saved version to compare against. Every level save stores \"Last save\""))
					.OnGetMenuContent(this, &SExposureProbePanel::MakeBaselineMenu)
					.ButtonContent()
					[
						SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont"))
						.Text_Lambda([]()
						{
							const UExposureProbeSubsystem* S = Subsys();
							return S && S->HasBaseline() ? FText::FromString(UExposureProbeSubsystem::BaselineDisplayName(S->GetBaseline().Name)) : EPT("不对比", "None");
						})
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(10, 0, 4, 0)
				[
					SNew(SEditableTextBox)
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.HintText(EPT("版本名，如 试玩 v3", "Version name, e.g. Playtest 3"))
					.Text_Lambda([this]() { return FText::FromString(BaselineDraft); })
					.OnTextChanged_Lambda([this](const FText& T) { BaselineDraft = T.ToString(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					TextButton(EPT("存为基线", "Save Baseline"), [this]()
					{
						if (UExposureProbeSubsystem* S = Subsys())
						{
							FString Error;
							if (S->SaveBaseline(BaselineDraft, Error)) { BaselineDraft.Reset(); }
							else { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Error); }
						}
					}, EPT("保存探针、设置、几何指纹与结果；地面分析为最新时一并保存", "Save probes, settings, geometry fingerprint and results, and the floor analysis when it is up to date"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2, 0, 0, 0)
				[
					TextButton(EPT("导出报告", "Export Report"), [this]()
					{
						if (UExposureProbeSubsystem* S = Subsys())
						{
							FString Error;
							ReportPath.Reset();
							if (!S->ExportReport(ReportPath, Error)) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Error); }
						}
					}, EPT("将相对基线的变化导出为 Markdown 与 JSON；未选基线时相对上一次改动", "Write the changes against the baseline, or the last edit when none is active, as Markdown and JSON"))
				]
			]
			// What changed against the baseline, in one line, then whether the inputs are the same.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.AutoWrapText(true)
				.Visibility_Lambda([]() { const UExposureProbeSubsystem* S = Subsys(); return S && S->HasBaseline() ? EVisibility::Visible : EVisibility::Collapsed; })
				.ColorAndOpacity_Lambda([]()
				{
					const UExposureProbeSubsystem* S = Subsys();
					bool bNew = false;
					if (S) { for (const FExposureEdgeChange& C : S->GetBaselineChanges()) { bNew |= C.bNewOneWay && !C.bExpected; } }
					return bNew ? FSlateColor(Alert) : FSlateColor::UseForeground();
				})
				.Text_Lambda([]()
				{
					const UExposureProbeSubsystem* S = Subsys();
					if (!S || !S->HasBaseline()) { return FText::GetEmpty(); }
					int32 NewOneWay = 0, TowardT = 0, TowardCT = 0, Far = 0, Expected = 0;
					for (const FExposureEdgeChange& C : S->GetBaselineChanges())
					{
						NewOneWay += C.bNewOneWay ? 1 : 0;
						Far += C.bFar ? 1 : 0;
						Expected += C.bExpected ? 1 : 0;
						if (C.bHadBefore && C.After.A - C.Before.A > 0.005f) { ++TowardT; }
						if (C.bHadBefore && C.After.A - C.Before.A < -0.005f) { ++TowardCT; }
					}
					FString Str = FString::Format(*EPS("改了 {0} 个物体 · {1} 对变化（偏 T {2} / 偏 CT {3}）· 新单向 {4} · 已消除 {5}",
						"{0} objects changed · {1} pairs changed ({2} toward T / {3} toward CT) · {4} new one-way · {5} resolved"),
						{ S->GetBaselineGeomChanges().Num(), S->GetBaselineChanges().Num(), TowardT, TowardCT, NewOneWay, S->GetBaselineResolved() });
					if (Far > 0) { Str += FString::Format(*EPS(" · 远处 {0}", " · {0} far"), { Far }); }
					if (Expected > 0) { Str += FString::Format(*EPS(" · 预期 {0}", " · {0} expected"), { Expected }); }
					if (S->GetBaselineMissing() > 0) { Str += FString::Format(*EPS(" · {0} 对的探针已删除", " · {0} pairs lost a probe"), { S->GetBaselineMissing() }); }
					return FText::FromString(Str);
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.AutoWrapText(true)
				.Visibility_Lambda([]() { const UExposureProbeSubsystem* S = Subsys(); return S && S->HasBaseline() ? EVisibility::Visible : EVisibility::Collapsed; })
				.ColorAndOpacity_Lambda([]()
				{
					const UExposureProbeSubsystem* S = Subsys();
					return S && S->IsBaselineReproMismatch() ? FSlateColor(Alert) : FSlateColor::UseSubduedForeground();
				})
				.Text_Lambda([]()
				{
					const UExposureProbeSubsystem* S = Subsys();
					if (!S || !S->HasBaseline()) { return FText::GetEmpty(); }
					if (S->IsBaselineReproMismatch()) { return EPT("复现检查未通过：输入与基线相同，结果不同", "Reproducibility check failed: same inputs as the baseline, different results"); }
					if (S->BaselineInputsMatch()) { return EPT("复现检查通过：输入与基线相同，结果一致", "Reproducibility check passed: same inputs, same results"); }
					FString Str = EPS("与基线不同的输入：", "Inputs that differ: ") + S->GetBaselineInputDiff();
					if (!S->IsBaselineComparable()) { Str += EPS("（身体或采样不同，点数不能直接比较）", " (body or sampling differ; counts do not compare directly)"); }
					return FText::FromString(Str);
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]() { return ReportPath.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this]() { return FText::Format(EPT("报告已导出：{0}", "Report written: {0}"), FText::FromString(FPaths::GetCleanFilename(ReportPath))); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					TextButton(EPT("打开", "Open"), [this]() { FPlatformProcess::LaunchFileInDefaultExternalApplication(*ReportPath); })
				]
			]
		];
}

TSharedRef<SWidget> SExposureProbePanel::MakeSmokeSection()
{
	using namespace ExposureProbePanel;
	auto Settings = []() { return GetMutableDefault<UExposureProbeSettings>(); };
	// Change a setting the way the details panel does, so presets apply and the smokes redraw.
	auto Change = [](FName Name, TFunction<void(UExposureProbeSettings*)> Apply)
	{
		UExposureProbeSettings* S = GetMutableDefault<UExposureProbeSettings>();
		FProperty* P = FindFProperty<FProperty>(UExposureProbeSettings::StaticClass(), Name);
		S->PreEditChange(P);
		Apply(S);
		FPropertyChangedEvent Event(P, EPropertyChangeType::ValueSet);
		// Applies the preset, saves the setting and tells the subsystem.
		S->PostEditChangeProperty(Event);
	};
	auto PresetName = [](EExposureSmokePreset P) -> FText
	{
		switch (P)
		{
		case EExposureSmokePreset::CS2: return EPT("CS2 烟雾弹", "CS2 smoke grenade");
		case EExposureSmokePreset::ValorantBrimstone: return EPT("无畏契约 Brimstone", "Valorant Brimstone");
		case EExposureSmokePreset::ValorantOmen: return EPT("无畏契约 Omen", "Valorant Omen");
		case EExposureSmokePreset::ValorantAstra: return EPT("无畏契约 Astra", "Valorant Astra");
		default: return EPT("自定义", "Custom");
		}
	};
	auto Message = [this](bool bOk, const FString& M) { SmokeMessage = bOk ? FString() : M; };
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				PrimaryButton(EPT("放置烟雾", "Place Smoke"), [Message]()
				{
					if (UExposureProbeSubsystem* S = Subsys()) { FString M; Message(S->PlaceSmokeAtView(M) != nullptr, M); }
				}, EPT("放在视口中心所指的位置，并吸附到最近的地面格子。也可从放置面板的「暴露探针」分类拖入「烟雾」。移动或 Alt 拖动复制后同样吸附",
					"At the point under the viewport centre, snapped to the nearest floor cell. A Smoke can also be dragged in from the Exposure Probe category of the Place panel. Moved and Alt-dragged copies snap too"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
			[
				PrimaryButton(EPT("评估已放置的烟雾", "Evaluate Placed Smoke"), [Message]()
				{
					if (UExposureProbeSubsystem* S = Subsys()) { FString M; Message(S->EvaluatePlacedSmokes(FString(), M), M); }
				}, EPT("针对选中探针：烟雾遮挡其对方视线的比例、每个烟雾的贡献与 RT。拖动烟雾后自动更新",
					"For the selected probe: the share of its sightlines to the other side the smoke cuts, what each smoke adds, and RT. Updates when a smoke is dragged"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
			[
				TextButton(EPT("计算烟雾方案", "Compute Smoke Plan"), [Message]()
				{
					if (UExposureProbeSubsystem* S = Subsys()) { FString M; Message(S->ComputeSmoke(FString(), M), M); }
				}, EPT("在已分析的地面格子中求遮挡选中探针视线所需的最少烟雾数 CC，不需要比赛数据", "The fewest smokes on the analysed floor cells that cut the selected probe's sightlines (CC); no match data is needed"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				TextButton(EPT("将方案转为放置的烟雾", "Convert Plan to Placed Smoke"), [Message]()
				{
					if (UExposureProbeSubsystem* S = Subsys()) { FString M; Message(S->PlacePlanSmokes(M) > 0, M); }
				}, EPT("方案中的烟雾变为关卡中可拖动、可调尺寸的烟雾", "The plan's smokes become level actors you can drag and resize"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
			[
				TextButton(EPT("清除结果", "Clear Result"), [this]() { SmokeMessage.Reset(); if (UExposureProbeSubsystem* S = Subsys()) { S->ClearSmoke(); } },
					EPT("清除视线与信息卡；已放置的烟雾保留", "Clears the sightlines and cards; placed smoke stays"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
			[
				TextButton(EPT("删除全部烟雾", "Delete All Smoke"), [this]() { SmokeMessage.Reset(); if (UExposureProbeSubsystem* S = Subsys()) { S->ClearPlacedSmokes(); } })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).Text(EPT("烟雾尺寸", "Smoke size"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ContentPadding(FMargin(6, 1))
				.ToolTipText(EPT("统一尺寸，用于烟雾方案和尺寸设为「统一」的烟雾。单个烟雾可在细节面板中选择其他游戏或自定义尺寸",
					"Shared size, used by the smoke plan and by smokes set to Shared. A single smoke can take another game or custom dimensions in its details"))
				.ButtonContent()
				[
					SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).Text_Lambda([Settings, PresetName]() { return PresetName(Settings()->SmokePreset); })
				]
				.OnGetMenuContent_Lambda([Change, PresetName]()
				{
					FMenuBuilder Menu(true, nullptr);
					for (EExposureSmokePreset P : { EExposureSmokePreset::CS2, EExposureSmokePreset::ValorantBrimstone, EExposureSmokePreset::ValorantOmen, EExposureSmokePreset::ValorantAstra })
					{
						Menu.AddMenuEntry(PresetName(P), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([Change, P]()
						{
							Change(GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, SmokePreset), [P](UExposureProbeSettings* S) { S->SmokePreset = P; });
						})));
					}
					Menu.AddMenuEntry(EPT("自定义尺寸…（插件设置）", "Custom size... (plugin settings)"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]()
					{
						FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Project", "Plugins", "ExposureProbeSettings");
					})));
					return Menu.MakeWidget();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([Settings]()
				{
					const UExposureProbeSettings* S = Settings();
					return FText::FromString(FString::Format(*EPS("半径 {0} · 球心高 {1} 单位 · {2} 秒", "radius {0} · centre {1} units up · {2} s"),
						{ FString::Printf(TEXT("%g"), S->SmokeRadius), FString::Printf(TEXT("%g"), S->SmokeLift), FString::Printf(TEXT("%.2f"), S->SmokeDuration) }));
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).Text(EPT("显示样式", "Style"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				Choice<EExposureSmokeStyle>(EPT("体积烟", "Volume"), EPT("光线步进的体积烟，带 3D 噪声，在墙面与地面处截断；首次使用时编译材质，编译期间以烟团显示",
					"Ray-marched volume with 3D noise, cut off at walls and floors; the material compiles on first use and puffs are shown until it is ready"), EExposureSmokeStyle::Volume,
					[Settings]() { return Settings()->SmokeStyle; }, [Change](EExposureSmokeStyle V) { Change(GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, SmokeStyle), [V](UExposureProbeSettings* S) { S->SmokeStyle = V; }); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2, 0, 0, 0)
			[
				Choice<EExposureSmokeStyle>(EPT("烟团", "Puffs"), EPT("半透明球体组成的烟团", "Clusters of translucent spheres"), EExposureSmokeStyle::Puffs,
					[Settings]() { return Settings()->SmokeStyle; }, [Change](EExposureSmokeStyle V) { Change(GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, SmokeStyle), [V](UExposureProbeSettings* S) { S->SmokeStyle = V; }); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([]()
				{
					const UExposureProbeSubsystem* S = Subsys();
					const int32 N = S ? S->GetPlacedSmokeCount() : 0;
					return N > 0 ? FText::FromString(FString::Format(*EPS("已放置烟雾 {0} 个", "{0} smoke(s) placed"), { N })) : FText::GetEmpty();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.AutoWrapText(true)
			.ColorAndOpacity_Lambda([]() { const UExposureProbeSubsystem* S = Subsys(); return S && S->IsSmokeStale() ? FSlateColor(Alert) : FSlateColor::UseSubduedForeground(); })
			.Text_Lambda([this]()
			{
				const UExposureProbeSubsystem* S = Subsys();
				if (!SmokeMessage.IsEmpty()) { return FText::FromString(SmokeMessage); }
				if (S && S->HasSmoke()) { return FText::FromString(S->SmokeSummary()); }
				return EPT("用「放置烟雾」或从放置面板拖入「烟雾」，烟雾吸附到地面分析的格子，可移动、复制并在细节面板中调整尺寸。放置的烟雾遮挡探针射线，探针连线、身体着色与 A 随之更新。选中探针后点「评估已放置的烟雾」，得到遮挡比例与 RT。「计算烟雾方案」求最少烟雾数 CC。灰色为被遮挡的视线，红色为仍然可见的视线。",
					"Place smoke with Place Smoke or by dragging a Smoke in from the Place panel; it snaps to the floor cells and can be moved, copied and resized in its details. Placed smoke blocks the probe rays, so the links, body fills and A update with it. Select a probe and press Evaluate Placed Smoke for the cut share and RT. Compute Smoke Plan finds the fewest smokes (CC). Grey sightlines are cut, red ones stay visible.");
			})
		];
}

TSharedRef<SWidget> SExposureProbePanel::MakeHeatSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 6)
		[
			SAssignNew(HeatStatus, STextBlock)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(HeatSpots, SVerticalBox)
		];
}

void SExposureProbePanel::RefreshHeat()
{
	using namespace ExposureProbePanel;
	const UExposureProbeSubsystem* S = Subsys();
	if (!S || !HeatStatus.IsValid())
	{
		return;
	}
	const UExposureProbeSettings* Set = GetDefault<UExposureProbeSettings>();
	FString Status;
	if (!S->HasHeat())
	{
		Status = EPS("点「分析地面」或打开「热力图」「单向视线」图层。地面按网格取样，一次计算全图 A（蓝色看见对方更多，红色被看见更多）与单向视线（黄色格子看得见别处，别处看不见它）。鼠标悬停在地面上可读取数值。",
			"Press Analyse Floor or turn on the Heatmap or One-way layer. The floor is sampled on a grid and both are computed in one pass: the full-map A (blue sees more, red is seen more) and one-way sightlines (yellow cells see others who cannot see them). Hover the floor to read values.");
	}
	else
	{
		Status = FString::Format(*EPS("{0} 个采样点 · 间距 {1} · 邻域半径 {2} · {3} 处单向视线位置 · {4} 计算，用时 {5} 秒",
			"{0} samples · spacing {1} · radius {2} · {3} one-way spots · computed {4} in {5} s"),
			{ S->GetHeat().Num(), FString::Printf(TEXT("%.0f"), S->GetHeatSpacingCm() / Set->UnitToCm), FString::Printf(TEXT("%.0f"), S->GetHeatRadiusCm() / Set->UnitToCm),
			  S->GetOneWaySpotCount(), S->GetHeatTime(), FString::Printf(TEXT("%.1f"), S->GetHeatSeconds()) });
		if (S->IsHeatStale()) { Status += TEXT("\n") + EPS("关卡或身体参数已改动，需重新分析", "The level or body changed; analyse again"); }
	}
	if (!HeatMessage.IsEmpty() && !S->HasHeat()) { Status = HeatMessage + TEXT("\n") + Status; }
	HeatStatus->SetText(FText::FromString(Status));
	HeatStatus->SetColorAndOpacity(S->IsHeatStale() ? FSlateColor(Alert) : FSlateColor::UseSubduedForeground());

	HeatSpots->ClearChildren();
	if (!S->HasHeat())
	{
		return;
	}
	const double MinD2 = FMath::Square(4.0 * S->GetHeatSpacingCm());
	// Best first, at least four grid steps apart so one area does not fill the list.
	auto Spread = [S, MinD2](TArray<int32> Sorted, int32 Max)
	{
		TArray<int32> Out;
		for (const int32 i : Sorted)
		{
			if (Out.Num() >= Max) { break; }
			if (!Out.ContainsByPredicate([&](int32 j) { return FVector::DistSquared(S->GetHeat()[i].Feet, S->GetHeat()[j].Feet) < MinD2; })) { Out.Add(i); }
		}
		return Out;
	};
	auto Title = [this](const FText& T)
	{
		HeatSpots->AddSlot().AutoHeight().Padding(0, 6, 0, 4)[ SNew(STextBlock).Text(T).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)) ];
	};
	auto None = [this](const FText& T)
	{
		HeatSpots->AddSlot().AutoHeight()[ SNew(STextBlock).Text(T).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground()).AutoWrapText(true) ];
	};
	auto Row = [this](int32 i, EExposureLayer Layer, const FLinearColor& Col, const FString& Head, const FString& Sub, bool bNew)
	{
		HeatSpots->AddSlot().AutoHeight().Padding(0, 0, 0, 3)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(8, 4))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Fill).Padding(0, 0, 8, 0)
				[
					SNew(SBox).WidthOverride(3.f)[ SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(Col) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
				[
					SNew(STextBlock).Text(FText::FromString(Head)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).MinDesiredWidth(70)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(Sub)).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground()).AutoWrapText(true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0)
				[
					SNew(STextBlock).Text(EPT("新", "new")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8)).ColorAndOpacity(Alert)
					.Visibility(bNew ? EVisibility::Visible : EVisibility::Collapsed)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					IconButton("Icons.Visible", EPT("在视口中定位", "Show in the viewport"), [i, Layer]() { if (UExposureProbeSubsystem* X = Subsys()) { X->FocusViewportOnHeat(i, Layer); } })
				]
			]
		];
	};
	const TArray<FExposureHeatPoint>& Heat = S->GetHeat();

	if (S->IsLayerOn(EExposureLayer::OneWay))
	{
		TArray<int32> Order;
		if (S->HasProbeOneWay())
		{
			// Cells that can peek the selected probes one-way, most probes first.
			Title(FText::Format(EPT("单向看见 {0} 的位置", "Spots that see {0} one-way"), FText::FromString(FString::Join(S->GetProbeOneWayNames(), TEXT(", ")))));
			for (int32 i = 0; i < Heat.Num(); ++i) { if (S->GetCellThreatens(i).Num() > 0) { Order.Add(i); } }
			Order.Sort([S, &Heat](int32 L, int32 R)
			{
				const int32 A = S->GetCellThreatens(L).Num(), B = S->GetCellThreatens(R).Num();
				return A != B ? A > B : Heat[L].OneWaySees > Heat[R].OneWaySees;
			});
			const TArray<int32> Spots = Spread(Order, 6);
			if (Spots.Num() == 0) { None(EPT("无：没有位置单向看见选中探针", "None: no spot sees the selection one-way")); }
			for (const int32 i : Spots)
			{
				TArray<FString> Names;
				for (const int32 k : S->GetCellThreatens(i)) { Names.Add(S->GetProbeOneWayNames()[k]); }
				Row(i, EExposureLayer::OneWay, UExposureProbeSettings::PeekColor(), FString::Join(Names, TEXT(", ")),
					FString::Format(*EPS("此处看得见它，它看不见此处{0}", "sees it, it cannot see here{0}"), { Heat[i].bCrouch ? EPS(" · 蹲", " · crouch") : FString() }), false);
			}
			None(FText::Format(EPT("另有 {0} 个格子被选中探针单向看见（深色）", "{0} more cells are seen one-way by the selection (dark)"), FText::AsNumber(S->GetAdvantageCellCount())));
		}
		else
		{
			Title(EPT("单向视线最多的位置", "Strongest one-way spots"));
			for (int32 i = 0; i < Heat.Num(); ++i) { if (Heat[i].OneWaySees > 0) { Order.Add(i); } }
			Order.Sort([S, &Heat](int32 L, int32 R)
			{
				const bool NL = S->IsNewOneWaySpot(L), NR = S->IsNewOneWaySpot(R);
				return NL != NR ? NL : Heat[L].OneWaySees > Heat[R].OneWaySees;
			});
			const TArray<int32> Spots = Spread(Order, 6);
			if (Spots.Num() == 0) { None(EPT("没有单向视线", "No one-way sightlines")); }
			for (const int32 i : Spots)
			{
				Row(i, EExposureLayer::OneWay, UExposureProbeSettings::PeekColor(), FString::Format(*EPS("单向看见 {0} 处", "sees {0}"), { Heat[i].OneWaySees }),
					FString::Format(*EPS("被 {0} 处单向看见{1}", "seen by {0}{1}"), { Heat[i].OneWaySeenBy, Heat[i].bCrouch ? EPS(" · 蹲", " · crouch") : FString() }), S->IsNewOneWaySpot(i));
			}
			None(EPT("选中探针后，格子改为显示与这些探针的关系：黄色格子单向看见它们，深色格子被它们单向看见。", "Select probes to colour the cells by their relation to them: yellow sees them one-way, dark is seen by them one-way."));
		}
	}

	if (S->IsLayerOn(EExposureLayer::Heatmap))
	{
		TArray<int32> Order;
		for (int32 i = 0; i < Heat.Num(); ++i) { float V; if (S->HeatValue(i, V)) { Order.Add(i); } }
		const bool bDelta = S->GetHeatShow() == EExposureHeatShow::Delta;
		auto Section = [&](bool bPositive, const FText& T, const FLinearColor& Col)
		{
			Title(T);
			TArray<int32> Sorted = Order.FilterByPredicate([S, bPositive](int32 i) { float V = 0; S->HeatValue(i, V); return bPositive ? V > 0.005f : V < -0.005f; });
			Sorted.Sort([S, bPositive](int32 L, int32 R) { float A = 0, B = 0; S->HeatValue(L, A); S->HeatValue(R, B); return bPositive ? A > B : A < B; });
			const TArray<int32> Spots = Spread(Sorted, 5);
			if (Spots.Num() == 0) { None(EPT("没有", "None")); }
			for (const int32 i : Spots)
			{
				const FExposureHeatPoint& P = Heat[i];
				float V = 0.f;
				S->HeatValue(i, V);
				Row(i, EExposureLayer::Heatmap, Col, bDelta ? FString::Printf(TEXT("ΔA %+.2f"), V) : FString::Printf(TEXT("A %+.2f"), V),
					FString::Format(*EPS("看见 {0}% · 被看见 {1}%{2}", "sees {0}% · seen {1}%{2}"),
						{ FMath::RoundToInt(P.Vout * 100.f), FMath::RoundToInt(P.Vin * 100.f), P.bCrouch ? EPS(" · 蹲", " · crouch") : FString() }), false);
			}
		};
		Section(true, bDelta ? EPT("相对基线改善最多的位置", "Spots that gained the most") : EPT("看见多于被看见的位置", "Spots that see more than they are seen"), Set->HeatSeesColor);
		Section(false, bDelta ? EPT("相对基线变差最多的位置", "Spots that lost the most") : EPT("被看见多于看见的位置", "Spots seen more than they see"), Set->HeatSeenColor);
	}
	if (!S->IsLayerOn(EExposureLayer::OneWay) && !S->IsLayerOn(EExposureLayer::Heatmap))
	{
		None(EPT("打开「热力图」或「单向视线」图层以查看结果。", "Turn on the Heatmap or One-way layer to see the results."));
	}
}

TSharedRef<SWidget> SExposureProbePanel::MakeShareBar(const FExposureEdge& E)
{
	using namespace ExposureProbePanel;
	// Two bars back to back: what T sees of CT (left, T colour) and what CT sees of T (right, CT colour).
	auto Bar = [](float Share, const FLinearColor& Col, bool bRight)
	{
		const float W = 90.f;
		return SNew(SBox).WidthOverride(W).HeightOverride(6.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()[ SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(FLinearColor(1, 1, 1, 0.07f)) ]
				+ SOverlay::Slot().HAlign(bRight ? HAlign_Left : HAlign_Right)
				[
					SNew(SBox).WidthOverride(FMath::Clamp(Share, 0.f, 1.f) * W)
					[
						SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(Col)
					]
				]
			];
	};
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
		[
			SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%d/%d"), E.VisibleTtoCT, E.ValidCT)))
			.Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(TCol()).MinDesiredWidth(34).Justification(ETextJustify::Right)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ Bar(E.ShareTtoCT, TCol(), false) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1, 0)[ SNew(SBox).WidthOverride(1).HeightOverride(10)[ SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(FLinearColor(1, 1, 1, 0.4f)) ] ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ Bar(E.ShareCTtoT, CTCol(), true) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
		[
			SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%d/%d"), E.VisibleCTtoT, E.ValidT)))
			.Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(CTCol()).MinDesiredWidth(34)
		];
}

TSharedRef<SWidget> SExposureProbePanel::MakePairRow(const FExposureEdge& E, const FExposureEdgeChange* Change)
{
	using namespace ExposureProbePanel;
	const FString Name = E.Edge;
	const FString TName = E.TProbe, CTName = E.CTProbe;
	const bool bAlert = E.bOneWay && !E.bDesignIntent;

	FString Status;
	if (E.bOneWay) { Status = E.bDesignIntent ? EPS("单向 · 设计保留", "one-way · intent") : EPS("单向视线", "one-way"); }
	else if (E.bDesignIntent) { Status = EPS("设计保留", "intent"); }
	if (Change && Change->Kind != EExposureChangeKind::Minor && !(Change->Kind == EExposureChangeKind::NewOneWay && E.bOneWay))
	{
		Status += (Status.IsEmpty() ? TEXT("") : TEXT("  ")) + UExposureProbeSubsystem::ChangeKindText(Change->Kind);
	}
	if (Change && Change->bExpected && !E.bDesignIntent)
	{
		Status += (Status.IsEmpty() ? TEXT("") : TEXT("  ")) + EPS("预期", "expected");
	}
	if (Change && Change->bFar)
	{
		Status += (Status.IsEmpty() ? TEXT("") : TEXT("  ")) + FString::Format(*EPS("远处 · 距改动 {0}", "far · {0} from edit"), { FString::Printf(TEXT("%.1f m"), Change->DistanceCm / 100.f) });
	}

	FString Sub = FString::Format(*EPS("T 看 CT：{0}    CT 看 T：{1}", "T sees CT: {0}    CT sees T: {1}"),
		{ Regions(E.RegionVisibleTtoCT, E.RegionValidCT), Regions(E.RegionVisibleCTtoT, E.RegionValidT) });
	if (Change && Change->bHadBefore)
	{
		Sub = FString::Format(*EPS("之前 T {0}/{1} · CT {2}/{3}\n", "before T {0}/{1} · CT {2}/{3}\n"),
			{ Change->Before.VisibleTtoCT, Change->Before.ValidCT, Change->Before.VisibleCTtoT, Change->Before.ValidT }) + Sub;
	}
	else if (Change)
	{
		Sub = EPS("新的探针对\n", "new pair\n") + Sub;
	}
	if (!E.DesignNote.IsEmpty()) { Sub += TEXT("\n") + E.DesignNote; }
	const FString LeadText = Change && Change->bHadBefore ? FString::Printf(TEXT("A %+.2f → %+.2f"), Change->Before.A, E.A) : Lead(E);

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(8, 6))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
				[
					SNew(STextBlock).Text(FText::FromString(Name)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).MinDesiredWidth(70)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ MakeShareBar(E) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
				[
					SNew(STextBlock).Text(FText::FromString(LeadText)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).MinDesiredWidth(58)
					.ToolTipText(EPT("A = T 看见 CT 的身体比例 − CT 看见 T 的身体比例；正值 T 占优，负值 CT 占优",
						"A = share of CT's body visible to T minus share of T's body visible to CT; positive favours T"))
					.ColorAndOpacity(FMath::Abs(E.A) < 0.005f ? FSlateColor::UseSubduedForeground() : FSlateColor(E.A > 0 ? TCol() : CTCol()))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
				[
					SNew(STextBlock).Text(FText::FromString(Status)).Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(bAlert ? FSlateColor(Alert) : FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					IconButton("Icons.Visible", EPT("在视口中只看这一对", "Show only this pair"), [Name]()
					{
						if (UExposureProbeSubsystem* S = Subsys())
						{
							if (!S->IsLayerOn(EExposureLayer::Probes)) { S->SetLayer(EExposureLayer::Probes, true); }
							S->SetFocusEdge(S->GetFocusEdge() == Name ? FString() : Name);
							if (S->GetDrawMode() == EExposureDrawMode::Off) { S->SetDrawMode(EExposureDrawMode::Changed); }
						}
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					TextButton(EPT("T 视角", "T view"), [TName, CTName]()
					{
						if (UExposureProbeSubsystem* S = Subsys()) { S->FocusViewportOnEdge(TName + TEXT("->") + CTName); }
					}, EPT("视口移到 T 的眼位，看向 CT", "Viewport at T's eye, looking at CT"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					TextButton(EPT("CT 视角", "CT view"), [TName, CTName]()
					{
						if (UExposureProbeSubsystem* S = Subsys()) { S->FocusViewportOnEdge(CTName + TEXT("->") + TName); }
					}, EPT("视口移到 CT 的眼位，看向 T", "Viewport at CT's eye, looking at T"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					TextButton(E.bDesignIntent ? EPT("取消保留", "Clear Intent") : EPT("设计保留", "Mark as Design Intent"),
						[this, Name, bMarked = E.bDesignIntent]()
					{
						if (UExposureProbeSubsystem* S = Subsys())
						{
							FString Error;
							const bool bOk = bMarked ? S->ClearDesignIntent(Name, Error) : S->MarkDesignIntent(Name, NoteDraft, Error);
							if (!bOk) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Error); }
						}
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
			[
				SNew(STextBlock).Text(FText::FromString(Sub)).Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground()).AutoWrapText(true)
			]
		];
}

void SExposureProbePanel::Refresh()
{
	using namespace ExposureProbePanel;
	const UExposureProbeSubsystem* S = Subsys();
	if (!S || !SummaryText.IsValid())
	{
		return;
	}
	const FExposureAuditReport& R = S->GetLastReport();
	SummaryText->SetText(S->IsBuilt() ? FText::FromString(R.Summary) : EPT("放置 T 探针与 CT 探针后点「全量计算」", "Place T and CT probes, then press Recompute"));
	SummaryText->SetColorAndOpacity(R.NewOneWay > 0 ? FSlateColor(Alert) : FSlateColor::UseForeground());

	FString Stats;
	if (S->IsBuilt())
	{
		Stats = FString::Format(*EPS("{0} · {1} 个探针 · 射线 {2}/{3} · {4} ms", "{0} · {1} probes · rays {2}/{3} · {4} ms"),
			{ R.bFullRecompute ? EPS("全量", "full") : EPS("局部", "local"), R.Probes, R.RaysRecast, R.RaysTotal, FString::Printf(TEXT("%.1f"), R.TotalMs) });
		const TArray<FString>& Geom = S->HasBaseline() ? S->GetBaselineGeomChanges() : R.EditedActors;
		if (Geom.Num() > 0)
		{
			TArray<FString> Shown(Geom.GetData(), FMath::Min(Geom.Num(), 12));
			Stats += TEXT("\n") + (S->HasBaseline() ? EPS("相对基线改动的物体：", "Objects changed since the baseline: ") : EPS("改动：", "Edited: "))
				+ FString::Join(Shown, TEXT(", ")) + (Geom.Num() > Shown.Num() ? FString::Printf(TEXT(" … (+%d)"), Geom.Num() - Shown.Num()) : FString());
		}
		if (S->HasBaseline() && S->GetBaselineProbeChanges().Num() > 0)
		{
			Stats += TEXT("\n") + EPS("探针变化：", "Probe changes: ") + FString::Join(S->GetBaselineProbeChanges(), TEXT(", "));
		}
		int32 Far = 0;
		for (const FExposureEdgeChange& C : S->HasBaseline() ? S->GetBaselineChanges() : R.Changes) { Far += C.bFar ? 1 : 0; }
		if (Far > 0)
		{
			const UExposureProbeSettings* Set = GetDefault<UExposureProbeSettings>();
			Stats += TEXT("\n") + FString::Format(*EPS("远处影响：{0} 对探针离改动超过 {1}（视口中用虚线从改动处连过去）",
				"Far-reaching: {0} pairs more than {1} from the edit (dashed connectors in the viewport)"),
				{ Far, FString::Printf(TEXT("%.0f m"), Set->FarDistance * Set->UnitToCm / 100.f) });
		}
	}
	StatsText->SetText(FText::FromString(Stats));

	WarningBox->ClearChildren();
	for (const FString& W : R.Warnings)
	{
		WarningBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)[ SNew(SImage).Image(FAppStyle::GetBrush("Icons.WarningWithColor")) ]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(W)).AutoWrapText(true).Font(FAppStyle::GetFontStyle("SmallFont")) ]
		];
	}

	ChangedBox->ClearChildren();
	ChangedBox->AddSlot().AutoHeight().Padding(0, 2, 0, 6)
	[
		SNew(SEditableTextBox)
		.HintText(EPT("设计保留备注", "Design intent note"))
		.Text_Lambda([this]() { return FText::FromString(NoteDraft); })
		.OnTextChanged_Lambda([this](const FText& T) { NoteDraft = T.ToString(); })
	];
	for (const FExposureEdgeChange& C : S->HasBaseline() ? S->GetBaselineChanges() : R.Changes)
	{
		ChangedBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ MakePairRow(C.After, &C) ];
	}

	EvidenceBox->ClearChildren();
	static const TCHAR* RegZh[] = { TEXT("腿"), TEXT("躯干"), TEXT("头") };
	static const TCHAR* RegEn[] = { TEXT("legs"), TEXT("torso"), TEXT("head") };
	for (const FExposureRayChange& Ev : R.Evidence)
	{
		const int32 Ri = int32(Ev.Region);
		const FString Line = FString::Format(*EPS("{0}  {1} {2} · {3}°  {4}{5}", "{0}  {1} {2} · {3}°  {4}{5}"),
			{ Ev.Direction, ExposureProbeText::IsChinese() ? RegZh[Ri] : RegEn[Ri], FString::Printf(TEXT("%.0f"), Ev.HeightUnits), FString::Printf(TEXT("%.0f"), Ev.AngleDeg),
			  Ev.Change == TEXT("blocked") ? EPS("被挡", "blocked") : EPS("打开", "opened"),
			  Ev.Blocker.IsEmpty() ? FString() : TEXT("  ") + Ev.Blocker });
		EvidenceBox->AddSlot().AutoHeight().Padding(0, 1)
		[
			SNew(STextBlock).Text(FText::FromString(Line)).Font(FAppStyle::GetFontStyle("SmallFont"))
		];
	}

	AllBox->ClearChildren();
	for (const FExposureEdge& E : S->GetEdges())
	{
		AllBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ MakePairRow(E, nullptr) ];
	}
	RefreshHeat();
}
