// SpaceGlobalCoords.h
#pragma once

#include "CoreMinimal.h"
#include "SpaceGlobalCoords.generated.h"

USTRUCT(BlueprintType)
struct SPACETEST_API FIntVector64
{
    GENERATED_BODY()

public:
    FIntVector64() = default;
    FIntVector64(int64 InX, int64 InY, int64 InZ) : X(InX), Y(InY), Z(InZ) {}

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int64 X = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int64 Y = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int64 Z = 0;

    static FIntVector64 Zero() { return FIntVector64(0, 0, 0); }

    FString ToString() const
    {
        return FString::Printf(TEXT("(%lld,%lld,%lld)"), X, Y, Z);
    }
};

USTRUCT(BlueprintType)
struct SPACETEST_API FGlobalPos
{
    GENERATED_BODY()

public:
    // Сектор (крупная клетка координат)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FIntVector64 Sector = FIntVector64::Zero();

    // Смещение внутри сектора в UU (0..1'000'000)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FVector Offset = FVector::ZeroVector;

    // Человеческое представление
    FString ToString() const
    {
        return FString::Printf(
            TEXT("Sector=(%lld,%lld,%lld) Offset=%s"),
            Sector.X, Sector.Y, Sector.Z,
            *Offset.ToString()
        );
    }
};

namespace SpaceGlobal
{
    // Размер сектора в UU (10 км при 1 UU = 1 см)
    static constexpr double SectorUU = 1000000.0;

    // FGlobalPos -> глобальный FVector3d (ВСЁ в UU)
    SPACETEST_API FVector3d ToGlobalVector(const FGlobalPos& P);

    // Глобальный FVector3d (UU) -> FGlobalPos
    SPACETEST_API void FromGlobalVector(const FVector3d& G, FGlobalPos& Out);

    // Утилиты для преобразования к/из WorldLocation (UU)
    SPACETEST_API void   FromWorldLocationUU(const FVector& WorldLocUU, FGlobalPos& Out);
    SPACETEST_API FVector ToWorldLocationUU(const FGlobalPos& P);
    SPACETEST_API void   AdvanceByWorldDeltaUU(FGlobalPos& P, const FVector& DeltaWorldUU);
}
