// ShipAIPilotComponent.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ShipAIPilotComponent.generated.h"

UENUM(BlueprintType)
enum class EDogfightStyle : uint8
{
	Pursuit     UMETA(DisplayName="Pursuit"),
	FlankLeft   UMETA(DisplayName="Flank Left"),
	FlankRight  UMETA(DisplayName="Flank Right"),
	BoomAndZoom UMETA(DisplayName="Boom & Zoom")
};

class UFlightComponent;
class UPrimitiveComponent;

/**
 * AI-пилот: сервер-only автопилот, который:
 *  - держит нос на игроке,
 *  - летит за ним на заданной дистанции,
 *  - использует в основном тягу вперёд (W),
 *  - A/D и вертикаль сильно ограничены.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipAIPilotComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipAIPilotComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction
	) override;
	void UpdateAI_AttackLaser(float Dt, AActor* Target);
	// Насколько сильно мы "штрафуем" цель, на которую уже смотрят союзники.
	// 0.0 = игнорировать, 0.7..1.0 = активно распределять цели.
	UPROPERTY(EditAnywhere, Category="AI|Targeting")
	float TargetSpreadWeight = 0.7f;

	// Сколько секунд держать жёсткий лок, когда мы реально сидим на хвосте.
	UPROPERTY(EditAnywhere, Category="AI|Targeting")
	float TailLockTimeSec = 4.0f;

	// Сколько времени ещё действует этот жёсткий лок.
	// Отображается только для дебага.
	UPROPERTY(VisibleInstanceOnly, Category="AI|Targeting")
	float TailLockTimeLeft = 0.0f;
	/** Кого преследуем. Если пусто и bAutoAcquirePlayer=true — выбираем ближайший корабль игрока. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Target")
	TWeakObjectPtr<AActor> TargetActor;

	/** Автоматически искать ближайшего корабль игрока, если TargetActor не задан. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Target")
	bool bAutoAcquirePlayer = true;
	/** Автовыбор ближайшего вражеского корабля (TeamId отличается от владельца). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Target")
	bool bAutoAcquireEnemies = true;
	float OrbitStuckTime = 0.f;
	/** Дистанция (см) позади носа цели, на которой хотим висеть. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow")
	float FollowDistanceCm = 6000.f; // 60 м
	/** Наклоняться в поворот (ролл) или держать ровный корабль. */


	// ----------------- ATTACK MODE (лазер) -----------------

	/** Включить режим агрессивной атаки с лазером вместо обычного Follow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	bool bAttackMode = true;

	/** Комфортная дистанция атаки (см, по сути "кольцо" вокруг цели). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackIdealDistanceCm = 4000.f; // 40 м

	/** "Далеко": дальше этого — жмём газ почти в пол. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackFarDistanceCm = 9000.f; // 90 м

	/** "Слишком близко": ближе — не разгоняемся вперёд как бешеный. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackTooCloseDistanceCm = 1500.f; // 15 м

	/** Anti head-on bubble size (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="250.0", ClampMax="10000.0"))
	float AvoidHeadOnDistanceCm = 1400.f;

	/** Dot threshold to treat alignment as head-on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="1.0"))
	float AvoidHeadOnFrontDot = 0.55f;

	/** Closing speed (m/s) that triggers head-on dodge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="1.0", ClampMax="200.0"))
	float AvoidHeadOnClosingSpeedMps = 40.f;

	/** Насколько агрессивнее, чем Follow, разгоняться к цели (множитель для PosKp). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.1", ClampMax="10.0"))
	float AttackPosKpMul = 2.f;

	/** Насколько усилить демпфирование по скорости (множитель для PosKd). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.1", ClampMax="10.0"))
	float AttackPosKdMul = 1.5f;

	/** При каком угле (градусы), если цель за кормой, переходим в "мертвую петлю". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="180.0"))
	float AttackLoopTriggerAngleDeg = 130.f;

	/** Минимальная дистанция (м), при которой пускать петлю имеет смысл. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackLoopMinDistM = 500.f;

	/** Максимальная дистанция стрельбы лазером (м). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackFireMaxDistM = 3000.f;

	/** Допустимый угол (конуса) по носу для стрельбы (градусы). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="1.0", ClampMax="45.0"))
	float AttackFireAngleDeg = 10.f;
	// --- Tail lock ---


	bool  bInExtendState = false;
	float ExtendTimeLeft = 0.0f;

	/** Tail-chase window to keep bots glued to the target instead of re-orbiting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.1", ClampMax="5.0"))
	float TailLockDistMinFactor = 0.65f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.1", ClampMax="5.0"))
	float TailLockDistMaxFactor = 2.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="1.0"))
	float TailLockFrontDot = 0.55f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="-1.0", ClampMax="1.0"))
	float TailLockTargetFacingDot = 0.25f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.01", ClampMax="1.0"))
	float TailLockLookAheadSec = 0.25f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.05", ClampMax="1.0"))
	float TailLockLateralK = 0.35f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.05", ClampMax="1.0"))
	float TailLockVerticalK = 0.35f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="400.0"))
	float TailLockMaxClosingSpeedMps = 120.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="1.0"))
	float TailLockForwardHold = 0.45f;

	/** Minimum forward push bots try to keep even while maneuvering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="1.0"))
	float AggressiveMinForwardAxis = 0.55f;

	/** Forward boost when already nose-on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="1.0"))
	float AggressiveAlignedBoost = 0.2f;

	/** Extra strafe bias to keep orbiting instead of ramming. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="1.5"))
	float OrbitStrafeBias = 0.5f;

	/** Keep at least this forward speed in combat (m/s) so bots don't stall in place. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="200.0"))
	float CombatMinSpeedMps = 38.f;

	/** How hard we can brake/reverse when too close. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.1", ClampMax="1.0"))
	float CloseRangeBackoffAxis = 0.4f;

	/** Upward bias while separating at knife-fight range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="1.5"))
	float CloseRangeVerticalBias = 0.35f;

	/** How long a selected dogfight style is held (min seconds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Strategy", meta=(ClampMin="1.0", ClampMax="20.0"))
	float StrategyMinHoldTime = 3.f;

	/** How long a selected dogfight style is held (max seconds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Strategy", meta=(ClampMin="1.0", ClampMax="20.0"))
	float StrategyMaxHoldTime = 7.f;

	/** Lateral offset used while dodging head-on merges (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Strategy", meta=(ClampMin="100.0", ClampMax="6000.0"))
	float MergeDodgeLateralCm = 2600.f;

	/** Distance at which extend/reset maneuver is allowed to finish (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Strategy", meta=(ClampMin="500.0", ClampMax="20000.0"))
	float ExtendExitDistanceCm = 6500.f;

	/** Time in close/merge stall before forcing an extend/reset (sec). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Strategy", meta=(ClampMin="0.1", ClampMax="5.0"))
	float StuckTimeBeforeExtend = 1.0f;

	/** Min time bots keep an extend/reset maneuver (sec). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Strategy", meta=(ClampMin="0.1", ClampMax="5.0"))
	float ExtendStateMinTime = 1.0f;

	/** Max time bots keep an extend/reset maneuver (sec). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Strategy", meta=(ClampMin="0.1", ClampMax="5.0"))
	float ExtendStateMaxTime = 1.8f;

	/** Vertical offset used by boom-zoom style (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Strategy", meta=(ClampMin="0.0", ClampMax="8000.0"))
	float BoomZoomHeightCm = 2200.f;

	/** Эффективная "скорость" лазера (м/с) для расчёта упреждения. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackLaserSpeedMps = 300000.f;

	/** Серверное решение: хотим ли в этом тике жать триггер лазера. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="AI|Attack")
	bool bWantsToFireLaser = false;

	/** Куда целиться лазером (мировая позиция с упреждением). Актуально, если bWantsToFireLaser=true. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="AI|Attack")
	FVector LaserAimWorldLocation = FVector::ZeroVector;
	/** Смещен	ие по высоте (см) относительно цели (по её Up-вектору). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow")
	float FollowHeightCm = 0.f;

	/** П-гейн для контура позиция→скорость (1/с). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow", meta=(ClampMin="0.05", ClampMax="2.0"))
	float PosKp = 0.4f;

	/** D-гейн по относительной скорости. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow", meta=(ClampMin="0.0", ClampMax="3.0"))
	float PosKd = 0.8f;

	/** Если расстояние меньше этого, разрешаем чуть сдавать назад для торможения (см). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow")
	float BrakeDistanceCm = 2500.f;

	/** Гейн [deg/s per deg] для yaw (поворот носа лево/право). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient")
	float YawGain_DegPerDeg = 6.f;

	/** Гейн [deg/s per deg] для pitch (нос вверх/вниз). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient")
	float PitchGain_DegPerDeg = 6.f;
	// Rate clamps while aligning (prevents loops on heavy corvettes)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.05", ClampMax="1.0"))
	float FollowYawRateFraction = 0.6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.05", ClampMax="1.0"))
	float FollowPitchRateFraction = 0.35f;
	// Heavy ship caps (corvettes)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.05", ClampMax="1.0"))
	float HeavyYawRateFraction = 0.50f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.05", ClampMax="1.0"))
	float HeavyPitchRateFraction = 0.35f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.1", ClampMax="2.0"))
	float AlignRateFactor = 1.2f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.1", ClampMax="1.0"))
	float HeavyAlignRateFactor = 0.8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="100.0", ClampMax="20000.0"))
	float HeavyBehindDistanceCm = 12000.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="-10000.0", ClampMax="10000.0"))
	float HeavyLateralOffsetCm = 3500.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.05", ClampMax="1.0"))
	float HeavyMaxForwardAxis = 0.45f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.05", ClampMax="1.5"))
	float HeavyMaxStrafeRightAxis = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.05", ClampMax="1.5"))
	float HeavyMaxThrustUpAxis = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient")
	bool bDisableRearLoopForHeavy = true;
	/** Scale forward thrust for AI so bots don't outrun players. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Throttle", meta=(ClampMin="0.1", ClampMax="1.0"))
	float AIForwardAxisScale = 0.6f;
	void    UpdateAI_Follow(float Dt, AActor* Target);

	/** Максимальная ось страфа (A/D), чтобы корабль не улетал боком. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MaxStrafeRightAxis = 0.35f;
	// --- сглаженные команды ---
	float SmoothedForwardAxis   = 0.f;
	float SmoothedYawRateDeg    = 0.f;
	float SmoothedPitchRateDeg  = 0.f;
	float SmoothedRollRateDeg   = 0.f;
	/** Максимальная ось вертикали (Space/Ctrl). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MaxThrustUpAxis = 0.35f;

	/** Наклоняться в поворот (ролл) или держать ровный корабль. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient")
	bool bUseBankingRoll = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Modes")
	bool bStaticGunship = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Modes", meta=(ClampMin="0.01", ClampMax="0.5"))
	float StaticGunshipAxisClamp = 0.2f;

	/** Сколько секунд держим tail-chase, даже если геометрия слегка ломается (гистерезис). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="5.0"))
	float TailLockStickyTime = 1.2f;

protected:
	TWeakObjectPtr<UFlightComponent>   Flight;
	TWeakObjectPtr<UPrimitiveComponent> Body;
	FVector        AimDirSmooth         = FVector::ForwardVector;
	bool           bAimDirInit          = false;
	float          TailLockStickyTimer  = 0.f;
	EDogfightStyle CurrentDogfightStyle = EDogfightStyle::Pursuit;
	float          DogfightStyleTimeLeft = 0.f;
	float          CurrentOrbitSign      = 1.f;
	float          CloseRangeStall       = 0.f;
	float          ExtendStateTimeLeft   = 0.f;
	FVector        ExtendDirWorld        = FVector::ZeroVector;

	void TryBindComponents();
	void UpdateAI(float Dt);
	void TickDogfightStyle(float Dt);
	void SelectNewDogfightStyle();

	AActor* ResolveTarget();
	AActor* FindBestEnemyShip();


	void    ApplyIdleInput();
};
