#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "ExposureProbeTypes.h"
#include "ExposureProbeSettings.generated.h"

UENUM()
enum class EExposureSampling : uint8
{
	/** 8 layers x 8 bearings over the full body height. */
	Fine UMETA(DisplayName = "64 points (8 x 8)"),
	/** 4 layers x 4 bearings (feet, knee, waist, head). */
	Points16 UMETA(DisplayName = "16 points (4 x 4)"),
	Custom
};

UENUM()
enum class EExposureHeatBody : uint8
{
	/** 4 layers x 4 bearings; fastest. */
	Points16 UMETA(DisplayName = "16 points (4 x 4)"),
	/** The probes' sampling. */
	Probe UMETA(DisplayName = "Same as probes")
};

UENUM()
enum class EExposureLanguage : uint8
{
	Auto,
	Chinese,
	English
};

/**
 * Body model and audit settings. Body dimensions are in units of the selected preset.
 * Visibility is always measured from the observer's eye to sample points on the other body.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Exposure Probe"))
class EXPOSUREPROBE_API UExposureProbeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UExposureProbeSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual void PreEditChange(FProperty* Property) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual bool CanEditChange(const FProperty* Property) const override;
#endif

	/** Units and body dimensions of a preset. */
	void ApplyPreset(EExposureUnitPreset InPreset);
	/** Sample layers and bearings of a sampling preset, from the current body height. */
	void ApplySampling(EExposureSampling InSampling);
	/** Keep the body consistent after Property changed: height scales the segments, a segment changes the height. */
	void ApplyBodyEdit(FName Property);
	/** Remember the body before an edit, for ApplyBodyEdit. */
	void SnapshotBody();
	/** Radius, lift and duration of a game's smoke, in the current map units. */
	void ApplySmokePreset(EExposureSmokePreset InPreset);
	/** Shared smoke size of the placed smokes and of the smoke plan. */
	FExposureSmokeShape GetSmokeShape() const;

	float GetNeck() const { return LegLength + TorsoLength; }
	float GetCrotch() const { return LegLength; }
	FExposureBodyShape GetShape() const;
	void SetShape(const FExposureBodyShape& Shape);
	/** Colour of one-way alerts. */
	static FLinearColor AlertColor() { return FLinearColor(FColor(229, 41, 79)); }
	/** One-way floor cells: yellow, apart from the red and blue of the heatmap and from the white floor by its outline. */
	static FLinearColor PeekColor() { return FLinearColor(FColor(255, 214, 10)); }
	static FLinearColor PeekEdgeColor() { return FLinearColor(FColor(150, 108, 0)); }
	/** Cells someone peeks, or that the selected probes peek. */
	static FLinearColor PeekedColor() { return FLinearColor(FColor(38, 38, 44)); }

	UPROPERTY(config, EditAnywhere, Category = "Units")
	EExposureUnitPreset UnitPreset = EExposureUnitPreset::Source;

	UPROPERTY(config, EditAnywhere, Category = "Units", meta = (ClampMin = "0.01"))
	float UnitToCm = 2.54f;

	/** Top of the head; also the head clearance needed to stand. Changing it scales head, torso, legs and eye. */
	UPROPERTY(config, EditAnywhere, Category = "Body", meta = (ClampMin = "1"))
	float StandHeight = 72.f;

	/** Neck to the top of the head. Changing a segment changes the height by the same amount. */
	UPROPERTY(config, EditAnywhere, Category = "Body", meta = (ClampMin = "0"))
	float HeadLength = 12.f;

	/** Crotch to neck. */
	UPROPERTY(config, EditAnywhere, Category = "Body", meta = (ClampMin = "0"))
	float TorsoLength = 26.f;

	/** Feet to crotch. */
	UPROPERTY(config, EditAnywhere, Category = "Body", meta = (ClampMin = "0"))
	float LegLength = 34.f;

	UPROPERTY(config, EditAnywhere, Category = "Body", meta = (ClampMin = "0"))
	float StandEye = 64.f;

	/** Body width; sample points lie on a circle of this diameter around the body axis. */
	UPROPERTY(config, EditAnywhere, Category = "Body", meta = (ClampMin = "0"))
	float BodyWidth = 32.f;

	/** Top of the head when crouching; also the head clearance needed to crouch. Head, torso and legs scale with it. */
	UPROPERTY(config, EditAnywhere, Category = "Body|Crouching", meta = (ClampMin = "1"))
	float CrouchHeight = 54.f;

	UPROPERTY(config, EditAnywhere, Category = "Body|Crouching", meta = (ClampMin = "1"))
	float CrouchEye = 46.f;

	UPROPERTY(config, EditAnywhere, Category = "Body|Sampling")
	EExposureSampling Sampling = EExposureSampling::Points16;

	/** Heights of the body sample layers above the feet when standing. */
	UPROPERTY(config, EditAnywhere, Category = "Body|Sampling", meta = (ClampMin = "0"))
	TArray<float> StandSampleHeights;

	/** Heights of the body sample layers when crouching (same number of layers as standing). */
	UPROPERTY(config, EditAnywhere, Category = "Body|Sampling", meta = (ClampMin = "0"))
	TArray<float> CrouchSampleHeights;

	/** Sample bearings per layer, evenly spaced around the body axis. */
	UPROPERTY(config, EditAnywhere, Category = "Body|Sampling", meta = (ClampMin = "1", ClampMax = "32"))
	int32 SampleBearings = 4;

	UPROPERTY(config, EditAnywhere, Category = "Audit", meta = (ClampMin = "0", ClampMax = "1"))
	float OneWayThreshold = 0.5f;

	/** Changed pairs farther than this from the edit are flagged as far-reaching (units). */
	UPROPERTY(config, EditAnywhere, Category = "Audit", meta = (ClampMin = "0"))
	float FarDistance = 500.f;

	/** |dA| at or above which a pair counts as clearly changed in the regression report. */
	UPROPERTY(config, EditAnywhere, Category = "Audit", meta = (ClampMin = "0.005", ClampMax = "1"))
	float ChangeThreshold = 0.05f;

	/** Save a "Last save" baseline every time the level is saved. */
	UPROPERTY(config, EditAnywhere, Category = "Audit")
	bool bBaselineOnSave = true;

	UPROPERTY(config, EditAnywhere, Category = "Audit")
	bool bAutoUpdate = true;

	UPROPERTY(config, EditAnywhere, Category = "Audit", meta = (ClampMin = "0", Units = "s"))
	float DebounceSeconds = 0.4f;

	UPROPERTY(config, EditAnywhere, Category = "Tracing")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Visibility;

	UPROPERTY(config, EditAnywhere, Category = "Tracing")
	bool bTraceComplex = true;

	/** A hit this close to the sample point still counts as visible. */
	UPROPERTY(config, EditAnywhere, Category = "Tracing", meta = (ClampMin = "0", Units = "cm"))
	float HitToleranceCm = 1.f;

	UPROPERTY(config, EditAnywhere, Category = "Tracing")
	bool bSnapProbesToFloor = true;

	UPROPERTY(config, EditAnywhere, Category = "Tracing")
	bool bParallelTraces = true;

	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ClampMin = "0.5", ClampMax = "6"))
	float RayThickness = 1.5f;

	UPROPERTY(config, EditAnywhere, Category = "Display")
	bool bShowBlockedRays = true;

	UPROPERTY(config, EditAnywhere, Category = "Display")
	bool bShowLabels = true;

	UPROPERTY(config, EditAnywhere, Category = "Display")
	FLinearColor TColor = FLinearColor(FColor(245, 166, 35));

	UPROPERTY(config, EditAnywhere, Category = "Display")
	FLinearColor CTColor = FLinearColor(FColor(61, 139, 255));

	UPROPERTY(config, EditAnywhere, Category = "Display")
	EExposureLanguage Language = EExposureLanguage::Auto;

	/** Grid spacing of the heatmap samples (units). */
	UPROPERTY(config, EditAnywhere, Category = "Heatmap", meta = (ClampMin = "4"))
	float HeatmapSpacing = 64.f;

	/** Other samples within this distance count towards a sample's A (units). */
	UPROPERTY(config, EditAnywhere, Category = "Heatmap", meta = (ClampMin = "16"))
	float HeatmapRadius = 1000.f;

	/** |A| at which the colour is full; 0 picks it from the map (90th percentile of |A|). */
	UPROPERTY(config, EditAnywhere, Category = "Heatmap", meta = (ClampMin = "0", ClampMax = "1"))
	float HeatmapScale = 0.f;

	UPROPERTY(config, EditAnywhere, Category = "Heatmap")
	EExposureHeatBody HeatmapBody = EExposureHeatBody::Points16;

	/** Sample only where the navigation mesh is, when the level has one. */
	UPROPERTY(config, EditAnywhere, Category = "Heatmap")
	bool bHeatmapOnNavMesh = true;

	/** Without a navigation mesh, keep only floor that can be walked to from a probe or a player start. */
	UPROPERTY(config, EditAnywhere, Category = "Heatmap")
	bool bHeatmapReachableOnly = true;

	/** Highest step a player can get up onto, e.g. by crouch-jumping (units). */
	UPROPERTY(config, EditAnywhere, Category = "Heatmap", meta = (ClampMin = "1"))
	float HeatmapMaxClimb = 64.f;

	/** Colour where a spot sees more than it is seen. */
	UPROPERTY(config, EditAnywhere, Category = "Heatmap")
	FLinearColor HeatSeesColor = FLinearColor(FColor(33, 102, 172));

	/** Colour where a spot is seen more than it sees. */
	UPROPERTY(config, EditAnywhere, Category = "Heatmap")
	FLinearColor HeatSeenColor = FLinearColor(FColor(178, 24, 43));

	/** Game whose smoke size the smoke plan and the placed smokes use; editing a size switches it to Custom. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke")
	EExposureSmokePreset SmokePreset = EExposureSmokePreset::CS2;

	/** Radius of a smoke, modelled as a sphere (units). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "1"))
	float SmokeRadius = 144.f;

	/** Height of the smoke centre above the landing point (units). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0"))
	float SmokeLift = 60.f;

	/** Share of the visible sightlines the smokes must cut; CC is the fewest smokes that reach it. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0.05", ClampMax = "1"))
	float SmokeCoverage = 0.8f;

	/** Most smokes tried before CC counts as right-censored. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "1", ClampMax = "10"))
	int32 SmokeMaxCount = 5;

	/** A landing point needs this much free height above it (units). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0"))
	float SmokeClearance = 64.f;

	/** Landing points closer to the observer than this are skipped (units). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0"))
	float SmokeMinDistance = 200.f;

	/** Landing points within this distance of the observer or a target count (units). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "1"))
	float SmokeSearchRadius = 2500.f;

	/** How long a smoke lasts; RT never exceeds it. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0", Units = "s"))
	float SmokeDuration = 22.06f;

	/** Detour spots are searched within this distance of the observer (units). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "1"))
	float SmokeRetreatRadius = 2000.f;

	/** A detour spot must see at least this share of the original sightlines past the smokes. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0.05", ClampMax = "1"))
	float SmokeRecoverShare = 0.8f;

	/** A detour spot may have an A at most this much lower than the observer's spot (needs the floor analysis). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0", ClampMax = "2"))
	float SmokeRecoverDeltaA = 0.05f;

	/** Walking speed for the detour time (units per second). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "1"))
	float SmokeMoveSpeed = 113.f;

	/** Time to aim again after arriving. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0", Units = "s"))
	float SmokeReacquireSeconds = 0.5f;

	/** Volume: a ray-marched cloud cut off by walls and floors. Puffs: drifting translucent spheres. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke")
	EExposureSmokeStyle SmokeStyle = EExposureSmokeStyle::Volume;

	/** Thickness of the volume smoke; higher hides more behind it. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke", meta = (ClampMin = "0.1", ClampMax = "20"))
	float SmokeDensity = 6.f;

	UPROPERTY(config, EditAnywhere, Category = "Smoke")
	FLinearColor SmokeColor = FLinearColor(0.62f, 0.62f, 0.65f);

	/** Add a local fog volume inside each smoke; it shows from about 10 m away, the puffs show at any distance. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke")
	bool bSmokeFog = false;

	/** Placed smokes block the probe rays: links, body fills, A and the change report count them. The floor layers stay static. */
	UPROPERTY(config, EditAnywhere, Category = "Smoke")
	bool bSmokesBlockProbes = true;

	/** Placed smokes snap to the nearest floor cell after a move (needs the floor analysis). */
	UPROPERTY(config, EditAnywhere, Category = "Smoke")
	bool bSnapSmokesToCells = true;

private:
	FExposureBodyShape OldShape;
};
