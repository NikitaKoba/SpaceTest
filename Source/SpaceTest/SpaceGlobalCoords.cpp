// SpaceGlobalCoords.cpp

#include "SpaceGlobalCoords.h"
#include "Math/UnrealMathUtility.h"

namespace SpaceGlobal
{
	FVector3d ToGlobalVector(const FGlobalPos& P)
	{
		const double SX = (double)P.Sector.X;
		const double SY = (double)P.Sector.Y;
		const double SZ = (double)P.Sector.Z;

		return FVector3d(
			SX * SectorUU + (double)P.Offset.X,
			SY * SectorUU + (double)P.Offset.Y,
			SZ * SectorUU + (double)P.Offset.Z
		);
	}

	void FromGlobalVector(const FVector3d& G, FGlobalPos& Out)
	{
		const double Inv = 1.0 / SectorUU;

		const double SXd = FMath::Floor(G.X * Inv);
		const double SYd = FMath::Floor(G.Y * Inv);
		const double SZd = FMath::Floor(G.Z * Inv);

		const int32 SX = (int32)SXd;
		const int32 SY = (int32)SYd;
		const int32 SZ = (int32)SZd;

		const double BaseX = (double)SX * SectorUU;
		const double BaseY = (double)SY * SectorUU;
		const double BaseZ = (double)SZ * SectorUU;

		Out.Sector = FIntVector(SX, SY, SZ);
		Out.Offset.X = (float)(G.X - BaseX);
		Out.Offset.Y = (float)(G.Y - BaseY);
		Out.Offset.Z = (float)(G.Z - BaseZ);
	}
}
