#include "ShipLaserComponent.h"
#include "LaserBolt.h"
#include "LaserBeam.h"
#include "ShipPawn.h"
#include "ShipCursorPilotComponent.h"
#include "SpaceFloatingOriginSubsystem.h"

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
	const float  EffectiveHz = FireRateHz * FMath::Max(0.05f, AIFireRateScale);
	const double Period = 1.0 / FMath::Max(1.0f, EffectiveHz);

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
	BeamClass     = ALaserBeam::StaticClass();
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
			FGlobalPos OriginGlobal;
			if (USpaceFloatingOriginSubsystem* FO = W->GetSubsystem<USpaceFloatingOriginSubsystem>())
			{
				FO->WorldToGlobal(O, OriginGlobal);
			}
			else
			{
				SpaceGlobal::FromWorldLocationUU(O, OriginGlobal);
			}
			ServerFireShot(OriginGlobal, D);

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
	const FGlobalPos& Origin,
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

	// Shot origin converted via GlobalPos so server/world origin mismatch does not hide bolts
	FVector OriginWorld = SpaceGlobal::ToWorldLocationUU(Origin);
	if (USpaceFloatingOriginSubsystem* FO = W->GetSubsystem<USpaceFloatingOriginSubsystem>())
	{
		OriginWorld = FO->GlobalToWorld(Origin);
	}

	const FVector O = OriginWorld;
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

	const bool bSpawnBolt = (VisualMode == ELaserVisualMode::Bolts || VisualMode == ELaserVisualMode::BoltsAndBeam);
	const bool bSpawnBeam = (VisualMode == ELaserVisualMode::ContinuousBeam || VisualMode == ELaserVisualMode::BoltsAndBeam);

	auto FireFrom = [&](const FName& Sock)
	{
		FTransform TM; GetMuzzleTransform(Sock, TM);
		const FVector MuzzleLoc = TM.GetLocation();
		const FVector Dir = Jitter(DirFromMuzzle(MuzzleLoc, AimPoint), AimJitterDeg);
		TM.SetRotation(RotFromDir(Dir).Quaternion());
		float BeamLengthUU = FMath::Clamp((AimPoint - MuzzleLoc).Size(), 10.f, MaxAimRangeUU);

		// ИСПРАВЛЕНО: Используем FGlobalPos для надежной передачи координат
		if (UWorld* W = GetWorld())
		{
			FHitResult Hit;
			if (GetOwner())
			{
				FCollisionQueryParams Q(SCENE_QUERY_STAT(LaserDamage), true, GetOwner());
				Q.AddIgnoredActor(GetOwner());

				const FVector TraceEnd = MuzzleLoc + Dir * MaxAimRangeUU;
				if (W->LineTraceSingleByChannel(Hit, MuzzleLoc, TraceEnd, ECC_Visibility, Q))
				{
					BeamLengthUU = FMath::Clamp(Hit.Distance, 10.f, MaxAimRangeUU);
				}
			}

			if (!Hit.bBlockingHit)
			{
				BeamLengthUU = FMath::Clamp((AimPoint - MuzzleLoc).Size(), 10.f, MaxAimRangeUU);
			}

			if (USpaceFloatingOriginSubsystem* FO = W->GetSubsystem<USpaceFloatingOriginSubsystem>())
			{
				FGlobalPos GlobalPos;
				FO->WorldToGlobal(TM.GetLocation(), GlobalPos);
				if (bSpawnBolt)
				{
					Multicast_SpawnBolt(GlobalPos, TM.GetRotation().Rotator());
				}
				if (bSpawnBeam)
				{
					Multicast_SpawnBeam(GlobalPos, TM.GetRotation().Rotator(), BeamLengthUU);
				}
			}
			else
			{
				// Fallback без FloatingOrigin
				FGlobalPos GlobalPos;
				SpaceGlobal::FromWorldLocationUU(TM.GetLocation(), GlobalPos);
				if (bSpawnBolt)
				{
					Multicast_SpawnBolt(GlobalPos, TM.GetRotation().Rotator());
				}
				if (bSpawnBeam)
				{
					Multicast_SpawnBeam(GlobalPos, TM.GetRotation().Rotator(), BeamLengthUU);
				}
			}
		}
	};

	if (FirePattern == ELaserFirePattern::AllAtOnce)
	{
		for (const FName& S : MuzzleSockets) FireFrom(S);
	}
	else
	{
		if (NextMuzzleIndex >= MuzzleSockets.Num()) NextMuzzleIndex = 0;
		FireFrom(MuzzleSockets[NextMuzzleIndex++]);
	}
}


// ИСПРАВЛЕНО: Multicast с FGlobalPos
void UShipLaserComponent::Multicast_SpawnBolt_Implementation(
	const FGlobalPos& GlobalPos, 
	const FRotator& Rot)
{
	if (!GetOwner() || !BoltClass) return;

	FActorSpawnParameters Params;
	Params.Owner = GetOwner();
	Params.Instigator = Cast<APawn>(GetOwner());
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	FTransform SpawnTM;
	SpawnTM.SetRotation(Rot.Quaternion());

	// КРИТИЧНО: Преобразуем FGlobalPos в world coordinates через FloatingOrigin
	if (UWorld* W = GetWorld())
	{
		if (USpaceFloatingOriginSubsystem* FO = W->GetSubsystem<USpaceFloatingOriginSubsystem>())
		{
			SpawnTM.SetLocation(FO->GlobalToWorld(GlobalPos));
		}
		else
		{
			// Fallback: напрямую из GlobalPos
			SpawnTM.SetLocation(SpaceGlobal::ToWorldLocationUU(GlobalPos));
		}
	}

	const AShipPawn* OwnerShip = Cast<AShipPawn>(GetOwner());
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const bool bOwnerIsPlayer = OwnerPawn && OwnerPawn->GetController() && OwnerPawn->GetController()->IsPlayerController();
	const int32 OwnerTeam = OwnerShip ? OwnerShip->GetTeamId() : INDEX_NONE;
	const float DamageToApply = DamagePerShot * (bOwnerIsPlayer ? 1.f : FMath::Clamp(AIDamageScale, 0.05f, 1.0f));

	ALaserBolt* Bolt = GetWorld()->SpawnActor<ALaserBolt>(BoltClass, SpawnTM, Params);
	if (!Bolt) return;

	Bolt->ConfigureDamage(DamageToApply, GetOwner(), OwnerTeam);

	// Наследуем скорость владельца
	FVector OwnerVel = FVector::ZeroVector;
	if (const UPrimitiveComponent* P = CachedRootPrim.Get())
		OwnerVel = P->GetComponentVelocity();
	else if (const AActor* Ow = GetOwner())
		OwnerVel = Ow->GetVelocity();

	Bolt->SetBaseVelocity(OwnerVel * Bolt->InheritOwnerVelPct);

	// ДИАГНОСТИКА: Логируем позицию спавна
	UE_LOG(LogTemp, Verbose,
		TEXT("[BOLT SPAWN] %s | GlobalPos=Sector(%d,%d,%d) Offset(%s) | WorldLoc=%s | Owner=%s"),
		*GetNameSafe(Bolt),
		GlobalPos.Sector.X, GlobalPos.Sector.Y, GlobalPos.Sector.Z,
		*GlobalPos.Offset.ToString(),
		*SpawnTM.GetLocation().ToString(),
		*GetNameSafe(GetOwner()));
}

void UShipLaserComponent::Multicast_SpawnBeam_Implementation(
	const FGlobalPos& GlobalPos,
	const FRotator& Rot,
	float BeamLengthUU)
{
	if (!GetOwner() || !BeamClass) return;

	FActorSpawnParameters Params;
	Params.Owner = GetOwner();
	Params.Instigator = Cast<APawn>(GetOwner());
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	FTransform SpawnTM;
	SpawnTM.SetRotation(Rot.Quaternion());

	if (UWorld* W = GetWorld())
	{
		if (USpaceFloatingOriginSubsystem* FO = W->GetSubsystem<USpaceFloatingOriginSubsystem>())
		{
			SpawnTM.SetLocation(FO->GlobalToWorld(GlobalPos));
		}
		else
		{
			SpawnTM.SetLocation(SpaceGlobal::ToWorldLocationUU(GlobalPos));
		}
	}

	ALaserBeam* Beam = GetWorld()->SpawnActor<ALaserBeam>(BeamClass, SpawnTM, Params);
	if (!Beam) return;

	Beam->ConfigureBeam(BeamLengthUU, BeamDurationSec);

	UE_LOG(LogTemp, Verbose,
		TEXT("[BEAM SPAWN] %s | GlobalPos=Sector(%d,%d,%d) Offset(%s) | WorldLoc=%s | Owner=%s | Length=%.1f"),
		*GetNameSafe(Beam),
		GlobalPos.Sector.X, GlobalPos.Sector.Y, GlobalPos.Sector.Z,
		*GlobalPos.Offset.ToString(),
		*SpawnTM.GetLocation().ToString(),
		*GetNameSafe(GetOwner()),
		BeamLengthUU);
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

// === ИСПРАВЛЕНО: Старый режим (серверный таймер) ===
void UShipLaserComponent::Server_SpawnOnce()
{
	const bool bSpawnBolt = (VisualMode == ELaserVisualMode::Bolts || VisualMode == ELaserVisualMode::BoltsAndBeam);
	const bool bSpawnBeam = (VisualMode == ELaserVisualMode::ContinuousBeam || VisualMode == ELaserVisualMode::BoltsAndBeam);
	// Legacy fire path when server doesn't have an aim ray (kept for safety).
	if (!bUseReticleAim || !bHaveServerAim)
	{
		// Fire straight forward from every muzzle
		for (const FName& S : MuzzleSockets)
		{
			FTransform TM; GetMuzzleTransform(S, TM);
			const float BeamLengthUU = MaxAimRangeUU;
			// Multicast uses FGlobalPos
			if (UWorld* W = GetWorld())
			{
				if (USpaceFloatingOriginSubsystem* FO = W->GetSubsystem<USpaceFloatingOriginSubsystem>())
				{
					FGlobalPos GlobalPos;
					FO->WorldToGlobal(TM.GetLocation(), GlobalPos);
					if (bSpawnBolt)
					{
						Multicast_SpawnBolt(GlobalPos, TM.GetRotation().Rotator());
					}
					if (bSpawnBeam)
					{
						Multicast_SpawnBeam(GlobalPos, TM.GetRotation().Rotator(), BeamLengthUU);
					}
				}
				else
				{
					FGlobalPos GlobalPos;
					SpaceGlobal::FromWorldLocationUU(TM.GetLocation(), GlobalPos);
					if (bSpawnBolt)
					{
						Multicast_SpawnBolt(GlobalPos, TM.GetRotation().Rotator());
					}
					if (bSpawnBeam)
					{
						Multicast_SpawnBeam(GlobalPos, TM.GetRotation().Rotator(), BeamLengthUU);
					}
				}
			}
		}
		return;
	}

	// Rebuild AimPoint from ServerAimOrigin/Dir (legacy path)
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

bool UShipLaserComponent::ValidateShot(const FVector& Origin, const FVector& Dir)
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
	
	

	const UWorld* W = GetWorld();
	const double Now = W ? (double)W->GetTimeSeconds() : 0.0;
	if (PrevValidateTimeS > 0.0)
	{
		const double Dt = FMath::Max(1e-3, Now - PrevValidateTimeS);
		const double CosD = FMath::Clamp(FVector::DotProduct(PrevValidateDir.GetSafeNormal(), DirNorm), -1.0, 1.0);
		const double dAngDeg = FMath::RadiansToDegrees(FMath::Acos(CosD));

		constexpr double MaxDegPerSec = 1080.0; // до 3 оборотов/сек
		if (dAngDeg / Dt > MaxDegPerSec)
		{
			PrevValidateTimeS = Now;
			PrevValidateDir  = DirNorm;
			return false;
		}
	}

	PrevValidateTimeS = Now;
	PrevValidateDir  = DirNorm;
	return true;
}





