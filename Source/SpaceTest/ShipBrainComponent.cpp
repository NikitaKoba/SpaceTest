// ShipBrainComponent.cpp (SpaceTest)
#include "ShipBrainComponent.h"
#include "ShipAIPilotComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "CollisionQueryParams.h"

UShipBrainComponent::UShipBrainComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup    = TG_PrePhysics; // чтобы директивы пришли до физики
	bAutoActivate = true;
}

void UShipBrainComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!Pilot.IsValid())
	{
		if (AActor* Ow = GetOwner())
		{
			Pilot = Ow->FindComponentByClass<UShipAIPilotComponent>();
			if (!Pilot.IsValid() && GEngine)
			{
				GEngine->AddOnScreenDebugMessage((uint64)this + 1, 6.f, FColor::Red,
					TEXT("[Brain] Pilot not found on owner! Brain is idle."));
			}
		}
	}
}

bool UShipBrainComponent::ShouldDriveOnThisMachine(const AActor* Ow)
{
	return Ow && (Ow->HasAuthority() || Ow->GetNetMode() == NM_Standalone);
}

void UShipBrainComponent::TickComponent(float Dt, enum ELevelTick, FActorComponentTickFunction*)
{
	AActor* Ow = GetOwner();
	if (!Ow || !Pilot.IsValid()) return;
	if (!ShouldDriveOnThisMachine(Ow)) return;

	FShipDirective D; // пустая по умолчанию

	switch (Mode)
	{
	case EShipBrainMode::Idle:
		{
			// Пустая директива -> пилот сам отпустит инпуты
		} break;

	case EShipBrainMode::Follow:
	case EShipBrainMode::Dogfight: // пока тот же фоллоу, позже добавим упреждение/энергетику
	default:
		{
			BuildDirective_Follow(Dt, D);
		} break;
	}

	// Отдать пилоту
	Pilot->ApplyDirective(Dt, D);

	// Экранный лог по таймауту (удобно видеть режим)
	if (GEngine)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - LastLogWallS >= 1.5)
		{
			LastLogWallS = Now;
			const FString L = FString::Printf(TEXT("[Brain] Mode=%d  Target=%s  Avoid=%d"),
				(int32)Mode, *GetNameSafe(TargetActor.Get()), bEnableAvoidance ? 1 : 0);
			UE_LOG(LogTemp, Display, TEXT("%s"), *L);
			GEngine->AddOnScreenDebugMessage((uint64)this + 2, 1.6f, FColor::Cyan, L);
		}
	}
}


AActor* UShipBrainComponent::ResolveTargetNearestPlayer() const
{
	UWorld* W = GetWorld(); if (!W) return nullptr;
	APawn* SelfP = Cast<APawn>(GetOwner());

	AActor* Best = nullptr; float BestD2 = TNumericLimits<float>::Max();
	for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get(); if (!PC) continue;
		APawn* P = PC->GetPawn(); if (!P || P == SelfP) continue;

		const float d2 = FVector::DistSquared(P->GetActorLocation(), SelfP ? SelfP->GetActorLocation() : FVector::ZeroVector);
		if (d2 < BestD2) { BestD2 = d2; Best = P; }
	}
	return Best;
}

UPrimitiveComponent* UShipBrainComponent::GetSimPrim(const AActor* Ow) const
{
	if (!Ow) return nullptr;
	if (auto* P = Cast<UPrimitiveComponent>(Ow->GetRootComponent()))
		if (P->IsSimulatingPhysics()) return P;
	TArray<UPrimitiveComponent*> Ps; Ow->GetComponents<UPrimitiveComponent>(Ps);
	for (auto* C : Ps) if (C && C->IsSimulatingPhysics()) return C;
	return nullptr;
}

void UShipBrainComponent::BuildDirective_Follow(float Dt, /*out*/FShipDirective& D)
{
	AActor* Ow = GetOwner(); if (!Ow) return;

	// 1) ЦЕЛЬ
	AActor* Tgt = TargetActor.IsValid() ? TargetActor.Get() : (bAutoPickNearestTarget ? ResolveTargetNearestPlayer() : nullptr);
	D.AimActor = Tgt;
	D.AimSocket = AimSocket;

	// Базовые Follow-параметры (пилот их тоже умеет, но мы сразу подсчитаем anchor — нужно для Avoidance)
	D.FollowBehindMetersOverride = FollowBehindMeters;
	D.FollowLeadSecondsOverride  = FollowLeadSeconds;
	D.MinApproachMetersOverride  = MinApproachMeters;

	if (!Tgt)
	{
		// Без цели — отдаём пустую директиву (пилот сбросит инпуты)
		return;
	}

	// 2) Рассчитать follow-anchor (как в твоём ApplyDirective, но тут — чтобы добавить избегание)
	const UPrimitiveComponent* TgtPrim = GetSimPrim(Tgt);
	const FVector vTgtW   = TgtPrim ? TgtPrim->GetComponentVelocity() : FVector::ZeroVector;
	const FVector TgtFwd  = Tgt->GetActorForwardVector();
	const FVector TgtFut  = Tgt->GetActorLocation() + vTgtW * FollowLeadSeconds;
	const float   Back    = FMath::Max(FollowBehindMeters, MinApproachMeters);
	FVector Anchor        = TgtFut - TgtFwd * Back;

	// 3) Избегание (опционально) — сдвигаем anchor
	if (bEnableAvoidance)
	{
		const FRotator Face = Ow->GetActorRotation();
		ApplyAvoidanceOffset(Ow->GetActorLocation(), Face, /*inout*/Anchor);
	}

	D.FollowAnchorW = Anchor;
}

void UShipBrainComponent::ApplyAvoidanceOffset(const FVector& Origin, const FRotator& Facing, /*inout*/FVector& AnchorOut)
{
	UWorld* W = GetWorld(); if (!W) return;

	const FVector Fwd = Facing.Vector();
	const FVector Start = Origin;
	const FVector End   = Origin + Fwd * AvoidanceProbeLength;

	FCollisionQueryParams Q(TEXT("ShipBrain_Avoid"), /*bTraceComplex*/ false, GetOwner());
	Q.AddIgnoredActor(GetOwner());

	TArray<FHitResult> Hits;
	const FCollisionShape Shape = FCollisionShape::MakeSphere(AvoidanceRadius);

	// простой радар вперёд
	const bool bHit = W->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, ECC_WorldStatic, Shape, Q);

	FVector Push = FVector::ZeroVector;
	if (bHit)
	{
		for (const FHitResult& H : Hits)
		{
			const FVector N = H.ImpactNormal.GetSafeNormal();
			const float   DistAlpha = 1.f - FMath::Clamp(H.Distance / AvoidanceProbeLength, 0.f, 1.f);
			Push += N * (AvoidanceStrength * DistAlpha);
		}
	}

	if (!Push.IsNearlyZero())
	{
		const float Clamp = FMath::Max(0.f, AvoidanceOffsetClampMeters);
		const FVector Delta = Push.GetClampedToMaxSize(Clamp * 100.f); // м → uu
		AnchorOut += Delta;

		if (bDrawDebug)
		{
			DrawDebugDirectionalArrow(W, Origin, End, 40.f, FColor::Purple, false, 0.f, 0, 1.5f);
			DrawDebugSphere(W, AnchorOut, 30.f, 12, FColor::Magenta, false, 0.f, 0, 1.5f);
		}
	}
	else if (bDrawDebug)
	{
		DrawDebugDirectionalArrow(W, Origin, End, 40.f, FColor::Blue, false, 0.f, 0, 0.8f);
	}
}
