// ShipPawn.cpp
#include "ShipPawn.h"
#include "FlightComponent.h"
#include "ShipNetComponent.h"
#include "ShipCursorPilotComponent.h"
#include "SpacePlayerState.h"
#include "SpaceFloatingOriginSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/GameStateBase.h"
#include "Camera/CameraComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
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
	ShipMesh->SetSimulatePhysics(false);              // <--- Ð’ÐÐ–ÐÐž: Ð¸Ð·Ð½Ð°Ñ‡Ð°Ð»ÑŒÐ½Ð¾ false
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
	Laser = CreateDefaultSubobject<UShipLaserComponent>(TEXT("Laser")); // <â€”
	// Ð‘Ð°Ð·Ð¾Ð²Ñ‹Ðµ Ð´ÐµÑ„Ð¾Ð»Ñ‚Ñ‹. ÐœÐ¾Ð¶Ð½Ð¾ Ð¿Ñ€Ð°Ð²Ð¸Ñ‚ÑŒ Ð² Details/Blueprint:
	Laser->bServerTraceAim     = true;     // Ð¿ÑƒÑÑ‚ÑŒ ÑÐµÑ€Ð²ÐµÑ€ Ð²ÑÑ‘ Ñ€Ð°Ð²Ð½Ð¾ Ñ‚Ñ€ÐµÐ¹ÑÐ¸Ñ‚ Ñ‚Ð¾Ñ‡ÐºÑƒ (Ð´Ð»Ñ Ð¿Ð¾Ð¿Ð°Ð´Ð°Ð½Ð¸Ð¹)
	Laser->MuzzleSockets    = { FName("Muzzle_L"), FName("Muzzle_R") }; // Ð´Ð¾Ð»Ð¶Ð½Ñ‹ Ð±Ñ‹Ñ‚ÑŒ Ð½Ð° ShipMesh
	SpawnCollisionHandlingMethod = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	// --- Cruise / Hyper flight profiles ---
	CruiseLongi.VxMax_Mps  = 120.f;
	CruiseLongi.AxMax_Mps2 = 15.f;
	CruiseFA.VelocityPD.Kp = 1.5f;
	CruiseFA.VelocityPD.Kd = 0.25f;
	CruiseFA.VelocityPD.OutputAbsMax = 50.f;
	CruiseFA.AccelMax_Mps2 = 60.f;
	CruiseFA.RetroBoostMultiplier = 2.f;
	CruiseFA.RetroDotThreshold   = -0.25f;
	CruiseFA.Deadzone_Mps        = 0.05f;

	HyperLongi.VxMax_Mps  = 128200.f;
	HyperLongi.AxMax_Mps2 = 28200.f;
	HyperFA.VelocityPD.Kp = 1.5f;
	HyperFA.VelocityPD.Kd = 0.25f;
	HyperFA.VelocityPD.OutputAbsMax = 128200.f;
	HyperFA.AccelMax_Mps2 = 128200.f;
	HyperFA.RetroBoostMultiplier = 2.f;
	HyperFA.RetroDotThreshold   = -0.25f;
	HyperFA.Deadzone_Mps        = 0.05f;

}

// ShipPawn.cpp
// ShipPawn.cpp
void AShipPawn::BeginPlay()
{
	Super::BeginPlay();

	// --- 1. ÐÐ° ÑÐµÑ€Ð²ÐµÑ€Ðµ ÐžÐ”Ð˜Ð Ð ÐÐ— ÑÑ‡Ð¸Ñ‚Ð°ÐµÐ¼ GlobalPos Ð¸Ð· Ñ‚ÐµÐºÑƒÑ‰Ð¸Ñ… world-ÐºÐ¾Ð¾Ñ€Ð´Ð¸Ð½Ð°Ñ‚ ---
	if (HasAuthority())
	{
		// Ð’ÐÐ–ÐÐž: Ð¢Ð¾Ð»ÑŒÐºÐ¾ ÐžÐ”Ð˜Ð Ð ÐÐ— Ð¿Ñ€Ð¸ ÑÐ¿Ð°Ð²Ð½Ðµ!
		// ÐŸÐ¾ÑÐ»Ðµ ÑÑ‚Ð¾Ð³Ð¾ GlobalPos Ð¾Ð±Ð½Ð¾Ð²Ð»ÑÐµÑ‚ÑÑ Ð˜ÐÐšÐ Ð•ÐœÐ•ÐÐ¢ÐÐž Ð² Tick()
		SyncGlobalFromWorld();
		
		UE_LOG(LogTemp, Warning,
			TEXT("[SHIP SPAWN] %s | WorldLoc=%s | GlobalPos=%s"),
			*GetName(),
			*GetActorLocation().ToString(),
			*GlobalPos.ToString());
	}

	// --- 2. Floating Origin anchor ---
	if (HasAuthority() && bEnableFloatingOrigin)
	{
		if (UWorld* World = GetWorld())
		{
			if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
			{
				UE_LOG(LogTemp, Warning, TEXT("[ShipPawn] Enable FloatingOrigin, Anchor=%s"), *GetName());
				FO->SetEnabled(true);
				FO->SetAnchor(this);
			}
		}
	}

	// --- 3. ÐšÐ°Ð¼ÐµÑ€Ð° (ÐºÐ°Ðº Ñƒ Ñ‚ÐµÐ±Ñ Ð±Ñ‹Ð»Ð¾) ---
	FCamSample S;
	S.Time = FApp::GetCurrentTime();
	const FTransform X = ShipMesh->GetComponentTransform();
	S.Loc = X.GetLocation();
	S.Rot = X.GetRotation();
	S.Vel = ShipMesh->GetComponentVelocity();
	PushCamSample(S);

	// Clamp residual linear speed in cruise mode to avoid reusing hyper-scale velocity when W is held after exit.
	if (!bHyperDriveActive && ShipMesh && ShipMesh->IsSimulatingPhysics())
	{
		const float MaxMps  = (Flight ? Flight->Longi.VxMax_Mps : CruiseLongi.VxMax_Mps);
		const float MaxUUps = FMath::Max(100.f, MaxMps * 100.f);
		const FVector Vel   = ShipMesh->GetPhysicsLinearVelocity();
		if (Vel.SizeSquared() > FMath::Square(MaxUUps))
		{
			ShipMesh->SetPhysicsLinearVelocity(Vel.GetClampedToMaxSize(MaxUUps), false);
		}
	}

	if (Camera)
	{
		Camera->SetFieldOfView(CameraFOV);
	}

	// Apply default cruise profile on start
	ApplyFlightProfile(false);
	bHyperDrivePrev = bHyperDriveActive;
	RequestCameraResync();
	CameraResyncFrames = 3;
}

void AShipPawn::Destroyed()
{
	UE_LOG(LogTemp, Warning,
		TEXT("[ShipPawn] Destroyed: %s Loc=%s"),
		*GetName(),
		*GetActorLocation().ToString());

	Super::Destroyed();
}

void AShipPawn::OutsideWorldBounds()
{
	UE_LOG(LogTemp, Warning,
		TEXT("[ShipPawn] OutsideWorldBounds: %s Loc=%s"),
		*GetName(),
		*GetActorLocation().ToString());

	// ВАЖНО: не зовём Super::OutsideWorldBounds(), чтобы движок не делал Destroy() автоматически.
	// Super::OutsideWorldBounds();
}

void AShipPawn::FellOutOfWorld(const UDamageType& DamageType)
{
	UE_LOG(LogTemp, Warning,
		TEXT("[ShipPawn] FellOutOfWorld: %s Loc=%s"),
		*GetName(),
		*GetActorLocation().ToString());

	// Точно так же блокируем автокилл при падении из мира.
	// Super::FellOutOfWorld(DamageType);
}


void AShipPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// --- Камера: без изменений ---
	FCamSample S;
	S.Time = FApp::GetCurrentTime();
	const FTransform X = ShipMesh->GetComponentTransform();
	S.Loc = X.GetLocation();
	S.Rot = X.GetRotation();
	S.Vel = ShipMesh->GetComponentVelocity();
	PushCamSample(S);

	

	// After hyper exit, kill residual velocity and ignore thrust for a short cooldown.
	if (HyperExitThrottleLockTime > 0.f)
	{
		HyperExitThrottleLockTime = FMath::Max(0.f, HyperExitThrottleLockTime - DeltaSeconds);

		if (ShipMesh)
		{
			ShipMesh->SetPhysicsLinearVelocity(FVector::ZeroVector, false);
			ShipMesh->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector, false);

			if (FBodyInstance* BI = ShipMesh->GetBodyInstance())
			{
				BI->ClearForces();
				BI->ClearTorques();
			}
		}

		if (Flight)
		{
			Flight->ResetDynamicsState();
			Flight->SetThrustForward(0.f);
			Flight->SetStrafeRight(0.f);
			Flight->SetThrustUp(0.f);
			Flight->SetRollAxis(0.f);
		}

		if (Net)
		{
			Net->SetLocalAxes(0.f, 0.f, 0.f, 0.f);
		}
	}

	// Clamp residual linear speed in cruise mode to avoid reusing hyper-scale velocity when W is held after exit.
	if (!bHyperDriveActive && ShipMesh)
	{
		const float MaxMps  = (Flight ? Flight->Longi.VxMax_Mps : CruiseLongi.VxMax_Mps);
		const float MaxUUps = FMath::Max(100.f, MaxMps * 100.f);
		const FVector Vel   = ShipMesh->GetPhysicsLinearVelocity();
		if (Vel.SizeSquared() > FMath::Square(MaxUUps))
		{
			ShipMesh->SetPhysicsLinearVelocity(Vel.GetClampedToMaxSize(MaxUUps), false);
		}
	}
// === ГЛОБАЛЬНЫЕ КООРДИНАТЫ ===
	//
	// ВАЖНО:
	//  - Никакого инкрементального "прибавления дельт".
	//  - Каждый тик на сервере просто пересчитываем GlobalPos из world-позиции.
	//  - FloatingOrigin уже гарантирует, что World+Origin -> Global стабильны,
	//    даже после ShiftOrigin().
	if (HasAuthority())
	{
		SyncGlobalFromWorld();

		if (ASpacePlayerState* SPS = Cast<ASpacePlayerState>(GetPlayerState()))
		{
			SPS->SetReplicatedGlobalPos(GlobalPos);
		}
	}

	// --- Debug marker to show where the other player is ---
	if (IsLocallyControlled() && bDrawOtherPlayerMarker)
	{
		UWorld* World = GetWorld();
		if (World && World->GetNetMode() != NM_DedicatedServer)
		{
			ASpacePlayerState* TargetPS = nullptr;
			ASpacePlayerState* MyPS = Cast<ASpacePlayerState>(GetPlayerState());

			if (AGameStateBase* GS = World->GetGameState())
			{
				for (APlayerState* PS : GS->PlayerArray)
				{
					if (PS && PS != MyPS)
					{
						TargetPS = Cast<ASpacePlayerState>(PS);
						if (TargetPS)
						{
							break;
						}
					}
				}
			}

			if (TargetPS)
			{
				const FGlobalPos& TargetGP = TargetPS->GetReplicatedGlobalPos();

				FVector TargetWorld = FVector::ZeroVector;
				if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
				{
					TargetWorld = FO->GlobalToWorld(TargetGP);
				}
				else
				{
					const FVector3d G = SpaceGlobal::ToGlobalVector(TargetGP);
					TargetWorld = FVector(G);
				}

				const FVector MyLoc = GetActorLocation();
				const FVector Dir   = TargetWorld - MyLoc;
				const double  Dist  = Dir.Length();
				const FVector DirN  = Dir.IsNearlyZero() ? FVector::ForwardVector : Dir.GetSafeNormal();

				const float MaxArrowLen = 20000.f; // clamp so it stays visible even for huge distances
				const float ArrowLen    = (Dist > MaxArrowLen) ? MaxArrowLen : (float)Dist;
				const FVector MarkerLoc = MyLoc + DirN * ArrowLen;

				const FColor Color = FColor::Cyan;
				DrawDebugSphere(World, MarkerLoc, OtherPlayerMarkerRadius, 16, Color, false, 0.f, 0, 2.f);
				DrawDebugDirectionalArrow(World, MyLoc, MarkerLoc, 400.f, Color, false, 0.f, 0, 2.f);

				// show distance in km (1 UU = 1 cm)
				const float DistKm = (float)(Dist / 100000.0); // cm -> km
				DrawDebugString(World, MarkerLoc + FVector(0.f, 0.f, OtherPlayerMarkerRadius + 150.f),
					FString::Printf(TEXT("%.1f km"), DistKm), nullptr, Color, 0.f, true);
			}
		}
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
	PlayerInputComponent->BindAction(TEXT("ToggleHyperDrive"),   IE_Pressed, this, &AShipPawn::Action_ToggleHyperDrive);
	PlayerInputComponent->BindAction(TEXT("FirePrimary"), IE_Pressed,  this, &AShipPawn::Action_FirePressed);
	PlayerInputComponent->BindAction(TEXT("FirePrimary"), IE_Released, this, &AShipPawn::Action_FireReleased);

}	
// --- Combat input ---
void AShipPawn::Action_FirePressed()
{
	if (Laser) Laser->StartFire();   // ÐºÐ¾Ð¼Ð¿Ð¾Ð½ÐµÐ½Ñ‚ ÑÐ°Ð¼ Ð´ÐµÑ€Ð½Ñ‘Ñ‚ ÑÐµÑ€Ð²ÐµÑ€ Ð¸ Ð½Ð°Ñ‡Ð½Ñ‘Ñ‚ Ñ‚Ð°Ð¹Ð¼ÐµÑ€ ÑÐ¿Ð°Ð²Ð½Ð°
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

	// Absolute snap window after hyper exit: no history, no lag, lock to current transform.
	if (!bCameraUseHyperProfileAlways && HyperExitHardSnapTime > 0.f)
	{
		// Tick down snap timer even while short-circuiting the rest of the camera logic.
		HyperExitHardSnapTime = FMath::Max(0.f, HyperExitHardSnapTime - DeltaTime);

		const FTransform X = ShipMesh->GetComponentTransform();
		const FVector CamLoc = X.TransformPosition(CameraLocalOffset);
		const FVector LookPos = X.TransformPosition(LookAtLocalOffset);
		const FVector Fwd = (LookPos - CamLoc).GetSafeNormal();
		const FVector Up = X.GetUnitAxis(EAxis::Z);

		OutResult.Location = CamLoc;
		OutResult.Rotation = FRotationMatrix::MakeFromXZ(Fwd, Up).Rotator();
		OutResult.FOV      = CameraFOV;

		// Keep state aligned so next frame continues smoothly.
		PivotLoc_Sm = X.GetLocation();
		PivotRot_Sm = X.GetRotation();
		bPivotInit = true;
		LastViewLoc = OutResult.Location;
		LastViewRot = OutResult.Rotation;
		bHaveLastView = true;
		return;
	}

const double Now = FApp::GetCurrentTime();
double DelayFrames = CameraDelayFrames;
const bool bExitBlendActive = (!bCameraUseHyperProfileAlways && !bHyperDriveActive && HyperExitBlendTime > 0.f);
const bool bExitSnapActive  = (!bCameraUseHyperProfileAlways && !bHyperDriveActive && HyperExitSnapCounter > 0);
if (bHyperDriveActive || bExitBlendActive || bExitSnapActive)
{
		// В гипере не используем исторический лаг по кадрам
		DelayFrames = 0.0;
	}
	const double DelaySec= FMath::Clamp(DelayFrames, 0.0, 2.0) * (1.0/60.0);
	const double Tq      = Now - DelaySec;

	// 1) Ð·Ð°Ð´ÐµÑ€Ð¶Ð°Ð½Ð½Ñ‹Ð¹ Ñ‚Ñ€Ð°Ð½ÑÑ„Ð¾Ñ€Ð¼ ÐºÐ¾Ñ€Ð°Ð±Ð»Ñ
	FCamSample ShipT;
	const bool bUseHistory = (!bHyperDriveActive && !bExitBlendActive && !bExitSnapActive && CameraResyncFrames == 0);
	if (bUseHistory && SampleAtTime(Tq, ShipT))
	{
		// ok
	}
	else
	{
		const FTransform X = ShipMesh->GetComponentTransform();
		ShipT.Time = Now; ShipT.Loc = X.GetLocation(); ShipT.Rot = X.GetRotation();
	}

	// 2) pivot = delayed Ship âˆ˜ SpringArm(Relative)
	FTransform ShipTM(ShipT.Rot, ShipT.Loc);
	FTransform TargetPivot = ShipTM;
	if (SpringArm)
	{
		const FTransform ArmRel = SpringArm->GetRelativeTransform();
		TargetPivot = ArmRel * ShipTM;
	}

	// Pending resync when switching hyper <-> normal: align camera state to target to avoid pops.
	if (bCameraResyncPending)
	{
		ResetCameraBufferImmediate();
		PivotLoc_Sm = TargetPivot.GetLocation();
		PivotRot_Sm = TargetPivot.GetRotation();
		bPivotInit = true;
		bHaveLastView = false;
		bCameraResyncPending = false;
	}

	// 3) lag
	const bool bHyperCam = bHyperDriveActive || bExitBlendActive || bExitSnapActive;
	float ExitBlendAlpha = 1.f;
	if (bExitBlendActive && HyperExitBlendSeconds > KINDA_SMALL_NUMBER)
	{
		const float Ratio = FMath::Clamp(HyperExitBlendTime / HyperExitBlendSeconds, 0.f, 1.f);
		ExitBlendAlpha = 1.f - Ratio; // 0=hyper, 1=normal
	}
	const bool bExitGlue = (bExitSnapActive) || (bExitBlendActive && (ExitBlendAlpha <= HyperExitSnapThreshold));
	const bool bForceNoLag = (bHyperCam && HyperMaxPositionLagDistance <= KINDA_SMALL_NUMBER) || bExitGlue;
	const bool bFreezeResync = (CameraResyncFrames > 0);
	float PosLagSpeed    = PositionLagSpeed;
	float MaxPosLag      = MaxPositionLagDistance;
	float RotLagSpeed    = RotationLagSpeed;
	float ViewLerpAlpha  = FinalViewLerpAlpha;

	const bool bForceHyperProfile = bCameraUseHyperProfileAlways;

	if (bHyperDriveActive || bForceHyperProfile)
	{
		PosLagSpeed   = HyperPositionLagSpeed;
		MaxPosLag     = HyperMaxPositionLagDistance;
		RotLagSpeed   = HyperRotationLagSpeed;
		ViewLerpAlpha = HyperFinalViewLerpAlpha;
	}
	else if (bExitBlendActive || bExitSnapActive)
	{
		PosLagSpeed   = FMath::Lerp(HyperPositionLagSpeed, PositionLagSpeed, ExitBlendAlpha);
		MaxPosLag     = FMath::Lerp(HyperMaxPositionLagDistance, MaxPositionLagDistance, ExitBlendAlpha);
		RotLagSpeed   = FMath::Lerp(HyperRotationLagSpeed, RotationLagSpeed, ExitBlendAlpha);
		ViewLerpAlpha = FMath::Lerp(HyperFinalViewLerpAlpha, FinalViewLerpAlpha, ExitBlendAlpha);
	}
	if (bExitGlue)
	{
		PosLagSpeed = 0.f;
		MaxPosLag   = 0.f;
		RotLagSpeed = 0.f;
		ViewLerpAlpha = 0.f;
	}

	if (!bPivotInit)
	{
		PivotLoc_Sm = TargetPivot.GetLocation();
		PivotRot_Sm = TargetPivot.GetRotation();
		bPivotInit  = true;
	}
	else
	{
		if (bForceNoLag || bFreezeResync)
		{
			PivotLoc_Sm = TargetPivot.GetLocation();
			PivotRot_Sm = TargetPivot.GetRotation();
			ViewLerpAlpha = 0.f;
		}
		else
		{
			// position
			if (PosLagSpeed > 0.f && DeltaTime > KINDA_SMALL_NUMBER)
			{
				const float aL = 1.f - FMath::Exp(-PosLagSpeed * DeltaTime);
				PivotLoc_Sm = FMath::Lerp(PivotLoc_Sm, TargetPivot.GetLocation(), aL);

				if (MaxPosLag > 0.f)
				{
					const FVector d = PivotLoc_Sm - TargetPivot.GetLocation();
					const float d2 = d.SizeSquared();
					const float m2 = FMath::Square(MaxPosLag);
					if (d2 > m2) PivotLoc_Sm = TargetPivot.GetLocation() + d * (MaxPosLag / FMath::Sqrt(d2));
				}
			}
			else
			{
				PivotLoc_Sm = TargetPivot.GetLocation();
			}

			// rotation
			if (RotLagSpeed > 0.f && DeltaTime > KINDA_SMALL_NUMBER)
			{
				const float aR = 1.f - FMath::Exp(-RotLagSpeed * DeltaTime);
				PivotRot_Sm = FQuat::Slerp(PivotRot_Sm, TargetPivot.GetRotation(), aR);
			}
			else
			{
				PivotRot_Sm = TargetPivot.GetRotation();
			}
		}
	}

	const FTransform PivotSm(PivotRot_Sm, PivotLoc_Sm);

	// decrement freeze counter (keeps camera glued for a few frames after hyper switch)
	if (CameraResyncFrames > 0)
	{
		--CameraResyncFrames;
	}

	if (bExitBlendActive && !bHyperDriveActive)
	{
		HyperExitBlendTime = FMath::Max(0.f, HyperExitBlendTime - DeltaTime);
	}
	else if (bHyperDriveActive)
	{
		HyperExitBlendTime = 0.f;
	}

	if (bExitSnapActive && !bHyperDriveActive)
	{
		--HyperExitSnapCounter;
	}

	if (!bHyperDriveActive && HyperExitHardSnapTime > 0.f)
	{
		HyperExitHardSnapTime = FMath::Max(0.f, HyperExitHardSnapTime - DeltaTime);
	}
	else if (bHyperDriveActive)
	{
		HyperExitHardSnapTime = 0.f;
	}

	// 4) ÐºÐ°Ð¼ÐµÑ€Ð°
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

	// 5) Ñ„Ð¸Ð½Ð°Ð»ÑŒÐ½Ð¾Ðµ ÑÐ³Ð»Ð°Ð¶Ð¸Ð²Ð°Ð½Ð¸Ðµ
	FVector OutLoc = CamLoc;
	FRotator OutRot = CamRot;
	if (ViewLerpAlpha > 0.f && bHaveLastView)
	{
		const float A = Clamp01(ViewLerpAlpha);
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

// ==== Input passthrough (Ð¸ Ð·ÐµÑ€ÐºÐ°Ð»Ð¸Ð¼ Ð² Net Ð´Ð»Ñ RPC) ====
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
	// Ð•ÑÐ»Ð¸ ÐºÑƒÑ€ÑÐ¾Ñ€Ð½Ñ‹Ð¹ Ð¿Ð¸Ð»Ð¾Ñ‚ Ð°ÐºÑ‚Ð¸Ð²ÐµÐ½ â€” yaw/pitch ÑƒÐ¶Ðµ Ð¿Ð¾Ð´Ð°ÑŽÑ‚ÑÑ Ð¸Ð· ÐºÐ¾Ð¼Ð¿Ð¾Ð½ÐµÐ½Ñ‚Ð°
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

void AShipPawn::Action_ToggleHyperDrive()
{
	ToggleHyperDrive();
}
void AShipPawn::SetGlobalPos(const FGlobalPos& InPos)
{
	GlobalPos = InPos;

	if (UWorld* World = GetWorld())
	{
		if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
		{
			const FVector NewWorldLoc = FO->GlobalToWorld(GlobalPos);
			SetActorLocation(NewWorldLoc, false, nullptr, ETeleportType::TeleportPhysics);
		}
		else
		{
			// Ð¤Ð¾Ð»Ð»Ð±ÐµÐº: origin = (0,0,0), Ð¿Ñ€Ð¾ÑÑ‚Ð¾ Ñ€Ð°ÑÐºÐ»Ð°Ð´Ñ‹Ð²Ð°ÐµÐ¼ Ð³Ð»Ð¾Ð±Ð°Ð»ÑŒÐ½Ñ‹Ðµ ÐºÐ¾Ð¾Ñ€Ð´Ð¸Ð½Ð°Ñ‚Ñ‹
			const FVector3d GlobalVec = SpaceGlobal::ToGlobalVector(GlobalPos);
			SetActorLocation(FVector(GlobalVec), false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
}

void AShipPawn::SyncGlobalFromWorld()
{
	UWorld* World = GetWorld();
	if (!World)
		return;

	if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
	{
		// Ð¾ÑÐ½Ð¾Ð²Ð½Ð¾Ð¹ Ð¿ÑƒÑ‚ÑŒ: Ñ‡ÐµÑ€ÐµÐ· Ð¿Ð»Ð°Ð²Ð°ÑŽÑ‰Ð¸Ð¹ origin
		FO->WorldToGlobal(GetActorLocation(), GlobalPos);
	}
	else
	{
		// fallback, ÐµÑÐ»Ð¸ Ð¿Ð¾Ð´ÑÐ¸ÑÑ‚ÐµÐ¼Ð° Ð½Ðµ Ð¿Ð¾Ð´Ð½ÑÑ‚Ð° (Ð½Ð°Ð¿Ñ€Ð¸Ð¼ÐµÑ€, Ð² ÐºÐ°ÐºÐ¸Ñ…-Ñ‚Ð¾ Ñ‚ÐµÑÑ‚Ð°Ñ…)
		const FVector3d GlobalVec = FVector3d(GetActorLocation()) / 100.0; // ÑÐ¼ -> Ð¼
		SpaceGlobal::FromGlobalVector(GlobalVec, GlobalPos);
	}
}

void AShipPawn::SyncWorldFromGlobal()
{
	// НА СЕРВЕРЕ world-позиция задаётся физикой и FloatingOrigin.
	// Никакого пересчёта "из GlobalPos" там делать нельзя, иначе словим телепорт.
	if (HasAuthority())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
		return;

	if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
	{
		const FVector NewWorldLoc = FO->GlobalToWorld(GlobalPos);
		SetActorLocation(NewWorldLoc, false, nullptr, ETeleportType::TeleportPhysics);
	}
	else
	{
		const FVector3d GlobalVec = SpaceGlobal::ToGlobalVector(GlobalPos);
		SetActorLocation(FVector(GlobalVec), false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AShipPawn::SetHyperDriveActive(bool bActive)
{
	if (HasAuthority())
	{
		if (bHyperDriveActive != bActive)
		{
			bHyperDriveActive = bActive;
			OnHyperDriveChanged();
		}
	}
	else
	{
		ServerSetHyperDrive(bActive);
	}
}

void AShipPawn::ToggleHyperDrive()
{
	SetHyperDriveActive(!bHyperDriveActive);
}

void AShipPawn::ServerSetHyperDrive_Implementation(bool bNewActive)
{
	SetHyperDriveActive(bNewActive);
}

void AShipPawn::OnRep_HyperDrive()
{
	OnHyperDriveChanged();
}

void AShipPawn::OnHyperDriveChanged()
{
	const bool bWasHyper = bHyperDrivePrev;
	bHyperDrivePrev = bHyperDriveActive;

	if (GEngine)
	{
		const FColor C = bHyperDriveActive ? FColor::Purple : FColor::Green;
		GEngine->AddOnScreenDebugMessage((uint64)this + 777, 2.0f, C,
			FString::Printf(TEXT("HYPERDRIVE: %s"), bHyperDriveActive ? TEXT("ON") : TEXT("OFF")));
	}

	ApplyFlightProfile(bHyperDriveActive);
	RequestCameraResync();
	CameraResyncFrames = 3;

	// If exiting hyper, stop instantly to avoid long decel and camera trailing.
	if (!bHyperDriveActive && bWasHyper)
	{
		StopAfterHyperDrive();
		if (Net)
		{
			Net->OnHyperDriveExited();
		}
		HyperExitThrottleLockTime = HyperExitThrottleLockSeconds;
		if (!bCameraUseHyperProfileAlways)
		{
			HyperExitBlendTime = HyperExitBlendSeconds;
			HyperExitSnapCounter = HyperExitSnapFrames;
			HyperExitHardSnapTime = HyperExitHardSnapSeconds;
		}
		else
		{
			HyperExitBlendTime = 0.f;
			HyperExitSnapCounter = 0;
			HyperExitHardSnapTime = 0.f;
		}
	}
	else if (bHyperDriveActive)
	{
		HyperExitBlendTime = 0.f;
		HyperExitSnapCounter = 0;
		HyperExitHardSnapTime = 0.f;
		HyperExitThrottleLockTime = 0.f;
	}
}

void AShipPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AShipPawn, bHyperDriveActive);
}

void AShipPawn::ApplyFlightProfile(bool bHyper)
{
	if (!Flight)
	{
		return;
	}

	if (bHyper)
	{
		Flight->Longi = HyperLongi;
		Flight->FA_Trans.VelocityPD      = HyperFA.VelocityPD;
		Flight->FA_Trans.AccelMax_Mps2   = HyperFA.AccelMax_Mps2;
		Flight->FA_Trans.RetroBoostMultiplier = HyperFA.RetroBoostMultiplier;
		Flight->FA_Trans.RetroDotThreshold   = HyperFA.RetroDotThreshold;
		Flight->FA_Trans.Deadzone_Mps        = HyperFA.Deadzone_Mps;
	}
	else
	{
		Flight->Longi = CruiseLongi;
		Flight->FA_Trans.VelocityPD      = CruiseFA.VelocityPD;
		Flight->FA_Trans.AccelMax_Mps2   = CruiseFA.AccelMax_Mps2;
		Flight->FA_Trans.RetroBoostMultiplier = CruiseFA.RetroBoostMultiplier;
		Flight->FA_Trans.RetroDotThreshold   = CruiseFA.RetroDotThreshold;
		Flight->FA_Trans.Deadzone_Mps        = CruiseFA.Deadzone_Mps;
	}
}

void AShipPawn::StopAfterHyperDrive()
{
	if (ShipMesh)
	{
		// Force-stop instead of additive zero so we don't keep hyper velocity after exit.
		ShipMesh->SetPhysicsLinearVelocity(FVector::ZeroVector, false);
		ShipMesh->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector, false);
	}

	if (Flight)
	{
		Flight->ResetDynamicsState();
		// Ensure cruise tuning is active even if a deferred switch left hyper settings around.
		ApplyFlightProfile(false);
	}

	if (Net)
	{
		Net->SetLocalAxes(0.f, 0.f, 0.f, 0.f);
	}

	ResetCameraBufferImmediate();
}

void AShipPawn::OnFloatingOriginShifted()
{
	// Clear stale camera samples after an origin rebasing to keep the view glued to the ship.
	ResetCameraBufferImmediate();
	RequestCameraResync();
	CameraResyncFrames = 2;
}

void AShipPawn::ResetCameraBufferImmediate()
{
	FCamSample S;
	S.Time = FApp::GetCurrentTime();
	const FTransform X = ShipMesh ? ShipMesh->GetComponentTransform() : GetActorTransform();
	S.Loc = X.GetLocation();
	S.Rot = X.GetRotation();
	S.Vel = ShipMesh ? ShipMesh->GetComponentVelocity() : FVector::ZeroVector;

	CamSamples.Empty();
	PushCamSample(S);
	bHaveLastView = false;
	bPivotInit = false;
	bCameraResyncPending = false;
}

void AShipPawn::RequestCameraResync()
{
	bCameraResyncPending = true;
	CameraResyncFrames = 5;
}








