// ShipNetComponent.cpp
#include "ShipNetComponent.h"
#include "ShipPawn.h"
#include "FlightComponent.h"
#include "SpaceFloatingOriginSubsystem.h"

#include "Components/StaticMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY(LogShipNet);
// --- Quant helpers (Р±РµР· Р·Р°РІРёСЃРёРјРѕСЃС‚РµР№) ---
static FORCEINLINE float QuantStep(float v, float step)
{
	// СЃРёРјРјРµС‚СЂРёС‡РЅР°СЏ РєРІР°РЅС‚РѕРІРєР° (РІ С‚.С‡. РґР»СЏ РѕС‚СЂРёС†Р°С‚РµР»СЊРЅС‹С…), Р±РµР· РґСЂРµР№С„Р°
	return step * FMath::RoundToFloat(v / step);
}

static FORCEINLINE FVector QuantVec(const FVector& v, float step)
{
	return FVector(QuantStep(v.X, step), QuantStep(v.Y, step), QuantStep(v.Z, step));
}

static FORCEINLINE FRotator QuantRotDeg(const FRotator& r, float degStep)
{
	// РЅРѕСЂРјР°Р»РёР·СѓРµРј РІ [-180,180), РєРІР°РЅС‚РёРј, СЃРЅРѕРІР° РЅРѕСЂРјР°Р»РёР·СѓРµРј
	FRotator n = r.GetNormalized();
	n.Pitch = QuantStep(n.Pitch, degStep);
	n.Yaw   = QuantStep(n.Yaw,   degStep);
	n.Roll  = QuantStep(n.Roll,  degStep);
	return n.GetNormalized();
}

UShipNetComponent::UShipNetComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup    = TG_PostPhysics;
	SetIsReplicatedByDefault(true);
}

void UShipNetComponent::BeginPlay()
{
	Super::BeginPlay();

	OwPawn = Cast<APawn>(GetOwner());
	Ship   = Cast<AShipPawn>(GetOwner());

	if (Ship)
	{
		ShipMesh = Ship->ShipMesh;
		Flight   = Ship->Flight;
	}
	if (UWorld* World = GetWorld())
	{
		if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
		{
			PrevOriginGlobal = FO->GetOriginGlobal();
			bHavePrevOrigin = true;
		}
	}
	SetupTickOrder();
	UpdatePhysicsSimState();
}
void UShipNetComponent::HandleFloatingOriginShift()
{
	if (!OwPawn) return;
	UWorld* World = OwPawn->GetWorld();
	USpaceFloatingOriginSubsystem* FO = World ? World->GetSubsystem<USpaceFloatingOriginSubsystem>() : nullptr;
	if (!FO) return;

	const FVector3d CurOrigin = FO->GetOriginGlobal();
	if (!bHavePrevOrigin)
	{
		PrevOriginGlobal = CurOrigin;
		bHavePrevOrigin = true;
		return;
	}

	const FVector3d DeltaOrigin = CurOrigin - PrevOriginGlobal;
	if (DeltaOrigin.IsNearlyZero()) return;

	// РњРёСЂ СЃРґРІРёРЅСѓР»СЃСЏ РЅР° -DeltaOrigin РІ UU
	const FVector ShiftWorld(
		static_cast<float>(-DeltaOrigin.X),
		static_cast<float>(-DeltaOrigin.Y),
		static_cast<float>(-DeltaOrigin.Z));

	for (FInterpNode& N : NetBuffer)
	{
		N.Loc += ShiftWorld;
	}

	// РЎРґРІРёРЅСѓС‚СЊ С†РµР»СЊ СЂРµРєРѕРЅСЃРёР»СЏС†РёРё, С‡С‚РѕР±С‹ РЅРµ РїРѕР»СѓС‡РёС‚СЊ Р»РѕР¶РЅС‹Р№ С‚РµР»РµРїРѕСЂС‚
	OwnerReconTarget.Loc += ShiftWorld;

	PrevOriginGlobal = CurOrigin;
}

void UShipNetComponent::SetupTickOrder()
{
	// РІР°Р¶РЅРѕ: СЃРЅР°С‡Р°Р»Р° С„РёР·РёРєР°/Flight, РїРѕС‚РѕРј вЂ” РјС‹
	if (Flight)
	{
		AddTickPrerequisiteComponent(Flight);
	}
	if (ShipMesh)
	{
		AddTickPrerequisiteComponent(ShipMesh);
	}
}

void UShipNetComponent::SetLocalAxes(float F, float R, float U, float Roll)
{
	AxisF_Cur = F; AxisR_Cur = R; AxisU_Cur = U; AxisRoll_Cur = Roll;
}

void UShipNetComponent::AddMouseDelta(float Dx, float Dy)
{
	MouseX_Accum += Dx;
	MouseY_Accum += Dy;
}

void UShipNetComponent::OnHyperDriveExited()
{
	if (!ShipMesh)
	{
		return;
	}

	ShipMesh->SetPhysicsLinearVelocity(FVector::ZeroVector, false);
	ShipMesh->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector, false);

	NetBuffer.Reset();

	FInterpNode N;
	N.Time   = GetWorld() ? (double)GetWorld()->GetTimeSeconds() : 0.0;
	N.Loc    = ShipMesh->GetComponentLocation();
	N.Rot    = ShipMesh->GetComponentQuat();
	N.Vel    = FVector::ZeroVector;
	N.AngVel = FVector::ZeroVector;
	NetBuffer.Add(N);

	PendingInputs.Reset();
	bHaveOwnerRecon = false;
}

void UShipNetComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!OwPawn || !ShipMesh) return;
	HandleFloatingOriginShift();
	UpdatePhysicsSimState();

	// === 1) Р’Р»Р°РґРµР»РµС† в†’ СЃРµСЂРІРµСЂ: RPC СЃ РёРЅРїСѓС‚РѕРј + Р»РѕРєР°Р»СЊРЅРѕРµ РєСЌС€РёСЂРѕРІР°РЅРёРµ ===
	if (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole() == ROLE_AutonomousProxy)
	{
		FControlState St;
		St.Seq       = ++LocalInputSeq;
		St.DeltaTime = DeltaTime;
		St.ThrustF   = AxisF_Cur;
		St.ThrustR   = AxisR_Cur;
		St.ThrustU   = AxisU_Cur;
		St.Roll      = AxisRoll_Cur;
		St.MouseX    = MouseX_Accum;
		St.MouseY    = MouseY_Accum;

		Server_SendInput(St);

		// Р—Р°РїРѕРјРЅРёРј РґР»СЏ СЂРµСЃРёРјР° (ACK СЃРїРёР»РёС‚ С…РІРѕСЃС‚)
		PendingInputs.Add(St);

		MouseX_Accum = 0.f;
		MouseY_Accum = 0.f;

		// [PREDICT] С‚СѓС‚ РёРґРµР°Р»СЊРЅС‹Р№ РјРѕРјРµРЅС‚ РІС‹РїРѕР»РЅРёС‚СЊ Р»РѕРєР°Р»СЊРЅС‹Р№ РґРµС‚РµСЂРјРёРЅРёСЂРѕРІР°РЅРЅС‹Р№ SimStep
		// РµСЃР»Рё UFlightComponent РїСЂРµРґРѕСЃС‚Р°РІРёС‚ С‡РёСЃС‚СѓСЋ С„СѓРЅРєС†РёСЋ РїСЂРµРґСЃРєР°Р·Р°РЅРёСЏ (Р±РµР· РїРѕР±РѕС‡РµРє).
		// РџРѕРєР° РѕСЃС‚Р°РІР»СЏРµРј РіРёР±СЂРёРґРЅСѓСЋ РјРѕРґРµР»СЊ (С„РёР·РёРєР° СѓР¶Рµ Рє СЌС‚РѕРјСѓ С‚РёРє-РєР°РґСЂСѓ РїСЂРёРјРµРЅРµРЅР° FlightвЂ™РѕРј).
	}

	// === 2) РќР°Р±Р»СЋРґР°С‚РµР»Рё: РёРЅС‚РµСЂРїРѕР»СЏС†РёСЏ ===
	if (OwPawn->GetLocalRole() == ROLE_SimulatedProxy)
	{
		DriveSimulatedProxy();
	}

	// === 3) Р’Р»Р°РґРµР»РµС†: РјСЏРіРєР°СЏ СЂРµРєРѕРЅСЃРёР»СЏС†РёСЏ (РїРѕРІРµСЂС… Р»РѕРєР°Р»СЊРЅРѕР№ С„РёР·РёРєРё) ===
	if (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole() == ROLE_AutonomousProxy)
	{
		OwnerReconcile_Tick(DeltaTime);
	}

	// === 4) РЎРµСЂРІРµСЂ: СЃРЅРёРјР°РµРј Р°РІС‚РѕСЂРёС‚Р°С‚РёРІРЅС‹Р№ СЃРЅР°Рї ===
	if (OwPawn->HasAuthority())
	{
		// РџРѕРґР±РёСЂР°РµРј РјРёРЅРёРјР°Р»СЊРЅС‹Рµ В«СЃС‚СѓРїРµРЅСЊРєРёВ», РєРѕС‚РѕСЂС‹С… РґРѕСЃС‚Р°С‚РѕС‡РЅРѕ, С‡С‚РѕР±С‹ СЃРєСЂС‹С‚СЊ С€СѓРј,
		// РЅРѕ РЅРµ СЃСЉРµСЃС‚СЊ СѓРїСЂР°РІР»СЏРµРјРѕСЃС‚СЊ. Р’СЃС‘ РІ СЃРј/РіСЂР°РґСѓСЃР°С…, РєР°Рє Сѓ С‚РµР±СЏ.
		// Р•СЃР»Рё РЅСѓР¶РЅРѕ РµС‰С‘ РјСЏРіС‡Рµ вЂ” СѓРјРµРЅСЊС€Рё С€Р°РіРё РІРґРІРѕРµ.
		constexpr float POS_STEP_CM   = 1.0f;   // РїРѕР·РёС†РёСЏ: 1 СЃРј
		constexpr float VEL_STEP_CMPS = 1.0f;   // Р»РёРЅ. СЃРєРѕСЂРѕСЃС‚СЊ: 1 СЃРј/СЃ
		constexpr float ANG_STEP_DEGPS= 0.1f;
		constexpr float ROT_STEP_DEG  = 0.1f;   // РѕСЂРёРµРЅС‚Р°С†РёСЏ (СЌР№Р»РµСЂС‹): 0.5 В°

		const FTransform X   = ShipMesh->GetComponentTransform();
		const FVector    V   = ShipMesh->GetComponentVelocity();
		const FVector    Wrd = ShipMesh->GetPhysicsAngularVelocityInRadians();

		// РљРІР°РЅС‚СѓРµРј РІСЃС‘ РїРµСЂРµРґ Р·Р°РїРёСЃСЊСЋ РІ СЃРЅР°Рї:
		const FVector   LocQ    = QuantVec(X.GetLocation(), POS_STEP_CM);
		const FRotator  RotQdeg = QuantRotDeg(X.Rotator(), ROT_STEP_DEG);
		const FVector   VelQ    = QuantVec(V, VEL_STEP_CMPS);
		const FVector   AngQdeg = QuantVec(Wrd * (180.f / PI), ANG_STEP_DEGPS);

		ServerSnap.Loc        = LocQ;
		ServerSnap.RotCS.FromRotator(RotQdeg);
		ServerSnap.Vel        = VelQ;
		ServerSnap.AngVelDeg  = AngQdeg;
		ServerSnap.ServerTime = GetWorld()->GetTimeSeconds();
		if (UWorld* World = GetWorld())
		{
			if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
			{
				ServerSnap.OriginGlobalM = FVector(FO->GetOriginGlobal());
				ServerSnap.WorldOriginUU = FVector(FO->GetWorldOriginUU());
			}
		}
		// LastAckSeq СЃРµСЂРІРµСЂ РѕР±РЅРѕРІР»СЏРµС‚ РІ Server_SendInput()
	}
}

// ShipNetComponent.cpp
void UShipNetComponent::UpdatePhysicsSimState()
{
	if (!Ship || !ShipMesh || !Flight || !OwPawn) return;

	const bool bOwner = (OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole()==ROLE_AutonomousProxy);
	const bool bAuth  = OwPawn->HasAuthority();
	const bool bShouldSim = bAuth || bOwner;

	const bool bWasSim = ShipMesh->IsSimulatingPhysics();
	if (bWasSim != bShouldSim)
	{
		if (bShouldSim)
		{
			// РЎРёРЅС…СЂРѕРЅРёР·РёСЂСѓРµРј С‚СЂР°РЅСЃС„РѕСЂРј Рё РІРєР»СЋС‡Р°РµРј С„РёР·РёРєСѓ
			ShipMesh->SetWorldTransform(Ship->GetActorTransform(), false, nullptr, ETeleportType::TeleportPhysics);
			ShipMesh->SetSimulatePhysics(true);
			ShipMesh->WakeAllRigidBodies();

			// РљР РРўРР§Р•РЎРљРћ: СЃСЂР°Р·Сѓ СЂРµР±Р°Р№РЅРґРёРј С‚РµР»Рѕ РґР»СЏ Flight
			Flight->RebindAfterSimToggle();
		}
		else
		{
			// РђРєРєСѓСЂР°С‚РЅРѕ РіР°СЃРёРј Рё РІС‹РєР»СЋС‡Р°РµРј
			ShipMesh->SetPhysicsLinearVelocity(FVector::ZeroVector, false);
			ShipMesh->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector, false);
			ShipMesh->PutAllRigidBodiesToSleep();
			ShipMesh->SetSimulatePhysics(false);
		}
	}

	// РўРёРє Flight С‚РѕР»СЊРєРѕ С‚Р°Рј, РіРґРµ СЃРёРјСѓР»РёСЂСѓРµРј
	Flight->SetComponentTickEnabled(bShouldSim);
}


// ===== RPC =====
void UShipNetComponent::Server_SendInput_Implementation(const FControlState& State)
{
	// ACK вЂ” СЃРµСЂРІРµСЂ СЃРѕРѕР±С‰Р°РµС‚, С‡С‚Рѕ РѕР±СЂР°Р±РѕС‚Р°Р» СЌС‚РѕС‚ seq
	ServerSnap.LastAckSeq = State.Seq;

	// РЎР°РЅРёС‚Рё/РєР»Р°РјРїС‹ Р·РґРµСЃСЊ (Р·Р°С‰РёС‚Р° РѕС‚ В«СЃРІРµСЂС…В»-РёРЅРїСѓС‚РѕРІ) вЂ” РѕРїСѓСЃС‚РёР» СЂР°РґРё РєСЂР°С‚РєРѕСЃС‚Рё

	// РџСЂРѕРєР°С‚С‹РІР°РµРј public API, РєР°Рє Р±С‹Р»Рѕ
	if (Flight)
	{
		Flight->SetThrustForward(State.ThrustF);
		Flight->SetStrafeRight (State.ThrustR);
		Flight->SetThrustUp    (State.ThrustU);
		Flight->SetRollAxis    (State.Roll);
		Flight->AddMouseYaw    (State.MouseX);
		Flight->AddMousePitch  (State.MouseY);
	}
}

// ===== OnRep (СѓРЅРёРІРµСЂСЃР°Р»СЊРЅРѕ: Рё owner, Рё СЃРёРј-РїСЂРѕРєСЃРё) =====
void UShipNetComponent::OnRep_ServerSnap()
{
    HandleFloatingOriginShift();

    const double Now = GetWorld() ? (double)GetWorld()->GetTimeSeconds() : 0.0;

    // Временная подстройка смещения времени (EMA) + выборка для адаптивного delay
    {
        const double EstOffset = Now - (double)ServerSnap.ServerTime;
        if (!bHaveTimeSync)
        {
            ServerTimeToClientTime = EstOffset;
            bHaveTimeSync = true;
        }
        else
        {
            ServerTimeToClientTime = FMath::Lerp(ServerTimeToClientTime, EstOffset, 0.10);
        }

        const double Residual = EstOffset - ServerTimeToClientTime;
        AdaptiveInterpDelay_OnSample(Residual);
    }

    const FVector3d SnapOrigin    = FVector3d(ServerSnap.OriginGlobalM);
    const FVector3d SnapWorldOrig = FVector3d(ServerSnap.WorldOriginUU);

    // Подгоняем локальный буфер под origin из снапа (не зависит от порядка репликации)
    if (!bHavePrevOrigin)
    {
        PrevOriginGlobal = SnapOrigin;
        bHavePrevOrigin  = true;
    }
    else
    {
        const FVector3d DeltaOrigin = SnapOrigin - PrevOriginGlobal;
        if (!DeltaOrigin.IsNearlyZero())
        {
            const FVector ShiftWorld(
                static_cast<float>(-DeltaOrigin.X),
                static_cast<float>(-DeltaOrigin.Y),
                static_cast<float>(-DeltaOrigin.Z));

            for (FInterpNode& N : NetBuffer)
            {
                N.Loc += ShiftWorld;
            }
            OwnerReconTarget.Loc += ShiftWorld;
            PrevOriginGlobal = SnapOrigin;
        }
    }

    // Сим-прокси: интерполяция + телепорт-детектор
    if (OwPawn && OwPawn->GetLocalRole() == ROLE_SimulatedProxy)
    {
        const double ClientTime = (double)ServerSnap.ServerTime + ServerTimeToClientTime;
        const float TeleportCutoffUU = 100000.f;  // 1 км
        bool bDidTeleport = false;

        if (NetBuffer.Num() > 0 && Ship)
        {
            const FInterpNode& Last = NetBuffer.Last();

            const FVector3d LastGlobal = SnapOrigin + (FVector3d(Last.Loc) - SnapWorldOrig);
            const FVector3d SnapGlobal = SnapOrigin + (FVector3d(ServerSnap.Loc) - SnapWorldOrig);

            const double JumpGlobal = FVector3d::Distance(SnapGlobal, LastGlobal);

            if (JumpGlobal > (double)TeleportCutoffUU)
            {
                UE_LOG(LogShipNet, Warning,
                    TEXT("[TELEPORT DETECTED] %s | JumpGlobal=%.0f m | LastGlobal=%s | SnapGlobal=%s"),
                    *GetNameSafe(Ship),
                    JumpGlobal / 100.0,
                    *LastGlobal.ToString(),
                    *SnapGlobal.ToString());

                NetBuffer.Reset();

                FInterpNode N;
                N.Time   = ClientTime;
                N.Loc    = ServerSnap.Loc;
                N.Rot    = ServerSnap.RotCS.ToRotator().Quaternion();
                N.Vel    = ServerSnap.Vel;
                N.AngVel = (ServerSnap.AngVelDeg) * (PI / 180.f);
                NetBuffer.Add(N);

                Ship->SetActorLocationAndRotation(
                    N.Loc, N.Rot.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);

                bDidTeleport = true;
            }
            else if (JumpGlobal > 50000.0)  // >500 м - логируем, но не телепортим
            {
                UE_LOG(LogShipNet, Verbose,
                    TEXT("[BIG JUMP] %s | JumpGlobal=%.0f m (below teleport threshold)"),
                    *GetNameSafe(Ship),
                    JumpGlobal / 100.0);
            }
        }

        if (!bDidTeleport)
        {
            FInterpNode N;
            N.Time   = ClientTime;
            N.Loc    = ServerSnap.Loc;
            N.Rot    = ServerSnap.RotCS.ToRotator().Quaternion();
            N.Vel    = ServerSnap.Vel;
            N.AngVel = (ServerSnap.AngVelDeg) * (PI / 180.f);
            NetBuffer.Add(N);

            const double KeepFrom = Now - 1.0;
            int32 FirstValid = 0;
            while (FirstValid < NetBuffer.Num() && NetBuffer[FirstValid].Time < KeepFrom) ++FirstValid;
            if (FirstValid > 0)
                NetBuffer.RemoveAt(0, FirstValid, EAllowShrinking::No);
        }
    }
    // Владелец: ACK + целевой снап для рекона
    else if (OwPawn && OwPawn->IsLocallyControlled() && OwPawn->GetLocalRole() == ROLE_AutonomousProxy)
    {
        OwnerReconTarget = ServerSnap;
        bHaveOwnerRecon  = true;

        int32 CutIdx = INDEX_NONE;
        for (int32 i = PendingInputs.Num()-1; i >= 0; --i)
        {
            if (PendingInputs[i].Seq <= ServerSnap.LastAckSeq)
            {
                CutIdx = i; break;
            }
        }
        if (CutIdx >= 0)
        {
            PendingInputs.RemoveAt(0, CutIdx+1, EAllowShrinking::No);
        }
    }
}

// === РђРґР°РїС‚РёРІРЅР°СЏ РЅР°СЃС‚СЂРѕР№РєР° РёРЅС‚РµСЂРї-Р·Р°РґРµСЂР¶РєРё РїРѕ РґР¶РёС‚С‚РµСЂСѓ (РјРµРґРёР°РЅР° + 0.5 * IQR90) ===
void UShipNetComponent::AdaptiveInterpDelay_OnSample(double OffsetSample)
{
	DelaySamples[DelaySamplesHead] = OffsetSample;
	DelaySamplesHead = (DelaySamplesHead + 1) % DelaySamples.Num();
	DelaySamplesCount = FMath::Min(DelaySamplesCount + 1, DelaySamples.Num());

	if (DelaySamplesCount < 16) return; // РїСЂРѕРіСЂРµРІ

	// РєРѕРїРёСЏ РѕРєРЅР° Рё СЃРѕСЂС‚
	TArray<double> W;
	W.Reserve(DelaySamplesCount);
	for (int32 i=0; i<DelaySamplesCount; ++i)
	{
		W.Add(DelaySamples[i]);
	}
	W.Sort();

	const auto Pct = [&](double p)->double {
		if (W.Num()==0) return 0.0;
		const double idx = FMath::Clamp(p * (W.Num()-1), 0.0, double(W.Num()-1));
		const int32 i0 = int32(FMath::FloorToDouble(idx));
		const int32 i1 = FMath::Min(i0+1, W.Num()-1);
		const double t = idx - i0;
		return FMath::Lerp(W[i0], W[i1], t);
	};

	const double Med  = Pct(0.50);
	const double P90  = Pct(0.90);
	const double NewD = FMath::Clamp(Med + 0.5*(P90 - Med), (double)NetInterpDelayMin, (double)NetInterpDelayMax);

	// СЃРіР»Р°РґРёРј
	NetInterpDelay = FMath::Lerp(NetInterpDelay, (float)NewD, 0.15f);
}

// === РЎРёРј-РїСЂРѕРєСЃРё: РёРЅС‚РµСЂРїРѕР»СЏС†РёСЏ Hermite + РёРЅС‚РµРіСЂР°Р» РѕСЂРёРµРЅС‚Р°С†РёРё РїРѕ AngVel ===
FQuat UShipNetComponent::IntegrateQuat(const FQuat& Q0, const FVector& Wrad, float Dt)
{
	const float Angle = Wrad.Size() * Dt;
	if (Angle < 1e-6f) return Q0;
	const FVector Axis = Wrad.GetSafeNormal();
	const FQuat dQ(Axis, Angle);
	return (dQ * Q0).GetNormalized();
}

void UShipNetComponent::DriveSimulatedProxy()
{
	if (!Ship || !ShipMesh || NetBuffer.Num()==0 || !bHaveTimeSync || !GetWorld()) return;

	const double Now = GetWorld()->GetTimeSeconds();
	const double Tq  = Now - (double)NetInterpDelay;

	// РіСЂР°РЅРёС‡РЅС‹Рµ СЃР»СѓС‡Р°Рё
	if (NetBuffer.Num()==1 || Tq <= NetBuffer[0].Time)
	{
		const auto& N = NetBuffer[0];
		Ship->SetActorLocationAndRotation(N.Loc, N.Rot.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}
	if (Tq >= NetBuffer.Last().Time)
	{
		// РєРѕСЂРѕС‚РєР°СЏ СЌРєСЃС‚СЂР°РїРѕР»СЏС†РёСЏ в‰¤ ~100 РјСЃ РЅР° РѕСЃРЅРѕРІРµ СЃРєРѕСЂРѕСЃС‚РµР№
		const auto& L = NetBuffer.Last();
		const double Ahead = FMath::Min(0.1, Tq - L.Time);
		const FVector P = L.Loc + L.Vel * (float)Ahead;
		const FQuat   Q = IntegrateQuat(L.Rot, L.AngVel, (float)Ahead);
		Ship->SetActorLocationAndRotation(P, Q.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}

	// Р±РёРЅР°СЂРЅС‹Р№ РїРѕРёСЃРє
	int32 L=0, R=NetBuffer.Num()-1;
	while (L+1<R)
	{
		const int32 M=(L+R)/2;
		if (NetBuffer[M].Time <= Tq) L=M; else R=M;
	}
	const auto& A = NetBuffer[L];
	const auto& B = NetBuffer[L+1];

	const double dt = FMath::Max(1e-6, B.Time - A.Time);
	const float  s  = (float)FMath::Clamp((Tq - A.Time)/dt, 0.0, 1.0);

	// Hermite РїРѕ Loc/Vel
	const float s2=s*s, s3=s2*s;
	const float h00 =  2*s3 - 3*s2 + 1;
	const float h10 =    s3 - 2*s2 + s;
	const float h01 = -2*s3 + 3*s2;
	const float h11 =    s3 -   s2;

	const FVector P =  h00*A.Loc + h10*(A.Vel*(float)dt)
	                 + h01*B.Loc + h11*(B.Vel*(float)dt);

	// РћСЂРёРµРЅС‚Р°С†РёСЏ: SLERP + РєРѕСЂСЂРµРєС‚РЅС‹Р№ РёРЅС‚РµРіСЂР°Р» СѓРіР». СЃРєРѕСЂРѕСЃС‚Рё РјРµР¶РґСѓ СѓР·Р»Р°РјРё
	// (РєРІРѕР·Рё-СЃРєРѕСЂСЂРµРєС†РёСЏ: СЌРєРІРёРІР°Р»РµРЅС‚ РґРѕР±Р°РІР»РµРЅРёСЋ "twist" РѕС‚ AngVel)
	FQuat QA = A.Rot;
	FQuat QB = B.Rot;
	const FQuat QAe = IntegrateQuat(QA, A.AngVel, (float)(s*(float)dt));       // РґРѕ С‚РѕС‡РєРё РІРЅСѓС‚СЂРё РёРЅС‚РµСЂРІР°Р»Р°
	const FQuat QBe = IntegrateQuat(QB, -B.AngVel, (float)(((1.f-s))* (float)dt)); // РѕР±СЂР°С‚РЅР°СЏ "РїРѕРґС‚СЏР¶РєР°"
	const FQuat Qs  = FQuat::Slerp(QAe, QBe, s).GetNormalized();

	Ship->SetActorLocationAndRotation(P, Qs.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
}

// === Р’Р»Р°РґРµР»РµС†: РјСЏРіРєР°СЏ СЂРµРєРѕРЅСЃРёР»СЏС†РёСЏ СЃ РѕРіСЂР°РЅРёС‡РµРЅРЅС‹Рј В«РїРёРЅРєРѕРјВ» ===
// ShipNetComponent.cpp
void UShipNetComponent::OwnerReconcile_Tick(float DeltaSeconds)
{
	if (!bHaveOwnerRecon || !ShipMesh || DeltaSeconds <= 0.f || !GetWorld())
		return;

	// --- СЃРµСЂРІРµСЂРЅРѕРµ РІСЂРµРјСЏ РІ РєР»РёРµРЅС‚СЃРєРёС… СЃРµРєСѓРЅРґР°С… + Р»Р°Рі РІРїРµСЂС‘Рґ ---
	const double Now     = (double)GetWorld()->GetTimeSeconds();
	const double Tserver = (double)OwnerReconTarget.ServerTime + ServerTimeToClientTime;
	const double Ahead   = FMath::Max(0.0, Now - Tserver);

	// --- С†РµР»РµРІРѕРµ СЃРѕСЃС‚РѕСЏРЅРёРµ (СЃ СѓС‡С‘С‚РѕРј Р»Р°РіР° РІРїРµСЂС‘Рґ РїРѕ Р»РёРЅРµР№РЅРѕР№ С‡Р°СЃС‚Рё) ---
	const FVector LocS = OwnerReconTarget.Loc + OwnerReconTarget.Vel * (float)Ahead;
	const FQuat   RotS = OwnerReconTarget.RotCS.ToRotator().Quaternion();
	const FVector VelS = OwnerReconTarget.Vel;
	const FVector Wrad = OwnerReconTarget.AngVelDeg * (PI / 180.f);

	// --- С‚РµРєСѓС‰РµРµ СЃРѕСЃС‚РѕСЏРЅРёРµ ---
	const FVector LocC = ShipMesh->GetComponentLocation();
	const FQuat   RotC = ShipMesh->GetComponentQuat();
	const FVector VelC = ShipMesh->GetComponentVelocity();
	const FVector Wcur = ShipMesh->GetPhysicsAngularVelocityInRadians();

	// ========================================================================
	// РљР РРўРР§Р•РЎРљРћР• РРЎРџР РђР’Р›Р•РќРР•: РћС€РёР±РєР° РІ Р“Р›РћР‘РђР›Р¬РќР«РҐ РєРѕРѕСЂРґРёРЅР°С‚Р°С…!
	// ========================================================================
	
	double PosErr = 0.0;
	double VelErr = (VelS - VelC).Size();
	double AngErr = (Wrad - Wcur).Size();
	
	UWorld* World = GetWorld();
	USpaceFloatingOriginSubsystem* FO = World ? World->GetSubsystem<USpaceFloatingOriginSubsystem>() : nullptr;
	
	if (FO)
	{
		// РљРѕРЅРІРµСЂС‚РёСЂСѓРµРј РІ РіР»РѕР±Р°Р»СЊРЅС‹Рµ РєРѕРѕСЂРґРёРЅР°С‚С‹ РґР»СЏ РєРѕСЂСЂРµРєС‚РЅРѕРіРѕ РёР·РјРµСЂРµРЅРёСЏ РѕС€РёР±РєРё
		const FVector3d LocSGlobal = FO->WorldToGlobalVector(LocS);
		const FVector3d LocCGlobal = FO->WorldToGlobalVector(LocC);
		PosErr = FVector3d::Distance(LocSGlobal, LocCGlobal);
	}
	else
	{
		// Fallback
		PosErr = (LocS - LocC).Size();
	}

	// --- РїРѕСЂРѕРіРё Р¶С‘СЃС‚РєРѕР№ СЂРµСЃРёРЅС…СЂРѕРЅРёР·Р°С†РёРё ---
		const bool bHyper = (Ship && Ship->IsHyperDriveActive());

	float PosSnap_Soft = 15000.f;   // 80 m (cm)
	float VelSnap_Soft = 50000.f;  // 300 m/s (cm/s)
	float AngSnap_Soft = 6.0;     // ~230 deg/s

	const float HyperScale = bHyper ? 8.0f : 1.0f;
	PosSnap_Soft *= HyperScale;
	VelSnap_Soft *= HyperScale;

	const float PosSnap_Hard = PosSnap_Soft * (bHyper ? 6.0f : 3.0f);
	const float VelSnap_Hard = VelSnap_Soft * (bHyper ? 6.0f : 3.0f);
	const float AngSnap_Hard = AngSnap_Soft * 2.5f;

	const bool bHardSnap = (PosErr > PosSnap_Hard) || (VelErr > VelSnap_Hard) || (AngErr > AngSnap_Hard);
	const bool bSoftSnap = !bHardSnap && ((PosErr > PosSnap_Soft) || (VelErr > VelSnap_Soft) || (AngErr > AngSnap_Soft));

	if (bHardSnap)
	{
		UE_LOG(LogShipNet, Warning,
			TEXT("[HARD SNAP] %s | PosErr=%.0f m | VelErr=%.0f m/s | AngErr=%.1f rad/s"),
			*GetNameSafe(Ship),
			PosErr / 100.0,
			VelErr / 100.0,
			AngErr);
		
		ShipMesh->SetWorldLocationAndRotation(LocS, RotS.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		ShipMesh->SetPhysicsLinearVelocity(VelS, false);
		ShipMesh->SetPhysicsAngularVelocityInRadians(Wrad, false);

		if (PendingInputs.Num() > 0)
			PendingInputs.Reset();

		return;
	}

	// --- РјСЏРіРєР°СЏ СЂРµРєРѕРЅСЃРёР»СЏС†РёСЏ (РєСЂРёС‚РёС‡РµСЃРєРё РґРµРјРїС„РёСЂРѕРІР°РЅРЅС‹Р№ PD) ---
	const float TauPos   = bSoftSnap ? (bHyper ? 0.25f : 0.14f) : 0.08f;
	const float TauAng   = bSoftSnap ? 0.18f : 0.10f;
	const float AlphaPos = 1.f - FMath::Exp(-DeltaSeconds / TauPos);
	const float AlphaAng = 1.f - FMath::Exp(-DeltaSeconds / TauAng);

	// Р›РёРЅРµР№РЅР°СЏ С‡Р°СЃС‚СЊ
	const FVector Vtgt = VelS + (LocS - LocC) / FMath::Max(1e-3f, TauPos);

	const float MAX_VEL_NUDGE = bSoftSnap ? (bHyper ? 60000.f : 25000.f) : 10000.f;
	FVector Vnew = FMath::Lerp(VelC, Vtgt, AlphaPos);
	const FVector Nudge = Vnew - VelC;
	if (Nudge.Size() > MAX_VEL_NUDGE)
		Vnew = VelC + Nudge.GetClampedToMaxSize(MAX_VEL_NUDGE);

	ShipMesh->SetPhysicsLinearVelocity(Vnew, false);

	// РЈРіР»РѕРІР°СЏ С‡Р°СЃС‚СЊ
	FVector Axis; float Angle = 0.f;
	(RotS * RotC.Inverse()).ToAxisAndAngle(Axis, Angle);
	if (Angle > PI) Angle -= 2.f * PI;
	Axis = Axis.GetSafeNormal();

	if (!Axis.IsNearlyZero(1e-3f))
	{
		const FVector Wtgt = Wrad + Axis * (Angle / FMath::Max(1e-3f, TauAng));
		const FVector Wblend = FMath::Lerp(Wcur, Wtgt, AlphaAng);
		ShipMesh->SetPhysicsAngularVelocityInRadians(Wblend, false);
	}
}



// === Р РµРїР»РёРєР°С†РёСЏ ===
// ShipNetComponent.cpp

void UShipNetComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UShipNetComponent, ServerSnap);
}


