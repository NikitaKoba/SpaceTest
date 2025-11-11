// ShipCursorPilotComponent.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ShipCursorPilotComponent.generated.h"

/**
 * Клиентский HUD-пилот: рисует курсор/индикаторы на Canvas через DebugDrawService
 * и ведёт виртуальный курсор относительно центра экрана.
 * Сейчас компонент только рисует HUD и считает нормализованные смещения.
 * (при желании можно включить управление кораблём флагом bDriveShip).
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipCursorPilotComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipCursorPilotComponent();

	// === Настройки HUD / курсора ===
	/** Максимальный радиус отклонения виртуального курсора (px) */
	UPROPERTY(EditAnywhere, Category="Cursor HUD")
	float MaxDeflectPx = 180.f;

	/** Мёртвая зона (px) — чисто визуально рисуем тонкое кольцо */
	UPROPERTY(EditAnywhere, Category="Cursor HUD")
	float DeadzonePx = 28.f;

	/** Скорость возврата курсора к центру (1/с), 0 — нет возврата */
	UPROPERTY(EditAnywhere, Category="Cursor HUD")
	float ReturnToCenterRate = 1.5f;

	/** Гладкость сглаживания курсора (Гц) */
	UPROPERTY(EditAnywhere, Category="Cursor HUD")
	float SmoothHz = 12.f;

	/** Масштаб перевода дельты мыши в пиксели курсора */
	UPROPERTY(EditAnywhere, Category="Cursor HUD")
	float MouseToPixels = 1.0f;

	/** Включить отрисовку HUD */
	UPROPERTY(EditAnywhere, Category="Cursor HUD")
	bool bDebugDraw = true;

	/** Проброс управления кораблём (пока по умолчанию выкл.) */
	UPROPERTY(EditAnywhere, Category="Drive")
	bool bDriveShip = false;

	// Нормализованные смещения курсора [-1..1] в локальных экранных осях (X вправо, Y вверх)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Drive")
	FVector2D NormalizedDeflect = FVector2D::ZeroVector;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	// DebugDrawService
	void OnDebugDraw(class UCanvas* Canvas, class APlayerController* PC);

	// Хелперы рисования
	void DrawCircle(class UCanvas* Canvas, const FVector2D& Center, float Radius, int32 Segments, const FLinearColor& Color, float Thickness=1.f) const;
	void DrawCrosshair(class UCanvas* Canvas, const FVector2D& C, float len, const FLinearColor& Color, float Thickness=1.f) const;

	// Внутреннее состояние курсора
	FVector2D CursorRaw = FVector2D::ZeroVector; // не сглаженный
	FVector2D CursorSm  = FVector2D::ZeroVector; // сглаженный

	// Делегат
	FDelegateHandle DebugDrawHandle;

	// Экспоненциальное сглаживание (в терминах Гц)
	static FORCEINLINE float AlphaFromHz(float Hz, float Dt)
	{
		return (Hz > 0.f && Dt > KINDA_SMALL_NUMBER) ? (1.f - FMath::Exp(-Hz * Dt)) : 1.f;
	}
};
