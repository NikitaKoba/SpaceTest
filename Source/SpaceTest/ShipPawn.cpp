// ShipPawn.cpp
#include "ShipPawn.h"
#include "FlightComponent.h"

#include "Components/StaticMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/App.h"

AShipPawn::AShipPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup    = TG_PostPhysics;

	// Root mesh
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

	// SpringArm — только как якорь оффсетов (без встроенных лагов)
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
}

void AShipPawn::BeginPlay()
{
	Super::BeginPlay();

	if (ShipMesh && !ShipMesh->IsSimulatingPhysics())
	{
		ShipMesh->SetSimulatePhysics(true);
		ShipMesh->SetEnableGravity(false);
	}

	// стартовый сэмпл
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

	// свежий сэмпл после физики
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

	const double Now = FApp::GetCurrentTime();
	const double DelaySec = FMath::Clamp((double)CameraDelayFrames, 0.0, 2.0) * (1.0/60.0);
	const double Tq = Now - DelaySec;

	// 1) берём задержанный трансформ корабля
	FCamSample ShipT;
	if (!SampleAtTime(Tq, ShipT))
	{
		const FTransform X = ShipMesh->GetComponentTransform();
		ShipT.Time = Now; ShipT.Loc = X.GetLocation(); ShipT.Rot = X.GetRotation();
	}

	// 2) считаем целевой pivot = delayed Ship ∘ SpringArm(Relative)
	FTransform ShipTM(ShipT.Rot, ShipT.Loc);
	FTransform TargetPivot = ShipTM;
	if (SpringArm)
	{
		const FTransform ArmRel = SpringArm->GetRelativeTransform();
		TargetPivot = ArmRel * ShipTM; // ChildWorld = ChildRel * ParentWorld
	}

	// 3) применяем position/rotation lag к pivot
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

	// 4) считаем положение/поворот камеры
	const FVector CamLoc = PivotSm.TransformPosition(CameraLocalOffset);

	FRotator CamRot;
	if (bLookAtTarget)
	{
		const FVector LookPos = PivotSm.TransformPosition(LookAtLocalOffset);
		const FVector Fwd = (LookPos - CamLoc).GetSafeNormal();

		// up-вектор: смесь мирового Up и Up корабля -> даёт «банковать» вместе с кораблём
		const FVector ShipUp = PivotSm.GetUnitAxis(EAxis::Z);
		const FVector UpBlend = (FVector::UpVector * (1.f - BankWithShipAlpha) + ShipUp * BankWithShipAlpha).GetSafeNormal();

		const FMatrix M = FRotationMatrix::MakeFromXZ(Fwd, UpBlend);
		CamRot = M.Rotator();
	}
	else
	{
		CamRot = PivotRot_Sm.Rotator();
	}

	// 5) лёгкое финальное сглаживание (по желанию)
	FVector OutLoc = CamLoc;
	FRotator OutRot = CamRot;
	if (FinalViewLerpAlpha > 0.f && bHaveLastView)
	{
		const float A = FMath::Clamp(FinalViewLerpAlpha, 0.f, 1.f);
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

// === sample buffer ===
void AShipPawn::PushCamSample(const FCamSample& S)
{
	CamSamples.Add(S);

	const double Now = FApp::GetCurrentTime();
	const double KeepFrom = Now - (double)CameraBufferSeconds;

	int32 FirstValid = 0;
	while (FirstValid < CamSamples.Num() && CamSamples[FirstValid].Time < KeepFrom)
		++FirstValid;

	if (FirstValid > 0) CamSamples.RemoveAt(0, FirstValid, false);

	if (CamSamples.Num() > MaxCameraSamples)
		CamSamples.RemoveAt(0, CamSamples.Num() - MaxCameraSamples, false);
}

bool AShipPawn::SampleAtTime(double T, FCamSample& Out) const
{
	const int32 N = CamSamples.Num();
	if (N == 0) return false;
	if (N == 1) { Out = CamSamples[0]; return true; }

	if (T <= CamSamples[0].Time) { Out = CamSamples[0]; return true; }
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

// === input passthrough ===
void AShipPawn::Axis_ThrustForward(float V) { if (Flight) Flight->SetThrustForward(V); }
void AShipPawn::Axis_StrafeRight (float V)  { if (Flight) Flight->SetStrafeRight (V); }
void AShipPawn::Axis_ThrustUp    (float V)  { if (Flight) Flight->SetThrustUp    (V); }
void AShipPawn::Axis_Roll        (float V)  { if (Flight) Flight->SetRollAxis    (V); }
void AShipPawn::Axis_MouseYaw    (float V)  { if (Flight) Flight->AddMouseYaw   (V); }
void AShipPawn::Axis_MousePitch  (float V)  { if (Flight) Flight->AddMousePitch (V); }
void AShipPawn::Action_ToggleFA()           { if (Flight) Flight->ToggleFlightAssist(); }
