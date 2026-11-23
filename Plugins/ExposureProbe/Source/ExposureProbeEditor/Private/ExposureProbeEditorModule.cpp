#include "Modules/ModuleManager.h"

#include "ExposureProbeActor.h"
#include "ExposureSmokeActor.h"
#include "ExposureProbeDetails.h"
#include "PropertyEditorModule.h"
#include "ExposureProbeText.h"
#include "ExposureProbeSettings.h"
#include "ExposureProbeSubsystem.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ISettingsModule.h"
#include "LevelEditorViewport.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"
#include "SExposureProbePanel.h"
#include "AssetRegistry/AssetData.h"
#include "Framework/Docking/TabManager.h"
#include "IPlacementModeModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

static const FName ExposureProbeTabName(TEXT("ExposureProbe"));
static const FName ExposureProbePlacement(TEXT("ExposureProbe"));

namespace ExposureProbeMenu
{
	/** A floor layer needs the floor analysis; turning one on runs it once. */
	static void ToggleLayer(EExposureLayer Layer)
	{
		UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
		if (!S) { return; }
		const bool bOn = !S->IsLayerOn(Layer);
		S->SetLayer(Layer, bOn);
		if (bOn && Layer != EExposureLayer::Probes && !S->HasHeat())
		{
			FString Message;
			if (!S->ComputeHeatmap(Message))
			{
				S->SetLayer(Layer, false);
				UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Message);
			}
		}
	}

	/** New probe where the centre of the active viewport looks at. */
	static void PlaceProbe(UClass* Class)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
		if (!World || !Client)
		{
			return;
		}
		const FVector From = Client->GetViewLocation();
		const FVector Dir = Client->GetViewRotation().Vector();
		FHitResult Hit;
		const FVector At = World->LineTraceSingleByChannel(Hit, From, From + Dir * 100000.f, GetDefault<UExposureProbeSettings>()->TraceChannel,
			FCollisionQueryParams(SCENE_QUERY_STAT(ExposureProbePlace), true)) ? Hit.ImpactPoint : From + Dir * 500.f;
		const FScopedTransaction Transaction(EPT("放置探针", "Place Probe"));
		GEditor->SelectNone(false, true);
		GEditor->AddActor(World->GetCurrentLevel(), Class, FTransform(At));
	}

	static void FillMenu(UToolMenu* Menu)
	{
		const FName Style = FAppStyle::GetAppStyleSetName();
		{
			FToolMenuSection& Sec = Menu->AddSection("Panel", EPT("面板", "Panel"));
			Sec.AddMenuEntry("OpenPanel", EPT("打开暴露探针面板", "Open the Exposure Probe Panel"), EPT("探针对、基线对比、地面分析和报告", "Probe pairs, baseline comparison, floor analysis and reports"),
				FSlateIcon(Style, "LevelEditor.Tabs.Viewports"), FUIAction(FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(ExposureProbeTabName); })));
		}
		{
			FToolMenuSection& Sec = Menu->AddSection("Layers", EPT("视口显示（可同时打开）", "Viewport layers (any combination)"));
			auto Layer = [&Sec](FName Name, const FText& Label, const FText& Tip, EExposureLayer L)
			{
				Sec.AddMenuEntry(Name, Label, Tip, FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([L]() { ToggleLayer(L); }), FCanExecuteAction(),
						FIsActionChecked::CreateLambda([L]() { const UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get(); return S && S->IsLayerOn(L); })),
					EUserInterfaceActionType::ToggleButton);
			};
			Layer("LayerProbes", EPT("探针", "Probes"), EPT("成对探针的连线、身体着色和数值卡片", "Probe pair links, body fills and value cards"), EExposureLayer::Probes);
			Layer("LayerHeat", EPT("热力图（全图 A）", "Heatmap (full-map A)"), EPT("地面各位置的暴露不对称度 A：蓝色看见对方更多，红色被看见更多；首次打开时先分析地面",
				"Exposure asymmetry A on the floor: blue sees more, red is seen more; the first time analyses the floor"), EExposureLayer::Heatmap);
			Layer("LayerOneWay", EPT("单向视线", "One-way sightlines"), EPT("地面格子：黄色格子单向看见其他位置；选中探针时只显示与这些探针的关系；首次打开时先分析地面",
				"Floor cells: yellow cells see other spots one-way; with probes selected, only their relation to those probes; the first time analyses the floor"), EExposureLayer::OneWay);
		}
		{
			FToolMenuSection& Sec = Menu->AddSection("Place", EPT("放置", "Place"));
			Sec.AddMenuEntry("PlaceT", EPT("放置 T 探针", "Place T Probe"), EPT("放在视口中心所指的位置", "At the point under the viewport centre"),
				FSlateIcon(Style, "ClassIcon.Character"), FUIAction(FExecuteAction::CreateLambda([]() { PlaceProbe(AExposureProbeT::StaticClass()); })));
			Sec.AddMenuEntry("PlaceCT", EPT("放置 CT 探针", "Place CT Probe"), EPT("放在视口中心所指的位置", "At the point under the viewport centre"),
				FSlateIcon(Style, "ClassIcon.Character"), FUIAction(FExecuteAction::CreateLambda([]() { PlaceProbe(AExposureProbeCT::StaticClass()); })));
		}
		{
			FToolMenuSection& Sec = Menu->AddSection("Compute", EPT("计算", "Compute"));
			Sec.AddMenuEntry("Recompute", EPT("全量计算探针", "Recompute Probes"), FText::GetEmpty(), FSlateIcon(Style, "Icons.Refresh"),
				FUIAction(FExecuteAction::CreateLambda([]() { if (UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get()) { S->Rebuild(true); } })));
			Sec.AddMenuEntry("AnalyseFloor", EPT("分析地面", "Analyse Floor"), EPT("一次计算全图 A 与单向视线", "Full-map A and one-way sightlines in one pass"), FSlateIcon(Style, "Icons.Refresh"),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
					FString Message;
					if (S && !S->ComputeHeatmap(Message)) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Message); }
				})));
		}
		{
			FToolMenuSection& Sec = Menu->AddSection("Smoke", EPT("烟雾", "Smoke"));
			Sec.AddMenuEntry("PlaceSmoke", EPT("放置烟雾", "Place Smoke"), EPT("放在视口中心所指的位置，并吸附到最近的地面格子；拖动后重新吸附",
				"At the point under the viewport centre, snapped to the nearest floor cell; it snaps again after a drag"),
				FSlateIcon(Style, "ClassIcon.ExponentialHeightFogComponent"), FUIAction(FExecuteAction::CreateLambda([]()
				{
					UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
					FString Message;
					if (S && !S->PlaceSmokeAtView(Message)) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Message); }
				})));
			Sec.AddMenuEntry("EvaluateSmoke", EPT("评估已放置的烟雾（选中探针）", "Evaluate Placed Smoke (Selected Probe)"),
				EPT("计算已放置的烟雾遮挡选中探针对对方视线的比例，以及恢复视线的位置与时间 RT；拖动烟雾后自动更新",
					"Share of the selected probe's sightlines to the other side that the placed smoke cuts, and where and how fast it sees them again (RT); updates when a smoke is dragged"),
				FSlateIcon(Style, "Icons.Visible"), FUIAction(FExecuteAction::CreateLambda([]()
				{
					UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
					FString Message;
					if (S && !S->EvaluatePlacedSmokes(FString(), Message)) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Message); }
				})));
			Sec.AddMenuEntry("ComputeSmoke", EPT("计算烟雾方案（CC / RT）", "Compute Smoke Plan (CC / RT)"),
				EPT("在地面格子中求遮挡选中探针视线所需的最少烟雾数 CC，以及恢复视线的位置与时间 RT", "The fewest smokes on the floor cells that cut the selected probe's sightlines (CC), and where and how fast it sees them again (RT)"),
				FSlateIcon(Style, "Icons.Visible"), FUIAction(FExecuteAction::CreateLambda([]()
				{
					UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
					FString Message;
					if (S && !S->ComputeSmoke(FString(), Message)) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Message); }
				})));
			Sec.AddMenuEntry("PlacePlanSmokes", EPT("将方案转为放置的烟雾", "Convert Plan to Placed Smoke"), EPT("方案中的烟雾变为关卡中可拖动、可调尺寸的烟雾", "The plan's smokes become level actors you can drag and resize"),
				FSlateIcon(Style, "Icons.Plus"), FUIAction(FExecuteAction::CreateLambda([]()
				{
					UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
					FString Message;
					if (S && S->PlacePlanSmokes(Message) == 0) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Message); }
				}), FCanExecuteAction::CreateLambda([]() { const UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get(); return S && S->HasSmoke() && !S->GetSmoke().bPlaced && S->GetSmoke().Smokes.Num() > 0; })));
			Sec.AddMenuEntry("ClearSmoke", EPT("清除烟雾结果", "Clear Smoke Result"), EPT("清除视线与信息卡；已放置的烟雾保留", "Clears the sightlines and cards; placed smoke stays"), FSlateIcon(Style, "Icons.X"),
				FUIAction(FExecuteAction::CreateLambda([]() { if (UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get()) { S->ClearSmoke(); } }),
					FCanExecuteAction::CreateLambda([]() { const UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get(); return S && S->HasSmoke(); })));
			Sec.AddMenuEntry("ClearPlacedSmokes", EPT("删除全部已放置的烟雾", "Delete All Placed Smoke"), FText::GetEmpty(), FSlateIcon(Style, "Icons.Delete"),
				FUIAction(FExecuteAction::CreateLambda([]() { if (UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get()) { S->ClearPlacedSmokes(); } }),
					FCanExecuteAction::CreateLambda([]() { const UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get(); return S && S->GetPlacedSmokeCount() > 0; })));
		}
		{
			FToolMenuSection& Sec = Menu->AddSection("Baseline", EPT("基线与报告", "Baseline and report"));
			Sec.AddMenuEntry("SaveBaseline", EPT("存为基线（按时间命名）", "Save Baseline (named by time)"), EPT("命名基线或选择对比基线请使用面板", "Use the panel to name a baseline or pick one to compare against"),
				FSlateIcon(Style, "Icons.Save"), FUIAction(FExecuteAction::CreateLambda([]()
				{
					FString Error;
					UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
					if (S && !S->SaveBaseline(FString(), Error)) { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Error); }
				})));
			Sec.AddMenuEntry("ExportReport", EPT("导出并打开报告", "Export and Open Report"), EPT("相对当前基线的回归报告；未选基线时相对上一次改动", "Regression report against the active baseline, or against the last edit when none is active"),
				FSlateIcon(Style, "Icons.Documentation"), FUIAction(FExecuteAction::CreateLambda([]()
				{
					FString Path, Error;
					UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
					if (!S) { return; }
					if (S->ExportReport(Path, Error)) { FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path); }
					else { UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *Error); }
				})));
			Sec.AddMenuEntry("OpenFolder", EPT("打开日志、基线与报告文件夹", "Open the Log, Baseline and Report Folder"), FText::GetEmpty(), FSlateIcon(Style, "Icons.FolderOpen"),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					const FString Dir = FPaths::ConvertRelativePathToFull(UExposureProbeSubsystem::LogDir());
					IFileManager::Get().MakeDirectory(*Dir, true);
					FPlatformProcess::ExploreFolder(*Dir);
				})));
		}
		{
			FToolMenuSection& Sec = Menu->AddSection("Settings", EPT("设置", "Settings"));
			Sec.AddMenuEntry("OpenSettings", EPT("插件设置（身体、单位预设、热力图、烟雾尺寸）", "Plugin Settings (body, unit preset, heatmap, smoke size)"), FText::GetEmpty(), FSlateIcon(Style, "Icons.Settings"),
				FUIAction(FExecuteAction::CreateLambda([]() { FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Project", "Plugins", "ExposureProbeSettings"); })));
		}
	}
}

class FExposureProbeEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(ExposureProbeTabName,
			FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
			{
				return SNew(SDockTab).TabRole(ETabRole::NomadTab)[ SNew(SExposureProbePanel) ];
			}))
			.SetDisplayName(EPT("暴露探针", "Exposure Probe"))
			.SetTooltipText(EPT("当前关卡的 T/CT 成对视线审计", "Paired T/CT sightline audit of the current level"))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyEditor.RegisterCustomClassLayout(AExposureProbeActor::StaticClass()->GetFName(), FOnGetDetailCustomizationInstance::CreateStatic(&FExposureProbeDetails::MakeInstance));
		PropertyEditor.NotifyCustomizationModuleChanged();

		// One button on the level editor toolbar gathers everything the plugin offers.
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FExposureProbeEditorModule::RegisterMenus));

		IPlacementModeModule& Placement = IPlacementModeModule::Get();
		const FPlacementCategoryInfo Info(EPT("暴露探针", "Exposure Probe"), FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"),
			ExposureProbePlacement, TEXT("PMExposureProbe"), 60);
		if (Placement.RegisterPlacementCategory(Info))
		{
			for (UClass* Class : { AExposureProbeT::StaticClass(), AExposureProbeCT::StaticClass(), AExposureSmokeActor::StaticClass() })
			{
				TSharedRef<FPlaceableItem> Item = MakeShared<FPlaceableItem>(nullptr, FAssetData(Class));
				Item->DisplayName = Class == AExposureProbeT::StaticClass() ? EPT("T 探针", "T Probe")
					: Class == AExposureProbeCT::StaticClass() ? EPT("CT 探针", "CT Probe") : EPT("烟雾", "Smoke");
				if (TOptional<FPlacementModeID> Id = Placement.RegisterPlaceableItem(ExposureProbePlacement, Item))
				{
					PlaceIds.Add(Id.GetValue());
				}
			}
		}
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);
		UToolMenu* Bar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
		FToolMenuSection& Sec = Bar->FindOrAddSection("ExposureProbe");
		FToolMenuEntry Entry = FToolMenuEntry::InitComboButton("ExposureProbeMenu", FUIAction(), FNewToolMenuDelegate::CreateStatic(&ExposureProbeMenu::FillMenu),
			EPT("暴露探针", "Exposure Probe"), EPT("暴露探针：面板、视口图层（探针 / 热力图 / 单向视线）、放置探针、烟雾、基线与报告",
				"Exposure Probe: panel, viewport layers (probes / heatmap / one-way), placing probes, smoke, baselines and reports"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));
		Entry.StyleNameOverride = "CalloutToolbar";
		Sec.AddEntry(Entry);
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ExposureProbeTabName);
		}
		if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
		{
			PropertyEditor->UnregisterCustomClassLayout(AExposureProbeActor::StaticClass()->GetFName());
		}
		if (IPlacementModeModule::IsAvailable())
		{
			for (const FPlacementModeID& Id : PlaceIds) { IPlacementModeModule::Get().UnregisterPlaceableItem(Id); }
			IPlacementModeModule::Get().UnregisterPlacementCategory(ExposureProbePlacement);
		}
	}

private:
	TArray<FPlacementModeID> PlaceIds;
};

IMPLEMENT_MODULE(FExposureProbeEditorModule, ExposureProbeEditor)
