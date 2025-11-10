// ShipNetComponent.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ShipNetComponent.generated.h"

class AShipPawn;
class UStaticMeshComponent;
class UFlightComponent;

USTRUCT()
struct FControlState
{
	GENERATED_BODY()
	UPROPERTY() uint16 Seq = 0;
	UPROPERTY() float  DeltaTime = 0.f;
	UPROPERTY() float  ThrustF = 0.f;
	UPROPERTY() float  ThrustR = 0.f;
	UPROPERTY() float  ThrustU = 0.f;
	UPROPERTY() float  Roll    = 0.f;
	UPROPERTY() float  MouseX  = 0.f;
	UPROPERTY() float  MouseY  = 0.f;
};

USTRUCT()
struct FNetShipState
{
	GENERATED_BODY()
	UPROPERTY() FVector_NetQuantize100 Loc = FVector::ZeroVector; // см
	UPROPERTY() FQuat                  Rot = FQuat::Identity;
	UPROPERTY() FVector_NetQuantize100 Vel = FVector::ZeroVector; // см/с
	UPROPERTY() float   ServerTime = 0.f; // GetWorld()->TimeSeconds
	UPROPERTY() uint16  LastAckSeq = 0;
};

UCLASS(ClassGroup=(Net), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipNetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipNetComponent();

	// === Тюнинг сети (как было в Pawn) ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	float NetInterpDelay = 0.15f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	float OwnerReconTau = 0.12f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	float OwnerMaxVelNudge = 2500.f; // см/с за тик
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	float OwnerHardSnapDistance = 3000.f; // см

	// Сюда павн прокидывает сырые инпуты для RPC (как раньше)
	void SetLocalAxes(float F, float R, float U, float Roll);
	void AddMouseDelta(float Dx, float Dy);

protected:
	// UActorComponent
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// RPC
	UFUNCTION(Server, Unreliable)
	void Server_SendInput(const FControlState& State);

	UFUNCTION()
	void OnRep_ServerState();

private:
	// Кэш ссылок
	APawn*              OwPawn = nullptr;
	AShipPawn*          Ship   = nullptr;
	UStaticMeshComponent* ShipMesh = nullptr;
	UFlightComponent*     Flight   = nullptr;

	// Локальный накопитель инпута для RPC
	uint16 LocalInputSeq = 0;
	float  AxisF_Cur = 0.f, AxisR_Cur = 0.f, AxisU_Cur = 0.f, AxisRoll_Cur = 0.f;
	float  MouseX_Accum = 0.f, MouseY_Accum = 0.f;

	// Авторитативный снап, реплицируется
	UPROPERTY(ReplicatedUsing=OnRep_ServerState)
	FNetShipState ServerState;

	// Временная синхра и интерп буфер
	bool   bHaveTimeSync = false;
	double ServerTimeToClientTime = 0.0; // Now - ServerTime (EMA)

	struct FInterpNode
	{
		double Time = 0.0; // клиентское Now
		FVector Loc = FVector::ZeroVector;
		FQuat   Rot = FQuat::Identity;
		FVector Vel = FVector::ZeroVector; // см/с
	};
	TArray<FInterpNode> NetBuffer;

	// Мягкая реконсиляция владельца
	FNetShipState OwnerReconTarget;
	bool bHaveOwnerRecon = false;

	// Хелперы
	void UpdatePhysicsSimState();
	void DriveSimulatedProxy();
	void OwnerReconcile_Tick(float DeltaSeconds);
};
