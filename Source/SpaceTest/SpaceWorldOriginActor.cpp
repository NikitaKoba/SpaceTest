// SpaceWorldOriginActor.cpp
#include "SpaceWorldOriginActor.h"

#include "Net/UnrealNetwork.h"
#include "SpaceFloatingOriginSubsystem.h"
#include "Engine/World.h"

ASpaceWorldOriginActor::ASpaceWorldOriginActor()
{
	bReplicates           = true;
	bAlwaysRelevant       = true;
	NetUpdateFrequency    = 1.f;
	MinNetUpdateFrequency = 0.2f;

	SetReplicateMovement(false);
}

void ASpaceWorldOriginActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ASpaceWorldOriginActor, OriginWorldUU);
	DOREPLIFETIME(ASpaceWorldOriginActor, OriginGlobalM);
}

void ASpaceWorldOriginActor::ServerSetOrigin(const FVector& NewWorldOriginUU,
											 const FVector3d& NewOriginGlobal)
{
	if (!HasAuthority())
	{
		return;
	}

	// Оба поля трактуем как UU (см), просто OriginGlobalM хранится в float
	OriginWorldUU = NewWorldOriginUU;
	OriginGlobalM = FVector(NewOriginGlobal); // double -> float

	// На сервере сразу обновляем сабсистему
	ApplyToSubsystem();
}


void ASpaceWorldOriginActor::OnRep_Origin()
{
	// На сервере мир уже сдвинут подсистемой FloatingOrigin.
	if (HasAuthority())
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
		{
			const FVector3d NewOriginGlobal(
				static_cast<double>(OriginGlobalM.X),
				static_cast<double>(OriginGlobalM.Y),
				static_cast<double>(OriginGlobalM.Z));

			FO->ApplyReplicatedOrigin(NewOriginGlobal, OriginWorldUU);
		}
	}
}



void ASpaceWorldOriginActor::ApplyToSubsystem()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
	{
		FO->ApplyReplicatedOrigin(
			FVector3d(OriginGlobalM), // глобальный origin в UU
			OriginWorldUU             // world-origin в UU
		);
	}
}

