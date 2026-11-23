#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "ExposureProbeTypes.h"
#include "ExposureProbeActor.generated.h"

class UBillboardComponent;

/**
 * Body drawn in the editor: legs, torso and head outlines and an eye marker. A region seen by the other side is
 * filled in that side's colour, brighter the larger its visible share. Sample points are shown with the ray view.
 */
UCLASS(ClassGroup = (LevelDesign), meta = (BlueprintSpawnableComponent))
class EXPOSUREPROBE_API UExposureProbeBodyComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UExposureProbeBodyComponent();

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

	UPROPERTY(Transient)
	bool bCrouch = false;

	UPROPERTY(Transient)
	bool bSkipped = false;

	UPROPERTY(Transient)
	FLinearColor Color = FLinearColor::White;

	/** Visible share of the legs, torso and head as seen by the other side; negative when not shown. */
	float RegionShare[3] = { -1.f, -1.f, -1.f };
	FLinearColor SeenByColor = FLinearColor::White;
	/** Drawn faded when other probes are selected and this one takes no part. */
	bool bFaded = false;
	bool bShowPoints = false;
	/** Own body of the probe; the shared body when false. */
	bool bOwnShape = false;
	FExposureBodyShape Shape;
};

/**
 * A paired exposure probe. The actor location is the feet position. Visibility is measured from this
 * probe's eye to the sample points on the other probe's body, using the body model in the plugin settings.
 * Probes have no collision, so they never block the rays they measure.
 */
UCLASS(Blueprintable, ClassGroup = (LevelDesign), HideCategories = (Rendering, Physics, Collision, Input, HLOD, LOD, Cooking, Replication, Networking, Actor, LevelInstance, WorldPartition, DataLayers),
	meta = (DisplayName = "Exposure Probe"))
class EXPOSUREPROBE_API AExposureProbeActor : public AActor
{
	GENERATED_BODY()

public:
	AExposureProbeActor();

	/** Name used in pair keys such as "T0-CT3". Falls back to the actor label when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure Probe")
	FString ProbeName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure Probe")
	EExposureTeam Team = EExposureTeam::TSide;

	/** Auto picks stand or crouch from the head clearance above the feet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure Probe")
	EExposurePose Pose = EExposurePose::Auto;

	/** Pairs with these CT probes are kept on purpose (T probes only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure Probe")
	TArray<FExposureDesignMark> DesignIntent;

	/** Probes of the other side whose links with this probe are not drawn. Display only; the audit still covers them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exposure Probe")
	TArray<FString> HiddenLinks;

	bool IsLinkShown(const FString& Other) const { return !HiddenLinks.Contains(Other); }

	/** Shared body, a game's body, or custom dimensions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body")
	EExposureProbeBody BodyPreset = EExposureProbeBody::Global;

	/** Body dimensions in map units. Editing a value switches the body to Custom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body", meta = (ShowOnlyInnerProperties))
	FExposureBodyShape CustomBody;

	/** This probe's body in map units; false when it uses the shared body. */
	bool GetOwnBody(FExposureBodyShape& Out) const;

	UFUNCTION(BlueprintPure, Category = "Exposure Probe")
	FString GetProbeName() const;

	/** Pose and colour shown by the body wireframe; set by the audit after each update. */
	void SetDisplayState(bool bCrouch, bool bSkipped);
	/** Region fill: shares of legs, torso and head seen by the other side (negative = none). */
	void SetExposure(const float InShare[3], const FLinearColor& InSeenBy, bool bInFaded, bool bInShowPoints);
	void RefreshBody();

	UExposureProbeBodyComponent* GetBody() const { return Body; }

#if WITH_EDITOR
	virtual void PreEditChange(FProperty* Property) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Exposure Probe")
	TObjectPtr<UExposureProbeBodyComponent> Body;

	FExposureBodyShape OldCustomBody;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};

UCLASS(meta = (DisplayName = "T Probe"))
class EXPOSUREPROBE_API AExposureProbeT : public AExposureProbeActor
{
	GENERATED_BODY()

public:
	AExposureProbeT() { Team = EExposureTeam::TSide; }
};

UCLASS(meta = (DisplayName = "CT Probe"))
class EXPOSUREPROBE_API AExposureProbeCT : public AExposureProbeActor
{
	GENERATED_BODY()

public:
	AExposureProbeCT() { Team = EExposureTeam::CTSide; }
};
