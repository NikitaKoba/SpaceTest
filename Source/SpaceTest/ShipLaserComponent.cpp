#include "ShipLaserComponent.h"
#include "LaserBolt.h"
#include "ShipCursorPilotComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"
#include "Math/RotationMatrix.h"
#include "Kismet/KismetMathLibrary.h"

// ---- utils ----
static FRotator MakeRotFromDir(const FVector& Dir)
{
	return FRotationMatrix::MakeFromX(Dir.GetSafeNormal()).Rotator();
}

static FVector JitterDir(const FVector& Dir, float Deg)
{
	if (Deg <= KINDA_SMALL_NUMBER) return Dir.GetSafeNormal();
	return UKismetMathLibrary::RandomUnitVectorInConeInDegrees(Dir.GetSafeNormal(), Deg).GetSafeNormal();
}

// ---- component ----
UShipLaserComponent::UShipLaserComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);

	MuzzleSockets = { FName("Muzzle_L"), FName("Muzzle_R") };
	BoltClass     = ALaserBolt::StaticClass();
}

void UShipLaserComponent::BeginPlay()
{
	Super::BeginPlay();
	ResolveRootPrim();
}

void UShipLaserComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const APawn* P  = Cast<APawn>(GetOwner());
	const APlayerController* PC = P ? Cast<APlayerController>(P->GetController()) : nullptr;
	const bool bOwningClient = P && PC && P->IsLocallyControlled();

	if (bOwningClient && bLocalFireHeld && bUseReticleAim)
	{
		MaybeSendAimToServer(false);
	}
}

bool UShipLaserComponent::ResolveRootPrim()
{
	if (CachedRootPrim.IsValid()) return true;

	if (AActor* Ow = GetOwner())
	{
		if (UPrimitiveComponent* P = Cast<UPrimitiveComponent>(Ow->GetRootComponent()))
		{
			CachedRootPrim = P;
			return true;
		}
	}
	return false;
}

void UShipLaserComponent::StartFire()
{
	bLocalFireHeld = true;
	MaybeSendAimToServer(true);

	if (GetOwner()) ServerStartFire();
}

void UShipLaserComponent::StopFire()
{
	bLocalFireHeld = false;

	if (GetOwner()) ServerStopFire();
}

void UShipLaserComponent::ServerStartFire_Implementation()
{
	if (bIsFiring) return;
	bIsFiring = true;

	if (UWorld* W = GetWorld())
	{
		const float Period = 1.0f / FMath::Max(1.f, FireRateHz);
		W->GetTimerManager().SetTimer(FireTimer, this, &UShipLaserComponent::Server_SpawnOnce, Period, true, 0.f);
	}
}

void UShipLaserComponent::ServerStopFire_Implementation()
{
	if (!bIsFiring) return;
	bIsFiring = false;

	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(FireTimer);
	}
}

void UShipLaserComponent::ServerUpdateAim_Implementation(const FVector_NetQuantize& Origin, const FVector_NetQuantizeNormal& Dir)
{
	ServerAimOrigin = Origin;
	ServerAimDir    = Dir;
	bHaveServerAim  = true;
}

bool UShipLaserComponent::GetMuzzleTransform(const FName& Socket, FTransform& OutTM) const
{
	if (!GetOwner()) return false;

	if (UPrimitiveComponent* P = CachedRootPrim.Get())
	{
		if (P->DoesSocketExist(Socket))
		{
			OutTM = P->GetSocketTransform(Socket, ERelativeTransformSpace::RTS_World);
			return true;
		}
	}

	OutTM = FTransform(GetOwner()->GetActorRotation(), GetOwner()->GetActorLocation());
	return false;
}


// ==============================
// ShipLaserComponent.cpp
// ==============================

bool UShipLaserComponent::ComputeAimRay_Client(FVector& OutOrigin, FVector& OutDir) const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!Pawn || !PC || !Pawn->IsLocallyControlled()) return false;

	if (const UShipCursorPilotComponent* CP = Pawn->FindComponentByClass<UShipCursorPilotComponent>())
		if (CP->GetAimRay(OutOrigin, OutDir))
			return true;

	// фоллбек: центр вида
	FVector CamLoc; FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	OutOrigin = CamLoc;
	OutDir    = CamRot.Vector().GetSafeNormal();
	return true;
}


bool UShipLaserComponent::ComputeAimPointOnServer(FVector& OutPoint) const
{
	if (!bHaveServerAim) return false;

	const FVector Start = ServerAimOrigin;
	const FVector End   = Start + ServerAimDir.GetSafeNormal() * MaxAimRangeUU;

	FHitResult Hit;
	FCollisionQueryParams Q(TEXT("LaserAim"), /*bTraceComplex=*/true, GetOwner());
	Q.AddIgnoredActor(GetOwner());

	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Q) && Hit.bBlockingHit)
	{
		OutPoint = Hit.ImpactPoint;
	}
	else
	{
		OutPoint = End;
	}
	return true;
}

void UShipLaserComponent::Server_SpawnOnce()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !BoltClass) return;
	ResolveRootPrim();

	const bool bAimEnabled = bUseReticleAim && bHaveServerAim;

	// Единожды на тик находим реальную точку по лучу клиента
	FVector AimPoint = FVector::ZeroVector;
	if (bAimEnabled && bServerTraceAim)
	{
		if (!ComputeAimPointOnServer(AimPoint))
			return; // нет луча — не стреляем
	}

	auto DirFromMuzzle = [&](const FVector& MuzzleLoc)
	{
		if (!bAimEnabled)     return ServerAimDir.GetSafeNormal(); // худший случай
		if (!bServerTraceAim) return ServerAimDir.GetSafeNormal(); // параллельно лучу
		return (AimPoint - MuzzleLoc).GetSafeNormal();             // сводим каждый ствол в одну точку
	};

	if (MuzzleSockets.Num() == 0) return;

	auto FireFromSocket = [&](const FName& Sock)
	{
		FTransform TM; GetMuzzleTransform(Sock, TM);
		const FVector Dir  = DirFromMuzzle(TM.GetLocation());
		TM.SetRotation(MakeRotFromDir(JitterDir(Dir, AimJitterDeg)).Quaternion());
		Multicast_SpawnBolt(TM);
	};

	if (FirePattern == ELaserFirePattern::AllAtOnce)
		for (const FName& S : MuzzleSockets) FireFromSocket(S);
	else
	{
		if (NextMuzzleIndex >= MuzzleSockets.Num()) NextMuzzleIndex = 0;
		FireFromSocket(MuzzleSockets[NextMuzzleIndex++]);
	}
}


void UShipLaserComponent::Multicast_SpawnBolt_Implementation(const FTransform& SpawnTM)
{
	if (!GetOwner() || !BoltClass) return;

	FActorSpawnParameters Params;
	Params.Owner = GetOwner();
	Params.Instigator = Cast<APawn>(GetOwner());
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ALaserBolt* Bolt = GetWorld()->SpawnActor<ALaserBolt>(BoltClass, SpawnTM, Params);
	if (!Bolt) return;

	// Наследуем скорость стрелка (берём с корневого примитива, если есть)
	FVector OwnerVel = FVector::ZeroVector;
	if (const UPrimitiveComponent* P = CachedRootPrim.Get())
		OwnerVel = P->GetComponentVelocity();
	else if (const AActor* Ow = GetOwner())
		OwnerVel = Ow->GetVelocity();

	Bolt->SetBaseVelocity(OwnerVel * Bolt->InheritOwnerVelPct);
}

void UShipLaserComponent::MaybeSendAimToServer(bool bForce)
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!Pawn || !PC || !Pawn->IsLocallyControlled()) return;

	UWorld* W = GetWorld(); if (!W) return;

	const double Now   = W->GetTimeSeconds();
	const double MinDt = 1.0 / FMath::Max(5.f, AimUpdateHz);
	if (!bForce && (Now - LastAimSendTimeS) < MinDt) return;

	FVector O, D;
	if (ComputeAimRay_Client(O, D))
	{
		LastAimSendTimeS = Now;
		ServerUpdateAim(O, D);
	}
}
