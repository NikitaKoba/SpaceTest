// ProceduralPlanetActor.cpp

#include "ProceduralPlanetActor.h"

#include "ProceduralMeshComponent.h"
#include "FastNoiseLite.h"
#include "Engine/World.h"

// ================== ХЕЛПЕРЫ ==================

static float To01(float n)
{
    return 0.5f * (n + 1.f);
}

static float SmoothStep01(float x)
{
    x = FMath::Clamp(x, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

static float SmoothStep(float a, float b, float x)
{
    if (a == b) return x >= b ? 1.f : 0.f;
    float t = (x - a) / (b - a);
    return SmoothStep01(t);
}

static FVector FaceDir(int32 Face)
{
    switch (Face)
    {
    case 0: return FVector::ForwardVector;  // +X
    case 1: return -FVector::ForwardVector; // -X
    case 2: return FVector::RightVector;    // +Y
    case 3: return -FVector::RightVector;   // -Y
    case 4: return FVector::UpVector;       // +Z
    default:return -FVector::UpVector;      // -Z
    }
}
// ========== КУБОСФЕРА: МАППИНГ НАПРАВЛЕНИЙ ==========

struct FCubeFaceUV
{
    int32 Face; // 0..5
    float U;    // [0..1]
    float V;    // [0..1]
};

// direction (нормаль) -> (грань куба, UV)
static FCubeFaceUV FaceUVFromDirection(const FVector& NIn)
{
    FVector N = NIn.GetSafeNormal();
    const float ax = FMath::Abs(N.X);
    const float ay = FMath::Abs(N.Y);
    const float az = FMath::Abs(N.Z);

    FCubeFaceUV R;

    if (ax >= ay && ax >= az)
    {
        // +/-X
        if (N.X > 0.0f)
        {
            R.Face = 0; // +X
            const float u = N.Y / ax;
            const float v = N.Z / ax;
            R.U = 0.5f * (u + 1.0f);
            R.V = 0.5f * (v + 1.0f);
        }
        else
        {
            R.Face = 1; // -X
            const float u = N.Y / ax;
            const float v = N.Z / ax;
            R.U = 0.5f * (u + 1.0f);
            R.V = 0.5f * (v + 1.0f);
        }
    }
    else if (ay >= ax && ay >= az)
    {
        // +/-Y
        if (N.Y > 0.0f)
        {
            R.Face = 2; // +Y
            const float u = N.X / ay;
            const float v = N.Z / ay;
            R.U = 0.5f * (u + 1.0f);
            R.V = 0.5f * (v + 1.0f);
        }
        else
        {
            R.Face = 3; // -Y
            const float u = N.X / ay;
            const float v = N.Z / ay;
            R.U = 0.5f * (u + 1.0f);
            R.V = 0.5f * (v + 1.0f);
        }
    }
    else
    {
        // +/-Z
        if (N.Z > 0.0f)
        {
            R.Face = 4; // +Z
            const float u = N.X / az;
            const float v = N.Y / az;
            R.U = 0.5f * (u + 1.0f);
            R.V = 0.5f * (v + 1.0f);
        }
        else
        {
            R.Face = 5; // -Z
            const float u = N.X / az;
            const float v = N.Y / az;
            R.U = 0.5f * (u + 1.0f);
            R.V = 0.5f * (v + 1.0f);
        }
    }

    R.U = FMath::Clamp(R.U, 0.0f, 1.0f);
    R.V = FMath::Clamp(R.V, 0.0f, 1.0f);
    return R;
}

// (грань, UV) -> нормаль (направление от центра планеты)
static FVector DirectionFromFaceUV(int32 Face, float U01, float V01)
{
    const float u = FMath::Lerp(-1.0f, 1.0f, U01);
    const float v = FMath::Lerp(-1.0f, 1.0f, V01);

    FVector P;

    switch (Face)
    {
    case 0: // +X
        P = FVector(1.0f, u, v);
        break;
    case 1: // -X
        P = FVector(-1.0f, u, v);
        break;
    case 2: // +Y
        P = FVector(u, 1.0f, v);
        break;
    case 3: // -Y
        P = FVector(u, -1.0f, v);
        break;
    case 4: // +Z
        P = FVector(u, v, 1.0f);
        break;
    default: // 5: -Z
        P = FVector(u, v, -1.0f);
        break;
    }

    return P.GetSafeNormal();
}

// ================== AProceduralPlanetActor ==================
float AProceduralPlanetActor::ComputeBaseHeight01_NoErosion(const FVector& NormalDir) const
{
    // Тут мы считаем только крупные континенты / океаны (без горных деталей)
    FVector P = NormalDir;

    // Континенты 0..1
    float cont = To01(ContinentalNoise->GetNoise(P.X, P.Y, P.Z));
    const float landMask = SmoothStep(SeaLevel01 - ContinentBlend, SeaLevel01 + ContinentBlend, cont);

    // Простейшая "плоская" карта: океан чуть ниже, суша чуть выше
    const float OceanFloor = SeaLevel01 - 0.35f;
    const float LandPlateau = SeaLevel01 + 0.15f;

    float h = FMath::Lerp(OceanFloor, LandPlateau, landMask);
    return FMath::Clamp(h, 0.0f, 1.0f);
}
void AProceduralPlanetActor::BuildControlMaps()
{
    if (bControlMapsBuilt)
        return;

    // На всякий случай
    InitNoise();

    // Берём разрешение глобальной карты из настроек
    const int32 Faces = 6;
    const int32 MinRes = 64;
    const int32 MaxRes = 2048;

    // Пример: примерно соответствуем детализации чанков
    const int32 TargetRes = FMath::Clamp(ChunksPerFace * ChunkResolution, MinRes, MaxRes);
    ControlResolution = TargetRes;

    const int32 FaceVerts = ControlResolution * ControlResolution;

    // Временный массив на все 6 граней
    TArray<float> Heights01;
    Heights01.SetNumUninitialized(FaceVerts * Faces);

    // 1) Заполняем базовый рельеф без эрозии (континенты + океаны)
    for (int32 Face = 0; Face < Faces; ++Face)
    {
        float* FaceH = Heights01.GetData() + Face * FaceVerts;

        for (int32 y = 0; y < ControlResolution; ++y)
        {
            float v01 = (float)y / float(ControlResolution - 1);

            for (int32 x = 0; x < ControlResolution; ++x)
            {
                float u01 = (float)x / float(ControlResolution - 1);

                const FVector N = DirectionFromFaceUV(Face, u01, v01);
                const float hBase = ComputeBaseHeight01_NoErosion(N);
                FaceH[y * ControlResolution + x] = hBase;
            }
        }
    }

    // 2) Термо-эрозия + blur по этим картам (глобально, один раз)
    if (bEnableErosion && ErosionIterations > 0)
    {
        ApplySlopeErosion(Heights01, ControlResolution, Faces);
    }

    ApplyHeightBlur(Heights01,
                    ControlResolution,
                    Faces,
                    HeightBlurStrength,
                    HeightBlurSharpness,
                    HeightBlurIterations);

    // 3) Копируем по граней в BaseHeightMaps[face]
    for (int32 Face = 0; Face < Faces; ++Face)
    {
        BaseHeightMaps[Face].SetNumUninitialized(FaceVerts);
        float* Dst = BaseHeightMaps[Face].GetData();
        float* Src = Heights01.GetData() + Face * FaceVerts;
        FMemory::Memcpy(Dst, Src, sizeof(float) * FaceVerts);
    }

    bControlMapsBuilt = true;
}
float AProceduralPlanetActor::SampleBaseHeightFaceUV(int32 Face, float U01, float V01) const
{
    if (ControlResolution <= 1 || Face < 0 || Face >= 6 || BaseHeightMaps[Face].Num() == 0)
        return SeaLevel01;

    const int32 Res = ControlResolution;
    const float fx = U01 * (Res - 1);
    const float fy = V01 * (Res - 1);

    const int32 x0 = FMath::Clamp(FMath::FloorToInt(fx), 0, Res - 1);
    const int32 y0 = FMath::Clamp(FMath::FloorToInt(fy), 0, Res - 1);
    const int32 x1 = FMath::Min(x0 + 1, Res - 1);
    const int32 y1 = FMath::Min(y0 + 1, Res - 1);

    const float tx = fx - float(x0);
    const float ty = fy - float(y0);

    const TArray<float>& Map = BaseHeightMaps[Face];

    auto At = [&](int32 X, int32 Y) -> float
    {
        return Map[Y * Res + X];
    };

    const float h00 = At(x0, y0);
    const float h10 = At(x1, y0);
    const float h01 = At(x0, y1);
    const float h11 = At(x1, y1);

    const float h0 = FMath::Lerp(h00, h10, tx);
    const float h1 = FMath::Lerp(h01, h11, tx);
    return FMath::Lerp(h0, h1, ty);
}

float AProceduralPlanetActor::SampleBaseHeight01(const FVector& NormalDir) const
{
    FCubeFaceUV FUV = FaceUVFromDirection(NormalDir);
    return SampleBaseHeightFaceUV(FUV.Face, FUV.U, FUV.V);
}

AProceduralPlanetActor::AProceduralPlanetActor()
{
    PrimaryActorTick.bCanEverTick = false;

    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlanetMesh"));
    SetRootComponent(Mesh);
    Mesh->bUseAsyncCooking = true;
    Mesh->SetCollisionProfileName(TEXT("BlockAll"));
    Mesh->ClearFlags(RF_Transactional); // avoid huge undo/redo payloads when saving large procedural mesh in editor
}

void AProceduralPlanetActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);

#if WITH_EDITOR
    UWorld* World = GetWorld();
    const bool bIsGameWorld =
        World && (World->IsGameWorld() || World->WorldType == EWorldType::PIE);

    if (!bIsGameWorld && !bAutoRebuildInEditor)
    {
        return;
    }
#endif

    BuildPlanet();
}

#if WITH_EDITOR
void AProceduralPlanetActor::RebuildPlanet_Editor()
{
    BuildPlanet();
}
#endif

FVector AProceduralPlanetActor::GetBrushDir() const
{
    return BrushCenterRot.Quaternion().GetForwardVector().GetSafeNormal();
}

// ---------- NOISE INIT ----------

void AProceduralPlanetActor::InitNoise()
{
    auto MakeNoise = []() -> FastNoiseLite*
    {
        return new FastNoiseLite();
    };

    if (!ContinentalNoise)    ContinentalNoise    = MakeNoise();
    if (!MountainNoise)       MountainNoise       = MakeNoise();
    if (!MountainDetailNoise) MountainDetailNoise = MakeNoise();
    if (!HillsNoise)          HillsNoise          = MakeNoise();
    if (!CanyonNoise)         CanyonNoise         = MakeNoise();
    if (!WarpNoiseX)          WarpNoiseX          = MakeNoise();
    if (!WarpNoiseY)          WarpNoiseY          = MakeNoise();
    if (!WarpNoiseZ)          WarpNoiseZ          = MakeNoise();
    if (!OceanNoise)          OceanNoise          = MakeNoise();
    if (!ErosionControlNoise) ErosionControlNoise = MakeNoise();

    const int32 BaseSeed = Seed;

    // --- континенты ---
    ContinentalNoise->SetSeed(BaseSeed + 11);
    ContinentalNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    ContinentalNoise->SetFrequency(ContinentFrequency);
    ContinentalNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
    ContinentalNoise->SetFractalOctaves(4);
    ContinentalNoise->SetFractalLacunarity(2.0f);
    ContinentalNoise->SetFractalGain(0.5f);

    // --- горы ---
    MountainNoise->SetSeed(BaseSeed + 23);
    MountainNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    MountainNoise->SetFrequency(MountainFrequency);
    MountainNoise->SetFractalType(FastNoiseLite::FractalType_Ridged);
    MountainNoise->SetFractalOctaves(5);
    MountainNoise->SetFractalLacunarity(2.0f);
    MountainNoise->SetFractalGain(0.5f);

    // --- детали гор ---
    MountainDetailNoise->SetSeed(BaseSeed + 29);
    MountainDetailNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    MountainDetailNoise->SetFrequency(MountainFrequency * 2.0f);
    MountainDetailNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
    MountainDetailNoise->SetFractalOctaves(4);
    MountainDetailNoise->SetFractalLacunarity(2.0f);
    MountainDetailNoise->SetFractalGain(0.5f);

    // --- холмы ---
    HillsNoise->SetSeed(BaseSeed + 37);
    HillsNoise->SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    HillsNoise->SetFrequency(HillsFrequency);
    HillsNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
    HillsNoise->SetFractalOctaves(3);
    HillsNoise->SetFractalLacunarity(2.0f);
    HillsNoise->SetFractalGain(0.5f);

    // --- каньоны ---
    if (bEnableCanyons)
    {
        CanyonNoise->SetSeed(BaseSeed + 51);
        CanyonNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
        CanyonNoise->SetFrequency(CanyonFrequency);
        CanyonNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
        CanyonNoise->SetFractalOctaves(3);
        CanyonNoise->SetFractalLacunarity(2.0f);
        CanyonNoise->SetFractalGain(0.5f);
    }

    // --- domain warp ---
    if (bEnableDomainWarp)
    {
        WarpNoiseX->SetSeed(BaseSeed + 101);
        WarpNoiseX->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
        WarpNoiseX->SetFrequency(WarpFrequency);
        WarpNoiseX->SetFractalType(FastNoiseLite::FractalType_FBm);
        WarpNoiseX->SetFractalOctaves(3);
        WarpNoiseX->SetFractalLacunarity(2.0f);
        WarpNoiseX->SetFractalGain(0.5f);

        WarpNoiseY->SetSeed(BaseSeed + 133);
        WarpNoiseY->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
        WarpNoiseY->SetFrequency(WarpFrequency);
        WarpNoiseY->SetFractalType(FastNoiseLite::FractalType_FBm);
        WarpNoiseY->SetFractalOctaves(3);
        WarpNoiseY->SetFractalLacunarity(2.0f);
        WarpNoiseY->SetFractalGain(0.5f);

        WarpNoiseZ->SetSeed(BaseSeed + 167);
        WarpNoiseZ->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
        WarpNoiseZ->SetFrequency(WarpFrequency);
        WarpNoiseZ->SetFractalType(FastNoiseLite::FractalType_FBm);
        WarpNoiseZ->SetFractalOctaves(3);
        WarpNoiseZ->SetFractalLacunarity(2.0f);
        WarpNoiseZ->SetFractalGain(0.5f);
    }

    // --- океанская рябь ---
    OceanNoise->SetSeed(BaseSeed + 71);
    OceanNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    OceanNoise->SetFrequency(8.0f);
    OceanNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
    OceanNoise->SetFractalOctaves(2);
    OceanNoise->SetFractalLacunarity(2.0f);
    OceanNoise->SetFractalGain(0.5f);

    // --- карта "уровня эрозии" ---
    ErosionControlNoise->SetSeed(BaseSeed + 61);
    ErosionControlNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    ErosionControlNoise->SetFrequency(ContinentFrequency * 0.6f);
    ErosionControlNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
    ErosionControlNoise->SetFractalOctaves(3);
    ErosionControlNoise->SetFractalLacunarity(2.0f);
    ErosionControlNoise->SetFractalGain(0.5f);
}

// ---------- ФУНКЦИЯ ВЫСОТЫ ----------

float AProceduralPlanetActor::GetHeight01(const FVector& NormalDir) const
{
    // Базовая высота (континенты+эрозия), 0..1
    const float baseH = SampleBaseHeight01(NormalDir);

    // Локальные координаты для шумов (включая optional domain warp)
    FVector P = NormalDir;

    if (bEnableDomainWarp)
    {
        const float wx = WarpNoiseX->GetNoise(P.X, P.Y, P.Z) * WarpStrength;
        const float wy = WarpNoiseY->GetNoise(P.Y, P.Z, P.X) * WarpStrength;
        const float wz = WarpNoiseZ->GetNoise(P.Z, P.X, P.Y) * WarpStrength;
        P += FVector(wx, wy, wz);

        // Дополнительная деформация по континентам
        P += NormalDir * (ContinentWarpStrength * ContinentalNoise->GetNoise(P.Z, P.X, P.Y));
    }

    // --- Маска суши / океана на основе базовой высоты ---
    const float landMask = SmoothStep(SeaLevel01 - ContinentBlend,
                                      SeaLevel01 + ContinentBlend,
                                      baseH);

    // --- Контроль эрозии (где горы острые / сглаженные) ---
    float erosionCtrl = To01(ErosionControlNoise->GetNoise(P.X, P.Y, P.Z));
    erosionCtrl = FMath::Pow(erosionCtrl, 1.3f);
    const float lowErosionMask  = SmoothStep01(1.f - erosionCtrl); // мало эрозии -> острые горы
    const float highErosionMask = 1.f - lowErosionMask;

    // --- Горы (ridged + детали) ---
    float ridged = MountainNoise->GetNoise(P.X, P.Y, P.Z); // [-1,1]
    ridged = 1.f - FMath::Abs(ridged);
    ridged = FMath::Clamp(ridged, 0.f, 1.f);

    float ridgedShaped = FMath::Pow(ridged, MountainMaskPower);

    float detail = To01(MountainDetailNoise->GetNoise(P.X * 2.f, P.Y * 2.f, P.Z * 2.f));
    float detailFactor = FMath::Lerp(0.6f, 1.0f, ridgedShaped);

    float mountainSignal = ridgedShaped * (0.5f + detail * 0.5f) * detailFactor;

    // Маска регионов гор — не из «сырых» континентов, а из базовой высоты
    float mountainRegion = SmoothStep(SeaLevel01 + 0.05f, SeaLevel01 + 0.35f, baseH);
    float mountainMask = landMask * mountainRegion * lowErosionMask;

    float mountains = mountainSignal * mountainMask;

    // --- Холмы ---
    float hillsBase   = To01(HillsNoise->GetNoise(P.X, P.Y, P.Z));
    float hillsSignal = FMath::Pow(hillsBase, 1.2f);

    float hillsMask = landMask * (1.f - mountainMask + highErosionMask * 0.5f);
    hillsMask = FMath::Clamp(hillsMask, 0.f, 1.f);

    float hills = hillsSignal * hillsMask;

    // --- Каньоны ---
    float canyon = 0.f;
    if (bEnableCanyons)
    {
        float c = To01(CanyonNoise->GetNoise(P.X, P.Y, P.Z));
        c = 1.f - c;
        c = FMath::Pow(c, 2.f);
        float canyonMask = landMask * SmoothStep(SeaLevel01 - 0.1f, SeaLevel01 + 0.2f, baseH);
        canyon = c * canyonMask;
    }

    // --- Океанская рябь ---
    float oceanMask = 1.f - landMask;
    float ocean = To01(OceanNoise->GetNoise(P.X * 2.f, P.Y * 2.f, P.Z * 2.f)) * oceanMask;

    // --- Итог ---
    float h = baseH;
    h += mountains * MountainHeight;
    h += hills * HillsHeight;
    h -= canyon * CanyonDepth;
    h += ocean * 0.08f;

    return FMath::Clamp(h, 0.f, 1.f);
}

// ---------- ТЕРМАЛЬНАЯ ЭРОЗИЯ ----------

void AProceduralPlanetActor::ApplySlopeErosion(TArray<float>& Heights01, int32 Res, int32 NumFaces) const
{
    if (!bEnableErosion || ErosionIterations <= 0)
        return;

    const int32 VertsPerFace = Res * Res;
    TArray<float> Temp;
    Temp.SetNumUninitialized(VertsPerFace);

    for (int32 Face = 0; Face < NumFaces; ++Face)
    {
        float* FaceH = Heights01.GetData() + Face * VertsPerFace;

        for (int32 Iter = 0; Iter < ErosionIterations; ++Iter)
        {
            FMemory::Memcpy(Temp.GetData(), FaceH, sizeof(float) * VertsPerFace);

            for (int32 y = 1; y < Res - 1; ++y)
            {
                for (int32 x = 1; x < Res - 1; ++x)
                {
                    const int32 idx = y * Res + x;
                    const float h  = Temp[idx];

                    const int32 NeighIdx[4] =
                    {
                        idx - 1,
                        idx + 1,
                        idx - Res,
                        idx + Res
                    };

                    float maxDiff = 0.f;
                    float sumDiff = 0.f;

                    for (int32 k = 0; k < 4; ++k)
                    {
                        const float dh = h - Temp[NeighIdx[k]];
                        if (dh > 0.f)
                        {
                            maxDiff = FMath::Max(maxDiff, dh);
                            sumDiff += dh;
                        }
                    }

                    if (maxDiff > ErosionTalus && sumDiff > KINDA_SMALL_NUMBER)
                    {
                        const float amount = (maxDiff - ErosionTalus) * ErosionStrength;
                        const float share  = amount / sumDiff;

                        FaceH[idx] -= amount;

                        for (int32 k = 0; k < 4; ++k)
                        {
                            const float dh = h - Temp[NeighIdx[k]];
                            if (dh > 0.f)
                            {
                                FaceH[NeighIdx[k]] += share * dh;
                            }
                        }
                    }
                }
            }
        }
    }
}

// ---------- BILATERAL BLUR (сглаживание высот) ----------

void AProceduralPlanetActor::ApplyHeightBlur(
    TArray<float>& Heights01,
    int32 Res,
    int32 NumFaces,
    float Strength,
    float Sharpness,
    int32 Iterations) const
{
    if (!bEnableHeightBlur || Strength <= 0.f || Iterations <= 0)
        return;

    const int32 VertsPerFace = Res * Res;
    TArray<float> Temp;
    Temp.SetNumUninitialized(VertsPerFace);

    for (int32 Face = 0; Face < NumFaces; ++Face)
    {
        float* FaceH = Heights01.GetData() + Face * VertsPerFace;

        for (int32 Iter = 0; Iter < Iterations; ++Iter)
        {
            FMemory::Memcpy(Temp.GetData(), FaceH, sizeof(float) * VertsPerFace);

            for (int32 y = 1; y < Res - 1; ++y)
            {
                for (int32 x = 1; x < Res - 1; ++x)
                {
                    const int32 idx = y * Res + x;
                    const float center = Temp[idx];

                    float accum = center;
                    float wSum  = 1.f;

                    const int32 NeighIdx[8] =
                    {
                        idx - 1,         // W
                        idx + 1,         // E
                        idx - Res,       // N
                        idx + Res,       // S
                        idx - Res - 1,   // NW
                        idx - Res + 1,   // NE
                        idx + Res - 1,   // SW
                        idx + Res + 1    // SE
                    };

                    for (int32 k = 0; k < 8; ++k)
                    {
                        const float hN = Temp[NeighIdx[k]];
                        const float dh = FMath::Abs(center - hN);
                        const float w  = FMath::Exp(-dh * Sharpness);
                        accum += hN * w;
                        wSum  += w;
                    }

                    const float blurred = accum / FMath::Max(wSum, KINDA_SMALL_NUMBER);
                    FaceH[idx] = FMath::Lerp(center, blurred, Strength);
                }
            }
        }
    }
}

// ---------- ЦВЕТ ----------

FLinearColor AProceduralPlanetActor::HeightToColor(float H) const
{
    const FLinearColor DeepOcean(0.0f, 0.02f, 0.10f);
    const FLinearColor ShallowOcean(0.0f, 0.05f, 0.18f);
    const FLinearColor Beach(0.90f, 0.85f, 0.60f);
    const FLinearColor Grass(0.10f, 0.35f, 0.08f);
    const FLinearColor Rock(0.45f, 0.45f, 0.47f);
    const FLinearColor Snow(0.96f, 0.96f, 1.0f);

    if (H < SeaLevel01 - 0.15f)
    {
        const float t = FMath::GetRangePct(0.f, SeaLevel01 - 0.15f, H);
        return FLinearColor::LerpUsingHSV(DeepOcean, ShallowOcean, t);
    }
    if (H < SeaLevel01 + 0.02f)
    {
        const float t = FMath::GetRangePct(SeaLevel01 - 0.03f, SeaLevel01 + 0.02f, H);
        return FLinearColor::LerpUsingHSV(ShallowOcean, Beach, t);
    }
    if (H < SeaLevel01 + 0.25f)
    {
        const float t = FMath::GetRangePct(SeaLevel01 + 0.02f, SeaLevel01 + 0.25f, H);
        return FLinearColor::LerpUsingHSV(Beach, Grass, t);
    }
    if (H < SeaLevel01 + 0.45f)
    {
        const float t = FMath::GetRangePct(SeaLevel01 + 0.25f, SeaLevel01 + 0.45f, H);
        return FLinearColor::LerpUsingHSV(Grass, Rock, t);
    }

    const float t = FMath::GetRangePct(SeaLevel01 + 0.45f, 1.0f, H);
    return FLinearColor::LerpUsingHSV(Rock, Snow, t);
}

// ---------- ОСНОВНОЙ БИЛД ----------

void AProceduralPlanetActor::BuildPlanet()
{
    if (!Mesh) return;

    UWorld* World = GetWorld();
    const bool bIsGameWorld =
        World && (World->IsGameWorld() || World->WorldType == EWorldType::PIE);

    int32 ChunkResRaw = ChunkResolution;
#if WITH_EDITOR
    if (!bIsGameWorld)
    {
        ChunkResRaw = EditorPreviewResolution;
    }
#endif
    const int32 ChunkRes = FMath::Clamp(ChunkResRaw, 8, 256);

    const int32 NumChunksPerFace = FMath::Max(1, ChunksPerFace);
    const int32 NumFaces = 6;

    const float RadiusCm = FMath::Max(1.f, PlanetRadiusKm * 100000.f);

    Mesh->ClearAllMeshSections();

    InitNoise();
    bControlMapsBuilt = false; // force rebuild so parameter tweaks (SeaLevel, noise, erosion) apply every time
    BuildControlMaps();

    const FVector BrushDir = GetBrushDir();
    const float BrushR    = FMath::Clamp(BrushRadiusDeg,  0.1f, 180.f);
    const float BrushFall = FMath::Clamp(BrushFalloffDeg, 0.1f, 180.f);

    auto ComputeInfluence = [&](const FVector& N)
    {
        if (!bUseLocalizedNoise)
            return 1.f;

        const float CosAng = FVector::DotProduct(N, BrushDir);
        const float AngDeg = FMath::RadiansToDegrees(
            FMath::Acos(FMath::Clamp(CosAng, -1.f, 1.f)));
        const float t = FMath::Clamp(
            (AngDeg - BrushR) / FMath::Max(1e-3f, BrushFall), 0.f, 1.f);
        return 1.f - SmoothStep01(t);
    };

    const bool bCreateCollision = bIsGameWorld;
    // Allow backfaces even in editor so interior is visible; beware of heavier meshes when saving.
    const bool bNeedBackfaces   = bGenerateBackfaces;

    int32 SectionIndex = 0;

    for (int32 Face = 0; Face < NumFaces; ++Face)
    {
        for (int32 ChunkY = 0; ChunkY < NumChunksPerFace; ++ChunkY)
        {
            for (int32 ChunkX = 0; ChunkX < NumChunksPerFace; ++ChunkX, ++SectionIndex)
            {
                TArray<FVector> Verts;
                TArray<int32> Indices;
                TArray<FVector> Normals;
                TArray<FVector2D> UVs;
                TArray<FLinearColor> Colors;
                TArray<FProcMeshTangent> Tangents; // tangents not used now

                const int32 BaseReserve = ChunkRes * ChunkRes;
                Verts.Reserve(BaseReserve * (bNeedBackfaces ? 2 : 1));
                Normals.Reserve(BaseReserve * (bNeedBackfaces ? 2 : 1));
                UVs.Reserve(BaseReserve * (bNeedBackfaces ? 2 : 1));
                Colors.Reserve(BaseReserve * (bNeedBackfaces ? 2 : 1));
                Indices.Reserve((ChunkRes - 1) * (ChunkRes - 1) * 6 * (bNeedBackfaces ? 2 : 1));

                for (int32 y = 0; y < ChunkRes; ++y)
                {
                    const float localV = (float)y / float(ChunkRes - 1);
                    const float v01 = (float(ChunkY) + localV) / float(NumChunksPerFace);

                    for (int32 x = 0; x < ChunkRes; ++x)
                    {
                        const float localU = (float)x / float(ChunkRes - 1);
                        const float u01 = (float(ChunkX) + localU) / float(NumChunksPerFace);

                        const FVector NormalDir = DirectionFromFaceUV(Face, u01, v01);

                        float hRaw01 = SampleHeight01(NormalDir);

                        const float Influence = ComputeInfluence(NormalDir);
                        const float FlatH     = SeaLevel01;
                        const float h01       = FMath::Lerp(FlatH, hRaw01, Influence);

                        const float HeightCm = ComputeHeightCmFrom01(h01);

                        const FVector Pos = NormalDir * (RadiusCm + HeightCm);

                        Verts.Add(Pos);
                        Normals.Add(NormalDir);
                        UVs.Add(FVector2D(u01, v01));
                        Colors.Add(HeightToColor(h01));
                    }
                }

                const int32 BaseVertCount = Verts.Num();

                if (bNeedBackfaces)
                {
                    Verts.Reserve(BaseVertCount * 2);
                    Normals.Reserve(BaseVertCount * 2);
                    UVs.Reserve(BaseVertCount * 2);
                    Colors.Reserve(BaseVertCount * 2);

                    for (int32 i = 0; i < BaseVertCount; ++i)
                    {
                        const FVector Pos    = Verts[i];
                        const FVector Normal = Normals[i];
                        const FVector2D UV   = UVs[i];
                        const FLinearColor C = Colors[i];

                        Verts.Add(Pos);
                        Normals.Add(-Normal); // inward facing
                        UVs.Add(UV);
                        Colors.Add(C);
                    }
                }

                auto AddTriEnsuringOutward = [&](int32 a, int32 b, int32 c)
                {
                    const FVector& va = Verts[a];
                    const FVector& vb = Verts[b];
                    const FVector& vc = Verts[c];
                    const FVector triNormal = FVector::CrossProduct(vb - va, vc - va);
                    const FVector desired   = Normals[a];
                    const bool bCorrectWinding = FVector::DotProduct(triNormal, desired) >= 0.f;
                    if (bCorrectWinding)
                    {
                        Indices.Add(a); Indices.Add(b); Indices.Add(c);
                    }
                    else
                    {
                        Indices.Add(a); Indices.Add(c); Indices.Add(b);
                    }
                };

                for (int32 y = 0; y < ChunkRes - 1; ++y)
                {
                    for (int32 x = 0; x < ChunkRes - 1; ++x)
                    {
                        const int32 i0 = y * ChunkRes + x;
                        const int32 i1 = i0 + 1;
                        const int32 i2 = i0 + ChunkRes;
                        const int32 i3 = i2 + 1;

                        AddTriEnsuringOutward(i0, i2, i3);
                        AddTriEnsuringOutward(i0, i3, i1);

                        if (bNeedBackfaces)
                        {
                            const int32 j0 = i0 + BaseVertCount;
                            const int32 j1 = i1 + BaseVertCount;
                            const int32 j2 = i2 + BaseVertCount;
                            const int32 j3 = i3 + BaseVertCount;

                            AddTriEnsuringOutward(j0, j3, j2);
                            AddTriEnsuringOutward(j0, j1, j3);
                        }
                    }
                }

                Mesh->CreateMeshSection_LinearColor(
                    SectionIndex,
                    Verts,
                    Indices,
                    Normals,
                    UVs,
                    Colors,
                    Tangents,
                    bCreateCollision);

                Mesh->SetMeshSectionVisible(SectionIndex, true);
            }
        }
    }

    if (PlanetMaterial)
    {
        Mesh->SetMaterial(0, PlanetMaterial);
    }
}
float AProceduralPlanetActor::ComputeHeightCmFrom01(float H01) const
{
    const float RadiusCm = FMath::Max(1.f, PlanetRadiusKm * 100000.f);

    float AmpCm = 0.f;
    if (bUseRelativeAmplitude)
    {
        AmpCm = RadiusCm * RelativeAmplitude;
    }
    else
    {
        AmpCm = HeightAmplitudeM * 100.f;
    }

    const float Sea = SeaLevel01;

    const float MaxLandHeightCm  = AmpCm;
    const float MaxOceanDepthCm  = AmpCm * 0.7f;

    if (H01 < Sea)
    {
        const float t = H01 / FMath::Max(Sea, 1e-3f);
        const float depth01 = 1.f - FMath::Pow(t, 1.5f);
        return -depth01 * MaxOceanDepthCm;
    }
    else
    {
        float t = (H01 - Sea) / FMath::Max(1.f - Sea, 1e-3f);
        float t2 = FMath::Pow(t, 1.3f);
        t2 = 1.f - FMath::Pow(1.f - t2, 3.f);
        return t2 * MaxLandHeightCm;
    }
}
float AProceduralPlanetActor::SampleHeight01(const FVector& NormalDir) const
{
    // гарантируем, что всё инициализировано
    AProceduralPlanetActor* Self = const_cast<AProceduralPlanetActor*>(this);

    if (!Self->ContinentalNoise)
    {
        Self->InitNoise();
    }

    if (!Self->bControlMapsBuilt)
    {
        Self->BuildControlMaps();
    }

    return GetHeight01(NormalDir);
}

void AProceduralPlanetActor::SampleHeight(const FVector& NormalDir, float& OutH01, float& OutHeightCm) const
{
    OutH01      = SampleHeight01(NormalDir);
    OutHeightCm = ComputeHeightCmFrom01(OutH01);
}
