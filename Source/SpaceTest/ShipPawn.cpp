// ShipPawn.cpp
#include "ShipPawn.h"
#include "FlightComponent.h"
#include "ShipNetComponent.h"
#include "ShipCursorPilotComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Misc/App.h"

static FORCEINLINE float Clamp01(float v){ return FMath::Clamp(v, 0.f, 1.f); }

// ShipPawn.cpp
AShipPawn::AShipPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup    = TG_PostPhysics;

	bReplicates = true;
	SetReplicateMovement(false);
	bAlwaysRelevant = false;
	bOnlyRelevantToOwner = false;
	SetNetUpdateFrequency(120.f);
	SetMinNetUpdateFrequency(60.f);
	CursorPilot = CreateDefaultSubobject<UShipCursorPilotComponent>(TEXT("CursorPilot"));
	// Root mesh (physics)
	ShipMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ShipMesh"));
	SetRootComponent(ShipMesh);
	ShipMesh->SetSimulatePhysics(false);              // <--- ВАЖНО: изначально false
	ShipMesh->SetEnableGravity(false);
	ShipMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	ShipMesh->SetCollisionObjectType(ECC_Pawn);
	ShipMesh->BodyInstance.bUseCCD = true;
	ShipMesh->SetLinearDamping(0.02f);
	ShipMesh->SetAngularDamping(0.02f);

	Flight = CreateDefaultSubobject<UFlightComponent>(TEXT("Flight"));
	Net    = CreateDefaultSubobject<UShipNetComponent>(TEXT("Net"));

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
	Laser = CreateDefaultSubobject<UShipLaserComponent>(TEXT("Laser")); // <—
	// Базовые дефолты. Можно править в Details/Blueprint:
	Laser->bServerTraceAim     = true;     // пусть сервер всё равно трейсит точку (для попаданий)
	Laser->MuzzleSockets    = { FName("Muzzle_L"), FName("Muzzle_R") }; // должны быть на ShipMesh
	SpawnCollisionHandlingMethod = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
}


void AShipPawn::BeginPlay()
{
	Super::BeginPlay();

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

void AShipPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// свежий сэмпл для камеры
	FCamSample S;
	S.Time = FApp::GetCurrentTime();
	const FTransform X = ShipMesh->GetComponentTransform();
	S.Loc = X.GetLocation();
	S.Rot = X.GetRotation();
	S.Vel = ShipMesh->GetComponentVelocity();
	PushCamSample(S);
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
	PlayerInputComponent->BindAction(TEXT("FirePrimary"), IE_Pressed,  this, &AShipPawn::Action_FirePressed);
	PlayerInputComponent->BindAction(TEXT("FirePrimary"), IE_Released, this, &AShipPawn::Action_FireReleased);
}
// --- Combat input ---
void AShipPawn::Action_FirePressed()
{
	if (Laser) Laser->StartFire();   // компонент сам дернёт сервер и начнёт таймер спавна
}
void AShipPawn::Action_FireReleased()
{
	if (Laser) Laser->StopFire();
}
// === Camera ===
void AShipPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	if (!bUseCalcCamera || !ShipMesh)
	{
		if (Camera) { Camera->GetCameraView(DeltaTime, OutResult); return; }
		Super::CalcCamera(DeltaTime, OutResult);
		return;
	}

	const double Now     = FApp::GetCurrentTime();
	const double DelaySec= FMath::Clamp((double)CameraDelayFrames, 0.0, 2.0) * (1.0/60.0);
	const double Tq      = Now - DelaySec;

	// 1) задержанный трансформ корабля
	FCamSample ShipT;
	if (!SampleAtTime(Tq, ShipT))
	{
		const FTransform X = ShipMesh->GetComponentTransform();
		ShipT.Time = Now; ShipT.Loc = X.GetLocation(); ShipT.Rot = X.GetRotation();
	}

	// 2) pivot = delayed Ship ∘ SpringArm(Relative)
	FTransform ShipTM(ShipT.Rot, ShipT.Loc);
	FTransform TargetPivot = ShipTM;
	if (SpringArm)
	{
		const FTransform ArmRel = SpringArm->GetRelativeTransform();
		TargetPivot = ArmRel * ShipTM;
	}

	// 3) lag
	if (!bPivotInit)
	{
		PivotLoc_Sm = TargetPivot.GetLocation();
		PivotRot_Sm = TargetPivot.GetRotation();
		bPivotInit  = true;
	}
	else
	{
		// position
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

		// rotation
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

	// 4) камера
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

	// 5) финальное сглаживание
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

// ==== Camera samples ====
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

// ==== Input passthrough (и зеркалим в Net для RPC) ====
void AShipPawn::Axis_ThrustForward(float V)
{
	if (Flight) Flight->SetThrustForward(V);
	if (Net)    Net->SetLocalAxes(V,
		Flight ? Flight->ThrustRight_Target : 0.f,
		Flight ? Flight->ThrustUp_Target    : 0.f,
		Flight ? Flight->RollAxis_Target    : 0.f);
}
void AShipPawn::Axis_StrafeRight(float V)
{
	if (Flight) Flight->SetStrafeRight(V);
	if (Net)    Net->SetLocalAxes(
		Flight ? Flight->ThrustForward_Target : 0.f,
		V,
		Flight ? Flight->ThrustUp_Target : 0.f,
		Flight ? Flight->RollAxis_Target : 0.f);
}
void AShipPawn::Axis_ThrustUp(float V)
{
	if (Flight) Flight->SetThrustUp(V);
	if (Net)    Net->SetLocalAxes(
		Flight ? Flight->ThrustForward_Target : 0.f,
		Flight ? Flight->ThrustRight_Target   : 0.f,
		V,
		Flight ? Flight->RollAxis_Target      : 0.f);
}
void AShipPawn::Axis_Roll(float V)
{
	if (Flight) Flight->SetRollAxis(V);
	if (Net)    Net->SetLocalAxes(
		Flight ? Flight->ThrustForward_Target : 0.f,
		Flight ? Flight->ThrustRight_Target   : 0.f,
		Flight ? Flight->ThrustUp_Target      : 0.f,
		V);
}
void AShipPawn::Axis_MouseYaw(float V)
{
	// Если курсорный пилот активен — yaw/pitch уже подаются из компонента
	const bool bCursorActive = (CursorPilot && CursorPilot->IsActive());
	if (!bCursorActive)
	{
		if (Flight) Flight->AddMouseYaw(V);
		if (Net)    Net->AddMouseDelta(V, 0.f);
	}
}
void AShipPawn::Axis_MousePitch(float V)
{
	const bool bCursorActive = (CursorPilot && CursorPilot->IsActive());
	if (!bCursorActive)
	{
		if (Flight) Flight->AddMousePitch(V);
		if (Net)    Net->AddMouseDelta(0.f, V);
	}
}
void AShipPawn::Action_ToggleFA()
{
	if (Flight) Flight->ToggleFlightAssist();
}
