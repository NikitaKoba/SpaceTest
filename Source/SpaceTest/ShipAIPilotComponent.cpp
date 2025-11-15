// ShipAIPilotComponent.cpp

#include "ShipAIPilotComponent.h"
#include "FlightComponent.h"
#include "ShipPawn.h"

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
    AActor* Ow     = GetOwner();
    AActor* Target = ResolveTarget();

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

    // Скорость корабля
    const FVector Vself_cmps = Body->GetPhysicsLinearVelocity();
    const FVector Vself_mps  = Vself_cmps / 100.f;

    // Проекция скорости на нашу ось вперёд (как будто газ W)
    const float Vf_self = FVector::DotProduct(Vself_mps, FwdW);

    // Ошибка по дистанции (в метрах)
    const float DesiredDistCm = FollowDistanceCm;
    const float DistErrCm     = Dist - DesiredDistCm;
    const float DistErr_m     = DistErrCm / 100.f;

    const float VxMax = FMath::Max(Flight->Longi.VxMax_Mps, 1.f);

    // Простой PD: хотим скорость вперёд, чтобы выйти на нужную дистанцию
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

    // ВАЖНО: пока выключаем страф и вертикаль, чтобы не было спиралей/сумасшествий
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
    // Если цель дальше чем ~140° от носа — F×Dir почти 0, используем чистый разворот по yaw.
    const float rearThresholdDeg = 140.f;
    if (angleToTargetDeg > rearThresholdDeg)
    {
        // Определяем, крутиться влево или вправо:
        // если цель справа от нас (по Right-вектору), крутимся вправо, иначе влево.
        const float sideDot = FVector::DotProduct(DirToTargetW, RightW); // + цель справа, - слева
        const float yawSign = (sideDot >= 0.f) ? 1.f : -1.f;

        YawRateCmdDeg   = yawSign * YawRateMax * 0.7f; // 70% от максимального разворота
        PitchRateCmdDeg = 0.f;
        RollRateCmdDeg  = 0.f; // пока не банкуем

        Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
        return;
    }

    // --- ОБЫЧНЫЙ РЕЖИМ: кросс-продукт Fwd и DirToTarget ---
    FVector axisWorld = FVector::CrossProduct(FwdW, DirToTargetW);
    const float axisLen = axisWorld.Size();

    if (axisLen < KINDA_SMALL_NUMBER)
    {
        // Уже почти смотрим на цель (или почти строго обратно, но это мы выше отфильтровали)
        Flight->SetAngularRateOverride(false, 0.f, 0.f, 0.f);
        return;
    }

    // Максимальная скорость выравнивания (рад/с)
    const float AlignRateMaxDeg = 1.8f * FMath::Min(YawRateMax, PitchRateMax); // берём 80% от жесткого лимита
    const float AlignRateMaxRad = FMath::DegreesToRadians(AlignRateMaxDeg);

    // Вектор угловой скорости в МИРОВЫХ координатах:
    //  - направление = axisWorld
    //  - длина ~ sin(theta) * AlignRateMax
    FVector omegaWorld = axisWorld * AlignRateMaxRad;

    // Переводим в ЛОКАЛЬНЫЕ координаты корабля, чтобы вытащить yaw/pitch
    FVector omegaLocal = TM.InverseTransformVectorNoScale(omegaWorld);

    const float pitchRateRad = omegaLocal.Y; // вращение вокруг локальной оси Y (pitch)
    const float yawRateRad   = omegaLocal.Z; // вращение вокруг локальной оси Z (yaw)

    PitchRateCmdDeg = FMath::Clamp(FMath::RadiansToDegrees(pitchRateRad), -PitchRateMax, PitchRateMax);
    YawRateCmdDeg   = FMath::Clamp(FMath::RadiansToDegrees(yawRateRad),   -YawRateMax,   YawRateMax);

    // Небольшое ослабление команд, когда мы почти выровнялись, чтобы нос не дрожал
    const float AlignScale = FMath::GetMappedRangeValueClamped(
        FVector2D(0.f, 30.f),     // для углов до 30°
        FVector2D(0.6f, 1.0f),    // масштаб от 0.6 до 1.0
        angleToTargetDeg
    );
    PitchRateCmdDeg *= AlignScale;
    YawRateCmdDeg   *= AlignScale;

    // Пока ROLL=0 — не даём боту заваливаться, чтобы не висел кверх ногами
    RollRateCmdDeg = 0.f;

    Flight->SetAngularRateOverride(true, PitchRateCmdDeg, YawRateCmdDeg, RollRateCmdDeg);
}
