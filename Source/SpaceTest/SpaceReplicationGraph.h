#pragma once

#include "CoreMinimal.h"
#include "ReplicationGraph.h"
#include "SpaceReplicationGraph.generated.h"

class AShipPawn;
class UReplicationGraphNode_GridSpatialization2D;
class UReplicationGraphNode_ActorList;
class UReplicationGraphNode_AlwaysRelevant_ForConnection;
class UNetReplicationGraphConnection;

/**
 * AAA-style RepGraph с перцептуальной приоритизацией и адаптивным бюджетом.
 */
UCLASS(Transient, Config=Engine)
class SPACETEST_API USpaceReplicationGraph : public UReplicationGraph
{
	GENERATED_BODY()

public:
	USpaceReplicationGraph();

	// UReplicationGraph
	virtual void InitGlobalGraphNodes() override;
	virtual void InitGlobalActorClassSettings() override;
	virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnectionManager) override;
	virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo) override;
	virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual void BeginDestroy() override;
	// Инициализация узлов под конкретное соединение оставляем как было

	// НОВЫЙ оверрайд: корректное удаление per-connection узлов/состояния
	virtual void RemoveClientConnection(UNetConnection* NetConnection) override;

	// Зови на сервере сразу после Possess/смены владельца
	void HandlePawnPossessed(class APawn* Pawn);

	// Мягкий ребайас XY для 2D-грида
	void RebiasToXY(const FVector& WorldLoc);

	// LiveLog/авто-ребайас тикер
	bool LiveLog_Tick(float DeltaTime);
	void LiveLog_OnConnAdded(class UNetReplicationGraphConnection* ConnMgr);
	void LiveLog_OnConnRemoved(class UNetReplicationGraphConnection* ConnMgr);

private:
	// --- Глобальные узлы ---
	UPROPERTY() TObjectPtr<UReplicationGraphNode_GridSpatialization2D> GridNode = nullptr;
	UPROPERTY() TObjectPtr<UReplicationGraphNode_ActorList>            AlwaysRelevantNode = nullptr;

	// --- Per-connection base node ---
	TMap<UNetReplicationGraphConnection*, TWeakObjectPtr<UReplicationGraphNode_AlwaysRelevant_ForConnection>> PerConnAlwaysMap;

	// --- Live telemetry & scoring state per connection ---
	struct FActorEMA
	{
		// Оценки стоимости
		float BytesEMA       = 96.f;   // средний вес апдейта, байт
		float SerializeMsEMA = 0.02f;  // оценка CPU на сериализацию, мс

		// Оценки динамики (непредсказуемость)
		float SigmaA = 50.f;           // uu/s^2
		float SigmaJ = 0.f;            // uu/s^3

		// Радиус визуальный (кэш)
		float RadiusUU = 200.f;

		// Последние состояния для вычисления a/j
		FVector PrevPos = FVector::ZeroVector;
		FVector PrevVel = FVector::ZeroVector;
		FVector PrevAccel = FVector::ZeroVector;
		double  PrevStamp = 0.0;

		bool bInit = false;
	};

	struct FViewerEMA
	{
		// Векторная кинематика зрителя
		FVector PrevPos = FVector::ZeroVector;
		FVector PrevVel = FVector::ZeroVector;
		double  PrevStamp = 0.0;
		bool    bInit = false;

		// RTT/потери (оценки)
		float RTTmsEMA   = 80.f;   // мс
		float LossEMA    = 0.0f;   // 0..1

		// Бюджет (адаптивный)
		float BudgetBytesPerTick = 6000.f; // стартовый (≈24 кБ/с @ 4 Гц)
		float UsedBytesEMA       = 0.f;

		// AIMD
		int32 OkTicks = 0;
	};

	struct FConnState
	{
		// Видимые/выбранные акторы для стабильности
		TSet<TWeakObjectPtr<AActor>> Visible;    // для хистерезиса
		TSet<TWeakObjectPtr<AActor>> Selected;   // кого добавили в тик

		// EMA per actor
		TMap<TWeakObjectPtr<AActor>, FActorEMA> ActorStats;

		// Viewer stats
		FViewerEMA Viewer;

		// Группа-ячейки для батчинга (счётчики)
		int32 GroupsFormed = 0;
	};

	// Все активные коннекты и их состояние
	TMap<UNetReplicationGraphConnection*, FConnState> ConnStates;

	// Трекинг «кораблей»
	TSet<TWeakObjectPtr<AShipPawn>> TrackedShips;

	// Тикер
	FTSTicker::FDelegateHandle LiveLogTickerHandle;

	// Авто-ребайас гистерезис (не static)
	int64 LastAppliedCellX = INT64_MIN;
	int64 LastAppliedCellY = INT64_MIN;
	double LastRebiasWall  = 0.0;

	// Helpers
	static bool IsAlwaysRelevantByClass(const AActor* Actor);
	static class UNetConnection* FindOwnerConnection(AActor* Actor);
	static void LogChannelState(UNetReplicationGraphConnection* ConnMgr, AActor* A, const TCHAR* Reason);

	// === Приоритизатор ===
	struct FCandidate
	{
		TWeakObjectPtr<AActor> Actor;
		int32 GroupKey = 0;     // ключ батч-ячейки
		float Score    = 0.f;   // u / c
		float Cost     = 0.f;   // байты (+ header если первый в группе)
		float U        = 0.f;   // полезность
	};

	// Расчёты
	float ComputePerceptualScore(
		const AActor* Actor,
		const APawn* ViewerPawn,
		FActorEMA& AStat,
		FViewerEMA& VStat,
		float DeltaTime,
		float& OutCostB,     // байты
		float& OutU);

	FVector GetActorVelocity(const AActor* A) const;
	FVector GetActorAngularVel(const AActor* A) const;
	float   GetActorRadiusUU(AActor* A, FActorEMA& Cache);
	FVector GetViewerForward(const APawn* ViewerPawn) const;

	// Группировка (ячейки вокруг зрителя)
	int32   MakeGroupKey(const FVector& ViewLoc, const FVector& ActorLoc, float CellUU) const;

	// Обновление EMA
	// СТАЛО: универсальная EMA (работает и с float, и с FVector)
	template<typename T>
	static FORCEINLINE T EMA(const T& Old, const T& Value, float Alpha)
	{
		return Old + (Value - Old) * Alpha; // без FMath::Lerp, чтобы не ловить несоответствие типов
	}


	// Обновление бюджета
	void UpdateAdaptiveBudget(UNetReplicationGraphConnection* ConnMgr, FConnState& CS, float UsedBytesThisTick, float TickDt);

	// Отладка
	void LogPerConnTick(UNetReplicationGraphConnection* ConnMgr, const FConnState& CS, int32 NumTracked, int32 NumCand, int32 NumChosen, float UsedBytes, float TickDt);
};
