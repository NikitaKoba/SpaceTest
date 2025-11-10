// ShipNetComponent.cpp
#include "ShipNetComponent.h"
#include "ShipPawn.h"
#include "FlightComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

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

	// стартовая инициализация симуляции физики по роли
	UpdatePhysicsSimState();
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

	// 0) поддерживаем состояние симуляции по роли
	UpdatePhysicsSimState();

	// 1) владелец → сервер: RPC инпут
	if (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole()==ROLE_AutonomousProxy)
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

		MouseX_Accum = 0.f;
		MouseY_Accum = 0.f;
	}

	// 2) наблюдатели: интерполяция
	if (OwPawn->GetLocalRole()==ROLE_SimulatedProxy)
	{
		DriveSimulatedProxy();
	}

	// 3) владелец: непрерывная мягкая реконсиляция
	if (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole()==ROLE_AutonomousProxy)
	{
		OwnerReconcile_Tick(DeltaTime);
	}

	// 4) сервер: обновить авторитативный снап (уйдёт по репликации)
	if (OwPawn->HasAuthority())
	{
		const FTransform X = ShipMesh->GetComponentTransform();
		ServerState.Loc = X.GetLocation();
		ServerState.Rot = X.GetRotation();
		ServerState.Vel = ShipMesh->GetComponentVelocity();
		ServerState.ServerTime = GetWorld()->GetTimeSeconds();
	}
}

void UShipNetComponent::UpdatePhysicsSimState()
{
	if (!ShipMesh || !Flight || !OwPawn) return;

	const bool bShouldSim =
		OwPawn->HasAuthority() || (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole()==ROLE_AutonomousProxy);

	if (ShipMesh->IsSimulatingPhysics()!=bShouldSim)
	{
		ShipMesh->SetSimulatePhysics(bShouldSim);
	}
	Flight->SetComponentTickEnabled(bShouldSim);
}

void UShipNetComponent::Server_SendInput_Implementation(const FControlState& State)
{
	// отметим, что обработали пакет
	ServerState.LastAckSeq = State.Seq;

	// Прокатываем те же публичные вызовы, что и у клиента
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

void UShipNetComponent::OnRep_ServerState()
{
	const double Now = GetWorld() ? (double)GetWorld()->GetTimeSeconds() : 0.0;

	// EMA времени сервера → клиентское Now
	{
		const double EstOffset = Now - (double)ServerState.ServerTime;
		if (!bHaveTimeSync)
		{
			ServerTimeToClientTime = EstOffset;
			bHaveTimeSync = true;
		}
		else
		{
			ServerTimeToClientTime = FMath::Lerp(ServerTimeToClientTime, EstOffset, 0.10);
		}
	}

	if (OwPawn && OwPawn->GetLocalRole()==ROLE_SimulatedProxy)
	{
		// Снап в интерп-буфер (в клиентском времени)
		FInterpNode N;
		N.Time = (double)ServerState.ServerTime + ServerTimeToClientTime;
		N.Loc  = ServerState.Loc;
		N.Rot  = ServerState.Rot;
		N.Vel  = ServerState.Vel;
		NetBuffer.Add(N);

		// Подрезать старые
		const double KeepFrom = Now - 1.0;
		int32 FirstValid = 0;
		while (FirstValid < NetBuffer.Num() && NetBuffer[FirstValid].Time < KeepFrom) ++FirstValid;
		if (FirstValid>0) NetBuffer.RemoveAt(0, FirstValid, EAllowShrinking::No);
	}
	else if (OwPawn && OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole()==ROLE_AutonomousProxy)
	{
		// целевой снап для непрерывной коррекции
		OwnerReconTarget = ServerState;
		bHaveOwnerRecon  = true;
	}
}

void UShipNetComponent::DriveSimulatedProxy()
{
	if (!Ship || !ShipMesh || NetBuffer.Num()==0 || !bHaveTimeSync || !GetWorld()) return;

	const double Now = GetWorld()->GetTimeSeconds();
	const double Tq  = Now - (double)NetInterpDelay;

	if (NetBuffer.Num()==1 || Tq <= NetBuffer[0].Time)
	{
		const auto& N = NetBuffer[0];
		Ship->SetActorLocationAndRotation(N.Loc, N.Rot.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}
	if (Tq >= NetBuffer.Last().Time)
	{
		const auto& N = NetBuffer.Last();
		Ship->SetActorLocationAndRotation(N.Loc, N.Rot.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}

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

	// кубический Hermite по Loc/Vel, slerp по Rot
	const float s2=s*s, s3=s2*s;
	const float h00 =  2*s3 - 3*s2 + 1;
	const float h10 =    s3 - 2*s2 + s;
	const float h01 = -2*s3 + 3*s2;
	const float h11 =    s3 -   s2;

	const FVector P =  h00*A.Loc + h10*(A.Vel*(float)dt)
	                 + h01*B.Loc + h11*(B.Vel*(float)dt);
	const FQuat   Q = FQuat::Slerp(A.Rot, B.Rot, s);

	Ship->SetActorLocationAndRotation(P, Q.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
}

void UShipNetComponent::OwnerReconcile_Tick(float DeltaSeconds)
{
	if (!bHaveOwnerRecon || !ShipMesh || DeltaSeconds <= 0.f || !GetWorld()) return;

	const double Now     = GetWorld()->GetTimeSeconds();
	const double Tserver = (double)OwnerReconTarget.ServerTime + ServerTimeToClientTime;
	const double Ahead   = FMath::Max(0.0, Now - Tserver);

	const FVector LocS = OwnerReconTarget.Loc + OwnerReconTarget.Vel * Ahead;
	const FQuat   RotS = OwnerReconTarget.Rot;
	const FVector VelS = OwnerReconTarget.Vel;

	const FVector LocC = ShipMesh->GetComponentLocation();
	const FQuat   RotC = ShipMesh->GetComponentQuat();

	// жёсткий снап при грубой рассинхре
	const double errPos = (LocS - LocC).Size();
	if (errPos > (double)OwnerHardSnapDistance)
	{
		ShipMesh->SetWorldLocationAndRotation(LocS, RotS.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		ShipMesh->SetPhysicsLinearVelocity(VelS);
		ShipMesh->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector, false);
		return;
	}

	// мягкая коррекция скорости (критически демпфированная пружина с Tau)
	const float Tau   = FMath::Max(0.02f, OwnerReconTau);
	const float Alpha = 1.f - FMath::Exp(-DeltaSeconds / Tau);

	const FVector VelC   = ShipMesh->GetComponentVelocity(); // см/с
	const FVector Vtgt   = VelS + (LocS - LocC) / Tau;
	FVector       Vnew   = FMath::Lerp(VelC, Vtgt, Alpha);

	// лимит «пинка» за тик — чтобы не было ступеньки
	const FVector Nudge     = Vnew - VelC;
	const float   MaxNudge  = OwnerMaxVelNudge * DeltaSeconds;
	if (Nudge.Size() > MaxNudge)
		Vnew = VelC + Nudge.GetClampedToMaxSize(MaxNudge);

	ShipMesh->SetPhysicsLinearVelocity(Vnew, false);

	// угловая часть
	FVector Axis; float Angle=0.f;
	(RotS * RotC.Inverse()).ToAxisAndAngle(Axis, Angle);
	if (Angle > PI) Angle -= 2.f*PI;
	Axis = Axis.GetSafeNormal();

	const FVector Wcur = ShipMesh->GetPhysicsAngularVelocityInRadians();
	const FVector Wtgt = Axis * (Angle / Tau);
	const FVector Wnew = FMath::Lerp(Wcur, Wtgt, Alpha);
	ShipMesh->SetPhysicsAngularVelocityInRadians(Wnew, false);
}

void UShipNetComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UShipNetComponent, ServerState);
}
