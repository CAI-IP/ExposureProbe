#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "ExposureProbeTypes.h"

class UWorld;
class AActor;

/** Body model in body units (see UnitToCm). Visibility is measured from one body's eye to the other body's sample points. */
struct EXPOSUREPROBE_API FExposureBodyModel
{
	float UnitToCm = 2.54f;
	float StandHeight = 72.f;
	float StandEye = 64.f;
	float StandNeck = 60.f;
	float StandCrotch = 34.f;
	float CrouchHeight = 54.f;
	float CrouchEye = 46.f;
	TArray<float> HeightsStand = { 6.f, 20.f, 38.f, 66.f };
	TArray<float> HeightsCrouch = { 5.f, 15.f, 28.f, 50.f };
	float Radius = 16.f;
	int32 Bearings = 4;
	TEnumAsByte<ECollisionChannel> Channel = ECC_Visibility;
	bool bTraceComplex = true;
	float HitToleranceCm = 1.f;

	static FExposureBodyModel FromSettings();
	/** Same body with Layers x Bearings points spread evenly over the full body height. */
	FExposureBodyModel Dense(int32 Layers, int32 InBearings) const;
	/** Same body with 4 x 4 points (feet, knee, waist, head). */
	FExposureBodyModel Points16() const;
	/** Same sampling with other body dimensions; sample heights scale with the standing and crouching heights. */
	FExposureBodyModel WithShape(const FExposureBodyShape& Shape) const;

	int32 NumPoints() const { return HeightsStand.Num() * Bearings; }
	float TopUnits(bool bCrouch) const { return bCrouch ? CrouchHeight : StandHeight; }
	float EyeUnits(bool bCrouch) const { return bCrouch ? CrouchEye : StandEye; }
	float NeckUnits(bool bCrouch) const;
	float CrotchUnits(bool bCrouch) const;
	float EyeCm(bool bCrouch) const { return EyeUnits(bCrouch) * UnitToCm; }
	const TArray<float>& Heights(bool bCrouch) const { return bCrouch ? HeightsCrouch : HeightsStand; }
	float PointHeight(int32 Point, bool bCrouch) const { return Heights(bCrouch)[Point / Bearings]; }
	float BearingDeg(int32 Point) const { return 360.f * (Point % Bearings) / Bearings; }
	EExposureBodyRegion RegionOf(int32 Point, bool bCrouch) const;
	/** Layers x bearings, layer-major; bearings counter-clockwise in the map frame starting at +X. */
	void BodyPoints(const FVector& Feet, bool bCrouch, TArray<FVector>& Out) const;
	FString Key() const;
};

struct EXPOSUREPROBE_API FExposureProbeDesc
{
	FString Name;
	EExposureTeam Team = EExposureTeam::TSide;
	FVector Feet = FVector::ZeroVector;
	bool bCrouch = false;
	float ClearanceUnits = 0.f;
	/** Body of this probe; when bOwnBody is false, Build fills in the shared body. */
	FExposureBodyModel Body;
	bool bOwnBody = false;
};

struct EXPOSUREPROBE_API FExposureRay
{
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	int32 From = INDEX_NONE;
	int32 To = INDEX_NONE;
	int32 Point = 0;
	/** Axis-to-point ray used for the embedded-point mask. */
	bool bValidity = false;
	/** Visible: clear of static geometry and of the placed smokes. */
	bool bVisible = false;
	/** Clear of static geometry; smokes are not counted. */
	bool bClear = false;
	/** Placed smoke that cuts the ray, or INDEX_NONE. */
	int32 Smoke = INDEX_NONE;
	FVector Hit = FVector::ZeroVector;
	FString Blocker;
	AActor* BlockerRaw = nullptr;
};

/**
 * Ray table for a set of T/CT probes: for every T-CT pair, one ray from each eye to every sample point
 * of the other body, plus one axis-to-point ray per sample point for the embedded-point mask.
 */
class EXPOSUREPROBE_API FExposureProbeSolver
{
public:
	void Build(const TArray<FExposureProbeDesc>& InProbes, const FExposureBodyModel& InModel);
	void Cast(const UWorld* World, const TArray<int32>& Indices, bool bParallel);
	void CastAll(const UWorld* World, bool bParallel);

	/** Rays whose segment crosses any of the boxes (boxes are padded by 1 cm). */
	TArray<int32> RaysCrossing(const TArray<FBox>& Boxes) const;

	/** A placed smoke as an occluder of the probe rays. */
	struct FSmokeSphere
	{
		FVector Centre = FVector::ZeroVector;
		float Radius = 0.f;
		FString Name;
	};
	/** Rays clear of static geometry that pass through a smoke become blocked by it; the others go back to their static result.
	 * Validity rays are not affected. Returns the number of rays the smokes cut. */
	int32 ApplySmokes(const TArray<FSmokeSphere>& Smokes);
	TArray<FExposureEdge> ComputeEdges(float OneWayThreshold) const;
	FString RayKey(int32 Index) const;
	FString RayDirection(int32 Index) const;

	const TArray<FExposureRay>& Rays() const { return RayList; }
	const TArray<FExposureProbeDesc>& Probes() const { return ProbeList; }
	/** Shared body; a probe may have its own (see BodyOf). */
	const FExposureBodyModel& Model() const { return BodyModel; }
	const FExposureBodyModel& BodyOf(int32 Probe) const { return ProbeList[Probe].Body; }
	int32 PointsPerBody() const { return BodyModel.NumPoints(); }
	/** First ray index of the rays From eye -> To body, or INDEX_NONE. */
	int32 PairBase(int32 From, int32 To) const;
	/** Embedded-point mask; when every point of a probe is embedded, all count as valid. */
	bool PointValid(int32 Probe, int32 Point) const;
	int32 FindProbe(const FString& Name) const;

	/** True if the segment is clear (no hit, or the hit lies within the tolerance of the end point). */
	static bool Trace(const UWorld* World, const FVector& A, const FVector& B, const FExposureBodyModel& Model, AActor*& OutBlocker, FVector* OutHit = nullptr);
	/** Head clearance above the feet in body units (FLT_MAX when nothing is hit within 3x the standing height). */
	static float MeasureClearance(const UWorld* World, const FVector& Feet, const FExposureBodyModel& Model);
	static bool IsOneWay(const FExposureEdge& Edge, float Threshold);
	/** Stand, crouch or skip from the pose setting and the head clearance. Returns false when the probe is skipped. */
	static bool ResolvePose(const UWorld* World, const FVector& Feet, EExposurePose Pose, const FExposureBodyModel& Model, bool& bOutCrouch, float& OutClearance);
	/** First surface found from 30/72 of the standing height above the point downwards, or the point itself when nothing is hit. */
	static FVector SnapToFloor(const UWorld* World, const FVector& Point, const FExposureBodyModel& Model);

	/** A floor sample of the heatmap and its result. */
	struct FPoint
	{
		FVector Feet = FVector::ZeroVector;
		float Weight = 1.f;
		bool bCrouch = false;
	};
	struct FPointResult
	{
		double Vout = 0.0;
		double Vin = 0.0;
		double A = 0.0;
		int32 Neighbors = 0;
		int32 ValidPoints = 0;
	};
private:
	FExposureBodyModel BodyModel;
	TArray<FExposureProbeDesc> ProbeList;
	TArray<FExposureRay> RayList;
	TMap<FIntPoint, int32> PairBases;
	TArray<int32> ValidityBases;
};

/** Full-map A on sample points, cast in steps so the editor can show progress and cancel. */
class EXPOSUREPROBE_API FExposureHeatJob
{
public:
	/** Body points, the embedded-point mask and the neighbour lists. */
	void Prepare(const UWorld* World, const TArray<FExposureProbeSolver::FPoint>& InPoints, double RadiusCm, const FExposureBodyModel& InModel, bool bInParallel);
	/** Casts the rows of up to MaxObservers more samples. Returns true when every row is done. */
	bool Step(const UWorld* World, int32 MaxObservers);
	void Finish(TArray<FExposureProbeSolver::FPointResult>& Out) const;
	/** For each sample, the nearby samples it sees one-way: share >= Threshold one way and zero the other way. */
	void OneWayTargets(float Threshold, TArray<TArray<int32>>& Out) const;
	float Progress() const { return Points.Num() ? float(Next) / Points.Num() : 1.f; }
	int64 Rays() const { return RayCount; }
	int32 Num() const { return Points.Num(); }
	const FExposureBodyModel& GetModel() const { return Model; }
	const TArray<TArray<FVector>>& GetBodies() const { return Bodies; }
	const TArray<TArray<bool>>& GetValid() const { return Valid; }
	const TArray<FVector>& GetEyes() const { return Eyes; }

private:
	FExposureBodyModel Model;
	TArray<FExposureProbeSolver::FPoint> Points;
	TArray<TArray<FVector>> Bodies;
	TArray<TArray<bool>> Valid;
	TArray<FVector> Eyes;
	TArray<TArray<int32>> Nbr;
	TArray<TMap<int32, float>> Share;
	int32 Next = 0;
	int64 RayCount = 0;
	bool bParallel = true;
};

