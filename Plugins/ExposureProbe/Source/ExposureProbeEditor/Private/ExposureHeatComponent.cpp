#include "ExposureHeatComponent.h"

#include "DynamicMeshBuilder.h"
#include "Engine/Engine.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveSceneProxy.h"
#include "SceneManagement.h"
#include "SceneView.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ExposureHeatComponent)

namespace
{
	constexpr int32 DiscSides = 10;

	class FExposureHeatSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		FExposureHeatSceneProxy(const UExposureHeatComponent* InComponent)
			: FPrimitiveSceneProxy(InComponent)
		{
			bWillEverBeLit = false;
			bVerifyUsedMaterials = false;
			// Geometry is built once; each frame only copies it into the mesh builder.
			for (const FExposureHeatBucket& B : InComponent->GetBuckets())
			{
				FBucket& Out = Buckets.AddDefaulted_GetRef();
				Out.Color = B.Color;
				if (B.bQuads)
				{
					Out.Vertices.Reserve(B.Centers.Num());
					Out.Indices.Reserve(B.Centers.Num() / 4 * 6);
					for (int32 q = 0; q + 3 < B.Centers.Num(); q += 4)
					{
						const uint32 Base = Out.Vertices.Num();
						for (int32 k = 0; k < 4; ++k) { Out.Vertices.Add(FDynamicMeshVertex(FVector3f(B.Centers[q + k]))); }
						Out.Indices.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
					}
					continue;
				}
				Out.Vertices.Reserve(B.Centers.Num() * (DiscSides + 1));
				Out.Indices.Reserve(B.Centers.Num() * DiscSides * 3);
				for (const FVector& C : B.Centers)
				{
					const uint32 Base = Out.Vertices.Num();
					if (B.bSquare)
					{
						const float R = B.Radius;
						Out.Vertices.Add(FDynamicMeshVertex(FVector3f(C + FVector(-R, -R, 0.f))));
						Out.Vertices.Add(FDynamicMeshVertex(FVector3f(C + FVector(R, -R, 0.f))));
						Out.Vertices.Add(FDynamicMeshVertex(FVector3f(C + FVector(R, R, 0.f))));
						Out.Vertices.Add(FDynamicMeshVertex(FVector3f(C + FVector(-R, R, 0.f))));
						Out.Indices.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
						continue;
					}
					Out.Vertices.Add(FDynamicMeshVertex(FVector3f(C)));
					for (int32 k = 0; k < DiscSides; ++k)
					{
						const float A = 2.f * UE_PI * k / DiscSides;
						Out.Vertices.Add(FDynamicMeshVertex(FVector3f(C + FVector(B.Radius * FMath::Cos(A), B.Radius * FMath::Sin(A), 0.f))));
					}
					for (int32 k = 0; k < DiscSides; ++k)
					{
						Out.Indices.Add(Base);
						Out.Indices.Add(Base + 1 + k);
						Out.Indices.Add(Base + 1 + (k + 1) % DiscSides);
					}
				}
			}
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
		{
			if (!GEngine || !GEngine->DebugMeshMaterial)
			{
				return;
			}
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if (!(VisibilityMap & (1 << ViewIndex)))
				{
					continue;
				}
				for (const FBucket& B : Buckets)
				{
					if (B.Indices.Num() == 0)
					{
						continue;
					}
					FDynamicMeshBuilder Builder(Views[ViewIndex]->GetFeatureLevel());
					Builder.AddVertices(B.Vertices);
					Builder.AddTriangles(B.Indices);
					FColoredMaterialRenderProxy* Mat = new FColoredMaterialRenderProxy(GEngine->DebugMeshMaterial->GetRenderProxy(), B.Color);
					Collector.RegisterOneFrameMaterialProxy(Mat);
					Builder.GetMesh(FMatrix::Identity, Mat, SDPG_World, true, false, ViewIndex, Collector);
				}
			}
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.Editor;
			Result.bDynamicRelevance = true;
			Result.bShadowRelevance = false;
			Result.bNormalTranslucency = true;
			Result.bSeparateTranslucency = true;
			return Result;
		}

		virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

	private:
		struct FBucket
		{
			FLinearColor Color;
			TArray<FDynamicMeshVertex> Vertices;
			TArray<uint32> Indices;
		};
		TArray<FBucket> Buckets;
	};
}

UExposureHeatComponent::UExposureHeatComponent()
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	CastShadow = false;
	bHiddenInGame = true;
	bIsEditorOnly = true;
	bSelectable = false;
	SetCanEverAffectNavigation(false);
}

void UExposureHeatComponent::SetBuckets(TArray<FExposureHeatBucket>&& InBuckets)
{
	Buckets = MoveTemp(InBuckets);
	Box = FBox(ForceInit);
	for (const FExposureHeatBucket& B : Buckets)
	{
		for (const FVector& C : B.Centers) { Box += C; }
		if (Box.IsValid) { Box = Box.ExpandBy(FVector(B.Radius, B.Radius, 10.f)); }
	}
	UpdateBounds();
	MarkRenderStateDirty();
}

FPrimitiveSceneProxy* UExposureHeatComponent::CreateSceneProxy()
{
	return Buckets.Num() > 0 ? new FExposureHeatSceneProxy(this) : nullptr;
}

FBoxSphereBounds UExposureHeatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return Box.IsValid ? FBoxSphereBounds(Box) : FBoxSphereBounds(FVector::ZeroVector, FVector(1.f), 1.f);
}
