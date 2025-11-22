// SpaceGlobalCoords.h
#pragma once

#include "CoreMinimal.h"
#include "SpaceGlobalCoords.generated.h"

USTRUCT(BlueprintType)
struct SPACETEST_API FGlobalPos
{
    GENERATED_BODY()

public:
    // Целочисленный сектор
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FIntVector Sector = FIntVector::ZeroValue;

    // Смещение внутри сектора в UU (0..1'000'000)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FVector Offset = FVector::ZeroVector;

    // Удобный ToString для логов
    FString ToString() const
    {
        return FString::Printf(
            TEXT("Sector=(%d,%d,%d) Offset=%s"),
            Sector.X, Sector.Y, Sector.Z,
            *Offset.ToString()
        );
    }
};

namespace SpaceGlobal
{
    // Один сектор = 1'000'000 UU (10 км при 1 UU = 1 см)
    static constexpr double SectorUU = 1000000.0;

    // FGlobalPos -> глобальный FVector3d (ВСЁ в UU)
    SPACETEST_API FVector3d ToGlobalVector(const FGlobalPos& P);

    // Глобальный FVector3d (UU) -> FGlobalPos
    SPACETEST_API void FromGlobalVector(const FVector3d& G, FGlobalPos& Out);

    // Утилиты для работы с WorldLocation (UU)
    SPACETEST_API void   FromWorldLocationUU(const FVector& WorldLocUU, FGlobalPos& Out);
    SPACETEST_API FVector ToWorldLocationUU(const FGlobalPos& P);
    SPACETEST_API void   AdvanceByWorldDeltaUU(FGlobalPos& P, const FVector& DeltaWorldUU);
}
