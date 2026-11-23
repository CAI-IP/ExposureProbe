#include "ExposureSmokeComponent.h"

#include "ExposureProbeTypes.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionSceneDepth.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialRelevance.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveDrawingUtils.h"
#include "PrimitiveSceneProxy.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ExposureSmokeComponent)

namespace ExposureSmokeVolume
{
	/** Rooted, so it lives for the whole editor session. */
	static UMaterial* Material = nullptr;
	static bool bTried = false;
	static bool bFailed = false;
	static const FName SphereName(TEXT("SmokeSphere"));
	static const FName ParamsName(TEXT("SmokeParams"));
	static const FName ColorName(TEXT("SmokeColor"));
	/** A new smoke swells to full size over this time. */
	static constexpr double GrowSeconds = 1.6;

	/**
	 * Ray march through one smoke sphere. Rel and CamRel are the pixel and the camera relative to the sphere centre (cm).
	 * Only the far side of the sphere mesh draws, so each pixel marches once with the camera outside or inside it.
	 * The scene depth ends the march at the first wall or floor in front; the cloud fades out just before it.
	 * P = (density, growth 0-1, noise seed, time in s).
	 */
	static const TCHAR* Code = TEXT(R"(
float3 rd = Rel - CamRel;
float tPix = length(rd);
rd /= max(tPix, 0.001);
if (dot(Rel, rd) < 0.0) { return float4(0, 0, 0, 0); }
float R = max(Radius * P.y, 1.0);
// The audit blocks every ray within R, so the cloud is opaque out to R; a thin noisy fringe reaches a little beyond.
float Rm = 1.08 * R;
float b = dot(CamRel, rd);
float disc = b * b - (dot(CamRel, CamRel) - Rm * Rm);
if (disc <= 0.0) { return float4(0, 0, 0, 0); }
disc = sqrt(disc);
float t0 = max(-b - disc, 0.0);
float tScene = SceneD * tPix / max(PixelD, 0.001);
float t1 = min(-b + disc, tScene);
if (t1 <= t0) { return float4(0, 0, 0, 0); }
const int STEPS = 40;
float dt = (t1 - t0) / STEPS;
float jitter = frac(52.9829189 * frac(dot(Parameters.SvPosition.xy, float2(0.06711056, 0.00583715))));
float sigma = 1.5 * P.x / R;
float3 seed = float3(P.z, P.z * 1.7, P.z * 2.3);
float T = 1.0;
float3 acc = float3(0, 0, 0);
[loop]
for (int i = 0; i < STEPS; i++)
{
	float t = t0 + (i + jitter) * dt;
	float3 p = (CamRel + rd * t) / R;
	float r = length(p);
	if (r >= 1.08) { continue; }
	float3 q = p * 1.9 + seed + float3(0.0, 0.0, -P.w * 0.07);
	q += 0.25 * sin(q.yzx * 2.1 + P.w * 0.31);
	float n = 0.0;
	float amp = 0.5;
	[unroll]
	for (int o = 0; o < 3; o++)
	{
		float3 ip = floor(q);
		ip -= 256.0 * floor(ip / 256.0);
		float3 fp = frac(q);
		fp = fp * fp * (3.0 - 2.0 * fp);
		float k = dot(ip, float3(1.0, 57.0, 113.0));
		float4 h0 = frac(sin(float4(k, k + 1.0, k + 57.0, k + 58.0)) * 43758.5453);
		float4 h1 = frac(sin(float4(k + 113.0, k + 114.0, k + 170.0, k + 171.0)) * 43758.5453);
		float4 hz = lerp(h0, h1, fp.z);
		float2 hy = lerp(hz.xy, hz.zw, fp.y);
		n += amp * lerp(hy.x, hy.y, fp.x);
		q = q * 2.03 + 17.1;
		amp *= 0.5;
	}
	n /= 0.875;
	float d = saturate((1.0 - r) * 7.0 + 0.35 + (n - 0.5) * 1.6);
	d *= saturate((tScene - t) / (0.12 * R));
	if (d <= 0.001) { continue; }
	float a = 1.0 - exp(-d * sigma * dt);
	float shade = lerp(0.6, 1.08, saturate(p.z * 0.5 + 0.55)) * (0.82 + 0.3 * n);
	acc += T * a * Col * shade;
	T *= 1.0 - a;
	if (T < 0.01) { break; }
}
float alpha = 1.0 - T;
return float4(acc / max(alpha, 0.0001), alpha);
)");

	/** The volume material with one smoke's sphere, parameters and colour. */
	class FSmokeMaterialProxy final : public FMaterialRenderProxy
	{
	public:
		FSmokeMaterialProxy(const FMaterialRenderProxy* InParent, const FLinearColor& InSphere, const FLinearColor& InParams, const FLinearColor& InColor)
			: FMaterialRenderProxy(InParent->GetMaterialName())
			, Parent(InParent), Sphere(InSphere), Params(InParams), Color(InColor)
		{
		}

		virtual const FMaterial* GetMaterialNoFallback(ERHIFeatureLevel::Type InFeatureLevel) const override { return Parent->GetMaterialNoFallback(InFeatureLevel); }
		virtual const FMaterialRenderProxy* GetFallback(ERHIFeatureLevel::Type InFeatureLevel) const override { return Parent->GetFallback(InFeatureLevel); }
		virtual bool GetParameterValue(EMaterialParameterType Type, const FHashedMaterialParameterInfo& Info, FMaterialParameterValue& OutValue, const FMaterialRenderContext& Context) const override
		{
			if (Type == EMaterialParameterType::Vector)
			{
				if (Info.Name == SphereName) { OutValue = Sphere; return true; }
				if (Info.Name == ParamsName) { OutValue = Params; return true; }
				if (Info.Name == ColorName) { OutValue = Color; return true; }
			}
			return Parent->GetParameterValue(Type, Info, OutValue, Context);
		}

	private:
		const FMaterialRenderProxy* Parent;
		FLinearColor Sphere;
		FLinearColor Params;
		FLinearColor Color;
	};
}

UMaterial* UExposureSmokeComponent::VolumeMaterial()
{
	using namespace ExposureSmokeVolume;
	if (Material || bTried)
	{
		return Material;
	}
	bTried = true;
	UMaterial* M = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_ExposureSmokeVolume"), RF_Transient);
	M->MaterialDomain = MD_Surface;
	M->BlendMode = BLEND_Translucent;
	M->SetShadingModel(MSM_Unlit);
	M->TwoSided = true;
	// The shader cuts the cloud at the scene depth itself, so the far side of the sphere draws even behind a wall.
	M->bDisableDepthTest = true;

	auto New = [M](TSubclassOf<UMaterialExpression> Class) { return UMaterialEditingLibrary::CreateMaterialExpression(M, Class); };
	auto Param = [&New](FName Name, const FLinearColor& Default)
	{
		UMaterialExpressionVectorParameter* P = CastChecked<UMaterialExpressionVectorParameter>(New(UMaterialExpressionVectorParameter::StaticClass()));
		P->ParameterName = Name;
		P->DefaultValue = Default;
		return P;
	};
	// Vector parameter outputs: 0 RGB, 4 A, 5 RGBA.
	UMaterialExpressionVectorParameter* Sphere = Param(SphereName, FLinearColor(0.f, 0.f, 0.f, 100.f));
	UMaterialExpressionVectorParameter* Params = Param(ParamsName, FLinearColor(4.f, 1.f, 0.f, 0.f));
	UMaterialExpressionVectorParameter* Col = Param(ColorName, FLinearColor(0.62f, 0.62f, 0.65f, 1.f));

	UMaterialExpressionWorldPosition* WorldPos = CastChecked<UMaterialExpressionWorldPosition>(New(UMaterialExpressionWorldPosition::StaticClass()));
	WorldPos->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	UMaterialExpression* Cam = New(UMaterialExpressionCameraPositionWS::StaticClass());
	UMaterialExpressionSubtract* Rel = CastChecked<UMaterialExpressionSubtract>(New(UMaterialExpressionSubtract::StaticClass()));
	Rel->A.Connect(0, WorldPos);
	Rel->B.Connect(0, Sphere);
	UMaterialExpressionSubtract* CamRel = CastChecked<UMaterialExpressionSubtract>(New(UMaterialExpressionSubtract::StaticClass()));
	CamRel->A.Connect(0, Cam);
	CamRel->B.Connect(0, Sphere);
	UMaterialExpression* SceneD = New(UMaterialExpressionSceneDepth::StaticClass());
	UMaterialExpression* PixelD = New(UMaterialExpressionPixelDepth::StaticClass());

	UMaterialExpressionCustom* March = CastChecked<UMaterialExpressionCustom>(New(UMaterialExpressionCustom::StaticClass()));
	March->Description = TEXT("ExposureSmokeMarch");
	March->OutputType = CMOT_Float4;
	March->Code = Code;
	March->Inputs.Reset();
	auto In = [March](const TCHAR* Name, UMaterialExpression* From, int32 Output)
	{
		FCustomInput& I = March->Inputs.AddDefaulted_GetRef();
		I.InputName = Name;
		I.Input.Connect(Output, From);
	};
	In(TEXT("Rel"), Rel, 0);
	In(TEXT("CamRel"), CamRel, 0);
	In(TEXT("Radius"), Sphere, 4);
	In(TEXT("SceneD"), SceneD, 0);
	In(TEXT("PixelD"), PixelD, 0);
	In(TEXT("P"), Params, 5);
	In(TEXT("Col"), Col, 0);

	auto Mask = [&New, March](bool bRGB)
	{
		UMaterialExpressionComponentMask* K = CastChecked<UMaterialExpressionComponentMask>(New(UMaterialExpressionComponentMask::StaticClass()));
		K->R = K->G = K->B = bRGB ? 1 : 0;
		K->A = bRGB ? 0 : 1;
		K->Input.Connect(0, March);
		return K;
	};
	UMaterialEditingLibrary::ConnectMaterialProperty(Mask(true), FString(), MP_EmissiveColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Mask(false), FString(), MP_Opacity);
	M->AddToRoot();
	UMaterialEditingLibrary::RecompileMaterial(M);
	Material = M;
	return M;
}

bool UExposureSmokeComponent::IsVolumeReady()
{
	using namespace ExposureSmokeVolume;
	if (bFailed)
	{
		return false;
	}
	UMaterial* M = VolumeMaterial();
	const FMaterialResource* Res = M ? M->GetMaterialResource(GMaxRHIShaderPlatform) : nullptr;
	if (!Res)
	{
		return false;
	}
	if (Res->GetCompileErrors().Num() > 0)
	{
		bFailed = true;
		UE_LOG(LogExposureProbe, Warning, TEXT("Volume smoke material failed to compile; smoke is drawn as puffs. %s"), *FString::Join(Res->GetCompileErrors(), TEXT(" | ")));
		return false;
	}
	return Res->GetGameThreadShaderMap() != nullptr && Res->IsGameThreadShaderMapComplete();
}

namespace
{
	constexpr int32 PuffsPerCloud = 26;

	class FExposureSmokeSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		FExposureSmokeSceneProxy(const UExposureSmokeComponent* InComponent)
			: FPrimitiveSceneProxy(InComponent)
			, Density(InComponent->GetDensity())
			, Color(InComponent->GetColor())
		{
			bWillEverBeLit = false;
			bVerifyUsedMaterials = false;
			for (const FExposureSmokeCloud& Cloud : InComponent->GetClouds())
			{
				FCloud& C = Clouds.AddDefaulted_GetRef();
				C.Centre = Cloud.Centre;
				C.Radius = Cloud.Radius;
				C.Born = Cloud.Born;
				C.Seed = float(Cloud.Seed % 97u) * 0.37f;
			}
			if (InComponent->GetStyle() == EExposureSmokeStyle::Volume)
			{
				if (UExposureSmokeComponent::IsVolumeReady())
				{
					UMaterial* M = UExposureSmokeComponent::VolumeMaterial();
					VolumeProxy = M->GetRenderProxy();
					Relevance = M->GetRelevance_Concurrent(GMaxRHIShaderPlatform);
				}
				else
				{
					// The material is still compiling: puffs until it is ready, then the component redraws.
					InComponent->bVolumeFallback = true;
				}
			}
			if (!VolumeProxy)
			{
				BuildPuffs(InComponent->GetClouds());
			}
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
		{
			const double Now = FPlatformTime::Seconds();
			const float Time = float(FMath::Fmod(Now, 600.0));
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if (!(VisibilityMap & (1 << ViewIndex)))
				{
					continue;
				}
				if (VolumeProxy)
				{
					for (const FCloud& C : Clouds)
					{
						const float Grow = 0.3f + 0.7f * FMath::SmoothStep(0.f, 1.f, float(FMath::Min(1.0, (Now - C.Born) / ExposureSmokeVolume::GrowSeconds)));
						FMaterialRenderProxy* Mat = new ExposureSmokeVolume::FSmokeMaterialProxy(VolumeProxy,
							FLinearColor(float(C.Centre.X), float(C.Centre.Y), float(C.Centre.Z), C.Radius),
							FLinearColor(Density, Grow, C.Seed, Time), Color);
						Collector.RegisterOneFrameMaterialProxy(Mat);
						// A little larger than the sphere, so the polygon mesh covers all of it.
						GetSphereMesh(C.Centre, FVector(1.12f * C.Radius), 28, 18, Mat, SDPG_World, true, ViewIndex, Collector, false, nullptr);
					}
				}
				else if (GEngine && GEngine->DebugMeshMaterial)
				{
					DrawPuffs(Now, ViewIndex, Collector);
				}
			}
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			if (VolumeProxy)
			{
				Relevance.SetPrimitiveViewRelevance(Result);
			}
			Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.Editor;
			Result.bDynamicRelevance = true;
			Result.bShadowRelevance = false;
			Result.bNormalTranslucency = true;
			Result.bSeparateTranslucency = true;
			return Result;
		}

		virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

	private:
		struct FCloud
		{
			FVector Centre = FVector::ZeroVector;
			float Radius = 1.f;
			double Born = 0.0;
			float Seed = 0.f;
		};
		struct FPuff
		{
			int32 Cloud = 0;
			FVector Offset = FVector::ZeroVector;
			FVector Phase = FVector::ZeroVector;
			float Radius = 1.f;
			float Drift = 0.f;
			FLinearColor Color = FLinearColor::White;
		};

		void BuildPuffs(const TArray<FExposureSmokeCloud>& InClouds)
		{
			for (int32 c = 0; c < InClouds.Num(); ++c)
			{
				FRandomStream Rng(int32(InClouds[c].Seed & 0x7fffffff));
				const float R = InClouds[c].Radius;
				// One soft body that fills most of the sphere, then puffs that keep the outline lumpy.
				FPuff& Core = Puffs.AddDefaulted_GetRef();
				Core.Cloud = c;
				Core.Offset = FVector(0.f, 0.f, -0.05f * R);
				Core.Radius = 0.78f * R;
				Core.Color = FLinearColor(Color.R, Color.G, Color.B + 0.03f, 0.45f);
				Core.Phase = FVector(Rng.FRand(), Rng.FRand(), Rng.FRand()) * 6.2832f;
				Core.Drift = 0.02f * R;
				for (int32 k = 0; k < PuffsPerCloud; ++k)
				{
					FPuff& P = Puffs.AddDefaulted_GetRef();
					P.Cloud = c;
					P.Radius = R * Rng.FRandRange(0.26f, 0.42f);
					FVector O = Rng.GetUnitVector() * (R - P.Radius) * FMath::Pow(Rng.FRand(), 0.4f);
					// The smoke rests on the floor, which lies below the centre: no puff sinks under it.
					O.Z = FMath::Max(O.Z, -0.42f * R + 0.6f * P.Radius);
					P.Offset = O;
					// Shades of the smoke colour, grey enough to read against a white block-out floor.
					const float G = Rng.FRandRange(0.9f, 1.25f);
					P.Color = FLinearColor(Color.R * G, Color.G * G, Color.B * G + 0.03f, 0.32f);
					P.Phase = FVector(Rng.FRand(), Rng.FRand(), Rng.FRand()) * 6.2832f;
					P.Drift = Rng.FRandRange(0.03f, 0.07f) * R;
				}
			}
		}

		void DrawPuffs(double Now, int32 ViewIndex, FMeshElementCollector& Collector) const
		{
			const float T = float(FMath::Fmod(Now, 600.0));
			for (const FPuff& P : Puffs)
			{
				const FCloud& C = Clouds[P.Cloud];
				const float Grow = FMath::SmoothStep(0.f, 1.f, float(FMath::Min(1.0, (Now - C.Born) / ExposureSmokeVolume::GrowSeconds)));
				const FVector Wobble(FMath::Sin(T * 0.45f + float(P.Phase.X)), FMath::Cos(T * 0.37f + float(P.Phase.Y)), 0.6f * FMath::Sin(T * 0.29f + float(P.Phase.Z)));
				const FVector Centre = C.Centre + (P.Offset + Wobble * P.Drift) * (0.35f + 0.65f * Grow);
				const float Radius = P.Radius * (1.f + 0.07f * FMath::Sin(T * 0.6f + float(P.Phase.X))) * (0.3f + 0.7f * Grow);
				FColoredMaterialRenderProxy* Mat = new FColoredMaterialRenderProxy(GEngine->DebugMeshMaterial->GetRenderProxy(), P.Color);
				Collector.RegisterOneFrameMaterialProxy(Mat);
				GetSphereMesh(Centre, FVector(Radius), 18, 10, Mat, SDPG_World, false, ViewIndex, Collector);
			}
		}

		TArray<FCloud> Clouds;
		TArray<FPuff> Puffs;
		const FMaterialRenderProxy* VolumeProxy = nullptr;
		FMaterialRelevance Relevance;
		float Density = 4.f;
		FLinearColor Color;
	};
}

UExposureSmokeComponent::UExposureSmokeComponent()
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	CastShadow = false;
	bHiddenInGame = true;
	bIsEditorOnly = true;
	bSelectable = false;
	SetCanEverAffectNavigation(false);
}

void UExposureSmokeComponent::SetClouds(TArray<FExposureSmokeCloud>&& InClouds, EExposureSmokeStyle InStyle, float InDensity, const FLinearColor& InColor)
{
	Clouds = MoveTemp(InClouds);
	Style = InStyle;
	if (Style == EExposureSmokeStyle::Volume)
	{
		// Start compiling before the proxy asks for it.
		VolumeMaterial();
	}
	Density = InDensity;
	Color = InColor;
	bVolumeFallback = false;
	Box = FBox(ForceInit);
	for (const FExposureSmokeCloud& C : Clouds)
	{
		Box += FBox(C.Centre - FVector(1.2f * C.Radius), C.Centre + FVector(1.2f * C.Radius));
	}
	UpdateBounds();
	MarkRenderStateDirty();
}

bool UExposureSmokeComponent::NeedsVolumeRefresh() const
{
	return bVolumeFallback && Style == EExposureSmokeStyle::Volume && IsVolumeReady();
}

FPrimitiveSceneProxy* UExposureSmokeComponent::CreateSceneProxy()
{
	return Clouds.Num() > 0 ? new FExposureSmokeSceneProxy(this) : nullptr;
}

FBoxSphereBounds UExposureSmokeComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return Box.IsValid ? FBoxSphereBounds(Box) : FBoxSphereBounds(FVector::ZeroVector, FVector(1.f), 1.f);
}
