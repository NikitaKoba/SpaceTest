// ShipAISquadronSubsystem.cpp

#include "ShipAISquadronSubsystem.h"
#include "ShipPawn.h"
#include "ShipAIPilotComponent.h"
#include "EngineUtils.h"

void UShipAISquadronSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
}

void UShipAISquadronSubsystem::Deinitialize()
{
    Squadrons.Reset();
    ShipToSquad.Reset();
    Super::Deinitialize();
}

TStatId UShipAISquadronSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UShipAISquadronSubsystem, STATGROUP_Tickables);
}

void UShipAISquadronSubsystem::Tick(float DeltaTime)
{
    UWorld* World = GetWorld();
    if (!World || World->GetNetMode() == NM_Client)
        return;

    const float Now = World->GetTimeSeconds();
    static float LastRebuild = 0.f;

    if (Now - LastRebuild > RebuildPeriodSec)
    {
        CleanupDead();
        RebuildSquadrons();
        UpdateSquadCenters();
        LastRebuild = Now;
    }
}

void UShipAISquadronSubsystem::RegisterShip(AShipPawn* Ship)
{
    if (!Ship || !Ship->HasAuthority()) return;
    // При следующем Rebuild корабль подхватится
}

void UShipAISquadronSubsystem::UnregisterShip(AShipPawn* Ship)
{
    if (!Ship) return;
    int32* SquadIdxPtr = ShipToSquad.Find(Ship);
    if (!SquadIdxPtr) return;

    const int32 SquadIdx = *SquadIdxPtr;
    if (Squadrons.IsValidIndex(SquadIdx))
    {
        FSquadron& Sq = Squadrons[SquadIdx];
        Sq.Members.RemoveAll([Ship](const FSquadMember& M){ return M.Ship.Get() == Ship; });
        if (Sq.Leader.Get() == Ship)
        {
            Sq.Leader = nullptr;
        }
    }
    ShipToSquad.Remove(Ship);
}

void UShipAISquadronSubsystem::CleanupDead()
{
    for (int32 i = Squadrons.Num() - 1; i >= 0; --i)
    {
        FSquadron& Sq = Squadrons[i];
        Sq.Members.RemoveAll([](const FSquadMember& M)
        {
            return !M.Ship.IsValid();
        });

        if (!Sq.Leader.IsValid() && Sq.Members.Num() > 0)
        {
            // промоутим первого в лидеры
            Sq.Leader = Sq.Members[0].Ship;
            Sq.Members[0].Role = ESquadRole::Leader;
            if (AShipPawn* Ship = Sq.Leader.Get())
            {
                if (UShipAIPilotComponent* Pilot = Ship->FindComponentByClass<UShipAIPilotComponent>())
                {
                    Pilot->OnSquadAssigned(Sq.Id, ESquadRole::Leader, Ship);
                }
            }
        }

        if (Sq.Members.Num() == 0)
        {
            Squadrons.RemoveAt(i);
        }
    }

    // почистить карту
    for (auto It = ShipToSquad.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid())
        {
            It.RemoveCurrent();
        }
    }
}
void UShipAISquadronSubsystem::RebuildSquadrons()
{
    UWorld* World = GetWorld();
    if (!World) return;

    Squadrons.Reset();
    ShipToSquad.Reset();

    // собираем всех AI-кораблей
    TArray<AShipPawn*> AllShips;
    for (TActorIterator<AShipPawn> It(World); It; ++It)
    {
        AShipPawn* Ship = *It;
        if (!Ship || !Ship->HasAuthority()) continue;

        if (UShipAIPilotComponent* Pilot = Ship->FindComponentByClass<UShipAIPilotComponent>())
        {
            if (!Pilot->IsActive()) continue;
            if (!Pilot->bEnableSquadLogic) continue;
            AllShips.Add(Ship);
        }
    }

    const float R2 = SquadRadiusUU * SquadRadiusUU;
    TSet<AShipPawn*> Unassigned(AllShips);

    int32 NextId = 1;

    while (Unassigned.Num() > 0)
    {
        AShipPawn* Seed = *Unassigned.CreateIterator();
        Unassigned.Remove(Seed);

        if (!Seed) continue;

        const int32 TeamId = Seed->GetTeamId();

        FSquadron Sq;
        Sq.Id      = NextId++;
        Sq.TeamId  = TeamId;

        // начнём с сид-корабля
        Sq.Members.Add({ Seed, ESquadRole::Leader });
        Sq.Leader = Seed;

        const FVector SeedLoc = Seed->GetActorLocation();

        // докидываем ближайших до радиуса / до лимита
        for (auto It = Unassigned.CreateIterator(); It; ++It)
        {
            if (Sq.Members.Num() >= MaxSquadSize)
                break;

            AShipPawn* Other = *It;
            if (!Other) { It.RemoveCurrent(); continue; }

            if (Other->GetTeamId() != TeamId)
                continue;

            const float DistSq = FVector::DistSquared(SeedLoc, Other->GetActorLocation());
            if (DistSq <= R2)
            {
                Sq.Members.Add({ Other, ESquadRole::Attacker });
                It.RemoveCurrent();
            }
        }

        // небольшая косметика: если есть корвет — делаем его лидером
        for (FSquadMember& M : Sq.Members)
        {
            if (AShipPawn* Ship = M.Ship.Get())
            {
                if (Ship->ShipRole == EShipRole::Corvette)
                {
                    Sq.Leader = Ship;
                    M.Role = ESquadRole::Leader;
                    break;
                }
            }
        }

        // раздать OnSquadAssigned
        // раздать OnSquadAssigned + squad tactics
        int32 LocalIdx = 0;

        for (FSquadMember& M : Sq.Members)
        {
            if (AShipPawn* Ship = M.Ship.Get())
            {
                if (UShipAIPilotComponent* Pilot = Ship->FindComponentByClass<UShipAIPilotComponent>())
                {
                    Pilot->OnSquadAssigned(Sq.Id, M.Role, Sq.Leader.Get());

                    // --- тактика по месту в эскадрилье ---
                    EDogfightStyle Style = EDogfightStyle::Pursuit;
                    bool bAnchor = false;

                    if (M.Role == ESquadRole::Leader)
                    {
                        Style = EDogfightStyle::Pursuit;     // лидер всегда преследователь
                    }
                    else
                    {
                        switch (LocalIdx)
                        {
                        case 0:
                            Style = EDogfightStyle::Pursuit;     // ведомый №1 — тоже хвост, помогать добивать
                            break;
                        case 1:
                            Style = EDogfightStyle::FlankLeft;   // заходит слева
                            break;
                        case 2:
                            Style = EDogfightStyle::FlankRight;  // заходит справа
                            break;
                        case 3:
                            Style   = EDogfightStyle::BoomAndZoom; // сверху/снизу
                            bAnchor = true;                         // этот бот чаще "зависает" и стреляет
                            break;
                        default:
                            Style = EDogfightStyle::Pursuit;
                            break;
                        }
                        ++LocalIdx;
                    }

                    Pilot->SetupSquadTactics(Style, bAnchor);
                }

                ShipToSquad.Add(Ship, Squadrons.Num());
            }
        }

        Squadrons.Add(Sq);
    }
}

void UShipAISquadronSubsystem::UpdateSquadCenters()
{
    for (FSquadron& Sq : Squadrons)
    {
        FVector Sum = FVector::ZeroVector;
        int32 Count = 0;

        for (const FSquadMember& M : Sq.Members)
        {
            if (AShipPawn* Ship = M.Ship.Get())
            {
                Sum += Ship->GetActorLocation();
                ++Count;
            }
        }

        Sq.CachedCenter = (Count > 0) ? (Sum / Count) : FVector::ZeroVector;
    }
}
int32 UShipAISquadronSubsystem::GetSquadIdForShip(AShipPawn* Ship) const
{
    if (!Ship) return INDEX_NONE;
    if (const int32* Idx = ShipToSquad.Find(Ship))
        return Squadrons.IsValidIndex(*Idx) ? Squadrons[*Idx].Id : INDEX_NONE;
    return INDEX_NONE;
}

bool UShipAISquadronSubsystem::IsLeader(AShipPawn* Ship) const
{
    if (!Ship) return false;
    const int32* Idx = ShipToSquad.Find(Ship);
    if (!Idx || !Squadrons.IsValidIndex(*Idx)) return false;
    const FSquadron& Sq = Squadrons[*Idx];
    return (Sq.Leader.Get() == Ship);
}

AActor* UShipAISquadronSubsystem::GetSquadTargetForShip(AShipPawn* Ship) const
{
    if (!Ship) return nullptr;
    const int32* Idx = ShipToSquad.Find(Ship);
    if (!Idx || !Squadrons.IsValidIndex(*Idx)) return nullptr;
    const FSquadron& Sq = Squadrons[*Idx];
    return Sq.CurrentTarget.Get();
}
void UShipAISquadronSubsystem::SetSquadTargetForLeader(AShipPawn* Leader, AActor* Target)
{
    if (!Leader || !Target) return;

    UWorld* World = GetWorld();
    if (!World || World->GetNetMode() == NM_Client)
        return;

    int32* IdxPtr = ShipToSquad.Find(Leader);
    if (!IdxPtr || !Squadrons.IsValidIndex(*IdxPtr))
        return;

    FSquadron& Sq = Squadrons[*IdxPtr];
    if (Sq.Leader.Get() != Leader)
        return;

    const float Now = World->GetTimeSeconds();
    if (Now - Sq.LastRetargetTime < RetargetCooldown && Sq.CurrentTarget.IsValid())
    {
        // ???????? ???? ??? ?????????
        return;
    }

    TSet<AActor*> ClaimedTargets;
    TArray<FVector> ClaimedLocations;
    for (const FSquadron& Other : Squadrons)
    {
        if (&Other == &Sq) continue;
        if (AActor* OtherTgt = Other.CurrentTarget.Get())
        {
            ClaimedTargets.Add(OtherTgt);
            ClaimedLocations.Add(OtherTgt->GetActorLocation());
        }
    }

    const float SpacingSq = SquadTargetSpacing * SquadTargetSpacing;
    const FVector LeaderLoc = Leader->GetActorLocation();

    struct FTargetCandidate
    {
        AShipPawn* Ship = nullptr;
        float DistSq = 0.f;
    };

    TArray<FTargetCandidate> Candidates;
    for (TActorIterator<AShipPawn> It(World); It; ++It)
    {
        AShipPawn* Ship = *It;
        if (!Ship || Ship->GetTeamId() == Leader->GetTeamId()) continue;
        FTargetCandidate C;
        C.Ship   = Ship;
        C.DistSq = FVector::DistSquared(LeaderLoc, Ship->GetActorLocation());
        Candidates.Add(C);
    }

    Candidates.Sort([](const FTargetCandidate& A, const FTargetCandidate& B)
    {
        return A.DistSq < B.DistSq;
    });

    auto IsSpacedFromOthers = [&](const FVector& Loc) -> bool
    {
        for (const FVector& OtherLoc : ClaimedLocations)
        {
            if (FVector::DistSquared(Loc, OtherLoc) < SpacingSq)
            {
                return false;
            }
        }
        return true;
    };

    AActor* Chosen = Target;

    for (const FTargetCandidate& C : Candidates)
    {
        if (!C.Ship) continue;
        if (ClaimedTargets.Contains(C.Ship))
        {
            continue;
        }

        const FVector Loc = C.Ship->GetActorLocation();
        if (!IsSpacedFromOthers(Loc))
        {
            continue;
        }

        Chosen = C.Ship;
        break;
    }

    Sq.CurrentTarget   = Chosen;
    Sq.LastRetargetTime = Now;
}

