// SpaceWorldOriginActor.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpaceWorldOriginActor.generated.h"

class USpaceFloatingOriginSubsystem;

UCLASS()
class SPACETEST_API ASpaceWorldOriginActor : public AActor
{
	GENERATED_BODY()

public:
	ASpaceWorldOriginActor();

	// World-origin в UU, куда "привязана" текущая сцена
	UPROPERTY(ReplicatedUsing=OnRep_Origin)
	FVector_NetQuantize OriginWorldUU;

	// Глобальный origin в "метрах" (или в твоей глобальной системе)
	UPROPERTY(ReplicatedUsing=OnRep_Origin)
	FVector_NetQuantize100 OriginGlobalM;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Вызывать ТОЛЬКО на сервере, когда FO пересчитал origin
	void ServerSetOrigin(const FVector& NewWorldOriginUU,
						 const FVector3d& NewOriginGlobal);

protected:
	UFUNCTION()
	void OnRep_Origin();

	// Применить текущие Origin* к сабсистеме FO (на сервере и клиентах)
	void ApplyToSubsystem();
};
