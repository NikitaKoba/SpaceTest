#include "PlanetLocalPatchActor.h"

APlanetLocalPatchActor::APlanetLocalPatchActor()
{
    PrimaryActorTick.bCanEverTick = false;

    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PatchMesh"));
    SetRootComponent(Mesh);

    Mesh->bUseAsyncCooking = true;
    Mesh->SetCollisionProfileName(TEXT("BlockAll"));
}

void APlanetLocalPatchActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);

    if (bAttachToPlanetCenter && Planet)
    {
        // чтобы локальные координаты совпадали с планетой
        SetActorLocation(Planet->GetActorLocation());
    }

    BuildPatch();
}

#if WITH_EDITOR
void APlanetLocalPatchActor::RebuildPatch_Editor()
{
    BuildPatch();
}
#endif

void APlanetLocalPatchActor::BuildTangentBasis(const FVector& CenterDir, FVector& OutRight, FVector& OutUp) const
{
    FVector UpRef = FVector::UpVector;
    if (FMath::Abs(FVector::DotProduct(UpRef, CenterDir)) > 0.99f)
    {
        UpRef = FVector::RightVector;
    }

    OutRight = FVector::CrossProduct(UpRef, CenterDir).GetSafeNormal();
    OutUp    = FVector::CrossProduct(CenterDir, OutRight).GetSafeNormal();
}

void APlanetLocalPatchActor::BuildPatch()
{
    if (!Planet || !Mesh)
    {
        return;
    }

    const int32 Res = FMath::Clamp(PatchResolution, 8, 2048);
    const int32 NumVerts = Res * Res;

    const float PlanetRadiusKm = Planet->GetPlanetRadiusKm();
    const float PlanetRadiusCm = Planet->GetPlanetRadiusCm();
    const FVector PlanetCenter = Planet->GetActorLocation();

    // угол по дуге: arc = R * angle
    const float PatchAngleRad = PatchRadiusKm / FMath::Max(PlanetRadiusKm, 1e-3f);

    // направление центра патча
    FVector CenterDir = PatchCenterRot.Quaternion().GetForwardVector().GetSafeNormal();
    if (!CenterDir.IsNormalized() || CenterDir.IsNearlyZero())
    {
        CenterDir = FVector::ForwardVector;
    }

    FVector Right, Up;
    BuildTangentBasis(CenterDir, Right, Up);

    // Буферы
    TArray<FVector> Verts;
    TArray<int32> Indices;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;
    TArray<FProcMeshTangent> Tangents;
    TArray<FVector> Dirs;        // направления на сфере
    TArray<float> Heights01;     // высота [0..1] до эрозии/блюра

    Verts.SetNumUninitialized(NumVerts);
    Normals.SetNumUninitialized(NumVerts);
    UVs.SetNumUninitialized(NumVerts);
    Colors.SetNumUninitialized(NumVerts);
    Dirs.SetNumUninitialized(NumVerts);
    Heights01.SetNumUninitialized(NumVerts);

    Mesh->ClearAllMeshSections();

    // 1) собираем направления + сырую высоту (как в планете до эрозии)
    int32 vIndex = 0;
    for (int32 y = 0; y < Res; ++y)
    {
        const float v01 = (Res <= 1) ? 0.f : float(y) / float(Res - 1);
        const float sy = v01 * 2.f - 1.f; // [-1..1]

        for (int32 x = 0; x < Res; ++x, ++vIndex)
        {
            const float u01 = (Res <= 1) ? 0.f : float(x) / float(Res - 1);
            const float sx = u01 * 2.f - 1.f; // [-1..1]

            const float AngleX = sx * PatchAngleRad;
            const float AngleY = sy * PatchAngleRad;

            FVector Dir = CenterDir;

            if (FMath::Abs(AngleX) > KINDA_SMALL_NUMBER)
            {
                const FQuat RotX(Up, AngleX);
                Dir = RotX.RotateVector(Dir);
            }
            if (FMath::Abs(AngleY) > KINDA_SMALL_NUMBER)
            {
                const FQuat RotY(Right, -AngleY);
                Dir = RotY.RotateVector(Dir);
            }

            Dir.Normalize();
            Dirs[vIndex] = Dir;

            // берём ровно тот же шум, что и планета
            const float h01 = Planet->GetHeight01(Dir);
            Heights01[vIndex] = h01;

            UVs[vIndex] = FVector2D(u01, v01); // локальные UV патча
        }
    }

    // 2) применяем ту же эрозию / blur, что и планета
    const int32 NumFaces = 1; // патч = одна "грань"
    if (Planet->bEnableErosion && Planet->ErosionIterations > 0)
    {
        Planet->ApplySlopeErosion(Heights01, Res, NumFaces);
    }

    if (Planet->bEnableHeightBlur && Planet->HeightBlurIterations > 0)
    {
        Planet->ApplyHeightBlur(
            Heights01,
            Res,
            NumFaces,
            Planet->HeightBlurStrength,
            Planet->HeightBlurSharpness,
            Planet->HeightBlurIterations
        );
    }

    // 3) превращаем высоты в вершины + цвета
    for (int32 i = 0; i < NumVerts; ++i)
    {
        const float h01 = FMath::Clamp(Heights01[i], 0.f, 1.f);
        const float HeightCm = Planet->ComputeHeightCmFrom01(h01);

        const FVector Dir = Dirs[i];
        const FVector WorldPos = PlanetCenter + Dir * (PlanetRadiusCm + HeightCm);
        const FVector LocalPos = WorldPos - GetActorLocation();

        Verts[i]   = LocalPos;
        Normals[i] = Dir;
        Colors[i]  = Planet->HeightToColor(h01);
    }

    // 4) индексы
    Indices.Reset();
    Indices.Reserve((Res - 1) * (Res - 1) * 6);

    for (int32 y = 0; y < Res - 1; ++y)
    {
        for (int32 x = 0; x < Res - 1; ++x)
        {
            const int32 i0 = y * Res + x;
            const int32 i1 = y * Res + (x + 1);
            const int32 i2 = (y + 1) * Res + x;
            const int32 i3 = (y + 1) * Res + (x + 1);

            Indices.Add(i0); Indices.Add(i2); Indices.Add(i3);
            Indices.Add(i0); Indices.Add(i3); Indices.Add(i1);
        }
    }

    const bool bCreateCollision = true;
    Mesh->CreateMeshSection_LinearColor(
        0,
        Verts,
        Indices,
        Normals,
        UVs,
        Colors,
        Tangents,
        bCreateCollision
    );

    Mesh->SetMeshSectionVisible(0, true);

    if (PatchMaterial)
    {
        Mesh->SetMaterial(0, PatchMaterial);
    }
    else if (Planet->PlanetMaterial)
    {
        Mesh->SetMaterial(0, Planet->PlanetMaterial);
    }
}
