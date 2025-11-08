// ShipPawn.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "ShipPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UFlightComponent;

/**
 * Базовый корабль: статический меш как корень (с включенной физикой),
 * полётный компонент для сил/моментов, простая камера на спрингарме.
 */
UCLASS(Blueprintable)
class SPACETEST_API AShipPawn : public APawn
{
	GENERATED_BODY()

public:
	AShipPawn();

	// === Components ===
	/** Корневой статикомеш — сюда в BP назначаем модель корабля. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UStaticMeshComponent* ShipMesh;

	/** Полётный компонент — вся физика/управление тут. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UFlightComponent* Flight;

	/** Камера, чтобы сразу удобно тестить. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	USpringArmComponent* SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UCameraComponent* Camera;

	// === APawn ===
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:
	// Axis bindings (перекидываем прямо в UFlightComponent)
	void Axis_ThrustForward(float V);
	void Axis_StrafeRight (float V);
	void Axis_ThrustUp    (float V);
	void Axis_Roll        (float V);

	void Axis_MouseYaw    (float V);   // Turn
	void Axis_MousePitch  (float V);   // LookUp

	// Action
	void Action_ToggleFA();
};
