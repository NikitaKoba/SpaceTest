// SpaceGlobalCoords.cpp

#include "SpaceGlobalCoords.h"
#include "HAL/IConsoleManager.h"

// CVar, чтобы можно было играться с размером ячейки из консоли:
//   space.CellSizeUU 100000000
// 1uu = 1см, 1км = 100000uu, 1000км = 1e8uu
static TAutoConsoleVariable<float> CVar_Space_CellSizeUU(
	TEXT("space.CellSizeUU"),
	100000000.f, // 1e8 uu = 1000 км
	TEXT("Size of one global cell in UU (1uu = 1cm). Default: 1e8 uu = 1000 km."),
	ECVF_Default
);

// ---------------- FGlobalPos ----------------

FGlobalPos::FGlobalPos(const FVector& WorldLoc)
{
	const double Cell = SpaceGlobal::GetCellSizeUU();
	*this = FromAbsolute(FVector3d(WorldLoc), Cell);
}

FString FGlobalPos::ToString() const
{
	return FString::Printf(
		TEXT("Sector=(%d,%d,%d) Offset=(%.2f, %.2f, %.2f)"),
		Sector.X, Sector.Y, Sector.Z,
		Offset.X, Offset.Y, Offset.Z
	);
}

FGlobalPos FGlobalPos::FromAbsolute(const FVector3d& AbsPos, double CellSizeUU)
{
	FGlobalPos Out;

	if (CellSizeUU <= 0.0)
	{
		// Деградация: всё в Offset, сектор = 0
		Out.Sector = FIntVector::ZeroValue;
		Out.Offset = FVector(
			(float)AbsPos.X,
			(float)AbsPos.Y,
			(float)AbsPos.Z
		);
		return Out;
	}

	const double InvCell = 1.0 / CellSizeUU;

	// integer-сектор — floor(Abs / Cell)
	Out.Sector.X = FMath::FloorToInt(AbsPos.X * InvCell);
	Out.Sector.Y = FMath::FloorToInt(AbsPos.Y * InvCell);
	Out.Sector.Z = FMath::FloorToInt(AbsPos.Z * InvCell);

	const double BaseX = (double)Out.Sector.X * CellSizeUU;
	const double BaseY = (double)Out.Sector.Y * CellSizeUU;
	const double BaseZ = (double)Out.Sector.Z * CellSizeUU;

	Out.Offset.X = (float)(AbsPos.X - BaseX);
	Out.Offset.Y = (float)(AbsPos.Y - BaseY);
	Out.Offset.Z = (float)(AbsPos.Z - BaseZ);

	return Out;
}

FVector3d FGlobalPos::ToAbsolute(double CellSizeUU) const
{
	if (CellSizeUU <= 0.0)
	{
		return FVector3d(Offset);
	}

	const FVector3d SectorD(
		(double)Sector.X,
		(double)Sector.Y,
		(double)Sector.Z
	);

	FVector3d Abs = SectorD * CellSizeUU;
	Abs += FVector3d(Offset);

	return Abs;
}

FVector3d FGlobalPos::Delta(const FGlobalPos& A, const FGlobalPos& B, double CellSizeUU)
{
	const FVector3d AbsA = A.ToAbsolute(CellSizeUU);
	const FVector3d AbsB = B.ToAbsolute(CellSizeUU);
	return AbsB - AbsA;
}

// ---------------- SpaceGlobal ----------------

double SpaceGlobal::GetCellSizeUU()
{
	// Весь геймплей у нас на GameThread, так что этого достаточно
	return (double)CVar_Space_CellSizeUU.GetValueOnGameThread();
}

FGlobalPos SpaceGlobal::WorldToGlobal(const FVector& WorldLoc)
{
	const double Cell = GetCellSizeUU();
	return FGlobalPos::FromAbsolute(FVector3d(WorldLoc), Cell);
}

FVector SpaceGlobal::GlobalToWorld(const FGlobalPos& GP)
{
	const double Cell = GetCellSizeUU();
	const FVector3d Abs = GP.ToAbsolute(Cell);
	return FVector(
		(float)Abs.X,
		(float)Abs.Y,
		(float)Abs.Z
	);
}

FVector3d SpaceGlobal::Delta(const FGlobalPos& A, const FGlobalPos& B)
{
	const double Cell = GetCellSizeUU();
	return FGlobalPos::Delta(A, B, Cell);
}
