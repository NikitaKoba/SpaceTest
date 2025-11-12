#include "LaserBolt.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

ALaserBolt::ALaserBolt()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates        = true;
	bAlwaysRelevant    = true;  // крошечный визуал, лучше сразу всем
	SetReplicateMovement(true);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);

	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->SetMobility(EComponentMobility::Movable);

	// Дефолтная геометрия — Cylinder из Engine (можно переопределить в BP)
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cyl.Succeeded())
	{
		Mesh->SetStaticMesh(Cyl.Object);
		// Повернем цилиндр, чтобы «ось длины» была вдоль +X
		Mesh->SetRelativeRotation(FRotator(0.f, 0.f, 90.f)); // у Cylinder длина по Z, развернём на X
	}

	// Жизненный цикл короткий
	InitialLifeSpan = 0.f; // будем задавать в BeginPlay из LifeTimeSec
}

void ALaserBolt::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyShapeScale();
	ApplyMaterialParams();
}

void ALaserBolt::BeginPlay()
{
	Super::BeginPlay();

	if (BaseMaterial)
	{
		MID = Mesh->CreateDynamicMaterialInstance(0, BaseMaterial);
	}
	ApplyMaterialParams();

	SetReplicateMovement(bRepMove);
	SetLifeSpan(LifeTimeSec);
}

void ALaserBolt::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (SpeedUUps > 1.f)
	{
		// ИСПРАВЛЕНИЕ: Для прицельной стрельбы НЕ наследуем скорость корабля
		// Болты летят строго к курсору без искривления траектории
		// Если нужна "реалистичная" баллистика - используйте InheritOwnerVelPct > 0
		
		const FVector BoltForward = GetActorForwardVector();
		const FVector vForward = BoltForward * SpeedUUps;
		
		// Опционально: можно добавить небольшой процент продольной скорости
		// для эффекта "выстрела с движущегося корабля"
		FVector vTotal = vForward;
		
		if (InheritOwnerVelPct > 0.001f)
		{
			// Наследуем только компонент вдоль направления полёта болта
			const float InheritedSpeed = FVector::DotProduct(BaseVelW, BoltForward);
			vTotal += BoltForward * FMath::Max(0.f, InheritedSpeed) * InheritOwnerVelPct;
		}
		
		const FVector Delta = vTotal * DeltaSeconds;

		// Визуальный снаряд — телепортом без sweep
		SetActorLocation(GetActorLocation() + Delta, false, nullptr, ETeleportType::None);
	}
}


void ALaserBolt::ApplyShapeScale()
{
	// Стандартный Cylinder имеет высоту ~200 UU вдоль локального Z.
	// Мы повернули его так, что высота теперь вдоль +X. Пропорция: ScaleX = Length / 200, ScaleY/Z = Radius / 50 (примерно).
	// Точную цифру можно подогнать в BP (или здесь поправить константы под ваш меш).
	const float BaseLen = 200.f;
	const float BaseRad = 50.f;

	const float sx = FMath::Max(LengthUU / BaseLen, 0.01f);
	const float sYZ = FMath::Max(RadiusUU / BaseRad, 0.01f);

	Mesh->SetRelativeScale3D(FVector(sx, sYZ, sYZ));

	if (!FMath::IsNearlyZero(ForwardOffsetUU))
	{
		Mesh->AddLocalOffset(FVector(ForwardOffsetUU, 0.f, 0.f));
	}
}

void ALaserBolt::ApplyMaterialParams()
{
	if (UMaterialInstanceDynamic* M = MID)
	{
		M->SetVectorParameterValue(FName("BeamColor"), BeamColor);
		M->SetScalarParameterValue(FName("EmissiveStrength"), EmissiveStrength);
		M->SetScalarParameterValue(FName("Opacity"), Opacity);
	}
}