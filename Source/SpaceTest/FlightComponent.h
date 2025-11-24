// FlightComponent.h  — clean standalone flight (no CSV), UE5.6
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "FlightComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFlight, Log, All);

UENUM(BlueprintType)
enum class EAxisSelector : uint8 { PlusX, MinusX, PlusY, MinusY, PlusZ, MinusZ };

USTRUCT(BlueprintType)
struct FControlFrame
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ControlFrame") EAxisSelector Forward = EAxisSelector::PlusX;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ControlFrame") EAxisSelector Up      = EAxisSelector::PlusZ;
};

USTRUCT(BlueprintType)
struct FPDParams
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PD") float Kp = 1.2f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PD") float Kd = 0.28f;
	// Защита от «перекорма» на выходе PD:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PD", meta=(ClampMin="0")) float OutputAbsMax = 20.f;
};

USTRUCT(BlueprintType)
struct FLongitudinalTuning
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Longitudinal", meta=(ClampMin="0")) float VxMax_Mps = 60.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Longitudinal", meta=(ClampMin="0")) float AxMax_Mps2 = 10.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Longitudinal") FPDParams VelocityPD;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Longitudinal", meta=(ClampMin="0")) float InputRisePerSec = 6.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Longitudinal", meta=(ClampMin="0")) float InputFallPerSec = 8.f;
};

USTRUCT(BlueprintType)
struct FLateralTuning
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lateral", meta=(ClampMin="0")) float VrMax_Mps = 45.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lateral", meta=(ClampMin="0")) float ArMax_Mps2 = 8.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lateral") FPDParams VelocityPD { 1.2f, 0.28f, 20.f };
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lateral", meta=(ClampMin="0")) float InputRisePerSec = 6.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lateral", meta=(ClampMin="0")) float InputFallPerSec = 8.f;
};

USTRUCT(BlueprintType)
struct FVerticalTuning
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Vertical", meta=(ClampMin="0")) float VuMax_Mps = 45.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Vertical", meta=(ClampMin="0")) float AuMax_Mps2 = 8.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Vertical") FPDParams VelocityPD { 1.2f, 0.28f, 20.f };
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Vertical", meta=(ClampMin="0")) float InputRisePerSec = 6.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Vertical", meta=(ClampMin="0")) float InputFallPerSec = 8.f;
};

USTRUCT(BlueprintType)
struct FPitchTuning
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pitch", meta=(ClampMin="0")) float PitchRateMax_Deg  = 60.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pitch", meta=(ClampMin="0")) float PitchAccelMax_Deg = 120.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pitch") FPDParams AngularVelPD { 2.0f, 0.32f, 6.0f };

	// Внутренний конвертер «дельта мыши → требуемая скорость поворота»
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pitch", meta=(ClampMin="0")) float MouseSmoothing_TimeConst = 0.12f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pitch") bool  bInvertMouse = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pitch", meta=(ClampMin="0", ClampMax="0.8")) float MouseDeadzone = 0.10f;
};

USTRUCT(BlueprintType)
struct FYawTuning
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Yaw", meta=(ClampMin="0")) float YawRateMax_Deg  = 80.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Yaw", meta=(ClampMin="0")) float YawAccelMax_Deg = 160.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Yaw") FPDParams AngularVelPD { 2.0f, 0.32f, 6.0f };

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Yaw", meta=(ClampMin="0")) float MouseSmoothing_TimeConst = 0.12f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Yaw") bool  bInvertMouse = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Yaw", meta=(ClampMin="0", ClampMax="0.9")) float MouseDeadzone = 0.50f;
};

USTRUCT(BlueprintType)
struct FFlightOptions
{
	GENERATED_BODY()
	// Включить Chaos Custom Physics субстеп
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Time") bool  bUseChaosSubstep = true;
	// Фиксированный шаг для сглаживания инпутов и mouse→stick преобразования (GameThread):
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Time", meta=(ClampMin="0.001", ClampMax="0.0333")) float FixedStepSec = 1.f/120.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Time", meta=(ClampMin="1", ClampMax="8")) int32 MaxStepsPerTick = 4;

	// Подменить демпфирование на время FA для «неинерциального» поведения
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics") bool  bOverrideDamping = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics", meta=(EditCondition="bOverrideDamping", ClampMin="0", ClampMax="1")) float LinearDamping_FA  = 0.02f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics", meta=(EditCondition="bOverrideDamping", ClampMin="0", ClampMax="1")) float AngularDamping_FA = 0.02f;

	// Диагональ аппроксимированной матрицы инерции (кг·м^2) для расчёта требуемых моментов
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics", meta=(ClampMin="1")) FVector InertiaDiag_KgM2 = FVector(1.6e6f, 2.0e6f, 1.8e6f);

	// Простые экранные подсказки (скорость/ω)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug") bool bScreenSummary = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug") bool bDrawDebug     = false;
};

USTRUCT(BlueprintType)
struct FTransAssist
{
	GENERATED_BODY()
	// Flight Assist для поступательного движения: регулирование скорости PD-контроллером
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FA") bool  bEnable = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FA") FPDParams VelocityPD { 2.0f, 0.25f, 50.f };
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FA", meta=(ClampMin="0")) float AccelMax_Mps2 = 30.f;
	// Усиление ретро-тяги, если хотим разгоняться в противоположную движению сторону
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FA", meta=(ClampMin="1")) float RetroBoostMultiplier = 2.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FA", meta=(ClampMin="-1", ClampMax="1")) float RetroDotThreshold = -0.25f;
	// Мёртвая зона на ошибку скорости
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FA", meta=(ClampMin="0")) float Deadzone_Mps = 0.05f;
};

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UFlightComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFlightComponent();
	UFUNCTION(BlueprintCallable, Category="Flight|AI")
	void SetAngularRateOverride(
		bool  bEnable,
		float PitchRateDegPerSec,
		float YawRateDegPerSec,
		float RollRateDegPerSec
	);
	// === Публичный инпут ===
	UFUNCTION(BlueprintCallable, Category="Flight|Input") void SetThrustForward(float AxisValue);
	UFUNCTION(BlueprintCallable, Category="Flight|Input") void SetStrafeRight (float AxisValue);
	UFUNCTION(BlueprintCallable, Category="Flight|Input") void SetThrustUp    (float AxisValue);
	UFUNCTION(BlueprintCallable, Category="Flight|Input") void SetRollAxis    (float AxisValue);
	UFUNCTION(BlueprintCallable, Category="Flight|Input") void AddMousePitch  (float MouseY_Delta);
	UFUNCTION(BlueprintCallable, Category="Flight|Input") void AddMouseYaw    (float MouseX_Delta);
	UFUNCTION(BlueprintCallable, Category="Flight|Input") void ResetInputFilters();
	// Hard reset of cached inputs/derivatives (used on hyper-exit snap).
	void ResetDynamicsState();

	UFUNCTION(BlueprintCallable, Category="Flight|Assist") void SetFlightAssistEnabled(bool bEnable);
	UFUNCTION(BlueprintCallable, Category="Flight|Assist") void ToggleFlightAssist();
	UFUNCTION(BlueprintPure,   Category="Flight|Assist") bool  IsFlightAssistEnabled() const { return FA_Trans.bEnable; }

	UPrimitiveComponent* GetBodyComponent() const { return Body; }

	// Тюнинг
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Tuning")  FLongitudinalTuning Longi;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Tuning")  FLateralTuning      Lateral;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Tuning")  FVerticalTuning     Vertical;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Tuning")  FPitchTuning        Pitch;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Tuning")  FYawTuning          Yaw;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Options") FFlightOptions      Opt;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|ControlFrame") FControlFrame   Frame;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Tuning")  FTransAssist        FA_Trans;

	// Визуал-отладка
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Debug") float DebugVectorScaleCm = 220.f;
	UFUNCTION()
	void RebindAfterSimToggle();
protected:
	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	// === Физика ===
	UPROPERTY() UPrimitiveComponent* Body = nullptr;
	float CachedMassKg = 0.f;
	float SavedLinearDamping  = -1.f;
	float SavedAngularDamping = -1.f;

	// Снимок команд в PhysThread
	struct FInputSnapshot
	{
		float VxDes_Mps = 0.f, VrDes_Mps = 0.f, VuDes_Mps = 0.f;
		float PitchRateDes_Rad = 0.f, YawRateDes_Rad = 0.f, RollRateDes_Rad = 0.f;
		FVector F_loc = FVector(1,0,0), R_loc = FVector(0,1,0), U_loc = FVector(0,0,1);
	} InputSnap;

	// --- AI override угловых скоростей ---
	bool  bHasAngularOverride   = false;
	float OverridePitchRate_Rad = 0.f;
	float OverrideYawRate_Rad   = 0.f;
	float OverrideRollRate_Rad  = 0.f;

	// Состояния для D-термов и jerk-диагностики
	struct FJerkStats
	{
		int32  Substeps = 0;
		float  MinDt = FLT_MAX, MaxDt = 0.f;
		bool   bHavePrev = false;
		FVector PrevVel_World = FVector::ZeroVector; // см/с (UE)
		FVector PrevAccel_Mps2 = FVector::ZeroVector;
		float  MaxDeltaAccel_Mps2 = 0.f;
		float  MinForceCmd_Mps2 = FLT_MAX, MaxForceCmd_Mps2 = 0.f;

		void ResetPerFrame()
		{
			Substeps = 0; MinDt = FLT_MAX; MaxDt = 0.f; bHavePrev = false;
			PrevVel_World = FVector::ZeroVector; PrevAccel_Mps2 = FVector::ZeroVector;
			MaxDeltaAccel_Mps2 = 0.f; MinForceCmd_Mps2 = FLT_MAX; MaxForceCmd_Mps2 = 0.f;
		}
	} Jerk;

	// D-оценки локальных скоростей (для поступательных PD)
	float PrevVx_Phys = 0.f; bool bPrevVxValid = false;
	float PrevVr_Phys = 0.f; bool bPrevVrValid = false;
	float PrevVu_Phys = 0.f; bool bPrevVuValid = false;

	// Субстеп делегат
	FCalculateCustomPhysics CustomPhysicsDelegate;

	// Сглаживание инпутов
	double AccumFixed = 0.0;

	// Внутренние хелперы
	void TryBindBody();
	void ApplyDampingFA(bool bEnable);

	void QueueSubstep();
	void SubstepPhysics(float Dt, FBodyInstance* BI);

	static FVector AxisSelToLocalVector(EAxisSelector S);
	void BuildControlBasisLocal(FVector& F_loc, FVector& R_loc, FVector& U_loc) const;

	// Поступательное и вращательное управление (PhysThread)
	void ComputeTransVectorFA(float StepSec, const FTransform& TM,
	                          const FVector& F_loc, const FVector& R_loc, const FVector& U_loc,
	                          FBodyInstance* BI, FVector& OutForceW);

	void ComputeLongitudinal(float StepSec, const FTransform& TM, const FVector& F_loc, FBodyInstance* BI,
	                         float& OutAx_Mps2, FVector& OutForceW);
	void ComputeLateral     (float StepSec, const FTransform& TM, const FVector& R_loc, FBodyInstance* BI,
	                         float& OutAr_Mps2, FVector& OutForceW);
	void ComputeVertical    (float StepSec, const FTransform& TM, const FVector& U_loc, FBodyInstance* BI,
	                         float& OutAu_Mps2, FVector& OutForceW);

	void ComputePitch(float StepSec, const FTransform& TM, const FVector& R_loc, FBodyInstance* BI,
	                  float& OutAlpha_Rad, FVector& OutTorqueW);
	void ComputeYaw  (float StepSec, const FTransform& TM, const FVector& U_loc, FBodyInstance* BI,
	                  float& OutAlpha_Rad, FVector& OutTorqueW);
	void ComputeRoll (float StepSec, const FTransform& TM, const FVector& F_loc, FBodyInstance* BI,
	                  float& OutAlpha_Rad, FVector& OutTorqueW);

	// Экран/дебаг
	void DrawDebugOnce(const FTransform& TM, const FVector& F_loc, const FVector& R_loc, const FVector& U_loc,
	                   const FVector& ForceW, const FVector& TorqueW, float LifeSec);
	void PrintScreenTelemetry(float DeltaTime, const FTransform& TM,
	                          const FVector& F_loc, const FVector& R_loc, const FVector& U_loc);

	// === ПЕРЕКАЧКА ИНПУТА (игровой поток) ===
public:
	// накопленные за кадр дельты мыши → в TickComponent пересчёт в скорость
	float MouseY_Accumulated = 0.f, MouseX_Accumulated = 0.f;
	float MouseY_RatePerSec = 0.f, MouseX_RatePerSec = 0.f;

	// виртуальные стики (−1..+1), после формы и сглаживания
	float StickP = 0.f, StickY = 0.f;
	float StickP_Sm = 0.f, StickY_Sm = 0.f;

	// сглаженные оси тяги
	float ThrustForward_Target = 0.f, ThrustForward_Smooth = 0.f;
	float ThrustRight_Target   = 0.f, ThrustRight_Smooth   = 0.f;
	float ThrustUp_Target      = 0.f, ThrustUp_Smooth      = 0.f;
	float RollAxis_Target      = 0.f, RollAxis_Smooth      = 0.f;

	// лимит изменения команд угл.скоростей (на будущее)
	float Pitch_RateSlew_DegPerSec2 = 900.f;
	float Yaw_RateSlew_DegPerSec2   = 1000.f;

	// экранная периодика
	double ScreenTimeAcc = 0.0;
};
