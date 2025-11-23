#include "LaserBolt.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Net/UnrealNetwork.h"

ALaserBolt::ALaserBolt()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates        = true;
	bAlwaysRelevant    = true;  // РєСЂРѕС€РµС‡РЅС‹Р№ РІРёР·СѓР°Р», Р»СѓС‡С€Рµ СЃСЂР°Р·Сѓ РІСЃРµРј
	SetReplicateMovement(true);
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

	// Р”РµС„РѕР»С‚РЅР°СЏ РіРµРѕРјРµС‚СЂРёСЏ вЂ” Cylinder РёР· Engine (РјРѕР¶РЅРѕ РїРµСЂРµРѕРїСЂРµРґРµР»РёС‚СЊ РІ BP)
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cyl.Succeeded())
	{
		Mesh->SetStaticMesh(Cyl.Object);
		// РџРѕРІРµСЂРЅРµРј С†РёР»РёРЅРґСЂ, С‡С‚РѕР±С‹ В«РѕСЃСЊ РґР»РёРЅС‹В» Р±С‹Р»Р° РІРґРѕР»СЊ +X
		Mesh->SetRelativeRotation(FRotator(0.f, 0.f, 90.f)); // Сѓ Cylinder РґР»РёРЅР° РїРѕ Z, СЂР°Р·РІРµСЂРЅС‘Рј РЅР° X
	}

	// Р–РёР·РЅРµРЅРЅС‹Р№ С†РёРєР» РєРѕСЂРѕС‚РєРёР№
	InitialLifeSpan = 0.f; // Р±СѓРґРµРј Р·Р°РґР°РІР°С‚СЊ РІ BeginPlay РёР· LifeTimeSec
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
		// РРЎРџР РђР’Р›Р•РќРР•: Р”Р»СЏ РїСЂРёС†РµР»СЊРЅРѕР№ СЃС‚СЂРµР»СЊР±С‹ РќР• РЅР°СЃР»РµРґСѓРµРј СЃРєРѕСЂРѕСЃС‚СЊ РєРѕСЂР°Р±Р»СЏ
		// Р‘РѕР»С‚С‹ Р»РµС‚СЏС‚ СЃС‚СЂРѕРіРѕ Рє РєСѓСЂСЃРѕСЂСѓ Р±РµР· РёСЃРєСЂРёРІР»РµРЅРёСЏ С‚СЂР°РµРєС‚РѕСЂРёРё
		// Р•СЃР»Рё РЅСѓР¶РЅР° "СЂРµР°Р»РёСЃС‚РёС‡РЅР°СЏ" Р±Р°Р»Р»РёСЃС‚РёРєР° - РёСЃРїРѕР»СЊР·СѓР№С‚Рµ InheritOwnerVelPct > 0
		
		const FVector BoltForward = GetActorForwardVector();
		const FVector vForward = BoltForward * SpeedUUps;
		
		// РћРїС†РёРѕРЅР°Р»СЊРЅРѕ: РјРѕР¶РЅРѕ РґРѕР±Р°РІРёС‚СЊ РЅРµР±РѕР»СЊС€РѕР№ РїСЂРѕС†РµРЅС‚ РїСЂРѕРґРѕР»СЊРЅРѕР№ СЃРєРѕСЂРѕСЃС‚Рё
		// РґР»СЏ СЌС„С„РµРєС‚Р° "РІС‹СЃС‚СЂРµР»Р° СЃ РґРІРёР¶СѓС‰РµРіРѕСЃСЏ РєРѕСЂР°Р±Р»СЏ"
		FVector vTotal = vForward;
		
		if (InheritOwnerVelPct > 0.001f)
		{
			// РќР°СЃР»РµРґСѓРµРј С‚РѕР»СЊРєРѕ РєРѕРјРїРѕРЅРµРЅС‚ РІРґРѕР»СЊ РЅР°РїСЂР°РІР»РµРЅРёСЏ РїРѕР»С‘С‚Р° Р±РѕР»С‚Р°
			const float InheritedSpeed = FVector::DotProduct(BaseVelW, BoltForward);
			vTotal += BoltForward * FMath::Max(0.f, InheritedSpeed) * InheritOwnerVelPct;
		}
		
		const FVector Delta = vTotal * DeltaSeconds;

		// Р’РёР·СѓР°Р»СЊРЅС‹Р№ СЃРЅР°СЂСЏРґ вЂ” С‚РµР»РµРїРѕСЂС‚РѕРј Р±РµР· sweep
		SetActorLocation(GetActorLocation() + Delta, false, nullptr, ETeleportType::None);
	}
}

void ALaserBolt::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ALaserBolt, BaseVelW);
}


void ALaserBolt::ApplyShapeScale()
{
	// РЎС‚Р°РЅРґР°СЂС‚РЅС‹Р№ Cylinder РёРјРµРµС‚ РІС‹СЃРѕС‚Сѓ ~200 UU РІРґРѕР»СЊ Р»РѕРєР°Р»СЊРЅРѕРіРѕ Z.
	// РњС‹ РїРѕРІРµСЂРЅСѓР»Рё РµРіРѕ С‚Р°Рє, С‡С‚Рѕ РІС‹СЃРѕС‚Р° С‚РµРїРµСЂСЊ РІРґРѕР»СЊ +X. РџСЂРѕРїРѕСЂС†РёСЏ: ScaleX = Length / 200, ScaleY/Z = Radius / 50 (РїСЂРёРјРµСЂРЅРѕ).
	// РўРѕС‡РЅСѓСЋ С†РёС„СЂСѓ РјРѕР¶РЅРѕ РїРѕРґРѕРіРЅР°С‚СЊ РІ BP (РёР»Рё Р·РґРµСЃСЊ РїРѕРїСЂР°РІРёС‚СЊ РєРѕРЅСЃС‚Р°РЅС‚С‹ РїРѕРґ РІР°С€ РјРµС€).
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
