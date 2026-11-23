#include "ExposureProbeSubsystem.h"

#include "ExposureProbeActor.h"
#include "ExposureProbeSettings.h"
#include "ExposureProbeText.h"
#include "ExposureSmokeActor.h"
#include "ExposureSmokeComponent.h"
#include "CanvasItem.h"
#include "Components/LineBatchComponent.h"
#include "Components/LocalFogVolumeComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "LevelEditorViewport.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Rendering/SlateRenderer.h"
#include "ScopedTransaction.h"
#include "Styling/CoreStyle.h"
#include "UObject/Package.h"

namespace ExposureSmoke
{
	static constexpr uint32 BatchId = 0x45585053; // 'EXPS'
	/** Detour spots tested for visibility at most, nearest by path first. */
	static constexpr int32 MaxDetourTests = 400;

	/** The segment A-B passes through the sphere. */
	static bool Cuts(const FVector& A, const FVector& B, const FVector& C, float R)
	{
		const FVector V = B - A;
		const double L = V.SizeSquared();
		const double T = L > 0.0 ? FMath::Clamp(FVector::DotProduct(C - A, V) / L, 0.0, 1.0) : 0.0;
		return FVector::DistSquared(A + T * V, C) <= double(R) * R;
	}

	static bool CutByAny(const FVector& A, const FVector& B, const TArray<FExposureSmokeResult::FSmoke>& Smokes)
	{
		for (const FExposureSmokeResult::FSmoke& Sm : Smokes) { if (Cuts(A, B, Sm.Centre, Sm.RadiusCm)) { return true; } }
		return false;
	}

	static FString Pct(float V) { return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(V * 100.f)); }
	static FString Sec(float V) { return FString::Printf(TEXT("%.1f"), V); }

	static FString Key(const FVector& V, float R)
	{
		return FString::Printf(TEXT("%.0f,%.0f,%.0f,%.0f"), V.X, V.Y, V.Z, R);
	}
}

// ---------------------------------------------------------------------------------------------
// Shared by the plan and the placed smokes

bool UExposureProbeSubsystem::PrepareSmoke(const FString& InObserver, bool bAllowFloor, FExposureSmokeResult& R, int32& OutProbe, FString& OutMessage)
{
	UWorld* World = GetEditorWorld();
	if (!World || (GEditor && GEditor->PlayWorld))
	{
		OutMessage = EPS("先停止运行", "Stop Play In Editor first");
		return false;
	}
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	FString Observer = InObserver;
	if (Observer.IsEmpty() && SelectedProbes.Num() > 0) { Observer = SelectedProbes[0]; }
	if (Observer.IsEmpty()) { Observer = LastSmokeObserver; }
	if (Observer.IsEmpty())
	{
		OutMessage = EPS("请先选中一个探针：烟雾按该探针对对方探针的视线计算", "Select a probe first: smoke is measured against its sightlines to the other side");
		return false;
	}
	if (!bBuilt || HasPendingUpdate()) { RunUpdate(!bBuilt, S->bParallelTraces, true); }
	const int32 O = Solver.FindProbe(Observer);
	if (!bBuilt || O == INDEX_NONE)
	{
		OutMessage = FString::Format(*EPS("找不到探针 {0}", "No probe named {0}"), { Observer });
		return false;
	}
	const FExposureProbeDesc& D = Solver.Probes()[O];
	R.Observer = Observer;
	R.Team = D.Team;
	R.Feet = D.Feet;
	R.Eye = D.Feet + FVector(0.f, 0.f, Solver.BodyOf(O).EyeCm(D.bCrouch));
	R.RadiusCm = S->SmokeRadius * S->UnitToCm;
	R.Tau = S->SmokeCoverage;
	R.WaitSeconds = S->SmokeDuration;

	// Targets: the opposing bodies' sample points the observer sees now.
	const int32 K = Solver.PointsPerBody();
	for (int32 c = 0; c < Solver.Probes().Num(); ++c)
	{
		if (Solver.Probes()[c].Team == D.Team) { continue; }
		const int32 Base = Solver.PairBase(O, c);
		if (Base == INDEX_NONE) { continue; }
		++R.Opponents;
		bool bSeen = false;
		for (int32 p = 0; p < K; ++p)
		{
			if (!Solver.PointValid(c, p)) { continue; }
			++R.TargetPoints;
			// Without the placed smokes: they are what is evaluated.
			if (Solver.Rays()[Base + p].bClear)
			{
				R.Ends.Add(Solver.Rays()[Base + p].End);
				bSeen = true;
			}
		}
		R.OpponentsSeen += bSeen ? 1 : 0;
	}
	if (R.Opponents == 0)
	{
		OutMessage = EPS("关卡里没有对方阵营的探针", "There is no probe of the other side");
		return false;
	}
	if (R.Ends.Num() == 0)
	{
		OutMessage = FString::Format(*EPS("{0} 看不见任何对方探针，无需烟雾", "{0} sees no opposing probe; no smoke is needed"), { Observer });
		return false;
	}

	// Landing points and detour spots: the standable floor of the floor analysis.
	if (bAllowFloor && (!HasHeat() || IsHeatStale()))
	{
		const bool bHeatWasOn = bShowHeat, bOneWayWasOn = bShowOneWay;
		FString HeatMsg;
		if (!ComputeHeatmap(HeatMsg))
		{
			OutMessage = EPS("地面分析没有完成：", "The floor analysis did not finish: ") + HeatMsg;
			return false;
		}
		bShowHeat = bHeatWasOn;
		bShowOneWay = bOneWayWasOn;
		RedrawHeat();
	}
	OutProbe = O;
	return true;
}

void UExposureProbeSubsystem::SolveRecovery(FExposureSmokeResult& R, int32 Probe)
{
	using namespace ExposureSmoke;
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	UWorld* World = GetEditorWorld();
	const float U = S->UnitToCm;
	const int32 NT = R.Ends.Num();
	if (R.Smokes.Num() == 0 || NT == 0 || !World)
	{
		return;
	}
	// Placed smokes can be of different games: RT never exceeds the time until the last one clears.
	R.WaitSeconds = 0.f;
	for (const FExposureSmokeResult::FSmoke& Sm : R.Smokes) { R.WaitSeconds = FMath::Max(R.WaitSeconds, Sm.Duration); }
	// Placed smokes that leave enough open: nothing to recover. A plan always searches for the detour.
	const float Open = 1.f - R.Coverage;
	if (R.bPlaced && Open >= S->SmokeRecoverShare)
	{
		R.bStillSees = true;
		R.RT = 0.f;
		return;
	}
	if (!HasHeat())
	{
		R.bRecoverySkipped = true;
		R.RT = R.WaitSeconds;
		return;
	}
	const FExposureProbeDesc& D = Solver.Probes()[Probe];
	const FExposureBodyModel& Model = Solver.Model();
	const int32 Here = NearestHeat(D.Feet);
	R.bHaveA = Here != INDEX_NONE;
	R.AObserver = R.bHaveA ? Heat[Here].A : 0.f;
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	const ANavigationData* Nav = NavSys ? NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) : nullptr;
	const FVector Extent(60.f, 60.f, 120.f);
	FNavLocation From;
	R.bNavPath = Nav && NavSys->ProjectPointToNavigation(D.Feet, From, Extent, Nav);
	struct FAlt { int32 Idx; double Len; FVector End; };
	TArray<FAlt> Alts;
	const float RetreatCm = S->SmokeRetreatRadius * U;
	auto Collect = [&](bool bNav)
	{
		for (int32 i = 0; i < Heat.Num(); ++i)
		{
			if (i == Here) { continue; }
			const double Dist = FVector::Dist(Heat[i].Feet, D.Feet);
			if (Dist > RetreatCm || Dist < 0.5 * HeatSpacingCm) { continue; }
			if (R.bHaveA && Heat[i].A < R.AObserver - S->SmokeRecoverDeltaA) { continue; }
			FAlt A{ i, Dist, Heat[i].Feet };
			if (bNav)
			{
				FNavLocation To;
				FVector::FReal Len = 0.0;
				if (!NavSys->ProjectPointToNavigation(Heat[i].Feet, To, Extent, Nav)
					|| NavSys->GetPathLength(From.Location, To.Location, Len, Nav) != ENavigationQueryResult::Success)
				{
					continue;
				}
				A.Len = Len;
				A.End = To.Location;
			}
			Alts.Add(A);
		}
	};
	Collect(R.bNavPath);
	// The observer stands on a nav island no path leaves (e.g. a box top it jumped onto): use straight lines instead.
	if (R.bNavPath && Alts.Num() == 0)
	{
		R.bNavPath = false;
		Collect(false);
	}
	Alts.Sort([](const FAlt& L, const FAlt& Rr) { return L.Len < Rr.Len; });
	R.DetourTested = FMath::Min(Alts.Num(), MaxDetourTests);
	for (int32 a = 0; a < R.DetourTested; ++a)
	{
		const FExposureHeatPoint& H = Heat[Alts[a].Idx];
		const FVector Eye = H.Feet + FVector(0.f, 0.f, HeatModel.EyeCm(H.bCrouch));
		int32 Vis = 0;
		for (int32 t = 0; t < NT; ++t)
		{
			if (CutByAny(Eye, R.Ends[t], R.Smokes)) { continue; }
			AActor* Ignored = nullptr;
			Vis += FExposureProbeSolver::Trace(World, Eye, R.Ends[t], Model, Ignored) ? 1 : 0;
		}
		const float Share = float(Vis) / NT;
		if (Share >= S->SmokeRecoverShare)
		{
			R.bFoundAlt = true;
			R.AltFeet = H.Feet;
			R.AltEye = Eye;
			R.AltShare = Share;
			R.AltA = H.A;
			R.DetourUnits = float(Alts[a].Len / U);
			if (R.bNavPath)
			{
				if (UNavigationPath* P = UNavigationSystemV1::FindPathToLocationSynchronously(World, From.Location, Alts[a].End))
				{
					R.Path = P->PathPoints;
				}
			}
			if (R.Path.Num() < 2) { R.Path = { D.Feet, H.Feet }; }
			break;
		}
	}
	const float Detour = R.bFoundAlt ? R.DetourUnits / FMath::Max(S->SmokeMoveSpeed, 1.f) + S->SmokeReacquireSeconds : TNumericLimits<float>::Max();
	R.DetourSeconds = R.bFoundAlt ? Detour : 0.f;
	R.RT = FMath::Min(R.WaitSeconds, Detour);
	R.bStructural = !R.bFoundAlt;
}

void UExposureProbeSubsystem::FinishSmoke(FExposureSmokeResult&& R, FString& OutMessage)
{
	LastSmokeObserver = R.Observer;
	Smoke = MoveTemp(R);
	bHasSmoke = true;
	bSmokeStale = false;
	bSmokeDirty = true;
	RedrawSmoke();
	OnReportUpdated.Broadcast();
	OutMessage = SmokeSummary();
}

// ---------------------------------------------------------------------------------------------
// Smoke plan: the fewest smokes on the floor that cut the observer's sightlines

bool UExposureProbeSubsystem::ComputeSmoke(const FString& InObserver, FString& OutMessage)
{
	using namespace ExposureSmoke;
	const double T0 = FPlatformTime::Seconds();
	FExposureSmokeResult R;
	int32 O = INDEX_NONE;
	if (!PrepareSmoke(InObserver, true, R, O, OutMessage))
	{
		return false;
	}
	UWorld* World = GetEditorWorld();
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const float U = S->UnitToCm;
	const int32 NT = R.Ends.Num();
	const float LiftCm = S->SmokeLift * U;
	const float ClearCm = S->SmokeClearance * U;
	const float MinCm = S->SmokeMinDistance * U;
	const float SearchCm = S->SmokeSearchRadius * U;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExposureSmoke), S->bTraceComplex);
	struct FCand
	{
		FVector Landing;
		TArray<int32> Hits;
	};
	TArray<FCand> Cands;
	for (int32 i = 0; i < Heat.Num(); ++i)
	{
		const FVector L = Heat[i].Feet;
		const double DEye = FVector::Dist2D(L, R.Eye);
		if (DEye <= MinCm) { continue; }
		bool bNear = DEye <= SearchCm;
		for (int32 t = 0; t < NT && !bNear; ++t) { bNear = FVector::Dist2D(L, R.Ends[t]) <= SearchCm; }
		if (!bNear) { continue; }
		if (World->LineTraceTestByChannel(L + FVector(0.f, 0.f, 2.f), L + FVector(0.f, 0.f, ClearCm), S->TraceChannel, Params)) { continue; }
		++R.Candidates;
		FCand C;
		C.Landing = L;
		const FVector Centre = L + FVector(0.f, 0.f, LiftCm);
		for (int32 t = 0; t < NT; ++t) { if (Cuts(R.Eye, R.Ends[t], Centre, R.RadiusCm)) { C.Hits.Add(t); } }
		if (C.Hits.Num() > 0) { Cands.Add(MoveTemp(C)); }
	}

	// Greedy set cover: the smoke that cuts the most still-visible rays, until the share reaches the threshold.
	TArray<bool> Cut;
	Cut.Init(false, NT);
	int32 NCut = 0;
	while (R.Smokes.Num() < S->SmokeMaxCount && NCut < R.Tau * NT - 1e-6)
	{
		int32 Best = INDEX_NONE, BestGain = 0;
		for (int32 c = 0; c < Cands.Num(); ++c)
		{
			int32 Gain = 0;
			for (const int32 t : Cands[c].Hits) { Gain += Cut[t] ? 0 : 1; }
			if (Gain > BestGain) { BestGain = Gain; Best = c; }
		}
		if (Best == INDEX_NONE) { break; }
		FExposureSmokeResult::FSmoke& Sm = R.Smokes.AddDefaulted_GetRef();
		Sm.Landing = Cands[Best].Landing;
		Sm.Centre = Sm.Landing + FVector(0.f, 0.f, LiftCm);
		Sm.RadiusCm = R.RadiusCm;
		Sm.Duration = S->SmokeDuration;
		Sm.Hits = Cands[Best].Hits.Num();
		Sm.NewlyCut = BestGain;
		for (const int32 t : Cands[Best].Hits) { if (!Cut[t]) { Cut[t] = true; ++NCut; } }
	}
	R.Cut = Cut;
	R.Coverage = float(NCut) / NT;
	R.bReached = R.Smokes.Num() > 0 && NCut >= R.Tau * NT - 1e-6;
	SolveRecovery(R, O);
	R.Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
	FinishSmoke(MoveTemp(R), OutMessage);
	UE_LOG(LogExposureProbe, Log, TEXT("Smoke plan for %s: %s"), *Smoke.Observer, *OutMessage.Replace(TEXT("\n"), TEXT(" | ")));
	return true;
}

// ---------------------------------------------------------------------------------------------
// Placed smokes

TArray<FExposureProbeSolver::FSmokeSphere> UExposureProbeSubsystem::ProbeSmokes(FString* OutKey, TMap<FString, FBox>* OutBoxes, TMap<FString, FString>* OutLabels) const
{
	const bool bOn = GetDefault<UExposureProbeSettings>()->bSmokesBlockProbes;
	TArray<FExposureProbeSolver::FSmokeSphere> Out;
	FString Key = bOn ? TEXT("on") : TEXT("off");
	for (const AExposureSmokeActor* A : GetPlacedSmokes(true))
	{
		const FVector C = A->GetCentre();
		const float R = A->GetRadiusCm();
		Key += TEXT("|") + ExposureSmoke::Key(C, R);
		if (!bOn) { continue; }
		Out.Add({ C, R, A->GetSmokeName() });
		const FString Path = A->GetPathName();
		if (OutBoxes) { OutBoxes->Add(Path, FBox(C - FVector(R), C + FVector(R))); }
		if (OutLabels) { OutLabels->Add(Path, A->GetSmokeName()); }
	}
	if (OutKey) { *OutKey = Key; }
	return Out;
}

void UExposureProbeSubsystem::LiveSmokeUpdate(const TArray<FExposureProbeSolver::FSmokeSphere>& Smokes, const FString& Key)
{
	if (!bBuilt)
	{
		return;
	}
	if (!bLiveSmoke)
	{
		bLiveSmoke = true;
		LiveBefore = Edges;
		LiveVisible.Reset();
		LiveBlocker.Reset();
		for (const FExposureRay& R : Solver.Rays())
		{
			LiveVisible.Add(R.bVisible);
			LiveBlocker.Add(R.Blocker);
		}
	}
	Solver.ApplySmokes(Smokes);
	LiveSmokeKey = Key;
	Edges = Solver.ComputeEdges(GetDefault<UExposureProbeSettings>()->OneWayThreshold);
	ApplyDesignIntent(Edges);
	LastReport.Edges = Edges;
	Redraw();
	OnReportUpdated.Broadcast();
}

void UExposureProbeSubsystem::UpdateSmokeCutsLive()
{
	using namespace ExposureSmoke;
	if (!bHasSmoke || !Smoke.bPlaced)
	{
		return;
	}
	FExposureSmokeResult& R = Smoke;
	R.Smokes.Reset();
	const int32 NT = R.Ends.Num();
	TArray<bool> Cut;
	Cut.Init(false, NT);
	int32 NCut = 0;
	for (const AExposureSmokeActor* A : GetPlacedSmokes(true))
	{
		FExposureSmokeResult::FSmoke& Sm = R.Smokes.AddDefaulted_GetRef();
		Sm.Name = A->GetSmokeName();
		Sm.Landing = A->GetActorLocation();
		Sm.Centre = A->GetCentre();
		Sm.RadiusCm = A->GetRadiusCm();
		Sm.Duration = A->GetShape().Duration;
		for (int32 t = 0; t < NT; ++t)
		{
			if (!Cuts(R.Eye, R.Ends[t], Sm.Centre, Sm.RadiusCm)) { continue; }
			++Sm.Hits;
			if (!Cut[t]) { Cut[t] = true; ++NCut; ++Sm.NewlyCut; }
		}
	}
	R.Cut = Cut;
	R.Coverage = NT > 0 ? float(NCut) / NT : 0.f;
	R.bReached = NCut >= R.Tau * NT - 1e-6;
	// The detour no longer holds once the smokes moved; it is found again after the drag.
	R.bFoundAlt = false;
	R.Path.Reset();
}

TArray<AExposureSmokeActor*> UExposureProbeSubsystem::GetPlacedSmokes(bool bEnabledOnly) const
{
	TArray<AExposureSmokeActor*> Out;
	if (UWorld* World = GetEditorWorld())
	{
		for (TActorIterator<AExposureSmokeActor> It(World); It; ++It)
		{
			if (IsValid(*It) && (!bEnabledOnly || It->bEnabled)) { Out.Add(*It); }
		}
	}
	Out.Sort([](const AExposureSmokeActor& A, const AExposureSmokeActor& B) { return A.GetSmokeName() < B.GetSmokeName(); });
	return Out;
}

bool UExposureProbeSubsystem::EvaluatePlacedSmokes(const FString& Observer, FString& OutMessage)
{
	return EvaluatePlaced(Observer, true, OutMessage);
}

bool UExposureProbeSubsystem::EvaluatePlaced(const FString& InObserver, bool bAllowFloor, FString& OutMessage)
{
	using namespace ExposureSmoke;
	const double T0 = FPlatformTime::Seconds();
	const TArray<AExposureSmokeActor*> Placed = GetPlacedSmokes(true);
	if (Placed.Num() == 0)
	{
		OutMessage = EPS("关卡中没有放置的烟雾：使用「放置烟雾」，或从放置面板拖入「烟雾」", "No smoke is placed: use Place Smoke, or drag a Smoke in from the Place panel");
		return false;
	}
	FExposureSmokeResult R;
	int32 O = INDEX_NONE;
	if (!PrepareSmoke(InObserver, bAllowFloor, R, O, OutMessage))
	{
		return false;
	}
	R.bPlaced = true;
	const int32 NT = R.Ends.Num();
	TArray<bool> Cut;
	Cut.Init(false, NT);
	int32 NCut = 0;
	for (const AExposureSmokeActor* A : Placed)
	{
		FExposureSmokeResult::FSmoke& Sm = R.Smokes.AddDefaulted_GetRef();
		const FExposureSmokeShape Shape = A->GetShape();
		Sm.Name = A->GetSmokeName();
		Sm.Landing = A->GetActorLocation();
		Sm.Centre = A->GetCentre();
		Sm.RadiusCm = A->GetRadiusCm();
		Sm.Duration = Shape.Duration;
		for (int32 t = 0; t < NT; ++t)
		{
			if (!Cuts(R.Eye, R.Ends[t], Sm.Centre, Sm.RadiusCm)) { continue; }
			++Sm.Hits;
			if (!Cut[t]) { Cut[t] = true; ++NCut; ++Sm.NewlyCut; }
		}
	}
	R.Cut = Cut;
	R.Coverage = float(NCut) / NT;
	R.bReached = NCut >= R.Tau * NT - 1e-6;
	R.RadiusCm = R.Smokes[0].RadiusCm;
	SolveRecovery(R, O);
	R.Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
	FinishSmoke(MoveTemp(R), OutMessage);
	return true;
}

FVector UExposureProbeSubsystem::SmokeSnapPoint(const FVector& Point, bool& bOutCell) const
{
	bOutCell = false;
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	if (S->bSnapSmokesToCells && HasHeat())
	{
		// The nearest cell around the point, at any height within a storey, so a smoke dropped on a box top finds that cell.
		int32 Best = INDEX_NONE;
		double BestD = TNumericLimits<double>::Max();
		const FIntVector C(FMath::FloorToInt(Point.X / HeatSpacingCm), FMath::FloorToInt(Point.Y / HeatSpacingCm), 0);
		for (int32 dx = -2; dx <= 2; ++dx)
		for (int32 dy = -2; dy <= 2; ++dy)
		{
			if (const TArray<int32>* Bucket = HeatGrid.Find(C + FIntVector(dx, dy, 0)))
			{
				for (const int32 i : *Bucket)
				{
					const FVector& F = Heat[i].Feet;
					const double Dz = FMath::Abs(F.Z - Point.Z);
					if (Dz > 3.0 * HeatSpacingCm) { continue; }
					const double D = FVector::DistSquared2D(F, Point) + 4.0 * Dz * Dz;
					if (D < BestD) { BestD = D; Best = i; }
				}
			}
		}
		if (Best != INDEX_NONE)
		{
			bOutCell = true;
			return Heat[Best].Feet;
		}
	}
	// No cell nearby: rest on the floor below.
	if (UWorld* World = GetEditorWorld())
	{
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ExposureSmokeSnap), S->bTraceComplex);
		if (World->LineTraceSingleByChannel(Hit, Point + FVector(0, 0, 50.f), Point - FVector(0, 0, 5000.f), S->TraceChannel, Params))
		{
			return Hit.ImpactPoint;
		}
	}
	return Point;
}

FString UExposureProbeSubsystem::NextSmokeLabel() const
{
	TSet<FString> Used;
	for (const AExposureSmokeActor* A : GetPlacedSmokes(false)) { Used.Add(A->GetSmokeName()); }
	for (int32 k = 1; ; ++k)
	{
		const FString L = FString::Printf(TEXT("Smoke%d"), k);
		if (!Used.Contains(L)) { return L; }
	}
}

AExposureSmokeActor* UExposureProbeSubsystem::PlaceSmokeAt(const FVector& Point, FString& OutMessage)
{
	UWorld* World = GetEditorWorld();
	if (!World || !GEditor || GEditor->PlayWorld)
	{
		OutMessage = EPS("先停止运行", "Stop Play In Editor first");
		return nullptr;
	}
	bool bCell = false;
	const FVector At = SmokeSnapPoint(Point, bCell);
	const FScopedTransaction Transaction(EPT("放置烟雾", "Place Smoke"));
	bSnapping = true;
	AExposureSmokeActor* A = Cast<AExposureSmokeActor>(GEditor->AddActor(World->GetCurrentLevel(), AExposureSmokeActor::StaticClass(), FTransform(At), true));
	bSnapping = false;
	if (!A)
	{
		OutMessage = EPS("烟雾放置失败", "Could not place the smoke");
		return nullptr;
	}
	A->SetActorLabel(NextSmokeLabel());
	A->RefreshShape();
	HandleSmokeEdit(A);
	GEditor->SelectNone(false, true);
	GEditor->SelectActor(A, true, true);
	OutMessage = FString::Format(*EPS("已放置 {0}{1}", "Placed {0}{1}"), { A->GetSmokeName(),
		bCell ? EPS("，吸附到地面格子", ", snapped to a floor cell") : (HasHeat() ? EPS("；附近没有地面格子，落在下方地面", "; no floor cell nearby: rests on the floor below")
			: EPS("；分析地面后可吸附到格子", "; after a floor analysis smoke snaps to its cells")) });
	return A;
}

AExposureSmokeActor* UExposureProbeSubsystem::PlaceSmokeAtView(FString& OutMessage)
{
	UWorld* World = GetEditorWorld();
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if (!World || !Client)
	{
		OutMessage = EPS("没有活动的关卡视口", "No active level viewport");
		return nullptr;
	}
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const FVector From = Client->GetViewLocation();
	const FVector Dir = Client->GetViewRotation().Vector();
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExposureSmokePlace), S->bTraceComplex);
	const FVector At = World->LineTraceSingleByChannel(Hit, From, From + Dir * 100000.f, S->TraceChannel, Params) ? Hit.ImpactPoint : From + Dir * 500.f;
	return PlaceSmokeAt(At, OutMessage);
}

int32 UExposureProbeSubsystem::PlacePlanSmokes(FString& OutMessage)
{
	if (!bHasSmoke || Smoke.bPlaced || Smoke.Smokes.Num() == 0)
	{
		OutMessage = EPS("请先为一个探针计算烟雾方案", "Compute a smoke plan for a probe first");
		return 0;
	}
	UWorld* World = GetEditorWorld();
	if (!World || !GEditor || GEditor->PlayWorld)
	{
		OutMessage = EPS("先停止运行", "Stop Play In Editor first");
		return 0;
	}
	const FString Observer = Smoke.Observer;
	const TArray<FExposureSmokeResult::FSmoke> Plan = Smoke.Smokes;
	const FScopedTransaction Transaction(EPT("将方案转为放置的烟雾", "Convert Plan to Placed Smoke"));
	bSnapping = true;
	int32 N = 0;
	for (const FExposureSmokeResult::FSmoke& Sm : Plan)
	{
		if (AExposureSmokeActor* A = Cast<AExposureSmokeActor>(GEditor->AddActor(World->GetCurrentLevel(), AExposureSmokeActor::StaticClass(), FTransform(Sm.Landing), true, RF_Transactional, false)))
		{
			A->SetActorLabel(NextSmokeLabel());
			A->RefreshShape();
			++N;
		}
	}
	bSnapping = false;
	FString Eval;
	EvaluatePlaced(Observer, false, Eval);
	OutMessage = FString::Format(*EPS("已放置 {0} 个烟雾，可拖动以比较效果", "Placed {0} smoke(s); drag them to compare"), { N });
	return N;
}

int32 UExposureProbeSubsystem::ClearPlacedSmokes()
{
	UWorld* World = GetEditorWorld();
	const TArray<AExposureSmokeActor*> Placed = GetPlacedSmokes(false);
	if (!World || !GEditor || Placed.Num() == 0)
	{
		return 0;
	}
	const FScopedTransaction Transaction(EPT("删除烟雾", "Delete Smoke"));
	for (AExposureSmokeActor* A : Placed)
	{
		A->Modify();
		World->EditorDestroyActor(A, true);
	}
	if (bHasSmoke && Smoke.bPlaced)
	{
		ClearSmoke();
	}
	bSmokeDirty = true;
	return Placed.Num();
}

void UExposureProbeSubsystem::HandleSmokeEdit(const AExposureSmokeActor* Actor)
{
	if (!Actor || bSnapping)
	{
		return;
	}
	PendingSnap.AddUnique(const_cast<AExposureSmokeActor*>(Actor));
	bSmokeDirty = true;
	LastSmokeEdit = FPlatformTime::Seconds();
	if (bHasSmoke && Smoke.bPlaced)
	{
		bSmokeEvalPending = true;
	}
}

void UExposureProbeSubsystem::SnapPendingSmokes()
{
	if (PendingSnap.Num() == 0)
	{
		return;
	}
	bSnapping = true;
	for (const TWeakObjectPtr<AExposureSmokeActor>& W : PendingSnap)
	{
		AExposureSmokeActor* A = W.Get();
		if (!IsValid(A)) { continue; }
		bool bCell = false;
		const FVector At = SmokeSnapPoint(A->GetActorLocation(), bCell);
		if (bCell && !At.Equals(A->GetActorLocation(), 0.5))
		{
			bSmokeDirty = true;
			A->Modify();
			A->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
		}
		A->RefreshShape();
	}
	bSnapping = false;
	PendingSnap.Reset();
}

void UExposureProbeSubsystem::RefreshSmokes()
{
	for (AExposureSmokeActor* A : GetPlacedSmokes(false)) { A->RefreshShape(); }
	bSmokeDirty = true;
	if (bHasSmoke && Smoke.bPlaced)
	{
		bSmokeEvalPending = true;
		LastSmokeEdit = 0.0;
	}
	else
	{
		RedrawSmoke();
	}
}

// ---------------------------------------------------------------------------------------------
// Summary

FString UExposureProbeSubsystem::SmokeSummary() const
{
	using namespace ExposureSmoke;
	if (!bHasSmoke)
	{
		return FString();
	}
	const FExposureSmokeResult& R = Smoke;
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	FString Out;
	if (R.bPlaced)
	{
		Out = FString::Format(*EPS("{0} 看得见 {1}/{2} 个对方探针，共 {3} 个身体点；已放置烟雾 {4} 个",
			"{0} sees {1}/{2} opposing probes, {3} body points; {4} placed smoke(s)"),
			{ R.Observer, R.OpponentsSeen, R.Opponents, R.Ends.Num(), R.Smokes.Num() });
		Out += TEXT("\n") + FString::Format(*EPS("共遮挡 {0} 的可见视线（{1}阈值 {2}）", "Together they cut {0} of the visible sightlines ({1} the {2} threshold)"),
			{ Pct(R.Coverage), R.bReached ? EPS("达到", "meets") : EPS("未达到", "misses"), Pct(R.Tau) });
		for (const FExposureSmokeResult::FSmoke& Sm : R.Smokes)
		{
			Out += TEXT("\n") + FString::Format(*EPS("  {0}：半径 {1} 单位，遮挡 {2} 条，其中 {3} 条未被前面的烟雾遮挡",
				"  {0}: radius {1} units, cuts {2}, {3} of them not cut by the smokes before it"),
				{ Sm.Name, FMath::RoundToInt(Sm.RadiusCm / S->UnitToCm), Sm.Hits, Sm.NewlyCut });
		}
	}
	else
	{
		Out = FString::Format(*EPS("{0} 看得见 {1}/{2} 个对方探针，共 {3} 个身体点；候选落点 {4} 个",
			"{0} sees {1}/{2} opposing probes, {3} body points; {4} landing points"),
			{ R.Observer, R.OpponentsSeen, R.Opponents, R.Ends.Num(), R.Candidates });
		if (R.Smokes.Num() == 0)
		{
			return Out + TEXT("\n") + EPS("任何落点的烟雾都无法遮挡这些视线：CC 记为无穷（右删失）", "No landing point's smoke cuts these sightlines: CC is infinite (right-censored)");
		}
		Out += TEXT("\n") + (R.bReached
			? FString::Format(*EPS("CC = {0} 个烟雾：遮挡 {1} 的可见视线（阈值 {2}）", "CC = {0} smoke(s): cuts {1} of the visible sightlines (threshold {2})"),
				{ R.Smokes.Num(), Pct(R.Coverage), Pct(R.Tau) })
			: FString::Format(*EPS("{0} 个烟雾仅遮挡 {1}，未达到 {2}：CC 记为大于 {0}（右删失）", "{0} smoke(s) cut only {1}, short of {2}: CC is above {0} (right-censored)"),
				{ R.Smokes.Num(), Pct(R.Coverage), Pct(R.Tau) }));
	}
	if (R.bStillSees)
	{
		Out += TEXT("\n") + FString::Format(*EPS("烟雾后仍可见 {0} 的原视线，不低于 {1}：RT = 0", "{0} of the sightlines stay open, at least {1}: RT = 0"),
			{ Pct(1.f - R.Coverage), Pct(S->SmokeRecoverShare) });
	}
	else if (R.bRecoverySkipped)
	{
		Out += TEXT("\n") + EPS("无地面分析，未搜索绕行位置：RT 取烟雾持续时间", "No floor analysis to search detours on: RT is the time until the smoke clears");
	}
	else if (R.bFoundAlt)
	{
		Out += TEXT("\n") + FString::Format(*EPS("RT = {0} 秒：沿{1}绕行 {2} 单位 ÷ {3} 单位/秒 + {4} 秒重新瞄准，到达后可见 {5}；烟雾持续 {6} 秒",
			"RT = {0} s: {1} of {2} units at {3} units/s + {4} s to aim again; the spot still sees {5}; waiting the smoke out takes {6} s"),
			{ Sec(R.RT), R.bNavPath ? EPS("导航路线", "a nav path") : EPS("直线", "a straight line"), FMath::RoundToInt(R.DetourUnits),
			  FMath::RoundToInt(S->SmokeMoveSpeed), Sec(S->SmokeReacquireSeconds), Pct(R.AltShare), Sec(R.WaitSeconds) });
	}
	else if (R.Smokes.Num() > 0)
	{
		Out += TEXT("\n") + FString::Format(*EPS("附近没有可重新看见原目标的位置：RT 取烟雾持续时间 {0} 秒（结构性失明）",
			"No spot nearby still sees the targets: RT is the smoke duration, {0} s (structural blindness)"), { Sec(R.WaitSeconds) });
	}
	if (!R.bNavPath && R.bFoundAlt)
	{
		Out += TEXT("\n") + EPS("无导航网格或无可用导航路线，绕行按直线距离计算", "No navigation mesh, or no nav path leaves this spot: the detour is a straight line");
	}
	Out += FString::Printf(TEXT("\n%s %.0f ms"), *EPS("用时", "Took"), R.Ms);
	if (bSmokeStale)
	{
		Out += TEXT("\n") + EPS("关卡或探针已改动，需重新计算", "The level or the probes changed; compute again");
	}
	return Out;
}

void UExposureProbeSubsystem::ClearSmoke()
{
	// Moving a smoke after this does not bring the evaluation back until a probe is selected again.
	LastSmokeObserver.Reset();
	bHasSmoke = false;
	bSmokeStale = false;
	bSmokeEvalPending = false;
	Smoke = FExposureSmokeResult();
	bSmokeDirty = true;
	RedrawSmoke();
	OnReportUpdated.Broadcast();
}

// ---------------------------------------------------------------------------------------------
// Drawing

void UExposureProbeSubsystem::DestroySmokeComponents()
{
	if (SmokeComp)
	{
		if (SmokeComp->IsRegistered()) { SmokeComp->UnregisterComponent(); }
		SmokeComp = nullptr;
	}
	for (ULocalFogVolumeComponent* Fog : SmokeFog)
	{
		if (Fog && Fog->IsRegistered()) { Fog->UnregisterComponent(); }
	}
	SmokeFog.Reset();
}

void UExposureProbeSubsystem::RedrawSmoke()
{
	using namespace ExposureSmoke;
	bSmokeDirty = false;
	UWorld* World = GetEditorWorld();
	ULineBatchComponent* Lines = World ? World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent) : nullptr;
	if (Lines) { Lines->ClearBatch(BatchId); }
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();

	// Clouds: the plan's smokes, or every placed smoke (the disabled ones are not drawn).
	struct FDraw { FVector Landing; FVector Centre; float Radius; FString Key; };
	TArray<FDraw> Draw;
	if (bHasSmoke && !Smoke.bPlaced)
	{
		for (const FExposureSmokeResult::FSmoke& Sm : Smoke.Smokes) { Draw.Add({ Sm.Landing, Sm.Centre, Sm.RadiusCm, Key(Sm.Centre, Sm.RadiusCm) }); }
	}
	const TArray<AExposureSmokeActor*> Placed = World ? GetPlacedSmokes(true) : TArray<AExposureSmokeActor*>();
	PlacedSmokeCount = World ? GetPlacedSmokes(false).Num() : 0;
	// A placed smoke is keyed by its actor, so dragging it does not make it swell in again.
	for (const AExposureSmokeActor* A : Placed) { Draw.Add({ A->GetActorLocation(), A->GetCentre(), A->GetRadiusCm(), A->GetPathName() }); }

	if (!World || Draw.Num() == 0)
	{
		DestroySmokeComponents();
		SmokeBorn.Reset();
		if (bHasSmoke && Lines)
		{
			// No smoke helps: show the sightlines that stay open.
			for (const FVector& E : Smoke.Ends) { Lines->DrawLine(Smoke.Eye, E, UExposureProbeSettings::AlertColor(), SDPG_World, 1.5f, -1.f, BatchId); }
		}
		if (GEditor) { GEditor->RedrawLevelEditingViewports(false); }
		return;
	}
	if ((SmokeComp && SmokeComp->GetWorld() != World) || (SmokeFog.Num() > 0 && SmokeFog[0] && SmokeFog[0]->GetWorld() != World)) { DestroySmokeComponents(); }
	if (!SmokeComp)
	{
		SmokeComp = NewObject<UExposureSmokeComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		SmokeComp->RegisterComponentWithWorld(World);
	}
	// A smoke keeps the time it appeared while it stays put, so redraws do not make it swell in again.
	const double Now = FPlatformTime::Seconds();
	TMap<FString, double> Born;
	TArray<FExposureSmokeCloud> Clouds;
	for (const FDraw& D : Draw)
	{
		const double* Old = SmokeBorn.Find(D.Key);
		const double B = Old ? *Old : Now;
		Born.Add(D.Key, B);
		Clouds.Add({ D.Centre, D.Radius, B, uint32(GetTypeHash(D.Key)) });
	}
	SmokeBorn = MoveTemp(Born);
	SmokeComp->SetClouds(MoveTemp(Clouds), S->SmokeStyle, S->SmokeDensity, S->SmokeColor);

	for (ULocalFogVolumeComponent* Fog : SmokeFog) { if (Fog && Fog->IsRegistered()) { Fog->UnregisterComponent(); } }
	SmokeFog.Reset();
	if (S->bSmokeFog)
	{
		for (const FDraw& D : Draw)
		{
			ULocalFogVolumeComponent* Fog = NewObject<ULocalFogVolumeComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			Fog->RadialFogExtinction = 2.5f;
			Fog->HeightFogExtinction = 0.f;
			Fog->FogAlbedo = S->SmokeColor;
			Fog->FogEmissive = FLinearColor(0.04f, 0.04f, 0.045f);
			Fog->FogPhaseG = 0.2f;
			Fog->SetRelativeTransform(FTransform(FRotator::ZeroRotator, D.Centre, FVector(D.Radius / ULocalFogVolumeComponent::GetBaseVolumeSize())));
			Fog->RegisterComponentWithWorld(World);
			SmokeFog.Add(Fog);
		}
	}

	if (Lines)
	{
		const FLinearColor Ink(0.95f, 0.95f, 0.95f);
		for (const FDraw& D : Draw)
		{
			Lines->DrawCircle(D.Landing + FVector(0, 0, 3), FVector::ForwardVector, FVector::RightVector, Ink, 0.25f * D.Radius, 24, SDPG_World, BatchId);
		}
		if (bHasSmoke)
		{
			const FLinearColor Blocked(0.55f, 0.55f, 0.58f);
			const FLinearColor Open = UExposureProbeSettings::AlertColor();
			for (int32 t = 0; t < Smoke.Ends.Num(); ++t)
			{
				const bool bCut = Smoke.Cut.IsValidIndex(t) && Smoke.Cut[t];
				Lines->DrawLine(Smoke.Eye, Smoke.Ends[t], bCut ? Blocked : Open, SDPG_World, bCut ? 0.8f : 1.8f, -1.f, BatchId);
			}
			if (Smoke.bFoundAlt)
			{
				const FLinearColor Side = Smoke.Team == EExposureTeam::TSide ? S->TColor : S->CTColor;
				for (int32 k = 0; k + 1 < Smoke.Path.Num(); ++k)
				{
					Lines->DrawLine(Smoke.Path[k] + FVector(0, 0, 12), Smoke.Path[k + 1] + FVector(0, 0, 12), Side, SDPG_World, 3.f, -1.f, BatchId);
				}
				Lines->DrawCircle(Smoke.AltFeet + FVector(0, 0, 3), FVector::ForwardVector, FVector::RightVector, Side, 40.f, 24, SDPG_World, BatchId);
				Lines->DrawLine(Smoke.AltFeet, Smoke.AltEye, Side, SDPG_World, 2.f, -1.f, BatchId);
			}
		}
	}
	if (GEditor) { GEditor->RedrawLevelEditingViewports(false); }
}

void UExposureProbeSubsystem::DrawSmokeLabels(UCanvas* Canvas)
{
	using namespace ExposureSmoke;
	if (!bHasSmoke || !Canvas)
	{
		return;
	}
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle("Bold", 10);
	const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 8);
	const FLinearColor Ink(0.94f, 0.94f, 0.94f);
	const FLinearColor Muted(0.66f, 0.66f, 0.66f);
	const FLinearColor PlateCol(0.03f, 0.03f, 0.035f, 0.78f);
	const float Dpi = Canvas->GetDPIScale();
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	auto Card = [&](const FVector& World, const FLinearColor& Chip, const FString& Line1, const FString& Line2)
	{
		const FVector P = Canvas->Project(World, false);
		if (P.Z <= 0.f || P.X < 0 || P.Y < 0 || P.X > Canvas->ClipX || P.Y > Canvas->ClipY) { return; }
		const FVector2D S1(Measure->Measure(Line1, Title));
		const FVector2D S2 = Line2.IsEmpty() ? FVector2D::ZeroVector : FVector2D(Measure->Measure(Line2, Body));
		const float Pad = 6.f, ChipW = 3.f;
		const float W = FMath::Max(S1.X, S2.X) + 2.f * Pad + ChipW;
		const float H = S1.Y + S2.Y + 6.f;
		const FVector2D Min(P.X / Dpi - 0.5f * W, P.Y / Dpi - H - 6.f);
		FCanvasTileItem Plate(Min * Dpi, FVector2D(W, H) * Dpi, PlateCol);
		Plate.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Plate);
		FCanvasTileItem ChipItem(Min * Dpi, FVector2D(ChipW, H) * Dpi, Chip);
		ChipItem.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(ChipItem);
		auto Text = [&](const FVector2D& At, const FString& Str, const FSlateFontInfo& F, const FLinearColor& Col)
		{
			FCanvasTextItem Item(At, FText::FromString(Str), F, Col);
			Item.Font = GEngine->GetSmallFont();
			Item.EnableShadow(FLinearColor(0, 0, 0, 0.6f));
			Canvas->DrawItem(Item);
		};
		Text(Min + FVector2D(ChipW + Pad, 3.f), Line1, Title, Ink);
		if (!Line2.IsEmpty()) { Text(Min + FVector2D(ChipW + Pad, 3.f + S1.Y), Line2, Body, Muted); }
	};

	const FExposureSmokeResult& R = Smoke;
	const FLinearColor Side = R.Team == EExposureTeam::TSide ? S->TColor : S->CTColor;
	const FLinearColor Grey(0.7f, 0.7f, 0.72f);
	FString Head;
	if (R.bPlaced) { Head = FString::Format(*EPS("{0} 个烟雾遮挡 {1}", "{0} smoke(s) cut {1}"), { R.Smokes.Num(), Pct(R.Coverage) }); }
	else if (R.Smokes.Num() == 0) { Head = TEXT("CC = ∞"); }
	else { Head = R.bReached ? FString::Printf(TEXT("CC = %d"), R.Smokes.Num()) : FString::Printf(TEXT("CC > %d"), R.Smokes.Num()); }
	FString Sub = R.bPlaced ? FString() : FString::Format(*EPS("遮挡 {0} 的视线", "cuts {0} of the sightlines"), { Pct(R.Coverage) });
	if (R.Smokes.Num() > 0)
	{
		if (!Sub.IsEmpty()) { Sub += TEXT(" · "); }
		Sub += R.bStillSees ? EPS("仍可见，RT 0", "still sees, RT 0") : FString::Format(*EPS("RT {0} 秒", "RT {0} s"), { Sec(R.RT) });
	}
	if (bSmokeStale) { Sub += TEXT(" · ") + EPS("已过期", "out of date"); }
	Card(R.Eye + FVector(0, 0, 40), Side, R.Observer + TEXT("  ") + Head, Sub);
	for (int32 k = 0; k < R.Smokes.Num(); ++k)
	{
		const FExposureSmokeResult::FSmoke& Sm = R.Smokes[k];
		const FString Name = Sm.Name.IsEmpty() ? FString::Format(*EPS("烟雾 {0}", "Smoke {0}"), { k + 1 }) : Sm.Name;
		Card(Sm.Centre + FVector(0, 0, Sm.RadiusCm), Grey, Name,
			R.bPlaced ? FString::Format(*EPS("遮挡 {0}/{1}", "cuts {0}/{1}"), { Sm.Hits, R.Ends.Num() })
				: FString::Format(*EPS("新增遮挡 {0}/{1}", "cuts {0}/{1} more"), { Sm.NewlyCut, R.Ends.Num() }));
	}
	if (R.bFoundAlt)
	{
		Card(R.AltEye + FVector(0, 0, 30), Side, FString::Format(*EPS("绕行位置 · {0} 秒", "Detour · {0} s"), { Sec(R.DetourSeconds) }),
			FString::Format(*EPS("{0} 单位 · 可见 {1}", "{0} units · sees {1}"), { FMath::RoundToInt(R.DetourUnits), Pct(R.AltShare) }));
	}
}
