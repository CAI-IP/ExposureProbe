#pragma once

#include "CoreMinimal.h"
#include "ExposureProbeTypes.generated.h"

EXPOSUREPROBE_API DECLARE_LOG_CATEGORY_EXTERN(LogExposureProbe, Log, All);

UENUM(BlueprintType)
enum class EExposureTeam : uint8
{
	TSide UMETA(DisplayName = "T"),
	CTSide UMETA(DisplayName = "CT")
};

UENUM(BlueprintType)
enum class EExposurePose : uint8
{
	/** Stand when the head clearance fits the standing body, crouch when only the crouching body fits. */
	Auto,
	Stand,
	Crouch
};

UENUM(BlueprintType)
enum class EExposureBodyRegion : uint8
{
	Legs,
	Torso,
	Head
};

/** Game whose player body and map units a body follows. */
UENUM()
enum class EExposureUnitPreset : uint8
{
	/** Counter-Strike 2 / Source: 1 unit = 2.54 cm, player 72 x 32, eye 64, crouching 54 / eye 46. */
	Source UMETA(DisplayName = "CS2 / Source"),
	/** Call of Duty (Radiant): 1 unit = 2.54 cm, player 72 x 32, eye 60, crouching 52 / eye 40. */
	CallOfDuty UMETA(DisplayName = "Call of Duty"),
	/** Valorant: 1 unit = 1 cm, standing 180, crouching 140. */
	Valorant UMETA(DisplayName = "Valorant"),
	/** Unreal Engine first-person template: capsule 68 x 192 cm, eye 160, crouching 80 / eye 72. */
	UnrealTemplate UMETA(DisplayName = "UE First Person Template"),
	/** 1 unit = 1 cm; adult, 180 cm tall. */
	Unreal UMETA(DisplayName = "Adult 180 cm"),
	Custom
};

/** Body of one probe: the shared body, a game preset, or custom dimensions. */
UENUM()
enum class EExposureProbeBody : uint8
{
	Global UMETA(DisplayName = "Shared"),
	Source UMETA(DisplayName = "CS2 / Source"),
	CallOfDuty UMETA(DisplayName = "Call of Duty"),
	Valorant UMETA(DisplayName = "Valorant"),
	UnrealTemplate UMETA(DisplayName = "UE First Person Template"),
	Unreal UMETA(DisplayName = "Adult 180 cm"),
	Custom
};

/** Body dimensions in map units. Head, torso and legs stack from the feet up. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureBodyShape
{
	GENERATED_BODY()

	/** Top of the head; also the head clearance needed to stand. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ClampMin = "1"))
	float StandHeight = 72.f;

	/** Neck to the top of the head. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ClampMin = "0"))
	float HeadLength = 12.f;

	/** Crotch to neck. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ClampMin = "0"))
	float TorsoLength = 26.f;

	/** Feet to crotch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ClampMin = "0"))
	float LegLength = 34.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ClampMin = "0"))
	float StandEye = 64.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ClampMin = "0"))
	float BodyWidth = 32.f;

	/** Top of the head when crouching; also the head clearance needed to crouch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ClampMin = "1"))
	float CrouchHeight = 54.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ClampMin = "0"))
	float CrouchEye = 46.f;

	float Neck() const { return LegLength + TorsoLength; }

	/** Map units of a preset, in cm. */
	static float PresetUnitToCm(EExposureUnitPreset Preset);
	/** Far-reaching distance of a preset, in its own units. */
	static float PresetFarDistance(EExposureUnitPreset Preset);
	/** Body of a preset converted to map units of UnitToCm cm. */
	static FExposureBodyShape FromPreset(EExposureUnitPreset Preset, float UnitToCm);
	/** After Property changed from Old: the height scales every segment and eye, a segment changes the height. */
	void ApplyEdit(FName Property, const FExposureBodyShape& Old);
	void Clamp();
	void Scale(float K);
	bool Equals(const FExposureBodyShape& Other) const;
};

/** Preset of a probe body choice; Custom for Global and Custom. */
EXPOSUREPROBE_API EExposureUnitPreset ToUnitPreset(EExposureProbeBody Body);

/** A probe pair kept on purpose by the designer; stored on the T probe and saved with the level. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureDesignMark
{
	GENERATED_BODY()

	/** Name of the CT probe of the pair. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure Probe")
	FString CTProbe;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure Probe")
	FString Note;
};

/** One T-CT probe pair with both directed visible body shares. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureEdge
{
	GENERATED_BODY()

	/** "T0-CT3" */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Edge;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString TProbe;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString CTProbe;

	/** Body points of the CT probe visible from the T eye, out of ValidCT. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 VisibleTtoCT = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 ValidCT = 16;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 VisibleCTtoT = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 ValidT = 16;

	/** a(T->CT) */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float ShareTtoCT = 0.f;

	/** a(CT->T) */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float ShareCTtoT = 0.f;

	/** A = a(T->CT) - a(CT->T) */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float A = 0.f;

	/** Visible / valid points per body region (legs, torso, head) of the CT body seen from the T eye. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<int32> RegionVisibleTtoCT;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<int32> RegionValidCT;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<int32> RegionVisibleCTtoT;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<int32> RegionValidT;

	/** One side sees at least the one-way threshold of the other while being fully hidden itself. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bOneWay = false;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bDesignIntent = false;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString DesignNote;
};

/** A body sample ray whose visibility changed in an update. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureRayChange
{
	GENERATED_BODY()

	/** "CT3->T0" */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Direction;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	EExposureBodyRegion Region = EExposureBodyRegion::Torso;

	/** Sample height above the feet, in body units (see Unit To Cm). */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float HeightUnits = 0.f;

	/** Sample bearing around the body axis, degrees counter-clockwise in the map frame. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float AngleDeg = 0.f;

	/** "blocked" or "opened" */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Change;

	/** Actor that blocks the ray now ("blocked") or blocked it before ("opened"). */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Blocker;
};

/** How a probe pair changed between two versions, most important first in reports. */
UENUM(BlueprintType)
enum class EExposureChangeKind : uint8
{
	/** Counts changed, |dA| below the change threshold. */
	Minor,
	/** |dA| at or above the change threshold. */
	Shifted,
	/** A side that saw nothing of the other now sees part of it. */
	Opened,
	/** A side that saw part of the other now sees nothing. */
	Blocked,
	NewOneWay,
	OneWayResolved,
	/** The pair did not exist in the earlier version. */
	NewPair,
	/** One of the two probes moved, changed pose or body, so the change is not caused by geometry alone. */
	ProbeMoved
};

USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureEdgeChange
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Edge;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bHadBefore = false;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FExposureEdge Before;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FExposureEdge After;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bNewOneWay = false;

	/** Shortest distance from the edited geometry to the two probes of the pair, cm (-1 when unknown). */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float DistanceCm = -1.f;

	/** The pair lies farther from the edit than Far Distance. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bFar = false;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	EExposureChangeKind Kind = EExposureChangeKind::Minor;

	/** The pair is marked as design intent, so the change is expected. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bExpected = false;
};

/** Result of one audit update. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureAuditReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Time;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Summary;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bFullRecompute = false;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bParallel = true;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 Probes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 RaysTotal = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 RaysRecast = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 DirtyBoxes = 0;

	/** Actors whose collision geometry changed since the previous update. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FString> EditedActors;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 ChangedEdges = 0;

	/** New one-way pairs not marked as design intent. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 NewOneWay = 0;

	/** New one-way pairs already marked as design intent. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 NewOneWayDesignIntent = 0;

	/** Changed pairs farther from the edit than Far Distance. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 FarChanges = 0;

	/** Largest edit-to-pair distance among the changed pairs, cm. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float FarthestCm = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	double FilterMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	double CastMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	double MetricMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	double TotalMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FExposureEdgeChange> Changes;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FExposureRayChange> Evidence;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FExposureEdge> Edges;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FString> Warnings;
};

/** One sample of the full-map A field. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureHeatPoint
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FVector Feet = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bCrouch = false;

	/** Share of the neighbours' bodies seen from this eye. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float Vout = 0.f;

	/** Share of this body seen from the neighbours' eyes. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float Vin = 0.f;

	/** Vout - Vin: positive sees more than it is seen. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float A = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 Neighbors = 0;

	/** Nearby samples this spot sees one-way: it sees at least the one-way threshold of them, they see none of it. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 OneWaySees = 0;

	/** Nearby samples that see this spot one-way. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int32 OneWaySeenBy = 0;
};

/** A probe as it was when a baseline was saved. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureProbeRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	EExposureTeam Team = EExposureTeam::TSide;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FVector Feet = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	bool bCrouch = false;

	/** Body and sampling of the probe. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString BodyKey;
};

/** Collision geometry of one actor when a baseline was saved. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureGeomRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Label;

	/** Hash of the component transforms, bounds and meshes. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	int64 Hash = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FBox Box = FBox(ForceInit);
};

/** A saved state of the level's probe pairs and heatmap, for comparing later versions against. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureBaseline
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Time;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Level;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString ModelKey;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString EngineVersion;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString PluginVersion;

	/** Every plugin setting as exported text, so a later run can be checked against the same inputs. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString Settings;

	/** Body, sampling and grid of the heatmap. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	FString HeatKey;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FExposureProbeRecord> Probes;

	/** Fingerprint of every collision actor of the level. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FExposureGeomRecord> Geometry;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FExposureEdge> Edges;

	/** Heatmap grid spacing in cm; 0 when the baseline has no heatmap. */
	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	float HeatSpacingCm = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Exposure Probe")
	TArray<FExposureHeatPoint> Heat;
};


/** Smoke of a game. Sizes are radii of a sphere resting on the landing point. */
UENUM()
enum class EExposureSmokePreset : uint8
{
	/** Counter-Strike 2: radius 144, centre 60 above the landing point (Source units), lasting 22.06 s. */
	CS2 UMETA(DisplayName = "CS2 smoke grenade"),
	/** Valorant Brimstone Sky Smoke: radius about 4.1 m, 19.25 s (community measurements). */
	ValorantBrimstone UMETA(DisplayName = "Valorant Brimstone"),
	/** Valorant Omen Dark Cover: diameter about 8 m, 15 s (community measurements). */
	ValorantOmen UMETA(DisplayName = "Valorant Omen"),
	/** Valorant Astra Nebula: diameter about 9.5 m, 14.25 s (community measurements). */
	ValorantAstra UMETA(DisplayName = "Valorant Astra"),
	Custom
};

/** Size of one placed smoke: the shared size of the settings, a game's smoke, or custom. */
UENUM()
enum class EExposureSmokeSize : uint8
{
	Shared,
	CS2 UMETA(DisplayName = "CS2 smoke grenade"),
	ValorantBrimstone UMETA(DisplayName = "Valorant Brimstone"),
	ValorantOmen UMETA(DisplayName = "Valorant Omen"),
	ValorantAstra UMETA(DisplayName = "Valorant Astra"),
	Custom
};

/** How smoke is drawn in the viewport; the audit always uses the sphere. */
UENUM()
enum class EExposureSmokeStyle : uint8
{
	/** Ray-marched cloud with 3D noise, cut off by walls and floors. */
	Volume,
	/** Drifting translucent spheres. */
	Puffs
};

/** Smoke size in map units: a sphere of Radius whose centre is Lift above the landing point, lasting Duration seconds. */
USTRUCT(BlueprintType)
struct EXPOSUREPROBE_API FExposureSmokeShape
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoke", meta = (ClampMin = "1"))
	float Radius = 144.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoke", meta = (ClampMin = "0"))
	float Lift = 60.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoke", meta = (ClampMin = "0", Units = "s"))
	float Duration = 22.06f;

	/** Smoke of a preset in map units of UnitToCm cm. */
	static FExposureSmokeShape FromPreset(EExposureSmokePreset Preset, float UnitToCm);
	bool Equals(const FExposureSmokeShape& Other) const;
};

/** Preset of a placed smoke's size choice; Custom for Shared and Custom. */
EXPOSUREPROBE_API EExposureSmokePreset ToSmokePreset(EExposureSmokeSize Size);
