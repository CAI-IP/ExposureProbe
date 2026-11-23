#include "ExposureProbeSubsystem.h"

#include "ExposureProbeActor.h"
#include "ExposureSmokeActor.h"
#include "ExposureProbeSettings.h"
#include "ExposureProbeText.h"
#include "ActorEditorUtils.h"
#include "CanvasItem.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "JsonObjectConverter.h"
#include "LevelEditorViewport.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderer.h"
#include "SceneView.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"

#define LOCTEXT_NAMESPACE "ExposureProbe"

namespace ExposureBaseline
{
	static bool CountsDiffer(const FExposureEdge& A, const FExposureEdge& B)
	{
		return A.VisibleTtoCT != B.VisibleTtoCT || A.ValidCT != B.ValidCT || A.VisibleCTtoT != B.VisibleCTtoT || A.ValidT != B.ValidT;
	}

	/** Report order: what a designer must look at first. */
	static int32 Priority(EExposureChangeKind K)
	{
		switch (K)
		{
		case EExposureChangeKind::NewOneWay: return 0;
		case EExposureChangeKind::Opened: return 1;
		case EExposureChangeKind::Blocked: return 2;
		case EExposureChangeKind::OneWayResolved: return 3;
		case EExposureChangeKind::Shifted: return 4;
		case EExposureChangeKind::ProbeMoved: return 5;
		case EExposureChangeKind::NewPair: return 6;
		default: return 7;
		}
	}

	/** Settings that only change how results are drawn or reported, not the results. */
	static bool IsDisplayOnly(const FName& N)
	{
		static const TSet<FName> Names = {
			TEXT("RayThickness"), TEXT("bShowBlockedRays"), TEXT("bShowLabels"), TEXT("TColor"), TEXT("CTColor"), TEXT("Language"),
			TEXT("HeatSeesColor"), TEXT("HeatSeenColor"), TEXT("HeatmapScale"), TEXT("bAutoUpdate"), TEXT("DebounceSeconds"),
			TEXT("bBaselineOnSave"), TEXT("bParallelTraces"), TEXT("ChangeThreshold"),
			TEXT("SmokeRadius"), TEXT("SmokeLift"), TEXT("SmokeCoverage"), TEXT("SmokeMaxCount"), TEXT("SmokeClearance"), TEXT("SmokeMinDistance"),
			TEXT("SmokeSearchRadius"), TEXT("SmokeDuration"), TEXT("SmokeRetreatRadius"), TEXT("SmokeRecoverShare"), TEXT("SmokeRecoverDeltaA"),
			TEXT("SmokeMoveSpeed"), TEXT("SmokeReacquireSeconds"), TEXT("bSmokeFog"), TEXT("SmokePreset"), TEXT("SmokeStyle"), TEXT("SmokeDensity"),
			TEXT("SmokeColor"), TEXT("bSnapSmokesToCells") };
		return Names.Contains(N);
	}

	static TMap<FString, FString> ParseSettings(const FString& Text)
	{
		TMap<FString, FString> Out;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& L : Lines)
		{
			FString K, V;
			if (L.Split(TEXT("="), &K, &V)) { Out.Add(K, V); }
		}
		return Out;
	}

	static FString Signed(float V) { return FString::Printf(TEXT("%+.2f"), V); }
	static FString Pct(float V) { return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(V * 100.f)); }

	static const TCHAR* LastSave = TEXT("_LastSave");
	static const TCHAR* PreviousSave = TEXT("_PreviousSave");
}

FString UExposureProbeSubsystem::BaselineDir() const
{
	FString Level = TEXT("Untitled");
	if (UWorld* World = GetEditorWorld())
	{
		Level = World->GetOutermost()->GetName();
		Level.RemoveFromStart(TEXT("/"));
		Level.ReplaceInline(TEXT("/"), TEXT("_"));
	}
	return LogDir() / TEXT("Baselines") / FPaths::MakeValidFileName(Level);
}

FString UExposureProbeSubsystem::LastSaveBaselineName() { return ExposureBaseline::LastSave; }
FString UExposureProbeSubsystem::PreviousSaveBaselineName() { return ExposureBaseline::PreviousSave; }

FString UExposureProbeSubsystem::BaselineDisplayName(const FString& Name)
{
	if (Name == ExposureBaseline::LastSave) { return EPS("上次保存", "Last save"); }
	if (Name == ExposureBaseline::PreviousSave) { return EPS("上上次保存", "Save before last"); }
	return Name;
}

TArray<FString> UExposureProbeSubsystem::ListBaselines() const
{
	const FString Dir = BaselineDir();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
	Files.Sort([&Dir](const FString& L, const FString& R)
	{
		return IFileManager::Get().GetTimeStamp(*(Dir / L)) > IFileManager::Get().GetTimeStamp(*(Dir / R));
	});
	for (FString& F : Files) { F = FPaths::GetBaseFilename(F); }
	return Files;
}

FString UExposureProbeSubsystem::SettingsText()
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	FString Out;
	for (TFieldIterator<FProperty> It(UExposureProbeSettings::StaticClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Config) || ExposureBaseline::IsDisplayOnly(It->GetFName()))
		{
			continue;
		}
		FString Value;
		It->ExportText_InContainer(0, Value, S, nullptr, nullptr, PPF_None);
		Out += It->GetName() + TEXT("=") + Value + TEXT("\n");
	}
	return Out;
}

void UExposureProbeSubsystem::SnapshotGeometry(TArray<FExposureGeomRecord>& Out) const
{
	Out.Reset();
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return;
	}
	const ECollisionChannel Channel = GetDefault<UExposureProbeSettings>()->TraceChannel;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		FExposureGeomState G;
		if (!IsValid(Actor) || Actor->IsA<AExposureProbeActor>() || Actor->IsA<AExposureSmokeActor>() || FActorEditorUtils::IsABuilderBrush(Actor) || !ComputeGeom(Actor, Channel, G))
		{
			continue;
		}
		FExposureGeomRecord& R = Out.AddDefaulted_GetRef();
		R.Path = Actor->GetPathName();
		R.Label = G.Label;
		R.Hash = int64(G.Hash);
		R.Box = G.Box;
	}
	// Placed smokes block the probe rays, so they are part of the inputs.
	if (GetDefault<UExposureProbeSettings>()->bSmokesBlockProbes)
	{
		for (const AExposureSmokeActor* A : GetPlacedSmokes(true))
		{
			const FVector C = A->GetCentre();
			const float Radius = A->GetRadiusCm();
			FExposureGeomRecord& R = Out.AddDefaulted_GetRef();
			R.Path = A->GetPathName();
			R.Label = A->GetSmokeName();
			R.Box = FBox(C - FVector(Radius), C + FVector(Radius));
			R.Hash = int64(GetTypeHash(FString::Printf(TEXT("smoke %.1f %.1f %.1f %.1f"), C.X, C.Y, C.Z, Radius)));
		}
	}
	Out.Sort([](const FExposureGeomRecord& L, const FExposureGeomRecord& R) { return L.Path < R.Path; });
}

void UExposureProbeSubsystem::SnapshotProbes(TArray<FExposureProbeRecord>& Out) const
{
	Out.Reset();
	for (int32 i = 0; i < Solver.Probes().Num(); ++i)
	{
		const FExposureProbeDesc& D = Solver.Probes()[i];
		FExposureProbeRecord& R = Out.AddDefaulted_GetRef();
		R.Name = D.Name;
		R.Team = D.Team;
		R.Feet = D.Feet;
		R.bCrouch = D.bCrouch;
		R.BodyKey = Solver.BodyOf(i).Key();
	}
}

bool UExposureProbeSubsystem::SaveBaseline(const FString& InName, FString& OutError)
{
	if (!bBuilt)
	{
		OutError = EPS("还没有计算结果", "Nothing computed yet");
		return false;
	}
	FString Name = FPaths::MakeValidFileName(InName.TrimStartAndEnd());
	if (Name.IsEmpty()) { Name = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H.%M")); }

	FExposureBaseline B;
	B.Name = Name;
	B.Time = FDateTime::Now().ToIso8601();
	if (UWorld* World = GetEditorWorld()) { B.Level = World->GetOutermost()->GetName(); }
	B.ModelKey = ModelKey;
	B.EngineVersion = FEngineVersion::Current().ToString();
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ExposureProbe")))
	{
		B.PluginVersion = Plugin->GetDescriptor().VersionName;
	}
	B.Settings = SettingsText();
	SnapshotProbes(B.Probes);
	SnapshotGeometry(B.Geometry);
	B.Edges = Edges;
	if (HasHeat() && !IsHeatStale())
	{
		B.HeatKey = HeatKey;
		B.HeatSpacingCm = HeatSpacingCm;
		B.Heat = Heat;
	}
	FString Json;
	if (!FJsonObjectConverter::UStructToJsonObjectString(B, Json, 0, 0, 0, nullptr, false))
	{
		OutError = TEXT("Could not serialize the baseline");
		return false;
	}
	const FString Path = BaselineDir() / (Name + TEXT(".json"));
	IFileManager::Get().MakeDirectory(*BaselineDir(), true);
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write %s"), *Path);
		return false;
	}
	WriteEvent(TEXT("BaselineSaved"), Name, Path);
	OnReportUpdated.Broadcast();
	return true;
}

void UExposureProbeSubsystem::HandlePostSaveWorld(UWorld* World, FObjectPostSaveContext Context)
{
	if (!GetDefault<UExposureProbeSettings>()->bBaselineOnSave || !Context.SaveSucceeded() || Context.IsFromAutoSave() || Context.IsProceduralSave()
		|| !World || World != GetEditorWorld() || (GEditor && GEditor->PlayWorld) || !bBuilt)
	{
		return;
	}
	if (HasPendingUpdate()) { UpdateNow(); }
	// Keep one older save, so "since the save before" stays available after saving again.
	const FString Dir = BaselineDir();
	const FString Last = Dir / (LastSaveBaselineName() + TEXT(".json"));
	if (IFileManager::Get().FileExists(*Last))
	{
		IFileManager::Get().Move(*(Dir / (PreviousSaveBaselineName() + TEXT(".json"))), *Last, true, true);
	}
	FString Error;
	if (!SaveBaseline(LastSaveBaselineName(), Error))
	{
		UE_LOG(LogExposureProbe, Warning, TEXT("Baseline on save: %s"), *Error);
		return;
	}
	if (bHasBaseline && (Baseline.Name == LastSaveBaselineName() || Baseline.Name == PreviousSaveBaselineName()))
	{
		SetActiveBaseline(Baseline.Name, Error);
	}
}

bool UExposureProbeSubsystem::DeleteBaseline(const FString& Name)
{
	const bool bOk = IFileManager::Get().Delete(*(BaselineDir() / (Name + TEXT(".json"))), false, true, true);
	if (bHasBaseline && Baseline.Name == Name)
	{
		FString Ignored;
		SetActiveBaseline(FString(), Ignored);
	}
	OnReportUpdated.Broadcast();
	return bOk;
}

bool UExposureProbeSubsystem::SetActiveBaseline(const FString& Name, FString& OutError)
{
	if (Name.IsEmpty())
	{
		bHasBaseline = false;
		Baseline = FExposureBaseline();
	}
	else
	{
		FString Json;
		FExposureBaseline Loaded;
		const FString Path = BaselineDir() / (Name + TEXT(".json"));
		if (!FFileHelper::LoadFileToString(Json, *Path) || !FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Loaded, 0, 0))
		{
			OutError = FString::Printf(TEXT("Could not read %s"), *Path);
			return false;
		}
		Baseline = MoveTemp(Loaded);
		Baseline.Name = Name;
		bHasBaseline = true;
	}
	UpdateBaselineDiff();
	// A baseline with a matching heatmap is shown as the change of A.
	HeatShow = CanShowHeatDelta() ? EExposureHeatShow::Delta : EExposureHeatShow::A;
	Redraw();
	RedrawHeat();
	OnReportUpdated.Broadcast();
	return true;
}

EExposureChangeKind UExposureProbeSubsystem::ClassifyChange(const FExposureEdgeChange& C, float Threshold)
{
	if (!C.bHadBefore) { return EExposureChangeKind::NewPair; }
	if (C.bNewOneWay) { return EExposureChangeKind::NewOneWay; }
	if (C.Before.bOneWay && !C.After.bOneWay) { return EExposureChangeKind::OneWayResolved; }
	const bool bOpened = (C.Before.VisibleTtoCT == 0 && C.After.VisibleTtoCT > 0) || (C.Before.VisibleCTtoT == 0 && C.After.VisibleCTtoT > 0);
	const bool bBlocked = (C.Before.VisibleTtoCT > 0 && C.After.VisibleTtoCT == 0) || (C.Before.VisibleCTtoT > 0 && C.After.VisibleCTtoT == 0);
	if (bOpened && !bBlocked) { return EExposureChangeKind::Opened; }
	if (bBlocked && !bOpened) { return EExposureChangeKind::Blocked; }
	return FMath::Abs(C.After.A - C.Before.A) >= Threshold ? EExposureChangeKind::Shifted : EExposureChangeKind::Minor;
}

FString UExposureProbeSubsystem::ChangeKindText(EExposureChangeKind Kind)
{
	switch (Kind)
	{
	case EExposureChangeKind::NewOneWay: return EPS("新单向", "new one-way");
	case EExposureChangeKind::OneWayResolved: return EPS("单向已消除", "one-way resolved");
	case EExposureChangeKind::Opened: return EPS("视线打开", "sightline opened");
	case EExposureChangeKind::Blocked: return EPS("视线被挡", "sightline blocked");
	case EExposureChangeKind::Shifted: return EPS("明显变化", "shifted");
	case EExposureChangeKind::NewPair: return EPS("新探针对", "new pair");
	case EExposureChangeKind::ProbeMoved: return EPS("探针移动", "probe moved");
	default: return EPS("小变化", "minor");
	}
}

void UExposureProbeSubsystem::UpdateBaselineDiff()
{
	BaselineChanges.Reset();
	BaselineResolved = 0;
	BaselineMissing = 0;
	BaselineEditBoxes.Reset();
	BaselineGeomChanges.Reset();
	BaselineProbeChanges.Reset();
	bBaselineInputsMatch = false;
	BaselineInputDiff.Reset();
	HeatDelta.Reset();
	HeatDeltaValid.Reset();
	BaselineOneWay.Reset();
	if (!bHasBaseline)
	{
		if (HeatShow == EExposureHeatShow::Delta) { HeatShow = EExposureHeatShow::A; }
		return;
	}
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();

	// Inputs: geometry, probes and settings against the baseline.
	TArray<FExposureGeomRecord> GeomNow;
	SnapshotGeometry(GeomNow);
	TMap<FString, const FExposureGeomRecord*> GeomBefore;
	for (const FExposureGeomRecord& G : Baseline.Geometry) { GeomBefore.Add(G.Path, &G); }
	TSet<FString> GeomSeen;
	for (const FExposureGeomRecord& G : GeomNow)
	{
		GeomSeen.Add(G.Path);
		const FExposureGeomRecord* Old = GeomBefore.FindRef(G.Path);
		if (!Old)
		{
			BaselineEditBoxes.Add(G.Box);
			BaselineGeomChanges.Add(G.Label + EPS("（新增）", " (added)"));
		}
		else if (Old->Hash != G.Hash)
		{
			BaselineEditBoxes.Add(Old->Box);
			if (!Old->Box.Equals(G.Box)) { BaselineEditBoxes.Add(G.Box); }
			BaselineGeomChanges.Add(G.Label);
		}
	}
	for (const FExposureGeomRecord& G : Baseline.Geometry)
	{
		if (!GeomSeen.Contains(G.Path))
		{
			BaselineEditBoxes.Add(G.Box);
			BaselineGeomChanges.Add(G.Label + EPS("（已删除）", " (deleted)"));
		}
	}

	TArray<FExposureProbeRecord> ProbesNow;
	SnapshotProbes(ProbesNow);
	TMap<FString, const FExposureProbeRecord*> ProbesBefore;
	for (const FExposureProbeRecord& P : Baseline.Probes) { ProbesBefore.Add(P.Name, &P); }
	TSet<FString> Moved;
	for (const FExposureProbeRecord& P : ProbesNow)
	{
		const FExposureProbeRecord* Old = ProbesBefore.FindRef(P.Name);
		if (!Old)
		{
			BaselineProbeChanges.Add(P.Name + EPS("（新增）", " (added)"));
		}
		else if (!Old->Feet.Equals(P.Feet, 1.0) || Old->bCrouch != P.bCrouch || Old->Team != P.Team || Old->BodyKey != P.BodyKey)
		{
			Moved.Add(P.Name);
			BaselineProbeChanges.Add(P.Name + (Old->Feet.Equals(P.Feet, 1.0) ? EPS("（姿态或身体改了）", " (pose or body changed)") : FString::Format(*EPS("（移动 {0}）", " (moved {0})"),
				{ FString::Printf(TEXT("%.0f cm"), FVector::Dist(Old->Feet, P.Feet)) })));
		}
	}
	for (const FExposureProbeRecord& P : Baseline.Probes)
	{
		if (!ProbesNow.ContainsByPredicate([&P](const FExposureProbeRecord& X) { return X.Name == P.Name; }))
		{
			BaselineProbeChanges.Add(P.Name + EPS("（已删除）", " (deleted)"));
		}
	}

	TArray<FString> Diff;
	if (!Baseline.Settings.IsEmpty())
	{
		const TMap<FString, FString> A = ExposureBaseline::ParseSettings(Baseline.Settings), B = ExposureBaseline::ParseSettings(SettingsText());
		TArray<FString> Keys;
		for (const TPair<FString, FString>& KV : B) { if (A.FindRef(KV.Key) != KV.Value) { Keys.Add(KV.Key); } }
		if (Keys.Num() > 0) { Diff.Add(EPS("设置：", "settings: ") + FString::Join(Keys, TEXT(", "))); }
	}
	else if (Baseline.ModelKey != ModelKey)
	{
		Diff.Add(EPS("身体或采样设置", "body or sampling settings"));
	}
	if (BaselineProbeChanges.Num() > 0) { Diff.Add(FString::Format(*EPS("{0} 个探针", "{0} probes"), { BaselineProbeChanges.Num() })); }
	if (BaselineGeomChanges.Num() > 0) { Diff.Add(FString::Format(*EPS("{0} 个物体", "{0} objects"), { BaselineGeomChanges.Num() })); }
	if (Baseline.Geometry.Num() == 0) { Diff.Add(EPS("基线没有几何指纹（旧版本）", "the baseline has no geometry fingerprint (older version)")); }
	bBaselineInputsMatch = Diff.Num() == 0;
	BaselineInputDiff = FString::Join(Diff, *EPS("；", "; "));

	// Pairs.
	const float FarCm = S->FarDistance * S->UnitToCm;
	TMap<FString, const FExposureEdge*> Before;
	for (const FExposureEdge& E : Baseline.Edges) { Before.Add(E.Edge, &E); }
	TSet<FString> Now;
	for (const FExposureEdge& E : Edges)
	{
		Now.Add(E.Edge);
		const FExposureEdge* B = Before.FindRef(E.Edge);
		if (B && !ExposureBaseline::CountsDiffer(*B, E))
		{
			continue;
		}
		FExposureEdgeChange& C = BaselineChanges.AddDefaulted_GetRef();
		C.Edge = E.Edge;
		C.bHadBefore = B != nullptr;
		if (B) { C.Before = *B; }
		C.After = E;
		C.bNewOneWay = E.bOneWay && !(B && B->bOneWay);
		C.Kind = ClassifyChange(C, S->ChangeThreshold);
		if (C.bHadBefore && C.Kind != EExposureChangeKind::NewOneWay && (Moved.Contains(E.TProbe) || Moved.Contains(E.CTProbe)))
		{
			C.Kind = EExposureChangeKind::ProbeMoved;
		}
		C.bExpected = E.bDesignIntent;
		if (BaselineEditBoxes.Num() > 0)
		{
			const int32 Ti = Solver.FindProbe(E.TProbe), Ci = Solver.FindProbe(E.CTProbe);
			double Best = TNumericLimits<double>::Max();
			for (const FBox& Box : BaselineEditBoxes)
			{
				for (const int32 Pi : { Ti, Ci })
				{
					if (Pi != INDEX_NONE) { Best = FMath::Min(Best, FMath::Sqrt(Box.ComputeSquaredDistanceToPoint(Solver.Probes()[Pi].Feet))); }
				}
			}
			C.DistanceCm = Best < TNumericLimits<double>::Max() ? float(Best) : -1.f;
			C.bFar = C.DistanceCm > FarCm;
		}
	}
	for (const FExposureEdge& B : Baseline.Edges)
	{
		if (!Now.Contains(B.Edge)) { ++BaselineMissing; continue; }
		const FExposureEdge* E = Edges.FindByPredicate([&B](const FExposureEdge& X) { return X.Edge == B.Edge; });
		BaselineResolved += B.bOneWay && E && !E->bOneWay ? 1 : 0;
	}
	BaselineChanges.Sort([](const FExposureEdgeChange& L, const FExposureEdgeChange& R)
	{
		if (L.bExpected != R.bExpected) { return !L.bExpected; }
		const int32 PL = ExposureBaseline::Priority(L.Kind), PR = ExposureBaseline::Priority(R.Kind);
		if (PL != PR) { return PL < PR; }
		return FMath::Abs(L.After.A - L.Before.A) > FMath::Abs(R.After.A - R.Before.A);
	});

	// Floor: match each current sample with the baseline sample on the same grid spot and floor.
	const bool bSameFloor = Baseline.HeatKey.IsEmpty() || Baseline.HeatKey == HeatKey;
	if (HasHeat() && Baseline.Heat.Num() > 0 && bSameFloor && FMath::IsNearlyEqual(Baseline.HeatSpacingCm, HeatSpacingCm, 0.5f))
	{
		const float Sp = HeatSpacingCm;
		TMap<FIntVector, TArray<int32>> Grid;
		for (int32 j = 0; j < Baseline.Heat.Num(); ++j)
		{
			const FVector& F = Baseline.Heat[j].Feet;
			Grid.FindOrAdd(FIntVector(FMath::FloorToInt(F.X / Sp), FMath::FloorToInt(F.Y / Sp), 0)).Add(j);
		}
		HeatDelta.Init(0.f, Heat.Num());
		HeatDeltaValid.Init(false, Heat.Num());
		BaselineOneWay.Init(-1, Heat.Num());
		// Baselines from before the one-way analysis have no one-way counts to compare with.
		const bool bHasOneWay = !Baseline.HeatKey.IsEmpty();
		for (int32 i = 0; i < Heat.Num(); ++i)
		{
			const FVector& F = Heat[i].Feet;
			const FIntVector C(FMath::FloorToInt(F.X / Sp), FMath::FloorToInt(F.Y / Sp), 0);
			double Best = FMath::Square(0.5 * Sp);
			int32 Found = INDEX_NONE;
			for (int32 dx = -1; dx <= 1; ++dx)
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				if (const TArray<int32>* Bucket = Grid.Find(C + FIntVector(dx, dy, 0)))
				{
					for (const int32 j : *Bucket)
					{
						const FVector& G = Baseline.Heat[j].Feet;
						if (FMath::Abs(G.Z - F.Z) > 40.f) { continue; }
						const double D = FVector::DistSquared2D(F, G);
						if (D <= Best) { Best = D; Found = j; }
					}
				}
			}
			if (Found != INDEX_NONE)
			{
				HeatDelta[i] = Heat[i].A - Baseline.Heat[Found].A;
				HeatDeltaValid[i] = true;
				if (bHasOneWay) { BaselineOneWay[i] = Baseline.Heat[Found].OneWaySees; }
			}
		}
	}
	if (HeatShow == EExposureHeatShow::Delta && !CanShowHeatDelta()) { HeatShow = EExposureHeatShow::A; }
}

bool UExposureProbeSubsystem::ExportReport(FString& OutPath, FString& OutError) const
{
	if (!bBuilt)
	{
		OutError = EPS("还没有计算结果", "Nothing computed yet");
		return false;
	}
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const TArray<FExposureEdgeChange>& Changes = ActiveChanges();
	FString Level = TEXT("Untitled");
	if (UWorld* World = GetEditorWorld()) { Level = World->GetOutermost()->GetName(); }
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	const FString Dir = LogDir() / TEXT("Reports");
	const FString Base = Dir / FPaths::MakeValidFileName(FPaths::GetBaseFilename(Level) + TEXT("_") + Stamp);
	FString PluginVersion;
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ExposureProbe"))) { PluginVersion = Plugin->GetDescriptor().VersionName; }

	TMap<EExposureChangeKind, int32> Count;
	int32 Expected = 0, Far = 0, TowardT = 0, TowardCT = 0;
	for (const FExposureEdgeChange& C : Changes)
	{
		Count.FindOrAdd(C.Kind)++;
		Expected += C.bExpected ? 1 : 0;
		Far += C.bFar ? 1 : 0;
		const float D = C.After.A - C.Before.A;
		if (C.bHadBefore && D > 0.005f) { ++TowardT; }
		if (C.bHadBefore && D < -0.005f) { ++TowardCT; }
	}
	const TArray<FString>& Geom = bHasBaseline ? BaselineGeomChanges : LastReport.EditedActors;
	const FString Against = bHasBaseline ? BaselineDisplayName(Baseline.Name) : EPS("上一次更新", "the previous update");

	// Markdown for people.
	FString Md;
	Md += FString::Format(*EPS("# 暴露回归报告：{0}\n\n", "# Exposure regression report: {0}\n\n"), { FPaths::GetBaseFilename(Level) });
	Md += FString::Format(*EPS("- 对比对象：{0}{1}\n", "- Compared with: {0}{1}\n"), { Against, bHasBaseline ? FString::Printf(TEXT(" (%s)"), *Baseline.Time) : FString() });
	Md += FString::Format(*EPS("- 生成时间：{0}\n- 引擎 {1} · 插件 {2}\n", "- Generated: {0}\n- Engine {1} · plugin {2}\n"),
		{ FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M")), FEngineVersion::Current().ToString(), PluginVersion });
	Md += FString::Format(*EPS("- 探针 {0} 个 · 探针对 {1} 对 · 单向阈值 {2} · 明显变化阈值 |ΔA| ≥ {3}\n\n", "- {0} probes · {1} pairs · one-way threshold {2} · clear change |ΔA| ≥ {3}\n\n"),
		{ Solver.Probes().Num(), Edges.Num(), FString::Printf(TEXT("%.2f"), S->OneWayThreshold), FString::Printf(TEXT("%.2f"), S->ChangeThreshold) });

	Md += EPS("## 摘要\n\n", "## Summary\n\n");
	Md += FString::Format(*EPS("- 改动的物体：{0}\n- 变化的探针对：{1}（偏向 T {2}，偏向 CT {3}）\n", "- Objects changed: {0}\n- Pairs changed: {1} ({2} toward T, {3} toward CT)\n"),
		{ Geom.Num(), Changes.Num(), TowardT, TowardCT });
	for (const EExposureChangeKind K : { EExposureChangeKind::NewOneWay, EExposureChangeKind::Opened, EExposureChangeKind::Blocked, EExposureChangeKind::OneWayResolved,
		EExposureChangeKind::Shifted, EExposureChangeKind::ProbeMoved, EExposureChangeKind::NewPair, EExposureChangeKind::Minor })
	{
		if (Count.FindRef(K) > 0) { Md += FString::Printf(TEXT("  - %s：%d\n"), *ChangeKindText(K), Count.FindRef(K)); }
	}
	Md += FString::Format(*EPS("- 远处影响（离改动超过 {0}）：{1} 对\n- 标为设计保留（预期变化）：{2} 对\n", "- Far-reaching (more than {0} from the edit): {1}\n- Marked design intent (expected): {2}\n"),
		{ FString::Printf(TEXT("%.0f m"), S->FarDistance * S->UnitToCm / 100.f), Far, Expected });
	if (bHasBaseline)
	{
		Md += FString::Format(*EPS("- 单向视线已消除：{0} 对 · 探针已删除的对：{1}\n", "- One-way resolved: {0} · pairs that lost a probe: {1}\n"), { BaselineResolved, BaselineMissing });
		if (IsBaselineReproMismatch())
		{
			Md += EPS("- **复现检查未通过**：输入与基线完全相同，结果却不同。\n", "- **Reproducibility check failed**: inputs equal the baseline but the results differ.\n");
		}
		else if (BaselineInputsMatch())
		{
			Md += EPS("- 复现检查通过：输入与基线相同，结果一致。\n", "- Reproducibility check passed: same inputs, same results.\n");
		}
		else
		{
			Md += FString::Format(*EPS("- 与基线不同的输入：{0}\n", "- Inputs that differ from the baseline: {0}\n"), { BaselineInputDiff });
		}
	}
	if (HasHeat())
	{
		int32 NewSpots = 0;
		for (int32 i = 0; i < Heat.Num(); ++i) { NewSpots += IsNewOneWaySpot(i) ? 1 : 0; }
		Md += FString::Format(*EPS("- 地面分析：{0} 个采样点，{1} 处单向视线位置{2}{3}\n", "- Floor analysis: {0} samples, {1} one-way spots{2}{3}\n"),
			{ Heat.Num(), GetOneWaySpotCount(), bHasBaseline && BaselineOneWay.Num() > 0 ? FString::Format(*EPS("，其中 {0} 处为新增", ", {0} of them new"), { NewSpots }) : FString(),
			  IsHeatStale() ? EPS("（已过期，需重新计算）", " (stale; recompute)") : FString() });
	}

	auto Section = [&](const FString& Title, TFunctionRef<bool(const FExposureEdgeChange&)> Filter)
	{
		TArray<const FExposureEdgeChange*> Rows;
		for (const FExposureEdgeChange& C : Changes) { if (Filter(C)) { Rows.Add(&C); } }
		if (Rows.Num() == 0) { return; }
		Md += TEXT("\n## ") + Title + TEXT("\n\n");
		Md += EPS("| 探针对 | 类型 | A 之前 | A 之后 | ΔA | T 看 CT | CT 看 T | 距改动 | 备注 |\n|---|---|---|---|---|---|---|---|---|\n",
			"| Pair | Kind | A before | A after | ΔA | T sees CT | CT sees T | From edit | Note |\n|---|---|---|---|---|---|---|---|---|\n");
		for (const FExposureEdgeChange* C : Rows)
		{
			Md += FString::Printf(TEXT("| %s | %s | %s | %s | %s | %s → %s | %s → %s | %s | %s |\n"), *C->Edge, *ChangeKindText(C->Kind),
				C->bHadBefore ? *ExposureBaseline::Signed(C->Before.A) : TEXT("–"), *ExposureBaseline::Signed(C->After.A),
				C->bHadBefore ? *ExposureBaseline::Signed(C->After.A - C->Before.A) : TEXT("–"),
				C->bHadBefore ? *ExposureBaseline::Pct(C->Before.ShareTtoCT) : TEXT("–"), *ExposureBaseline::Pct(C->After.ShareTtoCT),
				C->bHadBefore ? *ExposureBaseline::Pct(C->Before.ShareCTtoT) : TEXT("–"), *ExposureBaseline::Pct(C->After.ShareCTtoT),
				C->DistanceCm >= 0.f ? *FString::Printf(TEXT("%.1f m%s"), C->DistanceCm / 100.f, C->bFar ? TEXT(" *") : TEXT("")) : TEXT("–"),
				*C->After.DesignNote.Replace(TEXT("|"), TEXT("/")));
		}
	};
	Section(EPS("需要复查", "Needs review"), [](const FExposureEdgeChange& C) { return !C.bExpected && C.Kind != EExposureChangeKind::Minor; });
	Section(EPS("预期变化（设计保留）", "Expected (design intent)"), [](const FExposureEdgeChange& C) { return C.bExpected; });
	Section(EPS("小变化", "Minor"), [](const FExposureEdgeChange& C) { return !C.bExpected && C.Kind == EExposureChangeKind::Minor; });
	if (Geom.Num() > 0)
	{
		Md += EPS("\n## 改动的物体\n\n", "\n## Objects changed\n\n");
		for (const FString& G : Geom) { Md += TEXT("- ") + G + TEXT("\n"); }
	}
	if (bHasBaseline && BaselineProbeChanges.Num() > 0)
	{
		Md += EPS("\n## 探针变化\n\n", "\n## Probe changes\n\n");
		for (const FString& P : BaselineProbeChanges) { Md += TEXT("- ") + P + TEXT("\n"); }
	}
	Md += EPS("\n距改动一栏带 * 的探针对离改动超过远处距离。\n", "\nPairs marked * in the From edit column lie beyond the far distance.\n");

	// JSON for scripts and CI.
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("level"), Level);
	Root->SetStringField(TEXT("time"), FDateTime::Now().ToIso8601());
	Root->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Root->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Root->SetStringField(TEXT("baseline"), bHasBaseline ? Baseline.Name : FString());
	Root->SetStringField(TEXT("baselineTime"), bHasBaseline ? Baseline.Time : FString());
	Root->SetBoolField(TEXT("inputsMatch"), bHasBaseline && BaselineInputsMatch());
	Root->SetBoolField(TEXT("reproMismatch"), IsBaselineReproMismatch());
	Root->SetStringField(TEXT("inputDiff"), BaselineInputDiff);
	Root->SetNumberField(TEXT("changeThreshold"), S->ChangeThreshold);
	Root->SetNumberField(TEXT("oneWayThreshold"), S->OneWayThreshold);
	Root->SetNumberField(TEXT("probes"), Solver.Probes().Num());
	Root->SetNumberField(TEXT("pairs"), Edges.Num());
	Root->SetNumberField(TEXT("resolvedOneWay"), bHasBaseline ? BaselineResolved : 0);
	Root->SetNumberField(TEXT("missingPairs"), bHasBaseline ? BaselineMissing : 0);
	TArray<TSharedPtr<FJsonValue>> GeomJson, ProbeJson, ChangeJson;
	for (const FString& G : Geom) { GeomJson.Add(MakeShared<FJsonValueString>(G)); }
	for (const FString& P : BaselineProbeChanges) { ProbeJson.Add(MakeShared<FJsonValueString>(P)); }
	const UEnum* KindEnum = StaticEnum<EExposureChangeKind>();
	for (const FExposureEdgeChange& C : Changes)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("pair"), C.Edge);
		O->SetStringField(TEXT("kind"), KindEnum->GetNameStringByValue(int64(C.Kind)));
		O->SetBoolField(TEXT("expected"), C.bExpected);
		O->SetBoolField(TEXT("hadBefore"), C.bHadBefore);
		O->SetNumberField(TEXT("aBefore"), C.bHadBefore ? C.Before.A : 0.0);
		O->SetNumberField(TEXT("aAfter"), C.After.A);
		O->SetNumberField(TEXT("tSeesCtBefore"), C.Before.ShareTtoCT);
		O->SetNumberField(TEXT("tSeesCtAfter"), C.After.ShareTtoCT);
		O->SetNumberField(TEXT("ctSeesTBefore"), C.Before.ShareCTtoT);
		O->SetNumberField(TEXT("ctSeesTAfter"), C.After.ShareCTtoT);
		O->SetBoolField(TEXT("oneWayBefore"), C.Before.bOneWay);
		O->SetBoolField(TEXT("oneWayAfter"), C.After.bOneWay);
		O->SetNumberField(TEXT("distanceCm"), C.DistanceCm);
		O->SetBoolField(TEXT("far"), C.bFar);
		ChangeJson.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("geometryChanges"), GeomJson);
	Root->SetArrayField(TEXT("probeChanges"), ProbeJson);
	Root->SetArrayField(TEXT("changes"), ChangeJson);
	if (HasHeat())
	{
		TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
		H->SetNumberField(TEXT("samples"), Heat.Num());
		H->SetNumberField(TEXT("spacingCm"), HeatSpacingCm);
		H->SetNumberField(TEXT("radiusCm"), HeatRadiusCm);
		H->SetNumberField(TEXT("oneWaySpots"), GetOneWaySpotCount());
		int32 NewSpots = 0;
		for (int32 i = 0; i < Heat.Num(); ++i) { NewSpots += IsNewOneWaySpot(i) ? 1 : 0; }
		H->SetNumberField(TEXT("newOneWaySpots"), NewSpots);
		H->SetBoolField(TEXT("stale"), IsHeatStale());
		Root->SetObjectField(TEXT("floor"), H);
	}
	FString Json;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));

	IFileManager::Get().MakeDirectory(*Dir, true);
	if (!FFileHelper::SaveStringToFile(Md, *(Base + TEXT(".md")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !FFileHelper::SaveStringToFile(Json, *(Base + TEXT(".json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write %s"), *Base);
		return false;
	}
	OutPath = FPaths::ConvertRelativePathToFull(Base + TEXT(".md"));
	WriteEvent(TEXT("ReportExported"), bHasBaseline ? Baseline.Name : FString(), OutPath);
	return true;
}

void UExposureProbeSubsystem::DrawHeatLabels(UCanvas* Canvas)
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	if (!HasHeat())
	{
		return;
	}
	const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle("Bold", 10);
	const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 8);
	const FLinearColor Ink(0.94f, 0.94f, 0.94f);
	const FLinearColor Muted(0.66f, 0.66f, 0.66f);
	const FLinearColor PlateCol(0.03f, 0.03f, 0.035f, 0.78f);
	const FLinearColor Alert = UExposureProbeSettings::AlertColor();
	const float Dpi = Canvas->GetDPIScale();
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	auto Tile = [&](const FVector2D& Min, const FVector2D& Size, const FLinearColor& Col)
	{
		FCanvasTileItem Item(Min * Dpi, Size * Dpi, Col);
		Item.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
	};
	auto Text = [&](const FVector2D& At, const FString& Str, const FSlateFontInfo& F, const FLinearColor& Col)
	{
		FCanvasTextItem Item(At, FText::FromString(Str), F, Col);
		Item.Font = GEngine->GetSmallFont();
		Item.EnableShadow(FLinearColor(0, 0, 0, 0.6f));
		Canvas->DrawItem(Item);
	};
	auto Size = [&](const FString& Str, const FSlateFontInfo& F) { return FVector2D(Measure->Measure(Str, F)); };
	const bool bDelta = HeatShow == EExposureHeatShow::Delta;
	const float Scale = HeatScale;

	if (IsHeatStale())
	{
		const FString Msg = EPS("关卡或身体参数已改动，需重新分析地面", "Level or body changed; recompute the floor analysis");
		const FVector2D MS = Size(Msg, Body);
		Tile(FVector2D(14.f, 14.f), MS + FVector2D(16.f, 8.f), PlateCol);
		Tile(FVector2D(14.f, 14.f), FVector2D(3.f, MS.Y + 8.f), Alert);
		Text(FVector2D(22.f, 18.f), Msg, Body, Ink);
	}

	// Readout of the cell under the mouse, in the viewport being edited.
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if (!Client || !Client->Viewport || Canvas->SceneView->Family->RenderTarget != Client->Viewport)
	{
		return;
	}
	HoverHeat = INDEX_NONE;
	FIntPoint Mouse;
	Client->Viewport->GetMousePos(Mouse);
	if (Mouse.X < 0 || Mouse.Y < 0 || Mouse.X >= Client->Viewport->GetSizeXY().X || Mouse.Y >= Client->Viewport->GetSizeXY().Y)
	{
		return;
	}
	FVector Origin, Dir;
	Canvas->SceneView->DeprojectFVector2D(FVector2D(Mouse), Origin, Dir);
	FHitResult Hit;
	UWorld* World = GetEditorWorld();
	if (!World || !World->LineTraceSingleByChannel(Hit, Origin, Origin + Dir * 200000.f, S->TraceChannel, FCollisionQueryParams(SCENE_QUERY_STAT(ExposureHeatPick), true)))
	{
		return;
	}
	const int32 Index = NearestHeat(Hit.ImpactPoint);
	if (Index == INDEX_NONE)
	{
		return;
	}
	HoverHeat = Index;
	const FExposureHeatPoint& P = Heat[Index];
	FString Line1;
	TArray<FString> More;
	FLinearColor Chip = Muted;
	float V = 0.f;
	if (bShowHeat && HeatValue(Index, V))
	{
		Line1 = bDelta ? FString::Printf(TEXT("ΔA %+.2f"), V) : FString::Printf(TEXT("A %+.2f"), V);
		More.Add(bDelta
			? FString::Format(*EPS("现在 {0} · 基线 {1}", "now {0} · baseline {1}"), { FString::Printf(TEXT("%+.2f"), P.A), FString::Printf(TEXT("%+.2f"), P.A - V) })
			: FString::Format(*EPS("看见 {0}% · 被看见 {1}%{2}", "sees {0}% · seen {1}%{2}"),
				{ FMath::RoundToInt(P.Vout * 100.f), FMath::RoundToInt(P.Vin * 100.f), P.bCrouch ? EPS(" · 蹲", " · crouch") : FString() }));
		const float T = FMath::Clamp(V / Scale, -1.f, 1.f);
		Chip = FMath::Abs(T) < 0.07f ? Muted : (T > 0 ? S->HeatSeesColor : S->HeatSeenColor);
	}
	if (bShowOneWay)
	{
		FString One;
		if (HasProbeOneWay())
		{
			TArray<FString> Threat, Exposed;
			for (const int32 k : GetCellThreatens(Index)) { Threat.Add(ProbeOneWayNames[k]); }
			for (const int32 k : GetCellExposedTo(Index)) { Exposed.Add(ProbeOneWayNames[k]); }
			if (Threat.Num() > 0) { One = FString::Format(*EPS("单向看见 {0}", "sees {0} one-way"), { FString::Join(Threat, TEXT(", ")) }); Chip = UExposureProbeSettings::PeekColor(); }
			else if (Exposed.Num() > 0) { One = FString::Format(*EPS("被 {0} 单向看见", "seen one-way by {0}"), { FString::Join(Exposed, TEXT(", ")) }); if (!bShowHeat) { Chip = UExposureProbeSettings::PeekedColor(); } }
			else { One = EPS("与选中探针之间没有单向视线", "no one-way sightline with the selection"); }
		}
		else
		{
			One = P.OneWaySees > 0 || P.OneWaySeenBy > 0
				? FString::Format(*EPS("单向看见 {0} 处 · 被 {1} 处单向看见{2}", "sees {0} spots one-way · seen by {1}{2}"),
					{ P.OneWaySees, P.OneWaySeenBy, IsNewOneWaySpot(Index) ? EPS(" · 新", " · new") : FString() })
				: EPS("没有单向视线", "no one-way sightline");
			if (P.OneWaySees > 0) { Chip = UExposureProbeSettings::PeekColor(); }
		}
		if (Line1.IsEmpty()) { Line1 = One; }
		else { More.Add(One); }
	}
	const FVector2D S1 = Size(Line1, Title);
	float W = S1.X, H = S1.Y + 6.f;
	for (const FString& L : More) { const FVector2D SL = Size(L, Body); W = FMath::Max(W, float(SL.X)); H += SL.Y; }
	W += 15.f;
	const FVector2D Min = FVector2D(Mouse) / Dpi + FVector2D(18.f, 14.f);
	Tile(Min, FVector2D(W, H), PlateCol);
	Tile(Min, FVector2D(3.f, H), Chip);
	Text(Min + FVector2D(9.f, 3.f), Line1, Title, Ink);
	float Y = 3.f + S1.Y;
	for (const FString& L : More) { Text(Min + FVector2D(9.f, Y), L, Body, Muted); Y += Size(L, Body).Y; }
}

#undef LOCTEXT_NAMESPACE
