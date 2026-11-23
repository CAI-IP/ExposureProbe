#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "ExposureProbeTypes.h"
#include "ExposureSmokeComponent.generated.h"

class UMaterial;

/** One smoke: the sphere the audit uses. */
struct FExposureSmokeCloud
{
	FVector Centre = FVector::ZeroVector;
	float Radius = 100.f;
	/** FPlatformTime seconds when the smoke appeared; it swells in from then. */
	double Born = 0.0;
	/** Varies the noise and the puffs between smokes. */
	uint32 Seed = 0;
};

/**
 * Smoke drawn in the editor, not part of any actor and without collision. Volume: a ray-marched cloud with 3D noise
 * inside each sphere, cut off where walls and floors are in front. Puffs: translucent spheres that drift and breathe.
 * The volume material compiles on first use; until it is ready the puffs are drawn.
 */
UCLASS(Transient)
class UExposureSmokeComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UExposureSmokeComponent();

	void SetClouds(TArray<FExposureSmokeCloud>&& InClouds, EExposureSmokeStyle InStyle, float InDensity, const FLinearColor& InColor);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

	const TArray<FExposureSmokeCloud>& GetClouds() const { return Clouds; }
	EExposureSmokeStyle GetStyle() const { return Style; }
	float GetDensity() const { return Density; }
	const FLinearColor& GetColor() const { return Color; }
	/** The proxy drew puffs while the volume material compiled, and the material is ready now. */
	bool NeedsVolumeRefresh() const;

	/** The ray-marching material, created and compiled on first use; null when it failed to compile. */
	static UMaterial* VolumeMaterial();
	static bool IsVolumeReady();

	/** Set by the proxy when it fell back to puffs. */
	mutable bool bVolumeFallback = false;

private:
	TArray<FExposureSmokeCloud> Clouds;
	EExposureSmokeStyle Style = EExposureSmokeStyle::Volume;
	float Density = 4.f;
	FLinearColor Color = FLinearColor(0.62f, 0.62f, 0.65f);
	FBox Box = FBox(ForceInit);
};
