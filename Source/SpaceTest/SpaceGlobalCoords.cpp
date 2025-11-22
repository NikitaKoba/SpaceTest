// SpaceGlobalCoords.cpp

#include "SpaceGlobalCoords.h"
#include "Math/UnrealMathUtility.h"

namespace SpaceGlobal
{
    FVector3d ToGlobalVector(const FGlobalPos& P)
    {
        const double SX = static_cast<double>(P.Sector.X);
        const double SY = static_cast<double>(P.Sector.Y);
        const double SZ = static_cast<double>(P.Sector.Z);

        return FVector3d(
            SX * SectorUU + static_cast<double>(P.Offset.X),
            SY * SectorUU + static_cast<double>(P.Offset.Y),
            SZ * SectorUU + static_cast<double>(P.Offset.Z)
        );
    }

    void FromGlobalVector(const FVector3d& G, FGlobalPos& Out)
    {
        const double Inv = 1.0 / SectorUU;

        const double SXd = FMath::FloorToDouble(G.X * Inv);
        const double SYd = FMath::FloorToDouble(G.Y * Inv);
        const double SZd = FMath::FloorToDouble(G.Z * Inv);

        const int32 SX = static_cast<int32>(SXd);
        const int32 SY = static_cast<int32>(SYd);
        const int32 SZ = static_cast<int32>(SZd);

        const double BaseX = static_cast<double>(SX) * SectorUU;
        const double BaseY = static_cast<double>(SY) * SectorUU;
        const double BaseZ = static_cast<double>(SZ) * SectorUU;

        Out.Sector = FIntVector(SX, SY, SZ);
        Out.Offset.X = static_cast<float>(G.X - BaseX);
        Out.Offset.Y = static_cast<float>(G.Y - BaseY);
        Out.Offset.Z = static_cast<float>(G.Z - BaseZ);
    }

    // Просто враппер: WorldLoc (UU) трактуем как глобальные UU
    void FromWorldLocationUU(const FVector& WorldLocUU, FGlobalPos& Out)
    {
        FromGlobalVector(FVector3d(WorldLocUU), Out);
    }

    // Обратное преобразование: FGlobalPos -> WorldLoc (UU)
    FVector ToWorldLocationUU(const FGlobalPos& P)
    {
        const FVector3d G = ToGlobalVector(P);
        return FVector(
            static_cast<float>(G.X),
            static_cast<float>(G.Y),
            static_cast<float>(G.Z));
    }

    // Инкремент: добавляем дельту в UU к глобальному положению
    void AdvanceByWorldDeltaUU(FGlobalPos& P, const FVector& DeltaWorldUU)
    {
        FVector3d G = ToGlobalVector(P);
        G += FVector3d(DeltaWorldUU);
        FromGlobalVector(G, P);
    }
}
