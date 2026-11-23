#include "ExposureProbeToolset.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

#include "ExposureProbeActor.h"
#include "ExposureSmokeActor.h"
#include "ExposureProbeSettings.h"
#include "ExposureProbeSolver.h"
#include "ExposureProbeSubsystem.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ExposureProbeToolset)

#define LOCTEXT_NAMESPACE "ExposureProbe"

namespace ExposureProbeTools
{
	static FString ToString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		FJsonSerializer::Serialize(Obj, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		return Out;
	}

	static FString Fail(const FString& Message)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("error"), Message);
		return ToString(Obj);
	}

	static FString Ok()
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		return ToString(Obj);
	}

	static FString Report(const FExposureAuditReport& R)
	{
		FString Out;
		FJsonObjectConverter::UStructToJsonObjectString(R, Out, 0, 0, 0, nullptr, false);
		return Out;
	}

	static TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
	}

	static bool ParseArray(const FString& Json, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Out);
	}

	static UExposureProbeSubsystem* Subsystem(FString& OutError)
	{
		UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
		if (!S || !S->GetEditorWorld())
		{
			OutError = TEXT("No editor world");
			return nullptr;
		}
		if (GEditor && GEditor->PlayWorld)
		{
			OutError = TEXT("Stop Play In Editor first");
			return nullptr;
		}
		return S;
	}
}

FString UExposureProbeToolset::SpawnProbes(const FString& ProbesJson)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	TArray<TSharedPtr<FJsonValue>> Items;
	if (!ParseArray(ProbesJson, Items)) { return Fail(TEXT("ProbesJson must be a JSON array")); }

	UWorld* World = S->GetEditorWorld();
	TArray<TSharedPtr<FJsonValue>> Spawned;
	const FScopedTransaction Transaction(LOCTEXT("SpawnProbes", "Spawn Exposure Probes"));
	for (const TSharedPtr<FJsonValue>& V : Items)
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (!V.IsValid() || !V->TryGetObject(O)) { return Fail(TEXT("Each probe must be an object")); }
		FString Name, Team = TEXT("T"), Pose = TEXT("auto");
		double X = 0, Y = 0, Z = 0;
		if (!(*O)->TryGetStringField(TEXT("name"), Name) || !(*O)->TryGetNumberField(TEXT("x"), X) || !(*O)->TryGetNumberField(TEXT("y"), Y)
			|| !(*O)->TryGetNumberField(TEXT("z"), Z))
		{
			return Fail(TEXT("Each probe needs name, x, y, z"));
		}
		(*O)->TryGetStringField(TEXT("team"), Team);
		(*O)->TryGetStringField(TEXT("pose"), Pose);

		for (TActorIterator<AExposureProbeActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetProbeName() == Name) { World->EditorDestroyActor(*It, true); }
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const bool bCT = Team.Equals(TEXT("CT"), ESearchCase::IgnoreCase);
		UClass* Class = bCT ? AExposureProbeCT::StaticClass() : AExposureProbeT::StaticClass();
		AExposureProbeActor* A = World->SpawnActor<AExposureProbeActor>(Class, FVector(X, Y, Z), FRotator::ZeroRotator, Params);
		if (!A) { return Fail(FString::Printf(TEXT("Could not spawn %s"), *Name)); }
		A->Modify();
		A->ProbeName = Name;
		A->Team = bCT ? EExposureTeam::CTSide : EExposureTeam::TSide;
		A->Pose = Pose.Equals(TEXT("stand"), ESearchCase::IgnoreCase) ? EExposurePose::Stand
			: Pose.Equals(TEXT("crouch"), ESearchCase::IgnoreCase) ? EExposurePose::Crouch : EExposurePose::Auto;
		A->SetActorLabel(TEXT("Probe_") + Name);
		A->SetFolderPath(TEXT("ExposureProbe"));
		Spawned.Add(MakeShared<FJsonValueString>(A->GetActorLabel()));
	}
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetArrayField(TEXT("spawned"), Spawned);
	return ToString(Obj);
}

FString UExposureProbeToolset::ClearProbes()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	UWorld* World = S->GetEditorWorld();
	TArray<AExposureProbeActor*> All;
	for (TActorIterator<AExposureProbeActor> It(World); It; ++It) { if (IsValid(*It)) { All.Add(*It); } }
	const FScopedTransaction Transaction(LOCTEXT("ClearProbes", "Delete Exposure Probes"));
	for (AExposureProbeActor* A : All) { World->EditorDestroyActor(A, true); }
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("deleted"), All.Num());
	return ToString(Obj);
}

FString UExposureProbeToolset::FullRecompute(bool bParallel)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	return S ? Report(S->Rebuild(bParallel)) : Fail(Err);
}

FString UExposureProbeToolset::UpdateNow()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	return S ? Report(S->UpdateNow()) : Fail(Err);
}

FString UExposureProbeToolset::GetLastReport()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	return S ? Report(S->GetLastReport()) : Fail(Err);
}

FString UExposureProbeToolset::GetRays(bool bIncludeSegments)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	const FExposureProbeSolver& Solver = S->GetSolver();
	TArray<TSharedPtr<FJsonValue>> Rays;
	for (int32 i = 0; i < Solver.Rays().Num(); ++i)
	{
		const FExposureRay& R = Solver.Rays()[i];
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("key"), Solver.RayKey(i));
		O->SetBoolField(TEXT("visible"), R.bVisible);
		O->SetStringField(TEXT("blocker"), R.Blocker);
		if (bIncludeSegments)
		{
			O->SetArrayField(TEXT("start"), Vec(R.Start));
			O->SetArrayField(TEXT("end"), Vec(R.End));
		}
		Rays.Add(MakeShared<FJsonValueObject>(O));
	}
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("built"), S->IsBuilt());
	Obj->SetArrayField(TEXT("rays"), Rays);
	return ToString(Obj);
}

FString UExposureProbeToolset::GetProbes()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	TArray<TSharedPtr<FJsonValue>> Probes;
	for (const FExposureProbeDesc& D : S->GetSolver().Probes())
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), D.Name);
		O->SetStringField(TEXT("team"), D.Team == EExposureTeam::TSide ? TEXT("T") : TEXT("CT"));
		O->SetArrayField(TEXT("feet"), Vec(D.Feet));
		O->SetStringField(TEXT("pose"), D.bCrouch ? TEXT("crouch") : TEXT("stand"));
		O->SetNumberField(TEXT("clearance"), D.ClearanceUnits >= FLT_MAX ? -1.0 : D.ClearanceUnits);
		Probes.Add(MakeShared<FJsonValueObject>(O));
	}
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetArrayField(TEXT("probes"), Probes);
	return ToString(Obj);
}

FString UExposureProbeToolset::MarkDesignIntent(const FString& Edge, const FString& Note)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	return S->MarkDesignIntent(Edge, Note, Err) ? Ok() : Fail(Err);
}

FString UExposureProbeToolset::ClearDesignIntent(const FString& Edge)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	return S->ClearDesignIntent(Edge, Err) ? Ok() : Fail(Err);
}

FString UExposureProbeToolset::SetDrawMode(const FString& Mode, const FString& FocusEdge, const FString& Style)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	const EExposureDrawMode M = Mode.Equals(TEXT("all"), ESearchCase::IgnoreCase) ? EExposureDrawMode::All
		: Mode.Equals(TEXT("off"), ESearchCase::IgnoreCase) ? EExposureDrawMode::Off : EExposureDrawMode::Changed;
	if (Style.Equals(TEXT("rays"), ESearchCase::IgnoreCase)) { S->SetDrawStyle(EExposureDrawStyle::Rays); }
	else if (Style.Equals(TEXT("body"), ESearchCase::IgnoreCase)) { S->SetDrawStyle(EExposureDrawStyle::Body); }
	S->SetFocusEdge(FocusEdge);
	S->SetDrawMode(M);
	return Ok();
}

FString UExposureProbeToolset::SetAutoUpdate(bool bEnabled)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	S->SetAutoUpdate(bEnabled);
	return Ok();
}

FString UExposureProbeToolset::GetSettings()
{
	using namespace ExposureProbeTools;
	const UExposureProbeSettings* Settings = GetDefault<UExposureProbeSettings>();
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(UExposureProbeSettings::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FString Value;
		It->ExportTextItem_InContainer(Value, Settings, nullptr, nullptr, PPF_None);
		Obj->SetStringField(It->GetName(), Value);
	}
	return ToString(Obj);
}

FString UExposureProbeToolset::SetSettings(const FString& SettingsJson)
{
	using namespace ExposureProbeTools;
	TSharedPtr<FJsonObject> In;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SettingsJson), In) || !In.IsValid())
	{
		return Fail(TEXT("SettingsJson must be a JSON object"));
	}
	UExposureProbeSettings* Settings = GetMutableDefault<UExposureProbeSettings>();
	TArray<FString> Bad;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : In->Values)
	{
		FProperty* P = FindFProperty<FProperty>(UExposureProbeSettings::StaticClass(), *KV.Key);
		FString Text;
		if (!P || !KV.Value.IsValid() || !KV.Value->TryGetString(Text))
		{
			double Num = 0;
			bool bBool = false;
			if (P && KV.Value.IsValid() && KV.Value->TryGetNumber(Num)) { Text = FString::SanitizeFloat(Num); }
			else if (P && KV.Value.IsValid() && KV.Value->TryGetBool(bBool)) { Text = bBool ? TEXT("True") : TEXT("False"); }
			else { Bad.Add(KV.Key); continue; }
		}
		if (!P->ImportText_InContainer(*Text, Settings, Settings, PPF_None))
		{
			Bad.Add(KV.Key);
			continue;
		}
		if (P->GetFName() == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, UnitPreset) && Settings->UnitPreset != EExposureUnitPreset::Custom)
		{
			Settings->ApplyPreset(Settings->UnitPreset);
		}
		if (P->GetFName() == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, Sampling) && Settings->Sampling != EExposureSampling::Custom)
		{
			Settings->ApplySampling(Settings->Sampling);
		}
		FPropertyChangedEvent Event(P);
		FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Settings, Event);
	}
	if (Bad.Num() > 0) { return Fail(TEXT("Could not set: ") + FString::Join(Bad, TEXT(", "))); }
	return GetSettings();
}

FString UExposureProbeToolset::SetView(const FString& Layers, const FString& Show)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	TArray<FString> Parts;
	Layers.ToLower().ParseIntoArray(Parts, TEXT(","));
	for (FString& P : Parts) { P.TrimStartAndEndInline(); }
	S->SetLayer(EExposureLayer::Probes, Parts.Contains(TEXT("probes")));
	S->SetLayer(EExposureLayer::Heatmap, Parts.Contains(TEXT("heatmap")));
	S->SetLayer(EExposureLayer::OneWay, Parts.Contains(TEXT("oneway")));
	if (Show.Equals(TEXT("delta"), ESearchCase::IgnoreCase)) { S->SetHeatShow(EExposureHeatShow::Delta); }
	else if (Show.Equals(TEXT("a"), ESearchCase::IgnoreCase)) { S->SetHeatShow(EExposureHeatShow::A); }
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("probes"), S->IsLayerOn(EExposureLayer::Probes));
	Obj->SetBoolField(TEXT("heatmap"), S->IsLayerOn(EExposureLayer::Heatmap));
	Obj->SetBoolField(TEXT("oneway"), S->IsLayerOn(EExposureLayer::OneWay));
	Obj->SetStringField(TEXT("show"), S->GetHeatShow() == EExposureHeatShow::Delta ? TEXT("delta") : TEXT("a"));
	return ToString(Obj);
}

FString UExposureProbeToolset::GetOneWay(int32 MaxSpots)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	const TArray<FExposureHeatPoint>& H = S->GetHeat();
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("samples"), H.Num());
	Obj->SetNumberField(TEXT("oneWaySpots"), S->GetOneWaySpotCount());
	int32 NewSpots = 0, Pairs = 0;
	for (int32 i = 0; i < H.Num(); ++i) { NewSpots += S->IsNewOneWaySpot(i) ? 1 : 0; Pairs += H[i].OneWaySees; }
	Obj->SetNumberField(TEXT("newSpots"), NewSpots);
	Obj->SetNumberField(TEXT("oneWayPairs"), Pairs);
	TArray<int32> Order;
	for (int32 i = 0; i < H.Num(); ++i) { if (H[i].OneWaySees > 0) { Order.Add(i); } }
	Order.Sort([&H](int32 L, int32 R) { return H[L].OneWaySees > H[R].OneWaySees; });
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (int32 n = 0; n < Order.Num() && n < FMath::Max(0, MaxSpots); ++n)
	{
		const int32 i = Order[n];
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("index"), i);
		O->SetNumberField(TEXT("x"), H[i].Feet.X); O->SetNumberField(TEXT("y"), H[i].Feet.Y); O->SetNumberField(TEXT("z"), H[i].Feet.Z);
		O->SetNumberField(TEXT("sees"), H[i].OneWaySees);
		O->SetNumberField(TEXT("seenBy"), H[i].OneWaySeenBy);
		O->SetBoolField(TEXT("new"), S->IsNewOneWaySpot(i));
		O->SetNumberField(TEXT("firstTarget"), S->GetOneWayTargets(i).Num() ? S->GetOneWayTargets(i)[0] : -1);
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	Obj->SetArrayField(TEXT("spots"), Arr);
	if (S->HasProbeOneWay())
	{
		TArray<TSharedPtr<FJsonValue>> Names;
		for (const FString& N : S->GetProbeOneWayNames()) { Names.Add(MakeShared<FJsonValueString>(N)); }
		Obj->SetArrayField(TEXT("selected"), Names);
		Obj->SetNumberField(TEXT("threatCells"), S->GetThreatCellCount());
		Obj->SetNumberField(TEXT("advantageCells"), S->GetAdvantageCellCount());
	}
	return ToString(Obj);
}

FString UExposureProbeToolset::SetFloorFilter(const FString& Filter)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	const UEnum* E = StaticEnum<EExposureFloorFilter>();
	const int64 V = E->GetValueByNameString(Filter.IsEmpty() ? TEXT("None") : Filter);
	if (V == INDEX_NONE) { return Fail(TEXT("Unknown filter ") + Filter); }
	if (S->GetFloorFilter() != EExposureFloorFilter(V)) { S->SetFloorFilter(EExposureFloorFilter(V)); }
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("filter"), E->GetNameStringByValue(int64(S->GetFloorFilter())));
	TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
	for (int32 f = 1; f <= int32(EExposureFloorFilter::Advantage); ++f)
	{
		Counts->SetNumberField(E->GetNameStringByValue(f), S->FloorFilterCount(EExposureFloorFilter(f)));
	}
	Obj->SetObjectField(TEXT("counts"), Counts);
	TArray<TSharedPtr<FJsonValue>> Spots;
	if (S->GetFloorFilter() != EExposureFloorFilter::None)
	{
		const TArray<int32> Idx = S->FloorFilterSpots(S->GetFloorFilter());
		for (int32 n = 0; n < Idx.Num() && n < 10; ++n)
		{
			const FVector& F = S->GetHeat()[Idx[n]].Feet;
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("index"), Idx[n]);
			O->SetNumberField(TEXT("x"), F.X); O->SetNumberField(TEXT("y"), F.Y); O->SetNumberField(TEXT("z"), F.Z);
			Spots.Add(MakeShared<FJsonValueObject>(O));
		}
	}
	Obj->SetArrayField(TEXT("spots"), Spots);
	return ToString(Obj);
}

FString UExposureProbeToolset::ExportReport()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	FString Path;
	if (!S->ExportReport(Path, Err)) { return Fail(Err); }
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("markdown"), Path);
	const FString JsonPath = FPaths::ChangeExtension(Path, TEXT("json"));
	Obj->SetStringField(TEXT("json"), JsonPath);
	FString Json;
	TSharedPtr<FJsonObject> Report;
	if (FFileHelper::LoadFileToString(Json, *JsonPath) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report) && Report.IsValid())
	{
		Obj->SetObjectField(TEXT("report"), Report);
	}
	return ToString(Obj);
}

namespace ExposureProbeTools
{
	static TSharedRef<FJsonObject> HeatSummary(const UExposureProbeSubsystem* S, bool bSamples)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		const TArray<FExposureHeatPoint>& H = S->GetHeat();
		Obj->SetNumberField(TEXT("samples"), H.Num());
		Obj->SetNumberField(TEXT("spacingCm"), S->GetHeatSpacingCm());
		Obj->SetNumberField(TEXT("radiusCm"), S->GetHeatRadiusCm());
		Obj->SetNumberField(TEXT("rays"), double(S->GetHeatRays()));
		Obj->SetNumberField(TEXT("seconds"), S->GetHeatSeconds());
		Obj->SetBoolField(TEXT("stale"), S->IsHeatStale());
		Obj->SetBoolField(TEXT("canShowDelta"), S->CanShowHeatDelta());
		float MinA = 0.f, MaxA = 0.f;
		double Sum = 0.0;
		int32 Crouch = 0;
		for (const FExposureHeatPoint& P : H) { MinA = FMath::Min(MinA, P.A); MaxA = FMath::Max(MaxA, P.A); Sum += P.A; Crouch += P.bCrouch ? 1 : 0; }
		Obj->SetNumberField(TEXT("minA"), MinA);
		Obj->SetNumberField(TEXT("maxA"), MaxA);
		Obj->SetNumberField(TEXT("meanA"), H.Num() ? Sum / H.Num() : 0.0);
		Obj->SetNumberField(TEXT("crouch"), Crouch);
		Obj->SetNumberField(TEXT("oneWaySpots"), S->GetOneWaySpotCount());
		if (bSamples)
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (int32 i = 0; i < H.Num(); ++i)
			{
				TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetNumberField(TEXT("x"), H[i].Feet.X); O->SetNumberField(TEXT("y"), H[i].Feet.Y); O->SetNumberField(TEXT("z"), H[i].Feet.Z);
				O->SetNumberField(TEXT("a"), H[i].A); O->SetNumberField(TEXT("vout"), H[i].Vout); O->SetNumberField(TEXT("vin"), H[i].Vin);
				O->SetBoolField(TEXT("crouch"), H[i].bCrouch);
				float D = 0.f;
				if (S->GetHeatShow() == EExposureHeatShow::Delta && S->HeatValue(i, D)) { O->SetNumberField(TEXT("delta"), D); }
				Arr.Add(MakeShared<FJsonValueObject>(O));
			}
			Obj->SetArrayField(TEXT("points"), Arr);
		}
		return Obj;
	}

	static TSharedRef<FJsonObject> BaselineSummary(const UExposureProbeSubsystem* S)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("active"), S->HasBaseline());
		Obj->SetStringField(TEXT("name"), S->HasBaseline() ? S->GetBaseline().Name : FString());
		Obj->SetBoolField(TEXT("comparable"), S->IsBaselineComparable());
		Obj->SetBoolField(TEXT("hasHeat"), S->HasBaseline() && S->GetBaseline().Heat.Num() > 0);
		Obj->SetBoolField(TEXT("inputsMatch"), S->BaselineInputsMatch());
		Obj->SetBoolField(TEXT("reproMismatch"), S->IsBaselineReproMismatch());
		Obj->SetStringField(TEXT("inputDiff"), S->GetBaselineInputDiff());
		TArray<TSharedPtr<FJsonValue>> Geom;
		for (const FString& G : S->GetBaselineGeomChanges()) { Geom.Add(MakeShared<FJsonValueString>(G)); }
		Obj->SetArrayField(TEXT("geometryChanges"), Geom);
		TArray<TSharedPtr<FJsonValue>> Probes;
		for (const FString& P : S->GetBaselineProbeChanges()) { Probes.Add(MakeShared<FJsonValueString>(P)); }
		Obj->SetArrayField(TEXT("probeChanges"), Probes);
		int32 NewOneWay = 0;
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FExposureEdgeChange& C : S->GetBaselineChanges())
		{
			NewOneWay += C.bNewOneWay ? 1 : 0;
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("edge"), C.Edge);
			O->SetBoolField(TEXT("hadBefore"), C.bHadBefore);
			O->SetNumberField(TEXT("aBefore"), C.Before.A);
			O->SetNumberField(TEXT("aAfter"), C.After.A);
			O->SetBoolField(TEXT("newOneWay"), C.bNewOneWay);
			O->SetStringField(TEXT("kind"), StaticEnum<EExposureChangeKind>()->GetNameStringByValue(int64(C.Kind)));
			O->SetBoolField(TEXT("expected"), C.bExpected);
			O->SetNumberField(TEXT("distanceCm"), C.DistanceCm);
			O->SetBoolField(TEXT("far"), C.bFar);
			Arr.Add(MakeShared<FJsonValueObject>(O));
		}
		Obj->SetNumberField(TEXT("changed"), Arr.Num());
		Obj->SetNumberField(TEXT("newOneWay"), NewOneWay);
		Obj->SetNumberField(TEXT("resolved"), S->GetBaselineResolved());
		Obj->SetNumberField(TEXT("missing"), S->GetBaselineMissing());
		Obj->SetArrayField(TEXT("changes"), Arr);
		return Obj;
	}
}

FString UExposureProbeToolset::ComputeHeatmap()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	FString Message;
	if (!S->ComputeHeatmap(Message)) { return Fail(Message); }
	TSharedRef<FJsonObject> Obj = HeatSummary(S, false);
	Obj->SetStringField(TEXT("message"), Message);
	return ToString(Obj);
}

FString UExposureProbeToolset::GetHeatmap(bool bIncludeSamples)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	return ToString(HeatSummary(S, bIncludeSamples));
}

FString UExposureProbeToolset::SaveBaseline(const FString& Name)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	if (!S->SaveBaseline(Name, Err)) { return Fail(Err); }
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& N : S->ListBaselines()) { Arr.Add(MakeShared<FJsonValueString>(N)); }
	Obj->SetArrayField(TEXT("baselines"), Arr);
	Obj->SetStringField(TEXT("dir"), S->BaselineDir());
	return ToString(Obj);
}

FString UExposureProbeToolset::SetBaseline(const FString& Name)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	if (!S->SetActiveBaseline(Name, Err)) { return Fail(Err); }
	return ToString(BaselineSummary(S));
}

FString UExposureProbeToolset::ListBaselines()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	TSharedRef<FJsonObject> Obj = BaselineSummary(S);
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& N : S->ListBaselines()) { Arr.Add(MakeShared<FJsonValueString>(N)); }
	Obj->SetArrayField(TEXT("baselines"), Arr);
	return ToString(Obj);
}

FString UExposureProbeToolset::ComputeSmoke(const FString& Observer)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	FString Message;
	if (!S->ComputeSmoke(Observer, Message)) { return Fail(Message); }
	const FExposureSmokeResult& R = S->GetSmoke();
	const float U = GetDefault<UExposureProbeSettings>()->UnitToCm;
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("observer"), R.Observer);
	Obj->SetNumberField(TEXT("opponents"), R.Opponents);
	Obj->SetNumberField(TEXT("opponentsSeen"), R.OpponentsSeen);
	Obj->SetNumberField(TEXT("visiblePoints"), R.Ends.Num());
	Obj->SetNumberField(TEXT("candidates"), R.Candidates);
	Obj->SetNumberField(TEXT("cc"), R.Smokes.Num());
	Obj->SetBoolField(TEXT("reached"), R.bReached);
	Obj->SetNumberField(TEXT("coverage"), R.Coverage);
	Obj->SetNumberField(TEXT("tau"), R.Tau);
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FExposureSmokeResult::FSmoke& Sm : R.Smokes)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetArrayField(TEXT("landing"), Vec(Sm.Landing));
		O->SetNumberField(TEXT("newlyCut"), Sm.NewlyCut);
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	Obj->SetArrayField(TEXT("smokes"), Arr);
	Obj->SetNumberField(TEXT("rt"), R.RT);
	Obj->SetBoolField(TEXT("structural"), R.bStructural);
	Obj->SetBoolField(TEXT("navPath"), R.bNavPath);
	Obj->SetNumberField(TEXT("detourTested"), R.DetourTested);
	if (R.bFoundAlt)
	{
		Obj->SetArrayField(TEXT("detourSpot"), Vec(R.AltFeet));
		Obj->SetNumberField(TEXT("detourUnits"), R.DetourUnits);
		Obj->SetNumberField(TEXT("detourSeconds"), R.DetourSeconds);
		Obj->SetNumberField(TEXT("detourShare"), R.AltShare);
		Obj->SetNumberField(TEXT("detourA"), R.AltA);
	}
	if (R.bHaveA) { Obj->SetNumberField(TEXT("observerA"), R.AObserver); }
	Obj->SetNumberField(TEXT("radiusUnits"), R.RadiusCm / U);
	Obj->SetNumberField(TEXT("ms"), R.Ms);
	Obj->SetStringField(TEXT("summary"), Message);
	return ToString(Obj);
}

namespace ExposureProbeTools
{
	static TSharedRef<FJsonObject> SmokeJson(const FExposureSmokeResult& R, const FString& Summary)
	{
		const float U = GetDefault<UExposureProbeSettings>()->UnitToCm;
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("observer"), R.Observer);
		Obj->SetBoolField(TEXT("placed"), R.bPlaced);
		Obj->SetNumberField(TEXT("visiblePoints"), R.Ends.Num());
		Obj->SetNumberField(TEXT("opponentsSeen"), R.OpponentsSeen);
		Obj->SetNumberField(TEXT("coverage"), R.Coverage);
		Obj->SetBoolField(TEXT("reached"), R.bReached);
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FExposureSmokeResult::FSmoke& Sm : R.Smokes)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), Sm.Name);
			O->SetArrayField(TEXT("landing"), Vec(Sm.Landing));
			O->SetNumberField(TEXT("radiusUnits"), Sm.RadiusCm / U);
			O->SetNumberField(TEXT("duration"), Sm.Duration);
			O->SetNumberField(TEXT("hits"), Sm.Hits);
			O->SetNumberField(TEXT("newlyCut"), Sm.NewlyCut);
			Arr.Add(MakeShared<FJsonValueObject>(O));
		}
		Obj->SetArrayField(TEXT("smokes"), Arr);
		Obj->SetNumberField(TEXT("rt"), R.RT);
		Obj->SetBoolField(TEXT("stillSees"), R.bStillSees);
		Obj->SetBoolField(TEXT("structural"), R.bStructural);
		Obj->SetBoolField(TEXT("recoverySkipped"), R.bRecoverySkipped);
		Obj->SetNumberField(TEXT("wait"), R.WaitSeconds);
		if (R.bFoundAlt)
		{
			Obj->SetArrayField(TEXT("detourSpot"), Vec(R.AltFeet));
			Obj->SetNumberField(TEXT("detourUnits"), R.DetourUnits);
			Obj->SetNumberField(TEXT("detourShare"), R.AltShare);
		}
		Obj->SetNumberField(TEXT("ms"), R.Ms);
		Obj->SetStringField(TEXT("summary"), Summary);
		return Obj;
	}

	static TSharedRef<FJsonObject> PlacedJson(const AExposureSmokeActor* A)
	{
		const FExposureSmokeShape Sh = A->GetShape();
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), A->GetSmokeName());
		O->SetArrayField(TEXT("landing"), Vec(A->GetActorLocation()));
		O->SetArrayField(TEXT("centre"), Vec(A->GetCentre()));
		O->SetNumberField(TEXT("radius"), Sh.Radius);
		O->SetNumberField(TEXT("lift"), Sh.Lift);
		O->SetNumberField(TEXT("duration"), Sh.Duration);
		O->SetStringField(TEXT("size"), StaticEnum<EExposureSmokeSize>()->GetNameStringByValue(int64(A->Size)));
		O->SetBoolField(TEXT("enabled"), A->bEnabled);
		return O;
	}
}

FString UExposureProbeToolset::PlaceSmokes(const FString& SmokesJson)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	TArray<TSharedPtr<FJsonValue>> In;
	if (!ParseArray(SmokesJson, In)) { return Fail(TEXT("SmokesJson must be a JSON array")); }
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const TSharedPtr<FJsonValue>& V : In)
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (!V.IsValid() || !V->TryGetObject(O)) { continue; }
		const FVector P((*O)->GetNumberField(TEXT("x")), (*O)->GetNumberField(TEXT("y")), (*O)->GetNumberField(TEXT("z")));
		FString Message;
		AExposureSmokeActor* A = S->PlaceSmokeAt(P, Message);
		if (!A) { return Fail(Message); }
		FString Size;
		if ((*O)->TryGetStringField(TEXT("size"), Size))
		{
			Size = Size.ToLower();
			A->Size = Size == TEXT("cs2") ? EExposureSmokeSize::CS2 : Size == TEXT("brimstone") ? EExposureSmokeSize::ValorantBrimstone
				: Size == TEXT("omen") ? EExposureSmokeSize::ValorantOmen : Size == TEXT("astra") ? EExposureSmokeSize::ValorantAstra
				: Size == TEXT("custom") ? EExposureSmokeSize::Custom : EExposureSmokeSize::Shared;
			double Num = 0.0;
			if ((*O)->TryGetNumberField(TEXT("radius"), Num)) { A->CustomSmoke.Radius = float(Num); }
			if ((*O)->TryGetNumberField(TEXT("lift"), Num)) { A->CustomSmoke.Lift = float(Num); }
			if ((*O)->TryGetNumberField(TEXT("duration"), Num)) { A->CustomSmoke.Duration = float(Num); }
			A->RefreshShape();
			S->RefreshSmokes();
		}
		TSharedRef<FJsonObject> J = PlacedJson(A);
		J->SetStringField(TEXT("message"), Message);
		Out.Add(MakeShared<FJsonValueObject>(J));
	}
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetArrayField(TEXT("placed"), Out);
	return ToString(Obj);
}

FString UExposureProbeToolset::GetPlacedSmokes()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const AExposureSmokeActor* A : S->GetPlacedSmokes(false)) { Out.Add(MakeShared<FJsonValueObject>(PlacedJson(A))); }
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetArrayField(TEXT("smokes"), Out);
	return ToString(Obj);
}

FString UExposureProbeToolset::MoveSmoke(const FString& Name, float X, float Y, float Z)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	for (AExposureSmokeActor* A : S->GetPlacedSmokes(false))
	{
		if (A->GetSmokeName() != Name) { continue; }
		const FScopedTransaction Transaction(LOCTEXT("MoveSmoke", "Move Smoke"));
		A->Modify();
		A->SetActorLocation(FVector(X, Y, Z));
		A->PostEditMove(true);
		return ToString(PlacedJson(A));
	}
	return Fail(FString::Printf(TEXT("No placed smoke named %s"), *Name));
}

FString UExposureProbeToolset::ClearPlacedSmokes()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("deleted"), S->ClearPlacedSmokes());
	return ToString(Obj);
}

FString UExposureProbeToolset::EvaluateSmokes(const FString& Observer)
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	FString Message;
	if (!S->EvaluatePlacedSmokes(Observer, Message)) { return Fail(Message); }
	return ToString(SmokeJson(S->GetSmoke(), Message));
}

FString UExposureProbeToolset::PlacePlanSmokes()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	FString Message;
	const int32 N = S->PlacePlanSmokes(Message);
	if (N == 0) { return Fail(Message); }
	TSharedRef<FJsonObject> Obj = SmokeJson(S->GetSmoke(), S->SmokeSummary());
	Obj->SetNumberField(TEXT("placedCount"), N);
	return ToString(Obj);
}

FString UExposureProbeToolset::ClearSmoke()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	S->ClearSmoke();
	return Ok();
}

FString UExposureProbeToolset::GetSmoke()
{
	using namespace ExposureProbeTools;
	FString Err;
	UExposureProbeSubsystem* S = Subsystem(Err);
	if (!S) { return Fail(Err); }
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("shown"), S->HasSmoke());
	if (S->HasSmoke())
	{
		const FExposureSmokeResult& R = S->GetSmoke();
		Obj->SetStringField(TEXT("observer"), R.Observer);
		Obj->SetNumberField(TEXT("cc"), R.Smokes.Num());
		Obj->SetBoolField(TEXT("reached"), R.bReached);
		Obj->SetNumberField(TEXT("rt"), R.RT);
		Obj->SetBoolField(TEXT("stale"), S->IsSmokeStale());
		Obj->SetBoolField(TEXT("placed"), R.bPlaced);
		Obj->SetNumberField(TEXT("coverage"), R.Coverage);
		Obj->SetStringField(TEXT("summary"), S->SmokeSummary());
	}
	return ToString(Obj);
}

#undef LOCTEXT_NAMESPACE
