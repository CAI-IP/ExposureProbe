#include "ExposureProbeSubsystem.h"

#include "ExposureProbeActor.h"
#include "ExposureSmokeActor.h"
#include "ExposureHeatComponent.h"
#include "ExposureSmokeComponent.h"
#include "ExposureProbeSettings.h"
#include "ExposureProbeText.h"
#include "ActorEditorUtils.h"
#include "CanvasItem.h"
#include "Components/LineBatchComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Debug/DebugDrawService.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "SceneView.h"
#include "Engine/Selection.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"
#include "LevelEditorViewport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Notifications/SNotificationList.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ExposureProbeSubsystem)

#define LOCTEXT_NAMESPACE "ExposureProbe"

namespace ExposureProbe
{
	static constexpr uint32 LineBatchId = 0x45585042; // 'EXPB'

	static FString ModelKeyOf(const FExposureBodyModel& M)
	{
		return M.Key() + FString::Printf(TEXT("|%d"), GetDefault<UExposureProbeSettings>()->bSnapProbesToFloor ? 1 : 0);
	}

	static bool SameProbes(const TArray<FExposureProbeDesc>& A, const TArray<FExposureProbeDesc>& B)
	{
		if (A.Num() != B.Num()) { return false; }
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (A[i].Name != B[i].Name || A[i].Team != B[i].Team || A[i].bCrouch != B[i].bCrouch || !A[i].Feet.Equals(B[i].Feet, 0.01)
				|| A[i].bOwnBody != B[i].bOwnBody || (A[i].bOwnBody && A[i].Body.Key() != B[i].Body.Key()))
			{
				return false;
			}
		}
		return true;
	}

	static bool CountsDiffer(const FExposureEdge& A, const FExposureEdge& B)
	{
		return A.VisibleTtoCT != B.VisibleTtoCT || A.ValidCT != B.ValidCT || A.VisibleCTtoT != B.VisibleCTtoT || A.ValidT != B.ValidT;
	}

	static void HashDoubles(uint32& Hash, std::initializer_list<double> Values)
	{
		for (const double V : Values)
		{
			Hash = FCrc::MemCrc32(&V, sizeof(V), Hash);
		}
	}

	static FString Meters(float Cm)
	{
		return FString::Printf(TEXT("%.1f m"), Cm / 100.f);
	}
}

UExposureProbeSubsystem* UExposureProbeSubsystem::Get()
{
	return GEditor ? GEditor->GetEditorSubsystem<UExposureProbeSubsystem>() : nullptr;
}

void UExposureProbeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bAutoUpdate = GetDefault<UExposureProbeSettings>()->bAutoUpdate;

	if (GEngine)
	{
		MovedHandle = GEngine->OnActorMoved().AddUObject(this, &ThisClass::HandleActorMoved);
		AddedHandle = GEngine->OnLevelActorAdded().AddUObject(this, &ThisClass::HandleActorAdded);
		DeletedHandle = GEngine->OnLevelActorDeleted().AddUObject(this, &ThisClass::HandleActorDeleted);
	}
	PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &ThisClass::HandleObjectPropertyChanged);
	UndoHandle = FEditorDelegates::PostUndoRedo.AddUObject(this, &ThisClass::HandleUndoRedo);
	MapHandle = FEditorDelegates::MapChange.AddUObject(this, &ThisClass::HandleMapChange);
	SelectionHandle = USelection::SelectionChangedEvent.AddUObject(this, &ThisClass::HandleSelectionChanged);
	DrawHandle = UDebugDrawService::Register(TEXT("Editor"), FDebugDrawDelegate::CreateUObject(this, &ThisClass::DrawLabels));
	CleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::HandleWorldCleanup);
	SaveHandle = FEditorDelegates::PostSaveWorldWithContext.AddUObject(this, &ThisClass::HandlePostSaveWorld);
	bInitialized = true;
}

void UExposureProbeSubsystem::Deinitialize()
{
	bInitialized = false;
	if (GEngine)
	{
		GEngine->OnActorMoved().Remove(MovedHandle);
		GEngine->OnLevelActorAdded().Remove(AddedHandle);
		GEngine->OnLevelActorDeleted().Remove(DeletedHandle);
	}
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
	FEditorDelegates::PostUndoRedo.Remove(UndoHandle);
	FEditorDelegates::MapChange.Remove(MapHandle);
	USelection::SelectionChangedEvent.Remove(SelectionHandle);
	UDebugDrawService::Unregister(DrawHandle);
	FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
	FEditorDelegates::PostSaveWorldWithContext.Remove(SaveHandle);
	bShowHeat = bShowOneWay = false;
	HoverHeat = INDEX_NONE;
	RedrawHeat();
	RedrawHover();
	RemoveLegendOverlay();
	DestroyHeatComponent();
	bHasSmoke = false;
	DestroySmokeComponents();

	if (UWorld* World = GetEditorWorld())
	{
		if (ULineBatchComponent* Lines = World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent))
		{
			Lines->ClearBatch(ExposureProbe::LineBatchId);
			Lines->ClearBatch(0x45585053); // smoke lines, 'EXPS'
		}
	}
	Super::Deinitialize();
}

TStatId UExposureProbeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UExposureProbeSubsystem, STATGROUP_Tickables);
}

UWorld* UExposureProbeSubsystem::GetEditorWorld() const
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FString UExposureProbeSubsystem::LogDir()
{
	return FPaths::ProjectSavedDir() / TEXT("ExposureProbe");
}

void UExposureProbeSubsystem::Tick(float DeltaTime)
{
	if ((bShowHeat || bShowOneWay) && HasHeat() && GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
	{
		FIntPoint Mouse;
		GCurrentLevelEditingViewportClient->Viewport->GetMousePos(Mouse);
		if (Mouse != LastMouse)
		{
			LastMouse = Mouse;
			GCurrentLevelEditingViewportClient->Invalidate(false, false);
		}
	}
	if (HoverHeat != DrawnHover)
	{
		RedrawHover();
	}
	// Placed smokes block the probe rays: follow them while they are dragged (a sphere test on the cached rays, no casting).
	if (bBuilt && !bProbesDirty && GEditor && !GEditor->PlayWorld && FPlatformTime::Seconds() - LastLiveSmoke > 0.05)
	{
		LastLiveSmoke = FPlatformTime::Seconds();
		FString Key;
		const TArray<FExposureProbeSolver::FSmokeSphere> Smokes = ProbeSmokes(&Key);
		if (Key != LiveSmokeKey)
		{
			LiveSmokeUpdate(Smokes, Key);
			UpdateSmokeCutsLive();
			bPendingScan = true;
			LastEditTime = LastLiveSmoke;
			LastSmokeEdit = LastLiveSmoke;
			bSmokeDirty = true;
			// Evaluate the placed smokes for the probe on show, or for the last selected probe.
			if ((bHasSmoke && Smoke.bPlaced) || (!bHasSmoke && !LastSmokeObserver.IsEmpty()))
			{
				bSmokeEvalPending = true;
			}
		}
	}
	// Placed smokes: snap to the floor cells and evaluate again once the drag is over.
	const bool bDragging = (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetPressedMouseButtons().Num() > 0) || (GEditor && GEditor->IsTransactionActive());
	if ((PendingSnap.Num() > 0 || bSmokeEvalPending) && !bDragging && FPlatformTime::Seconds() - LastSmokeEdit > 0.15 && GEditor && !GEditor->PlayWorld)
	{
		SnapPendingSmokes();
		if (bSmokeEvalPending)
		{
			bSmokeEvalPending = false;
			FString Message;
			if (!EvaluatePlaced(bHasSmoke ? Smoke.Observer : FString(), false, Message))
			{
				// Every placed smoke is gone or switched off: drop the evaluation.
				ClearSmoke();
				if (Message != LoggedSmokeError) { UE_LOG(LogExposureProbe, Log, TEXT("%s"), *Message); LoggedSmokeError = Message; }
			}
		}
		bSmokeDirty = true;
	}
	if (bSmokeDirty || (SmokeComp && SmokeComp->NeedsVolumeRefresh()))
	{
		RedrawSmoke();
	}
	// The smoke drifts, so the viewport keeps redrawing while any is shown.
	if ((bHasSmoke || PlacedSmokeCount > 0) && GCurrentLevelEditingViewportClient)
	{
		GCurrentLevelEditingViewportClient->Invalidate(false, false);
	}
	if (!bAutoUpdate || !GEditor || GEditor->PlayWorld || !GetEditorWorld())
	{
		return;
	}
	const bool bParallel = GetDefault<UExposureProbeSettings>()->bParallelTraces;
	if (bNeedsInitialBuild)
	{
		bNeedsInitialBuild = false;
		RunUpdate(true, bParallel, false);
		return;
	}
	if (!HasPendingUpdate())
	{
		return;
	}
	// Wait until the drag is over: debounce, mouse released, no open transaction.
	if (FPlatformTime::Seconds() - LastEditTime < GetDefault<UExposureProbeSettings>()->DebounceSeconds)
	{
		return;
	}
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetPressedMouseButtons().Num() > 0)
	{
		return;
	}
	if (GEditor->IsTransactionActive())
	{
		return;
	}
	RunUpdate(false, bParallel, true);
}

// ---------------------------------------------------------------------------------------------
// Edit tracking

bool UExposureProbeSubsystem::IsEditorActor(const AActor* Actor) const
{
	return Actor && Actor->GetWorld() && Actor->GetWorld() == GetEditorWorld();
}

void UExposureProbeSubsystem::MarkPending(const AActor* Actor)
{
	if (!IsEditorActor(Actor))
	{
		return;
	}
	if (const AExposureSmokeActor* SmokeActor = Cast<AExposureSmokeActor>(Actor))
	{
		// Smokes block nothing in the probe audit or the floor analysis; they only change the smoke evaluation.
		HandleSmokeEdit(SmokeActor);
		return;
	}
	bSmokeStale |= bHasSmoke && !Smoke.bPlaced;
	if (bHasSmoke && Smoke.bPlaced)
	{
		bSmokeEvalPending = true;
		LastSmokeEdit = FPlatformTime::Seconds();
	}
	if (Actor->IsA<AExposureProbeActor>())
	{
		bProbesDirty = true;
	}
	else
	{
		bPendingScan = true;
		bHeatStale |= HasHeat();
	}
	LastEditTime = FPlatformTime::Seconds();
}

void UExposureProbeSubsystem::HandleActorMoved(AActor* Actor) { MarkPending(Actor); }
void UExposureProbeSubsystem::HandleActorAdded(AActor* Actor) { MarkPending(Actor); }
void UExposureProbeSubsystem::HandleActorDeleted(AActor* Actor) { MarkPending(Actor); }

void UExposureProbeSubsystem::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (!Object)
	{
		return;
	}
	if (Object->IsA<UExposureProbeSettings>())
	{
		const FString Prop = Event.GetMemberPropertyName().ToString();
		if (Prop.StartsWith(TEXT("Smoke")) || Prop == TEXT("bSmokeFog") || Prop == TEXT("bSnapSmokesToCells") || Prop == TEXT("UnitPreset") || Prop == TEXT("UnitToCm"))
		{
			RefreshSmokes();
		}
		if (Prop.StartsWith(TEXT("Smoke")) || Prop == TEXT("bSmokeFog") || Prop == TEXT("bSnapSmokesToCells"))
		{
			return;
		}
		bProbesDirty = true;
		LastEditTime = FPlatformTime::Seconds();
		if (UWorld* World = GetEditorWorld())
		{
			for (TActorIterator<AExposureProbeActor> It(World); It; ++It) { It->RefreshBody(); }
		}
		Redraw();
		return;
	}
	const AActor* Actor = Cast<AActor>(Object);
	if (!Actor)
	{
		if (const UActorComponent* Component = Cast<UActorComponent>(Object))
		{
			Actor = Component->GetOwner();
		}
	}
	if (Actor && Actor->IsA<AExposureProbeActor>() && Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(AExposureProbeActor, DesignIntent))
	{
		RefreshDesignIntent();
		return;
	}
	if (Actor && Actor->IsA<AExposureProbeActor>() && Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(AExposureProbeActor, HiddenLinks))
	{
		Redraw();
		return;
	}
	MarkPending(Actor);
}

void UExposureProbeSubsystem::HandleUndoRedo()
{
	bSmokeDirty = true;
	bSmokeEvalPending |= bHasSmoke && Smoke.bPlaced;
	LiveSmokeKey = TEXT("?");
	bHeatStale |= HasHeat();
	bPendingScan = true;
	bProbesDirty = true;
	LastEditTime = FPlatformTime::Seconds();
	RefreshDesignIntent();
}

void UExposureProbeSubsystem::HandleMapChange(uint32 Flags)
{
	ResetState();
	bNeedsInitialBuild = true;
}

void UExposureProbeSubsystem::HandleSelectionChanged(UObject* Object)
{
	TArray<FString> Names;
	bool bOnlySmokes = false;
	if (GEditor)
	{
		int32 Smokes = 0, Others = 0;
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			if (const AExposureProbeActor* Probe = Cast<AExposureProbeActor>(*It))
			{
				Names.Add(Probe->GetProbeName());
			}
			else if ((*It)->IsA<AExposureSmokeActor>()) { ++Smokes; }
			else { ++Others; }
		}
		bOnlySmokes = Smokes > 0 && Others == 0 && Names.Num() == 0;
	}
	// Picking a smoke to drag keeps the probes in focus, so their links follow the smoke.
	if (bOnlySmokes)
	{
		return;
	}
	if (Names != SelectedProbes)
	{
		SelectedProbes = MoveTemp(Names);
		if (SelectedProbes.Num() > 0) { LastSmokeObserver = SelectedProbes[0]; }
		// Placed smokes follow the selected probe.
		if (bHasSmoke && Smoke.bPlaced && SelectedProbes.Num() > 0 && SelectedProbes[0] != Smoke.Observer)
		{
			Smoke.Observer = SelectedProbes[0];
			bSmokeEvalPending = true;
		}
		Redraw();
		if (bShowOneWay && HasHeat())
		{
			RedrawHeat();
			OnReportUpdated.Broadcast();
		}
	}
}

void UExposureProbeSubsystem::ResetState()
{
	Solver = FExposureProbeSolver();
	ModelKey.Reset();
	Edges.Reset();
	LastReport = FExposureAuditReport();
	LastEditBoxes.Reset();
	GeomCache.Reset();
	bBuilt = false;
	bPendingScan = false;
	bProbesDirty = false;
	bLastInitial = true;
	FocusEdge.Reset();
	SelectedProbes.Reset();
	Heat.Reset();
	HeatGrid.Reset();
	HeatDelta.Reset();
	HeatDeltaValid.Reset();
	HeatKey.Reset();
	bHeatStale = false;
	HighlightHeat = INDEX_NONE;
	HeatShow = EExposureHeatShow::A;
	bHasBaseline = false;
	Baseline = FExposureBaseline();
	BaselineChanges.Reset();
	BaselineResolved = 0;
	BaselineMissing = 0;
	BaselineEditBoxes.Reset();
	BaselineGeomChanges.Reset();
	BaselineProbeChanges.Reset();
	bBaselineInputsMatch = false;
	BaselineInputDiff.Reset();
	BaselineOneWay.Reset();
	OneWayTo.Reset();
	HeatBodies.Reset();
	HeatValid.Reset();
	HeatEyes.Reset();
	ProbeOneWayKey.Reset();
	ProbeOneWayNames.Reset();
	ProbeOneWayTeams.Reset();
	CellThreatens.Reset();
	CellExposedTo.Reset();
	HoverHeat = DrawnHover = INDEX_NONE;
	FloorFilter = EExposureFloorFilter::None;
	RemoveLegendOverlay();
	if (HeatComp) { HeatComp->SetVisibility(false); }
	bHasSmoke = false;
	Smoke = FExposureSmokeResult();
	PendingSnap.Reset();
	bSmokeEvalPending = false;
	LastSmokeObserver.Reset();
	SmokeBorn.Reset();
	LiveSmokeKey.Reset();
	SmokeBoxes.Reset();
	SmokeLabels.Reset();
	bLiveSmoke = false;
	LiveBefore.Reset();
	LiveVisible.Reset();
	LiveBlocker.Reset();
	bSmokeDirty = true;
	RedrawSmoke();
	OnReportUpdated.Broadcast();
}

bool UExposureProbeSubsystem::ComputeGeom(const AActor* Actor, ECollisionChannel Channel, FExposureGeomState& Out)
{
	Out = FExposureGeomState();
	Out.Label = Actor->GetActorLabel();
	TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
	bool bAny = false;
	for (const UPrimitiveComponent* C : Primitives)
	{
		if (!C || !C->IsRegistered() || !CollisionEnabledHasQuery(C->GetCollisionEnabled()) || C->GetCollisionResponseToChannel(Channel) == ECR_Ignore)
		{
			continue;
		}
		const FBox Box = C->Bounds.GetBox();
		Out.Box += Box;
		const FTransform& T = C->GetComponentTransform();
		const FQuat Q = T.GetRotation();
		ExposureProbe::HashDoubles(Out.Hash, {
			T.GetLocation().X, T.GetLocation().Y, T.GetLocation().Z, Q.X, Q.Y, Q.Z, Q.W,
			T.GetScale3D().X, T.GetScale3D().Y, T.GetScale3D().Z,
			Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z });
		if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(C))
		{
			Out.Hash = HashCombine(Out.Hash, FCrc::StrCrc32(*GetPathNameSafe(Mesh->GetStaticMesh().Get())));
		}
		bAny = true;
	}
	return bAny && Out.Box.IsValid;
}

void UExposureProbeSubsystem::CollectBoundsChanges(TArray<FBox>& OutBoxes, TArray<FString>& OutActors)
{
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return;
	}
	const ECollisionChannel Channel = GetDefault<UExposureProbeSettings>()->TraceChannel;
	TMap<TWeakObjectPtr<AActor>, FExposureGeomState> Now;
	Now.Reserve(GeomCache.Num());
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor->IsA<AExposureProbeActor>() || Actor->IsA<AExposureSmokeActor>() || FActorEditorUtils::IsABuilderBrush(Actor))
		{
			continue;
		}
		FExposureGeomState State;
		if (!ComputeGeom(Actor, Channel, State))
		{
			continue;
		}
		if (const FExposureGeomState* Old = GeomCache.Find(Actor))
		{
			if (Old->Hash != State.Hash)
			{
				OutBoxes.Add(Old->Box);
				if (!Old->Box.Equals(State.Box)) { OutBoxes.Add(State.Box); }
				OutActors.Add(State.Label);
			}
		}
		else
		{
			OutBoxes.Add(State.Box);
			OutActors.Add(State.Label);
		}
		Now.Add(Actor, MoveTemp(State));
	}
	for (const TPair<TWeakObjectPtr<AActor>, FExposureGeomState>& Old : GeomCache)
	{
		if (!Now.Contains(Old.Key))
		{
			OutBoxes.Add(Old.Value.Box);
			OutActors.Add(Old.Value.Label + EPS("（已删除）", " (deleted)"));
		}
	}
	GeomCache = MoveTemp(Now);
}

// ---------------------------------------------------------------------------------------------
// Probes and design intent

TArray<FExposureProbeDesc> UExposureProbeSubsystem::CollectProbes(const FExposureBodyModel& Model, TArray<FString>& OutWarnings) const
{
	TArray<FExposureProbeDesc> Out;
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return Out;
	}
	const bool bSnap = GetDefault<UExposureProbeSettings>()->bSnapProbesToFloor;
	TSet<FString> Seen;
	for (TActorIterator<AExposureProbeActor> It(World); It; ++It)
	{
		const AExposureProbeActor* Probe = *It;
		if (!IsValid(Probe))
		{
			continue;
		}
		FExposureProbeDesc D;
		D.Name = Probe->GetProbeName();
		D.Team = Probe->Team;
		if (Seen.Contains(D.Name))
		{
			OutWarnings.Add(FString::Format(*EPS("探针名重复：{0}，只使用第一个", "Duplicate probe name {0}; only the first is used"), { D.Name }));
			continue;
		}
		Seen.Add(D.Name);
		FExposureBodyShape Own;
		D.bOwnBody = Probe->GetOwnBody(Own);
		D.Body = D.bOwnBody ? Model.WithShape(Own) : Model;
		const FVector Location = Probe->GetActorLocation();
		D.Feet = bSnap ? FExposureProbeSolver::SnapToFloor(World, Location, D.Body) : Location;
		if (!FExposureProbeSolver::ResolvePose(World, D.Feet, Probe->Pose, D.Body, D.bCrouch, D.ClearanceUnits))
		{
			OutWarnings.Add(FString::Format(*EPS("{0} 头顶净空 {1}，低于下蹲所需的 {2}，已跳过", "{0} skipped: head clearance {1} is below {2}"),
				{ D.Name, FString::Printf(TEXT("%.0f"), D.ClearanceUnits), FString::Printf(TEXT("%.0f"), D.Body.CrouchHeight) }));
			continue;
		}
		Out.Add(D);
	}
	Out.Sort([](const FExposureProbeDesc& L, const FExposureProbeDesc& R) { return L.Name < R.Name; });

	const int32 NumT = Out.FilterByPredicate([](const FExposureProbeDesc& D) { return D.Team == EExposureTeam::TSide; }).Num();
	if (NumT == 0 || NumT == Out.Num())
	{
		OutWarnings.Add(EPS("至少放置一个 T 探针和一个 CT 探针", "Place at least one T probe and one CT probe"));
	}
	return Out;
}

AExposureProbeActor* UExposureProbeSubsystem::FindProbeActor(const FString& Name) const
{
	if (UWorld* World = GetEditorWorld())
	{
		for (TActorIterator<AExposureProbeActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetProbeName() == Name)
			{
				return *It;
			}
		}
	}
	return nullptr;
}

void UExposureProbeSubsystem::ApplyDesignIntent(TArray<FExposureEdge>& InOutEdges) const
{
	TMap<FString, const AExposureProbeActor*> TProbes;
	if (UWorld* World = GetEditorWorld())
	{
		for (TActorIterator<AExposureProbeActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->Team == EExposureTeam::TSide) { TProbes.Add(It->GetProbeName(), *It); }
		}
	}
	for (FExposureEdge& E : InOutEdges)
	{
		E.bDesignIntent = false;
		E.DesignNote.Reset();
		if (const AExposureProbeActor* const* T = TProbes.Find(E.TProbe))
		{
			if (const FExposureDesignMark* Mark = (*T)->DesignIntent.FindByPredicate([&E](const FExposureDesignMark& M) { return M.CTProbe == E.CTProbe; }))
			{
				E.bDesignIntent = true;
				E.DesignNote = Mark->Note;
			}
		}
	}
}

void UExposureProbeSubsystem::Summarize(FExposureAuditReport& R, bool bInitial) const
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	R.NewOneWay = 0;
	R.NewOneWayDesignIntent = 0;
	R.FarChanges = 0;
	R.FarthestCm = 0.f;
	for (const FExposureEdgeChange& C : R.Changes)
	{
		if (C.bNewOneWay) { (C.After.bDesignIntent ? R.NewOneWayDesignIntent : R.NewOneWay)++; }
		if (C.bFar) { ++R.FarChanges; }
		R.FarthestCm = FMath::Max(R.FarthestCm, C.DistanceCm);
	}
	if (bInitial)
	{
		int32 OneWay = 0;
		for (const FExposureEdge& E : R.Edges) { OneWay += E.bOneWay ? 1 : 0; }
		R.Summary = FString::Format(*EPS("全量计算：{0} 对探针，{1} 对单向视线", "Full recompute: {0} probe pairs, {1} one-way"), { R.Edges.Num(), OneWay });
		return;
	}
	R.Summary = FString::Format(*EPS("本次改动影响 {0} 对探针，其中 {1} 对新增单向视线", "This edit affects {0} probe pairs; {1} with new one-way sightlines"),
		{ R.ChangedEdges, R.NewOneWay });
	if (R.FarChanges > 0)
	{
		R.Summary += FString::Format(*EPS("；{0} 对在 {1} 以外", "; {0} beyond {1}"),
			{ R.FarChanges, ExposureProbe::Meters(S->FarDistance * S->UnitToCm) });
	}
	if (R.NewOneWayDesignIntent > 0)
	{
		R.Summary += FString::Format(*EPS("（另有 {0} 对已标为设计保留）", " ({0} more marked as design intent)"), { R.NewOneWayDesignIntent });
	}
}

void UExposureProbeSubsystem::RefreshDesignIntent()
{
	ApplyDesignIntent(Edges);
	LastReport.Edges = Edges;
	TArray<FExposureEdge> After;
	for (const FExposureEdgeChange& C : LastReport.Changes) { After.Add(C.After); }
	ApplyDesignIntent(After);
	for (int32 i = 0; i < After.Num(); ++i) { LastReport.Changes[i].After = After[i]; }
	Summarize(LastReport, bLastInitial);
	UpdateBaselineDiff();
	OnReportUpdated.Broadcast();
	Redraw();
}

bool UExposureProbeSubsystem::MarkDesignIntent(const FString& Edge, const FString& Note, FString& OutError)
{
	const FExposureEdge* Found = Edges.FindByPredicate([&Edge](const FExposureEdge& E) { return E.Edge == Edge; });
	if (!Found)
	{
		OutError = FString::Printf(TEXT("Unknown probe pair %s"), *Edge);
		return false;
	}
	const FString CT = Found->CTProbe;
	AExposureProbeActor* T = FindProbeActor(Found->TProbe);
	if (!T)
	{
		OutError = FString::Printf(TEXT("Probe %s not found"), *Found->TProbe);
		return false;
	}
	{
		const FScopedTransaction Transaction(EPT("标为设计保留", "Mark as Design Intent"));
		T->Modify();
		FExposureDesignMark* Mark = T->DesignIntent.FindByPredicate([&CT](const FExposureDesignMark& M) { return M.CTProbe == CT; });
		if (!Mark)
		{
			Mark = &T->DesignIntent.AddDefaulted_GetRef();
			Mark->CTProbe = CT;
		}
		Mark->Note = Note;
	}
	RefreshDesignIntent();
	WriteEvent(TEXT("DesignIntentMarked"), Edge, Note);
	return true;
}

bool UExposureProbeSubsystem::ClearDesignIntent(const FString& Edge, FString& OutError)
{
	const FExposureEdge* Found = Edges.FindByPredicate([&Edge](const FExposureEdge& E) { return E.Edge == Edge; });
	if (!Found)
	{
		OutError = FString::Printf(TEXT("Unknown probe pair %s"), *Edge);
		return false;
	}
	const FString CT = Found->CTProbe;
	AExposureProbeActor* T = FindProbeActor(Found->TProbe);
	if (!T)
	{
		OutError = FString::Printf(TEXT("Probe %s not found"), *Found->TProbe);
		return false;
	}
	{
		const FScopedTransaction Transaction(EPT("取消设计保留", "Clear Design Intent"));
		T->Modify();
		T->DesignIntent.RemoveAll([&CT](const FExposureDesignMark& M) { return M.CTProbe == CT; });
	}
	RefreshDesignIntent();
	WriteEvent(TEXT("DesignIntentCleared"), Edge, FString());
	return true;
}

// ---------------------------------------------------------------------------------------------
// Updates

const FExposureAuditReport& UExposureProbeSubsystem::Rebuild(bool bParallel)
{
	return RunUpdate(true, bParallel, false);
}

const FExposureAuditReport& UExposureProbeSubsystem::UpdateNow()
{
	const bool bParallel = GetDefault<UExposureProbeSettings>()->bParallelTraces;
	return RunUpdate(!bBuilt, bParallel, false);
}

const FExposureAuditReport& UExposureProbeSubsystem::RunUpdate(bool bForceFull, bool bParallel, bool bQuiet)
{
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return LastReport;
	}
	const UExposureProbeSettings* Settings = GetDefault<UExposureProbeSettings>();
	const FExposureBodyModel Model = FExposureBodyModel::FromSettings();
	const FString Key = ExposureProbe::ModelKeyOf(Model);
	const double T0 = FPlatformTime::Seconds();

	const bool bHadCache = bBuilt;
	TArray<FString> Warnings;
	const TArray<FExposureProbeDesc> Probes = CollectProbes(Model, Warnings);
	TArray<FBox> Boxes;
	TArray<FString> EditedActors;
	CollectBoundsChanges(Boxes, EditedActors);
	// Placed smokes that moved, appeared or went since the last update count as edits too.
	FString SmokeKeyNow;
	TMap<FString, FBox> SmokeBoxesNow;
	TMap<FString, FString> SmokeLabelsNow;
	const TArray<FExposureProbeSolver::FSmokeSphere> Smokes = ProbeSmokes(&SmokeKeyNow, &SmokeBoxesNow, &SmokeLabelsNow);
	if (bBuilt)
	{
		for (const TPair<FString, FBox>& KV : SmokeBoxesNow)
		{
			const FBox* Old = SmokeBoxes.Find(KV.Key);
			if (Old && Old->Equals(KV.Value)) { continue; }
			if (Old) { Boxes.Add(*Old); }
			Boxes.Add(KV.Value);
			EditedActors.Add(SmokeLabelsNow.FindRef(KV.Key));
		}
		for (const TPair<FString, FBox>& KV : SmokeBoxes)
		{
			if (!SmokeBoxesNow.Contains(KV.Key))
			{
				Boxes.Add(KV.Value);
				EditedActors.Add(SmokeLabels.FindRef(KV.Key));
			}
		}
	}
	SmokeBoxes = MoveTemp(SmokeBoxesNow);
	SmokeLabels = MoveTemp(SmokeLabelsNow);

	const bool bSameRays = bBuilt && Key == ModelKey && ExposureProbe::SameProbes(Probes, Solver.Probes());
	const bool bFull = bForceFull || !bSameRays;
	bPendingScan = false;
	bProbesDirty = false;
	if (bQuiet && !bFull && Boxes.Num() == 0)
	{
		// Nothing changed since the last report (e.g. a smoke dragged back to where it was): the live result stands.
		bLiveSmoke = false;
		LiveBefore.Reset();
		LiveVisible.Reset();
		LiveBlocker.Reset();
		return LastReport;
	}

	TArray<bool> VisibleBefore;
	TArray<FString> BlockerBefore;
	if (bSameRays && bLiveSmoke && LiveVisible.Num() == Solver.Rays().Num())
	{
		VisibleBefore = LiveVisible;
		BlockerBefore = LiveBlocker;
	}
	else if (bSameRays)
	{
		for (const FExposureRay& R : Solver.Rays())
		{
			VisibleBefore.Add(R.bVisible);
			BlockerBefore.Add(R.Blocker);
		}
	}
	const TArray<FExposureEdge> Before = bLiveSmoke ? LiveBefore : Edges;
	bLiveSmoke = false;
	LiveBefore.Reset();
	LiveVisible.Reset();
	LiveBlocker.Reset();

	TArray<int32> Indices;
	if (bFull)
	{
		if (!bSameRays)
		{
			Solver.Build(Probes, Model);
			ModelKey = Key;
		}
		Indices.Reserve(Solver.Rays().Num());
		for (int32 i = 0; i < Solver.Rays().Num(); ++i) { Indices.Add(i); }
	}
	else
	{
		Indices = Solver.RaysCrossing(Boxes);
	}
	const double T1 = FPlatformTime::Seconds();
	Solver.Cast(World, Indices, bParallel);
	Solver.ApplySmokes(Smokes);
	LiveSmokeKey = SmokeKeyNow;
	const double T2 = FPlatformTime::Seconds();
	TArray<FExposureEdge> After = Solver.ComputeEdges(Settings->OneWayThreshold);
	ApplyDesignIntent(After);
	const double T3 = FPlatformTime::Seconds();

	FExposureAuditReport R;
	R.Time = FDateTime::Now().ToIso8601();
	R.bFullRecompute = bFull;
	R.bParallel = bParallel;
	R.Probes = Probes.Num();
	R.RaysTotal = Solver.Rays().Num();
	R.RaysRecast = Indices.Num();
	if (bHadCache)
	{
		R.DirtyBoxes = Boxes.Num();
		R.EditedActors = EditedActors;
		LastEditBoxes = Boxes;
	}
	else
	{
		LastEditBoxes.Reset();
	}
	R.Warnings = Warnings;

	const bool bInitial = Before.Num() == 0;
	if (!bInitial)
	{
		TMap<FString, const FExposureEdge*> BeforeByName;
		for (const FExposureEdge& E : Before) { BeforeByName.Add(E.Edge, &E); }
		const float FarCm = Settings->FarDistance * Settings->UnitToCm;
		for (const FExposureEdge& E : After)
		{
			const FExposureEdge* B = BeforeByName.FindRef(E.Edge);
			if (B && !ExposureProbe::CountsDiffer(*B, E))
			{
				continue;
			}
			FExposureEdgeChange& C = R.Changes.AddDefaulted_GetRef();
			C.Edge = E.Edge;
			C.bHadBefore = B != nullptr;
			if (B) { C.Before = *B; }
			C.After = E;
			C.bNewOneWay = E.bOneWay && !(B && B->bOneWay);
			if (LastEditBoxes.Num() > 0)
			{
				const int32 Ti = Solver.FindProbe(E.TProbe), Ci = Solver.FindProbe(E.CTProbe);
				double Best = TNumericLimits<double>::Max();
				for (const FBox& Box : LastEditBoxes)
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
		R.Changes.Sort([](const FExposureEdgeChange& L, const FExposureEdgeChange& Rr)
		{
			if (L.bNewOneWay != Rr.bNewOneWay) { return L.bNewOneWay; }
			return FMath::Abs(L.After.A - L.Before.A) > FMath::Abs(Rr.After.A - Rr.Before.A);
		});
		for (FExposureEdgeChange& C : R.Changes)
		{
			C.Kind = ClassifyChange(C, Settings->ChangeThreshold);
			C.bExpected = C.After.bDesignIntent || C.Before.bDesignIntent;
		}
	}
	R.ChangedEdges = R.Changes.Num();

	if (bSameRays)
	{
		const TArray<FExposureRay>& Rays = Solver.Rays();
		for (int32 i = 0; i < Rays.Num(); ++i)
		{
			const FExposureRay& Ray = Rays[i];
			if (Ray.bValidity || Ray.bVisible == VisibleBefore[i])
			{
				continue;
			}
			const bool bCrouch = Solver.Probes()[Ray.To].bCrouch;
			FExposureRayChange& Ev = R.Evidence.AddDefaulted_GetRef();
			Ev.Direction = Solver.RayDirection(i);
			const FExposureBodyModel& ToBody = Solver.BodyOf(Ray.To);
			Ev.Region = ToBody.RegionOf(Ray.Point, bCrouch);
			Ev.HeightUnits = ToBody.PointHeight(Ray.Point, bCrouch);
			Ev.AngleDeg = ToBody.BearingDeg(Ray.Point);
			Ev.Change = Ray.bVisible ? TEXT("opened") : TEXT("blocked");
			Ev.Blocker = Ray.bVisible ? BlockerBefore[i] : Ray.Blocker;
		}
	}

	R.Edges = After;
	R.FilterMs = (T1 - T0) * 1000.0;
	R.CastMs = (T2 - T1) * 1000.0;
	R.MetricMs = (T3 - T2) * 1000.0;
	R.TotalMs = (T3 - T0) * 1000.0;
	Summarize(R, bInitial);

	Edges = MoveTemp(After);
	LastReport = MoveTemp(R);
	bLastInitial = bInitial;
	bBuilt = true;
	if (!FocusEdge.IsEmpty() && !Edges.ContainsByPredicate([this](const FExposureEdge& E) { return E.Edge == FocusEdge; }))
	{
		FocusEdge.Reset();
	}

	for (TActorIterator<AExposureProbeActor> It(World); It; ++It)
	{
		const int32 Pi = Solver.FindProbe(It->GetProbeName());
		It->SetDisplayState(Pi != INDEX_NONE && Solver.Probes()[Pi].bCrouch, Pi == INDEX_NONE);
	}

	UE_LOG(LogExposureProbe, Log, TEXT("%s | %s, rays %d/%d, filter %.2f ms, cast %.2f ms, total %.2f ms"), *LastReport.Summary,
		bFull ? TEXT("full") : TEXT("local"), LastReport.RaysRecast, LastReport.RaysTotal, LastReport.FilterMs, LastReport.CastMs, LastReport.TotalMs);
	for (const FString& W : LastReport.Warnings)
	{
		UE_LOG(LogExposureProbe, Warning, TEXT("%s"), *W);
	}
	WriteLog(LastReport);
	if (!bInitial && LastReport.ChangedEdges > 0)
	{
		Notify(LastReport);
	}
	UpdateBaselineDiff();
	Redraw();
	OnReportUpdated.Broadcast();
	return LastReport;
}

// ---------------------------------------------------------------------------------------------
// Output

void UExposureProbeSubsystem::WriteLog(const FExposureAuditReport& Report) const
{
	TSharedPtr<FJsonObject> Obj = FJsonObjectConverter::UStructToJsonObject(Report);
	if (!Obj.IsValid())
	{
		return;
	}
	Obj->SetStringField(TEXT("Event"), TEXT("Update"));
	if (UWorld* World = GetEditorWorld())
	{
		Obj->SetStringField(TEXT("Level"), World->GetOutermost()->GetName());
	}
	FString Line;
	FJsonSerializer::Serialize(Obj.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line));
	FString Pretty;
	FJsonSerializer::Serialize(Obj.ToSharedRef(), TJsonWriterFactory<>::Create(&Pretty));

	IFileManager::Get().MakeDirectory(*LogDir(), true);
	FFileHelper::SaveStringToFile(Line + LINE_TERMINATOR, *(LogDir() / TEXT("audit_log.jsonl")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append);
	FFileHelper::SaveStringToFile(Pretty, *(LogDir() / TEXT("last_report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void UExposureProbeSubsystem::WriteEvent(const FString& Event, const FString& Edge, const FString& Note) const
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("Event"), Event);
	Obj->SetStringField(TEXT("Time"), FDateTime::Now().ToIso8601());
	Obj->SetStringField(TEXT("Edge"), Edge);
	Obj->SetStringField(TEXT("Note"), Note);
	if (UWorld* World = GetEditorWorld())
	{
		Obj->SetStringField(TEXT("Level"), World->GetOutermost()->GetName());
	}
	FString Line;
	FJsonSerializer::Serialize(Obj, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line));
	IFileManager::Get().MakeDirectory(*LogDir(), true);
	FFileHelper::SaveStringToFile(Line + LINE_TERMINATOR, *(LogDir() / TEXT("audit_log.jsonl")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append);
}

void UExposureProbeSubsystem::Notify(const FExposureAuditReport& Report) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FNotificationInfo Info(FText::FromString(Report.Summary));
	Info.ExpireDuration = 5.f;
	Info.bFireAndForget = true;
	if (Report.NewOneWay > 0)
	{
		Info.Image = FCoreStyle::Get().GetBrush("Icons.WarningWithColor");
	}
	FSlateNotificationManager::Get().AddNotification(Info);
}

void UExposureProbeSubsystem::SetDrawMode(EExposureDrawMode Mode)
{
	DrawMode = Mode;
	Redraw();
}

void UExposureProbeSubsystem::SetDrawStyle(EExposureDrawStyle Style)
{
	DrawStyle = Style;
	Redraw();
}

void UExposureProbeSubsystem::SetFocusEdge(const FString& Edge)
{
	FocusEdge = Edge;
	Redraw();
}

void UExposureProbeSubsystem::FocusViewportOnEdge(const FString& Edge) const
{
	// Edge "A->B": place the viewport camera at A's eye, looking at B's chest.
	FString From, To;
	if (!Edge.Split(TEXT("->"), &From, &To) || !GCurrentLevelEditingViewportClient)
	{
		return;
	}
	const int32 Fi = Solver.FindProbe(From), Ti = Solver.FindProbe(To);
	if (Fi == INDEX_NONE || Ti == INDEX_NONE)
	{
		return;
	}
	const FExposureBodyModel& FM = Solver.BodyOf(Fi);
	const FExposureBodyModel& TM = Solver.BodyOf(Ti);
	const FExposureProbeDesc& F = Solver.Probes()[Fi];
	const FExposureProbeDesc& T = Solver.Probes()[Ti];
	const FVector Eye = F.Feet + FVector(0, 0, FM.EyeCm(F.bCrouch));
	const FVector Chest = T.Feet + FVector(0, 0, 0.5f * (TM.NeckUnits(T.bCrouch) + TM.CrotchUnits(T.bCrouch)) * TM.UnitToCm);
	GCurrentLevelEditingViewportClient->SetViewLocation(Eye);
	GCurrentLevelEditingViewportClient->SetViewRotation((Chest - Eye).Rotation());
	GCurrentLevelEditingViewportClient->Invalidate();
}

TArray<FString> UExposureProbeSubsystem::EdgesToDraw() const
{
	TArray<FString> Out;
	if (!bBuilt || DrawMode == EExposureDrawMode::Off)
	{
		return Out;
	}
	if (!FocusEdge.IsEmpty())
	{
		Out.Add(FocusEdge);
		return Out;
	}
	if (SelectedProbes.Num() > 0)
	{
		// Probes of both sides selected: only the pairs among them. One side only: every pair of the selected probes.
		bool bT = false, bCT = false;
		for (const FExposureEdge& E : Edges)
		{
			bT |= SelectedProbes.Contains(E.TProbe);
			bCT |= SelectedProbes.Contains(E.CTProbe);
		}
		for (const FExposureEdge& E : Edges)
		{
			const bool bHasT = SelectedProbes.Contains(E.TProbe), bHasCT = SelectedProbes.Contains(E.CTProbe);
			if (bT && bCT ? (bHasT && bHasCT) : (bHasT || bHasCT)) { Out.Add(E.Edge); }
		}
		return Out;
	}
	if (DrawMode == EExposureDrawMode::All)
	{
		for (const FExposureEdge& E : Edges) { Out.Add(E.Edge); }
	}
	else
	{
		for (const FExposureEdgeChange& C : ActiveChanges()) { Out.Add(C.Edge); }
	}
	return Out;
}

TMap<FString, AExposureProbeActor*> UExposureProbeSubsystem::ProbeActors() const
{
	TMap<FString, AExposureProbeActor*> Out;
	if (UWorld* World = GetEditorWorld())
	{
		for (TActorIterator<AExposureProbeActor> It(World); It; ++It)
		{
			if (IsValid(*It) && !Out.Contains(It->GetProbeName())) { Out.Add(It->GetProbeName(), *It); }
		}
	}
	return Out;
}

void UExposureProbeSubsystem::DrawRays(ULineBatchComponent* Lines, int32 T, int32 C) const
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const TArray<FExposureRay>& Rays = Solver.Rays();
	const float Thick = S->RayThickness;
	const uint32 Id = ExposureProbe::LineBatchId;
	for (const int32 From : { T, C })
	{
		const int32 To = From == T ? C : T;
		const int32 Base = Solver.PairBase(From, To);
		const FLinearColor Col = From == T ? S->TColor : S->CTColor;
		for (int32 p = 0; p < Solver.PointsPerBody() && Base != INDEX_NONE; ++p)
		{
			if (!Solver.PointValid(To, p))
			{
				continue;
			}
			const FExposureRay& R = Rays[Base + p];
			if (R.bVisible)
			{
				Lines->DrawLine(R.Start, R.End, Col, SDPG_World, Thick, -1.f, Id);
			}
			else if (S->bShowBlockedRays)
			{
				Lines->DrawLine(R.Start, R.Hit, Col * 0.45f, SDPG_World, Thick * 0.5f, -1.f, Id);
				const float X = 4.f;
				Lines->DrawLine(R.Hit + FVector(-X, -X, 0), R.Hit + FVector(X, X, 0), Col * 0.45f, SDPG_World, Thick * 0.5f, -1.f, Id);
				Lines->DrawLine(R.Hit + FVector(-X, X, 0), R.Hit + FVector(X, -X, 0), Col * 0.45f, SDPG_World, Thick * 0.5f, -1.f, Id);
			}
		}
	}
}

void UExposureProbeSubsystem::DrawLink(ULineBatchComponent* Lines, const FExposureEdge& E, int32 T, int32 C, bool bFaint) const
{
	// One link between the two eyes, split in the middle: the T half shows what T sees of CT, the CT half what CT sees of T.
	// A half is thicker the larger its share and dashed when that side sees nothing.
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const FExposureProbeDesc& Tp = Solver.Probes()[T];
	const FExposureProbeDesc& Cp = Solver.Probes()[C];
	const FVector TEye = Tp.Feet + FVector(0, 0, Solver.BodyOf(T).EyeCm(Tp.bCrouch));
	const FVector CEye = Cp.Feet + FVector(0, 0, Solver.BodyOf(C).EyeCm(Cp.bCrouch));
	const FVector Mid = 0.5f * (TEye + CEye);
	const uint32 Id = ExposureProbe::LineBatchId;
	const float Base = FMath::Max(1.f, S->RayThickness);

	auto Half = [&](const FVector& From, float Share, FLinearColor Col)
	{
		if (bFaint) { Col.A = 0.35f; }
		if (Share > 0.f)
		{
			Lines->DrawLine(From, Mid, Col, SDPG_World, Base * (2.f + 4.f * Share), -1.f, Id);
			return;
		}
		Col.A *= 0.6f;
		const float Len = FVector::Dist(From, Mid);
		const int32 N = FMath::Clamp(int32(Len / 25.f), 2, 400);
		for (int32 i = 0; i < N; i += 2)
		{
			Lines->DrawLine(FMath::Lerp(From, Mid, float(i) / N), FMath::Lerp(From, Mid, float(i + 1) / N), Col, SDPG_World, Base, -1.f, Id);
		}
	};
	Half(TEye, E.ShareTtoCT, S->TColor);
	Half(CEye, E.ShareCTtoT, S->CTColor);
}

void UExposureProbeSubsystem::Redraw()
{
	UWorld* World = GetEditorWorld();
	ULineBatchComponent* Lines = World ? World->GetLineBatcher(UWorld::ELineBatcherType::WorldPersistent) : nullptr;
	if (!Lines)
	{
		return;
	}
	Lines->ClearBatch(ExposureProbe::LineBatchId);

	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	const uint32 Id = ExposureProbe::LineBatchId;
	const TMap<FString, AExposureProbeActor*> Actors = ProbeActors();
	DrawnEdges.Reset();
	BodyFills.Reset();
	if (!bShowProbes)
	{
		// With the probe layer off the bodies stay as faint outlines, so the floor layers read on their own.
		const FExposureBodyFill Empty;
		for (const TPair<FString, AExposureProbeActor*>& A : Actors) { A.Value->SetExposure(Empty.Share, Empty.Color, true, false); }
		if (GEditor) { GEditor->RedrawLevelEditingViewports(false); }
		return;
	}
	auto Shown = [&Actors](const FExposureEdge& E)
	{
		const AExposureProbeActor* const* T = Actors.Find(E.TProbe);
		const AExposureProbeActor* const* C = Actors.Find(E.CTProbe);
		return (!T || (*T)->IsLinkShown(E.CTProbe)) && (!C || (*C)->IsLinkShown(E.TProbe));
	};

	TSet<FString> Involved;
	const bool bFocus = !FocusEdge.IsEmpty();
	for (const FString& EdgeName : EdgesToDraw())
	{
		const FExposureEdge* E = Edges.FindByPredicate([&EdgeName](const FExposureEdge& X) { return X.Edge == EdgeName; });
		const int32 T = E ? Solver.FindProbe(E->TProbe) : INDEX_NONE;
		const int32 C = E ? Solver.FindProbe(E->CTProbe) : INDEX_NONE;
		if (T == INDEX_NONE || C == INDEX_NONE || (!bFocus && !Shown(*E)))
		{
			continue;
		}
		Involved.Add(E->TProbe);
		Involved.Add(E->CTProbe);
		const bool bNone = E->VisibleTtoCT == 0 && E->VisibleCTtoT == 0;
		if (DrawStyle == EExposureDrawStyle::Rays)
		{
			DrawRays(Lines, T, C);
		}
		else if (!bNone || bFocus)
		{
			DrawLink(Lines, *E, T, C, bNone);
		}
		if (!bNone || bFocus)
		{
			DrawnEdges.Add(E->Edge);
		}

		// Each body is filled in the colour of the side that sees it, by the largest share over the drawn pairs.
		auto Accumulate = [this](const FString& Seen, const TArray<int32>& Vis, const TArray<int32>& Valid, const FLinearColor& By)
		{
			FExposureBodyFill& F = BodyFills.FindOrAdd(Seen);
			F.Color = By;
			for (int32 r = 0; r < 3 && Vis.Num() == 3 && Valid.Num() == 3; ++r)
			{
				if (Valid[r] > 0) { F.Share[r] = FMath::Max(F.Share[r], float(Vis[r]) / Valid[r]); }
			}
		};
		Accumulate(E->CTProbe, E->RegionVisibleTtoCT, E->RegionValidCT, S->TColor);
		Accumulate(E->TProbe, E->RegionVisibleCTtoT, E->RegionValidT, S->CTColor);
	}
	bRegionLabels = bFocus || (SelectedProbes.Num() == 1 && DrawnEdges.Num() <= 8);

	const bool bNarrowed = bFocus || SelectedProbes.Num() > 0;
	const FExposureBodyFill Empty;
	for (const TPair<FString, AExposureProbeActor*>& A : Actors)
	{
		const FExposureBodyFill* F = BodyFills.Find(A.Key);
		if (!F) { F = &Empty; }
		const bool bFaded = bNarrowed && !Involved.Contains(A.Key);
		A.Value->SetExposure(F->Share, F->Color, bFaded, DrawStyle == EExposureDrawStyle::Rays && Involved.Contains(A.Key));
	}

	// The last edit: its geometry, and a connector from it to every pair it changed.
	// Pairs farther than Far Distance get a dashed connector, so an edit reaching across the map is visible at a glance.
	if (DrawMode != EExposureDrawMode::Off && !bNarrowed && ActiveEditBoxes().Num() > 0)
	{
		const FLinearColor Mark(0.92f, 0.92f, 0.92f);
		FBox Edit(ForceInit);
		for (const FBox& B : ActiveEditBoxes())
		{
			Edit += B;
			Lines->DrawBox(B.GetCenter(), B.GetExtent(), Mark, -1.f, SDPG_World, 2.f, Id);
		}
		for (const FExposureEdgeChange& Ch : ActiveChanges())
		{
			const int32 T = Solver.FindProbe(Ch.After.TProbe), C = Solver.FindProbe(Ch.After.CTProbe);
			// Pairs with a moved or new probe did not change because of the geometry.
			if (T == INDEX_NONE || C == INDEX_NONE || Ch.Kind == EExposureChangeKind::ProbeMoved || Ch.Kind == EExposureChangeKind::NewPair)
			{
				continue;
			}
			const FExposureProbeDesc& Tp = Solver.Probes()[T];
			const FExposureProbeDesc& Cp = Solver.Probes()[C];
			const FVector Mid = 0.5f * (Tp.Feet + FVector(0, 0, Solver.BodyOf(T).EyeCm(Tp.bCrouch)) + Cp.Feet + FVector(0, 0, Solver.BodyOf(C).EyeCm(Cp.bCrouch)));
			const FVector From = Edit.GetClosestPointTo(Mid);
			FLinearColor Col = Ch.bNewOneWay ? UExposureProbeSettings::AlertColor() : Mark;
			if (!Ch.bFar)
			{
				Col.A = 0.55f;
				Lines->DrawLine(From, Mid, Col, SDPG_World, 1.f, -1.f, Id);
				continue;
			}
			const float Len = FVector::Dist(From, Mid);
			const int32 N = FMath::Clamp(int32(Len / 40.f), 2, 600);
			for (int32 i = 0; i < N; i += 2)
			{
				Lines->DrawLine(FMath::Lerp(From, Mid, float(i) / N), FMath::Lerp(From, Mid, float(i + 1) / N), Col, SDPG_World, 2.f, -1.f, Id);
			}
			Lines->DrawPoint(Mid, Col, 10.f, SDPG_World, -1.f, Id);
		}
	}
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports(false);
	}
}

void UExposureProbeSubsystem::DrawLabels(UCanvas* Canvas, APlayerController* PC)
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	if (!Canvas || !Canvas->SceneView || (GEditor && GEditor->PlayWorld) || !FSlateApplication::IsInitialized())
	{
		return;
	}
	if (bShowProbes && bBuilt && S->bShowLabels)
	{
		DrawProbeLabels(Canvas);
	}
	if ((bShowHeat || bShowOneWay) && HasHeat())
	{
		DrawHeatLabels(Canvas);
	}
	if (bHasSmoke)
	{
		DrawSmokeLabels(Canvas);
	}
}

void UExposureProbeSubsystem::DrawProbeLabels(UCanvas* Canvas)
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	// Labels are dark plates with neutral text; a colour chip on the left carries the side (or the alert).
	const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle("Bold", 10);
	const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 8);
	const FSlateFontInfo NameFont = FCoreStyle::GetDefaultFontStyle("Bold", 9);
	const FLinearColor Ink(0.94f, 0.94f, 0.94f);
	const FLinearColor Muted(0.66f, 0.66f, 0.66f);
	const FLinearColor PlateCol(0.03f, 0.03f, 0.035f, 0.78f);
	const FLinearColor Alert = UExposureProbeSettings::AlertColor();
	const float Dpi = Canvas->GetDPIScale();
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

	auto Project = [&](const FVector& World, FVector2D& Out)
	{
		const FVector P = Canvas->Project(World, false);
		Out = FVector2D(P.X, P.Y);
		return P.Z > 0.f && P.X >= 0 && P.Y >= 0 && P.X <= Canvas->ClipX && P.Y <= Canvas->ClipY;
	};
	auto Tile = [&](const FVector2D& Min, const FVector2D& Size, const FLinearColor& Col)
	{
		FCanvasTileItem Item(Min * Dpi, Size * Dpi, Col);
		Item.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
	};
	auto Text = [&](const FVector2D& At, const FString& Str, const FSlateFontInfo& F, const FLinearColor& Col)
	{
		FCanvasTextItem Item(At, FText::FromString(Str), F, Col);
		// Slate style fonts carry no UFont, and a canvas text item without one draws nothing; the engine font
		// supplies the font cache while the Slate font info (with CJK fallback) still decides the glyphs.
		Item.Font = GEngine->GetSmallFont();
		Item.EnableShadow(FLinearColor(0, 0, 0, 0.6f));
		Canvas->DrawItem(Item);
	};
	auto Width = [&](const FString& Str, const FSlateFontInfo& F) { return FVector2D(Measure->Measure(Str, F)).X; };
	auto Percent = [](float V) { return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(V * 100.f)); };

	// Card centred on a point: chip, a title line and an optional second line.
	auto Card = [&](const FVector2D& Centre, const FLinearColor& Chip, const FString& Line1, const FSlateFontInfo& F1, const FString& Line2)
	{
		const FVector2D S1(Measure->Measure(Line1, F1));
		const FVector2D S2 = Line2.IsEmpty() ? FVector2D::ZeroVector : FVector2D(Measure->Measure(Line2, Body));
		const float Pad = 6.f, ChipW = 3.f, H1 = S1.Y, H2 = S2.Y;
		const float W = FMath::Max(S1.X, S2.X) + 2.f * Pad + ChipW;
		const float H = H1 + H2 + 2.f * 3.f;
		const FVector2D Min(Centre.X - 0.5f * W, Centre.Y - 0.5f * H);
		Tile(Min, FVector2D(W, H), PlateCol);
		Tile(Min, FVector2D(ChipW, H), Chip);
		Text(Min + FVector2D(ChipW + Pad, 3.f), Line1, F1, Ink);
		if (!Line2.IsEmpty())
		{
			Text(Min + FVector2D(ChipW + Pad, 3.f + H1), Line2, Body, Muted);
		}
	};

	const bool bNarrowed = !FocusEdge.IsEmpty() || SelectedProbes.Num() > 0;
	for (int32 i = 0; i < Solver.Probes().Num(); ++i)
	{
		const FExposureProbeDesc& D = Solver.Probes()[i];
		const FExposureBodyModel& M = Solver.BodyOf(i);
		if (bNarrowed && !BodyFills.Contains(D.Name))
		{
			continue;
		}
		FVector2D P;
		if (Project(D.Feet + FVector(0, 0, M.TopUnits(D.bCrouch) * M.UnitToCm + 10.f), P))
		{
			Card(P - FVector2D(0.f, 12.f), D.Team == EExposureTeam::TSide ? S->TColor : S->CTColor, D.Name, NameFont, FString());
		}
	}

	// Pair card at the middle of the link: A first, then both directed shares. With many pairs only one-way pairs get a card.
	const bool bFew = DrawnEdges.Num() <= 40;
	for (const FString& EdgeName : DrawnEdges)
	{
		const FExposureEdge* E = Edges.FindByPredicate([&EdgeName](const FExposureEdge& X) { return X.Edge == EdgeName; });
		const int32 T = E ? Solver.FindProbe(E->TProbe) : INDEX_NONE;
		const int32 C = E ? Solver.FindProbe(E->CTProbe) : INDEX_NONE;
		if (T == INDEX_NONE || C == INDEX_NONE || (!bFew && !E->bOneWay))
		{
			continue;
		}
		const FExposureProbeDesc& Tp = Solver.Probes()[T];
		const FExposureProbeDesc& Cp = Solver.Probes()[C];
		const FVector Mid = 0.5f * (Tp.Feet + FVector(0, 0, Solver.BodyOf(T).EyeCm(Tp.bCrouch)) + Cp.Feet + FVector(0, 0, Solver.BodyOf(C).EyeCm(Cp.bCrouch)));
		FVector2D P;
		if (!Project(Mid, P))
		{
			continue;
		}
		const bool bEven = FMath::Abs(E->A) < 0.005f;
		FLinearColor Chip = bEven ? Muted : (E->A > 0 ? S->TColor : S->CTColor);
		const FExposureEdgeChange* Ch = ActiveChanges().FindByPredicate([&EdgeName](const FExposureEdgeChange& X) { return X.Edge == EdgeName; });
		FString Head = Ch && Ch->bHadBefore && FocusEdge.IsEmpty()
			? FString::Printf(TEXT("A %+.2f \u2192 %+.2f"), Ch->Before.A, E->A)
			: FString::Printf(TEXT("A %+.2f"), E->A);
		if (E->bOneWay)
		{
			Head += E->bDesignIntent ? EPS("   单向 · 保留", "   one-way · intent") : EPS("   单向", "   one-way");
			if (!E->bDesignIntent) { Chip = Alert; }
		}
		FString Sub = FString::Printf(TEXT("T %s   CT %s"), *Percent(E->ShareTtoCT), *Percent(E->ShareCTtoT));
		if (Ch && Ch->bFar)
		{
			Sub += FString::Format(*EPS("   距改动 {0}", "   {0} from edit"), { FString::Printf(TEXT("%.1f m"), Ch->DistanceCm / 100.f) });
		}
		if (Ch && Ch->Kind != EExposureChangeKind::Minor && Ch->Kind != EExposureChangeKind::Shifted && Ch->Kind != EExposureChangeKind::NewOneWay)
		{
			Sub += TEXT("   ") + ChangeKindText(Ch->Kind);
		}
		Card(P, Chip, Head, Title, Sub);
	}

	// The last edit: what it changed and how far that reaches.
	if (DrawMode != EExposureDrawMode::Off && !bNarrowed && ActiveEditBoxes().Num() > 0 && ActiveChanges().Num() > 0)
	{
		FBox Edit(ForceInit);
		for (const FBox& B : ActiveEditBoxes()) { Edit += B; }
		FVector2D P;
		if (Project(FVector(Edit.GetCenter().X, Edit.GetCenter().Y, Edit.Max.Z + 20.f), P))
		{
			int32 Far = 0, NewOneWay = 0;
			float Farthest = 0.f;
			for (const FExposureEdgeChange& C : ActiveChanges())
			{
				Far += C.bFar ? 1 : 0;
				NewOneWay += C.bNewOneWay ? 1 : 0;
				Farthest = FMath::Max(Farthest, C.DistanceCm);
			}
			const FString Line1 = bHasBaseline
				? FString::Format(*EPS("相对「{1}」改动影响 {0} 对", "{0} pairs changed since \"{1}\""), { ActiveChanges().Num(), BaselineDisplayName(Baseline.Name) })
				: FString::Format(*EPS("改动影响 {0} 对", "Edit affects {0} pairs"), { ActiveChanges().Num() });
			const FString Line2 = Far > 0
				? FString::Format(*EPS("{0} 对在 {1} 以外，最远 {2}", "{0} beyond {1}, farthest {2}"),
					{ Far, FString::Printf(TEXT("%.0f m"), S->FarDistance * S->UnitToCm / 100.f), FString::Printf(TEXT("%.1f m"), Farthest / 100.f) })
				: EPS("都在附近", "all nearby");
			Card(P - FVector2D(0.f, 16.f), NewOneWay > 0 ? Alert : FLinearColor(0.92f, 0.92f, 0.92f), Line1, Title, Line2);
		}
	}

	// Focused pair or a single selected probe: visible share beside each body region.
	if (bRegionLabels)
	{
		for (int32 i = 0; i < Solver.Probes().Num(); ++i)
		{
			const FExposureProbeDesc& D = Solver.Probes()[i];
			const FExposureBodyFill* F = BodyFills.Find(D.Name);
			if (!F)
			{
				continue;
			}
			const FExposureBodyModel& M = Solver.BodyOf(i);
			const float Z[4] = { 0.f, M.CrotchUnits(D.bCrouch), M.NeckUnits(D.bCrouch), M.TopUnits(D.bCrouch) };
			const float R = M.Radius * M.UnitToCm;
			for (int32 r = 0; r < 3; ++r)
			{
				FVector2D P, Axis;
				const FVector Centre = D.Feet + FVector(0, 0, 0.5f * (Z[r] + Z[r + 1]) * M.UnitToCm);
				if (F->Share[r] < 0.f || !Project(Centre, Axis))
				{
					continue;
				}
				// Beside the body, past its projected radius.
				const FVector Right = Canvas->SceneView->ViewMatrices.GetViewToWorld().GetUnitAxis(EAxis::Y);
				Project(Centre + Right * R * 1.4f, P);
				const FString Str = Percent(F->Share[r]);
				const float W = Width(Str, Body);
				const float H = FVector2D(Measure->Measure(Str, Body)).Y + 2.f;
				Tile(FVector2D(P.X, P.Y - 0.5f * H), FVector2D(W + 9.f, H), PlateCol);
				Tile(FVector2D(P.X, P.Y - 0.5f * H), FVector2D(2.f, H), F->Share[r] > 0.f ? F->Color : Muted);
				Text(FVector2D(P.X + 5.f, P.Y - 0.5f * H + 1.f), Str, Body, F->Share[r] > 0.f ? Ink : Muted);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
