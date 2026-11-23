#include "ExposureProbeSolver.h"

#include "ExposureProbeSettings.h"
#include "Async/ParallelFor.h"
#include "CollisionQueryParams.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

FExposureBodyModel FExposureBodyModel::FromSettings()
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	FExposureBodyModel M;
	M.UnitToCm = S->UnitToCm;
	M.StandHeight = S->StandHeight;
	M.StandEye = S->StandEye;
	M.StandNeck = S->GetNeck();
	M.StandCrotch = S->GetCrotch();
	M.CrouchHeight = S->CrouchHeight;
	M.CrouchEye = S->CrouchEye;
	if (S->StandSampleHeights.Num() > 0)
	{
		M.HeightsStand = S->StandSampleHeights;
		if (S->CrouchSampleHeights.Num() == S->StandSampleHeights.Num())
		{
			M.HeightsCrouch = S->CrouchSampleHeights;
		}
		else
		{
			const float Scale = S->CrouchHeight / FMath::Max(S->StandHeight, 1.f);
			M.HeightsCrouch.Reset();
			for (const float H : M.HeightsStand) { M.HeightsCrouch.Add(H * Scale); }
		}
	}
	M.Radius = 0.5f * S->BodyWidth;
	M.Bearings = FMath::Clamp(S->SampleBearings, 1, 32);
	M.Channel = S->TraceChannel;
	M.bTraceComplex = S->bTraceComplex;
	M.HitToleranceCm = S->HitToleranceCm;
	return M;
}

FExposureBodyModel FExposureBodyModel::Dense(int32 Layers, int32 InBearings) const
{
	FExposureBodyModel M = *this;
	M.Bearings = FMath::Max(1, InBearings);
	M.HeightsStand.Reset();
	M.HeightsCrouch.Reset();
	for (int32 i = 0; i < FMath::Max(1, Layers); ++i)
	{
		const float T = (i + 0.5f) / FMath::Max(1, Layers);
		M.HeightsStand.Add(T * StandHeight);
		M.HeightsCrouch.Add(T * CrouchHeight);
	}
	return M;
}

FExposureBodyModel FExposureBodyModel::Points16() const
{
	static const float StandFrac[4] = { 6.f / 72.f, 20.f / 72.f, 38.f / 72.f, 66.f / 72.f };
	static const float CrouchFrac[4] = { 5.f / 54.f, 15.f / 54.f, 28.f / 54.f, 50.f / 54.f };
	FExposureBodyModel M = *this;
	M.Bearings = 4;
	M.HeightsStand.Reset();
	M.HeightsCrouch.Reset();
	for (int32 i = 0; i < 4; ++i)
	{
		M.HeightsStand.Add(StandFrac[i] * StandHeight);
		M.HeightsCrouch.Add(CrouchFrac[i] * CrouchHeight);
	}
	return M;
}

FExposureBodyModel FExposureBodyModel::WithShape(const FExposureBodyShape& S) const
{
	FExposureBodyModel M = *this;
	const float KStand = S.StandHeight / FMath::Max(StandHeight, 1.f);
	const float KCrouch = S.CrouchHeight / FMath::Max(CrouchHeight, 1.f);
	for (float& H : M.HeightsStand) { H *= KStand; }
	for (float& H : M.HeightsCrouch) { H *= KCrouch; }
	M.StandHeight = S.StandHeight;
	M.StandEye = S.StandEye;
	M.StandNeck = S.Neck();
	M.StandCrotch = S.LegLength;
	M.CrouchHeight = S.CrouchHeight;
	M.CrouchEye = S.CrouchEye;
	M.Radius = 0.5f * S.BodyWidth;
	return M;
}

float FExposureBodyModel::NeckUnits(bool bCrouch) const
{
	return bCrouch ? StandNeck * CrouchHeight / FMath::Max(StandHeight, 1.f) : StandNeck;
}

float FExposureBodyModel::CrotchUnits(bool bCrouch) const
{
	return bCrouch ? StandCrotch * CrouchHeight / FMath::Max(StandHeight, 1.f) : StandCrotch;
}

EExposureBodyRegion FExposureBodyModel::RegionOf(int32 Point, bool bCrouch) const
{
	const float H = PointHeight(Point, bCrouch);
	if (H > NeckUnits(bCrouch)) { return EExposureBodyRegion::Head; }
	if (H > CrotchUnits(bCrouch)) { return EExposureBodyRegion::Torso; }
	return EExposureBodyRegion::Legs;
}

void FExposureBodyModel::BodyPoints(const FVector& Feet, bool bCrouch, TArray<FVector>& Out) const
{
	// Counter-clockwise in a right-handed map frame. UE is left-handed (map y is negated on import), so the UE y offset is -sin.
	Out.Reset(NumPoints());
	const float R = Radius * UnitToCm;
	for (const float H : Heights(bCrouch))
	{
		for (int32 b = 0; b < Bearings; ++b)
		{
			const float Rad = FMath::DegreesToRadians(360.f * b / Bearings);
			Out.Add(Feet + FVector(R * FMath::Cos(Rad), -R * FMath::Sin(Rad), H * UnitToCm));
		}
	}
}

FString FExposureBodyModel::Key() const
{
	FString K = FString::Printf(TEXT("%g|%g|%g|%g|%g|%g|%g|%g|%d|%d|%d|%g|"), UnitToCm, StandHeight, StandEye, StandNeck, StandCrotch,
		CrouchHeight, CrouchEye, Radius, Bearings, int32(Channel.GetValue()), bTraceComplex ? 1 : 0, HitToleranceCm);
	for (const float H : HeightsStand) { K += FString::Printf(TEXT("%g,"), H); }
	K += TEXT("|");
	for (const float H : HeightsCrouch) { K += FString::Printf(TEXT("%g,"), H); }
	return K;
}

bool FExposureProbeSolver::Trace(const UWorld* World, const FVector& A, const FVector& B, const FExposureBodyModel& Model, AActor*& OutBlocker, FVector* OutHit)
{
	OutBlocker = nullptr;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExposureProbeTrace), Model.bTraceComplex);
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, A, B, Model.Channel, Params))
	{
		return true;
	}
	if (Hit.Distance >= FVector::Dist(A, B) - Model.HitToleranceCm)
	{
		return true;
	}
	OutBlocker = Hit.GetActor();
	if (OutHit) { *OutHit = Hit.Location; }
	return false;
}

float FExposureProbeSolver::MeasureClearance(const UWorld* World, const FVector& Feet, const FExposureBodyModel& Model)
{
	const FVector Start = Feet + FVector(0.f, 0.f, 0.014f * Model.StandHeight * Model.UnitToCm);
	const FVector End = Feet + FVector(0.f, 0.f, 3.f * Model.StandHeight * Model.UnitToCm);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExposureProbeClearance), Model.bTraceComplex);
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Start, End, Model.Channel, Params))
	{
		return FLT_MAX;
	}
	return (Hit.Location.Z - Feet.Z) / Model.UnitToCm;
}

bool FExposureProbeSolver::ResolvePose(const UWorld* World, const FVector& Feet, EExposurePose Pose, const FExposureBodyModel& Model, bool& bOutCrouch, float& OutClearance)
{
	OutClearance = MeasureClearance(World, Feet, Model);
	switch (Pose)
	{
	case EExposurePose::Stand: bOutCrouch = false; return true;
	case EExposurePose::Crouch: bOutCrouch = true; return true;
	default: break;
	}
	if (OutClearance >= Model.StandHeight) { bOutCrouch = false; return true; }
	if (OutClearance >= Model.CrouchHeight) { bOutCrouch = true; return true; }
	return false;
}

FVector FExposureProbeSolver::SnapToFloor(const UWorld* World, const FVector& Point, const FExposureBodyModel& Model)
{
	const float Reach = Model.StandHeight * (30.f / 72.f) * Model.UnitToCm;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExposureProbeFloor), Model.bTraceComplex);
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Point + FVector(0.f, 0.f, Reach), Point - FVector(0.f, 0.f, Reach), Model.Channel, Params))
	{
		return FVector(Point.X, Point.Y, Hit.Location.Z);
	}
	return Point;
}

bool FExposureProbeSolver::IsOneWay(const FExposureEdge& E, float Threshold)
{
	return (E.ShareTtoCT >= Threshold && E.VisibleCTtoT == 0) || (E.ShareCTtoT >= Threshold && E.VisibleTtoCT == 0);
}

void FExposureProbeSolver::Build(const TArray<FExposureProbeDesc>& InProbes, const FExposureBodyModel& InModel)
{
	BodyModel = InModel;
	ProbeList = InProbes;
	ProbeList.Sort([](const FExposureProbeDesc& L, const FExposureProbeDesc& R) { return L.Name < R.Name; });
	for (FExposureProbeDesc& D : ProbeList)
	{
		// Every body needs the same number of sample points.
		if (!D.bOwnBody || D.Body.NumPoints() != BodyModel.NumPoints())
		{
			D.Body = BodyModel;
			D.bOwnBody = false;
		}
	}
	RayList.Reset();
	PairBases.Reset();
	ValidityBases.Reset();

	TArray<TArray<FVector>> Bodies;
	Bodies.SetNum(ProbeList.Num());
	for (int32 i = 0; i < ProbeList.Num(); ++i)
	{
		ProbeList[i].Body.BodyPoints(ProbeList[i].Feet, ProbeList[i].bCrouch, Bodies[i]);
	}

	auto AddPair = [&](int32 From, int32 To)
	{
		PairBases.Add(FIntPoint(From, To), RayList.Num());
		const FVector Eye = ProbeList[From].Feet + FVector(0.f, 0.f, ProbeList[From].Body.EyeCm(ProbeList[From].bCrouch));
		for (int32 p = 0; p < Bodies[To].Num(); ++p)
		{
			FExposureRay& R = RayList.AddDefaulted_GetRef();
			R.Start = Eye; R.End = Bodies[To][p]; R.From = From; R.To = To; R.Point = p;
		}
	};
	for (int32 t = 0; t < ProbeList.Num(); ++t)
	{
		if (ProbeList[t].Team != EExposureTeam::TSide) { continue; }
		for (int32 c = 0; c < ProbeList.Num(); ++c)
		{
			if (ProbeList[c].Team != EExposureTeam::CTSide) { continue; }
			AddPair(t, c);
			AddPair(c, t);
		}
	}
	for (int32 k = 0; k < ProbeList.Num(); ++k)
	{
		ValidityBases.Add(RayList.Num());
		const FVector& F = ProbeList[k].Feet;
		for (int32 p = 0; p < Bodies[k].Num(); ++p)
		{
			FExposureRay& R = RayList.AddDefaulted_GetRef();
			R.Start = FVector(F.X, F.Y, Bodies[k][p].Z); R.End = Bodies[k][p]; R.From = k; R.To = k; R.Point = p; R.bValidity = true;
		}
	}
}

void FExposureProbeSolver::Cast(const UWorld* World, const TArray<int32>& Indices, bool bParallel)
{
	if (!World || Indices.Num() == 0)
	{
		return;
	}
	const FExposureBodyModel& M = BodyModel;
	ParallelFor(Indices.Num(), [&](int32 n)
	{
		FExposureRay& R = RayList[Indices[n]];
		AActor* Blocker = nullptr;
		R.bClear = Trace(World, R.Start, R.End, M, Blocker, &R.Hit);
		R.bVisible = R.bClear;
		R.Smoke = INDEX_NONE;
		R.BlockerRaw = Blocker;
	}, bParallel ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread);

	for (const int32 i : Indices)
	{
		FExposureRay& R = RayList[i];
#if WITH_EDITOR
		R.Blocker = R.BlockerRaw ? R.BlockerRaw->GetActorLabel() : FString();
#else
		R.Blocker = R.BlockerRaw ? R.BlockerRaw->GetName() : FString();
#endif
		R.BlockerRaw = nullptr;
	}
}

void FExposureProbeSolver::CastAll(const UWorld* World, bool bParallel)
{
	TArray<int32> All;
	All.Reserve(RayList.Num());
	for (int32 i = 0; i < RayList.Num(); ++i) { All.Add(i); }
	Cast(World, All, bParallel);
}

int32 FExposureProbeSolver::ApplySmokes(const TArray<FSmokeSphere>& Smokes)
{
	int32 NCut = 0;
	for (FExposureRay& R : RayList)
	{
		if (R.bValidity)
		{
			continue;
		}
		int32 Hit = INDEX_NONE;
		double HitT = 1.0;
		if (R.bClear)
		{
			const FVector V = R.End - R.Start;
			const double A = V.SizeSquared();
			for (int32 s = 0; s < Smokes.Num() && A > 0.0; ++s)
			{
				// First entry of the segment into the sphere.
				const FVector D = R.Start - Smokes[s].Centre;
				const double B = FVector::DotProduct(D, V);
				const double C = D.SizeSquared() - double(Smokes[s].Radius) * Smokes[s].Radius;
				const double Disc = B * B - A * C;
				if (Disc < 0.0) { continue; }
				const double Sq = FMath::Sqrt(Disc);
				const double T0 = (-B - Sq) / A, T1 = (-B + Sq) / A;
				if (T1 < 0.0 || T0 > 1.0) { continue; }
				const double T = FMath::Max(T0, 0.0);
				if (Hit == INDEX_NONE || T < HitT) { Hit = s; HitT = T; }
			}
		}
		if (Hit != INDEX_NONE)
		{
			R.bVisible = false;
			R.Hit = R.Start + HitT * (R.End - R.Start);
			if (R.Smoke != Hit || R.Blocker != Smokes[Hit].Name) { R.Blocker = Smokes[Hit].Name; }
			R.Smoke = Hit;
			++NCut;
		}
		else
		{
			if (R.Smoke != INDEX_NONE) { R.Blocker.Reset(); }
			R.Smoke = INDEX_NONE;
			R.bVisible = R.bClear;
		}
	}
	return NCut;
}

TArray<int32> FExposureProbeSolver::RaysCrossing(const TArray<FBox>& Boxes) const
{
	TArray<int32> Out;
	for (int32 i = 0; i < RayList.Num(); ++i)
	{
		const FExposureRay& R = RayList[i];
		const FVector Dir = R.End - R.Start;
		for (const FBox& B : Boxes)
		{
			if (B.IsValid && FMath::LineBoxIntersection(B.ExpandBy(1.0), R.Start, R.End, Dir))
			{
				Out.Add(i);
				break;
			}
		}
	}
	return Out;
}

int32 FExposureProbeSolver::PairBase(int32 From, int32 To) const
{
	const int32* B = PairBases.Find(FIntPoint(From, To));
	return B ? *B : INDEX_NONE;
}

bool FExposureProbeSolver::PointValid(int32 Probe, int32 Point) const
{
	const int32 Base = ValidityBases[Probe];
	if (RayList[Base + Point].bVisible)
	{
		return true;
	}
	for (int32 p = 0; p < PointsPerBody(); ++p)
	{
		if (RayList[Base + p].bVisible) { return false; }
	}
	return true;
}

int32 FExposureProbeSolver::FindProbe(const FString& Name) const
{
	return ProbeList.IndexOfByPredicate([&Name](const FExposureProbeDesc& D) { return D.Name == Name; });
}

TArray<FExposureEdge> FExposureProbeSolver::ComputeEdges(float OneWayThreshold) const
{
	const int32 K = PointsPerBody();
	auto Count = [&](int32 From, int32 To, int32& OutVis, int32& OutValid, TArray<int32>& RegVis, TArray<int32>& RegValid)
	{
		OutVis = 0; OutValid = 0;
		RegVis.Init(0, 3); RegValid.Init(0, 3);
		const int32 Base = PairBase(From, To);
		const bool bCrouch = ProbeList[To].bCrouch;
		for (int32 p = 0; p < K; ++p)
		{
			if (!PointValid(To, p)) { continue; }
			const int32 Reg = int32(ProbeList[To].Body.RegionOf(p, bCrouch));
			const bool bVis = RayList[Base + p].bVisible;
			++OutValid; ++RegValid[Reg];
			if (bVis) { ++OutVis; ++RegVis[Reg]; }
		}
	};

	TArray<FExposureEdge> Out;
	for (int32 t = 0; t < ProbeList.Num(); ++t)
	{
		if (ProbeList[t].Team != EExposureTeam::TSide) { continue; }
		for (int32 c = 0; c < ProbeList.Num(); ++c)
		{
			if (ProbeList[c].Team != EExposureTeam::CTSide) { continue; }
			FExposureEdge E;
			E.TProbe = ProbeList[t].Name; E.CTProbe = ProbeList[c].Name;
			E.Edge = E.TProbe + TEXT("-") + E.CTProbe;
			Count(t, c, E.VisibleTtoCT, E.ValidCT, E.RegionVisibleTtoCT, E.RegionValidCT);
			Count(c, t, E.VisibleCTtoT, E.ValidT, E.RegionVisibleCTtoT, E.RegionValidT);
			E.ShareTtoCT = E.ValidCT ? float(E.VisibleTtoCT) / E.ValidCT : 0.f;
			E.ShareCTtoT = E.ValidT ? float(E.VisibleCTtoT) / E.ValidT : 0.f;
			E.A = E.ShareTtoCT - E.ShareCTtoT;
			E.bOneWay = IsOneWay(E, OneWayThreshold);
			Out.Add(E);
		}
	}
	return Out;
}

FString FExposureProbeSolver::RayDirection(int32 Index) const
{
	const FExposureRay& R = RayList[Index];
	return ProbeList[R.From].Name + TEXT("->") + ProbeList[R.To].Name;
}

FString FExposureProbeSolver::RayKey(int32 Index) const
{
	const FExposureRay& R = RayList[Index];
	return FString::Printf(TEXT("%s%s/%d"), R.bValidity ? TEXT("valid:") : TEXT(""), *RayDirection(Index), R.Point);
}

void FExposureHeatJob::Prepare(const UWorld* World, const TArray<FExposureProbeSolver::FPoint>& InPoints, double RadiusCm, const FExposureBodyModel& InModel, bool bInParallel)
{
	Model = InModel;
	Points = InPoints;
	bParallel = bInParallel;
	Next = 0;
	const int32 N = Points.Num();
	const int32 K = Model.NumPoints();
	Bodies.Reset(); Valid.Reset(); Eyes.Reset(); Nbr.Reset(); Share.Reset();
	Bodies.SetNum(N); Valid.SetNum(N); Eyes.SetNum(N); Nbr.SetNum(N); Share.SetNum(N);
	RayCount = int64(N) * K;
	if (!World || N == 0)
	{
		return;
	}
	const EParallelForFlags Flags = bParallel ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread;
	ParallelFor(N, [&](int32 i)
	{
		Model.BodyPoints(Points[i].Feet, Points[i].bCrouch, Bodies[i]);
		Eyes[i] = Points[i].Feet + FVector(0.f, 0.f, Model.EyeCm(Points[i].bCrouch));
		Valid[i].Init(false, K);
		bool bAny = false;
		for (int32 p = 0; p < K; ++p)
		{
			AActor* Ignored = nullptr;
			Valid[i][p] = FExposureProbeSolver::Trace(World, FVector(Points[i].Feet.X, Points[i].Feet.Y, Bodies[i][p].Z), Bodies[i][p], Model, Ignored);
			bAny |= Valid[i][p];
		}
		if (!bAny) { Valid[i].Init(true, K); }
	}, Flags);

	// Neighbours within the radius, through a uniform grid of radius-sized cells; lists stay in ascending order.
	const double R2 = RadiusCm * RadiusCm;
	const double Cell = FMath::Max(RadiusCm, 1.0);
	TMap<FIntVector, TArray<int32>> Grid;
	auto CellOf = [Cell](const FVector& P) { return FIntVector(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell), FMath::FloorToInt(P.Z / Cell)); };
	for (int32 i = 0; i < N; ++i) { Grid.FindOrAdd(CellOf(Points[i].Feet)).Add(i); }
	ParallelFor(N, [&](int32 i)
	{
		const FIntVector C = CellOf(Points[i].Feet);
		for (int32 dx = -1; dx <= 1; ++dx)
		for (int32 dy = -1; dy <= 1; ++dy)
		for (int32 dz = -1; dz <= 1; ++dz)
		{
			if (const TArray<int32>* Bucket = Grid.Find(C + FIntVector(dx, dy, dz)))
			{
				for (const int32 j : *Bucket)
				{
					if (i != j && FVector::DistSquared(Points[i].Feet, Points[j].Feet) <= R2) { Nbr[i].Add(j); }
				}
			}
		}
		Nbr[i].Sort();
	}, Flags);
}

bool FExposureHeatJob::Step(const UWorld* World, int32 MaxObservers)
{
	const int32 N = Points.Num();
	const int32 K = Model.NumPoints();
	const int32 Begin = Next;
	const int32 End = FMath::Min(N, Begin + FMath::Max(1, MaxObservers));
	if (World && End > Begin)
	{
		// Share[i][j]: share of j's valid body points visible from i's eye, computed once per ordered pair.
		TAtomic<int64> Cast(0);
		ParallelFor(End - Begin, [&](int32 n)
		{
			const int32 i = Begin + n;
			int64 Local = 0;
			for (const int32 j : Nbr[i])
			{
				int32 Vis = 0, Cnt = 0;
				for (int32 p = 0; p < K; ++p)
				{
					if (!Valid[j][p]) { continue; }
					++Cnt;
					AActor* Ignored = nullptr;
					Vis += FExposureProbeSolver::Trace(World, Eyes[i], Bodies[j][p], Model, Ignored) ? 1 : 0;
					++Local;
				}
				Share[i].Add(j, Cnt ? float(Vis) / Cnt : 0.f);
			}
			Cast += Local;
		}, bParallel ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread);
		RayCount += Cast.Load();
	}
	Next = End;
	return Next >= N;
}

void FExposureHeatJob::Finish(TArray<FExposureProbeSolver::FPointResult>& Out) const
{
	const int32 N = Points.Num();
	Out.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		double W = 0.0, Vo = 0.0, Vi = 0.0;
		int32 Kept = 0;
		for (const int32 j : Nbr[i])
		{
			const float Out_ij = Share[i].FindRef(j);
			const float In_ij = Share[j].FindRef(i);
			if (Out_ij == 0.f && In_ij == 0.f) { continue; }
			W += Points[j].Weight; Vo += Points[j].Weight * Out_ij; Vi += Points[j].Weight * In_ij; ++Kept;
		}
		FExposureProbeSolver::FPointResult& Res = Out[i];
		Res.Neighbors = Kept;
		Res.Vout = W > 0.0 ? Vo / W : 0.0;
		Res.Vin = W > 0.0 ? Vi / W : 0.0;
		Res.A = Res.Vout - Res.Vin;
		int32 V = 0;
		for (const bool b : Valid[i]) { V += b ? 1 : 0; }
		Res.ValidPoints = V;
	}
}

void FExposureHeatJob::OneWayTargets(float Threshold, TArray<TArray<int32>>& Out) const
{
	const int32 N = Points.Num();
	Out.Reset();
	Out.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		for (const int32 j : Nbr[i])
		{
			if (Share[i].FindRef(j) >= Threshold && Share[j].FindRef(i) == 0.f) { Out[i].Add(j); }
		}
	}
}
