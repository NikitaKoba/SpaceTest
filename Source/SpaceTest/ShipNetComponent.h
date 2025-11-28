// ShipNetComponent.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Net/UnrealNetwork.h"
#include "Net/UnrealNetwork.h"
#include "ShipNetComponent.generated.h"

// --- Лёгкая компрессия углов (Pitch/Yaw/Roll) в int16 ---
USTRUCT()
struct FRotShort
{
	GENERATED_BODY()

	int16 Pitch = 0, Yaw = 0, Roll = 0;

	static FORCEINLINE int16 AngleToS16(float Deg)
	{
		// 360° -> 65536
		const float S = Deg / 360.f;
		return (int16)FMath::RoundToInt(S * 65536.f);
	}
	static FORCEINLINE float S16ToAngle(int16 S)
	{
		return (float(S) / 65536.f) * 360.f;
	}

	FORCEINLINE FRotator ToRotator() const
	{
		return FRotator(S16ToAngle(Pitch), S16ToAngle(Yaw), S16ToAngle(Roll));
	}
	FORCEINLINE void FromRotator(const FRotator& R)
	{
		Pitch = AngleToS16(FRotator::ClampAxis(R.Pitch));
		Yaw   = AngleToS16(FRotator::ClampAxis(R.Yaw));
		Roll  = AngleToS16(FRotator::ClampAxis(R.Roll));
	}

	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
	{
		Ar << Pitch; Ar << Yaw; Ar << Roll;
		bOutSuccess = true;
		return true;
	}
};
template<> struct TStructOpsTypeTraits<FRotShort> : public TStructOpsTypeTraitsBase2<FRotShort>
{
	enum { WithNetSerializer = true };
};

// --- Квантованный серверный снап ---
USTRUCT()
struct FShipServerSnap
{
	GENERATED_BODY()

	// Позиция: точность ~1 см, в uu
	UPROPERTY() FVector_NetQuantize100 Loc;        // 16-20 бит/ось
	// Ориентация: 3*int16
	UPROPERTY() FRotShort              RotCS;
	// Лин. скорость: 0.1 см/с точность
	UPROPERTY() FVector_NetQuantize10 Vel;        // компактно
	// Угл. скорость (рад/с) → в uu: храню в град/с ради читаемости (квант 0.1)
	UPROPERTY() FVector_NetQuantize10 AngVelDeg;  // храню в deg/s

	// Серверное время (сек). Можно ужать в half, но оставлю float для стабильности оффсета.
	UPROPERTY() float                  ServerTime = 0.f;

	// ACK: до какого инпута сервер досчитал
	UPROPERTY() int32                  LastAckSeq = 0;

	UPROPERTY() FVector_NetQuantize100 OriginGlobalM = FVector::ZeroVector;
	UPROPERTY() FVector_NetQuantize    WorldOriginUU = FVector::ZeroVector;

	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
	{
		bool bOk = true, bTmp = true;
		bOk &= Loc.NetSerialize(Ar, Map, bTmp);
		bOk &= RotCS.NetSerialize(Ar, Map, bTmp);
		bOk &= Vel.NetSerialize(Ar, Map, bTmp);
		bOk &= AngVelDeg.NetSerialize(Ar, Map, bTmp);
		bOk &= OriginGlobalM.NetSerialize(Ar, Map, bTmp);
		bOk &= WorldOriginUU.NetSerialize(Ar, Map, bTmp);

		Ar << ServerTime;

		// varint для ack
		if (Ar.IsSaving())
		{
			Ar.SerializeIntPacked((uint32&)LastAckSeq);
		}
		else
		{
			uint32 Packed = 0;
			Ar.SerializeIntPacked(Packed);
			LastAckSeq = (int32)Packed;
		}

		bOutSuccess = bOk && bTmp;
		return true;
	}
};
template<> struct TStructOpsTypeTraits<FShipServerSnap> : public TStructOpsTypeTraitsBase2<FShipServerSnap>
{
	enum { WithNetSerializer = true };
};

// --- Инпут на кадр (то, что ты уже шлёшь) ---
USTRUCT()
struct FControlState
{
	GENERATED_BODY()
	UPROPERTY() int32  Seq = 0;
	UPROPERTY() float  DeltaTime = 0.f;
	UPROPERTY() float  ThrustF = 0.f;
	UPROPERTY() float  ThrustR = 0.f;
	UPROPERTY() float  ThrustU = 0.f;
	UPROPERTY() float  Roll    = 0.f;
	UPROPERTY() float  MouseX  = 0.f;
	UPROPERTY() float  MouseY  = 0.f;
};

// --- Узел интерполяции для сим-прокси ---
USTRUCT()
struct FInterpNode
{
	GENERATED_BODY()
	UPROPERTY() double   Time = 0.0;          // в клиентском времени
	UPROPERTY() FVector  Loc = FVector::ZeroVector;
	UPROPERTY() FQuat    Rot = FQuat::Identity;
	UPROPERTY() FVector  Vel = FVector::ZeroVector;      // см/с
	UPROPERTY() FVector  AngVel = FVector::ZeroVector;   // рад/с
};

// лог-тумблеры
DECLARE_LOG_CATEGORY_EXTERN(LogShipNet, Log, All);

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipNetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipNetComponent();

	// --- API из твоего кода ---
	void SetLocalAxes(float F, float R, float U, float Roll);
	void AddMouseDelta(float Dx, float Dy);
	// Reset interpolation state when exiting hyperdrive to prevent long trailing.
	void OnHyperDriveExited();
	FVector3d PrevOriginGlobal = FVector3d::ZeroVector;
	bool bHavePrevOrigin = false;
	void HandleFloatingOriginShift();
	// === Настройки ===
	// Задержка для сим-прокси, будет адаптивно настраиваться
	UPROPERTY(EditAnywhere) float NetInterpDelay = 0.05f;      // старт
	UPROPERTY(EditAnywhere) float NetInterpDelayMin = 0.01f;
	UPROPERTY(EditAnywhere) float NetInterpDelayMax = 0.1f;
	UPROPERTY() double LastSnapClientTime = 0.0;
	UPROPERTY() double LastSnapServerTime = 0.0;

	// Владельческая мягкая реконсиляция
	UPROPERTY(EditAnywhere) float OwnerReconTau       = 0.12f;
	UPROPERTY(EditAnywhere) float OwnerHardSnapDistance = 200.f;  // см
	UPROPERTY(EditAnywhere) float OwnerMaxVelNudge   = 8000.f;    // см/с за секунду

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// === Репликация ===
	UPROPERTY(ReplicatedUsing=OnRep_ServerSnap) FShipServerSnap ServerSnap;
	UFUNCTION() void OnRep_ServerSnap();

	// Входящий RPC с инпутом (unreliable — поток)
	UFUNCTION(Server, unreliable) void Server_SendInput(const FControlState& State);
	void Server_SendInput_Implementation(const FControlState& State);

	// === Служебка ===
	void UpdatePhysicsSimState();
	void DriveSimulatedProxy();
	void OwnerReconcile_Tick(float DeltaSeconds);
	void AdaptiveInterpDelay_OnSample(double OffsetSample);

	// Маленький интегратор для ориентации по угл.скорости
	static FQuat IntegrateQuat(const FQuat& Q0, const FVector& AngVelRad, float Dt);

	// === Поля ===
	UPROPERTY() class APawn*             OwPawn = nullptr;
	UPROPERTY() class AShipPawn*         Ship   = nullptr;
	UPROPERTY() class UStaticMeshComponent* ShipMesh = nullptr;
	UPROPERTY() class UFlightComponent*  Flight = nullptr;

	// Локальные оси/мышь (как у тебя)
	float AxisF_Cur=0, AxisR_Cur=0, AxisU_Cur=0, AxisRoll_Cur=0;
	float MouseX_Accum=0, MouseY_Accum=0;

	// Буфер для сим-прокси
	TArray<FInterpNode> NetBuffer;

	// Для привязки серверного времени
	bool   bHaveTimeSync = false;
	double ServerTimeToClientTime = 0.0;

	// === Владелец: pending inputs + ack ===
	int32  LocalInputSeq = 0;
	TArray<FControlState> PendingInputs; // ждут ack

	// Цель для мягкой реконсиляции (оставляю совместимой с твоим кодом)
	bool   bHaveOwnerRecon = false;
	FShipServerSnap OwnerReconTarget;

	// Адаптивный delay: окно смещений
	TStaticArray<double, 64> DelaySamples;
	int32 DelaySamplesCount = 0;
	int32 DelaySamplesHead  = 0;

	// Tick порядок
	void SetupTickOrder();
};
