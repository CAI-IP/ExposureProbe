#include "ExposureProbeSubsystem.h"

#include "ExposureHeatComponent.h"
#include "ExposureSmokeComponent.h"
#include "ExposureProbeActor.h"
#include "ExposureProbeSettings.h"
#include "ExposureProbeText.h"
#include "ActorEditorUtils.h"
#include "Async/ParallelFor.h"
#include "Components/LineBatchComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "SLevelViewport.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/PlatformTime.h"
#include "LevelEditorViewport.h"
#include "Misc/ScopedSlowTask.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "ExposureProbe"

namespace ExposureHeat
{
	static constexpr uint32 HighlightBatchId = 0x45585048; // 'EXPH'
	static constexpr uint32 MarkBatchId = 0x4558504D;      // 'EXPM'
	static constexpr uint32 HoverBatchId = 0x45585056;     // 'EXPV'
	/** Samples above this are thinned by widening the grid. */
	static constexpr int32 MaxSamples = 4000;
	/** Floors stacked in one grid column (ground, upper floors, roofs). */
	static constexpr int32 MaxLayers = 8;
	static constexpr int32 Levels = 7;
	/** Hover lines drawn at most, nearest first. */
	static constexpr int32 MaxHoverLines = 48;
	/** Share of the full-colour |A| from which a spot counts as clearly seen more or seeing more in the legend. */
	static constexpr float ClearShare = 0.3f;
	/** Below this share of the full-colour value the floor stays untinted. */
	static constexpr float DeadZone = 0.15f;
	/** Opacity of the heat colour at strength T (0..1); about 0.3 just past the dead zone, 0.8 at full. */
	static float HeatAlpha(float T) { return T <= 0.f ? 0.f : 0.24f + 0.56f * FMath::Clamp(T, 0.f, 1.f); }

	static FIntVector Cell(const FVector& P, float Spacing)
	{
		return FIntVector(FMath::FloorToInt(P.X / Spacing), FMath::FloorToInt(P.Y / Spacing), 0);
	}

	static const TArray<int32> Empty;

	static void DrawSquare(ULineBatchComponent* Lines, const FVector& C, float Half, const FLinearColor& Col, float Thick, uint32 Id)
	{
		const FVector P[4] = { C + FVector(-Half, -Half, 0), C + FVector(Half, -Half, 0), C + FVector(Half, Half, 0), C + FVector(-Half, Half, 0) };
		for (int32 k = 0; k < 4; ++k) { Lines->DrawLine(P[k], P[(k + 1) % 4], Col, SDPG_World, Thick, -1.f, Id); }
	}
}

FString UExposureProbeSubsystem::CurrentHeatKey() const
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	FExposureBodyModel M = FExposureBodyModel::FromSettings();
	if (S->HeatmapBody == EExposureHeatBody::Points16) { M = M.Points16(); }
	return M.Key() + FString::Printf(TEXT("|heat %g %g %d %g %d %g"), S->HeatmapSpacing * S->UnitToCm, S->HeatmapRadius * S->UnitToCm, S->bHeatmapOnNavMesh ? 1 : 0, S->OneWayThreshold,
		S->bHeatmapReachableOnly ? 1 : 0, S->HeatmapMaxClimb * S->UnitToCm);
}

bool UExposureProbeSubsystem::IsHeatStale() const
{
	return HasHeat() && (bHeatStale || HeatKey != CurrentHeatKey());
}

bool UExposureProbeSubsystem::IsLayerOn(EExposureLayer Layer) const
{
	switch (Layer)
	{
	case EExposureLayer::Probes: return bShowProbes;
	case EExposureLayer::Heatmap: return bShowHeat;
	default: return bShowOneWay;
	}
}

void UExposureProbeSubsystem::SetLayer(EExposureLayer Layer, bool bOn)
{
	switch (Layer)
	{
	case EExposureLayer::Probes: bShowProbes = bOn; break;
	case EExposureLayer::Heatmap: bShowHeat = bOn; break;
	default: bShowOneWay = bOn; break;
	}
	if (!bShowHeat && !bShowOneWay) { HoverHeat = INDEX_NONE; }
	Redraw();
	RedrawHeat();
	RedrawHover();
	OnReportUpdated.Broadcast();
}

void UExposureProbeSubsystem::SetHeatShow(EExposureHeatShow Show)
{
	HeatShow = Show == EExposureHeatShow::Delta && !CanShowHeatDelta() ? EExposureHeatShow::A : Show;
	RedrawHeat();
	OnReportUpdated.Broadcast();
}

bool UExposureProbeSubsystem::CanShowHeatDelta() const
{
	return bHasBaseline && HeatDeltaValid.Contains(true);
}

bool UExposureProbeSubsystem::HeatValue(int32 Index, float& OutValue) const
{
	if (!Heat.IsValidIndex(Index))
	{
		return false;
	}
	if (HeatShow == EExposureHeatShow::Delta)
	{
		if (!HeatDeltaValid.IsValidIndex(Index) || !HeatDeltaValid[Index]) { return false; }
		OutValue = HeatDelta[Index];
		return true;
	}
	OutValue = Heat[Index].A;
	return true;
}

const TArray<int32>& UExposureProbeSubsystem::GetOneWayTargets(int32 Index) const
{
	return OneWayTo.IsValidIndex(Index) ? OneWayTo[Index] : ExposureHeat::Empty;
}

int32 UExposureProbeSubsystem::GetOneWaySpotCount() const
{
	int32 N = 0;
	for (const FExposureHeatPoint& P : Heat) { N += P.OneWaySees > 0 ? 1 : 0; }
	return N;
}

bool UExposureProbeSubsystem::IsNewOneWaySpot(int32 Index) const
{
	return bHasBaseline && Heat.IsValidIndex(Index) && Heat[Index].OneWaySees > 0 && BaselineOneWay.IsValidIndex(Index) && BaselineOneWay[Index] == 0;
}

const TArray<int32>& UExposureProbeSubsystem::GetCellThreatens(int32 Index) const
{
	return CellThreatens.IsValidIndex(Index) ? CellThreatens[Index] : ExposureHeat::Empty;
}

const TArray<int32>& UExposureProbeSubsystem::GetCellExposedTo(int32 Index) const
{
	return CellExposedTo.IsValidIndex(Index) ? CellExposedTo[Index] : ExposureHeat::Empty;
}

int32 UExposureProbeSubsystem::GetThreatCellCount() const
{
	int32 N = 0;
	for (const TArray<int32>& L : CellThreatens) { N += L.Num() > 0 ? 1 : 0; }
	return N;
}

int32 UExposureProbeSubsystem::GetAdvantageCellCount() const
{
	int32 N = 0;
	for (const TArray<int32>& L : CellExposedTo) { N += L.Num() > 0 ? 1 : 0; }
	return N;
}

int32 UExposureProbeSubsystem::NearestHeat(const FVector& Point) const
{
	if (HeatSpacingCm <= 0.f)
	{
		return INDEX_NONE;
	}
	const FIntVector C = ExposureHeat::Cell(Point, HeatSpacingCm);
	int32 Best = INDEX_NONE;
	double BestD = FMath::Square(1.5 * HeatSpacingCm);
	for (int32 dx = -1; dx <= 1; ++dx)
	for (int32 dy = -1; dy <= 1; ++dy)
	{
		if (const TArray<int32>* Bucket = HeatGrid.Find(C + FIntVector(dx, dy, 0)))
		{
			for (const int32 i : *Bucket)
			{
				const FVector& F = Heat[i].Feet;
				if (FMath::Abs(F.Z - Point.Z) > 0.5 * HeatSpacingCm + 30.0) { continue; }
				const double D = FVector::DistSquared2D(F, Point);
				if (D < BestD) { BestD = D; Best = i; }
			}
		}
	}
	return Best;
}

bool UExposureProbeSubsystem::BuildHeatSamples(const FExposureBodyModel& Model, float& InOutSpacingCm, TArray<FExposureProbeSolver::FPoint>& Out, FVector2D& OutOrigin, FString& OutWhy) const
{
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return false;
	}
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();

	// Walkable area: the navigation mesh when there is one, otherwise every surface with collision.
	const ARecastNavMesh* Nav = nullptr;
	if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
	{
		Nav = Cast<ARecastNavMesh>(NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate));
	}
	const bool bUseNav = S->bHeatmapOnNavMesh && Nav && Nav->GetNavMeshBounds().IsValid;
	FBox Bounds(ForceInit);
	if (bUseNav)
	{
		Bounds = Nav->GetNavMeshBounds();
	}
	else
	{
		const ECollisionChannel Channel = S->TraceChannel;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			FExposureGeomState G;
			if (IsValid(*It) && !It->IsA<AExposureProbeActor>() && !FActorEditorUtils::IsABuilderBrush(*It) && ComputeGeom(*It, Channel, G))
			{
				Bounds += G.Box;
			}
		}
	}
	if (!Bounds.IsValid)
	{
		OutWhy = EPS("关卡中没有可采样的地面", "Nothing to sample in the level");
		return false;
	}
	OutOrigin = FVector2D(Bounds.Min.X, Bounds.Min.Y);

	// Keep the floor a player can walk (or jump) to from a probe or a player start. A navigation mesh also covers
	// roofs and tops the player cannot get onto, so this applies with one too.
	const bool bReach = S->bHeatmapReachableOnly;
	TArray<FVector> Seeds;
	if (bReach)
	{
		for (TActorIterator<AExposureProbeActor> It(World); It; ++It) { if (IsValid(*It)) { Seeds.Add(It->GetActorLocation()); } }
		for (TActorIterator<APlayerStart> It(World); It; ++It) { if (IsValid(*It)) { Seeds.Add(It->GetActorLocation()); } }
	}
	if (bReach && Seeds.Num() == 0)
	{
		OutWhy = EPS("关卡中没有探针或玩家出生点可作起点：地面分析覆盖所有可站立的表面，包括无法到达的台面", "No probe or player start to start from: every surface with room to stand is analysed, including tops that cannot be reached");
	}
	else if (!bUseNav && !bReach)
	{
		OutWhy = EPS("关卡没有导航网格：地面分析覆盖所有可站立的表面，包括无法到达的台面", "No navigation mesh: every surface with room to stand is analysed, including tops that cannot be reached");
	}

	const FVector Ext = Bounds.GetExtent();
	const float SpacingCm = InOutSpacingCm;
	float Spacing = SpacingCm;
	while ((Ext.X * 2.f / Spacing) * (Ext.Y * 2.f / Spacing) > 400000.f) { Spacing *= 1.5f; }

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExposureHeatFloor), Model.bTraceComplex);
	// Simple collision for the room test: a shape inside a solid block overlaps its simple collision, while a trace
	// that starts inside a mesh passes through its complex collision.
	const FCollisionQueryParams Simple(SCENE_QUERY_STAT(ExposureHeatRoom), false);
	const float CrouchCm = Model.CrouchHeight * Model.UnitToCm;
	const float ClimbCm = S->HeatmapMaxClimb * S->UnitToCm;
	const float BodyR = FMath::Max(4.f, 0.8f * Model.Radius * Model.UnitToCm);
	// The capsule starts above a step, so stairs and ramps next to the feet do not count as blocking.
	const float Lift = FMath::Min(ClimbCm, 0.3f * CrouchCm);
	const float HalfH = FMath::Max(BodyR, 0.5f * (CrouchCm - Lift));
	const FCollisionShape Capsule = FCollisionShape::MakeCapsule(BodyR, HalfH);
	const float Top = Bounds.Max.Z + Model.StandHeight * Model.UnitToCm;
	const float Bottom = Bounds.Min.Z - 50.f;

	struct FSample
	{
		FExposureProbeSolver::FPoint P;
		FIntPoint Cell;
	};
	int32 Unreachable = 0;
	for (int32 Pass = 0; Pass < 4; ++Pass)
	{
		Out.Reset();
		const int32 NX = FMath::Max(1, FMath::CeilToInt(Ext.X * 2.f / Spacing));
		const int32 NY = FMath::Max(1, FMath::CeilToInt(Ext.Y * 2.f / Spacing));
		TArray<TArray<FSample>> Columns;
		Columns.SetNum(NX * NY);
		ParallelFor(NX * NY, [&](int32 n)
		{
			const float X = Bounds.Min.X + (n % NX + 0.5f) * Spacing;
			const float Y = Bounds.Min.Y + (n / NX + 0.5f) * Spacing;
			float Z = Top;
			float Above = TNumericLimits<float>::Max();
			bool bAboveTop = false;
			for (int32 Layer = 0; Layer < ExposureHeat::MaxLayers && Z > Bottom; ++Layer)
			{
				FHitResult Hit;
				if (!World->LineTraceSingleByChannel(Hit, FVector(X, Y, Z), FVector(X, Y, Bottom), Model.Channel, Params))
				{
					break;
				}
				// Below the hit the trace starts inside the floor slab, which line traces pass through.
				Z = Hit.ImpactPoint.Z - 2.f;
				const float Gap = Above - Hit.ImpactPoint.Z;
				const bool bUnderTop = bAboveTop;
				Above = Hit.ImpactPoint.Z;
				bAboveTop = Hit.ImpactNormal.Z >= 0.7f;
				if (Hit.ImpactNormal.Z < 0.7f || Hit.bStartPenetrating)
				{
					continue;
				}
				const FVector Feet = Hit.ImpactPoint;
				// Below an upward-facing surface with no ceiling face in between: the trace went through a solid
				// (its top face, then its inside), so this is the floor under a block.
				if (bUnderTop && !World->LineTraceTestByChannel(Feet + FVector(0, 0, Lift), FVector(X, Y, Above + Gap - 1.f), Model.Channel, Params))
				{
					continue;
				}
				// Under something lower than a crouching player: no room, e.g. the floor under a block.
				if (Gap < CrouchCm)
				{
					continue;
				}
				bool bCrouch = false;
				float Clearance = 0.f;
				if (!FExposureProbeSolver::ResolvePose(World, Feet, EExposurePose::Auto, Model, bCrouch, Clearance))
				{
					continue;
				}
				// Room for a crouching body; this drops floor inside solid blocks and spots pressed against a wall.
				if (World->OverlapBlockingTestByChannel(Feet + FVector(0, 0, Lift + HalfH), FQuat::Identity, Model.Channel, Capsule, Simple))
				{
					continue;
				}
				if (bUseNav)
				{
					FNavLocation OnNav;
					const FVector Extent(0.5f * Spacing, 0.5f * Spacing, 0.5f * CrouchCm);
					if (!Nav->ProjectPoint(Feet, OnNav, Extent) || FMath::Abs(OnNav.Location.Z - Feet.Z) > 60.f)
					{
						continue;
					}
				}
				FSample& Sm = Columns[n].AddDefaulted_GetRef();
				Sm.P.Feet = Feet;
				Sm.P.bCrouch = bCrouch;
				Sm.Cell = FIntPoint(n % NX, n / NX);
			}
		});
		TArray<FSample> Samples;
		for (TArray<FSample>& C : Columns) { Samples.Append(MoveTemp(C)); }

		Unreachable = 0;
		if (bReach && Seeds.Num() > 0 && Samples.Num() > 0)
		{
			// Directed: a player can step or jump up at most the climb height, and drop down any height.
			const int32 N = Samples.Num();
			TMap<FIntPoint, TArray<int32>> ByCell;
			for (int32 i = 0; i < N; ++i) { ByCell.FindOrAdd(Samples[i].Cell).Add(i); }
			TArray<TArray<int32>> Next;
			Next.SetNum(N);
			ParallelFor(N, [&](int32 i)
			{
				const FVector& A = Samples[i].P.Feet;
				for (int32 dx = -1; dx <= 1; ++dx)
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					const TArray<int32>* Bucket = (dx || dy) ? ByCell.Find(Samples[i].Cell + FIntPoint(dx, dy)) : nullptr;
					for (const int32 j : Bucket ? *Bucket : TArray<int32>())
					{
						const FVector& B = Samples[j].P.Feet;
						if (B.Z - A.Z > ClimbCm) { continue; }
						const float Zt = FMath::Max(A.Z, B.Z) + 0.5f * CrouchCm;
						if (!World->LineTraceTestByChannel(FVector(A.X, A.Y, Zt), FVector(B.X, B.Y, Zt), Model.Channel, Params)) { Next[i].Add(j); }
					}
				}
			});
			TArray<bool> Reached;
			Reached.Init(false, N);
			TArray<int32> Queue;
			const float StandCm = Model.StandHeight * Model.UnitToCm;
			for (const FVector& Seed : Seeds)
			{
				// The sample under the seed: nearest within two grid steps, on the floor the seed stands on.
				int32 Best = INDEX_NONE;
				double BestD = FMath::Square(2.0 * Spacing);
				for (int32 i = 0; i < N; ++i)
				{
					const FVector& F = Samples[i].P.Feet;
					if (F.Z > Seed.Z + 0.5f * StandCm || F.Z < Seed.Z - 1.5f * StandCm) { continue; }
					const double D = FVector::DistSquared2D(F, Seed);
					if (D < BestD) { BestD = D; Best = i; }
				}
				if (Best != INDEX_NONE && !Reached[Best]) { Reached[Best] = true; Queue.Add(Best); }
			}
			for (int32 q = 0; q < Queue.Num(); ++q)
			{
				for (const int32 j : Next[Queue[q]])
				{
					if (!Reached[j]) { Reached[j] = true; Queue.Add(j); }
				}
			}
			if (Queue.Num() > 0)
			{
				TArray<FSample> Kept;
				Kept.Reserve(Queue.Num());
				for (int32 i = 0; i < N; ++i) { if (Reached[i]) { Kept.Add(Samples[i]); } }
				Unreachable = N - Kept.Num();
				Samples = MoveTemp(Kept);
			}
		}
		for (const FSample& Sm : Samples) { Out.Add(Sm.P); }
		if (Out.Num() <= ExposureHeat::MaxSamples)
		{
			break;
		}
		Spacing *= FMath::Sqrt(float(Out.Num()) / ExposureHeat::MaxSamples) * 1.05f;
	}
	if (Unreachable > 0)
	{
		const FString Dropped = FString::Format(*EPS("已排除 {0} 个无法到达的位置", "{0} unreachable spots left out"), { Unreachable });
		OutWhy = OutWhy.IsEmpty() ? Dropped : OutWhy + EPS("；", "; ") + Dropped;
	}
	if (Spacing > SpacingCm * 1.01f)
	{
		const FString Wider = FString::Format(*EPS("采样点过多，间距放宽到 {0}", "Too many samples; spacing widened to {0}"),
			{ FString::Printf(TEXT("%.0f"), Spacing / S->UnitToCm) });
		OutWhy = OutWhy.IsEmpty() ? Wider : OutWhy + EPS("；", "; ") + Wider;
	}
	InOutSpacingCm = Spacing;
	return Out.Num() > 0;
}

bool UExposureProbeSubsystem::VisiblePoint(const FVector& Eye, const TArray<FVector>& Body, const TArray<bool>* Valid, const FExposureBodyModel& Model, FVector& Out) const
{
	UWorld* World = GetEditorWorld();
	if (!World || Body.Num() == 0)
	{
		return false;
	}
	// The visible body point closest to the middle of the body, so the line shows a sightline that is actually clear.
	float MidZ = 0.f;
	for (const FVector& P : Body) { MidZ += P.Z / Body.Num(); }
	bool bFound = false;
	float Best = TNumericLimits<float>::Max();
	for (int32 p = 0; p < Body.Num(); ++p)
	{
		if (Valid && Valid->IsValidIndex(p) && !(*Valid)[p]) { continue; }
		AActor* Ignored = nullptr;
		if (FExposureProbeSolver::Trace(World, Eye, Body[p], Model, Ignored))
		{
			const float D = FMath::Abs(Body[p].Z - MidZ);
			if (D < Best) { Best = D; Out = Body[p]; bFound = true; }
		}
	}
	return bFound;
}

bool UExposureProbeSubsystem::ComputeHeatmap(FString& OutMessage)
{
	UWorld* World = GetEditorWorld();
	if (!World || (GEditor && GEditor->PlayWorld))
	{
		OutMessage = EPS("先停止运行", "Stop Play In Editor first");
		return false;
	}
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	FExposureBodyModel Model = FExposureBodyModel::FromSettings();
	if (S->HeatmapBody == EExposureHeatBody::Points16) { Model = Model.Points16(); }
	const double T0 = FPlatformTime::Seconds();

	FScopedSlowTask Task(100.f, EPT("分析地面：全图 A 与单向视线", "Analysing the floor: full-map A and one-way spots"));
	Task.MakeDialog(true);
	Task.EnterProgressFrame(2.f, EPT("采样地面", "Sampling the floor"));
	TArray<FExposureProbeSolver::FPoint> Points;
	FString Why;
	float Spacing = S->HeatmapSpacing * S->UnitToCm;
	FVector2D Origin;
	if (!BuildHeatSamples(Model, Spacing, Points, Origin, Why))
	{
		OutMessage = Why.IsEmpty() ? EPS("没有找到可站立的地面", "No standable floor found") : Why;
		return false;
	}

	FExposureHeatJob Job;
	Task.EnterProgressFrame(3.f, EPT("准备身体采样点", "Preparing body points"));
	Job.Prepare(World, Points, S->HeatmapRadius * S->UnitToCm, Model, S->bParallelTraces);
	const int32 Chunk = FMath::Max(1, Points.Num() / 90);
	float Done = 0.f;
	while (true)
	{
		if (Task.ShouldCancel())
		{
			OutMessage = EPS("已取消", "Cancelled");
			return false;
		}
		const bool bDone = Job.Step(World, Chunk);
		const float Now = Job.Progress() * 95.f;
		Task.EnterProgressFrame(FMath::Max(0.f, Now - Done), FText::Format(EPT("投射视线 {0} / {1}", "Casting sightlines {0} / {1}"),
			FText::AsNumber(FMath::RoundToInt(Job.Progress() * Points.Num())), FText::AsNumber(Points.Num())));
		Done = Now;
		if (bDone) { break; }
	}
	TArray<FExposureProbeSolver::FPointResult> Results;
	Job.Finish(Results);
	// One-way spots come from the same pair shares as A, so they cost no extra rays.
	Job.OneWayTargets(S->OneWayThreshold, OneWayTo);

	Heat.Reset(Points.Num());
	for (int32 i = 0; i < Points.Num(); ++i)
	{
		FExposureHeatPoint& H = Heat.AddDefaulted_GetRef();
		H.Feet = Points[i].Feet;
		H.bCrouch = Points[i].bCrouch;
		H.Vout = float(Results[i].Vout);
		H.Vin = float(Results[i].Vin);
		H.A = float(Results[i].A);
		H.Neighbors = Results[i].Neighbors;
		H.OneWaySees = OneWayTo[i].Num();
	}
	for (int32 i = 0; i < OneWayTo.Num(); ++i)
	{
		for (const int32 j : OneWayTo[i]) { ++Heat[j].OneWaySeenBy; }
		// Nearest first, for the hover lines.
		const FVector F = Heat[i].Feet;
		OneWayTo[i].Sort([this, F](int32 L, int32 R) { return FVector::DistSquared(Heat[L].Feet, F) < FVector::DistSquared(Heat[R].Feet, F); });
	}
	HeatModel = Job.GetModel();
	HeatBodies = Job.GetBodies();
	HeatValid = Job.GetValid();
	HeatEyes = Job.GetEyes();
	ProbeOneWayKey.Reset();

	HeatSpacingCm = Spacing;
	HeatOrigin = Origin;
	HeatRadiusCm = S->HeatmapRadius * S->UnitToCm;
	HeatRays = Job.Rays();
	HeatSeconds = FPlatformTime::Seconds() - T0;
	HeatTime = FDateTime::Now().ToString(TEXT("%H:%M"));
	HeatKey = CurrentHeatKey();
	bHeatStale = false;
	HighlightHeat = INDEX_NONE;
	HoverHeat = DrawnHover = INDEX_NONE;
	HeatGrid.Reset();
	for (int32 i = 0; i < Heat.Num(); ++i) { HeatGrid.FindOrAdd(ExposureHeat::Cell(Heat[i].Feet, HeatSpacingCm)).Add(i); }

	UE_LOG(LogExposureProbe, Log, TEXT("Floor analysis: %d samples, spacing %.0f cm, %lld rays, %d one-way spots, %.2f s"),
		Heat.Num(), HeatSpacingCm, HeatRays, GetOneWaySpotCount(), HeatSeconds);
	OutMessage = FString::Format(*EPS("{0} 个采样点 · {1} 条视线 · {2} 处单向视线位置 · {3} 秒", "{0} samples · {1} sightlines · {2} one-way spots · {3} s"),
		{ Heat.Num(), FText::AsNumber(HeatRays).ToString(), GetOneWaySpotCount(), FString::Printf(TEXT("%.1f"), HeatSeconds) });
	if (!Why.IsEmpty()) { OutMessage += TEXT("\n") + Why; }

	UpdateBaselineDiff();
	if (!bShowHeat && !bShowOneWay) { bShowHeat = true; }
	Redraw();
	RedrawHeat();
	OnReportUpdated.Broadcast();
	return true;
}

void UExposureProbeSubsystem::ClearHeatmap()
{
	Heat.Reset();
	HeatGrid.Reset();
	HeatDelta.Reset();
	HeatDeltaValid.Reset();
	BaselineOneWay.Reset();
	OneWayTo.Reset();
	HeatBodies.Reset();
	HeatValid.Reset();
	HeatEyes.Reset();
	ProbeOneWayKey.Reset();
	ProbeOneWayNames.Reset();
	CellThreatens.Reset();
	CellExposedTo.Reset();
	HighlightHeat = INDEX_NONE;
	HoverHeat = INDEX_NONE;
	bHeatStale = false;
	HeatShow = EExposureHeatShow::A;
	RedrawHeat();
	RedrawHover();
	OnReportUpdated.Broadcast();
}

void UExposureProbeSubsystem::DestroyHeatComponent()
{
	if (HeatComp)
	{
		if (HeatComp->IsRegistered()) { HeatComp->UnregisterComponent(); }
		HeatComp = nullptr;
	}
}

void UExposureProbeSubsystem::HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (HeatComp && HeatComp->GetWorld() == World)
	{
		DestroyHeatComponent();
	}
	if (SmokeComp && SmokeComp->GetWorld() == World)
	{
		DestroySmokeComponents();
		SmokeBorn.Reset();
		bSmokeDirty = true;
	}
}

void UExposureProbeSubsystem::UpdateProbeOneWay()
{
	// Floor cells against the selected probes: a cell that peeks a probe one-way is a danger to it,
	// a cell the probe peeks one-way is the probe's advantage. Cells outside the heatmap radius are skipped.
	TArray<int32> Idx;
	FString Key;
	for (const FString& N : SelectedProbes)
	{
		const int32 i = Solver.FindProbe(N);
		if (i == INDEX_NONE) { continue; }
		Idx.Add(i);
		const FExposureProbeDesc& D = Solver.Probes()[i];
		Key += FString::Printf(TEXT("%s %s %d %s|"), *N, *D.Feet.ToString(), D.bCrouch ? 1 : 0, *Solver.BodyOf(i).Key());
	}
	Key += FString::Printf(TEXT("#%d %.3f %g"), Heat.Num(), LastEditTime, GetDefault<UExposureProbeSettings>()->OneWayThreshold);
	if (Key == ProbeOneWayKey)
	{
		return;
	}
	ProbeOneWayKey = Key;
	ProbeOneWayNames.Reset();
	ProbeOneWayTeams.Reset();
	CellThreatens.Reset();
	CellExposedTo.Reset();
	UWorld* World = GetEditorWorld();
	if (!World || Idx.Num() == 0 || HeatBodies.Num() != Heat.Num())
	{
		return;
	}
	CellThreatens.SetNum(Heat.Num());
	CellExposedTo.SetNum(Heat.Num());
	const float Thr = GetDefault<UExposureProbeSettings>()->OneWayThreshold;
	const double R2 = FMath::Square(double(HeatRadiusCm));
	for (int32 k = 0; k < Idx.Num(); ++k)
	{
		const int32 Pi = Idx[k];
		const FExposureProbeDesc& D = Solver.Probes()[Pi];
		const FExposureBodyModel& PM = Solver.BodyOf(Pi);
		ProbeOneWayNames.Add(D.Name);
		ProbeOneWayTeams.Add(D.Team);
		TArray<FVector> Body;
		PM.BodyPoints(D.Feet, D.bCrouch, Body);
		TArray<bool> Valid;
		Valid.Init(false, Body.Num());
		bool bAny = false;
		for (int32 p = 0; p < Body.Num(); ++p)
		{
			AActor* Ignored = nullptr;
			Valid[p] = FExposureProbeSolver::Trace(World, FVector(D.Feet.X, D.Feet.Y, Body[p].Z), Body[p], PM, Ignored);
			bAny |= Valid[p];
		}
		if (!bAny) { Valid.Init(true, Body.Num()); }
		const FVector Eye = D.Feet + FVector(0, 0, PM.EyeCm(D.bCrouch));
		TArray<uint8> Rel;
		Rel.Init(0, Heat.Num());
		ParallelFor(Heat.Num(), [&](int32 c)
		{
			if (FVector::DistSquared(Heat[c].Feet, D.Feet) > R2) { return; }
			AActor* Ignored = nullptr;
			int32 Vis = 0, Cnt = 0;
			for (int32 p = 0; p < Body.Num(); ++p)
			{
				if (!Valid[p]) { continue; }
				++Cnt;
				Vis += FExposureProbeSolver::Trace(World, HeatEyes[c], Body[p], HeatModel, Ignored) ? 1 : 0;
			}
			const float CellSees = Cnt ? float(Vis) / Cnt : 0.f;
			Vis = 0; Cnt = 0;
			for (int32 p = 0; p < HeatBodies[c].Num(); ++p)
			{
				if (!HeatValid[c][p]) { continue; }
				++Cnt;
				Vis += FExposureProbeSolver::Trace(World, Eye, HeatBodies[c][p], PM, Ignored) ? 1 : 0;
			}
			const float ProbeSees = Cnt ? float(Vis) / Cnt : 0.f;
			Rel[c] = CellSees >= Thr && ProbeSees == 0.f ? 1 : (ProbeSees >= Thr && CellSees == 0.f ? 2 : 0);
		});
		for (int32 c = 0; c < Heat.Num(); ++c)
		{
			if (Rel[c] == 1) { CellThreatens[c].Add(k); }
			else if (Rel[c] == 2) { CellExposedTo[c].Add(k); }
		}
	}
}

void UExposureProbeSubsystem::UpdateHeatScale()
{
	const float Setting = GetDefault<UExposureProbeSettings>()->HeatmapScale;
	if (Setting > 0.f)
	{
		HeatScale = FMath::Max(0.02f, Setting);
		return;
	}
	TArray<float> Abs;
	Abs.Reserve(Heat.Num());
	for (int32 i = 0; i < Heat.Num(); ++i) { float V = 0.f; if (HeatValue(i, V)) { Abs.Add(FMath::Abs(V)); } }
	if (Abs.Num() == 0)
	{
		HeatScale = 0.1f;
		return;
	}
	Abs.Sort();
	HeatScale = FMath::Clamp(Abs[FMath::Min(Abs.Num() - 1, int32(0.9f * Abs.Num()))], 0.03f, 1.f);
}

void UExposureProbeSubsystem::RedrawHeat()
{
	UpdateHeatScale();
	UWorld* World = GetEditorWorld();
	ULineBatchComponent* Lines = World ? World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent) : nullptr;
	if (Lines)
	{
		Lines->ClearBatch(ExposureHeat::HighlightBatchId);
		Lines->ClearBatch(ExposureHeat::MarkBatchId);
	}
	const bool bHeatOn = bShowHeat && Heat.Num() > 0;
	const bool bOneWayOn = bShowOneWay && Heat.Num() > 0;
	if (bOneWayOn) { UpdateProbeOneWay(); }

	// Legend rows: counts, and drop a filter whose layer is off or whose mode no longer applies.
	FilterCounts.Init(0, int32(EExposureFloorFilter::Advantage) + 1);
	for (int32 f = 1; f < FilterCounts.Num(); ++f)
	{
		for (int32 i = 0; i < Heat.Num(); ++i) { FilterCounts[f] += CellMatches(i, EExposureFloorFilter(f)) ? 1 : 0; }
	}
	const bool bHeatFilter = FloorFilter == EExposureFloorFilter::SeenMore || FloorFilter == EExposureFloorFilter::SeesMore;
	const bool bProbeFilter = FloorFilter == EExposureFloorFilter::Threat || FloorFilter == EExposureFloorFilter::Advantage;
	if (FloorFilter != EExposureFloorFilter::None
		&& ((bHeatFilter && !bHeatOn) || (!bHeatFilter && !bOneWayOn) || (!bHeatFilter && bProbeFilter != HasProbeOneWay())))
	{
		FloorFilter = EExposureFloorFilter::None;
	}
	const bool bWayFilter = FloorFilter != EExposureFloorFilter::None && !(FloorFilter == EExposureFloorFilter::SeenMore || FloorFilter == EExposureFloorFilter::SeesMore);
	const bool bHeatDim = FloorFilter == EExposureFloorFilter::SeenMore || FloorFilter == EExposureFloorFilter::SeesMore;

	if (!World || (!bHeatOn && !bOneWayOn))
	{
		if (HeatComp) { HeatComp->SetVisibility(false); }
		UpdateLegendOverlay();
		if (GEditor) { GEditor->RedrawLevelEditingViewports(false); }
		return;
	}
	if (HeatComp && HeatComp->GetWorld() != World) { DestroyHeatComponent(); }
	if (!HeatComp)
	{
		HeatComp = NewObject<UExposureHeatComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		HeatComp->RegisterComponentWithWorld(World);
	}

	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const float Sp = HeatSpacingCm;
	TArray<FExposureHeatBucket> Buckets;

	if (bHeatOn)
	{
		// Blue-white-red over the floor: each grid cell is split into Sub x Sub patches whose value blends
		// bilinearly towards the neighbouring samples, so the colour runs smoothly from cell to cell.
		const float Scale = HeatScale;
		const int32 L = ExposureHeat::Levels;
		const int32 Sub = 4;
		const int32 Base = Buckets.Num();
		Buckets.SetNum(Base + 2 * (2 * L + 1));
		for (int32 d = 0; d < 2; ++d)
		{
			for (int32 b = 0; b < 2 * L + 1; ++b)
			{
				FExposureHeatBucket& B = Buckets[Base + d * (2 * L + 1) + b];
				const int32 Signed = b - L;
				const float T = float(FMath::Abs(Signed)) / L;
				B.Color = Signed > 0 ? S->HeatSeesColor : S->HeatSeenColor;
				B.Color.A = ExposureHeat::HeatAlpha(T) * (bOneWayOn ? 0.6f : 1.f) * (d == 1 ? 0.22f : 1.f);
				B.bQuads = true;
			}
		}
		const float SameFloor = 0.5f * HeatModel.CrouchHeight * HeatModel.UnitToCm;
		auto CellOf = [this, Sp](const FVector& F) { return FIntPoint(FMath::FloorToInt((F.X - HeatOrigin.X) / Sp), FMath::FloorToInt((F.Y - HeatOrigin.Y) / Sp)); };
		TMap<FIntPoint, TArray<int32>> ByCell;
		for (int32 i = 0; i < Heat.Num(); ++i) { ByCell.FindOrAdd(CellOf(Heat[i].Feet)).Add(i); }
		auto ValueNear = [&](const FIntPoint& C, float Z, float Fallback)
		{
			if (const TArray<int32>* Bucket = ByCell.Find(C))
			{
				for (const int32 j : *Bucket)
				{
					float V = 0.f;
					if (FMath::Abs(Heat[j].Feet.Z - Z) <= SameFloor && HeatValue(j, V)) { return V; }
				}
			}
			return Fallback;
		};
		for (int32 i = 0; i < Heat.Num(); ++i)
		{
			float V0 = 0.f;
			if (!HeatValue(i, V0))
			{
				continue;
			}
			const FVector& F = Heat[i].Feet;
			const FIntPoint C = CellOf(F);
			float Vx[2], Vy[2], Vxy[2][2];
			for (int32 k = 0; k < 2; ++k)
			{
				const int32 D = k == 0 ? -1 : 1;
				Vx[k] = ValueNear(C + FIntPoint(D, 0), F.Z, V0);
				Vy[k] = ValueNear(C + FIntPoint(0, D), F.Z, V0);
			}
			for (int32 kx = 0; kx < 2; ++kx)
			for (int32 ky = 0; ky < 2; ++ky)
			{
				Vxy[kx][ky] = ValueNear(C + FIntPoint(kx == 0 ? -1 : 1, ky == 0 ? -1 : 1), F.Z, 0.5f * (Vx[kx] + Vy[ky]));
			}
			const int32 Set = bHeatDim && !CellMatches(i, FloorFilter) ? 2 * L + 1 : 0;
			const float Z = F.Z + 3.f;
			for (int32 su = 0; su < Sub; ++su)
			for (int32 sv = 0; sv < Sub; ++sv)
			{
				const float Fx = (su + 0.5f) / Sub - 0.5f, Fy = (sv + 0.5f) / Sub - 0.5f;
				const int32 Ix = Fx < 0.f ? 0 : 1, Iy = Fy < 0.f ? 0 : 1;
				const float Wx = FMath::Abs(Fx), Wy = FMath::Abs(Fy);
				const float V = (1.f - Wx) * (1.f - Wy) * V0 + Wx * (1.f - Wy) * Vx[Ix] + (1.f - Wx) * Wy * Vy[Iy] + Wx * Wy * Vxy[Ix][Iy];
				const float Tv = FMath::Clamp(V / Scale, -1.f, 1.f);
				const int32 Level = FMath::Abs(Tv) < ExposureHeat::DeadZone ? 0 : FMath::Clamp(FMath::CeilToInt(FMath::Abs(Tv) * L), 1, L) * (Tv > 0.f ? 1 : -1);
				if (Level == 0)
				{
					continue;
				}
				const float X0 = F.X + (float(su) / Sub - 0.5f) * Sp, Y0 = F.Y + (float(sv) / Sub - 0.5f) * Sp;
				const float X1 = X0 + Sp / Sub, Y1 = Y0 + Sp / Sub;
				Buckets[Base + Set + Level + L].Centers.Append({ FVector(X0, Y0, Z), FVector(X1, Y0, Z), FVector(X1, Y1, Z), FVector(X0, Y1, Z) });
			}
		}
	}

	if (bOneWayOn)
	{
		// Grid cells in yellow, apart from the heatmap's red and blue; a thin amber outline keeps them visible on a white floor.
		const float Half = 0.40f * Sp;
		const float Z = bHeatOn ? 6.f : 2.5f;
		const FLinearColor Peek = UExposureProbeSettings::PeekColor();
		const FLinearColor Peeked = UExposureProbeSettings::PeekedColor();
		const FLinearColor Edge = UExposureProbeSettings::PeekEdgeColor();
		const FLinearColor Focus(0.05f, 0.05f, 0.06f);
		auto Add = [&Buckets, Half](const FLinearColor& Col, float Alpha)
		{
			FExposureHeatBucket& B = Buckets.AddDefaulted_GetRef();
			B.Color = Col;
			B.Color.A = Alpha;
			B.Radius = Half;
			B.bSquare = true;
			return Buckets.Num() - 1;
		};
		const float Alphas[4] = { 0.40f, 0.55f, 0.70f, 0.86f };
		const float Dim = 0.2f;
		const int32 GridB = bHeatOn ? INDEX_NONE : Add(FLinearColor(0.35f, 0.35f, 0.4f), 0.07f);
		const int32 P0 = Add(Peek, Alphas[0]); Add(Peek, Alphas[1]); Add(Peek, Alphas[2]); Add(Peek, Alphas[3]);
		const int32 PD0 = Add(Peek, Alphas[0] * Dim); Add(Peek, Alphas[1] * Dim); Add(Peek, Alphas[2] * Dim); Add(Peek, Alphas[3] * Dim);
		const int32 Dk = Add(Peeked, 0.38f);
		const int32 DkD = Add(Peeked, 0.45f * Dim);
		const double R2 = FMath::Square(double(HeatRadiusCm));
		auto Outline = [&](const FVector& C, bool bFocus, bool bNew)
		{
			if (!Lines) { return; }
			if (bFocus) { ExposureHeat::DrawSquare(Lines, C + FVector(0, 0, 1.f), Half + 3.f, Focus, 3.f, ExposureHeat::MarkBatchId); }
			else if (bNew) { ExposureHeat::DrawSquare(Lines, C + FVector(0, 0, 1.f), Half + 2.f, Focus, 2.f, ExposureHeat::MarkBatchId); }
			else { ExposureHeat::DrawSquare(Lines, C + FVector(0, 0, 1.f), Half, Edge, 1.f, ExposureHeat::MarkBatchId); }
		};
		for (int32 i = 0; i < Heat.Num(); ++i)
		{
			const FVector C = Heat[i].Feet + FVector(0, 0, Z);
			const bool bMatch = bWayFilter && CellMatches(i, FloorFilter);
			if (!HasProbeOneWay())
			{
				const int32 Lvl = OneWayLevel(i);
				if (Lvl > 0)
				{
					Buckets[(bWayFilter && !bMatch ? PD0 : P0) + Lvl - 1].Centers.Add(C);
					if (!bWayFilter || bMatch) { Outline(C, bMatch, IsNewOneWaySpot(i)); }
				}
				else if (Heat[i].OneWaySeenBy > 0)
				{
					Buckets[bWayFilter && !bMatch ? DkD : Dk].Centers.Add(C);
					if (bMatch) { Outline(C, true, false); }
				}
				else if (GridB != INDEX_NONE)
				{
					Buckets[GridB].Centers.Add(C);
				}
				continue;
			}
			// Against the selected probes: yellow where a cell peeks them one-way, dark where they peek the cell.
			const int32 NT = GetCellThreatens(i).Num(), NE = GetCellExposedTo(i).Num();
			if (NT > 0)
			{
				Buckets[(bWayFilter && !bMatch ? PD0 : P0) + FMath::Min(NT + 1, 4) - 1].Centers.Add(C);
				if (!bWayFilter || bMatch) { Outline(C, bMatch, false); }
			}
			else if (NE > 0)
			{
				Buckets[bWayFilter && !bMatch ? DkD : Dk].Centers.Add(C);
				if (bMatch) { Outline(C, true, false); }
			}
			else if (GridB != INDEX_NONE)
			{
				// Only the cells within reach of the selected probes, so the grid shows how far the analysis looked.
				for (const FString& N : ProbeOneWayNames)
				{
					const int32 Pi = Solver.FindProbe(N);
					if (Pi != INDEX_NONE && FVector::DistSquared(Heat[i].Feet, Solver.Probes()[Pi].Feet) <= R2) { Buckets[GridB].Centers.Add(C); break; }
				}
			}
		}
	}
	HeatComp->SetBuckets(MoveTemp(Buckets));
	HeatComp->SetVisibility(true);

	if (Lines && Heat.IsValidIndex(HighlightHeat))
	{
		const FVector C = Heat[HighlightHeat].Feet + FVector(0, 0, 8.f);
		ExposureHeat::DrawSquare(Lines, C, 0.62f * Sp, FLinearColor(0.05f, 0.05f, 0.06f), 3.5f, ExposureHeat::HighlightBatchId);
		ExposureHeat::DrawSquare(Lines, C + FVector(0, 0, 0.5f), 0.62f * Sp + 4.f, FLinearColor::White, 1.5f, ExposureHeat::HighlightBatchId);
	}
	UpdateLegendOverlay();
	if (GEditor) { GEditor->RedrawLevelEditingViewports(false); }
}

void UExposureProbeSubsystem::RedrawHover()
{
	DrawnHover = HoverHeat;
	UWorld* World = GetEditorWorld();
	ULineBatchComponent* Lines = World ? World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent) : nullptr;
	if (!Lines)
	{
		return;
	}
	Lines->ClearBatch(ExposureHeat::HoverBatchId);
	const int32 c = HoverHeat;
	if (bShowOneWay && Heat.IsValidIndex(c) && HeatEyes.IsValidIndex(c))
	{
		const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
		const FLinearColor Alert = UExposureProbeSettings::PeekEdgeColor();
		if (HasProbeOneWay())
		{
			for (const int32 k : GetCellThreatens(c))
			{
				const int32 Pi = Solver.FindProbe(ProbeOneWayNames[k]);
				if (Pi == INDEX_NONE) { continue; }
				const FExposureProbeDesc& D = Solver.Probes()[Pi];
				TArray<FVector> Body;
				Solver.BodyOf(Pi).BodyPoints(D.Feet, D.bCrouch, Body);
				FVector To;
				if (VisiblePoint(HeatEyes[c], Body, nullptr, HeatModel, To)) { Lines->DrawLine(HeatEyes[c], To, Alert, SDPG_World, 2.5f, -1.f, ExposureHeat::HoverBatchId); }
			}
			for (const int32 k : GetCellExposedTo(c))
			{
				const int32 Pi = Solver.FindProbe(ProbeOneWayNames[k]);
				if (Pi == INDEX_NONE) { continue; }
				const FExposureProbeDesc& D = Solver.Probes()[Pi];
				const FExposureBodyModel& PM = Solver.BodyOf(Pi);
				const FVector Eye = D.Feet + FVector(0, 0, PM.EyeCm(D.bCrouch));
				FVector To;
				if (VisiblePoint(Eye, HeatBodies[c], &HeatValid[c], PM, To)) { Lines->DrawLine(Eye, To, UExposureProbeSettings::PeekedColor(), SDPG_World, 2.5f, -1.f, ExposureHeat::HoverBatchId); }
			}
		}
		else
		{
			const TArray<int32>& To = GetOneWayTargets(c);
			for (int32 n = 0; n < To.Num() && n < ExposureHeat::MaxHoverLines; ++n)
			{
				const int32 j = To[n];
				FVector At;
				if (!HeatBodies.IsValidIndex(j) || !VisiblePoint(HeatEyes[c], HeatBodies[j], &HeatValid[j], HeatModel, At)) { continue; }
				Lines->DrawLine(HeatEyes[c], At, Alert, SDPG_World, 1.5f, -1.f, ExposureHeat::HoverBatchId);
				ExposureHeat::DrawSquare(Lines, Heat[j].Feet + FVector(0, 0, 7.f), 0.3f * HeatSpacingCm, Alert, 1.5f, ExposureHeat::HoverBatchId);
			}
		}
		ExposureHeat::DrawSquare(Lines, Heat[c].Feet + FVector(0, 0, 7.f), 0.5f * HeatSpacingCm, FLinearColor(0.05f, 0.05f, 0.06f), 2.5f, ExposureHeat::HoverBatchId);
	}
	if (GEditor) { GEditor->RedrawLevelEditingViewports(false); }
}

void UExposureProbeSubsystem::FocusViewportOnHeat(int32 Index, EExposureLayer Layer)
{
	if (!Heat.IsValidIndex(Index) || !GCurrentLevelEditingViewportClient)
	{
		return;
	}
	HighlightHeat = Index;
	HighlightLayer = Layer;
	const FVector Target = Heat[Index].Feet;
	const FVector Eye = Target + FVector(-0.55f, -0.55f, 1.f).GetSafeNormal() * 1400.f;
	GCurrentLevelEditingViewportClient->SetViewLocation(Eye);
	GCurrentLevelEditingViewportClient->SetViewRotation((Target - Eye).Rotation());
	GCurrentLevelEditingViewportClient->Invalidate();
	if (!IsLayerOn(Layer)) { SetLayer(Layer, true); }
	else { RedrawHeat(); }
}

int32 UExposureProbeSubsystem::OneWayLevel(int32 Index) const
{
	if (!Heat.IsValidIndex(Index)) { return 0; }
	const int32 N = Heat[Index].OneWaySees;
	return N <= 0 ? 0 : N == 1 ? 1 : N <= 3 ? 2 : N <= 7 ? 3 : 4;
}

bool UExposureProbeSubsystem::CellMatches(int32 Index, EExposureFloorFilter Filter) const
{
	if (!Heat.IsValidIndex(Index)) { return false; }
	const float Clear = ExposureHeat::ClearShare * HeatScale;
	float V = 0.f;
	switch (Filter)
	{
	case EExposureFloorFilter::SeenMore: return HeatValue(Index, V) && V <= -Clear;
	case EExposureFloorFilter::SeesMore: return HeatValue(Index, V) && V >= Clear;
	case EExposureFloorFilter::Peek1: return OneWayLevel(Index) == 1;
	case EExposureFloorFilter::Peek2: return OneWayLevel(Index) == 2;
	case EExposureFloorFilter::Peek4: return OneWayLevel(Index) == 3;
	case EExposureFloorFilter::Peek8: return OneWayLevel(Index) == 4;
	case EExposureFloorFilter::NewSpot: return IsNewOneWaySpot(Index);
	case EExposureFloorFilter::PeekedBy: return Heat[Index].OneWaySeenBy > 0 && Heat[Index].OneWaySees == 0;
	case EExposureFloorFilter::Threat: return GetCellThreatens(Index).Num() > 0;
	case EExposureFloorFilter::Advantage: return GetCellExposedTo(Index).Num() > 0;
	default: return false;
	}
}

int32 UExposureProbeSubsystem::FloorFilterCount(EExposureFloorFilter Filter) const
{
	return FilterCounts.IsValidIndex(int32(Filter)) ? FilterCounts[int32(Filter)] : 0;
}

TArray<int32> UExposureProbeSubsystem::FloorFilterSpots(EExposureFloorFilter Filter) const
{
	TArray<int32> Order;
	for (int32 i = 0; i < Heat.Num(); ++i) { if (CellMatches(i, Filter)) { Order.Add(i); } }
	auto Strength = [this, Filter](int32 i)
	{
		float V = 0.f;
		switch (Filter)
		{
		case EExposureFloorFilter::SeenMore: HeatValue(i, V); return -V;
		case EExposureFloorFilter::SeesMore: HeatValue(i, V); return V;
		case EExposureFloorFilter::PeekedBy: return float(Heat[i].OneWaySeenBy);
		case EExposureFloorFilter::Threat: return float(GetCellThreatens(i).Num() * 100 + Heat[i].OneWaySees);
		case EExposureFloorFilter::Advantage: return float(GetCellExposedTo(i).Num() * 100 + Heat[i].OneWaySeenBy);
		default: return float(Heat[i].OneWaySees);
		}
	};
	Order.Sort([&Strength](int32 L, int32 R) { return Strength(L) > Strength(R); });
	// Three grid steps apart, so stepping through them visits different places.
	TArray<int32> Out;
	const double MinD2 = FMath::Square(3.0 * HeatSpacingCm);
	for (const int32 i : Order)
	{
		if (Out.Num() >= 60) { break; }
		if (!Out.ContainsByPredicate([&](int32 j) { return FVector::DistSquared(Heat[i].Feet, Heat[j].Feet) < MinD2; })) { Out.Add(i); }
	}
	return Out;
}

void UExposureProbeSubsystem::SetFloorFilter(EExposureFloorFilter Filter)
{
	FloorFilter = Filter == FloorFilter ? EExposureFloorFilter::None : Filter;
	LegendStep = -1;
	HighlightHeat = INDEX_NONE;
	RedrawHeat();
	OnReportUpdated.Broadcast();
}

void UExposureProbeSubsystem::StepFloorFilter()
{
	const TArray<int32> Spots = FloorFilterSpots(FloorFilter);
	if (Spots.Num() == 0)
	{
		return;
	}
	LegendStep = (LegendStep + 1) % Spots.Num();
	const bool bHeat = FloorFilter == EExposureFloorFilter::SeenMore || FloorFilter == EExposureFloorFilter::SeesMore;
	FocusViewportOnHeat(Spots[LegendStep], bHeat ? EExposureLayer::Heatmap : EExposureLayer::OneWay);
}

namespace ExposureLegend
{
	static const FLinearColor Panel(0.03f, 0.03f, 0.035f, 0.84f);
	static const FLinearColor Ink(0.94f, 0.94f, 0.94f);
	static const FLinearColor Muted(0.62f, 0.62f, 0.64f);
	/** How a translucent cell of this colour looks on the white floor. */
	static FLinearColor OnFloor(const FLinearColor& C, float Alpha) { FLinearColor Out = FMath::Lerp(FLinearColor(0.95f, 0.95f, 0.95f), C, Alpha); Out.A = 1.f; return Out; }
	static FSlateFontInfo Small() { return FCoreStyle::GetDefaultFontStyle("Regular", 8); }
	static FSlateFontInfo Bold() { return FCoreStyle::GetDefaultFontStyle("Bold", 9); }
}

TSharedRef<SWidget> UExposureProbeSubsystem::BuildLegend()
{
	using namespace ExposureLegend;
	const TWeakObjectPtr<UExposureProbeSubsystem> Self(this);
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const FSlateBrush* White = FAppStyle::GetBrush("WhiteBrush");
	const float Scale = HeatScale;
	const float Clear = ExposureHeat::ClearShare * Scale;
	const bool bDelta = HeatShow == EExposureHeatShow::Delta;
	const bool bAuto = S->HeatmapScale <= 0.f;

	auto Swatch = [White](const FLinearColor& Fill, const FLinearColor& Edge, float EdgeWidth) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride(14.f).HeightOverride(14.f)
			[
				SNew(SBorder).BorderImage(White).BorderBackgroundColor(Edge).Padding(EdgeWidth)
				[
					SNew(SImage).Image(White).ColorAndOpacity(Fill)
				]
			];
	};
	// One legend row: clicking it isolates its cells; while isolated, the arrow steps the viewport through them.
	auto Row = [Self, White](EExposureFloorFilter F, TSharedRef<SWidget> Sw, const FString& Label) -> TSharedRef<SWidget>
	{
		auto Active = [Self, F]() { return Self.IsValid() && Self->GetFloorFilter() == F; };
		return SNew(SBorder)
			.BorderImage(White)
			.Padding(0)
			.BorderBackgroundColor_Lambda([Active]() { return Active() ? FLinearColor(1, 1, 1, 0.14f) : FLinearColor::Transparent; })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ContentPadding(FMargin(4, 2))
					.ToolTipText(EPT("点击：突出该类格子并淡化其余格子；再次点击恢复", "Click to isolate these cells and fade the rest; click again to show all"))
					.OnClicked_Lambda([Self, F]() { if (Self.IsValid()) { Self->SetFloorFilter(F); } return FReply::Handled(); })
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 7, 0)[ Sw ]
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(STextBlock).Font(Small())
							.Text_Lambda([Self, F, Label]()
							{
								const int32 N = Self.IsValid() ? Self->FloorFilterCount(F) : 0;
								return FText::FromString(Label + FString::Format(*EPS("  ·  {0} 格", "  ·  {0} cells"), { N }));
							})
							.ColorAndOpacity_Lambda([Active, Self, F]()
							{
								const bool bEmpty = !Self.IsValid() || Self->FloorFilterCount(F) == 0;
								return FSlateColor(Active() ? Ink : (bEmpty ? Muted * 0.7f : Muted));
							})
						]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ContentPadding(FMargin(6, 2))
					.Visibility_Lambda([Active]() { return Active() ? EVisibility::Visible : EVisibility::Collapsed; })
					.ToolTipText(EPT("视口移到下一个同类位置", "Move the viewport to the next such spot"))
					.OnClicked_Lambda([Self]() { if (Self.IsValid()) { Self->StepFloorFilter(); } return FReply::Handled(); })
					[
						SNew(STextBlock).Text(EPT("定位 ›", "Go ›")).Font(Small()).ColorAndOpacity(Ink)
					]
				]
			];
	};
	auto Title = [](const FString& T) -> TSharedRef<SWidget>
	{
		return SNew(STextBlock).Text(FText::FromString(T)).Font(Bold()).ColorAndOpacity(Ink);
	};

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	if (bShowHeat)
	{
		Box->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[
			Title(bDelta ? FString::Format(*EPS("热力图 · 相对「{0}」的变化 ΔA", "Heatmap · change ΔA since \"{0}\""), { BaselineDisplayName(Baseline.Name) })
				: EPS("热力图 · 暴露不对称度 A", "Heatmap · exposure asymmetry A"))
		];
		// Red (seen more, A < 0) through white to blue (sees more, A > 0), left to right as A grows.
		TSharedRef<SHorizontalBox> Bar = SNew(SHorizontalBox);
		const int32 N = 15;
		for (int32 k = 0; k < N; ++k)
		{
			const float T = (k + 0.5f) / N * 2.f - 1.f;
			const float W = FMath::Abs(T) < ExposureHeat::DeadZone ? 0.f : FMath::Abs(T);
			const FLinearColor C = OnFloor(T < 0.f ? S->HeatSeenColor : S->HeatSeesColor, ExposureHeat::HeatAlpha(W));
			Bar->AddSlot().AutoWidth()[ SNew(SBox).WidthOverride(14.f).HeightOverride(10.f)[ SNew(SImage).Image(White).ColorAndOpacity(C) ] ];
		}
		Box->AddSlot().AutoHeight().Padding(2, 0, 0, 1)[ Bar ];
		Box->AddSlot().AutoHeight().Padding(2, 0, 0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[ SNew(STextBlock).Font(Small()).ColorAndOpacity(Muted).Text(FText::FromString(FString::Printf(TEXT("−%.2f"), Scale))) ]
			+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Center)[ SNew(STextBlock).Font(Small()).ColorAndOpacity(Muted).Text(bAuto ? EPT("0  ·  色阶按本图自动", "0  ·  scale fitted to this map") : FText::FromString(TEXT("0"))) ]
			+ SHorizontalBox::Slot().AutoWidth()[ SNew(STextBlock).Font(Small()).ColorAndOpacity(Muted).Text(FText::FromString(FString::Printf(TEXT("+%.2f"), Scale))) ]
		];
		const FString Th = FString::Printf(TEXT("%.2f"), Clear);
		Box->AddSlot().AutoHeight()
		[
			Row(EExposureFloorFilter::SeenMore, Swatch(OnFloor(S->HeatSeenColor, ExposureHeat::HeatAlpha(0.7f)), FLinearColor::Transparent, 0.f),
				bDelta ? FString::Format(*EPS("比基线更暴露（ΔA ≤ −{0}）", "More exposed than in the baseline (ΔA ≤ −{0})"), { Th })
					: FString::Format(*EPS("被看见多于看见（A ≤ −{0}）", "Seen more than it sees (A ≤ −{0})"), { Th }))
		];
		Box->AddSlot().AutoHeight()
		[
			Row(EExposureFloorFilter::SeesMore, Swatch(OnFloor(S->HeatSeesColor, ExposureHeat::HeatAlpha(0.7f)), FLinearColor::Transparent, 0.f),
				bDelta ? FString::Format(*EPS("比基线更有利（ΔA ≥ +{0}）", "Better than in the baseline (ΔA ≥ +{0})"), { Th })
					: FString::Format(*EPS("看见多于被看见（A ≥ +{0}）", "Sees more than it is seen (A ≥ +{0})"), { Th }))
		];
	}
	if (bShowOneWay)
	{
		if (bShowHeat) { Box->AddSlot().AutoHeight().Padding(0, 6)[ SNew(SBox).HeightOverride(1.f)[ SNew(SImage).Image(White).ColorAndOpacity(FLinearColor(1, 1, 1, 0.12f)) ] ]; }
		const FLinearColor Peek = UExposureProbeSettings::PeekColor();
		const FLinearColor Edge = UExposureProbeSettings::PeekEdgeColor();
		const FLinearColor Dark = UExposureProbeSettings::PeekedColor();
		if (HasProbeOneWay())
		{
			const FString Names = FString::Join(ProbeOneWayNames, TEXT(", "));
			Box->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Title(FString::Format(*EPS("单向视线 · 和选中的 {0}", "One-way · with the selected {0}"), { Names })) ];
			Box->AddSlot().AutoHeight()[ Row(EExposureFloorFilter::Threat, Swatch(OnFloor(Peek, 0.7f), Edge, 1.5f),
				FString::Format(*EPS("此处单向看见 {0}（{0} 看不见此处）", "Sees {0} one-way ({0} cannot see here)"), { Names })) ];
			Box->AddSlot().AutoHeight()[ Row(EExposureFloorFilter::Advantage, Swatch(OnFloor(Dark, 0.45f), FLinearColor::Transparent, 0.f),
				FString::Format(*EPS("{0} 单向看见此处（此处看不见 {0}）", "{0} sees here one-way (cannot be seen from here)"), { Names })) ];
		}
		else
		{
			Box->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[ Title(EPS("单向视线 · 黄色格子看见其他位置，其他位置看不见它", "One-way · yellow cells see spots that cannot see them")) ];
			const float Alphas[4] = { 0.40f, 0.55f, 0.70f, 0.86f };
			const EExposureFloorFilter Rows[4] = { EExposureFloorFilter::Peek1, EExposureFloorFilter::Peek2, EExposureFloorFilter::Peek4, EExposureFloorFilter::Peek8 };
			const TCHAR* ZhLabels[4] = { TEXT("单向看见 1 处"), TEXT("单向看见 2–3 处"), TEXT("单向看见 4–7 处"), TEXT("单向看见 8 处以上") };
			const TCHAR* EnLabels[4] = { TEXT("sees 1 spot one-way"), TEXT("sees 2–3 spots"), TEXT("sees 4–7 spots"), TEXT("sees 8 or more") };
			for (int32 r = 0; r < 4; ++r)
			{
				Box->AddSlot().AutoHeight()[ Row(Rows[r], Swatch(OnFloor(Peek, Alphas[r]), Edge, 1.f), ExposureProbeText::Str(ZhLabels[r], EnLabels[r])) ];
			}
			if (bHasBaseline && BaselineOneWay.Num() > 0)
			{
				Box->AddSlot().AutoHeight()[ Row(EExposureFloorFilter::NewSpot, Swatch(OnFloor(Peek, 0.7f), Dark, 2.5f), EPS("相对基线新出现（深色粗框）", "New since the baseline (thick dark outline)")) ];
			}
			Box->AddSlot().AutoHeight()[ Row(EExposureFloorFilter::PeekedBy, Swatch(OnFloor(Dark, 0.38f), FLinearColor::Transparent, 0.f), EPS("被单向看见的位置（深灰）", "Spots seen one-way (dark grey)")) ];
		}
	}
	Box->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
	[
		SNew(STextBlock).Font(Small()).ColorAndOpacity(Muted).AutoWrapText(true)
		.Text(bShowOneWay ? EPT("点击一行只显示该类格子 · 悬停格子显示其视线 · 选中探针只显示与其相关的格子",
				"Click a row to isolate it · hover a cell to draw its sightlines · select probes to see only the cells related to them")
			: EPT("点击一行只显示该类格子 · 悬停地面读取数值", "Click a row to isolate it · hover the floor to read values"))
	];

	return SNew(SBox)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(12.f, 0.f, 0.f, 44.f))
		.Visibility(EVisibility::SelfHitTestInvisible)
		[
			SNew(SBox).WidthOverride(300.f)
			[
				SNew(SBorder)
				.BorderImage(White)
				.BorderBackgroundColor(Panel)
				.Padding(FMargin(10.f, 8.f))
				[
					Box
				]
			]
		];
}

void UExposureProbeSubsystem::UpdateLegendOverlay()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	const bool bOn = (bShowHeat || bShowOneWay) && HasHeat() && !(GEditor && GEditor->PlayWorld);
	TSharedPtr<SLevelViewport> Viewport;
	if (bOn && FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		Viewport = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor").GetFirstActiveLevelViewport();
	}
	const FString Key = bOn ? FString::Printf(TEXT("%d%d%d|%s|%s|%s|%d|%.3f"), bShowHeat ? 1 : 0, bShowOneWay ? 1 : 0, int32(HeatShow), *HeatTime,
		*FString::Join(ProbeOneWayNames, TEXT(",")), bHasBaseline ? *Baseline.Name : TEXT(""), BaselineOneWay.Num() > 0 ? 1 : 0, HeatScale) : FString();
	if (LegendWidget.IsValid() && (Key != LegendKey || LegendViewport.Pin() != Viewport))
	{
		RemoveLegendOverlay();
	}
	if (!Viewport.IsValid() || LegendWidget.IsValid())
	{
		return;
	}
	LegendWidget = BuildLegend();
	Viewport->AddOverlayWidget(LegendWidget.ToSharedRef());
	LegendViewport = Viewport;
	LegendKey = Key;
}

void UExposureProbeSubsystem::RemoveLegendOverlay()
{
	if (LegendWidget.IsValid())
	{
		if (const TSharedPtr<SLevelViewport> Viewport = LegendViewport.Pin()) { Viewport->RemoveOverlayWidget(LegendWidget.ToSharedRef()); }
	}
	LegendWidget.Reset();
	LegendViewport.Reset();
	LegendKey.Reset();
}

#undef LOCTEXT_NAMESPACE
