#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ExposureProbeTypes.h"
#include "ExposureSmokeActor.generated.h"

class UBillboardComponent;
class USphereComponent;

/**
 * A smoke placed by hand. The actor location is the landing point; the smoke is a sphere whose centre lies Lift above it.
 * After a move it snaps to the nearest floor cell. Smokes do not block the probe audit; they are evaluated against a
 * selected probe's sightlines (cut share and RT). The wire sphere is the volume the evaluation uses.
 */
UCLASS(HideCategories = (Rendering, Physics, Collision, Input, HLOD, LOD, Cooking, Replication, Networking, Actor, LevelInstance, WorldPartition, DataLayers),
	meta = (DisplayName = "Exposure Smoke"))
class EXPOSUREPROBE_API AExposureSmokeActor : public AActor
{
	GENERATED_BODY()

public:
	AExposureSmokeActor();

	/** Counted when smokes are evaluated, and drawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoke")
	bool bEnabled = true;

	/** Shared size of the settings, a game's smoke, or custom dimensions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoke")
	EExposureSmokeSize Size = EExposureSmokeSize::Shared;

	/** Smoke dimensions in map units. Editing a value switches the size to Custom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoke", meta = (ShowOnlyInnerProperties))
	FExposureSmokeShape CustomSmoke;

	/** This smoke's size in map units. */
	FExposureSmokeShape GetShape() const;
	FString GetSmokeName() const;
	/** Landing point plus the lift, in cm. */
	FVector GetCentre() const;
	float GetRadiusCm() const;
	void RefreshShape();

#if WITH_EDITOR
	virtual void PreEditChange(FProperty* Property) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Smoke")
	TObjectPtr<USphereComponent> Sphere;

	FExposureSmokeShape OldCustomSmoke;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};
