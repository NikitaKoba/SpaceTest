// SpaceGlobalCoords.h
#pragma once

#include "CoreMinimal.h"
#include "SpaceGlobalCoords.generated.h"

USTRUCT(BlueprintType)
struct SPACETEST_API FGlobalPos
{
    GENERATED_BODY()

public:
    // Целочисленный сектор (в твоих логах Sector=(-24,15,38))
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
    // Один сектор = 1'000'000 UU
    static constexpr double SectorUU = 1000000.0;

    // FGlobalPos -> глобальный FVector3d
    SPACETEST_API FVector3d ToGlobalVector(const FGlobalPos& P);

    // Глобальный FVector3d -> FGlobalPos
    SPACETEST_API void FromGlobalVector(const FVector3d& G, FGlobalPos& Out);
}
