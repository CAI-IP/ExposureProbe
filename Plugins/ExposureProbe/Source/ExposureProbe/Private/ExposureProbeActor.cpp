#include "ExposureProbeActor.h"

#include "ExposureProbeSettings.h"
#include "ExposureProbeSolver.h"
#include "Components/BillboardComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Engine/Texture2D.h"
#include "PrimitiveDrawingUtils.h"
#include "PrimitiveSceneProxy.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ExposureProbeActor)

namespace
{
	class FExposureBodySceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		FExposureBodySceneProxy(const UExposureProbeBodyComponent* InComponent)
			: FPrimitiveSceneProxy(InComponent)
			, Model(InComponent->bOwnShape ? FExposureBodyModel::FromSettings().WithShape(InComponent->Shape) : FExposureBodyModel::FromSettings())
			, bCrouch(InComponent->bCrouch)
			, bSkipped(InComponent->bSkipped)
			, bFaded(InComponent->bFaded)
			, bShowPoints(InComponent->bShowPoints)
			, Color(InComponent->Color)
			, SeenBy(InComponent->SeenByColor)
		{
			bWillEverBeLit = false;
			for (int32 r = 0; r < 3; ++r) { Share[r] = InComponent->RegionShare[r]; }
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
		{
			const FVector Feet = GetLocalToWorld().GetOrigin();
			const float U = Model.UnitToCm;
			const float R = Model.Radius * U;
			// Legs, torso, head from the feet up, with a small gap so the three parts read apart.
			const float Levels[4] = { 0.f, Model.CrotchUnits(bCrouch) * U, Model.NeckUnits(bCrouch) * U, Model.TopUnits(bCrouch) * U };
			const float Gap = FMath::Min(2.f, 0.1f * R);
			const float Eye = Model.EyeCm(bCrouch);
			FLinearColor Main = bSkipped ? FLinearColor(0.35f, 0.35f, 0.35f) : Color;
			if (bFaded) { Main = FMath::Lerp(Main, FLinearColor(0.2f, 0.2f, 0.2f), 0.7f); }

			TArray<FVector> Points;
			if (bShowPoints) { Model.BodyPoints(Feet, bCrouch, Points); }

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if (!(VisibilityMap & (1 << ViewIndex)))
				{
					continue;
				}
				FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
				const uint8 Depth = SDPG_World;
				for (int32 r = 0; r < 3; ++r)
				{
					const float Z0 = Levels[r] + (r > 0 ? Gap * 0.5f : 0.f);
					const float Z1 = Levels[r + 1] - (r < 2 ? Gap * 0.5f : 0.f);
					if (Z1 <= Z0)
					{
						continue;
					}
					const float Thick = r == 2 ? 1.6f : 1.0f;
					DrawCircle(PDI, Feet + FVector(0, 0, Z0), FVector::ForwardVector, FVector::RightVector, Main, R, 24, Depth, Thick);
					DrawCircle(PDI, Feet + FVector(0, 0, Z1), FVector::ForwardVector, FVector::RightVector, Main, R, 24, Depth, Thick);
					for (int32 k = 0; k < 4; ++k)
					{
						const float A = UE_HALF_PI * k + 0.25f * UE_PI;
						const FVector Off(R * FMath::Cos(A), R * FMath::Sin(A), 0.f);
						PDI->DrawLine(Feet + Off + FVector(0, 0, Z0), Feet + Off + FVector(0, 0, Z1), Main, Depth, 0.8f);
					}
					if (Share[r] > 0.f && !bSkipped)
					{
						// Filled in the observer's colour; opacity grows with the visible share of this region.
						FLinearColor Fill = SeenBy;
						Fill.A = (bFaded ? 0.5f : 1.f) * (0.18f + 0.6f * FMath::Clamp(Share[r], 0.f, 1.f));
						FColoredMaterialRenderProxy* Mat = new FColoredMaterialRenderProxy(GEngine->DebugMeshMaterial->GetRenderProxy(), Fill);
						Collector.RegisterOneFrameMaterialProxy(Mat);
						// GetCylinderMesh takes the view index; DrawCylinder reads PDI->View, which is null for a collector PDI.
						GetCylinderMesh(Feet + FVector(0, 0, Z0), Feet + FVector(0, 0, Z1), R * 0.98f, 24, Mat, Depth, ViewIndex, Collector);
					}
				}
				// Eye: a short sight cross at eye height.
				const FVector E = Feet + FVector(0, 0, Eye);
				const float S = 0.45f * R;
				PDI->DrawLine(E - FVector(S, 0, 0), E + FVector(S, 0, 0), Main, Depth, 2.f);
				PDI->DrawLine(E - FVector(0, S, 0), E + FVector(0, S, 0), Main, Depth, 2.f);
				for (const FVector& P : Points)
				{
					PDI->DrawPoint(P, Main, 5.f, Depth);
				}
			}
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.Editor;
			Result.bDynamicRelevance = true;
			Result.bShadowRelevance = false;
			Result.bOpaque = true;
			Result.bNormalTranslucency = true;
			Result.bSeparateTranslucency = true;
			return Result;
		}

		virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

	private:
		FExposureBodyModel Model;
		bool bCrouch;
		bool bSkipped;
		bool bFaded;
		bool bShowPoints;
		FLinearColor Color;
		FLinearColor SeenBy;
		float Share[3];
	};
}

UExposureProbeBodyComponent::UExposureProbeBodyComponent()
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	CastShadow = false;
	bHiddenInGame = true;
	bIsEditorOnly = true;
	SetCanEverAffectNavigation(false);
}

FPrimitiveSceneProxy* UExposureProbeBodyComponent::CreateSceneProxy()
{
	return new FExposureBodySceneProxy(this);
}

FBoxSphereBounds UExposureProbeBodyComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FExposureBodyModel M = bOwnShape ? FExposureBodyModel::FromSettings().WithShape(Shape) : FExposureBodyModel::FromSettings();
	const float R = M.Radius * M.UnitToCm + 10.f;
	const float H = M.StandHeight * M.UnitToCm + 10.f;
	const FBox Local(FVector(-R, -R, -10.f), FVector(R, R, H));
	return FBoxSphereBounds(Local.ShiftBy(LocalToWorld.GetLocation()));
}

AExposureProbeActor::AExposureProbeActor()
{
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Feet"));
	RootComponent = Root;

	Body = CreateDefaultSubobject<UExposureProbeBodyComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	Body->SetUsingAbsoluteRotation(true);
	Body->SetUsingAbsoluteScale(true);

#if WITH_EDITORONLY_DATA
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite && !IsRunningCommandlet())
	{
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> Icon(TEXT("/Engine/EditorResources/S_TargetPoint"));
		Sprite->Sprite = Icon.Get();
		Sprite->SetRelativeScale3D_Direct(FVector(0.35f));
		Sprite->bIsScreenSizeScaled = true;
		Sprite->SetupAttachment(Root);
	}
#endif

	SetActorEnableCollision(false);
	SetCanBeDamaged(false);
	SetHidden(true);
	bIsEditorOnlyActor = true;
}

FString AExposureProbeActor::GetProbeName() const
{
	if (!ProbeName.IsEmpty())
	{
		return ProbeName;
	}
#if WITH_EDITOR
	return GetActorLabel();
#else
	return GetName();
#endif
}

bool AExposureProbeActor::GetOwnBody(FExposureBodyShape& Out) const
{
	if (BodyPreset == EExposureProbeBody::Global)
	{
		return false;
	}
	// Game presets follow the map units, so a change of units keeps the body right.
	Out = BodyPreset == EExposureProbeBody::Custom ? CustomBody
		: FExposureBodyShape::FromPreset(ToUnitPreset(BodyPreset), GetDefault<UExposureProbeSettings>()->UnitToCm);
	Out.Clamp();
	return true;
}

void AExposureProbeActor::RefreshBody()
{
	const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
	FExposureBodyShape Own;
	const bool bOwn = GetOwnBody(Own);
	if (bOwn && BodyPreset != EExposureProbeBody::Custom)
	{
		CustomBody = Own;
	}
	else if (!bOwn)
	{
		CustomBody = S->GetShape();
	}
	if (Body)
	{
		Body->bOwnShape = bOwn;
		Body->Shape = Own;
		Body->Color = Team == EExposureTeam::TSide ? S->TColor : S->CTColor;
		Body->UpdateBounds();
		Body->MarkRenderStateDirty();
	}
#if WITH_EDITORONLY_DATA
	if (Sprite)
	{
		const FExposureBodyModel M = bOwn ? FExposureBodyModel::FromSettings().WithShape(Own) : FExposureBodyModel::FromSettings();
		Sprite->SetRelativeLocation(FVector(0.f, 0.f, M.EyeCm(Body && Body->bCrouch)));
	}
#endif
}

void AExposureProbeActor::SetDisplayState(bool bCrouch, bool bSkipped)
{
	if (Body && (Body->bCrouch != bCrouch || Body->bSkipped != bSkipped))
	{
		Body->bCrouch = bCrouch;
		Body->bSkipped = bSkipped;
	}
	RefreshBody();
}

void AExposureProbeActor::SetExposure(const float InShare[3], const FLinearColor& InSeenBy, bool bInFaded, bool bInShowPoints)
{
	if (!Body)
	{
		return;
	}
	bool bChanged = Body->bFaded != bInFaded || Body->bShowPoints != bInShowPoints || !Body->SeenByColor.Equals(InSeenBy);
	for (int32 r = 0; r < 3; ++r)
	{
		bChanged |= !FMath::IsNearlyEqual(Body->RegionShare[r], InShare[r]);
		Body->RegionShare[r] = InShare[r];
	}
	Body->SeenByColor = InSeenBy;
	Body->bFaded = bInFaded;
	Body->bShowPoints = bInShowPoints;
	if (bChanged)
	{
		Body->MarkRenderStateDirty();
	}
}

void AExposureProbeActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshBody();
}

#if WITH_EDITOR
void AExposureProbeActor::PreEditChange(FProperty* Property)
{
	Super::PreEditChange(Property);
	OldCustomBody = CustomBody;
}

void AExposureProbeActor::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	if (Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(AExposureProbeActor, CustomBody) && !CustomBody.Equals(OldCustomBody))
	{
		// Height scales head, torso, legs and eye; a segment changes the height. Any edit leaves the game preset.
		CustomBody.ApplyEdit(Event.GetPropertyName(), OldCustomBody);
		BodyPreset = EExposureProbeBody::Custom;
	}
	OldCustomBody = CustomBody;
	Super::PostEditChangeProperty(Event);
	RefreshBody();
}
#endif
