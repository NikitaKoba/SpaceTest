// ProceduralPlanetActor.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralPlanetActor.generated.h"

class UProceduralMeshComponent;
class USceneComponent;
struct FStaticBuffers;
struct FFaceMeshData;

struct FPlanetGenerationConfig
{
	float PlanetRadiusKm = 6371.0f;
	int32 FaceResolution = 128;
	float AmplitudeScale = 1.0f;
	float ContinentHeightKm = 8.0f;
	float MountainHeightKm = 5.0f;
	int32 NoiseSeed = 12345;
};

USTRUCT(BlueprintType)
struct FPlanetLODLevel
{
	GENERATED_BODY()

	/** Максимальная дистанция (км) от поверхности до центра чанка, при которой используется этот LOD. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	float DistanceKm = 2000.0f;

	/** Разрешение сетки для чанка этого LOD (Res x Res). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "4", ClampMax = "256"))
	int32 Resolution = 32;
};

struct FPlanetChunkId
{
	uint8 Face = 0;
	uint8 Lod = 0;
	uint16 X = 0;
	uint16 Y = 0;

	bool operator==(const FPlanetChunkId& Other) const
	{
		return Face == Other.Face && Lod == Other.Lod && X == Other.X && Y == Other.Y;
	}
};

FORCEINLINE uint32 GetTypeHash(const FPlanetChunkId& Id)
{
	return HashCombine(HashCombine(HashCombine(::GetTypeHash(Id.Face), ::GetTypeHash(Id.Lod)), ::GetTypeHash(Id.X)), ::GetTypeHash(Id.Y));
}

UCLASS()
class SPACETEST_API AProceduralPlanetActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralPlanetActor();
	virtual ~AProceduralPlanetActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnConstruction(const FTransform& Transform) override;
	void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent);

	UFUNCTION(BlueprintCallable, Category = "Procedural Planet")
	void RegeneratePlanet();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "1.0", ClampMax = "100000.0"))
	float PlanetRadiusKm = 6371.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "4", ClampMax = "1024"))
	int32 FaceResolution = 128;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float AmplitudeScale = 1.0f;

	/** Использовать ли FastNoiseLite (true) или старый FMath::PerlinNoise3D (false) для дебага/сравнения. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet")
	bool bUseFastNoise = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet")
	int32 NoiseSeed = 12345;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continents", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float ContinentHeightKm = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mountains", meta = (ClampMin = "0.0", ClampMax = "15.0"))
	float MountainHeightKm = 5.0f;

	/** Настройки LOD (упорядочены от дальней дистанции к ближней). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	TArray<FPlanetLODLevel> LODLevels;

	/** Радиус стабильности (км): пока игрок двигается внутри, пересчёта нет. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.1", ClampMax = "100.0"))
	float UpdateThresholdKm = 1.0f;

	/** Радиус детальной подгрузки (км) от поверхности: внутри строятся мелкие LOD, снаружи берётся более грубый. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "1.0", ClampMax = "200.0"))
	float StreamingRadiusKm = 12.0f;

	/** Шаг обновления потокера (сек). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float UpdateInterval = 0.2f;

	/** Максимальное число асинхронных генераций чанков одновременно. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxConcurrentBuilds = 3;

	/** Максимальное число заявок в очереди. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "4", ClampMax = "64"))
	int32 MaxQueuedBuilds = 64;

	/** Включать ли коллизию для сгенерированных секций. По умолчанию выключено для скорости. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bEnableCollision = false;

	/** Кого считать фокусом (если не задано — первый pawn). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	TWeakObjectPtr<AActor> FocusActorOverride;

	/** Показывать предпросмотр в редакторе даже без Pawn (используется виртуальная камера перед актёром). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Editor Preview")
	bool bPreviewInEditor = true;

	/** Дистанция виртуальной камеры для предпросмотра (км) от центра планеты вдоль Forward. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Editor Preview", meta = (ClampMin = "0.1", ClampMax = "5000.0"))
	float PreviewCameraDistanceKm = 50.0f;

	/** Доп. локальный сдвиг виртуальной камеры предпросмотра (UU). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Editor Preview")
	FVector PreviewCameraLocalOffset = FVector::ZeroVector;

	/** Показывать ли глобальный “лоу поли” слой (LOD0) всегда в редакторе, чтобы была видна целая планета. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Editor Preview")
	bool bShowGlobalLowLODInEditor = true;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USceneComponent* SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UProceduralMeshComponent* PlanetMesh;

private:
	uint64 ActiveGenerationId = 0;

	struct FChunkState
	{
		int32 SectionIndex = INDEX_NONE;
		int32 Resolution = 0;
		uint8 Lod = 0;
		bool bPending = false;
		bool bAttached = false;
	};

	TMap<FPlanetChunkId, FChunkState> ChunkStates;
	TArray<FPlanetChunkId> BuildQueue;
	TSet<FPlanetChunkId> FallbackChunks;
	int32 ActiveBuilds = 0;
	float TimeSinceUpdate = 0.0f;
	FVector LastFocusWorld = FVector::ZeroVector;
	bool bHasLastFocus = false;
	int32 NextSectionIndex = 0;
	TArray<int32> FreeSections;
	TMap<int32, TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe>> StaticCache;

	void GeneratePlanet();

	// LOD/чанки
	void SortLODLevels();
	int32 GetLODResolution(uint8 Lod) const;
	void UpdateStreaming(const FVector& FocusWorld);
	bool GetFocusLocation(FVector& OutFocusWorld) const;
	void CollectDesiredChunks(const FVector& FocusWorld, float BaseRadiusCm, const FPlanetGenerationConfig& Config, TSet<FPlanetChunkId>& OutDesired, TSet<FPlanetChunkId>& OutFallback) const;
	int32 DesiredDepthForDistance(float DistanceToSurfaceKm) const;
	void TraverseFace(uint8 Face, uint8 Lod, uint16 X, uint16 Y, const FVector& FocusWorld, float BaseRadiusCm, const FPlanetGenerationConfig& Config, TSet<FPlanetChunkId>& OutDesired, TSet<FPlanetChunkId>& OutFallback) const;
	void TrimAndQueueChunks(const TSet<FPlanetChunkId>& Desired, const TSet<FPlanetChunkId>& Fallback);
	void EnqueueChunkBuild(const FPlanetChunkId& Id);
	void KickBuilds();
	void OnChunkBuilt(const FPlanetChunkId& Id, FFaceMeshData&& MeshData, const TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe>& StaticBuffers, uint64 GenerationId);

	TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe> GetStaticBuffersForRes(int32 Resolution);
	bool AreAllChildrenAttached(const FPlanetChunkId& Parent) const;
};
