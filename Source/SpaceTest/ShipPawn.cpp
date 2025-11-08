// ShipPawn.cpp
#include "ShipPawn.h"
#include "FlightComponent.h"

#include "Components/StaticMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

AShipPawn::AShipPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup    = TG_PrePhysics;

	// Root mesh
	ShipMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ShipMesh"));
	SetRootComponent(ShipMesh);

	// Физика по умолчанию включена — важно для сил/моментов полётного компонента.
	ShipMesh->SetSimulatePhysics(true);
	ShipMesh->SetEnableGravity(false);
	ShipMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	ShipMesh->SetCollisionObjectType(ECC_Pawn);
	ShipMesh->SetNotifyRigidBodyCollision(false);
	ShipMesh->SetLinearDamping(0.02f);
	ShipMesh->SetAngularDamping(0.02f);
	ShipMesh->BodyInstance.bUseCCD = true; // лучше для быстрых кораблей

	// Flight
	Flight = CreateDefaultSubobject<UFlightComponent>(TEXT("Flight"));

	// Simple camera rig
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 800.f;
	SpringArm->bEnableCameraLag = false;     // пока без UE CameraLag — он конфликтен в сети и не нужен в standalone
	SpringArm->bDoCollisionTest = false;
	SpringArm->SetUsingAbsoluteRotation(false);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
}

void AShipPawn::BeginPlay()
{
	Super::BeginPlay();

	// Если дизайнер выключил физику в BP — принудительно включим.
	if (ShipMesh && !ShipMesh->IsSimulatingPhysics())
	{
		ShipMesh->SetSimulatePhysics(true);
		ShipMesh->SetEnableGravity(false);
	}
}

void AShipPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
}

void AShipPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	check(PlayerInputComponent);

	// Примерные биндинги (создай их в Project Settings → Input):
	// Axis:
	//  - "ThrustForward" (W=+1, S=-1)
	//  - "StrafeRight"   (D=+1, A=-1)
	//  - "ThrustUp"      (Space=+1, Ctrl=-1 или C=-1)
	//  - "Roll"          (Q=-1, E=+1)
	//  - "Turn"          (Mouse X)
	//  - "LookUp"        (Mouse Y)
	//
	// Action:
	//  - "ToggleFlightAssist" (например, F)

	PlayerInputComponent->BindAxis(TEXT("ThrustForward"), this, &AShipPawn::Axis_ThrustForward);
	PlayerInputComponent->BindAxis(TEXT("StrafeRight"),   this, &AShipPawn::Axis_StrafeRight);
	PlayerInputComponent->BindAxis(TEXT("ThrustUp"),      this, &AShipPawn::Axis_ThrustUp);
	PlayerInputComponent->BindAxis(TEXT("Roll"),          this, &AShipPawn::Axis_Roll);

	PlayerInputComponent->BindAxis(TEXT("Turn"),          this, &AShipPawn::Axis_MouseYaw);
	PlayerInputComponent->BindAxis(TEXT("LookUp"),        this, &AShipPawn::Axis_MousePitch);

	PlayerInputComponent->BindAction(TEXT("ToggleFlightAssist"), IE_Pressed, this, &AShipPawn::Action_ToggleFA);
}

void AShipPawn::Axis_ThrustForward(float V) { if (Flight) Flight->SetThrustForward(V); }
void AShipPawn::Axis_StrafeRight (float V)  { if (Flight) Flight->SetStrafeRight (V); }
void AShipPawn::Axis_ThrustUp    (float V)  { if (Flight) Flight->SetThrustUp    (V); }
void AShipPawn::Axis_Roll        (float V)  { if (Flight) Flight->SetRollAxis    (V); }

void AShipPawn::Axis_MouseYaw    (float V)  { if (Flight) Flight->AddMouseYaw   (V); }
void AShipPawn::Axis_MousePitch  (float V)  { if (Flight) Flight->AddMousePitch (V); }

void AShipPawn::Action_ToggleFA()           { if (Flight) Flight->ToggleFlightAssist(); }
