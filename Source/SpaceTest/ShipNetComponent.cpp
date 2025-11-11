// ShipNetComponent.cpp
#include "ShipNetComponent.h"
#include "ShipPawn.h"
#include "FlightComponent.h"

#include "Components/StaticMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY(LogShipNet);

UShipNetComponent::UShipNetComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup    = TG_PostPhysics;
	SetIsReplicatedByDefault(true);
}

void UShipNetComponent::BeginPlay()
{
	Super::BeginPlay();

	OwPawn = Cast<APawn>(GetOwner());
	Ship   = Cast<AShipPawn>(GetOwner());

	if (Ship)
	{
		ShipMesh = Ship->ShipMesh;
		Flight   = Ship->Flight;
	}

	SetupTickOrder();
	UpdatePhysicsSimState();
}

void UShipNetComponent::SetupTickOrder()
{
	// важно: сначала физика/Flight, потом — мы
	if (Flight)
	{
		AddTickPrerequisiteComponent(Flight);
	}
	if (ShipMesh)
	{
		AddTickPrerequisiteComponent(ShipMesh);
	}
}

void UShipNetComponent::SetLocalAxes(float F, float R, float U, float Roll)
{
	AxisF_Cur = F; AxisR_Cur = R; AxisU_Cur = U; AxisRoll_Cur = Roll;
}

void UShipNetComponent::AddMouseDelta(float Dx, float Dy)
{
	MouseX_Accum += Dx;
	MouseY_Accum += Dy;
}

void UShipNetComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!OwPawn || !ShipMesh) return;

	UpdatePhysicsSimState();

	// === 1) Владелец → сервер: RPC с инпутом + локальное кэширование ===
	if (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole() == ROLE_AutonomousProxy)
	{
		FControlState St;
		St.Seq       = ++LocalInputSeq;
		St.DeltaTime = DeltaTime;
		St.ThrustF   = AxisF_Cur;
		St.ThrustR   = AxisR_Cur;
		St.ThrustU   = AxisU_Cur;
		St.Roll      = AxisRoll_Cur;
		St.MouseX    = MouseX_Accum;
		St.MouseY    = MouseY_Accum;

		Server_SendInput(St);

		// Запомним для ресима (ACK спилит хвост)
		PendingInputs.Add(St);

		MouseX_Accum = 0.f;
		MouseY_Accum = 0.f;

		// [PREDICT] тут идеальный момент выполнить локальный детерминированный SimStep
		// если UFlightComponent предоставит чистую функцию предсказания (без побочек).
		// Пока оставляем гибридную модель (физика уже к этому тик-кадру применена Flight’ом).
	}

	// === 2) Наблюдатели: интерполяция ===
	if (OwPawn->GetLocalRole() == ROLE_SimulatedProxy)
	{
		DriveSimulatedProxy();
	}

	// === 3) Владелец: мягкая реконсиляция (поверх локальной физики) ===
	if (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole() == ROLE_AutonomousProxy)
	{
		OwnerReconcile_Tick(DeltaTime);
	}

	// === 4) Сервер: снимаем авторитативный снап ===
	if (OwPawn->HasAuthority())
	{
		const FTransform X = ShipMesh->GetComponentTransform();
		const FVector    Wrad = ShipMesh->GetPhysicsAngularVelocityInRadians();

		ServerSnap.Loc       = X.GetLocation();
		FRotator R = X.Rotator();
		ServerSnap.RotCS.FromRotator(R);
		ServerSnap.Vel       = ShipMesh->GetComponentVelocity();
		ServerSnap.AngVelDeg = Wrad * (180.f / PI); // храню в deg/s для читаемой сетки
		ServerSnap.ServerTime= GetWorld()->GetTimeSeconds();
		// LastAckSeq сервер обновляет в Server_SendInput()
	}
}

// ShipNetComponent.cpp
void UShipNetComponent::UpdatePhysicsSimState()
{
	if (!Ship || !ShipMesh || !Flight || !OwPawn) return;

	const bool bOwner = (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole()==ROLE_AutonomousProxy);
	const bool bAuth  = OwPawn->HasAuthority();
	const bool bShouldSim = bAuth || bOwner;

	const bool bWasSim = ShipMesh->IsSimulatingPhysics();
	if (bWasSim != bShouldSim)
	{
		if (bShouldSim)
		{
			// Синхронизируем трансформ и включаем физику
			ShipMesh->SetWorldTransform(Ship->GetActorTransform(), false, nullptr, ETeleportType::TeleportPhysics);
			ShipMesh->SetSimulatePhysics(true);
			ShipMesh->WakeAllRigidBodies();

			// КРИТИЧЕСКО: сразу ребайндим тело для Flight
			Flight->RebindAfterSimToggle();
		}
		else
		{
			// Аккуратно гасим и выключаем
			ShipMesh->SetPhysicsLinearVelocity(FVector::ZeroVector, false);
			ShipMesh->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector, false);
			ShipMesh->PutAllRigidBodiesToSleep();
			ShipMesh->SetSimulatePhysics(false);
		}
	}

	// Тик Flight только там, где симулируем
	Flight->SetComponentTickEnabled(bShouldSim);
}


// ===== RPC =====
void UShipNetComponent::Server_SendInput_Implementation(const FControlState& State)
{
	// ACK — сервер сообщает, что обработал этот seq
	ServerSnap.LastAckSeq = State.Seq;

	// Санити/клампы здесь (защита от «сверх»-инпутов) — опустил ради краткости

	// Прокатываем public API, как было
	if (Flight)
	{
		Flight->SetThrustForward(State.ThrustF);
		Flight->SetStrafeRight (State.ThrustR);
		Flight->SetThrustUp    (State.ThrustU);
		Flight->SetRollAxis    (State.Roll);
		Flight->AddMouseYaw    (State.MouseX);
		Flight->AddMousePitch  (State.MouseY);
	}
}

// ===== OnRep (универсально: и owner, и сим-прокси) =====
void UShipNetComponent::OnRep_ServerSnap()
{
	const double Now = GetWorld() ? (double)GetWorld()->GetTimeSeconds() : 0.0;

	// --- Подстройка оффсета времени (EMA) + выборка для адаптивного delay ---
	{
		const double EstOffset = Now - (double)ServerSnap.ServerTime;
		if (!bHaveTimeSync)
		{
			ServerTimeToClientTime = EstOffset;
			bHaveTimeSync = true;
		}
		else
		{
			// EMA для оффсета
			ServerTimeToClientTime = FMath::Lerp(ServerTimeToClientTime, EstOffset, 0.10);
		}
		// выборка «остатка» для джиттера (чем ближе к нулю — тем точнее оффсет)
		const double Residual = EstOffset - ServerTimeToClientTime;
		AdaptiveInterpDelay_OnSample(Residual);
	}

	// --- Сим-прокси: интерп-буфер (клиентское время в снапе) ---
	if (OwPawn && OwPawn->GetLocalRole() == ROLE_SimulatedProxy)
	{
		FInterpNode N;
		N.Time   = (double)ServerSnap.ServerTime + ServerTimeToClientTime;
		N.Loc    = ServerSnap.Loc;
		N.Rot    = ServerSnap.RotCS.ToRotator().Quaternion();
		N.Vel    = ServerSnap.Vel;
		N.AngVel = (ServerSnap.AngVelDeg) * (PI / 180.f); // обратно в рад/с
		NetBuffer.Add(N);

		// Подрезать хвост старых
		const double KeepFrom = Now - 1.0;
		int32 FirstValid = 0;
		while (FirstValid < NetBuffer.Num() && NetBuffer[FirstValid].Time < KeepFrom) ++FirstValid;
		if (FirstValid > 0) NetBuffer.RemoveAt(0, FirstValid, EAllowShrinking::No);

		// ЛОГ
		if (UE_LOG_ACTIVE(LogShipNet, VeryVerbose))
		{
			UE_LOG(LogShipNet, VeryVerbose, TEXT("[SIMPROXY] NetBuffer=%d delay=%.3f"),
				NetBuffer.Num(), NetInterpDelay);
		}
	}
	// --- Владелец: ACK + целевой снап для мягкой реконсиляции/ресима ---
	else if (OwPawn && OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole() == ROLE_AutonomousProxy)
	{
		OwnerReconTarget = ServerSnap;
		bHaveOwnerRecon  = true;

		// Срезаем подтверждённые инпуты
		int32 CutIdx = INDEX_NONE;
		for (int32 i = PendingInputs.Num()-1; i >= 0; --i)
		{
			if (PendingInputs[i].Seq <= ServerSnap.LastAckSeq)
			{
				CutIdx = i; break;
			}
		}
		if (CutIdx >= 0)
		{
			PendingInputs.RemoveAt(0, CutIdx+1, EAllowShrinking::No);
		}

		// [PREDICT] Здесь можно сделать настоящий rollback:
		// 1) Восстановить снап S=OwnerReconTarget (Loc/Rot/Vel/AngVel)
		// 2) Прогнать оставшиеся PendingInputs детерминированным SimStep(S, dt, input)
		// 3) Установить S обратно на ShipMesh (через SetPhysics* / или мягко через скорости)
		// Пока оставляем мягкую реконсиляцию в Tick (ниже).
	}
}

// === Адаптивная настройка интерп-задержки по джиттеру (медиана + 0.5 * IQR90) ===
void UShipNetComponent::AdaptiveInterpDelay_OnSample(double OffsetSample)
{
	DelaySamples[DelaySamplesHead] = OffsetSample;
	DelaySamplesHead = (DelaySamplesHead + 1) % DelaySamples.Num();
	DelaySamplesCount = FMath::Min(DelaySamplesCount + 1, DelaySamples.Num());

	if (DelaySamplesCount < 16) return; // прогрев

	// копия окна и сорт
	TArray<double> W;
	W.Reserve(DelaySamplesCount);
	for (int32 i=0; i<DelaySamplesCount; ++i)
	{
		W.Add(DelaySamples[i]);
	}
	W.Sort();

	const auto Pct = [&](double p)->double {
		if (W.Num()==0) return 0.0;
		const double idx = FMath::Clamp(p * (W.Num()-1), 0.0, double(W.Num()-1));
		const int32 i0 = int32(FMath::FloorToDouble(idx));
		const int32 i1 = FMath::Min(i0+1, W.Num()-1);
		const double t = idx - i0;
		return FMath::Lerp(W[i0], W[i1], t);
	};

	const double Med  = Pct(0.50);
	const double P90  = Pct(0.90);
	const double NewD = FMath::Clamp(Med + 0.5*(P90 - Med), (double)NetInterpDelayMin, (double)NetInterpDelayMax);

	// сгладим
	NetInterpDelay = FMath::Lerp(NetInterpDelay, (float)NewD, 0.15f);
}

// === Сим-прокси: интерполяция Hermite + интеграл ориентации по AngVel ===
FQuat UShipNetComponent::IntegrateQuat(const FQuat& Q0, const FVector& Wrad, float Dt)
{
	const float Angle = Wrad.Size() * Dt;
	if (Angle < 1e-6f) return Q0;
	const FVector Axis = Wrad.GetSafeNormal();
	const FQuat dQ(Axis, Angle);
	return (dQ * Q0).GetNormalized();
}

void UShipNetComponent::DriveSimulatedProxy()
{
	if (!Ship || !ShipMesh || NetBuffer.Num()==0 || !bHaveTimeSync || !GetWorld()) return;

	const double Now = GetWorld()->GetTimeSeconds();
	const double Tq  = Now - (double)NetInterpDelay;

	// граничные случаи
	if (NetBuffer.Num()==1 || Tq <= NetBuffer[0].Time)
	{
		const auto& N = NetBuffer[0];
		Ship->SetActorLocationAndRotation(N.Loc, N.Rot.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}
	if (Tq >= NetBuffer.Last().Time)
	{
		// короткая экстраполяция ≤ ~100 мс на основе скоростей
		const auto& L = NetBuffer.Last();
		const double Ahead = FMath::Min(0.1, Tq - L.Time);
		const FVector P = L.Loc + L.Vel * (float)Ahead;
		const FQuat   Q = IntegrateQuat(L.Rot, L.AngVel, (float)Ahead);
		Ship->SetActorLocationAndRotation(P, Q.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}

	// бинарный поиск
	int32 L=0, R=NetBuffer.Num()-1;
	while (L+1<R)
	{
		const int32 M=(L+R)/2;
		if (NetBuffer[M].Time <= Tq) L=M; else R=M;
	}
	const auto& A = NetBuffer[L];
	const auto& B = NetBuffer[L+1];

	const double dt = FMath::Max(1e-6, B.Time - A.Time);
	const float  s  = (float)FMath::Clamp((Tq - A.Time)/dt, 0.0, 1.0);

	// Hermite по Loc/Vel
	const float s2=s*s, s3=s2*s;
	const float h00 =  2*s3 - 3*s2 + 1;
	const float h10 =    s3 - 2*s2 + s;
	const float h01 = -2*s3 + 3*s2;
	const float h11 =    s3 -   s2;

	const FVector P =  h00*A.Loc + h10*(A.Vel*(float)dt)
	                 + h01*B.Loc + h11*(B.Vel*(float)dt);

	// Ориентация: SLERP + корректный интеграл угл. скорости между узлами
	// (квози-скоррекция: эквивалент добавлению "twist" от AngVel)
	FQuat QA = A.Rot;
	FQuat QB = B.Rot;
	const FQuat QAe = IntegrateQuat(QA, A.AngVel, (float)(s*(float)dt));       // до точки внутри интервала
	const FQuat QBe = IntegrateQuat(QB, -B.AngVel, (float)(((1.f-s))* (float)dt)); // обратная "подтяжка"
	const FQuat Qs  = FQuat::Slerp(QAe, QBe, s).GetNormalized();

	Ship->SetActorLocationAndRotation(P, Qs.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
}

// === Владелец: мягкая реконсиляция с ограниченным «пинком» ===
// ShipNetComponent.cpp
void UShipNetComponent::OwnerReconcile_Tick(float DeltaSeconds)
{
	if (!bHaveOwnerRecon || !ShipMesh || DeltaSeconds <= 0.f || !GetWorld()) return;

	const double Now     = GetWorld()->GetTimeSeconds();
	const double Tserver = (double)OwnerReconTarget.ServerTime + ServerTimeToClientTime;
	const double Ahead   = FMath::Max(0.0, Now - Tserver);

	// Целевое состояние, протянутое "вперёд" на локальный лаг
	const FVector LocS = OwnerReconTarget.Loc + OwnerReconTarget.Vel * (float)Ahead;
	const FQuat   RotS = OwnerReconTarget.RotCS.ToRotator().Quaternion();
	const FVector VelS = OwnerReconTarget.Vel;
	const FVector Wrad = (OwnerReconTarget.AngVelDeg) * (PI / 180.f);

	const FVector LocC = ShipMesh->GetComponentLocation();
	const FQuat   RotC = ShipMesh->GetComponentQuat();

	// Жёсткий снап на крупной ошибке (например, после телепорта)
	const double errPos = (LocS - LocC).Size();
	if (errPos > (double)OwnerHardSnapDistance)
	{
		ShipMesh->SetWorldLocationAndRotation(LocS, RotS.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		ShipMesh->SetPhysicsLinearVelocity(VelS);
		ShipMesh->SetPhysicsAngularVelocityInRadians(Wrad, false);
		return;
	}

	const float Tau   = FMath::Max(0.02f, OwnerReconTau);
	const float Alpha = 1.f - FMath::Exp(-DeltaSeconds / Tau);

	// --- Линейная часть ---
	const FVector VelC = ShipMesh->GetComponentVelocity();
	const FVector Vtgt = VelS + (LocS - LocC) / Tau;

	FVector Vnew = FMath::Lerp(VelC, Vtgt, Alpha);
	const FVector Nudge = Vnew - VelC;
	const float MaxNudge = OwnerMaxVelNudge * DeltaSeconds; // ограничим "пинок"
	if (Nudge.Size() > MaxNudge)
		Vnew = VelC + Nudge.GetClampedToMaxSize(MaxNudge);

	ShipMesh->SetPhysicsLinearVelocity(Vnew, false);

	// --- Угловая часть ---
	FVector Axis; float Angle=0.f;
	(RotS * RotC.Inverse()).ToAxisAndAngle(Axis, Angle);
	if (Angle > PI) Angle -= 2.f*PI;
	Axis = Axis.GetSafeNormal();

	const FVector Wcur = ShipMesh->GetPhysicsAngularVelocityInRadians();
	const FVector Wtgt = Wrad + Axis * (Angle / Tau);
	const FVector Wnew = FMath::Lerp(Wcur, Wtgt, Alpha);
	ShipMesh->SetPhysicsAngularVelocityInRadians(Wnew, false);
}


// === Репликация ===
// ShipNetComponent.cpp

void UShipNetComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UShipNetComponent, ServerSnap);
}

