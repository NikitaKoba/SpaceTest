// ShipPawn.cpp
#include "ShipPawn.h"
#include "FlightComponent.h"

#include "Components/StaticMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/App.h"
#include "Net/UnrealNetwork.h"

static FORCEINLINE float Clamp01(float v){ return FMath::Clamp(v, 0.f, 1.f); }

AShipPawn::AShipPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup    = TG_PostPhysics;

	bReplicates = true;
	SetReplicateMovement(false);       // сами двигаем (interp/reconcile)
	NetUpdateFrequency    = 120.f;     // чаще снапы — глаже
	MinNetUpdateFrequency = 60.f;
	bAlwaysRelevant = true;   
	bOnlyRelevantToOwner = false;
	// Root mesh (physics)
	ShipMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ShipMesh"));
	SetRootComponent(ShipMesh);
	ShipMesh->SetSimulatePhysics(true);
	ShipMesh->SetEnableGravity(false);
	ShipMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	ShipMesh->SetCollisionObjectType(ECC_Pawn);
	ShipMesh->BodyInstance.bUseCCD = true;
	ShipMesh->SetLinearDamping(0.02f);
	ShipMesh->SetAngularDamping(0.02f);

	Flight = CreateDefaultSubobject<UFlightComponent>(TEXT("Flight"));

	// SpringArm как якорь оффсетов (без лагов/коллизий)
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 0.f;
	SpringArm->bEnableCameraLag = false;
	SpringArm->bEnableCameraRotationLag = false;
	SpringArm->bDoCollisionTest = false;
	SpringArm->bUsePawnControlRotation = false;
	SpringArm->SetUsingAbsoluteRotation(false);
	SpringArm->PrimaryComponentTick.bCanEverTick = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;
	Camera->FieldOfView = CameraFOV;
	Camera->PrimaryComponentTick.bCanEverTick = false;

	// если при спауне есть коллизия — всё равно заспаунить
	SpawnCollisionHandlingMethod = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
}

void AShipPawn::BeginPlay()
{
	Super::BeginPlay();

	UpdatePhysicsSimState();

	// стартовый сэмпл для камеры
	FCamSample S;
	S.Time = FApp::GetCurrentTime();
	const FTransform X = ShipMesh->GetComponentTransform();
	S.Loc = X.GetLocation();
	S.Rot = X.GetRotation();
	S.Vel = ShipMesh->GetComponentVelocity();
	PushCamSample(S);

	if (Camera) Camera->SetFieldOfView(CameraFOV);
}

void AShipPawn::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	UpdatePhysicsSimState();
}

void AShipPawn::OnRep_Owner()
{
	Super::OnRep_Owner();
	UpdatePhysicsSimState();
}

void AShipPawn::UpdatePhysicsSimState()
{
	// Server и локальный владелец — симулируют; наблюдатели — нет
	const bool bShouldSim = HasAuthority() || (IsLocallyControlled() && GetLocalRole()==ROLE_AutonomousProxy);

	if (ShipMesh && ShipMesh->IsSimulatingPhysics()!=bShouldSim)
	{
		ShipMesh->SetSimulatePhysics(bShouldSim);
	}

	if (Flight)
	{
		Flight->SetComponentTickEnabled(bShouldSim);
	}
}

void AShipPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// 1) камера: свежий сэмпл
	{
		FCamSample S;
		S.Time = FApp::GetCurrentTime();
		const FTransform X = ShipMesh->GetComponentTransform();
		S.Loc = X.GetLocation();
		S.Rot = X.GetRotation();
		S.Vel = ShipMesh->GetComponentVelocity();
		PushCamSample(S);
	}

	// 2) владелец → сервер: RPC инпут
	if (IsLocallyControlled() && GetLocalRole()==ROLE_AutonomousProxy)
	{
		FControlState St;
		St.Seq       = ++LocalInputSeq;
		St.DeltaTime = DeltaSeconds;
		St.ThrustF   = AxisF_Cur;
		St.ThrustR   = AxisR_Cur;
		St.ThrustU   = AxisU_Cur;
		St.Roll      = AxisRoll_Cur;
		St.MouseX    = MouseX_Accum;
		St.MouseY    = MouseY_Accum;

		Server_SendInput(St);

		// только наши локальные аккумуляторы
		MouseX_Accum = 0.f;
		MouseY_Accum = 0.f;
	}

	// 3) наблюдатели: интерполяция
	if (GetLocalRole()==ROLE_SimulatedProxy)
	{
		DriveSimulatedProxy(DeltaSeconds);
	}

	// 4) владелец: непрерывная мягкая реконсиляция
	if (IsLocallyControlled() && GetLocalRole()==ROLE_AutonomousProxy)
	{
		OwnerReconcile_Tick(DeltaSeconds);
	}

	// 5) сервер: обновим авторитативный снап (уйдёт по репликации)
	if (HasAuthority())
	{
		const FTransform X = ShipMesh->GetComponentTransform();
		ServerState.Loc = X.GetLocation();
		ServerState.Rot = X.GetRotation();
		ServerState.Vel = ShipMesh->GetComponentVelocity();
		ServerState.ServerTime = GetWorld()->GetTimeSeconds();
	}
}

void AShipPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	check(PlayerInputComponent);

	PlayerInputComponent->BindAxis(TEXT("ThrustForward"), this, &AShipPawn::Axis_ThrustForward);
	PlayerInputComponent->BindAxis(TEXT("StrafeRight"),   this, &AShipPawn::Axis_StrafeRight);
	PlayerInputComponent->BindAxis(TEXT("ThrustUp"),      this, &AShipPawn::Axis_ThrustUp);
	PlayerInputComponent->BindAxis(TEXT("Roll"),          this, &AShipPawn::Axis_Roll);
	PlayerInputComponent->BindAxis(TEXT("Turn"),          this, &AShipPawn::Axis_MouseYaw);
	PlayerInputComponent->BindAxis(TEXT("LookUp"),        this, &AShipPawn::Axis_MousePitch);
	PlayerInputComponent->BindAction(TEXT("ToggleFlightAssist"), IE_Pressed, this, &AShipPawn::Action_ToggleFA);
}

// ---- Camera ----
void AShipPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	if (!bUseCalcCamera || !ShipMesh)
	{
		if (Camera) { Camera->GetCameraView(DeltaTime, OutResult); return; }
		Super::CalcCamera(DeltaTime, OutResult);
		return;
	}

	const double Now = FApp::GetCurrentTime();
	const double DelaySec = FMath::Clamp((double)CameraDelayFrames, 0.0, 2.0) * (1.0/60.0);
	const double Tq = Now - DelaySec;

	// задержанный трансформ корабля
	FCamSample ShipT;
	if (!SampleAtTime(Tq, ShipT))
	{
		const FTransform X = ShipMesh->GetComponentTransform();
		ShipT.Time = Now; ShipT.Loc = X.GetLocation(); ShipT.Rot = X.GetRotation();
	}

	// pivot = delayed Ship ∘ SpringArm(Relative)
	FTransform ShipTM(ShipT.Rot, ShipT.Loc);
	FTransform TargetPivot = ShipTM;
	if (SpringArm)
	{
		const FTransform ArmRel = SpringArm->GetRelativeTransform();
		TargetPivot = ArmRel * ShipTM; // ChildWorld = ChildRel * ParentWorld
	}

	// lag позиция/ротация
	if (!bPivotInit)
	{
		PivotLoc_Sm = TargetPivot.GetLocation();
		PivotRot_Sm = TargetPivot.GetRotation();
		bPivotInit  = true;
	}
	else
	{
		// pos
		if (PositionLagSpeed > 0.f && DeltaTime > KINDA_SMALL_NUMBER)
		{
			const float aL = 1.f - FMath::Exp(-PositionLagSpeed * DeltaTime);
			PivotLoc_Sm = FMath::Lerp(PivotLoc_Sm, TargetPivot.GetLocation(), aL);

			if (MaxPositionLagDistance > 0.f)
			{
				const FVector d = PivotLoc_Sm - TargetPivot.GetLocation();
				const float d2 = d.SizeSquared();
				const float m2 = FMath::Square(MaxPositionLagDistance);
				if (d2 > m2) PivotLoc_Sm = TargetPivot.GetLocation() + d * (MaxPositionLagDistance / FMath::Sqrt(d2));
			}
		}
		else
		{
			PivotLoc_Sm = TargetPivot.GetLocation();
		}

		// rot
		if (RotationLagSpeed > 0.f && DeltaTime > KINDA_SMALL_NUMBER)
		{
			const float aR = 1.f - FMath::Exp(-RotationLagSpeed * DeltaTime);
			PivotRot_Sm = FQuat::Slerp(PivotRot_Sm, TargetPivot.GetRotation(), aR);
		}
		else
		{
			PivotRot_Sm = TargetPivot.GetRotation();
		}
	}

	const FTransform PivotSm(PivotRot_Sm, PivotLoc_Sm);

	// камера
	const FVector CamLoc = PivotSm.TransformPosition(CameraLocalOffset);

	FRotator CamRot;
	if (bLookAtTarget)
	{
		const FVector LookPos = PivotSm.TransformPosition(LookAtLocalOffset);
		const FVector Fwd = (LookPos - CamLoc).GetSafeNormal();
		const FVector ShipUp = PivotSm.GetUnitAxis(EAxis::Z);
		const FVector UpBlend = (FVector::UpVector * (1.f - BankWithShipAlpha) + ShipUp * BankWithShipAlpha).GetSafeNormal();

		CamRot = FRotationMatrix::MakeFromXZ(Fwd, UpBlend).Rotator();
	}
	else
	{
		CamRot = PivotRot_Sm.Rotator();
	}

	// финальное сглаживание
	FVector OutLoc = CamLoc;
	FRotator OutRot = CamRot;
	if (FinalViewLerpAlpha > 0.f && bHaveLastView)
	{
		const float A = Clamp01(FinalViewLerpAlpha);
		OutLoc = FMath::Lerp(LastViewLoc, OutLoc, A);
		OutRot = FMath::RInterpTo(LastViewRot, OutRot, 1.f, A);
	}

	OutResult.Location = OutLoc;
	OutResult.Rotation = OutRot;
	OutResult.FOV      = CameraFOV;

	LastViewLoc = OutResult.Location;
	LastViewRot = OutResult.Rotation;
	bHaveLastView = true;
}

// ---- Camera sample buffer ----
void AShipPawn::PushCamSample(const FCamSample& S)
{
	CamSamples.Add(S);

	const double Now = FApp::GetCurrentTime();
	const double KeepFrom = Now - (double)CameraBufferSeconds;

	int32 FirstValid = 0;
	while (FirstValid < CamSamples.Num() && CamSamples[FirstValid].Time < KeepFrom)
		++FirstValid;

	if (FirstValid > 0)
	{
		CamSamples.RemoveAt(0, FirstValid, EAllowShrinking::No);
	}

	if (CamSamples.Num() > MaxCameraSamples)
	{
		CamSamples.RemoveAt(0, CamSamples.Num() - MaxCameraSamples, EAllowShrinking::No);
	}
}

bool AShipPawn::SampleAtTime(double T, FCamSample& Out) const
{
	const int32 N = CamSamples.Num();
	if (N == 0) return false;
	if (N == 1) { Out = CamSamples[0]; return true; }

	if (T <= CamSamples[0].Time)   { Out = CamSamples[0];   return true; }
	if (T >= CamSamples[N-1].Time) { Out = CamSamples[N-1]; return true; }

	int32 L = 0, R = N - 1;
	while (L + 1 < R)
	{
		const int32 M = (L + R) / 2;
		if (CamSamples[M].Time <= T) L = M; else R = M;
	}
	const FCamSample& A = CamSamples[L];
	const FCamSample& B = CamSamples[L+1];
	const double dt = (B.Time - A.Time);
	if (dt <= SMALL_NUMBER) { Out = B; return true; }

	const float s = (float)FMath::Clamp((T - A.Time) / dt, 0.0, 1.0);
	Out.Time = T;
	Out.Loc  = FMath::Lerp(A.Loc, B.Loc, s);
	Out.Rot  = FQuat::Slerp(A.Rot, B.Rot, s);
	Out.Vel  = FMath::Lerp(A.Vel, B.Vel, s);
	return true;
}

// ---- Input passthrough (+ локальная копия значений) ----
void AShipPawn::Axis_ThrustForward(float V) { AxisF_Cur   = V; if (Flight) Flight->SetThrustForward(V); }
void AShipPawn::Axis_StrafeRight (float V)  { AxisR_Cur   = V; if (Flight) Flight->SetStrafeRight (V); }
void AShipPawn::Axis_ThrustUp    (float V)  { AxisU_Cur   = V; if (Flight) Flight->SetThrustUp    (V); }
void AShipPawn::Axis_Roll        (float V)  { AxisRoll_Cur= V; if (Flight) Flight->SetRollAxis    (V); }
void AShipPawn::Axis_MouseYaw    (float V)  { MouseX_Accum+= V; if (Flight) Flight->AddMouseYaw   (V); }
void AShipPawn::Axis_MousePitch  (float V)  { MouseY_Accum+= V; if (Flight) Flight->AddMousePitch (V); }
void AShipPawn::Action_ToggleFA()           { if (Flight) Flight->ToggleFlightAssist(); }

// ---- RPC ----
void AShipPawn::Server_SendInput_Implementation(const FControlState& State)
{
	ServerState.LastAckSeq = State.Seq;

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

void AShipPawn::OnRep_ServerState()
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

	if (GetLocalRole()==ROLE_SimulatedProxy)
	{
		// положим снап в буфер интерполятора (в клиентском времени)
		FInterpNode N;
		N.Time = (double)ServerState.ServerTime + ServerTimeToClientTime;
		N.Loc  = ServerState.Loc;
		N.Rot  = ServerState.Rot;
		N.Vel  = ServerState.Vel;
		NetBuffer.Add(N);

		// подрезать старые
		const double KeepFrom = Now - 1.0;
		int32 FirstValid = 0;
		while (FirstValid < NetBuffer.Num() && NetBuffer[FirstValid].Time < KeepFrom) ++FirstValid;
		if (FirstValid>0) NetBuffer.RemoveAt(0, FirstValid, EAllowShrinking::No);
	}
	else if (IsLocallyControlled() && GetLocalRole()==ROLE_AutonomousProxy)
	{
		// сохраняем целевой снап для непрерывной коррекции каждый тик
		OwnerReconTarget = ServerState;
		bHaveOwnerRecon  = true;
	}
}

// ---- Continuous owner reconciliation ----
void AShipPawn::OwnerReconcile_Tick(float DeltaSeconds)
{
	if (!bHaveOwnerRecon || !ShipMesh || DeltaSeconds <= 0.f) return;

	// предсказываем авторитативную позу к "сейчас"
	const double Now     = GetWorld()->GetTimeSeconds();
	const double Tserver = (double)OwnerReconTarget.ServerTime + ServerTimeToClientTime;
	const double Ahead   = FMath::Max(0.0, Now - Tserver);

	const FVector LocS = OwnerReconTarget.Loc + OwnerReconTarget.Vel * Ahead; // см
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

	// мягкая коррекция скорости (критически демпфированная пружина с постоянной Tau)
	const float Tau   = FMath::Max(0.02f, OwnerReconTau);
	const float Alpha = 1.f - FMath::Exp(-DeltaSeconds / Tau);

	const FVector VelC = ShipMesh->GetComponentVelocity(); // см/с
	const FVector Vtarget = VelS + (LocS - LocC) / Tau;
	FVector Vnew = FMath::Lerp(VelC, Vtarget, Alpha);

	// лимитируем «пинок» за тик, чтобы не было ступеньки
	const FVector Nudge = Vnew - VelC;
	const float MaxNudge = OwnerMaxVelNudge * DeltaSeconds;
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

// ---- SimulatedProxy interpolation (Hermite по Loc/Vel) ----
void AShipPawn::DriveSimulatedProxy(float /*DeltaSeconds*/)
{
	if (!ShipMesh || NetBuffer.Num()==0 || !bHaveTimeSync) return;

	const double Now = GetWorld()->GetTimeSeconds();
	const double Tq  = Now - (double)NetInterpDelay;

	if (NetBuffer.Num()==1 || Tq <= NetBuffer[0].Time)
	{
		const auto& N = NetBuffer[0];
		SetActorLocationAndRotation(N.Loc, N.Rot.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}
	if (Tq >= NetBuffer.Last().Time)
	{
		const auto& N = NetBuffer.Last();
		SetActorLocationAndRotation(N.Loc, N.Rot.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
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

	// кубический Hermite: позиции + мгновенные скорости
	const float s2=s*s, s3=s2*s;
	const float h00 =  2*s3 - 3*s2 + 1;
	const float h10 =    s3 - 2*s2 + s;
	const float h01 = -2*s3 + 3*s2;
	const float h11 =    s3 -   s2;

	const FVector P =  h00*A.Loc + h10*(A.Vel*(float)dt)
	                 + h01*B.Loc + h11*(B.Vel*(float)dt);

	const FQuat   Q = FQuat::Slerp(A.Rot, B.Rot, s);

	SetActorLocationAndRotation(P, Q.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
}

// ---- Replication ----
void AShipPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AShipPawn, ServerState);
}
