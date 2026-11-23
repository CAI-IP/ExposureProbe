#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Tickable.h"
#include "TickableEditorObject.h"
#include "ExposureProbeSolver.h"
#include "ExposureProbeTypes.h"
#include "UObject/ObjectSaveContext.h"
#include "ExposureProbeSubsystem.generated.h"

class AActor;
class AExposureProbeActor;
class AExposureSmokeActor;
class UCanvas;
class APlayerController;
class UExposureHeatComponent;
class UExposureSmokeComponent;
class ULocalFogVolumeComponent;

UENUM()
enum class EExposureDrawMode : uint8
{
	Off,
	/** Rays of the selected probe or pair, otherwise of the pairs changed by the last update. */
	Changed,
	All
};

UENUM()
enum class EExposureDrawStyle : uint8
{
	/** One link per pair and the body regions the other side sees, filled by visible share. */
	Body,
	/** Every ray from the eye to the body sample points. */
	Rays
};

/** Viewport layers; any combination can be on. */
UENUM()
enum class EExposureLayer : uint8
{
	/** Probe pairs: links, body fills and labels. */
	Probes,
	/** Full-map A on the floor. */
	Heatmap,
	/** Floor cells from which a spot can be peeked one-way. */
	OneWay
};

UENUM()
enum class EExposureHeatShow : uint8
{
	/** A at each sample. */
	A,
	/** A minus the active baseline's A at the same sample. */
	Delta
};

/** Which floor cells the legend isolates; None shows every cell of the layers that are on. */
UENUM()
enum class EExposureFloorFilter : uint8
{
	None,
	/** Heatmap: clearly seen more than it sees (red). */
	SeenMore,
	/** Heatmap: clearly sees more than it is seen (blue). */
	SeesMore,
	/** One-way: cells that peek 1, 2-3, 4-7 or 8+ spots one-way. */
	Peek1,
	Peek2,
	Peek4,
	Peek8,
	/** One-way: peek cells that are new since the baseline. */
	NewSpot,
	/** One-way: cells someone peeks one-way. */
	PeekedBy,
	/** One-way with probes selected: cells that peek the selection one-way. */
	Threat,
	/** One-way with probes selected: cells the selection peeks one-way. */
	Advantage
};

/** Largest visible share of each body region (legs, torso, head) over the drawn partners. */
struct FExposureBodyFill
{
	float Share[3] = { -1.f, -1.f, -1.f };
	FLinearColor Color = FLinearColor::White;
};

/** Collision geometry of one actor as seen by the last update. */
struct FExposureGeomState
{
	FBox Box = FBox(ForceInit);
	/** Component transforms, bounds and mesh paths, so edits that keep the bounds are still caught. */
	uint32 Hash = 0;
	FString Label;
};

/** Fewest smokes that cut a probe's sightlines to the other side (CC) and the time to see the same targets again (RT). */
struct FExposureSmokeResult
{
	struct FSmoke
	{
		FVector Landing = FVector::ZeroVector;
		FVector Centre = FVector::ZeroVector;
		float RadiusCm = 0.f;
		float Duration = 0.f;
		/** Placed smoke: its actor label. */
		FString Name;
		/** Sightlines this smoke cuts. */
		int32 Hits = 0;
		/** Sightlines this smoke cuts that the earlier ones left open. */
		int32 NewlyCut = 0;
	};
	FString Observer;
	/** The smokes are the ones placed in the level (evaluated), not a plan the plugin chose. */
	bool bPlaced = false;
	EExposureTeam Team = EExposureTeam::TSide;
	FVector Feet = FVector::ZeroVector;
	FVector Eye = FVector::ZeroVector;
	float RadiusCm = 0.f;
	float Tau = 0.8f;
	float WaitSeconds = 0.f;
	int32 Opponents = 0;
	int32 OpponentsSeen = 0;
	int32 TargetPoints = 0;
	/** Opposing body points the observer sees without smoke. */
	TArray<FVector> Ends;
	TArray<bool> Cut;
	int32 Candidates = 0;
	TArray<FSmoke> Smokes;
	float Coverage = 0.f;
	/** The smokes reach the coverage threshold; otherwise CC is right-censored. */
	bool bReached = false;
	bool bHaveA = false;
	float AObserver = 0.f;
	bool bNavPath = false;
	int32 DetourTested = 0;
	bool bFoundAlt = false;
	FVector AltFeet = FVector::ZeroVector;
	FVector AltEye = FVector::ZeroVector;
	float AltShare = 0.f;
	float AltA = 0.f;
	float DetourUnits = 0.f;
	float DetourSeconds = 0.f;
	TArray<FVector> Path;
	float RT = 0.f;
	/** No detour spot sees the targets again: RT is the time until the smokes clear. */
	bool bStructural = false;
	/** The smokes leave the recover share of the sightlines open: nothing to recover, RT is 0. */
	bool bStillSees = false;
	/** No floor analysis to search detour spots on: RT was not computed. */
	bool bRecoverySkipped = false;
	double Ms = 0.0;
};

DECLARE_MULTICAST_DELEGATE(FOnExposureReportUpdated);

/**
 * Online paired-probe audit in the level editor. After an edit it diffs actor collision geometry
 * against the previous update, recasts only the rays whose segments cross the old or new bounds of
 * the changed actors, and reports which probe pairs changed, which became one-way, and how far from
 * the edit they are. Moving, adding or retyping a probe rebuilds the ray table.
 * It also analyses the floor (full-map A and one-way spots) and compares the level against saved baselines.
 */
UCLASS()
class EXPOSUREPROBEEDITOR_API UExposureProbeSubsystem : public UEditorSubsystem, public FTickableEditorObject
{
	GENERATED_BODY()

public:
	static UExposureProbeSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bInitialized; }

	/** Collect probes and cast every ray. */
	const FExposureAuditReport& Rebuild(bool bParallel);
	/** Diff actor geometry now and run a local update, ignoring debounce and mouse state. */
	const FExposureAuditReport& UpdateNow();

	bool MarkDesignIntent(const FString& Edge, const FString& Note, FString& OutError);
	bool ClearDesignIntent(const FString& Edge, FString& OutError);

	const FExposureAuditReport& GetLastReport() const { return LastReport; }
	const TArray<FExposureEdge>& GetEdges() const { return Edges; }
	const FExposureProbeSolver& GetSolver() const { return Solver; }
	bool IsBuilt() const { return bBuilt; }
	bool HasPendingUpdate() const { return bPendingScan || bProbesDirty; }

	bool IsAutoUpdate() const { return bAutoUpdate; }
	void SetAutoUpdate(bool bEnabled) { bAutoUpdate = bEnabled; }

	EExposureDrawMode GetDrawMode() const { return DrawMode; }
	void SetDrawMode(EExposureDrawMode Mode);
	EExposureDrawStyle GetDrawStyle() const { return DrawStyle; }
	void SetDrawStyle(EExposureDrawStyle Style);
	/** Redraw links and bodies, e.g. after a probe's hidden links changed. */
	void RefreshView() { Redraw(); }
	const FString& GetFocusEdge() const { return FocusEdge; }
	void SetFocusEdge(const FString& Edge);
	void FocusViewportOnEdge(const FString& Edge) const;

	bool IsLayerOn(EExposureLayer Layer) const;
	void SetLayer(EExposureLayer Layer, bool bOn);

	/** Sample the walkable floor and cast the full-map A (and the one-way spots) behind a cancellable progress dialog. */
	bool ComputeHeatmap(FString& OutMessage);
	void ClearHeatmap();
	const TArray<FExposureHeatPoint>& GetHeat() const { return Heat; }
	bool HasHeat() const { return Heat.Num() > 0; }
	/** The level or the body changed since the floor was analysed. */
	bool IsHeatStale() const;
	float GetHeatSpacingCm() const { return HeatSpacingCm; }
	float GetHeatRadiusCm() const { return HeatRadiusCm; }
	double GetHeatSeconds() const { return HeatSeconds; }
	int64 GetHeatRays() const { return HeatRays; }
	const FString& GetHeatTime() const { return HeatTime; }
	EExposureHeatShow GetHeatShow() const { return HeatShow; }
	void SetHeatShow(EExposureHeatShow Show);
	/** The active baseline has a heatmap on the same grid. */
	bool CanShowHeatDelta() const;
	/** The value shown at a sample (A, or A minus the baseline); false when the baseline has no sample there. */
	bool HeatValue(int32 Index, float& OutValue) const;
	/** Viewport looks down at a floor sample, which is ringed until the next one. */
	void FocusViewportOnHeat(int32 Index, EExposureLayer Layer);

	/** |value| shown at full colour: the setting, or the 90th percentile of |A| (or |ΔA|) when the setting is 0. */
	float GetHeatScale() const { return HeatScale; }
	EExposureFloorFilter GetFloorFilter() const { return FloorFilter; }
	void SetFloorFilter(EExposureFloorFilter Filter);
	/** Move the viewport to the next cell of the active legend row. */
	void StepFloorFilter();
	/** The sample belongs to the legend row of Filter. */
	bool CellMatches(int32 Index, EExposureFloorFilter Filter) const;
	int32 FloorFilterCount(EExposureFloorFilter Filter) const;
	/** Cells of a legend row, strongest first and spread apart, for stepping the viewport through them. */
	TArray<int32> FloorFilterSpots(EExposureFloorFilter Filter) const;
	/** 0 none, 1 for 1 spot, 2 for 2-3, 3 for 4-7, 4 for 8 or more spots seen one-way. */
	int32 OneWayLevel(int32 Index) const;

	/** Samples this one sees one-way. */
	const TArray<int32>& GetOneWayTargets(int32 Index) const;
	int32 GetOneWaySpotCount() const;
	/** The spot sees someone one-way now and saw no one one-way in the baseline. */
	bool IsNewOneWaySpot(int32 Index) const;
	/** One-way relations of floor cells with the selected probes (empty when none is selected). */
	bool HasProbeOneWay() const { return ProbeOneWayNames.Num() > 0; }
	const TArray<FString>& GetProbeOneWayNames() const { return ProbeOneWayNames; }
	/** Selected probes this cell can peek one-way (indices into GetProbeOneWayNames). */
	const TArray<int32>& GetCellThreatens(int32 Index) const;
	/** Selected probes that peek this cell one-way. */
	const TArray<int32>& GetCellExposedTo(int32 Index) const;
	int32 GetThreatCellCount() const;
	int32 GetAdvantageCellCount() const;

	/**
	 * Smokes for one probe (the first selected when Observer is empty): candidate landing points on the analysed floor,
	 * greedy cover of the sightlines it sees to the other side (CC), and the detour that sees them again (RT).
	 * The smokes are shown as drifting puffs, with a local fog volume inside each.
	 */
	bool ComputeSmoke(const FString& Observer, FString& OutMessage);
	void ClearSmoke();
	bool HasSmoke() const { return bHasSmoke; }
	bool IsSmokeStale() const { return bHasSmoke && bSmokeStale; }
	const FExposureSmokeResult& GetSmoke() const { return Smoke; }
	/** CC, RT and how they were found, in a few lines. */
	FString SmokeSummary() const;

	/**
	 * Smokes placed in the level, against one probe's sightlines to the other side: the share they cut,
	 * what each one adds, and the detour that sees the targets again (RT). Each smoke uses its own size.
	 */
	bool EvaluatePlacedSmokes(const FString& Observer, FString& OutMessage);
	/** New smoke where the centre of the active viewport looks, snapped to the nearest floor cell. */
	AExposureSmokeActor* PlaceSmokeAtView(FString& OutMessage);
	/** New smoke at a point, snapped to the nearest floor cell (or the floor below it). */
	AExposureSmokeActor* PlaceSmokeAt(const FVector& Point, FString& OutMessage);
	/** Turn the smokes of the plan on show into placed smokes, then evaluate them. Returns how many were placed. */
	int32 PlacePlanSmokes(FString& OutMessage);
	/** Delete every placed smoke of the level. */
	int32 ClearPlacedSmokes();
	TArray<AExposureSmokeActor*> GetPlacedSmokes(bool bEnabledOnly) const;
	int32 GetPlacedSmokeCount() const { return PlacedSmokeCount; }
	/** After the smoke size or style changed: resize the placed smokes, redraw, and evaluate the placed smokes again. */
	void RefreshSmokes();
	/** Landing point a smoke at Point snaps to: the nearest floor cell, else the floor below. */
	FVector SmokeSnapPoint(const FVector& Point, bool& bOutCell) const;

	/** Baselines saved for the current level, newest first. */
	TArray<FString> ListBaselines() const;
	/** Save the current pairs, probes, settings, geometry fingerprint and an up-to-date heatmap under a name. */
	bool SaveBaseline(const FString& Name, FString& OutError);
	bool DeleteBaseline(const FString& Name);
	/** Compare against a saved baseline; an empty name stops comparing. */
	bool SetActiveBaseline(const FString& Name, FString& OutError);
	bool HasBaseline() const { return bHasBaseline; }
	const FExposureBaseline& GetBaseline() const { return Baseline; }
	/** Pairs whose counts differ from the baseline, new one-way first. */
	const TArray<FExposureEdgeChange>& GetBaselineChanges() const { return BaselineChanges; }
	/** Pairs one-way in the baseline and no longer one-way. */
	int32 GetBaselineResolved() const { return BaselineResolved; }
	/** Pairs of the baseline whose probes are gone. */
	int32 GetBaselineMissing() const { return BaselineMissing; }
	/** Collision actors added, changed or deleted since the baseline. */
	const TArray<FString>& GetBaselineGeomChanges() const { return BaselineGeomChanges; }
	/** Probes added, removed or moved since the baseline. */
	const TArray<FString>& GetBaselineProbeChanges() const { return BaselineProbeChanges; }
	/** Same body, sampling and tracing as the baseline, so the counts compare directly. */
	bool IsBaselineComparable() const { return !bHasBaseline || Baseline.ModelKey == ModelKey; }
	/** Same settings, probes and geometry as the baseline. */
	bool BaselineInputsMatch() const { return bBaselineInputsMatch; }
	/** Inputs identical to the baseline but results differ: the run did not reproduce. */
	bool IsBaselineReproMismatch() const { return bHasBaseline && bBaselineInputsMatch && BaselineChanges.Num() > 0; }
	/** What differs between the baseline's inputs and the current ones. */
	const FString& GetBaselineInputDiff() const { return BaselineInputDiff; }
	FString BaselineDir() const;
	/** Name of the baseline saved with every level save. */
	static FString LastSaveBaselineName();
	/** The baseline saved by the level save before the last one. */
	static FString PreviousSaveBaselineName();
	/** Name shown for a baseline; the automatic ones are translated. */
	static FString BaselineDisplayName(const FString& Name);

	/** Write the comparison against the active baseline (or the last edit) as Markdown and JSON. */
	bool ExportReport(FString& OutPath, FString& OutError) const;

	static EExposureChangeKind ClassifyChange(const FExposureEdgeChange& Change, float Threshold);
	static FString ChangeKindText(EExposureChangeKind Kind);

	static FString LogDir();
	UWorld* GetEditorWorld() const;

	FOnExposureReportUpdated OnReportUpdated;

private:
	void HandleActorMoved(AActor* Actor);
	void HandleActorAdded(AActor* Actor);
	void HandleActorDeleted(AActor* Actor);
	void HandleObjectPropertyChanged(UObject* Object, struct FPropertyChangedEvent& Event);
	void HandleUndoRedo();
	void HandleMapChange(uint32 Flags);
	void HandleSelectionChanged(UObject* Object);
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void HandlePostSaveWorld(UWorld* World, FObjectPostSaveContext Context);
	void MarkPending(const AActor* Actor);
	bool IsEditorActor(const AActor* Actor) const;

	static bool ComputeGeom(const AActor* Actor, ECollisionChannel Channel, FExposureGeomState& Out);
	void CacheAllBounds();
	void CollectBoundsChanges(TArray<FBox>& OutBoxes, TArray<FString>& OutActors);
	void ResetState();

	TArray<FExposureProbeDesc> CollectProbes(const FExposureBodyModel& Model, TArray<FString>& OutWarnings) const;
	AExposureProbeActor* FindProbeActor(const FString& Name) const;
	void ApplyDesignIntent(TArray<FExposureEdge>& InOutEdges) const;
	void RefreshDesignIntent();
	void Summarize(FExposureAuditReport& R, bool bInitial) const;

	const FExposureAuditReport& RunUpdate(bool bForceFull, bool bParallel, bool bQuiet);
	void WriteLog(const FExposureAuditReport& Report) const;
	void WriteEvent(const FString& Event, const FString& Edge, const FString& Note) const;
	void Notify(const FExposureAuditReport& Report) const;
	void Redraw();
	void DrawLabels(UCanvas* Canvas, APlayerController* PC);
	void DrawProbeLabels(UCanvas* Canvas);
	void DrawHeatLabels(UCanvas* Canvas);
	TArray<FString> EdgesToDraw() const;
	/** Changes shown in the viewport: against the active baseline, otherwise from the last edit. */
	const TArray<FExposureEdgeChange>& ActiveChanges() const { return bHasBaseline ? BaselineChanges : LastReport.Changes; }
	/** Geometry behind ActiveChanges. */
	const TArray<FBox>& ActiveEditBoxes() const { return bHasBaseline ? BaselineEditBoxes : LastEditBoxes; }
	TMap<FString, AExposureProbeActor*> ProbeActors() const;
	void DrawRays(class ULineBatchComponent* Lines, int32 T, int32 C) const;
	void DrawLink(class ULineBatchComponent* Lines, const FExposureEdge& E, int32 T, int32 C, bool bFaint) const;

	/** Floor samples on a grid; InOutSpacingCm is widened when the level would need too many. */
	bool BuildHeatSamples(const FExposureBodyModel& Model, float& InOutSpacingCm, TArray<FExposureProbeSolver::FPoint>& Out, FVector2D& OutOrigin, FString& OutWhy) const;
	/** Line from an eye to the first body point of the sample it sees, for the hover lines. */
	bool VisiblePoint(const FVector& Eye, const TArray<FVector>& Body, const TArray<bool>* Valid, const FExposureBodyModel& Model, FVector& Out) const;
	void RedrawHeat();
	void RedrawHover();
	void UpdateBaselineDiff();
	int32 NearestHeat(const FVector& Point) const;
	/** Body, sampling, tracing and floor settings the floor analysis depends on. */
	FString CurrentHeatKey() const;
	void DestroyHeatComponent();
	/** Legend overlay in the active level viewport while a floor layer is on. */
	void UpdateLegendOverlay();
	void RemoveLegendOverlay();
	TSharedRef<class SWidget> BuildLegend();
	/** One-way relations between floor cells and the selected probes; cached by selection, positions and edit time. */
	void UpdateProbeOneWay();
	void RedrawSmoke();
	void DestroySmokeComponents();
	void DrawSmokeLabels(UCanvas* Canvas);
	/** Observer, the opposing body points it sees, and an up-to-date floor analysis (run when bAllowFloor). */
	bool PrepareSmoke(const FString& Observer, bool bAllowFloor, FExposureSmokeResult& R, int32& OutProbe, FString& OutMessage);
	bool EvaluatePlaced(const FString& Observer, bool bAllowFloor, FString& OutMessage);
	/** Wait the smokes out, or walk to a floor cell that sees the targets again past them. */
	void SolveRecovery(FExposureSmokeResult& R, int32 Probe);
	void FinishSmoke(FExposureSmokeResult&& R, FString& OutMessage);
	void HandleSmokeEdit(const AExposureSmokeActor* Actor);
	/** Enabled placed smokes as occluders of the probe rays (empty when the setting is off). OutKey changes with any smoke or the setting. */
	TArray<FExposureProbeSolver::FSmokeSphere> ProbeSmokes(FString* OutKey = nullptr, TMap<FString, FBox>* OutBoxes = nullptr, TMap<FString, FString>* OutLabels = nullptr) const;
	/** Smokes moved: re-apply them to the cached rays and redraw, without casting. The change report follows once the edit is over. */
	void LiveSmokeUpdate(const TArray<FExposureProbeSolver::FSmokeSphere>& Smokes, const FString& Key);
	/** Cut share of the evaluated sightlines with the smokes where they are now; RT follows once the edit is over. */
	void UpdateSmokeCutsLive();
	void SnapPendingSmokes();
	FString NextSmokeLabel() const;

	void SnapshotGeometry(TArray<FExposureGeomRecord>& Out) const;
	void SnapshotProbes(TArray<FExposureProbeRecord>& Out) const;
	static FString SettingsText();

	FExposureProbeSolver Solver;
	FString ModelKey;
	TArray<FExposureEdge> Edges;
	FExposureAuditReport LastReport;
	bool bLastInitial = true;
	bool bBuilt = false;
	/** Boxes of the last edit. */
	TArray<FBox> LastEditBoxes;
	/** Rays whose visibility the last edit changed. */
	TArray<int32> LastFlippedRays;

	TMap<TWeakObjectPtr<AActor>, FExposureGeomState> GeomCache;
	bool bPendingScan = false;
	bool bProbesDirty = false;
	double LastEditTime = 0.0;

	bool bInitialized = false;
	bool bAutoUpdate = true;
	bool bNeedsInitialBuild = true;
	EExposureDrawMode DrawMode = EExposureDrawMode::Changed;
	EExposureDrawStyle DrawStyle = EExposureDrawStyle::Body;
	bool bShowProbes = true;
	bool bShowHeat = false;
	bool bShowOneWay = false;
	FString FocusEdge;
	/** Body fills and labelled pairs of the last redraw, for the canvas labels. */
	TMap<FString, FExposureBodyFill> BodyFills;
	TArray<FString> DrawnEdges;
	bool bRegionLabels = false;
	TArray<FString> SelectedProbes;

	TArray<FExposureHeatPoint> Heat;
	float HeatSpacingCm = 0.f;
	/** Corner of the sample grid: sample (i, j) is centred at Origin + (i + 0.5, j + 0.5) * spacing. */
	FVector2D HeatOrigin = FVector2D::ZeroVector;
	float HeatRadiusCm = 0.f;
	double HeatSeconds = 0.0;
	int64 HeatRays = 0;
	FString HeatTime;
	bool bHeatStale = false;
	FString HeatKey;
	UPROPERTY(Transient)
	TObjectPtr<UExposureHeatComponent> HeatComp;
	EExposureHeatShow HeatShow = EExposureHeatShow::A;
	EExposureFloorFilter FloorFilter = EExposureFloorFilter::None;
	float HeatScale = 0.1f;
	void UpdateHeatScale();
	/** Cells per legend row, by EExposureFloorFilter value; refreshed with every floor redraw. */
	TArray<int32> FilterCounts;
	TSharedPtr<class SWidget> LegendWidget;
	TWeakPtr<class SLevelViewport> LegendViewport;
	/** Layers, display and selection the legend was built for; it is rebuilt when they change. */
	FString LegendKey;
	int32 LegendStep = 0;
	int32 HighlightHeat = INDEX_NONE;
	EExposureLayer HighlightLayer = EExposureLayer::Heatmap;
	/** Heat samples by grid cell of the heat spacing, for nearest-sample lookups. */
	TMap<FIntVector, TArray<int32>> HeatGrid;
	/** Mouse position in the active viewport, to redraw the readout when it moves. */
	FIntPoint LastMouse = FIntPoint(-1, -1);
	/** Sample under the mouse, found while drawing the labels, and the one the hover lines were drawn for. */
	int32 HoverHeat = INDEX_NONE;
	int32 DrawnHover = INDEX_NONE;

	/** Floor sample bodies kept after the analysis, for the probe one-way relations. */
	FExposureBodyModel HeatModel;
	TArray<TArray<FVector>> HeatBodies;
	TArray<TArray<bool>> HeatValid;
	TArray<FVector> HeatEyes;
	TArray<TArray<int32>> OneWayTo;
	FString ProbeOneWayKey;
	TArray<FString> ProbeOneWayNames;
	TArray<EExposureTeam> ProbeOneWayTeams;
	TArray<TArray<int32>> CellThreatens;
	TArray<TArray<int32>> CellExposedTo;

	FExposureSmokeResult Smoke;
	bool bHasSmoke = false;
	bool bSmokeStale = false;
	UPROPERTY(Transient)
	TObjectPtr<UExposureSmokeComponent> SmokeComp;
	UPROPERTY(Transient)
	TArray<TObjectPtr<ULocalFogVolumeComponent>> SmokeFog;
	/** Placed smokes moved or added since the last snap. */
	TArray<TWeakObjectPtr<AExposureSmokeActor>> PendingSnap;
	bool bSmokeDirty = true;
	/** A placed smoke changed: evaluate the placed smokes again once the edit is over. */
	bool bSmokeEvalPending = false;
	bool bSnapping = false;
	double LastSmokeEdit = 0.0;
	int32 PlacedSmokeCount = 0;
	/** Probe the last smoke plan or evaluation was for; used when no probe is selected. */
	FString LastSmokeObserver;
	/** When each drawn smoke appeared, by key, so a moved smoke keeps its look instead of swelling in again. */
	TMap<FString, double> SmokeBorn;
	FString LoggedSmokeError;
	/** Smokes applied to the probe rays, and their boxes at the last update, for the change report. */
	FString LiveSmokeKey;
	TMap<FString, FBox> SmokeBoxes;
	TMap<FString, FString> SmokeLabels;
	double LastLiveSmoke = 0.0;
	/** Pairs and rays before the first live smoke update since the last report, so the report still shows the change. */
	bool bLiveSmoke = false;
	TArray<FExposureEdge> LiveBefore;
	TArray<bool> LiveVisible;
	TArray<FString> LiveBlocker;

	bool bHasBaseline = false;
	FExposureBaseline Baseline;
	TArray<FExposureEdgeChange> BaselineChanges;
	int32 BaselineResolved = 0;
	int32 BaselineMissing = 0;
	TArray<FBox> BaselineEditBoxes;
	TArray<FString> BaselineGeomChanges;
	TArray<FString> BaselineProbeChanges;
	bool bBaselineInputsMatch = false;
	FString BaselineInputDiff;
	TArray<float> HeatDelta;
	TArray<bool> HeatDeltaValid;
	/** One-way count of the matching baseline sample, -1 when there is none. */
	TArray<int32> BaselineOneWay;

	FDelegateHandle MovedHandle;
	FDelegateHandle AddedHandle;
	FDelegateHandle DeletedHandle;
	FDelegateHandle PropertyHandle;
	FDelegateHandle UndoHandle;
	FDelegateHandle MapHandle;
	FDelegateHandle SelectionHandle;
	FDelegateHandle DrawHandle;
	FDelegateHandle CleanupHandle;
	FDelegateHandle SaveHandle;
};
