#include "ShipAIPilotComponent.h"
#include "FlightComponent.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogShipBot, Log, All);

// Рулим ботом только на сервере (или в Standalone)
static FORCEINLINE bool ShouldDriveOnThisMachine(const AActor* Ow)
{
	return Ow && (Ow->HasAuthority() || Ow->GetNetMode() == NM_Standalone);
}
// Высота над "землёй" (WorldStatic). Если ничего не поймали — считаем, что мы в космосе.
static float SampleAltitudeMeters(UWorld* World, const FVector& Origin)
{
	if (!World) return TNumericLimits<float>::Max();

	FHitResult Hit;
	const FVector Start = Origin;
	const FVector End   = Origin - FVector::UpVector * 100000.f; // трассим 1000 м вниз

	FCollisionQueryParams Q(TEXT("ShipBot_Altitude"), /*bTraceComplex*/ false);
	Q.bReturnPhysicalMaterial = false;
	Q.AddIgnoredActor(nullptr); // на всякий, но по факту игнорим только владельца дальше, если нужно

	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Q))
	{
		return Hit.Distance / 100.f; // см → м
	}
	return TNumericLimits<float>::Max(); // нет земли
}


namespace
{
	// На цели — где взять точку прицеливания
	static const TArray<FName> kAimSocketNamesOnTarget = {
		TEXT("CenterPilot"), TEXT("Aim"), TEXT("CockpitAim"), TEXT("Camera"),
		TEXT("View"), TEXT("Head"), TEXT("Muzzle"), TEXT("ScreenCenter")
	};

	// На нас — откуда целимся
	static const TArray<FName> kSelfAimSocketNames = {
		TEXT("CenterPilot"), TEXT("TopPilot"), TEXT("BottomPilot"),
		TEXT("LeftPilot"), TEXT("RightPilot"),
		TEXT("Nose"), TEXT("Aim"), TEXT("Forward"), TEXT("CockpitAim"),
		TEXT("Camera"), TEXT("View"), TEXT("Muzzle"), TEXT("ScreenCenter")
	};

	FORCEINLINE float ClampUnit(float v){ return FMath::Clamp(v, -1.f, 1.f); }
	FORCEINLINE float Deadzone(float v, float dz){ return (FMath::Abs(v) < dz) ? 0.f : v; }

	static FVector GetTargetAimPoint(AActor* Target, /*out*/FName& OutUsed)
	{
		OutUsed = NAME_None;
		if (!Target) return FVector::ZeroVector;

		FVector EyesLoc; FRotator EyesRot;
		Target->GetActorEyesViewPoint(EyesLoc, EyesRot);
		if (!EyesLoc.IsNearlyZero()) { OutUsed = TEXT("Eyes"); return EyesLoc; }

		TArray<USceneComponent*> Cs; Target->GetComponents<USceneComponent>(Cs);
		for (const FName Sock : kAimSocketNamesOnTarget)
			for (USceneComponent* C : Cs)
				if (C && C->DoesSocketExist(Sock))
				{ OutUsed = Sock; return C->GetSocketLocation(Sock); }

		return Target->GetActorLocation();
	}

	static FTransform GetSelfAimFrame(AActor* Self, /*out*/FName& OutUsed)
	{
		OutUsed = NAME_None;
		if (!Self) return FTransform::Identity;

		TArray<USceneComponent*> Cs; Self->GetComponents<USceneComponent>(Cs);

		// Сначала сокеты
		for (const FName Name : kSelfAimSocketNames)
			for (USceneComponent* C : Cs)
				if (C && C->DoesSocketExist(Name))
				{ OutUsed = Name; return FTransform(C->GetSocketRotation(Name), C->GetSocketLocation(Name), FVector::OneVector); }

		// Потом компоненты по имени
		for (const FName Name : kSelfAimSocketNames)
			for (USceneComponent* C : Cs)
				if (C && C->GetFName() == Name)
				{ OutUsed = Name; return C->GetComponentTransform(); }

		// Фоллбэк — пивот
		return Self->GetActorTransform();
	}

	// Достаём реально симулирующий примитив (для скоростей)
	static UPrimitiveComponent* GetSimPrim(const AActor* Ow)
	{
		if (!Ow) return nullptr;
		if (auto* P = Cast<UPrimitiveComponent>(Ow->GetRootComponent()))
			if (P->IsSimulatingPhysics()) return P;

		TArray<UPrimitiveComponent*> Ps; Ow->GetComponents<UPrimitiveComponent>(Ps);
		for (auto* C : Ps) if (C && C->IsSimulatingPhysics()) return C;
		return nullptr;
	}

	// Угловые скорости в рамке носа (deg/s)
	static FVector GetLocalAngularRatesDeg_Frame(const AActor* Ow, const FTransform& FrameTM)
	{
		const UPrimitiveComponent* Prim = GetSimPrim(Ow);
		if (!Prim) return FVector::ZeroVector;

		const FVector W = Prim->GetPhysicsAngularVelocityInRadians(); // рад/с в мире
		const FVector L = FrameTM.InverseTransformVectorNoScale(W);   // в локале рамки
		return FVector(FMath::RadiansToDegrees(L.X), FMath::RadiansToDegrees(L.Y), FMath::RadiansToDegrees(L.Z));
	}

	// Относительная линейная скорость цели в рамке носа (м/с)
	static FVector GetRelativeSpeedLocalMS_Frame(const AActor* Ow, const AActor* Tgt, const FTransform& FrameTM)
	{
		const UPrimitiveComponent* a = GetSimPrim(Ow);
		const UPrimitiveComponent* b = GetSimPrim(Tgt);
		const FVector va = a ? a->GetComponentVelocity() : FVector::ZeroVector;
		const FVector vb = b ? b->GetComponentVelocity() : FVector::ZeroVector;
		return FrameTM.InverseTransformVectorNoScale(vb - va) / 100.f;
	}

	// === Привязать оси FlightComponent к направлению «носа» ===
	static FVector AxisSelToLocalVector(EAxisSelector S)
	{
		switch (S)
		{
			case EAxisSelector::PlusX:  return FVector( 1, 0, 0);
			case EAxisSelector::MinusX: return FVector(-1, 0, 0);
			case EAxisSelector::PlusY:  return FVector( 0, 1, 0);
			case EAxisSelector::MinusY: return FVector( 0,-1, 0);
			case EAxisSelector::PlusZ:  return FVector( 0, 0, 1);
			default:                    return FVector( 0, 0,-1);
		}
	}

	static void SyncFlightAxesToNose(UFlightComponent* Flight, const FTransform& NoseTM, const UPrimitiveComponent* BodyPrim)
	{
		if (!Flight || !BodyPrim) return;

		const FTransform BodyTM = BodyPrim->GetComponentTransform();

		struct FCand { EAxisSelector Sel; FVector DirW; int32 AxisIndex; };
		auto Push = [&](EAxisSelector S, const FVector& Local, int32 AxisIdx, TArray<FCand>& Out)
		{ Out.Add({ S, BodyTM.TransformVectorNoScale(Local).GetSafeNormal(), AxisIdx }); };

		TArray<FCand> C;
		Push(EAxisSelector::PlusX,  FVector( 1, 0, 0), 0, C);
		Push(EAxisSelector::MinusX, FVector(-1, 0, 0), 0, C);
		Push(EAxisSelector::PlusY,  FVector( 0, 1, 0), 1, C);
		Push(EAxisSelector::MinusY, FVector( 0,-1, 0), 1, C);
		Push(EAxisSelector::PlusZ,  FVector( 0, 0, 1), 2, C);
		Push(EAxisSelector::MinusZ, FVector( 0, 0,-1), 2, C);

		const FVector NoseF = NoseTM.GetUnitAxis(EAxis::X);
		const FVector NoseU = NoseTM.GetUnitAxis(EAxis::Z);

		int32 BestF = INDEX_NONE; float BestFD = -1.f;
		for (int32 i=0;i<C.Num();++i){ float d=FMath::Abs(FVector::DotProduct(C[i].DirW, NoseF)); if (d>BestFD){BestFD=d; BestF=i;} }

		int32 BestU = INDEX_NONE; float BestUD = -1.f;
		for (int32 i=0;i<C.Num();++i)
		{
			if (i==BestF) continue;
			if (C[i].AxisIndex == C[BestF].AxisIndex) continue;
			float d=FMath::Abs(FVector::DotProduct(C[i].DirW, NoseU)); if (d>BestUD){BestUD=d; BestU=i;}
		}

                if (BestF != INDEX_NONE && BestU != INDEX_NONE)
                {
                        const EAxisSelector NewForward = C[BestF].Sel;
                        const EAxisSelector NewUp      = C[BestU].Sel;
                        if (Flight->Frame.Forward != NewForward || Flight->Frame.Up != NewUp)
                        {
                                Flight->Frame.Forward = NewForward; // «вперёд» = +X корпуса, ближайший к NoseF
                                Flight->Frame.Up      = NewUp;      // «вверх»  = +Z корпуса, ближайший к NoseU
                                UE_LOG(LogShipBot, Display, TEXT("[Bot] Rebound Flight frame to Nose  F=%d U=%d"),
                                        (int32)NewForward, (int32)NewUp);
                        }
                }
        }

        // Ремап thrust из рамки носа -> базис, который читает Flight
	static void RemapThrust_FromNoseToNetSimBasis(
		const AActor* Ow, const UFlightComponent* Flight, const UPrimitiveComponent* BodyPrim,
		const FVector& NoseF, const FVector& NoseR, const FVector& NoseU,
		float inF, float inR, float inU, float& outF, float& outR, float& outU)
	{
		outF = inF; outR = inR; outU = inU; // safe default
		if (!Ow || !Flight || !BodyPrim) return;

		const FTransform BodyTM = BodyPrim->GetComponentTransform();

		auto AxisToWorld = [&](EAxisSelector Sel)->FVector
		{
			return BodyTM.TransformVectorNoScale(AxisSelToLocalVector(Sel)).GetSafeNormal();
		};

		const FVector Ax = AxisToWorld(Flight->Frame.Forward);
		const FVector Az = AxisToWorld(Flight->Frame.Up);
		const FVector Ay = FVector::CrossProduct(Az, Ax).GetSafeNormal();

		// Проекция носовых команд на базис Flight
		outF = FMath::Clamp(inF * FVector::DotProduct(NoseF, Ax) + inR * FVector::DotProduct(NoseR, Ax) + inU * FVector::DotProduct(NoseU, Ax), -1.f, 1.f);
		outR = FMath::Clamp(inF * FVector::DotProduct(NoseF, Ay) + inR * FVector::DotProduct(NoseR, Ay) + inU * FVector::DotProduct(NoseU, Ay), -1.f, 1.f);
		outU = FMath::Clamp(inF * FVector::DotProduct(NoseF, Az) + inR * FVector::DotProduct(NoseR, Az) + inU * FVector::DotProduct(NoseU, Az), -1.f, 1.f);
	}
} // anon

// ====== UShipAIPilotComponent ======

UShipAIPilotComponent::UShipAIPilotComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup    = TG_PrePhysics;
	bAutoActivate = true;
}

void UShipAIPilotComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Ow = GetOwner();
	if (!Ow) return;

	Flight = Ow->FindComponentByClass<UFlightComponent>();

	if (Ow->HasAuthority() && Flight.IsValid())
	{
		Flight->SetFlightAssistEnabled(true);
		Flight->ResetInputFilters();
	}
}

void UShipAIPilotComponent::TickComponent(float Dt, enum ELevelTick, FActorComponentTickFunction*)
{
	if (!GetOwner() || !Flight.IsValid()) return;
	if (!ShouldDriveOnThisMachine(GetOwner())) return;
	// Пилота дёргает Brain через ApplyDirective()
}

AActor* UShipAIPilotComponent::ResolveTarget(){ return nullptr; }

void UShipAIPilotComponent::ResetTransientState()
{
	SmoothedMouseYaw = SmoothedMousePitch = 0.f;
	SmoothedThrustForward = SmoothedThrustRight = SmoothedThrustUp = 0.f;
}

float UShipAIPilotComponent::SignedAngleAroundAxisRad(const FVector& vFrom, const FVector& vTo, const FVector& Axis)
{
	const FVector c = FVector::CrossProduct(vFrom, vTo);
	const float   s = FVector::DotProduct(Axis, c);
	const float   d = FVector::DotProduct(vFrom, vTo);
	return FMath::Atan2(s, d);
}

void UShipAIPilotComponent::ApplyDirective(float Dt, const FShipDirective& D)
{
    AActor* Ow = GetOwner();
    if (!Ow || !Flight.IsValid())
        return;

    // --- ЦЕЛЬ ДЛЯ АИМА ---
    AActor* Target = D.AimActor.Get();
    FName   UsedTgtSock = D.AimSocket;

    FVector AimPoint = FVector::ZeroVector;
    if (D.AimPointW.IsSet())
    {
        AimPoint = D.AimPointW.GetValue();
    }
    else if (Target)
    {
        AimPoint = GetTargetAimPoint(Target, UsedTgtSock);
    }

    const bool bHasAim = !AimPoint.IsNearlyZero();

    // Если вообще нечего целить и якоря тоже нет — отпускаем всё и выходим
    if (!bHasAim && !D.FollowAnchorW.IsSet() && !Target)
    {
        Flight->SetStrafeRight(0.f);
        Flight->SetThrustUp(0.f);
        Flight->SetThrustForward(0.f);
        Flight->SetRollAxis(0.f);
        ResetTransientState();
        AimHoldTimerS = 0.f;
        bAimHoldActive = false;
        PrevRollAxis = 0.f;
        return;
    }

    // === Рамка «носа» ===
    FName SelfAimSock;
    const FTransform AimFrame = GetSelfAimFrame(Ow, SelfAimSock);
    const FVector Loc = AimFrame.GetLocation();

    auto OwnerAxis = [&](TEnumAsByte<EAxis::Type> Axis)->FVector
    {
        return Ow->GetActorTransform().GetUnitAxis((EAxis::Type)Axis).GetSafeNormal();
    };

    FVector Fwd = FVector::ForwardVector;
    FVector Up  = FVector::UpVector;

    if (bForceNoseAxes)
    {
        Fwd = OwnerAxis(NoseForwardAxis);
        if (bInvertNoseForward)
        {
            Fwd *= -1.f;
        }

        Up = OwnerAxis(NoseUpAxis);
    }
    else
    {
        const FVector Xs = AimFrame.GetUnitAxis(EAxis::X).GetSafeNormal();
        const FVector Ys = AimFrame.GetUnitAxis(EAxis::Y).GetSafeNormal();
        const FVector Zs = AimFrame.GetUnitAxis(EAxis::Z).GetSafeNormal();

        const FVector AFor = Ow->GetActorForwardVector().GetSafeNormal();
        const FVector AUp  = Ow->GetActorUpVector().GetSafeNormal();

        FVector FwdCand[3]  = { Xs, Ys, Zs };
        float   FwdScore[3] = {
            FMath::Abs(FVector::DotProduct(Xs, AFor)),
            FMath::Abs(FVector::DotProduct(Ys, AFor)),
            FMath::Abs(FVector::DotProduct(Zs, AFor))
        };

        int32 iBestF = (FwdScore[1] > FwdScore[0] ? 1 : 0);
        iBestF       = (FwdScore[2] > FwdScore[iBestF] ? 2 : iBestF);

        Fwd = FwdCand[iBestF];
        if (FVector::DotProduct(Fwd, AFor) < 0.f)
        {
            Fwd *= -1.f;
        }

        FVector UpCand[2];
        int32   idx = 0;
        for (int32 i = 0; i < 3; ++i)
        {
            if (i != iBestF)
            {
                UpCand[idx++] = FwdCand[i];
            }
        }

        Up = (FVector::DotProduct(UpCand[0], AUp) > FVector::DotProduct(UpCand[1], AUp))
            ? UpCand[0] : UpCand[1];

        if (FVector::DotProduct(Up, AUp) < 0.f)
        {
            Up *= -1.f;
        }
    }

    if (Fwd.IsNearlyZero())
    {
        Fwd = Ow->GetActorForwardVector().GetSafeNormal();
    }
    if (Up.IsNearlyZero())
    {
        Up = Ow->GetActorUpVector().GetSafeNormal();
    }

    FVector Right = FVector::CrossProduct(Up, Fwd);
    if (Right.IsNearlyZero())
    {
        Right = FVector::CrossProduct(Fwd, FVector::UpVector);
        if (Right.IsNearlyZero())
        {
            Right = Ow->GetActorRightVector().GetSafeNormal();
        }
    }
    Right = Right.GetSafeNormal();
    Up = FVector::CrossProduct(Fwd, Right).GetSafeNormal();

    const FRotator NoseRot = FRotationMatrix::MakeFromXZ(Fwd, Up).Rotator();
    const FTransform Nose(NoseRot, Loc);

    UPrimitiveComponent* SelfPrim = GetSimPrim(Ow);
    SyncFlightAxesToNose(Flight.Get(), Nose, SelfPrim);

    // --- Цель, якорь follow и скорости ---
    const UPrimitiveComponent* TgtPrim = Target ? GetSimPrim(Target) : nullptr;
    const FVector vTgtW   = TgtPrim ? TgtPrim->GetComponentVelocity() : FVector::ZeroVector;
    const FVector TgtFwd  = Target ? Target->GetActorForwardVector() : FVector::ForwardVector;

    FVector ToAim      = FVector::ZeroVector;
    FVector AimDir     = Fwd;
    float   DistToAimM = 0.f;

    if (bHasAim)
    {
        ToAim      = (AimPoint - Loc);
        AimDir     = ToAim.GetSafeNormal();
        DistToAimM = ToAim.Size() / 100.f;
    }

    const float BackMeters = D.FollowBehindMetersOverride.IsSet() ? D.FollowBehindMetersOverride.GetValue() : FollowBehindMeters;
    const float LeadSec    = D.FollowLeadSecondsOverride.IsSet()  ? D.FollowLeadSecondsOverride.GetValue()  : FollowLeadSeconds;
    const float MinAppro   = D.MinApproachMetersOverride.IsSet()  ? D.MinApproachMetersOverride.GetValue()  : MinApproachMeters;

    FVector FollowAnchor = Loc;
    if (D.FollowAnchorW.IsSet())
    {
        FollowAnchor = D.FollowAnchorW.GetValue();
    }
    else if (Target)
    {
        const FVector TgtFuture = Target->GetActorLocation() + vTgtW * LeadSec;
        const float   Back      = FMath::Max(BackMeters, MinAppro);
        FollowAnchor = TgtFuture - TgtFwd * Back;
    }

    const FVector ToAnchor = (FollowAnchor - Loc);
    const float   DistM    = ToAnchor.Size() / 100.f;

    const FVector vSelfW      = SelfPrim ? SelfPrim->GetComponentVelocity() : FVector::ZeroVector;
    const float   SelfSpeedMS = vSelfW.Size() / 100.f;
    const float   TgtSpeedMS  = vTgtW.Size()  / 100.f;

    const FVector vRelLocal = (Target ? GetRelativeSpeedLocalMS_Frame(Ow, Target, Nose) : FVector::ZeroVector);
    const float   RelFwdMS   = vRelLocal.X;
    const float   RelRightMS = vRelLocal.Y;
    const float   RelUpMS    = vRelLocal.Z;

    // --- Ошибки наведения (углы) ---
    const float c         = FVector::DotProduct(Fwd, AimDir);
    const float s         = FVector::CrossProduct(Fwd, AimDir).Length();
    const float align01   = FMath::Clamp(0.5f * (c + 1.f), 0.f, 1.f);
    const float aimErrDeg = FMath::RadiansToDegrees(FMath::Atan2(s, c));

    const float yawErrDeg   = FMath::RadiansToDegrees(SignedAngleAroundAxisRad(Fwd, AimDir, Up));
    const float pitchErrDeg = FMath::RadiansToDegrees(SignedAngleAroundAxisRad(Fwd, AimDir, Right));

    // === Угловые скорости в рамке носа ===
    const FVector ratesDeg         = GetLocalAngularRatesDeg_Frame(Ow, Nose);
    const float   rollRateDegPerS  = ratesDeg.X;
    const float   yawRateDegPerS   = ratesDeg.Z;
    const float   pitchRateDegPerS = ratesDeg.Y;

    // --- PD по углу: угол + демпфирование по угл. скорости ---
    float yawCmdDeg   = Deadzone(yawErrDeg,   YawDeadzoneDeg);
    float pitchCmdDeg = Deadzone(pitchErrDeg, PitchDeadzoneDeg);

    // Kp – из твоих коэффициентов, Kd – просто аккуратный демпфер
    const float KpYaw   = KpYaw_MouseDeltaPerDeg;
    const float KpPitch = KpPitch_MouseDeltaPerDeg;
    const float KdYaw   = 0.015f;   // подгон: чем больше, тем сильнее демпфирование
    const float KdPitch = 0.015f;

    float dYaw   = (KpYaw   * yawCmdDeg)   - (KdYaw   * yawRateDegPerS);
    float dPitch = (KpPitch * pitchCmdDeg) - (KdPitch * pitchRateDegPerS);

    const float facingDot = FVector::DotProduct(Fwd, AimDir);
    float yawPitchBoost = 1.f;
    if (bBoostTurnWhenTargetBehind && facingDot < -0.2f)
    {
        yawPitchBoost = FMath::GetMappedRangeValueClamped(FVector2D(-1.f, -0.2f), FVector2D(2.2f, 1.0f), facingDot);
    }
    dYaw   *= yawPitchBoost;
    dPitch *= yawPitchBoost;

    // ограничиваем "виртуальную мышь" за кадр
    dYaw   = FMath::Clamp(dYaw,   -MaxMouseDeltaPerFrame, +MaxMouseDeltaPerFrame);
    dPitch = FMath::Clamp(dPitch, -MaxMouseDeltaPerFrame, +MaxMouseDeltaPerFrame);

    FVector UpReference = Up;
    float rollAxisImmediate = 0.f;
    {
        const float AltitudeM = SampleAltitudeMeters(Ow->GetWorld(), Loc);
        const bool  bHaveGround = AltitudeM < TNumericLimits<float>::Max();

        FVector UpRef = FVector::UpVector;
        if (!bHaveGround && Target)
        {
            const FVector TgtUp = Target->GetActorUpVector().GetSafeNormal();
            if (!TgtUp.IsNearlyZero())
            {
                UpRef = TgtUp;
            }
        }

        UpReference = UpRef;

        FVector UpRefPlanar = (UpRef - Fwd * FVector::DotProduct(UpRef, Fwd)).GetSafeNormal();
        if (UpRefPlanar.IsNearlyZero())
        {
            UpRefPlanar = Up;
        }

        float rollErrDeg = FMath::RadiansToDegrees(SignedAngleAroundAxisRad(Up, UpRefPlanar, Fwd));
        rollErrDeg = Deadzone(rollErrDeg, RollDeadzoneDeg);

        float desiredRollRate = (KpRoll_RatePerDeg * rollErrDeg) - (KdRoll_RatePerDegPerSec * rollRateDegPerS);
        desiredRollRate = FMath::Clamp(desiredRollRate, -RollRateMaxDegPerSec, RollRateMaxDegPerSec);

        const float FlightRollRateMax = 220.f;
        if (FlightRollRateMax > KINDA_SMALL_NUMBER)
        {
            rollAxisImmediate = FMath::Clamp(desiredRollRate / FlightRollRateMax, -1.f, 1.f);
        }
    }

    // --- Линейная PD-центровка к follow-якорю ---
    const float offRightM = FVector::DotProduct(ToAnchor, Right) / 100.f;
    const float offUpM    = FVector::DotProduct(ToAnchor, Up)    / 100.f;
    const float offFwdM   = FVector::DotProduct(ToAnchor, Fwd)   / 100.f;

    const float KdLat_InputPerMS = 0.18f;

    float uRight = FMath::Clamp(
        (FMath::IsNearlyZero(offRightM, LateralDeadzoneM) ? 0.f : KpRight_InputPerM * offRightM)
        + (KdLat_InputPerMS * RelRightMS),
        -1.f, 1.f);

    float uUp = FMath::Clamp(
        (FMath::IsNearlyZero(offUpM, VerticalDeadzoneM) ? 0.f : KpUp_InputPerM * offUpM)
        + (KdLat_InputPerMS * RelUpMS),
        -1.f, 1.f);

    const float KdFwd_InputPerMS = 0.22f;
    const float fwdErrM = (FMath::Abs(offFwdM) < ForwardDeadzoneM) ? 0.f : offFwdM;

    // базовый PD вперёд/назад (ВАЖНО: БОЛЬШЕ НЕ ЗАБИВАЕМ ОТРИЦАТЕЛЬНЫЕ ЗНАЧЕНИЯ)
    float uFwd = (KpForward_InputPerM * fwdErrM) + (KdFwd_InputPerMS * RelFwdMS);
    uFwd = FMath::Clamp(uFwd, -1.f, 1.f);

    const float facingToAnchor = FVector::DotProduct(ToAnchor, Fwd);

    // если сильно не смотрим на цель – урезаем резкие движения, чтобы не крутило
    if (aimErrDeg > 120.f)
    {
        uFwd  = FMath::Clamp(uFwd, -0.3f, 0.3f);
        uRight *= 0.4f;
        uUp    *= 0.4f;
    }

    // если якорь позади и мы ещё не развернулись на него – не разгоняемся вперёд
    if (facingToAnchor < 0.f && align01 < 0.6f)
    {
        uFwd = FMath::Min(uFwd, 0.f); // максимум – тормозим / нейтраль, но не ускоряемся вперёд
    }

    // мягкое "догонять если далеко": доп. подталкивание только когда мы далеко
    if (DistM > BackMeters * 1.3f)
    {
        uFwd = FMath::Clamp(uFwd + 0.25f, -1.f, 1.f);
    }

    // --- Анти-таран: ближний пузырь вокруг цели ---
    if (Target && DistToAimM > 1.f) // есть цель и не сидим прямо в центре
    {
        const float NoRamRadiusM = FMath::Max(10.f, MinAppro * 0.7f);
        if (DistToAimM < NoRamRadiusM)
        {
            // мы заехали в "пузырь" вокруг цели -> гарантированно даём заднюю тягу,
            // пропорциональную тому, насколько глубоко залезли
            const float Penetration = (NoRamRadiusM - DistToAimM) / FMath::Max(NoRamRadiusM, 1.f); // 0..1
            const float Brake = -FMath::Lerp(0.15f, 0.8f, Penetration);
            uFwd = FMath::Min(uFwd, Brake);
        }
    }

    // ограничиваем задний ход, чтобы бот не улетал километры задом
    const float MaxReverse = 0.6f;
    uFwd = FMath::Clamp(uFwd, -MaxReverse, 1.f);

    // --- Сглаживание «мыши» и тяги ---
    {
        const float tauMouse = 0.04f;
        const float aMouse   = 1.f - FMath::Exp(-Dt / FMath::Max(0.001f, tauMouse));
        SmoothedMouseYaw     = FMath::Lerp(SmoothedMouseYaw,   dYaw,   aMouse);
        SmoothedMousePitch   = FMath::Lerp(SmoothedMousePitch, dPitch, aMouse);

        const float tauThrust = 0.12f;
        const float aThrust   = 1.f - FMath::Exp(-Dt / FMath::Max(0.001f, tauThrust));
        SmoothedThrustForward = FMath::Lerp(SmoothedThrustForward, uFwd,   aThrust);
        SmoothedThrustRight   = FMath::Lerp(SmoothedThrustRight,   uRight, aThrust);
        SmoothedThrustUp      = FMath::Lerp(SmoothedThrustUp,      uUp,    aThrust);

        const float tauRoll = FMath::Max(0.001f, RollSmoothingTime);
        const float aRoll   = 1.f - FMath::Exp(-Dt / tauRoll);
        PrevRollAxis        = FMath::Lerp(PrevRollAxis, rollAxisImmediate, aRoll);

        if (FMath::Abs(SmoothedMouseYaw)   < 0.002f) SmoothedMouseYaw   = 0.f;
        if (FMath::Abs(SmoothedMousePitch) < 0.002f) SmoothedMousePitch = 0.f;
        if (FMath::Abs(PrevRollAxis)       < 0.002f) PrevRollAxis       = 0.f;
    }

    // --- Ремап thrust из рамки носа в базис Flight ---
    float thF = 0.f, thR = 0.f, thU = 0.f;
    RemapThrust_FromNoseToNetSimBasis(
        Ow, Flight.Get(), SelfPrim,
        Fwd, Right, Up,
        SmoothedThrustForward, SmoothedThrustRight, SmoothedThrustUp,
        thF, thR, thU);

    // Разбудим физику
    if (UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Ow->GetRootComponent()))
    {
        if (RootPrim->IsSimulatingPhysics())
        {
            RootPrim->WakeAllRigidBodies();
        }
    }

    // --- Отправка команд в FlightComponent ---
    Flight->AddMouseYaw   (SmoothedMouseYaw);
    Flight->AddMousePitch (SmoothedMousePitch);
    Flight->SetStrafeRight(thR);
    Flight->SetThrustUp   (thU);
    Flight->SetThrustForward(thF);
    Flight->SetRollAxis(PrevRollAxis);

    // --- Debug ---
    if (bDrawDebug)
    {
        if (UWorld* W = GetWorld(); W && W->GetNetMode() != NM_DedicatedServer)
        {
            if (bHasAim)
            {
                DrawDebugLine(W, Loc, AimPoint, FColor::Cyan, false, 0.f, 0, 1.5f);
                DrawDebugSphere(W, AimPoint, 28.f, 16, FColor::Red, false, 0.f, 0, 1.5f);
                DrawDebugDirectionalArrow(W, Loc, Loc + AimDir * 450.f, 40.f, FColor::Blue, false, 0.f, 0, 1.5f);
            }

            DrawDebugDirectionalArrow(W, Loc, FollowAnchor, 40.f, FColor::Green, false, 0.f, 0, 1.5f);
            DrawDebugSphere(W, FollowAnchor, 22.f, 12, FColor::Green, false, 0.f, 0, 1.2f);

            DrawDebugDirectionalArrow(W, Loc, Loc + Fwd   * 250.f, 35.f, FColor::Red,   false, 0.f, 0, 2.f);
            DrawDebugDirectionalArrow(W, Loc, Loc + Right * 200.f, 35.f, FColor::Green, false, 0.f, 0, 2.f);
            DrawDebugDirectionalArrow(W, Loc, Loc + Up    * 150.f, 35.f, FColor::Blue,  false, 0.f, 0, 2.f);
            DrawDebugDirectionalArrow(W, Loc, Loc + UpReference.GetSafeNormal() * 180.f, 35.f, FColor::Yellow, false, 0.f, 0, 1.6f);
        }
    }
}


