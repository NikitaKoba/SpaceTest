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

	// === 1) Владелец → сервер: отправляем инпут + кэш для ACK ===
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
		PendingInputs.Add(St);

		MouseX_Accum = 0.f;
		MouseY_Accum = 0.f;
	}

	const bool bOwner      = (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole() == ROLE_AutonomousProxy);
	const bool bOwnerNoSim = bOwner && !ShipMesh->IsSimulatingPhysics();

	// === 2) Наблюдатели ИЛИ владелец без локальной физики: интерполяция ===
	if (OwPawn->GetLocalRole() == ROLE_SimulatedProxy || bOwnerNoSim)
	{
		DriveSimulatedProxy(); // безопасно для владельца без физики — двигаем актор по снапам
	}

	// === 3) Владелец с локальной физикой: мягкая реконсиляция поверх своей симуляции ===
	if (bOwner && ShipMesh->IsSimulatingPhysics())
	{
		OwnerReconcile_Tick(DeltaTime);
	}

	// === 4) Сервер: снимаем авторитетный снап ===
	if (OwPawn->HasAuthority())
	{
		const FTransform X = ShipMesh->GetComponentTransform();
		const FVector    Wrad = ShipMesh->GetPhysicsAngularVelocityInRadians();

		ServerSnap.Loc        = X.GetLocation();
		FRotator R = X.Rotator();
		ServerSnap.RotCS.FromRotator(R);
		ServerSnap.Vel        = ShipMesh->GetComponentVelocity();
		ServerSnap.AngVelDeg  = Wrad * (180.f / PI);
		ServerSnap.ServerTime = GetWorld()->GetTimeSeconds();
		ServerSnap.LastAckSeq = LastReceivedSeq; // обновляется в Server_SendInput
	}
}


void UShipNetComponent::UpdatePhysicsSimState()
{
	// ВАЖНО:
	//   - Больше НЕ меняем ShipMesh->SetSimulatePhysics() тут.
	//   - Решение о симуляции — только в AShipPawn::UpdateSimFlags().
	//   - Здесь лишь подстраиваем тик Flight под реальное состояние физики.

	if (!ShipMesh || !Flight) return;

	const bool bSimNow = ShipMesh->IsSimulatingPhysics();
	if (Flight->IsComponentTickEnabled() != bSimNow)
	{
		Flight->SetComponentTickEnabled(bSimNow);
	}
}

// ===== RPC =====
void UShipNetComponent::Server_SendInput_Implementation(const FControlState& State)
{
	// вместо мгновенного ACK просто запомним максимальный полученный seq
	LastReceivedSeq = FMath::Max(LastReceivedSeq, State.Seq);

	UE_LOG(LogShipNet, VeryVerbose, TEXT("RPC Server_SendInput: seq=%d dt=%.3f F=%.2f R=%.2f U=%.2f Roll=%.2f dMouse=(%.2f,%.2f)"),
		State.Seq, State.DeltaTime, State.ThrustF, State.ThrustR, State.ThrustU, State.Roll, State.MouseX, State.MouseY);

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

	// --- Синхронизация времени + адаптивная задержка ---
	{
		const double EstOffset = Now - (double)ServerSnap.ServerTime;
		if (!bHaveTimeSync)
		{
			ServerTimeToClientTime = EstOffset;
			bHaveTimeSync = true;
		}
		else
		{
			ServerTimeToClientTime = FMath::Lerp(ServerTimeToClientTime, EstOffset, 0.10);
		}
		const double Residual = EstOffset - ServerTimeToClientTime;
		AdaptiveInterpDelay_OnSample(Residual);
	}

	// --- Сим-прокси: обычный интерп-буфер ---
	if (OwPawn && OwPawn->GetLocalRole() == ROLE_SimulatedProxy)
	{
		FInterpNode N;
		N.Time   = (double)ServerSnap.ServerTime + ServerTimeToClientTime;
		N.Loc    = ServerSnap.Loc;
		N.Rot    = ServerSnap.RotCS.ToRotator().Quaternion();
		N.Vel    = ServerSnap.Vel;
		N.AngVel = (ServerSnap.AngVelDeg) * (PI / 180.f);
		NetBuffer.Add(N);

		const double KeepFrom = Now - 1.0;
		int32 FirstValid = 0;
		while (FirstValid < NetBuffer.Num() && NetBuffer[FirstValid].Time < KeepFrom) ++FirstValid;
		if (FirstValid > 0) NetBuffer.RemoveAt(0, FirstValid, EAllowShrinking::No);
	}
	// --- Владелец: ACK + цель для реконсиляции, и ЕСЛИ физика выключена — тоже кладём в интерп-буфер ---
	else if (OwPawn && OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole() == ROLE_AutonomousProxy)
	{
		OwnerReconTarget = ServerSnap;
		bHaveOwnerRecon  = true;

		// Срез подтверждённых инпутов
		int32 CutIdx = INDEX_NONE;
		for (int32 i = PendingInputs.Num()-1; i >= 0; --i)
		{
			if (PendingInputs[i].Seq <= ServerSnap.LastAckSeq) { CutIdx = i; break; }
		}
		if (CutIdx >= 0) PendingInputs.RemoveAt(0, CutIdx+1, EAllowShrinking::No);

		// ВАЖНО: если у владельца локальная физика отключена — ведём его через интерполяцию
		if (ShipMesh && !ShipMesh->IsSimulatingPhysics())
		{
			FInterpNode N;
			N.Time   = (double)ServerSnap.ServerTime + ServerTimeToClientTime;
			N.Loc    = ServerSnap.Loc;
			N.Rot    = ServerSnap.RotCS.ToRotator().Quaternion();
			N.Vel    = ServerSnap.Vel;
			N.AngVel = (ServerSnap.AngVelDeg) * (PI / 180.f);
			NetBuffer.Add(N);

			const double KeepFrom = Now - 1.0;
			int32 FirstValid = 0;
			while (FirstValid < NetBuffer.Num() && NetBuffer[FirstValid].Time < KeepFrom) ++FirstValid;
			if (FirstValid > 0) NetBuffer.RemoveAt(0, FirstValid, EAllowShrinking::No);
		}
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
void UShipNetComponent::OwnerReconcile_Tick(float DeltaSeconds)
{
	if (!bHaveOwnerRecon || !ShipMesh || DeltaSeconds <= 0.f || !GetWorld())
		return;

	const bool bHasPending = PendingInputs.Num() > 0; // есть не-ACKнутые инпуты

	// Переводим серверный снап во время клиента и чуть экстраполируем вперёд
	const double Now     = GetWorld()->GetTimeSeconds();
	const double Tserver = (double)OwnerReconTarget.ServerTime + ServerTimeToClientTime;
	const double Ahead   = FMath::Max(0.0, Now - Tserver);

	const FVector LocS = OwnerReconTarget.Loc + OwnerReconTarget.Vel * (float)Ahead;
	const FQuat   RotS = OwnerReconTarget.RotCS.ToRotator().Quaternion();
	const FVector VelS = OwnerReconTarget.Vel;
	const FVector Wrad = OwnerReconTarget.AngVelDeg * (PI / 180.f);

	const FVector LocC = ShipMesh->GetComponentLocation();
	const FQuat   RotC = ShipMesh->GetComponentQuat();

	// 1) Жёсткая защита от «разъехались далеко» — мгновенный снап
	const double ErrPos = (LocS - LocC).Size();
	if (ErrPos > (double)OwnerHardSnapDistance)
	{
		ShipMesh->SetWorldLocationAndRotation(LocS, RotS.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		ShipMesh->SetPhysicsLinearVelocity(VelS);
		ShipMesh->SetPhysicsAngularVelocityInRadians(Wrad, false);
		UE_LOG(LogShipNet, Verbose, TEXT("[OWNER] HARD-SNAP err=%.1fcm"), ErrPos);
		return;
	}

	// 2) Маленькая «мёртвая зона» — ничего не делаем, чтобы не гасить микродвижение
	const float DeadzonePosCm  = 10.f;     // 10 см
	const float DeadzoneAngDeg = 0.75f;    // < 1°
	FVector Axis; float Angle = 0.f;
	(RotS * RotC.Inverse()).ToAxisAndAngle(Axis, Angle);
	if (Angle > PI) Angle -= 2.f * PI;
	const float ErrAngDeg = FMath::Abs(FMath::RadiansToDegrees(Angle));
	if (ErrPos < DeadzonePosCm && ErrAngDeg < DeadzoneAngDeg)
		return;

	// 3) Пока есть не-ACKнутые инпуты — НЕ трогаем линейку вообще.
	//    Лёгкая подправка только угловой скорости, чтобы камера/нос не уплывали.
	if (bHasPending)
	{
		const float TauSoft = FMath::Max(0.02f, OwnerReconTau) * 3.f; // мягче, чем обычно
		const float Alpha   = FMath::Clamp(DeltaSeconds / TauSoft, 0.f, 1.f);

		Axis = Axis.GetSafeNormal();
		const FVector Wcur = ShipMesh->GetPhysicsAngularVelocityInRadians();
		const FVector Wtgt = Wrad + Axis * (Angle / TauSoft);
		const FVector Wnew = FMath::Lerp(Wcur, Wtgt, Alpha);
		ShipMesh->SetPhysicsAngularVelocityInRadians(Wnew, false);
		return;
	}

	// 4) Нормальная мягкая реконсиляция (когда весь твой инпут уже подтверждён)
	const float Tau   = FMath::Max(0.02f, OwnerReconTau);
	const float Alpha = 1.f - FMath::Exp(-DeltaSeconds / Tau);

	// ЛИНЕЙНАЯ часть: тянем скорость к (серверной + позиционная ошибка / Tau), но ограничиваем «пинок»
	const FVector VelC  = ShipMesh->GetComponentVelocity();
	const FVector Vtgt  = VelS + (LocS - LocC) / Tau;
	FVector       Vnew  = FMath::Lerp(VelC, Vtgt, Alpha);

	const float   MaxNudge = OwnerMaxVelNudge * DeltaSeconds; // лимит изменения скорости за тик
	const FVector Nudge    = Vnew - VelC;
	if (Nudge.Size() > MaxNudge)
		Vnew = VelC + Nudge.GetClampedToMaxSize(MaxNudge);

	ShipMesh->SetPhysicsLinearVelocity(Vnew, false);

	// УГЛОВАЯ часть
	Axis = Axis.GetSafeNormal();
	const FVector Wcur = ShipMesh->GetPhysicsAngularVelocityInRadians();
	const FVector Wtgt = Wrad + Axis * (Angle / Tau);
	const FVector Wnew = FMath::Lerp(Wcur, Wtgt, Alpha);
	ShipMesh->SetPhysicsAngularVelocityInRadians(Wnew, false);

	if (UE_LOG_ACTIVE(LogShipNet, VeryVerbose))
	{
		UE_LOG(LogShipNet, VeryVerbose, TEXT("[OWNER] reconc: errPos=%.2fcm errAng=%.2fdeg pend=%d ack=%d"),
			(float)ErrPos, ErrAngDeg, PendingInputs.Num(), OwnerReconTarget.LastAckSeq);
	}
}


// === Репликация ===
// ShipNetComponent.cpp

void UShipNetComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UShipNetComponent, ServerSnap);
}

