#include "ProceduralPlanetGPUActor.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"

#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "TextureResource.h"

// ---------------- Static buffers ----------------
struct FStaticFaceData
{
	TArray<int32> Indices;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;
};

struct FStaticBuffers
{
	int32 Resolution = 0;
	TArray<FStaticFaceData> Faces;
};

// ---------------- Cube faces basis (как у тебя) ----------------
namespace
{
	struct FFaceBasis
	{
		FVector3f Normal;
		FVector3f AxisA;
		FVector3f AxisB;
	};

	const FFaceBasis CubeFaces[6] =
	{
		{ FVector3f( 1,  0,  0), FVector3f(0,  1,  0), FVector3f(0,  0,  1) },
		{ FVector3f(-1,  0,  0), FVector3f(0,  1,  0), FVector3f(0,  0, -1) },
		{ FVector3f( 0,  1,  0), FVector3f(0,  0, -1), FVector3f(-1, 0,  0) },
		{ FVector3f( 0, -1,  0), FVector3f(0,  0,  1), FVector3f(-1, 0,  0) },
		{ FVector3f( 0,  0,  1), FVector3f(1,  0,  0), FVector3f(0,  1,  0) },
		{ FVector3f( 0,  0, -1), FVector3f(1,  0,  0), FVector3f(0, -1,  0) }
	};

	void BuildStaticFaceData(const int32 Res, const FFaceBasis& Basis, FStaticFaceData& Out)
	{
		const int32 VertPerSide = Res + 1;

		Out.UVs.Reset();
		Out.Indices.Reset();
		Out.Tangents.Reset();

		Out.UVs.Reserve(VertPerSide * VertPerSide);
		Out.Indices.Reserve(Res * Res * 6);
		Out.Tangents.Reserve(VertPerSide * VertPerSide);

		for (int32 Y = 0; Y < VertPerSide; ++Y)
		{
			const float V = (float)Y / Res;
			for (int32 X = 0; X < VertPerSide; ++X)
			{
				const float U = (float)X / Res;
				Out.UVs.Add(FVector2D(U, V));
				Out.Tangents.Add(FProcMeshTangent(FVector(Basis.AxisA), false));
			}
		}

		for (int32 Y = 0; Y < Res; ++Y)
		{
			for (int32 X = 0; X < Res; ++X)
			{
				const int32 I0 = (Y)*VertPerSide + (X);
				const int32 I1 = (Y)*VertPerSide + (X + 1);
				const int32 I2 = (Y + 1)*VertPerSide + (X);
				const int32 I3 = (Y + 1)*VertPerSide + (X + 1);

				Out.Indices.Add(I0); Out.Indices.Add(I2); Out.Indices.Add(I1);
				Out.Indices.Add(I1); Out.Indices.Add(I2); Out.Indices.Add(I3);
			}
		}
	}

	struct FFaceMeshData
	{
		int32 SectionIndex = 0;
		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		const TArray<int32>* Indices = nullptr;
		const TArray<FVector2D>* UVs = nullptr;
		const TArray<FProcMeshTangent>* Tangents = nullptr;
	};

	void BuildFaceMesh_NoNoise(const FPlanetGenerationConfigGPU& Config, const FStaticFaceData& StaticData, int32 FaceIndex, float BaseRadiusCm, int32 SectionIndex, FFaceMeshData& Out)
	{
		const FFaceBasis Basis = CubeFaces[FaceIndex];
		const int32 Res = FMath::Clamp(Config.FaceResolution, 4, 1024);
		const int32 VertPerSide = Res + 1;

		Out.SectionIndex = SectionIndex;
		Out.Vertices.Reset();
		Out.Normals.Reset();
		Out.Vertices.Reserve(VertPerSide * VertPerSide);
		Out.Normals.Reserve(VertPerSide * VertPerSide);

		Out.Indices = &StaticData.Indices;
		Out.UVs = &StaticData.UVs;
		Out.Tangents = &StaticData.Tangents;

		for (int32 Y = 0; Y < VertPerSide; ++Y)
		{
			const float V = (float)Y / Res;
			for (int32 X = 0; X < VertPerSide; ++X)
			{
				const float U = (float)X / Res;
				const FVector3f CubeDir =
					Basis.Normal +
					Basis.AxisA * (U * 2.f - 1.f) +
					Basis.AxisB * (V * 2.f - 1.f);

				const FVector3f Dir = CubeDir.GetSafeNormal();
				Out.Vertices.Add((FVector)(Dir * BaseRadiusCm));
				Out.Normals.Add((FVector)Dir);
			}
		}
	}
}

// ============================================================
// Compute shaders
// ============================================================

class FPlanetHeightCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPlanetHeightCS);
	SHADER_USE_PARAMETER_STRUCT(FPlanetHeightCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, FaceSize)
		SHADER_PARAMETER(float, BaseRadiusKm)
		SHADER_PARAMETER(float, AmplitudeScale)
		SHADER_PARAMETER(float, ContinentHeightKm)
		SHADER_PARAMETER(float, MountainHeightKm)
		SHADER_PARAMETER(int32, NoiseSeed)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, OutHeight)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FPlanetHeightCS, "/Project/PlanetHeightCS.usf", "MainCS", SF_Compute);

class FPlanetHeightMipCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPlanetHeightMipCS);
	SHADER_USE_PARAMETER_STRUCT(FPlanetHeightMipCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SrcSize)
		SHADER_PARAMETER(uint32, DstSize)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float>, InHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, OutHeight)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FPlanetHeightMipCS, "/Project/PlanetHeightMipCS.usf", "MainCS", SF_Compute);

// ============================================================
// Actor
// ============================================================

AProceduralPlanetGPUActor::AProceduralPlanetGPUActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bRunConstructionScriptOnDrag = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	PlanetMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlanetMesh"));
	PlanetMesh->SetupAttachment(SceneRoot);
	PlanetMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	PlanetMesh->bUseAsyncCooking = true;
}

AProceduralPlanetGPUActor::~AProceduralPlanetGPUActor() = default;

void AProceduralPlanetGPUActor::BeginPlay()
{
	Super::BeginPlay();
	GeneratePlanet_Internal();
}

void AProceduralPlanetGPUActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

#if WITH_EDITOR
	if (GIsEditor && GetWorld() && !GetWorld()->IsGameWorld())
	{
		if (PlanetMesh && PlanetMesh->GetNumSections() == 0)
		{
			GeneratePlanet_Internal();
		}
	}
#endif
}

#if WITH_EDITOR
void AProceduralPlanetGPUActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
		return;

	const FName P = PropertyChangedEvent.GetPropertyName();
	if (P == GET_MEMBER_NAME_CHECKED(AProceduralPlanetGPUActor, PlanetRadiusKm) ||
		P == GET_MEMBER_NAME_CHECKED(AProceduralPlanetGPUActor, FaceResolution) ||
		P == GET_MEMBER_NAME_CHECKED(AProceduralPlanetGPUActor, AmplitudeScale) ||
		P == GET_MEMBER_NAME_CHECKED(AProceduralPlanetGPUActor, ContinentHeightKm) ||
		P == GET_MEMBER_NAME_CHECKED(AProceduralPlanetGPUActor, MountainHeightKm) ||
		P == GET_MEMBER_NAME_CHECKED(AProceduralPlanetGPUActor, NoiseSeed) ||
		P == GET_MEMBER_NAME_CHECKED(AProceduralPlanetGPUActor, HeightCubeSize))
	{
		RegeneratePlanet();
	}
}
#endif

void AProceduralPlanetGPUActor::RegeneratePlanet()
{
	GeneratePlanet_Internal();
}

void AProceduralPlanetGPUActor::GeneratePlanet_Internal()
{
	if (!PlanetMesh) return;

	FPlanetGenerationConfigGPU Config;
	Config.PlanetRadiusKm = PlanetRadiusKm;
	Config.FaceResolution = FMath::Clamp(FaceResolution, 4, 1024);
	Config.AmplitudeScale = AmplitudeScale;
	Config.ContinentHeightKm = ContinentHeightKm;
	Config.MountainHeightKm = MountainHeightKm;
	Config.NoiseSeed = NoiseSeed;

	const float BaseRadiusCm = FMath::Max(1000.f, Config.PlanetRadiusKm * 100000.f);
	const uint64 GenerationId = ++ActiveGenerationId;

	// ---- static buffers (indices/uv/tangents) ----
	const bool bRebuildStatic = !CurrentStaticBuffers.IsValid() ||
		CachedResolution != Config.FaceResolution ||
		CurrentStaticBuffers->Resolution != Config.FaceResolution;

	if (bRebuildStatic)
	{
		TSharedRef<FStaticBuffers, ESPMode::ThreadSafe> StaticBuffers = MakeShared<FStaticBuffers, ESPMode::ThreadSafe>();
		StaticBuffers->Resolution = Config.FaceResolution;
		StaticBuffers->Faces.SetNum(6);

		for (int32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
		{
			BuildStaticFaceData(Config.FaceResolution, CubeFaces[FaceIdx], StaticBuffers->Faces[FaceIdx]);
		}

		CurrentStaticBuffers = StaticBuffers;
		CachedResolution = Config.FaceResolution;

		PlanetMesh->ClearAllMeshSections();
	}

	// ---- Height cubemap on GPU ----
	EnsureHeightCubeAndDispatch(Config);

	// ---- Material MID ----
	if (PlanetMaterial)
	{
		PlanetMID = UMaterialInstanceDynamic::Create(PlanetMaterial, this);
		if (PlanetMID && HeightCubeRT)
		{
			PlanetMID->SetTextureParameterValue(TEXT("HeightCube"), HeightCubeRT);
			PlanetMID->SetScalarParameterValue(TEXT("KmToCm"), 100000.0f);
		}
	}

	// ---- Build 6 faces (sphere only) ----
	for (int32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
	{
		LaunchFaceBuildTask_NoNoise(FaceIdx, BaseRadiusCm, FaceIdx, Config, GenerationId);
	}
}

void AProceduralPlanetGPUActor::EnsureHeightCubeAndDispatch(const FPlanetGenerationConfigGPU& Config)
{
	if (!HeightCubeRT || HeightCubeRT->SizeX != HeightCubeSize || HeightCubeRT->OverrideFormat != PF_R32_FLOAT)
	{
		HeightCubeRT = NewObject<UTextureRenderTargetCube>(this, NAME_None, RF_Transient);
		HeightCubeRT->ClearColor = FLinearColor(0, 0, 0, 0);
		HeightCubeRT->bSupportsUAV = true;
		HeightCubeRT->bAutoGenerateMips = true;
		HeightCubeRT->OverrideFormat = PF_R32_FLOAT;
		HeightCubeRT->Init((uint32)HeightCubeSize, PF_R32_FLOAT);
		HeightCubeRT->UpdateResourceImmediate(true);
	}

	FTextureRenderTargetResource* RTRes = HeightCubeRT->GameThread_GetRenderTargetResource();
	if (!RTRes) return;

	const uint32 FaceSize = (uint32)HeightCubeRT->SizeX;
	const int32 NumMips = HeightCubeRT->GetNumMips();

	ENQUEUE_RENDER_COMMAND(PlanetHeight_Dispatch)(
		[RTRes, FaceSize, NumMips, Config](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		FRHITexture* RHITexture = RTRes->GetRenderTargetTexture();
		if (!RHITexture)
		{
			GraphBuilder.Execute();
			return;
		}

		FRDGTextureRef HeightTex = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(RHITexture, TEXT("PlanetHeightCubeRT"))
		);

		// mip0
		{
			TShaderMapRef<FPlanetHeightCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			auto* P = GraphBuilder.AllocParameters<FPlanetHeightCS::FParameters>();

			P->FaceSize = FaceSize;
			P->BaseRadiusKm = Config.PlanetRadiusKm;
			P->AmplitudeScale = Config.AmplitudeScale;
			P->ContinentHeightKm = Config.ContinentHeightKm;
			P->MountainHeightKm = Config.MountainHeightKm;
			P->NoiseSeed = Config.NoiseSeed;

			P->OutHeight = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HeightTex, 0));

			const FIntVector Groups(
				FMath::DivideAndRoundUp((int32)FaceSize, 8),
				FMath::DivideAndRoundUp((int32)FaceSize, 8),
				6
			);
			FComputeShaderUtils::Dispatch(GraphBuilder, CS, *P, Groups);
		}

		// mips
		for (int32 Mip = 1; Mip < NumMips; ++Mip)
		{
			const uint32 SrcSize = FaceSize >> (Mip - 1);
			const uint32 DstSize = FaceSize >> (Mip);
			if (DstSize < 1) break;

			TShaderMapRef<FPlanetHeightMipCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			auto* P = GraphBuilder.AllocParameters<FPlanetHeightMipCS::FParameters>();

			P->SrcSize = SrcSize;
			P->DstSize = DstSize;
			P->InHeight = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(HeightTex, Mip - 1));
			P->OutHeight = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HeightTex, Mip));

			const FIntVector Groups(
				FMath::DivideAndRoundUp((int32)DstSize, 8),
				FMath::DivideAndRoundUp((int32)DstSize, 8),
				6
			);
			FComputeShaderUtils::Dispatch(GraphBuilder, CS, *P, Groups);
		}

		GraphBuilder.Execute();
	});
}

void AProceduralPlanetGPUActor::LaunchFaceBuildTask_NoNoise(int32 FaceIndex, float BaseRadiusCm, int32 SectionIndex, FPlanetGenerationConfigGPU Config, uint64 GenerationId)
{
	TWeakObjectPtr<AProceduralPlanetGPUActor> WeakThis(this);
	TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe> StaticBuffers = CurrentStaticBuffers;

	Async(EAsyncExecution::ThreadPool, [WeakThis, Config, BaseRadiusCm, FaceIndex, SectionIndex, GenerationId, StaticBuffers]()
	{
		if (!StaticBuffers.IsValid() || StaticBuffers->Faces.Num() <= FaceIndex) return;

		FFaceMeshData MeshData;
		BuildFaceMesh_NoNoise(Config, StaticBuffers->Faces[FaceIndex], FaceIndex, BaseRadiusCm, SectionIndex, MeshData);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, GenerationId, MeshData = MoveTemp(MeshData)]() mutable
		{
			if (!WeakThis.IsValid()) return;
			if (GenerationId != WeakThis->ActiveGenerationId) return;
			if (!WeakThis->PlanetMesh || !MeshData.Indices || !MeshData.UVs || !MeshData.Tangents) return;

			WeakThis->PlanetMesh->ClearMeshSection(MeshData.SectionIndex);
			WeakThis->PlanetMesh->CreateMeshSection_LinearColor(
				MeshData.SectionIndex,
				MeshData.Vertices,
				*MeshData.Indices,
				MeshData.Normals,
				*MeshData.UVs,
				TArray<FLinearColor>(),
				*MeshData.Tangents,
				true
			);

			if (WeakThis->PlanetMID)
			{
				WeakThis->PlanetMesh->SetMaterial(MeshData.SectionIndex, WeakThis->PlanetMID);
			}
		});
	});
}
