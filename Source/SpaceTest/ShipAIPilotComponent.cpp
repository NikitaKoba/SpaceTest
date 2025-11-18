// ShipAIPilotComponent.cpp

#include "ShipAIPilotComponent.h"
#include "FlightComponent.h"
#include "ShipPawn.h"
#include "ShipLaserComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h" // TActorIterator

DEFINE_LOG_CATEGORY_STATIC(LogShipBot, Log, All);

// Рулим ботом только на сервере (или в Standalone)
static FORCEINLINE bool ShouldDriveOnThisMachine(const AActor* Ow)
{
	return Ow && (Ow->HasAuthority() || Ow->GetNetMode() == NM_Standalone);
}

UShipAIPilotComponent::UShipAIPilotComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup    = TG_PrePhysics; // до физики / полётного компонента
	bAutoActivate = true;
}

void UShipAIPilotComponent::BeginPlay()
{
	Super::BeginPlay();

	TryBindComponents();

	if (Flight.IsValid())
	{
		// Для ботов всегда включаем FA, мы работаем по желаемой скорости
		Flight->SetFlightAssistEnabled(true);
		// Важно: чтобы полётный компонент тиковал ПОСЛЕ нас
		Flight->AddTickPrerequisiteComponent(this);
	}
}

void UShipAIPilotComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction
)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AActor* Ow = GetOwner();
	if (!Ow || !ShouldDriveOnThisMachine(Ow))
	{
		// На клиентах бот НЕ рулит — только реплика снапшотов
		return;
	}

	if (!Flight.IsValid() || !Body.IsValid())
	{
		TryBindComponents();
		if (!Flight.IsValid() || !Body.IsValid())
		{
			return;
		}
	}

	UpdateAI(DeltaTime);
}
void UShipAIPilotComponent::UpdateAI_Follow(float Dt, AActor* Target)
{
    bWantsToFireLaser     = false;
    LaserAimWorldLocation = FVector::ZeroVector;

    AActor* Ow = GetOwner();
    if (!Ow || !Target || !Flight.IsValid() || !Body.IsValid())
    {
        ApplyIdleInput();
        return;
    }

    const FVector SelfPos = Body->GetComponentLocation();
    const FVector TgtPos  = Target->GetActorLocation();

    FVector ToTargetW = TgtPos - SelfPos;
    const float Dist = ToTargetW.Size();

    if (Dist < KINDA_SMALL_NUMBER)
    {
        ApplyIdleInput();
        return;
    }

    const FVector DirToTargetW = ToTargetW / Dist; // нормализованный вектор на цель

    const FTransform TM = Body->GetComponentTransform();
    const FVector FwdW  = TM.GetUnitAxis(EAxis::X);
    const FVector RightW= TM.GetUnitAxis(EAxis::Y);
    const FVector UpW   = TM.GetUnitAxis(EAxis::Z);

    // -------------------------
    // 1) ДИСТАНЦИЯ — ПРОСТОЙ PD только по оси вперёд
    // -------------------------

    const FVector Vself_cmps = Body->GetPhysicsLinearVelocity();
    const FVector Vself_mps  = Vself_cmps / 100.f;

    const float Vf_self = FVector::DotProduct(Vself_mps, FwdW);

    const float DesiredDistCm = FollowDistanceCm;
    const float DistErrCm     = Dist - DesiredDistCm;
    const float DistErr_m     = DistErrCm / 100.f;

    const float VxMax = FMath::Max(Flight->Longi.VxMax_Mps, 1.f);

    float Vforward_des_mps = PosKp * DistErr_m - PosKd * Vf_self;
    Vforward_des_mps = FMath::Clamp(Vforward_des_mps, -VxMax, +VxMax);

    float AxisF = Vforward_des_mps / VxMax;

    // Немного ограничим задний ход, чтобы бот не улетал задом как ракета
    if (AxisF < 0.f)
    {
        AxisF = FMath::Clamp(AxisF, -0.4f, 0.8f);
    }

    // И ещё: если цель явно сзади — лучше сначала повернуться, а не разгоняться вперёд
    const float frontDot = FVector::DotProduct(FwdW, DirToTargetW); // 1 = строго перед носом, -1 = строго за спиной
    if (frontDot < 0.f)
    {
        AxisF = FMath::Min(AxisF, 0.2f); // не разгоняемся сильно вперёд, пока цель за кормой
    }

    Flight->SetThrustForward(AxisF);

    // ВАЖНО: в follow-режиме страф и вертикаль не используем
    Flight->SetStrafeRight(0.f);
    Flight->SetThrustUp(0.f);
    Flight->SetRollAxis(0.f);

    // -------------------------
    // 2) ОРИЕНТАЦИЯ — ВЕКТОРНЫЙ КОНТРОЛЛЕР
    // -------------------------

    const float frontDotClamped = FMath::Clamp(frontDot, -1.f, 1.f);
    const float angleToTargetRad = FMath::Acos(frontDotClamped);
    const float angleToTargetDeg = FMath::RadiansToDegrees(angleToTargetRad);

    const float YawRateMax   = Flight->Yaw.YawRateMax_Deg;
    const float PitchRateMax = Flight->Pitch.PitchRateMax_Deg;

    float YawRateCmdDeg   = 0.f;
    float PitchRateCmdDeg = 0.f;
    float RollRateCmdDeg  = 0.f;

    // --- РЕЖИМ "ЦЕЛЬ СИЛЬНО СЗАДИ": избегаем мёртвых петель ---
    const float rearThresholdDeg = 140.f;
    if (angleToTargetDeg > rearThresholdDeg)
    {
        const float sideDot = FVector::DotProduct(DirToTargetW, RightW); // + цель справа, - слева
        const float yawSign = (sideDot >= 0.f) ? 1.f : -1.f;

        YawRateCmdDeg   = yawSign * YawRateMax * 0.7f; // 70% от максимального разворота
        PitchRateCmdDeg = 0.f;
        RollRateCmdDeg  = 0.f;

        Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
        return;
    }

    // --- ОБЫЧНЫЙ РЕЖИМ: кросс-продукт Fwd и DirToTarget ---
    FVector axisWorld = FVector::CrossProduct(FwdW, DirToTargetW);
    const float axisLen = axisWorld.Size();

    if (axisLen < KINDA_SMALL_NUMBER)
    {
        Flight->SetAngularRateOverride(false, 0.f, 0.f, 0.f);
        return;
    }

    const float AlignRateMaxDeg = 1.8f * FMath::Min(YawRateMax, PitchRateMax);
    const float AlignRateMaxRad = FMath::DegreesToRadians(AlignRateMaxDeg);

    FVector omegaWorld = axisWorld * AlignRateMaxRad;
    FVector omegaLocal = TM.InverseTransformVectorNoScale(omegaWorld);

    const float pitchRateRad = omegaLocal.Y;
    const float yawRateRad   = omegaLocal.Z;

    PitchRateCmdDeg = FMath::Clamp(FMath::RadiansToDegrees(pitchRateRad), -PitchRateMax, PitchRateMax);
    YawRateCmdDeg   = FMath::Clamp(FMath::RadiansToDegrees(yawRateRad),   -YawRateMax,   YawRateMax);

    const float AlignScale = FMath::GetMappedRangeValueClamped(
        FVector2D(0.f, 30.f),
        FVector2D(0.6f, 1.0f),
        angleToTargetDeg
    );
    PitchRateCmdDeg *= AlignScale;
    YawRateCmdDeg   *= AlignScale;

    RollRateCmdDeg = 0.f;

    Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
}

void UShipAIPilotComponent::UpdateAI_AttackLaser(float Dt, AActor* Target)
{
    bWantsToFireLaser     = false;
    LaserAimWorldLocation = FVector::ZeroVector;

    AActor* Ow = GetOwner();
    if (!Ow || !Target || !Flight.IsValid() || !Body.IsValid())
    {
        ApplyIdleInput();
        return;
    }

    const FVector SelfPos = Body->GetComponentLocation();
    const FVector TgtPos  = Target->GetActorLocation();

    FVector ToTargetW = TgtPos - SelfPos;
    const float Dist  = ToTargetW.Size();
    if (Dist < KINDA_SMALL_NUMBER)
    {
        ApplyIdleInput();
        return;
    }

    const FVector DirToTargetW = ToTargetW / Dist;

    const FTransform TM   = Body->GetComponentTransform();
    const FVector   FwdW  = TM.GetUnitAxis(EAxis::X);
    const FVector   RightW= TM.GetUnitAxis(EAxis::Y);
    const FVector   UpW   = TM.GetUnitAxis(EAxis::Z);

    const float DistM = Dist * 0.01f;

    // -------------------------
    // 1) Скорость: агрессивный PD по дистанции
    // -------------------------

    const FVector Vself_cmps = Body->GetPhysicsLinearVelocity();
    const FVector Vself_mps  = Vself_cmps / 100.f;

    const float VxMax    = FMath::Max(Flight->Longi.VxMax_Mps, 1.f);
    const float Vf_self  = FVector::DotProduct(Vself_mps, FwdW);

    const float IdealDistM = AttackIdealDistanceCm     * 0.01f;
    const float FarDistM   = AttackFarDistanceCm       * 0.01f;
    const float CloseDistM = AttackTooCloseDistanceCm  * 0.01f;

    float DistErrM = DistM - IdealDistM;

    const float Kp = PosKp * AttackPosKpMul;
    const float Kd = PosKd * AttackPosKdMul;

    float Vforward_des_mps = Kp * DistErrM - Kd * Vf_self;
    Vforward_des_mps = FMath::Clamp(Vforward_des_mps, -VxMax, +VxMax);

    float AxisF = Vforward_des_mps / VxMax;

    // Далеко — жмём газ серьёзнее
    if (DistM > FarDistM)
    {
        AxisF = FMath::Max(AxisF, 0.9f);
    }
    else if (DistM > IdealDistM)
    {
        AxisF = FMath::Max(AxisF, 0.6f);
    }

    // Слишком близко — не разгоняемся как ракета
    if (DistM < CloseDistM)
    {
        AxisF = FMath::Clamp(AxisF, -0.4f, 0.4f);
    }
    else
    {
        AxisF = FMath::Clamp(AxisF, -0.6f, 1.0f);
    }

    float frontDot = FVector::DotProduct(FwdW, DirToTargetW);
    frontDot = FMath::Clamp(frontDot, -1.f, 1.f);
    const float angleRad = FMath::Acos(frontDot);
    const float angleDeg = FMath::RadiansToDegrees(angleRad);

    const float LoopTriggerDeg = AttackLoopTriggerAngleDeg;
    const float MinLoopDistM   = AttackLoopMinDistM;

    const bool bDoLoop = (angleDeg > LoopTriggerDeg && DistM > MinLoopDistM);

    // -------------------------
    // 2) Страф/вертикаль — манёвренность
    // -------------------------

    const float sidePos = FVector::DotProduct(DirToTargetW, RightW); // + справа, - слева
    const float vertPos = FVector::DotProduct(DirToTargetW, UpW);    // + выше,  - ниже

    float AxisStrafe = FMath::Clamp(sidePos * 0.8f, -MaxStrafeRightAxis, MaxStrafeRightAxis);
    float AxisUp     = FMath::Clamp(vertPos * 0.8f, -MaxThrustUpAxis,   MaxThrustUpAxis);

    if (bDoLoop)
    {
        // Во время петли — газ в пол и активно вверх, чтобы реально закидывало
        AxisF      = 1.f;
        AxisUp     = FMath::Clamp(0.8f, -MaxThrustUpAxis, MaxThrustUpAxis);
        AxisStrafe *= 0.3f;
    }

    Flight->SetThrustForward(AxisF);
    Flight->SetStrafeRight(AxisStrafe);
    Flight->SetThrustUp(AxisUp);
    Flight->SetRollAxis(0.f); // пока без банкинга

    // -------------------------
    // 3) ОРИЕНТАЦИЯ — обычная + режим петли
    // -------------------------

    const float YawRateMax   = Flight->Yaw.YawRateMax_Deg;
    const float PitchRateMax = Flight->Pitch.PitchRateMax_Deg;

    float YawRateCmdDeg   = 0.f;
    float PitchRateCmdDeg = 0.f;
    float RollRateCmdDeg  = 0.f;

    if (bDoLoop)
    {
        // Мёртвая петля: крутим pitch на цель, немного yaw
        const float upDot   = FVector::DotProduct(DirToTargetW, UpW);
        const float sideDot = FVector::DotProduct(DirToTargetW, RightW);

        const float pitchSign = (upDot  >= 0.f) ? 1.f : -1.f;
        const float yawSign   = (sideDot>= 0.f) ? 1.f : -1.f;

        PitchRateCmdDeg = pitchSign * PitchRateMax;
        YawRateCmdDeg   = yawSign   * YawRateMax * 0.25f;
        RollRateCmdDeg  = 0.f;

        Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
    }
    else
    {
        FVector axisWorld = FVector::CrossProduct(FwdW, DirToTargetW);
        const float axisLen = axisWorld.Size();

        if (axisLen < KINDA_SMALL_NUMBER)
        {
            Flight->SetAngularRateOverride(false, 0.f, 0.f, 0.f);
        }
        else
        {
            const float AlignRateMaxDeg = 2.2f * FMath::Min(YawRateMax, PitchRateMax);
            const float AlignRateMaxRad = FMath::DegreesToRadians(AlignRateMaxDeg);

            FVector omegaWorld = axisWorld * AlignRateMaxRad;
            FVector omegaLocal = TM.InverseTransformVectorNoScale(omegaWorld);

            const float pitchRateRad = omegaLocal.Y;
            const float yawRateRad   = omegaLocal.Z;

            PitchRateCmdDeg = FMath::Clamp(FMath::RadiansToDegrees(pitchRateRad), -PitchRateMax, PitchRateMax);
            YawRateCmdDeg   = FMath::Clamp(FMath::RadiansToDegrees(yawRateRad),   -YawRateMax,   YawRateMax);

            const float AlignScale = FMath::GetMappedRangeValueClamped(
                FVector2D(0.f, 25.f),
                FVector2D(0.6f, 1.0f),
                angleDeg
            );
            PitchRateCmdDeg *= AlignScale;
            YawRateCmdDeg   *= AlignScale;

            RollRateCmdDeg = 0.f;

            Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
        }
    }

    // -------------------------
    // 4) ЛАЗЕР: конус + дистанция + упреждение + ВЫСТРЕЛ
    // -------------------------

    const bool bInFireDist  = (DistM <= AttackFireMaxDistM);
    const bool bInFireAngle = (angleDeg <= AttackFireAngleDeg);

    if (!bDoLoop && bInFireDist && bInFireAngle)
    {
        const FVector TgtVel_uu   = Target->GetVelocity();            // см/с
        const float   LaserSpeedUU= AttackLaserSpeedMps * 100.f;      // м/с → uu/с

        float leadTime = Dist / FMath::Max(LaserSpeedUU, 1.f);
        leadTime = FMath::Clamp(leadTime, 0.02f, 0.6f);

        const FVector AimPos = TgtPos + TgtVel_uu * leadTime;

        bWantsToFireLaser     = true;
        LaserAimWorldLocation = AimPos;

        // СЕРВЕРНЫЙ ВЫСТРЕЛ через ShipLaserComponent
        if (Ow->HasAuthority())
        {
            if (UShipLaserComponent* Laser = Ow->FindComponentByClass<UShipLaserComponent>())
            {
                Laser->FireFromAI(AimPos);
            }
        }

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
        DrawDebugLine(GetWorld(), SelfPos, AimPos, FColor::Red, false, 0.05f, 0, 2.f);
        DrawDebugSphere(GetWorld(), AimPos, 500.f, 8, FColor::Red, false, 0.05f, 0, 1.f);
#endif
    }
    else
    {
        bWantsToFireLaser     = false;
        LaserAimWorldLocation = FVector::ZeroVector;
    }


}

void UShipAIPilotComponent::TryBindComponents()
{
	AActor* Ow = GetOwner();
	if (!Ow) return;

	if (!Body.IsValid())
	{
		if (auto* Prim = Cast<UPrimitiveComponent>(Ow->GetRootComponent()))
		{
			Body = Prim;
		}
	}

	if (!Flight.IsValid())
	{
		Flight = Ow->FindComponentByClass<UFlightComponent>();
	}

	if (Flight.IsValid())
	{
		Flight->AddTickPrerequisiteComponent(this);
	}
}

AActor* UShipAIPilotComponent::ResolveTarget()
{
	if (TargetActor.IsValid())
	{
		return TargetActor.Get();
	}

	if (bAutoAcquirePlayer)
	{
		TargetActor = FindBestPlayerShip();
	}

	return TargetActor.Get();
}

AActor* UShipAIPilotComponent::FindBestPlayerShip() const
{
	UWorld* World = GetWorld();
	AActor* Ow    = GetOwner();
	if (!World || !Ow) return nullptr;

	const FVector SelfLoc = Ow->GetActorLocation();
	AActor* Best          = nullptr;
	float   BestDistSq    = TNumericLimits<float>::Max();

	// UE5: используем TActorIterator вместо GetPawnIterator
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		APawn* P = *It;
		if (!P || P == Ow) continue;

		AController* C = P->GetController();
		if (!C || !C->IsPlayerController()) continue;

		// Ищем именно наши корабли
		if (!P->IsA(AShipPawn::StaticClass()))
			continue;

		const float DistSq = FVector::DistSquared(SelfLoc, P->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best       = P;
		}
	}

	return Best;
}

void UShipAIPilotComponent::ApplyIdleInput()
{
	if (!Flight.IsValid()) return;

	Flight->SetThrustForward(0.f);
	Flight->SetStrafeRight(0.f);
	Flight->SetThrustUp(0.f);
	Flight->SetRollAxis(0.f);
	Flight->SetAngularRateOverride(false, 0.f, 0.f, 0.f);
}

void UShipAIPilotComponent::UpdateAI(float Dt)
{
	// Здесь мы НЕ занимаемся физикой напрямую.
	// Только выбираем, какую директиву вызвать.

	AActor* Target = ResolveTarget();

	if (bAttackMode)
	{
		UpdateAI_AttackLaser(Dt, Target);
	}
	else
	{
		UpdateAI_Follow(Dt, Target);
	}
}