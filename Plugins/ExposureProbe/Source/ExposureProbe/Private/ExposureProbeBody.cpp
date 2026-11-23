#include "ExposureProbeTypes.h"

namespace
{
	struct FPresetDef
	{
		float UnitToCm, Height, Head, Torso, Legs, Eye, Width, CrouchHeight, CrouchEye, Far;
	};

	FPresetDef Def(EExposureUnitPreset Preset)
	{
		switch (Preset)
		{
		case EExposureUnitPreset::CallOfDuty:
			// Radiant gameplay standards: hull 72 x 32, crouch clearance 52; view heights 60 / 40 (bg_viewheight_standing / crouched).
			return { 2.54f, 72.f, 14.f, 24.f, 34.f, 60.f, 32.f, 52.f, 40.f, 500.f };
		case EExposureUnitPreset::Valorant:
			// Community estimate (standing about 180, crouching about 140); eye height and width are not published.
			return { 1.f, 180.f, 26.f, 66.f, 88.f, 166.f, 68.f, 140.f, 126.f, 1270.f };
		case EExposureUnitPreset::UnrealTemplate:
			// First-person template capsule (radius 34, half height 96); APawn::BaseEyeHeight 64 above the capsule centre;
			// crouched half height 40 with the eyes at 0.8 of it above the centre.
			return { 1.f, 192.f, 34.f, 66.f, 92.f, 160.f, 68.f, 80.f, 72.f, 1270.f };
		case EExposureUnitPreset::Unreal:
			return { 1.f, 180.f, 28.f, 67.f, 85.f, 167.f, 80.f, 135.f, 120.f, 1270.f };
		default:
			// Counter-Strike player hull (Valve Mapper's Reference): 72 x 32, eye 64, crouching 54 / eye 46.
			return { 2.54f, 72.f, 12.f, 26.f, 34.f, 64.f, 32.f, 54.f, 46.f, 500.f };
		}
	}
}

float FExposureBodyShape::PresetUnitToCm(EExposureUnitPreset Preset)
{
	return Def(Preset).UnitToCm;
}

float FExposureBodyShape::PresetFarDistance(EExposureUnitPreset Preset)
{
	return Def(Preset).Far;
}

FExposureBodyShape FExposureBodyShape::FromPreset(EExposureUnitPreset Preset, float UnitToCm)
{
	const FPresetDef D = Def(Preset);
	FExposureBodyShape S;
	S.StandHeight = D.Height;
	S.HeadLength = D.Head;
	S.TorsoLength = D.Torso;
	S.LegLength = D.Legs;
	S.StandEye = D.Eye;
	S.BodyWidth = D.Width;
	S.CrouchHeight = D.CrouchHeight;
	S.CrouchEye = D.CrouchEye;
	S.Scale(D.UnitToCm / FMath::Max(UnitToCm, 0.01f));
	return S;
}

void FExposureBodyShape::Scale(float K)
{
	StandHeight *= K;
	HeadLength *= K;
	TorsoLength *= K;
	LegLength *= K;
	StandEye *= K;
	BodyWidth *= K;
	CrouchHeight *= K;
	CrouchEye *= K;
}

void FExposureBodyShape::ApplyEdit(FName Name, const FExposureBodyShape& Old)
{
	const bool bHeight = Name == GET_MEMBER_NAME_CHECKED(FExposureBodyShape, StandHeight);
	const bool bSegment = Name == GET_MEMBER_NAME_CHECKED(FExposureBodyShape, HeadLength)
		|| Name == GET_MEMBER_NAME_CHECKED(FExposureBodyShape, TorsoLength)
		|| Name == GET_MEMBER_NAME_CHECKED(FExposureBodyShape, LegLength);
	const bool bCrouch = Name == GET_MEMBER_NAME_CHECKED(FExposureBodyShape, CrouchHeight);

	if (bHeight && Old.StandHeight > 0.f)
	{
		// The height scales head, torso, legs and eye together.
		const float K = StandHeight / Old.StandHeight;
		HeadLength = Old.HeadLength * K;
		TorsoLength = Old.TorsoLength * K;
		LegLength = Old.LegLength * K;
		StandEye = Old.StandEye * K;
		CrouchHeight = Old.CrouchHeight * K;
		CrouchEye = Old.CrouchEye * K;
	}
	else if (bSegment)
	{
		// A segment adds to the height; the eye keeps its place within the head.
		StandHeight = FMath::Max(1.f, HeadLength + TorsoLength + LegLength);
		StandEye = Old.HeadLength > 0.f
			? Neck() + (Old.StandEye - Old.Neck()) / Old.HeadLength * HeadLength
			: StandHeight - (Old.StandHeight - Old.StandEye);
		if (Old.StandHeight > 0.f)
		{
			const float K = StandHeight / Old.StandHeight;
			CrouchHeight = Old.CrouchHeight * K;
			CrouchEye = Old.CrouchEye * K;
		}
	}
	else if (bCrouch && Old.CrouchHeight > 0.f)
	{
		CrouchEye = Old.CrouchEye * CrouchHeight / Old.CrouchHeight;
	}
	Clamp();
}

void FExposureBodyShape::Clamp()
{
	HeadLength = FMath::Max(0.f, HeadLength);
	TorsoLength = FMath::Max(0.f, TorsoLength);
	LegLength = FMath::Max(0.f, LegLength);
	StandHeight = FMath::Max(1.f, StandHeight);
	StandEye = FMath::Clamp(StandEye, 0.f, StandHeight);
	BodyWidth = FMath::Max(0.f, BodyWidth);
	CrouchHeight = FMath::Clamp(CrouchHeight, 1.f, StandHeight);
	CrouchEye = FMath::Clamp(CrouchEye, 0.f, CrouchHeight);
}

bool FExposureBodyShape::Equals(const FExposureBodyShape& O) const
{
	return FMath::IsNearlyEqual(StandHeight, O.StandHeight) && FMath::IsNearlyEqual(HeadLength, O.HeadLength)
		&& FMath::IsNearlyEqual(TorsoLength, O.TorsoLength) && FMath::IsNearlyEqual(LegLength, O.LegLength)
		&& FMath::IsNearlyEqual(StandEye, O.StandEye) && FMath::IsNearlyEqual(BodyWidth, O.BodyWidth)
		&& FMath::IsNearlyEqual(CrouchHeight, O.CrouchHeight) && FMath::IsNearlyEqual(CrouchEye, O.CrouchEye);
}

EExposureUnitPreset ToUnitPreset(EExposureProbeBody Body)
{
	switch (Body)
	{
	case EExposureProbeBody::Source: return EExposureUnitPreset::Source;
	case EExposureProbeBody::CallOfDuty: return EExposureUnitPreset::CallOfDuty;
	case EExposureProbeBody::Valorant: return EExposureUnitPreset::Valorant;
	case EExposureProbeBody::UnrealTemplate: return EExposureUnitPreset::UnrealTemplate;
	case EExposureProbeBody::Unreal: return EExposureUnitPreset::Unreal;
	default: return EExposureUnitPreset::Custom;
	}
}
