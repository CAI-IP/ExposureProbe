#include "ExposureSmokeActor.h"

#include "ExposureProbeSettings.h"
#include "Components/BillboardComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ExposureSmokeActor)

namespace
{
	struct FSmokeDef
	{
		/** Radius and centre height in cm, duration in seconds. */
		float RadiusCm, LiftCm, Duration;
	};

	FSmokeDef SmokeDef(EExposureSmokePreset Preset)
	{
		// The Valorant smokes have no published centre height; it takes the same share of the radius as CS2 (60 of 144).
		constexpr float LiftShare = 60.f / 144.f;
		switch (Preset)
		{
		case EExposureSmokePreset::ValorantBrimstone: return { 410.f, 410.f * LiftShare, 19.25f };
		case EExposureSmokePreset::ValorantOmen: return { 400.f, 400.f * LiftShare, 15.f };
		case EExposureSmokePreset::ValorantAstra: return { 475.f, 475.f * LiftShare, 14.25f };
		default:
			// CS2: 144 x 60 Source units, 22.06 s.
			return { 144.f * 2.54f, 60.f * 2.54f, 22.06f };
		}
	}

	float Round1(float V) { return FMath::RoundToFloat(V * 10.f) / 10.f; }
}

FExposureSmokeShape FExposureSmokeShape::FromPreset(EExposureSmokePreset Preset, float UnitToCm)
{
	const FSmokeDef D = SmokeDef(Preset);
	const float U = FMath::Max(UnitToCm, 0.01f);
	FExposureSmokeShape S;
	S.Radius = Round1(D.RadiusCm / U);
	S.Lift = Round1(D.LiftCm / U);
	S.Duration = D.Duration;
	return S;
}

bool FExposureSmokeShape::Equals(const FExposureSmokeShape& O) const
{
	return FMath::IsNearlyEqual(Radius, O.Radius, 0.05f) && FMath::IsNearlyEqual(Lift, O.Lift, 0.05f) && FMath::IsNearlyEqual(Duration, O.Duration, 0.005f);
}

EExposureSmokePreset ToSmokePreset(EExposureSmokeSize Size)
{
	switch (Size)
	{
	case EExposureSmokeSize::CS2: return EExposureSmokePreset::CS2;
	case EExposureSmokeSize::ValorantBrimstone: return EExposureSmokePreset::ValorantBrimstone;
	case EExposureSmokeSize::ValorantOmen: return EExposureSmokePreset::ValorantOmen;
	case EExposureSmokeSize::ValorantAstra: return EExposureSmokePreset::ValorantAstra;
	default: return EExposureSmokePreset::Custom;
	}
}

AExposureSmokeActor::AExposureSmokeActor()
{
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Landing"));
	RootComponent = Root;

	Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("Sphere"));
	Sphere->SetupAttachment(Root);
	Sphere->SetUsingAbsoluteRotation(true);
	Sphere->SetUsingAbsoluteScale(true);
	Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sphere->SetGenerateOverlapEvents(false);
	Sphere->SetCanEverAffectNavigation(false);
	Sphere->ShapeColor = FColor(150, 150, 158);
	Sphere->bDrawOnlyIfSelected = true;
	Sphere->bHiddenInGame = true;
	Sphere->bIsEditorOnly = true;

#if WITH_EDITORONLY_DATA
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite && !IsRunningCommandlet())
	{
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> Icon(TEXT("/Engine/EditorResources/S_Emitter"));
		Sprite->Sprite = Icon.Get();
		Sprite->SetRelativeScale3D_Direct(FVector(0.4f));
		Sprite->bIsScreenSizeScaled = true;
		Sprite->SetupAttachment(Root);
	}
#endif

	CustomSmoke = FExposureSmokeShape::FromPreset(EExposureSmokePreset::CS2, 2.54f);
	SetActorEnableCollision(false);
	SetCanBeDamaged(false);
	SetHidden(true);
	bIsEditorOnlyActor = true;
}

FExposureSmokeShape AExposureSmokeActor::GetShape() const
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	FExposureSmokeShape Out;
	switch (Size)
	{
	case EExposureSmokeSize::Shared: Out = S->GetSmokeShape(); break;
	case EExposureSmokeSize::Custom: Out = CustomSmoke; break;
	// Game smokes follow the map units, so a change of units keeps them right.
	default: Out = FExposureSmokeShape::FromPreset(ToSmokePreset(Size), S->UnitToCm); break;
	}
	Out.Radius = FMath::Max(Out.Radius, 1.f);
	Out.Lift = FMath::Max(Out.Lift, 0.f);
	Out.Duration = FMath::Max(Out.Duration, 0.f);
	return Out;
}

FString AExposureSmokeActor::GetSmokeName() const
{
#if WITH_EDITOR
	return GetActorLabel();
#else
	return GetName();
#endif
}

FVector AExposureSmokeActor::GetCentre() const
{
	return GetActorLocation() + FVector(0.f, 0.f, GetShape().Lift * GetDefault<UExposureProbeSettings>()->UnitToCm);
}

float AExposureSmokeActor::GetRadiusCm() const
{
	return GetShape().Radius * GetDefault<UExposureProbeSettings>()->UnitToCm;
}

void AExposureSmokeActor::RefreshShape()
{
	const FExposureSmokeShape Shape = GetShape();
	if (Size != EExposureSmokeSize::Custom)
	{
		CustomSmoke = Shape;
	}
	if (Sphere)
	{
		const float U = GetDefault<UExposureProbeSettings>()->UnitToCm;
		Sphere->SetRelativeLocation(FVector(0.f, 0.f, Shape.Lift * U));
		Sphere->SetSphereRadius(Shape.Radius * U, false);
	}
}

void AExposureSmokeActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshShape();
}

#if WITH_EDITOR
void AExposureSmokeActor::PreEditChange(FProperty* Property)
{
	Super::PreEditChange(Property);
	OldCustomSmoke = CustomSmoke;
}

void AExposureSmokeActor::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	if (Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(AExposureSmokeActor, CustomSmoke) && !CustomSmoke.Equals(OldCustomSmoke))
	{
		Size = EExposureSmokeSize::Custom;
	}
	OldCustomSmoke = CustomSmoke;
	Super::PostEditChangeProperty(Event);
	RefreshShape();
}
#endif
