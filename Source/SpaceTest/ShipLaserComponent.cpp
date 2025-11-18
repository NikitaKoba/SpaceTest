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

static FRotator RotFromDir(const FVector& D)
{
	return FRotationMatrix::MakeFromX(D.GetSafeNormal()).Rotator();
}

static FVector Jitter(const FVector& D, float Deg)
{
	if (Deg <= KINDA_SMALL_NUMBER) return D.GetSafeNormal();
	return UKismetMathLibrary::RandomUnitVectorInConeInDegrees(D.GetSafeNormal(), Deg).GetSafeNormal();
}
void UShipLaserComponent::FireFromAI(const FVector& AimWorldLocation)
{
	AActor* Ow = GetOwner();
	UWorld* W  = GetWorld();
	if (!Ow || !W)
		return;

	// Только сервер управляет AI-огнём
	if (!Ow->HasAuthority())
		return;

	const double Now    = (double)W->GetTimeSeconds();
	const double Period = 1.0 / FMath::Max(1.0f, FireRateHz);

	// Уважение каденса, как и в ServerFireShot_Implementation
	if ((Now - ServerLastShotTimeS) + 1e-6 < Period - (double)CadenceToleranceSec)
	{
		return;
	}

	ServerLastShotTimeS = Now;

	// Используем уже готовую механику спавна из стволов
	ServerSpawn_FromAimPoint(AimWorldLocation);
}

// === ctor ===
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

	// кэшнём рут-примитив (чтобы унаследовать скорость при спавне болта)
	if (AActor* Ow = GetOwner())
	{
		if (UPrimitiveComponent* P = Cast<UPrimitiveComponent>(Ow->GetRootComponent()))
			CachedRootPrim = P;
	}

	// инициал для client-driven
	if (UWorld* W = GetWorld())
		ClientNextShotTimeS = W->GetTimeSeconds();
}

void UShipLaserComponent::TickComponent(float Dt, ELevelTick TickType, FActorComponentTickFunction* ThisTick)
{
	Super::TickComponent(Dt, TickType, ThisTick);

	const APawn* P  = Cast<APawn>(GetOwner());
	const APlayerController* PC = P ? Cast<APlayerController>(P->GetController()) : nullptr;
	const bool bOwningClient = P && PC && P->IsLocallyControlled();

	// === Client-driven cadence ===
	if (bClientDrivesCadence && bOwningClient && bLocalFireHeld)
	{
		UWorld* W = GetWorld();
		if (!W) return;

		const double Now    = (double)W->GetTimeSeconds();
		const double Period = 1.0 / FMath::Max(1.0f, FireRateHz);

		while (Now + 1e-6 >= ClientNextShotTimeS)
		{
			FVector O, D;
			if (!bUseReticleAim || !ComputeAimRay_Client(O, D))
			{
				// фоллбек — смотрим по камере
				FVector CamLoc; FRotator CamRot;
				PC->GetPlayerViewPoint(CamLoc, CamRot);
				O = CamLoc;
				D = CamRot.Vector();
			}

			// RPC на сервер
			ServerFireShot(O, D);

			ClientNextShotTimeS += Period;
		}
	}
}


void UShipLaserComponent::StartFire()
{
	bLocalFireHeld = true;

	if (bClientDrivesCadence)
	{
		// Для client-driven режима — сбросить фазу, чтобы не было "догоняющих" шотов
		if (UWorld* W = GetWorld())
		{
			ClientNextShotTimeS = W->GetTimeSeconds();
		}
	}
	else
	{
		// Старый режим — сервер сам по таймеру спавнит болты
		if (GetOwner())
		{
			ServerStartFire(); // Server RPC
		}
	}
}

void UShipLaserComponent::StopFire()
{
	bLocalFireHeld = false;

	if (!bClientDrivesCadence && GetOwner())
	{
		ServerStopFire(); // Server RPC
	}
}

// === Aim (client) ===
bool UShipLaserComponent::ComputeAimRay_Client(FVector& OutOrigin, FVector& OutDir) const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!Pawn || !PC || !Pawn->IsLocallyControlled())
		return false;

	// 1) пробуем наш курсор-пилот (экранная точка = центр + CursorSm)
	if (const UShipCursorPilotComponent* CP = Pawn->FindComponentByClass<UShipCursorPilotComponent>())
	{
		if (CP->GetAimRay(OutOrigin, OutDir))
			return true;
	}

	// 2) фоллбек — центр камеры
	FVector CamLoc; FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	OutOrigin = CamLoc;
	OutDir    = CamRot.Vector().GetSafeNormal();
	return true;
}

bool UShipLaserComponent::GetMuzzleTransform(const FName& Socket, FTransform& OutTM) const
{
	if (const UPrimitiveComponent* P = CachedRootPrim.Get())
	{
		if (P->DoesSocketExist(Socket))
		{
			OutTM = P->GetSocketTransform(Socket, ERelativeTransformSpace::RTS_World);
			return true;
		}
	}

	// фоллбек — из позиции/ротации актора
	const AActor* Ow = GetOwner();
	OutTM = FTransform(Ow ? Ow->GetActorRotation() : FRotator::ZeroRotator,
	                   Ow ? Ow->GetActorLocation() : FVector::ZeroVector);
	return false;
}

FVector UShipLaserComponent::DirFromMuzzle(const FVector& MuzzleLoc, const FVector& AimPoint) const
{
	return (AimPoint - MuzzleLoc).GetSafeNormal();
}

// === RPC: мгновенный шот с прицелом ===
void UShipLaserComponent::ServerFireShot_Implementation(
	const FVector_NetQuantize& Origin,
	const FVector_NetQuantizeNormal& Dir)
{
	UWorld* W = GetWorld();
	if (!W) return;

	// ---- анти-скорострел (каденс сервера) ----
	const double Now    = (double)W->GetTimeSeconds();
	const double Period = 1.0 / FMath::Max(1.0f, FireRateHz);

	// маленький допуск на сетевой джиттер
	if ((Now - ServerLastShotTimeS) + 1e-6 < Period - (double)CadenceToleranceSec)
	{
		return; // рано — дропаем
	}

	// нормализуем входы
	const FVector O = (FVector)Origin;
	const FVector D = ((FVector)Dir).GetSafeNormal();

	// ---- серверная валидация луча (анти-чит/ sanity) ----
	if (!ValidateShot(O, D))
	{
		// по желанию можно не обновлять ServerLastShotTimeS,
		// чтобы не позволять «ддосить» проверками. Оставим как есть.
		return;
	}

	// ок — засчитываем шот по времени
	ServerLastShotTimeS = Now;

	// ---- считаем AimPoint ----
	FVector AimPoint = O + D * MaxAimRangeUU;

	if (bServerTraceAim)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(LaserAim), /*bTraceComplex*/true);
		Q.AddIgnoredActor(GetOwner());

		FHitResult Hit;
		const bool bHit = W->LineTraceSingleByChannel(
			Hit, O, O + D * MaxAimRangeUU, ECC_Visibility, Q);

		if (bHit)
		{
			AimPoint = Hit.ImpactPoint;
		}
	}

	// ---- спавн из выбранных стволов в AimPoint ----
	ServerSpawn_FromAimPoint(AimPoint);
}


void UShipLaserComponent::ServerSpawn_FromAimPoint(const FVector& AimPoint)
{
	if (MuzzleSockets.Num() == 0) return;

	auto FireFrom = [&](const FName& Sock)
	{
		FTransform TM; GetMuzzleTransform(Sock, TM);
		const FVector Dir = Jitter(DirFromMuzzle(TM.GetLocation(), AimPoint), AimJitterDeg);
		TM.SetRotation(RotFromDir(Dir).Quaternion());
		Multicast_SpawnBolt(TM);
	};

	if (FirePattern == ELaserFirePattern::AllAtOnce)
	{
		for (const FName& S : MuzzleSockets) FireFrom(S);
	}
	else
	{
		static int32 NextIdx = 0;
		if (NextIdx >= MuzzleSockets.Num()) NextIdx = 0;
		FireFrom(MuzzleSockets[NextIdx++]);
	}
}

// === Multicast: визуальный болт ===
void UShipLaserComponent::Multicast_SpawnBolt_Implementation(const FTransform& SpawnTM)
{
	if (!GetOwner() || !BoltClass) return;

	FActorSpawnParameters Params;
	Params.Owner = GetOwner();
	Params.Instigator = Cast<APawn>(GetOwner());
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ALaserBolt* Bolt = GetWorld()->SpawnActor<ALaserBolt>(BoltClass, SpawnTM, Params);
	if (!Bolt) return;

	// Наследуем скорость носителя
	FVector OwnerVel = FVector::ZeroVector;
	if (const UPrimitiveComponent* P = CachedRootPrim.Get())
		OwnerVel = P->GetComponentVelocity();
	else if (const AActor* Ow = GetOwner())
		OwnerVel = Ow->GetVelocity();

	Bolt->SetBaseVelocity(OwnerVel * Bolt->InheritOwnerVelPct);
}

// === Старый режим (серверный таймер) — оставлен для совместимости ===
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
		W->GetTimerManager().ClearTimer(FireTimer);
}

void UShipLaserComponent::Server_SpawnOnce()
{
	// ИСПОЛЬЗУЕТ ПОСЛЕДНИЙ ПРИСЛАННЫЙ ЛУЧ (не моментальный).
	if (!bUseReticleAim || !bHaveServerAim)
	{
		// просто по forward каждого сокета
		for (const FName& S : MuzzleSockets)
		{
			FTransform TM; GetMuzzleTransform(S, TM);
			Multicast_SpawnBolt(TM);
		}
		return;
	}

	// иначе вычислим AimPoint по ServerAimOrigin/Dir (устаревшая схема)
	FVector AimPoint = ServerAimOrigin + ServerAimDir * MaxAimRangeUU;

	if (bServerTraceAim)
	{
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(LaserAimLegacy), true, GetOwner());
		if (GetWorld()->LineTraceSingleByChannel(Hit, ServerAimOrigin, AimPoint, ECC_Visibility, Q))
			AimPoint = Hit.ImpactPoint;
	}

	ServerSpawn_FromAimPoint(AimPoint);
}
bool UShipLaserComponent::ValidateShot(const FVector& Origin, const FVector& Dir) const
{
	const APawn* P = Cast<APawn>(GetOwner());
	const AController* C = P ? P->GetController() : nullptr;

	// Если это не игрок (нет PlayerController) — не включаем анти-чит.
	// Боты/турели/серверные штуки могут стрелять свободно.
	const APlayerController* PC = C ? Cast<APlayerController>(C) : nullptr;
	if (!P || !PC)
	{
		return true;
	}

	// 1) серверный viewpoint игрока
	FVector ViewLoc = FVector::ZeroVector;
	FRotator ViewRot = FRotator::ZeroRotator;
	PC->GetPlayerViewPoint(ViewLoc, ViewRot);

	const FVector DirNorm = Dir.GetSafeNormal();

	// 2) расстояние Origin от камеры
	// В мультиплеере камера клиента и сервера могут очень расходиться,
	// плюс third-person, плюс наша хитрая CalcCamera → увеличиваем лимит.
	constexpr float MaxOriginDist = 5000.f;
	if (FVector::DistSquared(Origin, ViewLoc) > FMath::Square(MaxOriginDist))
	{
		return false;
	}

	// 3) угол между направлением шота и «вперёд» камеры
	const FVector ViewFwd = ViewRot.Vector();
	constexpr float MaxAngleDeg = 75.f;
	const float CosLimit = FMath::Cos(FMath::DegreesToRadians(MaxAngleDeg));
	const float CosAng   = FVector::DotProduct(DirNorm, ViewFwd);
	if (CosAng < CosLimit)
	{
		return false;
	}

	// 4) ограничение скорости поворота луча между шотами
	static thread_local double PrevTime = 0.0;
	static thread_local FVector PrevDir = FVector::ZeroVector;

	const UWorld* W = GetWorld();
	const double Now = W ? (double)W->GetTimeSeconds() : 0.0;
	if (PrevTime > 0.0)
	{
		const double Dt = FMath::Max(1e-3, Now - PrevTime);
		const double CosD = FMath::Clamp(FVector::DotProduct(PrevDir.GetSafeNormal(), DirNorm), -1.0, 1.0);
		const double dAngDeg = FMath::RadiansToDegrees(FMath::Acos(CosD));

		constexpr double MaxDegPerSec = 1080.0; // до 3 оборотов/сек
		if (dAngDeg / Dt > MaxDegPerSec)
		{
			PrevTime = Now;
			PrevDir  = DirNorm;
			return false;
		}
	}

	PrevTime = Now;
	PrevDir  = DirNorm;
	return true;
}
