// ProceduralPlanetActor.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralPlanetActor.generated.h"

// --- forward declarations (тут без include'ов) ---
class UProceduralMeshComponent;
class FastNoiseLite;

UCLASS()
class AProceduralPlanetActor : public AActor
{
    GENERATED_BODY()

public:
    AProceduralPlanetActor();
    // === НОВОЕ: API выборки высоты ===

    // Чисто h01 в [0..1] (0 – океан, 1 – вершины)
    UFUNCTION(BlueprintCallable, Category="Planet|Sampling")
    float SampleHeight01(const FVector& NormalDir) const;

    // h01 + высота в см относительно радиуса
    UFUNCTION(BlueprintCallable, Category="Planet|Sampling")
    void SampleHeight(const FVector& NormalDir, float& OutH01, float& OutHeightCm) const;

    // Параметры, которые нужны патчу
    UFUNCTION(BlueprintCallable, Category="Planet|Params")
    float GetPlanetRadiusKm() const { return PlanetRadiusKm; }

    UFUNCTION(BlueprintCallable, Category="Planet|Params")
    float GetPlanetRadiusCm() const { return PlanetRadiusKm * 100000.f; }

    UFUNCTION(BlueprintCallable, Category="Planet|Params")
    float GetSeaLevel01() const { return SeaLevel01; }

    UFUNCTION(BlueprintCallable, Category="Planet|Params")
    bool IsUsingRelativeAmplitude() const { return bUseRelativeAmplitude; }

    UFUNCTION(BlueprintCallable, Category="Planet|Params")
    float GetHeightAmplitudeM() const { return HeightAmplitudeM; }

    UFUNCTION(BlueprintCallable, Category="Planet|Params")
    float GetRelativeAmplitude() const { return RelativeAmplitude; }

    // Если хочешь тот же градиент цвета (океан/пляж/лес/камни/снег)
    UFUNCTION(BlueprintCallable, Category="Planet|Color")
    FLinearColor SampleColorFromHeight01(float H) const { return HeightToColor(H); }
    virtual void OnConstruction(const FTransform& Transform) override;
    float ComputeHeightCmFrom01(float H01) const;
    UPROPERTY(EditAnywhere, Category="Planet")
    UMaterialInterface* PlanetMaterial = nullptr;
#if WITH_EDITOR
    UFUNCTION(CallInEditor, Category="Planet")
    void RebuildPlanet_Editor();
#endif
    // --- чанки ---

    // Сколько чанков по одной стороне на каждую грань кубосферы (4 = 4x4 = 16 чанков на грань)
    UPROPERTY(EditAnywhere, Category="Planet|Chunks")
    int32 ChunksPerFace = 4;

    // Разрешение чанка по одной стороне (вершины). Итоговая детализация = ChunksPerFace * ChunkResolution.
    UPROPERTY(EditAnywhere, Category="Planet|Chunks")
    int32 ChunkResolution = 33; // Типичный вариант: 33, 65, 129
    // --- контрольные карты планеты (базовый рельеф после эрозии) ---
    // Одна карта на каждую грань куба, храним в [face][y * ControlResolution + x]

    int32 ControlResolution = 0;
    TArray<float> BaseHeightMaps[6];

    bool bControlMapsBuilt = false;

    // Вспомогательные функции:
    float ComputeBaseHeight01_NoErosion(const FVector& NormalDir) const;
    void  BuildControlMaps();
    float SampleBaseHeight01(const FVector& NormalDir) const;
    float SampleBaseHeightFaceUV(int32 Face, float U01, float V01) const;
protected:
    // ---- Вспомогательные методы ----
    void InitNoise();
    float GetHeight01(const FVector& NormalDir) const;
    void ApplySlopeErosion(TArray<float>& Heights01, int32 Res, int32 NumFaces) const;
    void ApplyHeightBlur(TArray<float>& Heights01, int32 Res, int32 NumFaces,
                         float Strength, float Sharpness, int32 Iterations) const;
    FLinearColor HeightToColor(float H) const;
    FVector GetBrushDir() const;
    void BuildPlanet();
    
    // ---- Компоненты ----
    UPROPERTY(VisibleAnywhere)
    UProceduralMeshComponent* Mesh = nullptr;

    // ---- Параметры планеты ----
    UPROPERTY(EditAnywhere, Category="Planet")
    float PlanetRadiusKm = 6371.f;

    UPROPERTY(EditAnywhere, Category="Planet")
    float HeightAmplitudeM = 6000.f;

    UPROPERTY(EditAnywhere, Category="Planet")
    bool bUseRelativeAmplitude = true;

    // доля от радиуса (0..0.05 = до 5% радиуса)
    UPROPERTY(EditAnywhere, Category="Planet",
        meta=(EditCondition="bUseRelativeAmplitude", ClampMin="0.0", ClampMax="0.05"))
    float RelativeAmplitude = 0.002f;

    UPROPERTY(EditAnywhere, Category="Planet")
    int32 Resolution = 512;

    UPROPERTY(EditAnywhere, Category="Planet")
    int32 EditorPreviewResolution = 128;

    UPROPERTY(EditAnywhere, Category="Planet")
    int32 Seed = 1337;

    // 0 = самое дно, 1 = вершины. Уровень моря
    UPROPERTY(EditAnywhere, Category="Planet")
    float SeaLevel01 = 0.48f;

    // ---- Континенты / базовый шум ----
    UPROPERTY(EditAnywhere, Category="Noise")
    float ContinentFrequency = 0.18f;

    UPROPERTY(EditAnywhere, Category="Noise")
    float ContinentBlend = 0.2f;

    UPROPERTY(EditAnywhere, Category="Noise")
    float ContinentWarpStrength = 1.0f;

    UPROPERTY(EditAnywhere, Category="Noise")
    float ContinentWarpFrequency = 3.0f;

    // ---- Горы ----
    UPROPERTY(EditAnywhere, Category="Noise|Mountains")
    float MountainFrequency = 1.0f;

    UPROPERTY(EditAnywhere, Category="Noise|Mountains")
    float MountainHeight = 0.4f;

    UPROPERTY(EditAnywhere, Category="Noise|Mountains")
    float MountainMaskPower = 1.2f;

    // ---- Холмы ----
    UPROPERTY(EditAnywhere, Category="Noise|Hills")
    float HillsFrequency = 2.0f;

    UPROPERTY(EditAnywhere, Category="Noise|Hills")
    float HillsHeight = 0.3f;

    // ---- Каньоны ----
    UPROPERTY(EditAnywhere, Category="Noise|Canyons")
    bool  bEnableCanyons = false;

    UPROPERTY(EditAnywhere, Category="Noise|Canyons",
        meta=(EditCondition="bEnableCanyons"))
    float CanyonFrequency = 6.0f;

    UPROPERTY(EditAnywhere, Category="Noise|Canyons",
        meta=(EditCondition="bEnableCanyons"))
    float CanyonDepth = 0.4f;

    // ---- Domain warp ----
    UPROPERTY(EditAnywhere, Category="Noise|Warp")
    bool bEnableDomainWarp = true;

    UPROPERTY(EditAnywhere, Category="Noise|Warp",
        meta=(EditCondition="bEnableDomainWarp"))
    float WarpStrength = 0.35f;

    UPROPERTY(EditAnywhere, Category="Noise|Warp",
        meta=(EditCondition="bEnableDomainWarp"))
    float WarpFrequency = 2.5f;

    // ---- Термоэрозия ----
    UPROPERTY(EditAnywhere, Category="Erosion")
    bool bEnableErosion = false;

    UPROPERTY(EditAnywhere, Category="Erosion",
        meta=(EditCondition="bEnableErosion"))
    int32 ErosionIterations = 10;

    UPROPERTY(EditAnywhere, Category="Erosion",
        meta=(EditCondition="bEnableErosion"))
    float ErosionTalus = 0.02f;

    UPROPERTY(EditAnywhere, Category="Erosion",
        meta=(EditCondition="bEnableErosion"))
    float ErosionStrength = 0.5f;

    // ---- Blur высот (быстрая псевдо-эрозия) ----
    UPROPERTY(EditAnywhere, Category="Erosion|Blur")
    bool bEnableHeightBlur = true;

    UPROPERTY(EditAnywhere, Category="Erosion|Blur",
        meta=(EditCondition="bEnableHeightBlur", ClampMin="0.0", ClampMax="1.0"))
    float HeightBlurStrength = 0.4f;

    UPROPERTY(EditAnywhere, Category="Erosion|Blur",
        meta=(EditCondition="bEnableHeightBlur", ClampMin="0.1", ClampMax="20.0"))
    float HeightBlurSharpness = 6.0f;

    UPROPERTY(EditAnywhere, Category="Erosion|Blur",
        meta=(EditCondition="bEnableHeightBlur", ClampMin="1", ClampMax="8"))
    int32 HeightBlurIterations = 1;

    // ---- Локальная кисть ----
    UPROPERTY(EditAnywhere, Category="Brush")
    bool bUseLocalizedNoise = false;

    UPROPERTY(EditAnywhere, Category="Brush",
        meta=(EditCondition="bUseLocalizedNoise"))
    FRotator BrushCenterRot = FRotator::ZeroRotator;

    UPROPERTY(EditAnywhere, Category="Brush",
        meta=(EditCondition="bUseLocalizedNoise"))
    float BrushRadiusDeg = 45.f;
    
    UPROPERTY(EditAnywhere, Category="Brush",
        meta=(EditCondition="bUseLocalizedNoise"))
    float BrushFalloffDeg = 20.f;
    friend class APlanetLocalPatchActor; // <--- ДОБАВЬ ЭТО
    UPROPERTY(EditAnywhere, Category="Planet")
    bool bAutoRebuildInEditor = true;
    // --- Rendering ---
    UPROPERTY(EditAnywhere, Category="Planet|Rendering")
    bool bGenerateBackfaces = true; // duplicate triangles inward so surface is visible from inside
    
    // ---- FastNoiseLite инстансы ----
    FastNoiseLite* ContinentalNoise    = nullptr;
    FastNoiseLite* MountainNoise       = nullptr;
    FastNoiseLite* MountainDetailNoise = nullptr;
    FastNoiseLite* HillsNoise          = nullptr;
    FastNoiseLite* CanyonNoise         = nullptr;
    FastNoiseLite* WarpNoiseX          = nullptr;
    FastNoiseLite* WarpNoiseY          = nullptr;
    FastNoiseLite* WarpNoiseZ          = nullptr;
    FastNoiseLite* OceanNoise          = nullptr;
    FastNoiseLite* ErosionControlNoise = nullptr;
};
