#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "ExposureHeatComponent.generated.h"

/** Shapes of one colour; the floor layers draw one mesh per bucket so the colours need no material of their own. */
struct FExposureHeatBucket
{
	FLinearColor Color = FLinearColor::White;
	/** Disc radius, or half the side of a square. */
	float Radius = 50.f;
	/** Squares (grid cells) instead of discs. */
	bool bSquare = false;
	/** Free quads: Centers holds four corners per quad, counter-clockwise seen from above. */
	bool bQuads = false;
	TArray<FVector> Centers;
};

/** Translucent shapes on the floor: the full-map A surface and the one-way cells. Editor only, no collision, not part of any actor. */
UCLASS(Transient)
class UExposureHeatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UExposureHeatComponent();

	void SetBuckets(TArray<FExposureHeatBucket>&& InBuckets);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

	const TArray<FExposureHeatBucket>& GetBuckets() const { return Buckets; }

private:
	TArray<FExposureHeatBucket> Buckets;
	FBox Box = FBox(ForceInit);
};
