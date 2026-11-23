#include "ExposureProbeSettings.h"

#include "ExposureProbeText.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ExposureProbeSettings)

UExposureProbeSettings::UExposureProbeSettings()
{
	ApplyPreset(EExposureUnitPreset::Source);
}

void UExposureProbeSettings::ApplyPreset(EExposureUnitPreset InPreset)
{
	UnitPreset = InPreset;
	if (InPreset != EExposureUnitPreset::Custom)
	{
		const float OldStand = StandHeight, OldCrouch = CrouchHeight, OldUnit = UnitToCm;
		UnitToCm = FExposureBodyShape::PresetUnitToCm(InPreset);
		SetShape(FExposureBodyShape::FromPreset(InPreset, UnitToCm));
		for (float& H : StandSampleHeights) { H *= StandHeight / FMath::Max(OldStand, 1.f); }
		for (float& H : CrouchSampleHeights) { H *= CrouchHeight / FMath::Max(OldCrouch, 1.f); }
		FarDistance = FExposureBodyShape::PresetFarDistance(InPreset);
		// 64 Source units (1.6 m) between samples and the 1000-unit neighbourhood of the full-map A, in this preset's units.
		HeatmapSpacing = FMath::RoundToFloat(162.56f / UnitToCm);
		HeatmapRadius = FMath::RoundToFloat(2540.f / UnitToCm);
		// A crouch-jump reaches about 0.89 of the standing height (64 of 72 units in Source).
		HeatmapMaxClimb = FMath::RoundToFloat(0.89f * StandHeight);
		// Smoke search and movement in Source units (CS2), in this preset's units; the smoke size follows its own preset.
		const float K = 2.54f / UnitToCm;
		if (SmokePreset != EExposureSmokePreset::Custom)
		{
			ApplySmokePreset(SmokePreset);
		}
		else
		{
			SmokeRadius = FMath::RoundToFloat(SmokeRadius * OldUnit / UnitToCm);
			SmokeLift = FMath::RoundToFloat(SmokeLift * OldUnit / UnitToCm);
		}
		SmokeClearance = FMath::RoundToFloat(64.f * K);
		SmokeMinDistance = FMath::RoundToFloat(200.f * K);
		SmokeSearchRadius = FMath::RoundToFloat(2500.f * K);
		SmokeRetreatRadius = FMath::RoundToFloat(2000.f * K);
		SmokeMoveSpeed = FMath::RoundToFloat(113.f * K);
		if (InPreset == EExposureUnitPreset::Source)
		{
			StandSampleHeights = { 6.f, 20.f, 38.f, 66.f };
			CrouchSampleHeights = { 5.f, 15.f, 28.f, 50.f };
		}
	}
	if (Sampling != EExposureSampling::Custom)
	{
		ApplySampling(Sampling);
	}
	SnapshotBody();
}

void UExposureProbeSettings::ApplySmokePreset(EExposureSmokePreset InPreset)
{
	SmokePreset = InPreset;
	if (InPreset == EExposureSmokePreset::Custom)
	{
		return;
	}
	const FExposureSmokeShape S = FExposureSmokeShape::FromPreset(InPreset, UnitToCm);
	SmokeRadius = S.Radius;
	SmokeLift = S.Lift;
	SmokeDuration = S.Duration;
}

FExposureSmokeShape UExposureProbeSettings::GetSmokeShape() const
{
	FExposureSmokeShape S;
	S.Radius = SmokeRadius;
	S.Lift = SmokeLift;
	S.Duration = SmokeDuration;
	return S;
}

FExposureBodyShape UExposureProbeSettings::GetShape() const
{
	FExposureBodyShape S;
	S.StandHeight = StandHeight; S.HeadLength = HeadLength; S.TorsoLength = TorsoLength; S.LegLength = LegLength;
	S.StandEye = StandEye; S.BodyWidth = BodyWidth; S.CrouchHeight = CrouchHeight; S.CrouchEye = CrouchEye;
	return S;
}

void UExposureProbeSettings::SetShape(const FExposureBodyShape& S)
{
	StandHeight = S.StandHeight; HeadLength = S.HeadLength; TorsoLength = S.TorsoLength; LegLength = S.LegLength;
	StandEye = S.StandEye; BodyWidth = S.BodyWidth; CrouchHeight = S.CrouchHeight; CrouchEye = S.CrouchEye;
}

void UExposureProbeSettings::SnapshotBody()
{
	OldShape = GetShape();
}

void UExposureProbeSettings::ApplyBodyEdit(FName Name)
{
	FExposureBodyShape S = GetShape();
	if (S.Equals(OldShape))
	{
		return;
	}
	S.ApplyEdit(Name, OldShape);
	const float KStand = OldShape.StandHeight > 0.f ? S.StandHeight / OldShape.StandHeight : 1.f;
	const float KCrouch = OldShape.CrouchHeight > 0.f ? S.CrouchHeight / OldShape.CrouchHeight : 1.f;
	SetShape(S);
	if (Sampling != EExposureSampling::Custom)
	{
		ApplySampling(Sampling);
	}
	else
	{
		for (float& H : StandSampleHeights) { H = FMath::Clamp(H * KStand, 0.f, StandHeight); }
		for (float& H : CrouchSampleHeights) { H = FMath::Clamp(H * KCrouch, 0.f, CrouchHeight); }
	}
	// Editing a dimension by hand leaves the game preset.
	if (UnitPreset != EExposureUnitPreset::Custom && !GetShape().Equals(FExposureBodyShape::FromPreset(UnitPreset, UnitToCm)))
	{
		UnitPreset = EExposureUnitPreset::Custom;
	}
	SnapshotBody();
}

void UExposureProbeSettings::ApplySampling(EExposureSampling InSampling)
{
	Sampling = InSampling;
	if (InSampling == EExposureSampling::Points16)
	{
		// Feet, knee, waist and head of the Counter-Strike hull (6, 20, 38, 66 of 72), scaled to the body height.
		static const float StandFrac[4] = { 6.f / 72.f, 20.f / 72.f, 38.f / 72.f, 66.f / 72.f };
		static const float CrouchFrac[4] = { 5.f / 54.f, 15.f / 54.f, 28.f / 54.f, 50.f / 54.f };
		StandSampleHeights.Reset(); CrouchSampleHeights.Reset();
		for (int32 i = 0; i < 4; ++i)
		{
			StandSampleHeights.Add(StandFrac[i] * StandHeight);
			CrouchSampleHeights.Add(CrouchFrac[i] * CrouchHeight);
		}
		SampleBearings = 4;
	}
	else if (InSampling == EExposureSampling::Fine)
	{
		StandSampleHeights.Reset(); CrouchSampleHeights.Reset();
		for (int32 i = 0; i < 8; ++i)
		{
			const float T = (i + 0.5f) / 8.f;
			StandSampleHeights.Add(T * StandHeight);
			CrouchSampleHeights.Add(T * CrouchHeight);
		}
		SampleBearings = 8;
	}
}

#if WITH_EDITOR
FText UExposureProbeSettings::GetSectionText() const
{
	return EPT("暴露探针", "Exposure Probe");
}

void UExposureProbeSettings::PreEditChange(FProperty* Property)
{
	Super::PreEditChange(Property);
	SnapshotBody();
}

void UExposureProbeSettings::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	const FName Name = Event.GetMemberPropertyName();
	if (Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, UnitPreset) && UnitPreset != EExposureUnitPreset::Custom)
	{
		ApplyPreset(UnitPreset);
	}
	else if (Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, Sampling) && Sampling != EExposureSampling::Custom)
	{
		ApplySampling(Sampling);
	}
	else if (Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, SmokePreset))
	{
		ApplySmokePreset(SmokePreset);
	}
	else if ((Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, SmokeRadius) || Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, SmokeLift)
		|| Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, SmokeDuration)) && SmokePreset != EExposureSmokePreset::Custom
		&& !GetSmokeShape().Equals(FExposureSmokeShape::FromPreset(SmokePreset, UnitToCm)))
	{
		// Editing a size by hand leaves the game preset.
		SmokePreset = EExposureSmokePreset::Custom;
	}
	else if (Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, StandSampleHeights) || Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, CrouchSampleHeights)
		|| Name == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, SampleBearings))
	{
		Sampling = EExposureSampling::Custom;
	}
	// Interactive drags fire several changes; each one moves the body from the previous step.
	ApplyBodyEdit(Name);

	Super::PostEditChangeProperty(Event);
	if (Event.ChangeType != EPropertyChangeType::Interactive)
	{
		TryUpdateDefaultConfigFile();
	}
}

bool UExposureProbeSettings::CanEditChange(const FProperty* Property) const
{
	if (Property && Property->GetFName() == GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, UnitToCm))
	{
		return Super::CanEditChange(Property) && UnitPreset == EExposureUnitPreset::Custom;
	}
	return Super::CanEditChange(Property);
}
#endif
