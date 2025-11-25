#include "LaserBeam.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Net/UnrealNetwork.h"

ALaserBeam::ALaserBeam()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates        = true;
	bAlwaysRelevant    = true;
	SetReplicateMovement(false);
	bNetUseOwnerRelevancy = false;
	NetUpdateFrequency = 120.f;
	MinNetUpdateFrequency = 60.f;
	NetPriority = 3.f;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);

	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->SetMobility(EComponentMobility::Movable);

	// Reuse the engine cylinder and rotate it so +X is forward.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cyl.Succeeded())
	{
		Mesh->SetStaticMesh(Cyl.Object);
		Mesh->SetRelativeRotation(FRotator(0.f, 0.f, 90.f));
	}

	InitialLifeSpan = 0.f; // Set in BeginPlay via DurationSec
}

void ALaserBeam::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyShapeScale();
	ApplyMaterialParams();
}

void ALaserBeam::BeginPlay()
{
	Super::BeginPlay();

	if (BaseMaterial)
	{
		MID = Mesh->CreateDynamicMaterialInstance(0, BaseMaterial);
	}
	ApplyMaterialParams();

	SetLifeSpan(DurationSec);
}

void ALaserBeam::ApplyShapeScale()
{
	const float BaseLen = 200.f;
	const float BaseRad = 50.f;

	const float sx  = FMath::Max(LengthUU / BaseLen, 0.01f);
	const float sYZ = FMath::Max(RadiusUU / BaseRad, 0.01f);

	Mesh->SetRelativeScale3D(FVector(sx, sYZ, sYZ));

	if (!FMath::IsNearlyZero(ForwardOffsetUU))
	{
		Mesh->AddLocalOffset(FVector(ForwardOffsetUU, 0.f, 0.f));
	}
}

void ALaserBeam::ApplyMaterialParams()
{
	if (UMaterialInstanceDynamic* M = MID)
	{
		M->SetVectorParameterValue(FName("BeamColor"), BeamColor);
		M->SetScalarParameterValue(FName("EmissiveStrength"), EmissiveStrength);
		M->SetScalarParameterValue(FName("Opacity"), Opacity);
	}
}

void ALaserBeam::SetBeamLength(float NewLengthUU)
{
	LengthUU = FMath::Max(NewLengthUU, 10.f);
	ApplyShapeScale();
}

void ALaserBeam::SetBeamDuration(float NewDurationSec)
{
	DurationSec = FMath::Max(NewDurationSec, 0.01f);
	SetLifeSpan(DurationSec);
}

void ALaserBeam::ConfigureBeam(float NewLengthUU, float NewDurationSec)
{
	LengthUU   = FMath::Max(NewLengthUU, 10.f);
	DurationSec = FMath::Max(NewDurationSec, 0.01f);
	SetLifeSpan(DurationSec);
	ApplyShapeScale();
}

void ALaserBeam::OnRep_Length()
{
	ApplyShapeScale();
}

void ALaserBeam::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ALaserBeam, LengthUU);
}
