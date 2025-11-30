// ShipAIPilotComponent.cpp

#include "ShipAIPilotComponent.h"
#include "FlightComponent.h"
#include "ShipPawn.h"
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
static TAutoConsoleVariable<int32> CVar_ShipBot_DebugOrbit(
    TEXT("ship.bot.debug.orbit"),
    0,
    TEXT("Enable orbit-break debug logs for ship bots (0=off, 1=on)")
);

static TAutoConsoleVariable<int32> CVar_ShipBot_DebugDraw(
    TEXT("ship.bot.debug.draw"),
    0,
    TEXT("Draw debug aim gizmos for ship bots (0=off, 1=on)")
);
static TAutoConsoleVariable<float> CVar_ShipBot_TargetRefresh(
	TEXT("ship.bot.target.refresh"),
	0.35f,
	TEXT("Seconds between rebuilding global ship list cache for AI target selection (per world)"));

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

    // Сдвигаем фазу мозгового тика, чтобы боты думали не в один кадр
    BrainTimeAccumulator = FMath::FRandRange(0.f, BrainUpdateInterval);

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

    // --- "Редкий мозг": тяжёлый AI апдейт не каждый кадр ---
    // FlightComponent всё равно тикает каждый кадр и применяет прошлые оси управления.
    BrainTimeAccumulator += DeltaTime;

    const float EffectiveInterval = FMath::Max(BrainUpdateInterval, 0.01f); // защита от нуля/отрицательных

    if (BrainTimeAccumulator < EffectiveInterval)
    {
        // В этом кадре только продолжаем лететь по уже выставленным осям
        return;
    }

    const float BrainDt = BrainTimeAccumulator;
    BrainTimeAccumulator = 0.f;

    UpdateAI(BrainDt);
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
    TickDogfightStyle(Dt);

    const FTransform TM   = Body->GetComponentTransform();
    const FVector   SelfPos = TM.GetLocation();
    const FVector   FwdW    = TM.GetUnitAxis(EAxis::X);
    const FVector   RightW  = TM.GetUnitAxis(EAxis::Y);
    const FVector   UpW     = TM.GetUnitAxis(EAxis::Z);

    const FVector TgtPos    = Target->GetActorLocation();
    const FVector TgtFwdW   = Target->GetActorForwardVector();
    const FVector TgtRightW = Target->GetActorRightVector();

    bool bIsHeavyShip = false;
    if (AShipPawn* Ship = Cast<AShipPawn>(Ow))
    {
        bIsHeavyShip = (Ship->ShipRole == EShipRole::Corvette);
    }

    // Desired anchor relative to the target (understands above/below/side).
    FVector AnchorPos = TgtPos + FVector(0.f, 0.f, FollowHeightCm);
    const float BehindDistCm = bIsHeavyShip ? HeavyBehindDistanceCm : FollowDistanceCm;
    AnchorPos -= TgtFwdW * BehindDistCm;
    if (bIsHeavyShip)
    {
        AnchorPos += TgtRightW * HeavyLateralOffsetCm;
    }

    FVector ToAnchorW = AnchorPos - SelfPos;
    const float DistToAnchor = ToAnchorW.Size();
    if (DistToAnchor < KINDA_SMALL_NUMBER)
    {
        ApplyIdleInput();
        return;
    }

    FVector ToTargetW = TgtPos - SelfPos;
    const float DistToTarget = ToTargetW.Size();
    const FVector DirToTargetW = (DistToTarget > KINDA_SMALL_NUMBER) ? (ToTargetW / DistToTarget) : FwdW;

    const FVector Vself_cmps = Body->GetPhysicsLinearVelocity();
    const FVector Vself_mps  = Vself_cmps / 100.f;
    const FVector TgtVel_cmps= Target->GetVelocity();
    const FVector TgtVel_mps = TgtVel_cmps / 100.f;
    const FVector Vrel_mps   = TgtVel_mps - Vself_mps;

    const float VxMax = FMath::Max(Flight->Longi.VxMax_Mps, 1.f);
    const float VrMax = FMath::Max(Flight->Lateral.VrMax_Mps, 1.f);
    const float VuMax = FMath::Max(Flight->Vertical.VuMax_Mps, 1.f);
    const float MaxStrafe = bIsHeavyShip ? HeavyMaxStrafeRightAxis : MaxStrafeRightAxis;
    const float MaxUp     = bIsHeavyShip ? HeavyMaxThrustUpAxis    : MaxThrustUpAxis;

    const float ErrF_m = FVector::DotProduct(ToAnchorW, FwdW)  / 100.f;
    const float ErrR_m = FVector::DotProduct(ToAnchorW, RightW)/ 100.f;
    const float ErrU_m = FVector::DotProduct(ToAnchorW, UpW)   / 100.f;

    const float Vf_self = FVector::DotProduct(Vself_mps, FwdW);
    const float Vr_self = FVector::DotProduct(Vself_mps, RightW);
    const float Vu_self = FVector::DotProduct(Vself_mps, UpW);

    const float Kp = PosKp;
    const float Kd = PosKd;

    float AxisF = FMath::Clamp((Kp * ErrF_m - Kd * Vf_self) / VxMax, -1.f, 1.f);
    float AxisR = FMath::Clamp((Kp * ErrR_m - Kd * Vr_self) / VrMax, -MaxStrafe, MaxStrafe);
    float AxisU = FMath::Clamp((Kp * ErrU_m - Kd * Vu_self) / VuMax, -MaxUp,     MaxUp);

    // Brake near anchor to avoid overshooting or looping around.
    const float BrakeDist = FMath::Max(10.f, BrakeDistanceCm);
    if (DistToAnchor < BrakeDist)
    {
        const float BrakeAlpha = FMath::Clamp(DistToAnchor / BrakeDist, 0.f, 1.f);
        AxisF *= BrakeAlpha;
        AxisR *= BrakeAlpha;
        AxisU *= BrakeAlpha;
    }
    // If we are almost at anchor, stop driving forward and only strafe/up.
    if (DistToAnchor < BrakeDist * 0.5f)
    {
        AxisF = 0.f;
    }

    const float frontDot = FVector::DotProduct(FwdW, DirToTargetW);
    if (frontDot < 0.f)
    {
        // Turn-in-place bias when target is behind us.
        const float turnScale = FMath::GetMappedRangeValueClamped(
            FVector2D(-1.f, 0.f),
            FVector2D(0.f, 1.f),
            frontDot);
        AxisF = FMath::Clamp(AxisF, -0.35f, turnScale * (bIsHeavyShip ? HeavyMaxForwardAxis : 0.55f));
        AxisR *= 0.6f;
        AxisU *= 0.6f;
    }

    if (bIsHeavyShip)
    {
        AxisF = FMath::Clamp(AxisF, -0.5f, HeavyMaxForwardAxis);
    }

    if (bStaticGunship)
    {
        const float clampAxis = FMath::Clamp(StaticGunshipAxisClamp, 0.01f, 0.5f);
        AxisF = FMath::Clamp(AxisF, -clampAxis, clampAxis);
        AxisR = FMath::Clamp(AxisR, -clampAxis, clampAxis);
        AxisU = FMath::Clamp(AxisU, -clampAxis, clampAxis);
    }

    Flight->SetThrustForward(AxisF);
    Flight->SetStrafeRight(AxisR);
    Flight->SetThrustUp(AxisU);
    Flight->SetRollAxis(0.f);

    // -------------------------
    // 2) Orientation: face the target without looping
    // -------------------------

    FVector AimDir = DirToTargetW;
    if (bIsHeavyShip)
    {
        FVector planar = DirToTargetW;
        planar.Z = 0.f;
        if (!planar.IsNearlyZero())
        {
            AimDir = planar.GetSafeNormal();
        }
    }

    const float alignDot = FVector::DotProduct(FwdW, AimDir);
    const float alignDotClamped = FMath::Clamp(alignDot, -1.f, 1.f);
    const float angleToTargetRad = FMath::Acos(alignDotClamped);
    const float angleToTargetDeg = FMath::RadiansToDegrees(angleToTargetRad);

    const float YawRateMax   = Flight->Yaw.YawRateMax_Deg;
    const float PitchRateMax = Flight->Pitch.PitchRateMax_Deg;

    float YawRateCmdDeg   = 0.f;
    float PitchRateCmdDeg = 0.f;
    float RollRateCmdDeg  = 0.f;

    const float rearThresholdDeg = 135.f;
    const float sideDot = FVector::DotProduct(DirToTargetW, RightW);
    const float upDot   = FVector::DotProduct(DirToTargetW, UpW);

    if (bIsHeavyShip && bDisableRearLoopForHeavy && angleToTargetDeg > rearThresholdDeg)
    {
        const float yawSign = (sideDot >= 0.f) ? 1.f : -1.f;
        YawRateCmdDeg   = yawSign * YawRateMax * 0.75f;
        PitchRateCmdDeg = 0.f;
        RollRateCmdDeg  = 0.f;

        Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
        return;
    }
    else if (!bIsHeavyShip && angleToTargetDeg > rearThresholdDeg)
    {
        const float yawSign = (sideDot >= 0.f) ? 1.f : -1.f;
        YawRateCmdDeg   = yawSign * YawRateMax * 0.7f;
        PitchRateCmdDeg = 0.f;
        RollRateCmdDeg  = 0.f;

        Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
        return;
    }

    // --- Alignment axis: rotate Fwd toward DirToTarget ---
    FVector axisWorld = FVector::CrossProduct(FwdW, AimDir);
    const float axisLen = axisWorld.Size();

    if (axisLen < KINDA_SMALL_NUMBER)
    {
        Flight->SetAngularRateOverride(false, 0.f, 0.f, 0.f);
        return;
    }

    const float AlignRateMaxDeg = (bIsHeavyShip ? HeavyAlignRateFactor : AlignRateFactor) * FMath::Min(YawRateMax, PitchRateMax);
    const float AlignRateMaxRad = FMath::DegreesToRadians(AlignRateMaxDeg);

    FVector omegaWorld = axisWorld * AlignRateMaxRad;
    FVector omegaLocal = TM.InverseTransformVectorNoScale(omegaWorld);

    const float pitchRateRad = omegaLocal.Y;
    const float yawRateRad   = omegaLocal.Z;

    PitchRateCmdDeg = FMath::Clamp(FMath::RadiansToDegrees(pitchRateRad), -PitchRateMax, PitchRateMax);
    YawRateCmdDeg   = FMath::Clamp(FMath::RadiansToDegrees(yawRateRad),   -YawRateMax,   YawRateMax);

    const float AlignScale = FMath::GetMappedRangeValueClamped(
        FVector2D(0.f, 45.f),
        FVector2D(0.5f, bIsHeavyShip ? 0.85f : 1.0f),
        angleToTargetDeg
    );
    PitchRateCmdDeg *= AlignScale;
    YawRateCmdDeg   *= AlignScale;

    // Damp pitch when target is above/below and mostly behind to avoid loops.
    if (bIsHeavyShip && angleToTargetDeg > rearThresholdDeg * 0.8f && FMath::Abs(upDot) > 0.4f)
    {
        PitchRateCmdDeg *= 0.5f;
    }

    // Extra clamp so corvettes do not loop; uses per-ship fractions.
    const float PitchFrac = bIsHeavyShip ? HeavyPitchRateFraction : FollowPitchRateFraction;
    const float YawFrac   = bIsHeavyShip ? HeavyYawRateFraction   : FollowYawRateFraction;
    PitchRateCmdDeg = FMath::Clamp(
        PitchRateCmdDeg,
        -PitchRateMax * PitchFrac,
        +PitchRateMax * PitchFrac);
    YawRateCmdDeg = FMath::Clamp(
        YawRateCmdDeg,
        -YawRateMax * YawFrac,
        +YawRateMax * YawFrac);

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

    // Обновляем стиль боя (но будем сильно резать его, когда сядем на хвост)
    TickDogfightStyle(Dt);

    const FTransform TM   = Body->GetComponentTransform();
    const FVector   SelfPos = TM.GetLocation();
    const FVector   FwdW    = TM.GetUnitAxis(EAxis::X);
    const FVector   RightW  = TM.GetUnitAxis(EAxis::Y);
    const FVector   UpW     = TM.GetUnitAxis(EAxis::Z);

    const FVector TgtPos    = Target->GetActorLocation();
    FVector       ToTargetW = TgtPos - SelfPos;
    const float   Dist      = ToTargetW.Size();
    if (Dist < KINDA_SMALL_NUMBER)
    {
        ApplyIdleInput();
        return;
    }

    const FVector DirToTargetW = ToTargetW / Dist;
    const FVector TgtFwdW      = Target->GetActorForwardVector();

    if (!bAimDirInit)
    {
        AimDirSmooth = DirToTargetW;
        bAimDirInit  = true;
    }

    const AShipPawn* SelfShip = Cast<AShipPawn>(Ow);
    const int32 SelfTeam = SelfShip ? SelfShip->GetTeamId() : INDEX_NONE;

    bool bIsHeavyShip = false;
    if (SelfShip)
    {
        bIsHeavyShip = (SelfShip->ShipRole == EShipRole::Corvette);
    }

    // --- Дистанции в метрах ---
    const float DistM      = Dist * 0.01f;
    const float IdealDistM = AttackIdealDistanceCm    * 0.01f;
    const float FarDistM   = AttackFarDistanceCm      * 0.01f;
    const float CloseDistM = AttackTooCloseDistanceCm * 0.01f;
    const float AvoidDistM = FMath::Max(AvoidHeadOnDistanceCm * 0.01f, CloseDistM);

    // --- Скорости ---
    const FVector Vself_cmps = Body->GetPhysicsLinearVelocity();
    const FVector Vself_mps  = Vself_cmps / 100.f;
    const FVector TgtVel_cmps= Target->GetVelocity();
    const FVector TgtVel_mps = TgtVel_cmps / 100.f;
    const FVector Vrel_mps   = TgtVel_mps - Vself_mps;

    const float VxMax     = FMath::Max(Flight->Longi.VxMax_Mps, 1.f);
    const float VrMax     = FMath::Max(Flight->Lateral.VrMax_Mps, 1.f);
    const float VuMax     = FMath::Max(Flight->Vertical.VuMax_Mps, 1.f);
    const float MaxStrafe = bIsHeavyShip ? HeavyMaxStrafeRightAxis : MaxStrafeRightAxis;
    const float MaxUp     = bIsHeavyShip ? HeavyMaxThrustUpAxis    : MaxThrustUpAxis;

    const float MinForwardAxis =
        bIsHeavyShip
            ? FMath::Min(HeavyMaxForwardAxis, AggressiveMinForwardAxis)
            : AggressiveMinForwardAxis;

    // --- Геометрия / относительное положение ---
    const float selfFrontDot    = FVector::DotProduct(FwdW,    DirToTargetW);      // 1 = цель по курсу
    const float targetFacingUs  = FVector::DotProduct(TgtFwdW, -DirToTargetW);     // >0 = он смотрит на нас
    const float closingSpeedMps = -FVector::DotProduct(Vrel_mps, DirToTargetW);    // >0 = сближаемся

    // === Корректный детектор "мы у него на хвосте" ===
    // DirToTargetW: от нас к цели. Если мы СЗАДИ, то он примерно совпадает с TgtFwdW.
    const float behindDot = FVector::DotProduct(DirToTargetW, TgtFwdW);

    // Используем твои TailLock-настройки из конфига
    const float tailDistMin = IdealDistM * TailLockDistMinFactor; // 0.65
    const float tailDistMax = IdealDistM * TailLockDistMaxFactor; // 2.0

    const bool bInTailConeGeom =
        (behindDot    > TailLockTargetFacingDot) &&   // 0.25 = мы реально позади
        (selfFrontDot > TailLockFrontDot);            // 0.55 = мы смотрим на цель

    const bool bInTailDistance =
        (DistM > tailDistMin) &&
        (DistM < tailDistMax);

    const bool bTailClosingOk =
        (FMath::Abs(closingSpeedMps) < TailLockMaxClosingSpeedMps);

    const bool bInTailCone = bInTailConeGeom && bInTailDistance && bTailClosingOk;

    // Гистерезис: если были в tail-chase недавно, держим его ещё TailLockStickyTime
    if (bInTailCone)
    {
        TailLockStickyTimer = TailLockStickyTime;
    }
    else if (TailLockStickyTimer > 0.f)
    {
        TailLockStickyTimer = FMath::Max(0.f, TailLockStickyTimer - Dt);
    }

    // --- Детектор "орбитального танца" + Break State Machine ---
    // OrbitStuckTime > 0: накапливаем детекцию орбиты
    // OrbitStuckTime < 0: активный break маневр + cooldown

    const float OrbitDetectThreshold  = 0.5f + (float(Ow->GetUniqueID() % 100) * 0.01f);  // 0.5-1.5 сек с асинхронностью
    const float OrbitBreakDuration    = 2.5f + FMath::FRandRange(0.f, 1.5f);              // 2.5-4 сек break
    const float OrbitCooldownDuration = 3.f;                                              // 3 сек cooldown после break

    const float OrbitSideDotLimit      = 0.55f;
    const float OrbitMinRadialSpeedMps = 25.f;
    const float OrbitDistBandMin       = IdealDistM * 0.5f;
    const float OrbitDistBandMax       = IdealDistM * 2.2f;

    const bool bOrbitGeom =
        DistM > OrbitDistBandMin &&
        DistM < OrbitDistBandMax &&
        FMath::Abs(selfFrontDot) < OrbitSideDotLimit;

    const bool bOrbitRadialSlow = FMath::Abs(closingSpeedMps) < OrbitMinRadialSpeedMps;

    const FVector ToTargetNorm = DirToTargetW;
    const FVector CrossVel = FVector::CrossProduct(ToTargetNorm, Vself_mps);
    const float TangentialSpeedMps = CrossVel.Size();
    const bool bHighTangentialSpeed = (TangentialSpeedMps > 40.f);

    // Состояние break'а: OrbitStuckTime < 0
    const bool bInOrbitBreak = (OrbitStuckTime < -0.01f);

    if (bInOrbitBreak)
    {
        // В режиме break - отсчитываем время обратно к 0
        const float oldTime = OrbitStuckTime;
        OrbitStuckTime += Dt;

        // Когда досчитали до cooldown порога - переходим в cooldown
        if (OrbitStuckTime > -OrbitCooldownDuration)
        {
            OrbitStuckTime = FMath::Min(OrbitStuckTime, -0.01f);  // Остаемся в cooldown

            if (oldTime <= -OrbitCooldownDuration && OrbitStuckTime > -OrbitCooldownDuration)
            {
                if (CVar_ShipBot_DebugOrbit.GetValueOnGameThread() != 0)
                {
                    UE_LOG(LogShipBot, Log, TEXT("[BOT %s] OrbitBreak -> Cooldown (%.1f sec remaining)"),
                        *Ow->GetName(), -OrbitStuckTime);
                }
            }
        }
    }
    else if (bOrbitGeom && (bOrbitRadialSlow || bHighTangentialSpeed) && !bInTailCone)
    {
        // Накапливаем детекцию
        OrbitStuckTime += Dt;

        // Триггер: переходим в break mode
        if (OrbitStuckTime > OrbitDetectThreshold)
        {
            if (CVar_ShipBot_DebugOrbit.GetValueOnGameThread() != 0)
            {
                UE_LOG(LogShipBot, Warning, TEXT("[BOT %s] ORBIT DETECTED! Starting OrbitBreak (%.1f sec)"),
                    *Ow->GetName(), OrbitBreakDuration);
            }

            OrbitStuckTime = -(OrbitBreakDuration + OrbitCooldownDuration);  // Break + cooldown
        }
    }
    else
    {
        // Сбрасываем детекцию только если не в break/cooldown
        if (OrbitStuckTime > 0.f)
        {
            OrbitStuckTime = 0.f;
        }
    }

    // --- Выбор mode'а ---
    enum class EAttackMode : uint8
    {
        HeadOnAvoid,
        TooCloseBreak,
        OrbitBreak,
        StrafePass,          // НОВЫЙ: пролет мимо цели с огнем
        AggressivePursuit,   // НОВЫЙ: прямая атака без точного наведения
        EvasiveApproach,     // НОВЫЙ: сближение со змейкой
        TailChase,
        Approach
    };
    EAttackMode Mode = EAttackMode::Approach;

    // Принудительный OrbitBreak если в активной фазе (до cooldown)
    const float BreakCooldownStart = -OrbitCooldownDuration;
    const bool bInActiveBreak = (OrbitStuckTime < BreakCooldownStart);

    // head-on: оба смотрят друг на друга
    const bool bHeadOnGeometry =
        (selfFrontDot   > AvoidHeadOnFrontDot) &&
        (targetFacingUs > AvoidHeadOnFrontDot);

    const bool bHeadOnDanger =
        bHeadOnGeometry &&
        (DistM < AvoidDistM || closingSpeedMps > AvoidHeadOnClosingSpeedMps);

    if (bHeadOnDanger)
    {
        Mode = EAttackMode::HeadOnAvoid;
    }
    else if (DistM < CloseDistM * 0.9f && !bInActiveBreak)
    {
        Mode = EAttackMode::TooCloseBreak;
    }
    else if (bInActiveBreak)  // Принудительный break пока таймер активен
    {
        Mode = EAttackMode::OrbitBreak;
    }
    else if (bInTailCone)
    {
        Mode = EAttackMode::TailChase;
    }
    else
    {
        // === ВЫБОР СТИЛЯ АТАКИ НА ОСНОВЕ DOGFIGHT STYLE ===
        // Больше разнообразия - не все должны делать одно и то же!

        const bool bMediumRange = (DistM > IdealDistM * 0.8f && DistM < IdealDistM * 2.0f);
        const bool bLongRange   = (DistM >= IdealDistM * 2.0f);

        switch (CurrentDogfightStyle)
        {
        case EDogfightStyle::BoomAndZoom:
            // Boom & Zoom = пролет мимо на высокой скорости
            if (bMediumRange || bLongRange)
            {
                Mode = EAttackMode::StrafePass;
            }
            else
            {
                Mode = EAttackMode::Approach;
            }
            break;

        case EDogfightStyle::FlankLeft:
        case EDogfightStyle::FlankRight:
            // Фланговая атака = змейка при сближении
            if (bMediumRange)
            {
                Mode = EAttackMode::EvasiveApproach;
            }
            else
            {
                Mode = EAttackMode::Approach;
            }
            break;

        case EDogfightStyle::Pursuit:
        default:
            // Прямое преследование = агрессивная атака без точного наведения
            if (bMediumRange || bLongRange)
            {
                Mode = EAttackMode::AggressivePursuit;
            }
            else
            {
                Mode = EAttackMode::Approach;
            }
            break;
        }
    }

    // Если недавно сидели на хвосте — держимся за ним, пока не критично
    if (TailLockStickyTimer > 0.f && !bHeadOnDanger)
    {
        Mode = EAttackMode::TailChase;
    }

    // --- Управляющие величины ---
    float  AxisF      = 0.f;
    float  AxisStrafe = 0.f;
    float  AxisUp     = 0.f;
    bool   bInBreakAway = false;   // в этих режимах не стреляем
    FVector AimDir    = DirToTargetW;

    // Направление "орбиты" / фланга
    const float orbitDir =
        (CurrentOrbitSign != 0.f)
            ? CurrentOrbitSign
            : ((FMath::RandRange(0, 1) == 0) ? -1.f : 1.f);

    // Базовые стилистические смещения (но в хвостовом режиме их почти вырубаем)
    float styleStrafeBias  = 0.f;
    float styleUpBias      = 0.f;
    float styleForwardBias = 0.f;

    switch (CurrentDogfightStyle)
    {
    case EDogfightStyle::Pursuit:
        styleForwardBias = 0.15f;  // чуть больше форсаж
        break;
    case EDogfightStyle::FlankLeft:
    case EDogfightStyle::FlankRight:
        styleStrafeBias  = orbitDir * OrbitStrafeBias * 0.3f;  // НАМНОГО меньше!
        styleForwardBias = 0.20f;  // больше вперёд
        styleUpBias      = 0.03f;
        break;
    case EDogfightStyle::BoomAndZoom:
        styleForwardBias = 0.25f;  // больше форсаж
        styleStrafeBias  = orbitDir * OrbitStrafeBias * 0.15f;  // меньше!
        styleUpBias      = (SelfPos.Z < TgtPos.Z + BoomZoomHeightCm) ? 0.8f : -0.4f;
        break;
    default:
        break;
    }

    // Близко к цели — почти не допускаем стилистических отклонений, чтобы не уходить в орбиту
    // Расширили диапазон подавления
    if (DistM < IdealDistM * 1.6f)
    {
        styleStrafeBias  *= 0.1f;  // ещё сильнее подавляем
        styleUpBias      *= 0.1f;
        styleForwardBias *= 0.2f;
    }

    // =========================
    // 1) Режимы разлёта / выхода
    // =========================

    if (Mode == EAttackMode::HeadOnAvoid)
    {
        const float dodgeSign =
            (orbitDir != 0.f)
                ? orbitDir
                : (((Ow->GetUniqueID() ^ 0x55aa) & 1) ? 1.f : -1.f);

        FVector breakDir =
              FwdW
            + dodgeSign * RightW * 0.9f
            + UpW * 0.6f;

        if (!breakDir.Normalize())
        {
            breakDir = FwdW;
        }

        AimDir     = breakDir;
        AxisF      = 1.0f;
        AxisStrafe = dodgeSign * MaxStrafe;
        AxisUp     = FMath::Clamp(CloseRangeVerticalBias, -MaxUp, MaxUp);
        bInBreakAway = true;
    }
    else if (Mode == EAttackMode::TooCloseBreak && TailLockStickyTimer <= 0.f)
    {
        const float dodgeSign =
            (orbitDir != 0.f)
                ? orbitDir
                : (((Ow->GetUniqueID() ^ 0x33cc) & 1) ? 1.f : -1.f);

        FVector extendDir =
              FwdW * 0.7f
            - DirToTargetW * 0.3f
            + dodgeSign * RightW * 0.7f
            + UpW * 0.4f;

        if (!extendDir.Normalize())
        {
            extendDir = FwdW;
        }

        AimDir     = extendDir;
        AxisF      = 1.0f;
        AxisStrafe = dodgeSign * MaxStrafe;
        AxisUp     = MaxUp * 0.7f;
        bInBreakAway = true;
    }
    else if (Mode == EAttackMode::OrbitBreak && TailLockStickyTimer <= 0.f)
    {
        // Используем UniqueID для генерации "стабильной" случайности для этого бота
        const uint32 seed = Ow->GetUniqueID();
        const float dodgeSign = ((seed ^ 0x7f7f) & 1) ? 1.f : -1.f;
        const float verticalSign = ((seed ^ 0xA5A5) & 1) ? 1.f : -1.f;
        const float mixFactor = float((seed >> 8) % 100) / 100.f;  // 0.0-1.0

        // Вариативные веса для разнообразия
        const float forwardWeight  = 0.5f + mixFactor * 0.3f;        // 0.5-0.8
        const float lateralWeight  = 0.3f + mixFactor * 0.2f;        // 0.3-0.5
        const float verticalWeight = 0.7f + mixFactor * 0.3f;        // 0.7-1.0

        // Агрессивный 3D маневр
        FVector extendDir =
              FwdW * forwardWeight                            // Умеренно вперед
            - DirToTargetW * 0.2f                              // Немного от цели
            + dodgeSign * RightW * lateralWeight               // Бок
            + verticalSign * UpW * verticalWeight;             // Сильно вверх/вниз

        if (!extendDir.Normalize())
        {
            extendDir = FwdW;
        }

        AimDir     = extendDir;
        AxisF      = 0.9f + mixFactor * 0.1f;                  // 0.9-1.0 форсаж
        AxisStrafe = dodgeSign * MaxStrafe * (0.4f + mixFactor * 0.3f);
        AxisUp     = verticalSign * MaxUp * (0.8f + mixFactor * 0.2f);
        bInBreakAway = true;
    }

    // =========================
    // NEW COMBAT MODES - РАЗНООБРАЗИЕ БОЯ
    // =========================

    else if (Mode == EAttackMode::StrafePass)
    {
        // STRAFE PASS: пролет мимо цели на высокой скорости с огнем
        // Летим прямо, минимум маневров, стреляем на проходе

        const uint32 seed = Ow->GetUniqueID();
        const float passOffset = ((seed & 1) ? 1.f : -1.f);  // лево/право

        // Целимся НЕ в центр цели, а мимо - проходим сбоку
        const FVector PassPoint = TgtPos + RightW * (passOffset * 400.f * 100.f); // 400м сбоку
        AimDir = (PassPoint - SelfPos).GetSafeNormal();

        // ФОРСАЖ - летим быстро
        AxisF = 1.0f;

        // Минимальный страф - только для корректировки
        const float sideError = FVector::DotProduct(PassPoint - SelfPos, RightW);
        AxisStrafe = FMath::Clamp(sideError * 0.0003f, -MaxStrafe * 0.3f, MaxStrafe * 0.3f);
        AxisUp = 0.f;
    }
    else if (Mode == EAttackMode::AggressivePursuit)
    {
        // AGGRESSIVE PURSUIT: прямая атака без идеального наведения
        // Летим на цель, стреляем широким конусом, не тратим время на точное наведение

        AimDir = DirToTargetW;

        // Почти форсаж
        AxisF = FMath::GetMappedRangeValueClamped(
            FVector2D(IdealDistM * 0.5f, IdealDistM * 2.5f),
            FVector2D(0.7f, 1.0f),
            DistM
        );

        // МИНИМАЛЬНЫЕ боковые маневры - только грубая корректировка
        const float sidePos = FVector::DotProduct(DirToTargetW, RightW);
        const float vertPos = FVector::DotProduct(DirToTargetW, UpW);

        AxisStrafe = FMath::Clamp(sidePos * 0.25f, -MaxStrafe * 0.4f, MaxStrafe * 0.4f);
        AxisUp     = FMath::Clamp(vertPos * 0.25f, -MaxUp * 0.4f,     MaxUp * 0.4f);
    }
    else if (Mode == EAttackMode::EvasiveApproach)
    {
        // EVASIVE APPROACH: змейка при сближении
        // Летим к цели, но делаем синусоидальные уклонения влево-вправо

        AimDir = DirToTargetW;

        // Синусоида для змейки - каждый бот со своей частотой
        const uint32 seed = Ow->GetUniqueID();
        const float snakeFreq = 0.8f + float(seed % 50) * 0.02f;  // 0.8-1.8 Hz
        const float snakeTime = GetWorld()->GetTimeSeconds() * snakeFreq;
        const float snakePattern = FMath::Sin(snakeTime * PI * 2.f);

        // Базовая скорость сближения
        const float Vf_self  = FVector::DotProduct(Vself_mps, FwdW);
        const float DistErrM = DistM - IdealDistM;
        float Vforward_des_mps = PosKp * 0.8f * DistErrM - PosKd * 0.8f * Vf_self;
        Vforward_des_mps = FMath::Clamp(Vforward_des_mps, 0.f, VxMax);

        AxisF = FMath::Max(Vforward_des_mps / VxMax, 0.6f);

        // Змейка - интенсивность зависит от дистанции
        const float snakeIntensity = FMath::GetMappedRangeValueClamped(
            FVector2D(IdealDistM * 0.5f, IdealDistM * 2.0f),
            FVector2D(0.3f, 0.8f),
            DistM
        );

        AxisStrafe = snakePattern * MaxStrafe * snakeIntensity;

        // Вертикаль - небольшая коррекция
        const float vertPos = FVector::DotProduct(DirToTargetW, UpW);
        AxisUp = FMath::Clamp(vertPos * 0.3f, -MaxUp * 0.4f, MaxUp * 0.4f);
    }

    // =========================
    // 2) Режим "сел на хвост" — стабильное сопровождение
    // =========================

    if (Mode == EAttackMode::TailChase)
    {
        styleStrafeBias  = 0.f;
        styleUpBias      = 0.f;
        styleForwardBias = 0.f;

        // Точка, куда целимся: чуть вперёд по скорости цели (lead-погоня)
        const float TgtSpeed_mps = TgtVel_mps.Size();
        FVector TgtDirVel        = (TgtSpeed_mps > KINDA_SMALL_NUMBER) ? (TgtVel_mps / TgtSpeed_mps) : TgtFwdW;

        // Чем дальше, тем чуть дальше вперёд берём точку (очень короткое упреждение, чтобы не качать носом)
        const float aheadM   = FMath::Clamp(DistM * 0.18f, 120.f, 280.f);
        const FVector AimPos = TgtPos + (TgtDirVel * aheadM * 100.f); // м → см

        AimDir = (AimPos - SelfPos).GetSafeNormal();

        // Продольная скорость: PD по дистанции, но только вперёд (не газуем задом)
        const float Vf_self  = FVector::DotProduct(Vself_mps, FwdW);
        const float DistErrM = DistM - IdealDistM;
        const float closeScale = FMath::GetMappedRangeValueClamped(
            FVector2D(0.f, IdealDistM * 1.0f),
            FVector2D(0.55f, 1.0f),
            DistM);
        const float KpTail   = PosKp * 0.7f * AttackPosKpMul * closeScale;
        const float KdTail   = PosKd * 0.8f * AttackPosKdMul * closeScale;

        float Vforward_des_mps = KpTail * DistErrM - KdTail * closingSpeedMps;
        Vforward_des_mps       = FMath::Clamp(Vforward_des_mps, 0.f, VxMax);

        AxisF = Vforward_des_mps / VxMax;
        AxisF = FMath::Max(AxisF, MinForwardAxis * 0.8f);

        // Бок / вертикаль: позиция + гашение относительной скорости
        const float sidePos   = FVector::DotProduct(DirToTargetW, RightW);
        const float vertPos   = FVector::DotProduct(DirToTargetW, UpW);
        const float sideVel   = FVector::DotProduct(Vrel_mps, RightW);
        const float vertVel   = FVector::DotProduct(Vrel_mps, UpW);

        const float posK = 0.5f;
        const float velK = 0.75f;

        AxisStrafe = posK * sidePos - velK * (sideVel / VrMax);
        AxisUp     = posK * vertPos - velK * (vertVel / VuMax);

        AxisStrafe = FMath::Clamp(AxisStrafe, -MaxStrafe * 0.4f, MaxStrafe * 0.4f);
        AxisUp     = FMath::Clamp(AxisUp,     -MaxUp     * 0.4f, MaxUp     * 0.4f);

        // Немного прижимаем вперёд, но без пересближения
        AxisF = FMath::Clamp(AxisF, TailLockForwardHold, 1.0f);
    }

    // =========================
    // 3) Нормальный подход (ещё не в хвосте)
    // =========================

    if (Mode == EAttackMode::Approach)
    {
        const float Vf_self  = FVector::DotProduct(Vself_mps, FwdW);
        const float DistErrM = DistM - IdealDistM;
        const float Kp       = PosKp * AttackPosKpMul;
        const float Kd       = PosKd * AttackPosKdMul;

        float Vforward_des_mps = Kp * DistErrM - Kd * Vf_self;
        Vforward_des_mps       = FMath::Clamp(Vforward_des_mps, 0.f, VxMax);

        AxisF = Vforward_des_mps / VxMax;
        if (DistM > FarDistM)
        {
            AxisF = 2.0f;  // форсаж на дальних дистанциях
        }
        else if (DistM > IdealDistM * 1.2f)
        {
            // На средних дистанциях тоже летим быстро
            AxisF = FMath::Max(AxisF, 0.75f);
        }
        else
        {
            // Даже близко к цели не летим медленно
            AxisF = FMath::Max(AxisF, MinForwardAxis * 1.3f);
        }

        const float sidePos = FVector::DotProduct(DirToTargetW, RightW);
        const float vertPos = FVector::DotProduct(DirToTargetW, UpW);

        // Сильно уменьшили боковые компоненты - меньше орбиты, больше прямых атак
        const float farScale = (DistM > IdealDistM * 1.4f) ? 0.5f : 0.3f;
        AxisStrafe = FMath::Clamp(sidePos * farScale, -MaxStrafe * 0.6f, MaxStrafe * 0.6f);
        AxisUp     = FMath::Clamp(vertPos * 0.4f,     -MaxUp * 0.6f,     MaxUp * 0.6f);

        const float orbitAlpha = FMath::GetMappedRangeValueClamped(
            FVector2D(CloseDistM, IdealDistM * 1.4f),
            FVector2D(1.f,        0.05f),  // ещё меньше!
            DistM
        );

        AxisStrafe += orbitDir * OrbitStrafeBias * orbitAlpha * 0.2f; // НАМНОГО меньше орбиты!
        AimDir      = DirToTargetW;
    }

    // =========================
    // 4) Применяем стиль + клампы
    // =========================

    AxisStrafe += styleStrafeBias;
    AxisUp     += styleUpBias;
    AxisF      += styleForwardBias;

    // --- Simple flock separation so bots do not pile up in one spot ---
    {
        const float SepRadius = 8000.f;           // 80 m
        const float SepRadiusSq = SepRadius * SepRadius;
        const float SepScale = 0.35f;

        UWorld* World = Ow ? Ow->GetWorld() : nullptr;
        if (World)
        {
            FVector Push = FVector::ZeroVector;
            for (TActorIterator<AShipPawn> It(World); It; ++It)
            {
                AShipPawn* Other = *It;
                if (!Other || Other == Ow) continue;

                // Only separate from same-team ships; collisions with enemies are fine.
                if (SelfTeam != INDEX_NONE && Other->GetTeamId() != SelfTeam)
                {
                    continue;
                }

                const FVector Diff = SelfPos - Other->GetActorLocation();
                const float DistSq = Diff.SizeSquared();
                if (DistSq < KINDA_SMALL_NUMBER || DistSq > SepRadiusSq)
                {
                    continue;
                }

                // Stronger push when very close.
                Push += Diff.GetSafeNormal() * (SepRadiusSq - DistSq) / SepRadiusSq;
            }

            const float PushRight = FVector::DotProduct(Push, RightW);
            const float PushUp    = FVector::DotProduct(Push, UpW);
            const float PushFwd   = FVector::DotProduct(Push, FwdW);

            AxisStrafe += FMath::Clamp(PushRight * SepScale, -MaxStrafe, MaxStrafe);
            AxisUp     += FMath::Clamp(PushUp    * SepScale, -MaxUp,     MaxUp);
            AxisF      += FMath::Clamp(PushFwd   * SepScale * 0.25f, -0.5f, 0.0f); // back off a bit if we are inside a blob
        }
    }

    AxisStrafe = FMath::Clamp(AxisStrafe, -MaxStrafe, MaxStrafe);
    AxisUp     = FMath::Clamp(AxisUp,     -MaxUp,     MaxUp);

    // Если сильно не смотрим туда, куда летим — режем страф и ап,
    // чтобы не уходить в очередной орбитальный танец
    const float alignDotSteer = FVector::DotProduct(FwdW, AimDir);
    if (alignDotSteer < 0.25f)
    {
        AxisStrafe *= 0.5f;
        AxisUp     *= 0.5f;
    }
    else if (Mode == EAttackMode::TailChase)
    {
        const float tailAlignDamp = FMath::GetMappedRangeValueClamped(
            FVector2D(0.5f, 0.95f),
            FVector2D(0.7f, 0.4f),
            alignDotSteer
        );
        AxisStrafe *= tailAlignDamp;
        AxisUp     *= tailAlignDamp;
    }

    if (bStaticGunship)
    {
        const float clampAxis = FMath::Clamp(StaticGunshipAxisClamp, 0.01f, 0.5f);
        AxisF      = FMath::Clamp(AxisF,      -clampAxis, clampAxis);
        AxisStrafe = FMath::Clamp(AxisStrafe, -clampAxis, clampAxis);
        AxisUp     = FMath::Clamp(AxisUp,     -clampAxis, clampAxis);
    }
    else
    {
        // Всегда летим ВПЕРЁД, как живой пилот на W
        AxisF = FMath::Clamp(AxisF, MinForwardAxis, 1.0f);
    }

    AxisF *= FMath::Clamp(AIForwardAxisScale, 0.1f, 1.0f);

    Flight->SetThrustForward(AxisF);
    Flight->SetStrafeRight(AxisStrafe);
    Flight->SetThrustUp(AxisUp);
    Flight->SetRollAxis(0.f);

    // =========================
    // 5) Ориентация: тянем нос к AimDir, но стабилизированно
    // =========================

    const float YawRateMax   = Flight->Yaw.YawRateMax_Deg;
    const float PitchRateMax = Flight->Pitch.PitchRateMax_Deg;

    float PitchRateCmdDeg = 0.f;
    float YawRateCmdDeg   = 0.f;
    float RollRateCmdDeg  = 0.f;

    // Плавно сглаживаем AimDir, чтобы нос не дёргался (особенно в хвостовом режиме)
    // но если угол большой, даём повернуть быстро без сглаживания
    const float rawAngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(AimDirSmooth, AimDir), -1.f, 1.f)));
    if (rawAngleDeg > 30.f)
    {
        AimDirSmooth = AimDir;
    }
    else
    {
        const float smoothRate = (Mode == EAttackMode::TailChase) ? 10.f : 16.f;
        AimDirSmooth = FMath::VInterpNormalRotationTo(AimDirSmooth, AimDir, Dt, smoothRate);
    }
    AimDir       = AimDirSmooth;

    const float alignDot = FMath::Clamp(FVector::DotProduct(FwdW, AimDir), -1.f, 1.f);
    const float angleToAimDeg = FMath::RadiansToDegrees(FMath::Acos(alignDot));

    FVector axisWorld = FVector::CrossProduct(FwdW, AimDir);
    const float axisLen = axisWorld.Size();

    if (axisLen < KINDA_SMALL_NUMBER || angleToAimDeg < 0.5f)
    {
        // Почти идеально смотрим — выключаем override, чтобы не рыскал
        Flight->SetAngularRateOverride(false, 0.f, 0.f, 0.f);
    }
    else
    {
        // === РАЗНАЯ АГРЕССИВНОСТЬ ПОВОРОТОВ ДЛЯ РАЗНЫХ РЕЖИМОВ ===

        const float BaseAlignFactor = bIsHeavyShip ? HeavyAlignRateFactor : AlignRateFactor;
        float UsedAlignFactor = BaseAlignFactor;

        // Режимы с минимальным кручением носом
        if (Mode == EAttackMode::TailChase)
        {
            UsedAlignFactor = BaseAlignFactor * 0.8f;  // Стабильное ведение
        }
        else if (Mode == EAttackMode::StrafePass)
        {
            UsedAlignFactor = BaseAlignFactor * 0.4f;  // МИНИМУМ кручения - летим прямо!
        }
        else if (Mode == EAttackMode::AggressivePursuit)
        {
            UsedAlignFactor = BaseAlignFactor * 0.55f;  // Грубое наведение
        }
        else if (Mode == EAttackMode::EvasiveApproach)
        {
            UsedAlignFactor = BaseAlignFactor * 0.65f;  // Умеренное наведение
        }
        else
        {
            UsedAlignFactor = BaseAlignFactor * 1.0f;   // Стандартное
        }

        const float AlignRateMaxDeg =
            UsedAlignFactor * FMath::Min(YawRateMax, PitchRateMax);

        const float AlignRateMaxRad = FMath::DegreesToRadians(AlignRateMaxDeg);

        FVector omegaWorld = axisWorld * AlignRateMaxRad;
        FVector omegaLocal = TM.InverseTransformVectorNoScale(omegaWorld);

        PitchRateCmdDeg = FMath::RadiansToDegrees(omegaLocal.Y);
        YawRateCmdDeg   = FMath::RadiansToDegrees(omegaLocal.Z);

        // Чем ближе к цели по углу, тем мягче доворачиваем
        const float AlignScale = FMath::GetMappedRangeValueClamped(
            FVector2D(0.f, 35.f),
            FVector2D(0.4f, 1.0f),
            angleToAimDeg
        );

        PitchRateCmdDeg *= AlignScale;
        YawRateCmdDeg   *= AlignScale;

        if (Mode == EAttackMode::TailChase)
        {
            PitchRateCmdDeg *= 0.75f; 
            YawRateCmdDeg   *= 0.75f;
        }

        PitchRateCmdDeg = FMath::Clamp(PitchRateCmdDeg, -PitchRateMax, PitchRateMax);
        YawRateCmdDeg   = FMath::Clamp(YawRateCmdDeg,   -YawRateMax,   YawRateMax);

        RollRateCmdDeg  = 0.f;

        Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
    }

    // =========================
    // 6) Стрельба: в хвостовом/подходе, когда реально в конусе
    // =========================

    const bool bAllowFire = !bInBreakAway;

    if (bAllowFire)
    {
        const float angleToTargetDeg = FMath::RadiansToDegrees(
            FMath::Acos(FMath::Clamp(FVector::DotProduct(FwdW, DirToTargetW), -1.f, 1.f))
        );

        // === РАСШИРЕННЫЕ УГЛЫ ОГНЯ ДЛЯ РАЗНЫХ РЕЖИМОВ ===
        // Больше огня = больше экшена!

        float UsedFireCone = AttackFireAngleDeg * 2.0f;  // Базовый конус x2

        // Разные конусы для разных режимов
        if (Mode == EAttackMode::TailChase)
        {
            UsedFireCone = FMath::Max(AttackFireAngleDeg * 2.0f, 25.f);  // 25° в хвосте
        }
        else if (Mode == EAttackMode::StrafePass)
        {
            UsedFireCone = 45.f;  // ШИРОКИЙ конус для стрейф-атаки
        }
        else if (Mode == EAttackMode::AggressivePursuit)
        {
            UsedFireCone = 38.f;  // Широкий конус для агрессивной атаки
        }
        else if (Mode == EAttackMode::EvasiveApproach)
        {
            UsedFireCone = 32.f;  // Средний конус при змейке
        }
        else
        {
            UsedFireCone = FMath::Max(AttackFireAngleDeg * 2.0f, 20.f);  // Минимум 20°
        }

        const bool bInFireDist  = (DistM <= AttackFireMaxDistM);
        const bool bInFireAngle = (angleToTargetDeg <= UsedFireCone);

        if (bInFireDist && bInFireAngle)
        {
            const float LaserSpeedUU = AttackLaserSpeedMps * 100.f;
            float       leadTime     = Dist / FMath::Max(LaserSpeedUU, 1.f);
            leadTime = FMath::Clamp(leadTime, 0.02f, 0.6f);

            const FVector AimPos = TgtPos + TgtVel_cmps * leadTime;

            bWantsToFireLaser     = true;
            LaserAimWorldLocation = AimPos;

            if (Ow->HasAuthority())
            {
                if (UShipLaserComponent* Laser = Ow->FindComponentByClass<UShipLaserComponent>())
                {
                    Laser->FireFromAI(AimPos);
                }
            }

            if (CVar_ShipBot_DebugDraw.GetValueOnGameThread() != 0)
            {
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
                DrawDebugLine(GetWorld(), SelfPos, AimPos, FColor::Red, false, 0.05f, 0, 2.f);
                DrawDebugSphere(GetWorld(), AimPos, 500.f, 8, FColor::Red, false, 0.05f, 0, 1.f);
#endif
            }
        }
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

	if (bAutoAcquireEnemies || bAutoAcquirePlayer)
	{
		TargetActor = FindBestEnemyShip();
	}

	return TargetActor.Get();
}

AActor* UShipAIPilotComponent::FindBestEnemyShip()
{
    UWorld* World = GetWorld();
    AActor* Ow    = GetOwner();
    if (!World || !Ow) return nullptr;

    const FVector     SelfLoc  = Ow->GetActorLocation();
    const AShipPawn*  SelfShip = Cast<AShipPawn>(Ow);
    const int32       SelfTeam = SelfShip ? SelfShip->GetTeamId() : INDEX_NONE;

    // Cache ship list per-world to avoid O(N^2) scans each tick across hundreds of bots.
    struct FShipCache
    {
        TArray<TWeakObjectPtr<AShipPawn>> Ships;
        double LastRefresh = -1.0;
    };
    static TMap<TWeakObjectPtr<UWorld>, FShipCache> CacheByWorld;

    const double Now = World->GetTimeSeconds();
    const float RefreshPeriod = FMath::Max(0.05f, CVar_ShipBot_TargetRefresh.GetValueOnGameThread());
    FShipCache& Cache = CacheByWorld.FindOrAdd(World);

    const bool bStale = (Cache.LastRefresh < 0.0) || (Now - Cache.LastRefresh > RefreshPeriod);
    if (bStale)
    {
        Cache.Ships.Reset();
        for (TActorIterator<AShipPawn> It(World); It; ++It)
        {
            AShipPawn* Ship = *It;
            if (Ship)
            {
                Cache.Ships.Add(Ship);
            }
        }
        Cache.LastRefresh = Now;
    }

    AActor* Best      = nullptr;
    float   BestScore = TNumericLimits<float>::Max();

    for (const TWeakObjectPtr<AShipPawn>& ShipPtr : Cache.Ships)
    {
        AShipPawn* OtherShip = ShipPtr.Get();
        if (!OtherShip || OtherShip == Ow) continue;

        if (SelfTeam != INDEX_NONE && OtherShip->GetTeamId() == SelfTeam)
        {
            continue;
        }

        const float DistSq = FVector::DistSquared(SelfLoc, OtherShip->GetActorLocation());

        // Slight random bias per ship to reduce lock-step targeting.
        const float Jitter = 1.f + (float(OtherShip->GetUniqueID() & 0xFF) / 1024.f);
        const float Score  = DistSq * Jitter;

        if (Score < BestScore)
        {
            BestScore = Score;
            Best      = OtherShip;
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

	bAimDirInit  = false;
	AimDirSmooth = FVector::ForwardVector;
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

void UShipAIPilotComponent::TickDogfightStyle(float Dt)
{
	if (DogfightStyleTimeLeft <= 0.f)
	{
		SelectNewDogfightStyle();
	}
	else
	{
		DogfightStyleTimeLeft -= Dt;
	}
}

void UShipAIPilotComponent::SelectNewDogfightStyle()
{
	const float minHold = FMath::Max(0.2f, StrategyMinHoldTime);
	const float maxHold = FMath::Max(minHold, StrategyMaxHoldTime);
	const float roll    = FMath::FRand();

	if (roll < 0.45f)
	{
		CurrentDogfightStyle = EDogfightStyle::Pursuit;
	}
	else if (roll < 0.65f)
	{
		CurrentDogfightStyle = EDogfightStyle::FlankLeft;
	}
	else if (roll < 0.85f)
	{
		CurrentDogfightStyle = EDogfightStyle::FlankRight;
	}
	else
	{
		CurrentDogfightStyle = EDogfightStyle::BoomAndZoom;
	}

	if (CurrentDogfightStyle == EDogfightStyle::FlankLeft)
	{
		CurrentOrbitSign = -1.f;
	}
	else if (CurrentDogfightStyle == EDogfightStyle::FlankRight)
	{
		CurrentOrbitSign = 1.f;
	}
	else
	{
		CurrentOrbitSign = (FMath::RandRange(0, 1) == 0) ? -1.f : 1.f;
	}

	DogfightStyleTimeLeft = FMath::FRandRange(minHold, maxHold);
}
