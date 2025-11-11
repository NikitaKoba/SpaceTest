// FlightComponent.cpp — clean standalone flight (no CSV)
#include "FlightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY(LogFlight);

static float DegToRad(float d){ return d * PI / 180.f; }
static float RadToDeg(float r){ return r * 180.f / PI; }
static float ClampAbs(float v, float m){ return FMath::Clamp(v, -m, m); }
static float ToCmps2(float a){ return a * 100.f; } // м/с^2 → см/с^2
static float Torque_SI_to_UE(float kgm2_alpha){ return kgm2_alpha * 10000.f; } // (кг·м^2 * рад/с^2) → Н·см

UFlightComponent::UFlightComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	bAutoActivate = true;
}
void UFlightComponent::RebindAfterSimToggle()
{
	// Если только что включили симуляцию — привяжем тело.
	// Срываем старую ссылку, чтобы TryBindBody пересканил компоненты.
	if (!Body || !Body->IsSimulatingPhysics())
	{
		Body = nullptr;
		TryBindBody();        // внутренняя, уже есть
		if (Body && Opt.bOverrideDamping && SavedLinearDamping >= 0.f && SavedAngularDamping >= 0.f)
		{
			// восстановим нужный демпфинг, если он у тебя включён
			ApplyDampingFA(true);
		}
	}
}

void UFlightComponent::OnRegister()
{
	Super::OnRegister();
	TryBindBody();

	if (Body)
	{
		CachedMassKg = Body->GetMass();

		if (Opt.bOverrideDamping)
		{
			SavedLinearDamping  = Body->GetLinearDamping();
			SavedAngularDamping = Body->GetAngularDamping();
			ApplyDampingFA(true);
		}

		FVector F,R,U; BuildControlBasisLocal(F,R,U);
		InputSnap.F_loc=F; InputSnap.R_loc=R; InputSnap.U_loc=U;

		UE_LOG(LogFlight, Display, TEXT("[FlightComponent] Registered on %s | Mass=%.1f kg"),
			*GetOwner()->GetName(), CachedMassKg);
	}
	else
	{
		UE_LOG(LogFlight, Warning, TEXT("[FlightComponent] %s: No physics body yet; will retry."), *GetOwner()->GetName());
	}
}

void UFlightComponent::BeginPlay()
{
	Super::BeginPlay();
	TryBindBody();
}

void UFlightComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Body && Opt.bOverrideDamping && SavedLinearDamping >= 0.f && SavedAngularDamping >= 0.f)
	{
		Body->SetLinearDamping(SavedLinearDamping);
		Body->SetAngularDamping(SavedAngularDamping);
	}
	Super::EndPlay(EndPlayReason);
}

void UFlightComponent::SetFlightAssistEnabled(bool bEnable)
{
	const bool bOld = FA_Trans.bEnable;
	FA_Trans.bEnable = bEnable;
	if (GEngine && bOld != bEnable)
	{
		const bool bOff = !FA_Trans.bEnable;
		const FColor C = bOff ? FColor::Orange : FColor::Green;
		GEngine->AddOnScreenDebugMessage((uint64)this + 300, 2.0f, C,
			FString::Printf(TEXT("FLIGHT ASSIST: %s"), bOff ? TEXT("OFF (Inertial)") : TEXT("ON (Stabilized)")));
		UE_LOG(LogFlight, Display, TEXT("[FlightAssist] %s -> %s"),
			*GetOwner()->GetName(), bOff ? TEXT("OFF") : TEXT("ON"));
	}
}

void UFlightComponent::ToggleFlightAssist(){ SetFlightAssistEnabled(!FA_Trans.bEnable); }

// === Input ===
void UFlightComponent::SetThrustForward(float AxisValue){ ThrustForward_Target = FMath::Clamp(AxisValue, -1.f, 1.f); }
void UFlightComponent::SetStrafeRight (float AxisValue){ ThrustRight_Target   = FMath::Clamp(AxisValue,   -1.f, 1.f); }
void UFlightComponent::SetThrustUp    (float AxisValue){ ThrustUp_Target      = FMath::Clamp(AxisValue,   -1.f, 1.f); }
void UFlightComponent::SetRollAxis    (float AxisValue){ RollAxis_Target      = FMath::Clamp(AxisValue,   -1.f, 1.f); }
void UFlightComponent::AddMousePitch  (float MouseY_Delta){ MouseY_Accumulated += MouseY_Delta; }
void UFlightComponent::AddMouseYaw    (float MouseX_Delta){ MouseX_Accumulated += MouseX_Delta; }

void UFlightComponent::ResetInputFilters()
{
	ThrustForward_Smooth=ThrustRight_Smooth=ThrustUp_Smooth=0.f;
	MouseY_Accumulated=MouseX_Accumulated=0.f;
	MouseY_RatePerSec=MouseX_RatePerSec=0.f;
	StickP=StickY=StickP_Sm=StickY_Sm=0.f;
	RollAxis_Smooth=0.f;
	AccumFixed=0.0;
	bPrevVxValid=bPrevVrValid=bPrevVuValid=false;
}

void UFlightComponent::TryBindBody()
{
	if (Body) return;
	AActor* Ow = GetOwner(); if (!Ow) return;

	if (auto* RootPrim = Cast<UPrimitiveComponent>(Ow->GetRootComponent());
		RootPrim && RootPrim->IsSimulatingPhysics())
	{
		Body = RootPrim;
	}
	else
	{
		TArray<UPrimitiveComponent*> Prims; Ow->GetComponents<UPrimitiveComponent>(Prims);
		for (auto* P : Prims) if (P && P->IsSimulatingPhysics()) { Body = P; break; }
	}

	if (!Body)
	{
		if (!GEngine) return;
		GEngine->AddOnScreenDebugMessage((uint64)this + 555, 3.f, FColor::Red,
			FString::Printf(TEXT("FlightComponent: NO PHYS BODY on %s (will retry)"), *Ow->GetName()));
	}
	else
	{
		CachedMassKg = Body->GetMass();
	}
}

void UFlightComponent::ApplyDampingFA(bool bEnable)
{
	if (!Body) return;
	if (bEnable){ Body->SetLinearDamping(Opt.LinearDamping_FA); Body->SetAngularDamping(Opt.AngularDamping_FA); }
	else if (SavedLinearDamping >= 0.f && SavedAngularDamping >= 0.f)
	{ Body->SetLinearDamping(SavedLinearDamping); Body->SetAngularDamping(SavedAngularDamping); }
}

// === Main Tick (GameThread) ===
void UFlightComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// mouse → per-sec rates
	const float frameDt = FMath::Max(0.001f, DeltaTime);
	MouseY_RatePerSec = MouseY_Accumulated / frameDt;
	MouseX_RatePerSec = MouseX_Accumulated / frameDt;
	MouseY_Accumulated = 0.f;
	MouseX_Accumulated = 0.f;

	// build control basis
	FVector F_loc, R_loc, U_loc;
	BuildControlBasisLocal(F_loc, R_loc, U_loc);
	InputSnap.F_loc = F_loc; InputSnap.R_loc = R_loc; InputSnap.U_loc = U_loc;

	// fixed-step input smoothing
	if (Body)
	{
		AccumFixed += DeltaTime;
		int Steps = 0;
		while (AccumFixed + 1e-6f >= Opt.FixedStepSec && Steps < Opt.MaxStepsPerTick)
		{
			AccumFixed -= Opt.FixedStepSec;
			Steps++;

			auto smoothAxis = [this](float Target, float& Smoothed, float RisePerSec, float FallPerSec)
			{
				const float Rise = RisePerSec * Opt.FixedStepSec;
				const float Fall = FallPerSec * Opt.FixedStepSec;
				if (FMath::Abs(Target) > FMath::Abs(Smoothed))
					Smoothed += FMath::Clamp(Target - Smoothed, -Rise, +Rise);
				else
					Smoothed += FMath::Clamp(Target - Smoothed, -Fall, +Fall);
			};

			smoothAxis(ThrustForward_Target, ThrustForward_Smooth, Longi.InputRisePerSec,  Longi.InputFallPerSec);
			smoothAxis(ThrustRight_Target,   ThrustRight_Smooth,  Lateral.InputRisePerSec, Lateral.InputFallPerSec);
			smoothAxis(ThrustUp_Target,      ThrustUp_Smooth,     Vertical.InputRisePerSec,Vertical.InputFallPerSec);
			smoothAxis(RollAxis_Target,      RollAxis_Smooth,     8.f, 10.f);

			// mouse → virtual sticks
			const float Dt = Opt.FixedStepSec;

			// скорости мыши → приращения стиков
			const float My = MouseY_RatePerSec * Dt;
			const float Mx = MouseX_RatePerSec * Dt;

			float sx = (Yaw.bInvertMouse   ? -Mx : Mx);
			float sy = (Pitch.bInvertMouse ? -My : My);

			const float dzPitch = FMath::Clamp(Pitch.MouseDeadzone, 0.f, 0.8f);
			const float dzYaw   = FMath::Clamp(Yaw.MouseDeadzone,   0.f, 0.9f);

			auto shaped = [](float v, float dz, float gamma)->float
			{
				const float r = FMath::Abs(v);
				if (r <= dz) return 0.f;
				const float t    = (r - dz) / FMath::Max(1e-6f, (1.f - dz));
				const float gain = FMath::Pow(t, gamma);
				return (v >= 0.f ? +1.f : -1.f) * gain;
			};

			float vy = shaped(sy, dzPitch, 1.05f);
			float vx = shaped(sx, dzYaw,   1.25f);

			// масштаб «мышь→стик»: yaw чуть резвее
			vy *= 0.075f;
			vx *= 0.100f;

			// Интеграция стиков и «пружина»
			FVector2D s(StickY, StickP);
			s += FVector2D(vx, vy);
			const float sLen = s.Size();
			if (sLen > 1.f) s *= (1.f / sLen);
			StickY = s.X; StickP = s.Y;

			const float springTau   = 0.08f;
			const float springDecay = FMath::Exp(-Dt / FMath::Max(0.001f, springTau));
			StickP *= springDecay; StickY *= springDecay;

			// Сглаживание стиков
			const float aP = 1.f - FMath::Exp(-Dt / FMath::Max(0.001f, Pitch.MouseSmoothing_TimeConst));
			const float aY = 1.f - FMath::Exp(-Dt / FMath::Max(0.001f, Yaw.MouseSmoothing_TimeConst));
			StickP_Sm += aP * (StickP - StickP_Sm);
			StickY_Sm += aY * (StickY - StickY_Sm);

			const float PitchDegps = FMath::Clamp(StickP_Sm, -1.f, 1.f) * Pitch.PitchRateMax_Deg;
			const float YawDegps   = FMath::Clamp(StickY_Sm, -1.f, 1.f) *   Yaw.YawRateMax_Deg;
			InputSnap.PitchRateDes_Rad = DegToRad(PitchDegps);
			InputSnap.YawRateDes_Rad   = DegToRad(YawDegps);
		}
	}

	// желаемые поступательные скорости (м/с)
	InputSnap.VxDes_Mps = ThrustForward_Smooth * Longi.VxMax_Mps;
	InputSnap.VrDes_Mps = ThrustRight_Smooth   * Lateral.VrMax_Mps;
	InputSnap.VuDes_Mps = ThrustUp_Smooth      * Vertical.VuMax_Mps;

	// желаемая угловая скорость крена (рад/с)
	const float Wr_deg = FMath::Clamp(RollAxis_Smooth * 220.f, -220.f, +220.f);
	InputSnap.RollRateDes_Rad = DegToRad(Wr_deg);

	// сброс пер-кадровой jerk-диагностики
	Jerk.ResetPerFrame();

	// очередь на PhysThread
	QueueSubstep();

	// дешёвые экранные строки
}

void UFlightComponent::QueueSubstep()
{
	if (!Opt.bUseChaosSubstep || !Body) return;
	if (!Body->IsSimulatingPhysics())   return;
	FBodyInstance* BI = Body->GetBodyInstance(); if (!BI) return;
	CustomPhysicsDelegate.BindUObject(this, &UFlightComponent::SubstepPhysics);
	BI->AddCustomPhysics(CustomPhysicsDelegate);
}

void UFlightComponent::SubstepPhysics(float Dt, FBodyInstance* BI)
{
	if (!BI || !Body || !Body->IsSimulatingPhysics()) return;

	Jerk.Substeps++;

	const FTransform TM = BI->GetUnrealWorldTransform_AssumesLocked();
	const FVector F_loc = InputSnap.F_loc;
	const FVector R_loc = InputSnap.R_loc;
	const FVector U_loc = InputSnap.U_loc;

	// --- Поступательное ---
	FVector ForceW(0);
	if (FA_Trans.bEnable)
	{
		ComputeTransVectorFA(Dt, TM, F_loc, R_loc, U_loc, BI, ForceW);
	}
	else
	{
		const float Ax = Longi.AxMax_Mps2    * ThrustForward_Smooth;
		const float Ar = Lateral.ArMax_Mps2  * ThrustRight_Smooth;
		const float Au = Vertical.AuMax_Mps2 * ThrustUp_Smooth;

		const FVector A_loc_cmps2 =
			F_loc * ToCmps2(Ax) +
			R_loc * ToCmps2(Ar) +
			U_loc * ToCmps2(Au);

		ForceW = TM.TransformVectorNoScale(A_loc_cmps2 * CachedMassKg);
	}

	// --- Вращение ---
	float AlpR=0.f, AlpP=0.f, AlpY=0.f;
	FVector TauR(0), TauP(0), TauY(0);
	ComputeRoll (Dt, TM, F_loc, BI, AlpR, TauR);
	ComputePitch(Dt, TM, R_loc, BI, AlpP, TauP);
	ComputeYaw  (Dt, TM, U_loc, BI, AlpY, TauY);
	const FVector TorqueW = TauR + TauP + TauY;

	if (!ForceW.IsNearlyZero())  BI->AddForce(ForceW, true, false);
	if (!TorqueW.IsNearlyZero()) BI->AddTorqueInRadians(TorqueW, true, false);

	// jerk-оценка
	const FVector Vw = BI->GetUnrealWorldVelocity_AssumesLocked(); // см/с
	FVector a_meas_mps2 = FVector::ZeroVector;
	if (Jerk.bHavePrev && Dt > KINDA_SMALL_NUMBER)
	{
		const FVector dv_cmps = Vw - Jerk.PrevVel_World;
		a_meas_mps2 = ((dv_cmps / Dt) / 100.f);
		const FVector da = a_meas_mps2 - Jerk.PrevAccel_Mps2;
		Jerk.MaxDeltaAccel_Mps2 = FMath::Max(Jerk.MaxDeltaAccel_Mps2, da.Size());
	}
	const float a_cmd_mag = ForceW.Size() / FMath::Max(1.f, CachedMassKg) / 100.f;
	Jerk.MinDt = FMath::Min(Jerk.MinDt, Dt);
	Jerk.MaxDt = FMath::Max(Jerk.MaxDt, Dt);
	Jerk.MinForceCmd_Mps2 = FMath::Min(Jerk.MinForceCmd_Mps2, a_cmd_mag);
	Jerk.MaxForceCmd_Mps2 = FMath::Max(Jerk.MaxForceCmd_Mps2, a_cmd_mag);
	Jerk.PrevVel_World = Vw;
	Jerk.PrevAccel_Mps2 = a_meas_mps2;
	Jerk.bHavePrev = true;

	if (Opt.bDrawDebug) DrawDebugOnce(TM, F_loc, R_loc, U_loc, ForceW, TorqueW, Dt*1.2f);
}

static FVector AxisSelToVector(EAxisSelector S)
{
	switch (S)
	{
		case EAxisSelector::PlusX:  return FVector( 1, 0, 0);
		case EAxisSelector::MinusX: return FVector(-1, 0, 0);
		case EAxisSelector::PlusY:  return FVector( 0, 1, 0);
		case EAxisSelector::MinusY: return FVector( 0,-1, 0);
		case EAxisSelector::PlusZ:  return FVector( 0, 0, 1);
		default:                    return FVector( 0, 0,-1);
	}
}

FVector UFlightComponent::AxisSelToLocalVector(EAxisSelector S) { return AxisSelToVector(S); }

void UFlightComponent::BuildControlBasisLocal(FVector& F_loc, FVector& R_loc, FVector& U_loc) const
{
	F_loc = AxisSelToLocalVector(Frame.Forward).GetSafeNormal();
	U_loc = AxisSelToLocalVector(Frame.Up).GetSafeNormal();
	// Защита от почти коллинеарных F и U
	if (FMath::Abs(FVector::DotProduct(F_loc, U_loc)) > 0.999f)
		U_loc = (FMath::Abs(F_loc.Z) < 0.9f) ? FVector(0,0,1) : FVector(0,1,0);

	R_loc = FVector::CrossProduct(U_loc, F_loc).GetSafeNormal();
	U_loc = FVector::CrossProduct(F_loc, R_loc).GetSafeNormal();
}

void UFlightComponent::ComputeTransVectorFA(float StepSec, const FTransform& TM,
	                                        const FVector& F_loc, const FVector& R_loc, const FVector& U_loc,
	                                        FBodyInstance* BI, FVector& OutForceW)
{
	OutForceW = FVector::ZeroVector;

	const FVector Vw_cmps = BI->GetUnrealWorldVelocity_AssumesLocked();
	const FVector Vw      = Vw_cmps / 100.f; // м/с

	FVector a_meas = FVector::ZeroVector;
	if (Jerk.bHavePrev && StepSec > KINDA_SMALL_NUMBER)
	{
		a_meas = ((Vw_cmps - Jerk.PrevVel_World) / StepSec) / 100.f;
	}

	const FVector Fw = TM.TransformVectorNoScale(F_loc);
	const FVector Rw = TM.TransformVectorNoScale(R_loc);
	const FVector Uw = TM.TransformVectorNoScale(U_loc);

	const FVector V_des_world = Fw * InputSnap.VxDes_Mps
	                          + Rw * InputSnap.VrDes_Mps
	                          + Uw * InputSnap.VuDes_Mps;

	const FVector Verr = V_des_world - Vw;

	FVector a_cmd = FA_Trans.VelocityPD.Kp * Verr - FA_Trans.VelocityPD.Kd * a_meas;

	float accelCap = FA_Trans.AccelMax_Mps2;
	const float Vmag = Vw.Size();
	if (Vmag > 1e-3f)
	{
		const float cosAng = FVector::DotProduct(Fw, Vw / Vmag);
		if (cosAng < FA_Trans.RetroDotThreshold)
			accelCap *= FA_Trans.RetroBoostMultiplier;
	}

	if (Verr.Size() < FA_Trans.Deadzone_Mps) a_cmd = FVector::ZeroVector;

	const float pdCap = FA_Trans.VelocityPD.OutputAbsMax;
	if (a_cmd.Size() > pdCap)    a_cmd = a_cmd.GetClampedToMaxSize(pdCap);
	if (a_cmd.Size() > accelCap) a_cmd = a_cmd.GetClampedToMaxSize(accelCap);

	OutForceW = a_cmd * CachedMassKg * 100.f; // Н = кг*м/с^2 → Н·см
}

void UFlightComponent::ComputeRoll(float /*StepSec*/, const FTransform& TM, const FVector& F_loc,
								   FBodyInstance* BI, float& OutAlpha_Rad, FVector& OutTorqueW)
{
	OutAlpha_Rad = 0.f; OutTorqueW = FVector::ZeroVector;

	const FVector Ww   = BI->GetUnrealWorldAngularVelocityInRadians_AssumesLocked();
	const FVector Wloc = TM.InverseTransformVectorNoScale(Ww);
	const float   Wr   = FVector::DotProduct(Wloc, F_loc);

	const float  Wdes = InputSnap.RollRateDes_Rad;
	const float  Err  = Wdes - Wr;

	float Kp = 11.0f;
	float Kd = 3.32f;

	const float zeroZoneRad = DegToRad(3.f);
	if (FMath::Abs(Wdes) < zeroZoneRad) { Kp *= 0.40f; }

	float Alpha_cmd = Kp * Err - Kd * Wr;
	Alpha_cmd = ClampAbs(Alpha_cmd, DegToRad(340.f)); // RollAccelMax_Deg
	Alpha_cmd = ClampAbs(Alpha_cmd, 6.0f);            // OutputAbsMax
	OutAlpha_Rad = Alpha_cmd;

	const float Ixx = Opt.InertiaDiag_KgM2.X;
	const float Tau_UE = Torque_SI_to_UE(Ixx * Alpha_cmd);
	const FVector Tau_local = F_loc * Tau_UE;
	OutTorqueW = TM.TransformVectorNoScale(Tau_local);
}

void UFlightComponent::ComputeLongitudinal(float StepSec, const FTransform& TM, const FVector& F_loc, FBodyInstance* BI,
                                           float& OutAx_Mps2, FVector& OutForceW)
{
	OutAx_Mps2 = 0.f; OutForceW = FVector::ZeroVector;

	const FVector Vw   = BI->GetUnrealWorldVelocity_AssumesLocked();
	const FVector Vloc = TM.InverseTransformVectorNoScale(Vw) / 100.f;
	const float   Vx   = FVector::DotProduct(Vloc, F_loc);

	float Ax_est = 0.f;
	if (bPrevVxValid && StepSec > KINDA_SMALL_NUMBER) Ax_est = (Vx - PrevVx_Phys) / StepSec;
	PrevVx_Phys = Vx; bPrevVxValid = true;

	float Ax_cmd = Longi.VelocityPD.Kp * (InputSnap.VxDes_Mps - Vx) - Longi.VelocityPD.Kd * Ax_est;
	Ax_cmd = ClampAbs(Ax_cmd, Longi.AxMax_Mps2);
	Ax_cmd = ClampAbs(Ax_cmd, Longi.VelocityPD.OutputAbsMax);
	OutAx_Mps2 = Ax_cmd;

	const FVector A_loc_cmps2 = F_loc * ToCmps2(Ax_cmd);
	OutForceW = TM.TransformVectorNoScale(A_loc_cmps2 * CachedMassKg);
}

void UFlightComponent::ComputeLateral(float StepSec, const FTransform& TM, const FVector& R_loc, FBodyInstance* BI,
                                      float& OutAr_Mps2, FVector& OutForceW)
{
	OutAr_Mps2 = 0.f; OutForceW = FVector::ZeroVector;

	const FVector Vw   = BI->GetUnrealWorldVelocity_AssumesLocked();
	const FVector Vloc = TM.InverseTransformVectorNoScale(Vw) / 100.f;
	const float   Vr   = FVector::DotProduct(Vloc, R_loc);

	float Ar_est = 0.f;
	if (bPrevVrValid && StepSec > KINDA_SMALL_NUMBER) Ar_est = (Vr - PrevVr_Phys) / StepSec;
	PrevVr_Phys = Vr; bPrevVrValid = true;

	float Ar_cmd = Lateral.VelocityPD.Kp * (InputSnap.VrDes_Mps - Vr) - Lateral.VelocityPD.Kd * Ar_est;
	Ar_cmd = ClampAbs(Ar_cmd, Lateral.ArMax_Mps2);
	Ar_cmd = ClampAbs(Ar_cmd, Lateral.VelocityPD.OutputAbsMax);
	OutAr_Mps2 = Ar_cmd;

	const FVector A_loc_cmps2 = R_loc * ToCmps2(Ar_cmd);
	OutForceW = TM.TransformVectorNoScale(A_loc_cmps2 * CachedMassKg);
}

void UFlightComponent::ComputeVertical(float StepSec, const FTransform& TM, const FVector& U_loc, FBodyInstance* BI,
                                       float& OutAu_Mps2, FVector& OutForceW)
{
	OutAu_Mps2 = 0.f; OutForceW = FVector::ZeroVector;

	const FVector Vw   = BI->GetUnrealWorldVelocity_AssumesLocked();
	const FVector Vloc = TM.InverseTransformVectorNoScale(Vw) / 100.f;
	const float   Vu   = FVector::DotProduct(Vloc, U_loc);

	float Au_est = 0.f;
	if (bPrevVuValid && StepSec > KINDA_SMALL_NUMBER) Au_est = (Vu - PrevVu_Phys) / StepSec;
	PrevVu_Phys = Vu; bPrevVuValid = true;

	float Au_cmd = Vertical.VelocityPD.Kp * (InputSnap.VuDes_Mps - Vu) - Vertical.VelocityPD.Kd * Au_est;
	Au_cmd = ClampAbs(Au_cmd, Vertical.AuMax_Mps2);
	Au_cmd = ClampAbs(Au_cmd, Vertical.VelocityPD.OutputAbsMax);
	OutAu_Mps2 = Au_cmd;

	const FVector A_loc_cmps2 = U_loc * ToCmps2(Au_cmd);
	OutForceW = TM.TransformVectorNoScale(A_loc_cmps2 * CachedMassKg);
}

void UFlightComponent::ComputePitch(float /*StepSec*/, const FTransform& TM, const FVector& R_loc, FBodyInstance* BI,
                                    float& OutAlpha_Rad, FVector& OutTorqueW)
{
	OutAlpha_Rad = 0.f; OutTorqueW = FVector::ZeroVector;

	const FVector Ww   = BI->GetUnrealWorldAngularVelocityInRadians_AssumesLocked();
	const FVector Wloc = TM.InverseTransformVectorNoScale(Ww);
	const float   Wp   = FVector::DotProduct(Wloc, R_loc);
	const float   Err  = InputSnap.PitchRateDes_Rad - Wp;

	const float Kp = Pitch.AngularVelPD.Kp;
	const float Kd = (Pitch.AngularVelPD.Kd > 0.f) ? Pitch.AngularVelPD.Kd : 0.08f;

	float Alpha_cmd = Kp * Err - Kd * Wp;
	Alpha_cmd = ClampAbs(Alpha_cmd, DegToRad(Pitch.PitchAccelMax_Deg));
	Alpha_cmd = ClampAbs(Alpha_cmd, Pitch.AngularVelPD.OutputAbsMax);
	OutAlpha_Rad = Alpha_cmd;

	const float Iyy = Opt.InertiaDiag_KgM2.Y;
	const float Tau_UE = Torque_SI_to_UE(Iyy * Alpha_cmd);
	const FVector Tau_local = R_loc * Tau_UE;
	OutTorqueW = TM.TransformVectorNoScale(Tau_local);
}

void UFlightComponent::ComputeYaw(float /*StepSec*/, const FTransform& TM, const FVector& U_loc, FBodyInstance* BI,
                                  float& OutAlpha_Rad, FVector& OutTorqueW)
{
	OutAlpha_Rad = 0.f; OutTorqueW = FVector::ZeroVector;

	const FVector Ww   = BI->GetUnrealWorldAngularVelocityInRadians_AssumesLocked();
	const FVector Wloc = TM.InverseTransformVectorNoScale(Ww);
	const float   Wy   = FVector::DotProduct(Wloc, U_loc);
	const float   Err  = InputSnap.YawRateDes_Rad - Wy;

	float Alpha_cmd = Yaw.AngularVelPD.Kp * Err - Yaw.AngularVelPD.Kd * Wy;
	Alpha_cmd = ClampAbs(Alpha_cmd, DegToRad(Yaw.YawAccelMax_Deg));
	Alpha_cmd = ClampAbs(Alpha_cmd, Yaw.AngularVelPD.OutputAbsMax);
	OutAlpha_Rad = Alpha_cmd;

	const float Izz = Opt.InertiaDiag_KgM2.Z;
	const float Tau_UE = Torque_SI_to_UE(Izz * Alpha_cmd);
	const FVector Tau_local = U_loc * Tau_UE;
	OutTorqueW = TM.TransformVectorNoScale(Tau_local);
}

void UFlightComponent::DrawDebugOnce(const FTransform& TM, const FVector& F_loc, const FVector& R_loc, const FVector& U_loc,
                                     const FVector& ForceW, const FVector& TorqueW, float LifeSec)
{
	if (!GetWorld()) return;
	const FVector P = TM.GetLocation();

	DrawDebugLine(GetWorld(), P, P + TM.TransformVectorNoScale(F_loc)*200.f, FColor::Red,   false, LifeSec, 0, 2.f);
	DrawDebugLine(GetWorld(), P, P + TM.TransformVectorNoScale(R_loc)*200.f, FColor::Green, false, LifeSec, 0, 2.f);
	DrawDebugLine(GetWorld(), P, P + TM.TransformVectorNoScale(U_loc)*200.f, FColor::Blue,  false, LifeSec, 0, 2.f);

	if (!ForceW.IsNearlyZero())
		DrawDebugDirectionalArrow(GetWorld(), P, P + ForceW.GetClampedToMaxSize(DebugVectorScaleCm), 40.f, FColor::Emerald, false, LifeSec, 0, 3.f);
	if (!TorqueW.IsNearlyZero())
		DrawDebugDirectionalArrow(GetWorld(), P, P + TorqueW.GetClampedToMaxSize(DebugVectorScaleCm), 40.f, FColor::Purple, false, LifeSec, 0, 3.f);
}

void UFlightComponent::PrintScreenTelemetry(float DeltaTime, const FTransform& TM,
	const FVector& F_loc, const FVector& R_loc, const FVector& U_loc)
{
	ScreenTimeAcc += DeltaTime;
	if (!Opt.bScreenSummary || !GEngine || ScreenTimeAcc < 0.5) return;
	ScreenTimeAcc = 0.0;

	const FVector Vw_cmps = Body ? Body->GetComponentVelocity() : FVector::ZeroVector;
	const float   Speed_mps = Vw_cmps.Size() / 100.f;
	const float   Speed_kmh = Speed_mps * 3.6f;

	const FVector Vloc = TM.InverseTransformVectorNoScale(Vw_cmps) / 100.f;
	const float Vx = FVector::DotProduct(Vloc, F_loc);
	const float Vr = FVector::DotProduct(Vloc, R_loc);
	const float Vu = FVector::DotProduct(Vloc, U_loc);

	const FVector Ww = Body ? Body->GetPhysicsAngularVelocityInRadians() : FVector::ZeroVector;
	const FVector Wloc = TM.InverseTransformVectorNoScale(Ww);
	const float Wp_deg = RadToDeg(FVector::DotProduct(Wloc, R_loc));
	const float Wy_deg = RadToDeg(FVector::DotProduct(Wloc, U_loc));
	const float Wr_deg = RadToDeg(FVector::DotProduct(Wloc, F_loc));

	const bool bFA = FA_Trans.bEnable;

	const FString Line1 = FString::Printf(
		TEXT("[Flight] m=%.0f | Fwd=%d Up=%d | |V|=%.1f m/s (%.0f km/h) | FA=%s"),
		CachedMassKg, (int32)Frame.Forward, (int32)Frame.Up,
		Speed_mps, Speed_kmh, bFA ? TEXT("ON") : TEXT("OFF")
	);

	const FString Line2 = FString::Printf(
		TEXT("Vloc: X=%.1f  Y=%.1f  Z=%.1f  m/s  |  ω: P=%.1f  Y=%.1f  R=%.1f  deg/s  | Sub=%d"),
		Vx, Vr, Vu, Wp_deg, Wy_deg, Wr_deg, Jerk.Substeps
	);

	GEngine->AddOnScreenDebugMessage((uint64)this + 1, 0.55f, FColor::Cyan,   Line1);
	GEngine->AddOnScreenDebugMessage((uint64)this + 2, 0.55f, FColor::Silver, Line2);
}
