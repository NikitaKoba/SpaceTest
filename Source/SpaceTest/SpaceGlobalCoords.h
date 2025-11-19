// SpaceGlobalCoords.h
#pragma once

#include "CoreMinimal.h"
#include "SpaceGlobalCoords.generated.h"

// ------------------------------
// Глобальная позиция во вселенной
// ------------------------------
//
// Мы делим пространство на кубические "ячейки" (сектора) размером CellSizeUU см.
// FGlobalPos = Sector * CellSizeUU + Offset, где:
//   - Sector: целочисленный индекс ячейки (всё, что далеко)
//   - Offset: локальное смещение внутри ячейки (float, до CellSizeUU по модулю)
//
// При этом игровые акторы живут в обычных world координатах
// (GetActorLocation), а FGlobalPos — параллельный, устойчивый
// к огромным расстояниям, без потери точности.
USTRUCT(BlueprintType)
struct SPACETEST_API FGlobalPos
{
	GENERATED_BODY()

	// Целочисленный индекс "сектора" (ячейки глобальной сетки)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space|Global")
	FIntVector Sector = FIntVector::ZeroValue;

	// Локальное смещение в пределах сектора, в UU (1uu = 1см)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space|Global")
	FVector Offset = FVector::ZeroVector;

	FGlobalPos() = default;

	// Просто удобный конструктор "из мира" (пользуется SpaceGlobal::WorldToGlobal)
	explicit FGlobalPos(const FVector& WorldLoc);

	// Для дебага
	FString ToString() const;

	// Построить из "абсолютной" позиции в UU (double)
	static FGlobalPos FromAbsolute(const FVector3d& AbsPos, double CellSizeUU);

	// Преобразовать в абсолютный вектор (double) в UU
	FVector3d ToAbsolute(double CellSizeUU) const;

	// Вектор от A к B (B - A) в UU (double)
	static FVector3d Delta(const FGlobalPos& A, const FGlobalPos& B, double CellSizeUU);
};

// Набор вспомогательных функций в неймспейсе
namespace SpaceGlobal
{
	// Размер ячейки (в UU). По умолчанию 1e8 uu = 1000 км, но можно менять через CVar.
	SPACETEST_API double GetCellSizeUU();

	// Конвертация из world-space в глобальные координаты
	SPACETEST_API FGlobalPos WorldToGlobal(const FVector& WorldLoc);

	// Конвертация обратно (в данный момент 1:1, без floating origin)
	SPACETEST_API FVector GlobalToWorld(const FGlobalPos& GP);

	// Вектор B - A, удобный алиас к FGlobalPos::Delta
	SPACETEST_API FVector3d Delta(const FGlobalPos& A, const FGlobalPos& B);
}
