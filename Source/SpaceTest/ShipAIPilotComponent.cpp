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
			Flight->Frame.Forward = C[BestF].Sel; // «вперёд» = +X корпуса, ближайший к NoseF
			Flight->Frame.Up      = C[BestU].Sel; // «вверх»  = +Z корпуса, ближайший к NoseU
			UE_LOG(LogShipBot, Display, TEXT("[Bot] Rebound Flight frame to Nose  F=%d U=%d"),
				(int32)C[BestF].Sel, (int32)C[BestU].Sel);
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

    // ---------- 1. ЦЕЛЬ / AIM ----------
    AActor* Target      = D.AimActor.Get();
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

    // Если вообще нечего делать — отпускаем всё
    if (!bHasAim && !D.FollowAnchorW.IsSet() && !Target)
    {
        Flight->SetStrafeRight(0.f);
        Flight->SetThrustUp(0.f);
        Flight->SetThrustForward(0.f);
        Flight->SetRollAxis(0.f);
        ResetTransientState();
        AimHoldTimerS  = 0.f;
        bAimHoldActive = false;
        PrevRollAxis   = 0.f;
        return;
    }

    // ---------- 2. Базис корабля (никаких рамок) ----------
    const FTransform ShipTM = Ow->GetActorTransform();
    const FVector   Loc     = ShipTM.GetLocation();
    const FVector   Fwd     = ShipTM.GetUnitAxis(EAxis::X); // вперёд
    const FVector   Right   = ShipTM.GetUnitAxis(EAxis::Y); // вправо
    const FVector   Up      = ShipTM.GetUnitAxis(EAxis::Z); // вверх

    // Физика
    UPrimitiveComponent* SelfPrim = GetSimPrim(Ow);
    const FVector vSelfW = SelfPrim ? SelfPrim->GetComponentVelocity() : FVector::ZeroVector;

    FVector angVelRadLocal = FVector::ZeroVector;
    if (SelfPrim)
    {
        const FVector wWorld = SelfPrim->GetPhysicsAngularVelocityInRadians();
        angVelRadLocal = ShipTM.InverseTransformVectorNoScale(wWorld);
    }
    const FVector angVelDegLocal(
        FMath::RadiansToDegrees(angVelRadLocal.X),
        FMath::RadiansToDegrees(angVelRadLocal.Y),
        FMath::RadiansToDegrees(angVelRadLocal.Z));

    // ---------- 3. Геометрия цели ----------
    const UPrimitiveComponent* TgtPrim = Target ? GetSimPrim(Target) : nullptr;
    const FVector vTgtW = TgtPrim ? TgtPrim->GetComponentVelocity() : FVector::ZeroVector;
    const FVector TgtFwd = Target ? Target->GetActorForwardVector() : FVector::ForwardVector;

    FVector AimDir = Fwd;
    float   DistToAimM = 0.f;
    if (bHasAim)
    {
        const FVector ToAim = AimPoint - Loc;
        DistToAimM          = ToAim.Size() / 100.f;
        if (!ToAim.IsNearlyZero())
            AimDir = ToAim.GetSafeNormal();
    }

    // ---------- 4. Поворот носа (PD) ----------
    float yawErrRad   = 0.f;
    float pitchErrRad = 0.f;
    if (bHasAim)
    {
        yawErrRad   = SignedAngleAroundAxisRad(Fwd, AimDir, Up);
        pitchErrRad = SignedAngleAroundAxisRad(Fwd, AimDir, Right);
    }

    const float yawErrDeg   = FMath::RadiansToDegrees(yawErrRad);
    const float pitchErrDeg = FMath::RadiansToDegrees(pitchErrRad);

    // Kp: при ~60° ошибке даём полный поворот
    const float MaxYawForFullDeg   = 60.f;
    const float MaxPitchForFullDeg = 45.f;
    const float KpYaw   = 1.f / MaxYawForFullDeg;
    const float KpPitch = 1.f / MaxPitchForFullDeg;

    // Kd: демпфер по угловой скорости
    const float KdYaw   = 1.f / 180.f; // 180°/с -> -1
    const float KdPitch = 1.f / 180.f;

    float yawCmd =
        KpYaw   * yawErrDeg
      - KdYaw   * angVelDegLocal.Z; // Z ~ yaw вокруг Up

    float pitchCmd =
        -KpPitch * pitchErrDeg      // минус, чтобы "цель выше" -> pitch up
      - KdPitch * angVelDegLocal.Y; // Y ~ pitch вокруг Right

    yawCmd   = FMath::Clamp(yawCmd,   -1.f, 1.f);
    pitchCmd = FMath::Clamp(pitchCmd, -1.f, 1.f);

    // Превращаем в «виртуальную мышь»
    float dYaw   = yawCmd   * MaxMouseDeltaPerFrame;
    float dPitch = pitchCmd * MaxMouseDeltaPerFrame;

    // Маленькая мёртвая зона — чтобы не дрожал на нуле
    if (FMath::Abs(yawErrDeg) < YawDeadzoneDeg)     dYaw   = 0.f;
    if (FMath::Abs(pitchErrDeg) < PitchDeadzoneDeg) dPitch = 0.f;

    // ---------- 5. Follow-anchor (точка, где бот хочет быть) ----------
    const float BackMeters = D.FollowBehindMetersOverride.IsSet() ? D.FollowBehindMetersOverride.GetValue() : FollowBehindMeters;
    const float LeadSec    = D.FollowLeadSecondsOverride.IsSet()  ? D.FollowLeadSecondsOverride.GetValue()  : FollowLeadSeconds;
    const float MinAppro   = D.MinApproachMetersOverride.IsSet()  ? D.MinApproachMetersOverride.GetValue()  : MinApproachMeters;

    FVector DesiredPos = Loc;

    if (D.FollowAnchorW.IsSet())
    {
        DesiredPos = D.FollowAnchorW.GetValue();
    }
    else if (Target)
    {
        const FVector TgtFuture = Target->GetActorLocation() + vTgtW * LeadSec;
        const float   Back      = FMath::Max(BackMeters, MinAppro);
        DesiredPos             = TgtFuture - TgtFwd * (Back * 100.f); // м → см
    }

    const FVector ToDesiredW = DesiredPos - Loc;
    const float   DistToAnchorM = ToDesiredW.Size() / 100.f;

    // Локальные смещения и относительные скорости в базисе корабля
    const FVector ToDesiredLocal = ShipTM.InverseTransformVectorNoScale(ToDesiredW); // см
    const FVector vRelLocalUU    = ShipTM.InverseTransformVectorNoScale(vTgtW - vSelfW); // uu/s

    const float offFwdM   = ToDesiredLocal.X / 100.f;
    const float offRightM = ToDesiredLocal.Y / 100.f;
    const float offUpM    = ToDesiredLocal.Z / 100.f;

    const float relFwdMS   = vRelLocalUU.X / 100.f;
    const float relRightMS = vRelLocalUU.Y / 100.f;
    const float relUpMS    = vRelLocalUU.Z / 100.f;

    // ---------- 6. Линейная PD-центровка ----------
    // Kp берём из твоих настроек, Kd фиксированные
    const float KdLat_InputPerMS = 0.20f;
    const float KdFwd_InputPerMS = 0.25f;

    float uRight = 0.f;
    if (!FMath::IsNearlyZero(offRightM, LateralDeadzoneM))
        uRight = KpRight_InputPerM * offRightM + KdLat_InputPerMS * relRightMS;

    float uUp = 0.f;
    if (!FMath::IsNearlyZero(offUpM, VerticalDeadzoneM))
        uUp = KpUp_InputPerM * offUpM + KdLat_InputPerMS * relUpMS;

    float uFwd = 0.f;
    if (!FMath::IsNearlyZero(offFwdM, ForwardDeadzoneM))
        uFwd = KpForward_InputPerM * offFwdM + KdFwd_InputPerMS * relFwdMS;

    uRight = FMath::Clamp(uRight, -1.f, 1.f);
    uUp    = FMath::Clamp(uUp,    -1.f, 1.f);
    uFwd   = FMath::Clamp(uFwd,   -1.f, 1.f);

    // Чуть завязать скорость на ориентацию к anchor,
    // но НЕ обнулять — чтобы не зависал.
    FVector dirToAnchor = ToDesiredW.IsNearlyZero() ? Fwd : ToDesiredW.GetSafeNormal();
    const float alignToAnchor = FMath::Clamp(FVector::DotProduct(Fwd, dirToAnchor), -1.f, 1.f);
    const float fAlign        = FMath::Clamp(0.2f + 0.8f * FMath::Max(alignToAnchor, 0.f), 0.2f, 1.f);
    uFwd *= fAlign;

    // ---------- 7. Анти-таран около цели ----------
    if (Target && DistToAimM > 1.f)
    {
        const float NoRamRadiusM = FMath::Max(10.f, MinAppro * 0.9f);
        if (DistToAimM < NoRamRadiusM)
        {
            const float Penetration = (NoRamRadiusM - DistToAimM) / NoRamRadiusM; // 0..1
            const float Brake       = -FMath::Lerp(0.2f, 1.0f, Penetration);
            uFwd = FMath::Min(uFwd, Brake); // начинаем сдавать назад
        }
    }

    // Ограничиваем задний ход
    const float MaxReverse = 0.6f;
    uFwd = FMath::Clamp(uFwd, -MaxReverse, 1.f);

    // ---------- 8. Сглаживание ----------
    {
        const float tauMouse = 0.03f;
        const float aMouse   = 1.f - FMath::Exp(-Dt / FMath::Max(0.001f, tauMouse));
        SmoothedMouseYaw     = FMath::Lerp(SmoothedMouseYaw,   dYaw,   aMouse);
        SmoothedMousePitch   = FMath::Lerp(SmoothedMousePitch, dPitch, aMouse);

        const float tauThrust = 0.10f;
        const float aThrust   = 1.f - FMath::Exp(-Dt / FMath::Max(0.001f, tauThrust));
        SmoothedThrustForward = FMath::Lerp(SmoothedThrustForward, uFwd,   aThrust);
        SmoothedThrustRight   = FMath::Lerp(SmoothedThrustRight,   uRight, aThrust);
        SmoothedThrustUp      = FMath::Lerp(SmoothedThrustUp,      uUp,    aThrust);

        if (FMath::Abs(SmoothedMouseYaw)   < 0.001f) SmoothedMouseYaw   = 0.f;
        if (FMath::Abs(SmoothedMousePitch) < 0.001f) SmoothedMousePitch = 0.f;
    }

    // ---------- 9. Отправляем в FlightComponent ----------
    // Здесь БЕЗ всякого ремапа: предполагаем, что Frame уже настроен под Actor.
    if (UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Ow->GetRootComponent()))
    {
        if (RootPrim->IsSimulatingPhysics())
            RootPrim->WakeAllRigidBodies();
    }

    Flight->AddMouseYaw   (SmoothedMouseYaw);
    Flight->AddMousePitch (SmoothedMousePitch);
    Flight->SetStrafeRight(SmoothedThrustRight);
    Flight->SetThrustUp   (SmoothedThrustUp);
    Flight->SetThrustForward(SmoothedThrustForward);
    Flight->SetRollAxis(0.f); // крен пока ровный

    // ---------- 10. Debug ----------
    if (bDrawDebug)
    {
        if (UWorld* W = GetWorld(); W && W->GetNetMode() != NM_DedicatedServer)
        {
            if (bHasAim)
            {
                DrawDebugLine(W, Loc, AimPoint, FColor::Cyan, false, 0.f, 0, 1.5f);
                DrawDebugSphere(W, AimPoint, 28.f, 16, FColor::Red, false, 0.f, 0, 1.5f);
            }

            DrawDebugSphere(W, DesiredPos, 30.f, 12, FColor::Green, false, 0.f, 0, 1.5f);
            DrawDebugDirectionalArrow(W, Loc, Loc + Fwd   * 300.f, 40.f, FColor::Red,   false, 0.f, 0, 2.f);
            DrawDebugDirectionalArrow(W, Loc, Loc + Right * 200.f, 40.f, FColor::Green, false, 0.f, 0, 2.f);
            DrawDebugDirectionalArrow(W, Loc, Loc + Up    * 150.f, 40.f, FColor::Blue,  false, 0.f, 0, 2.f);
        }
    }
}


