// SpaceSquadSubsystem.cpp

#include "SpaceSquadSubsystem.h"
#include "ShipPawn.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY(LogSpaceSquad);

FSquadInfo* USpaceSquadSubsystem::FindSquad(int32 SquadId)
{
	return Squads.Find(SquadId);
}

const FSquadInfo* USpaceSquadSubsystem::FindSquad(int32 SquadId) const
{
	return Squads.Find(SquadId);
}

void USpaceSquadSubsystem::CompactSquadMembers(FSquadInfo& Squad)
{
	Squad.Members.RemoveAll([](const TWeakObjectPtr<AShipPawn>& ShipPtr)
	{
		AShipPawn* Ship = ShipPtr.Get();
		return !Ship || Ship->IsActorBeingDestroyed() || !Ship->IsAlive();
	});

	if (!Squad.Leader.IsValid() || !Squad.Members.Contains(Squad.Leader))
	{
		RefreshLeader(Squad);
	}
}

void USpaceSquadSubsystem::RefreshLeader(FSquadInfo& Squad)
{
	Squad.Leader = nullptr;
	for (const TWeakObjectPtr<AShipPawn>& ShipPtr : Squad.Members)
	{
		AShipPawn* Ship = ShipPtr.Get();
		if (Ship && Ship->IsAlive())
		{
			Squad.Leader = Ship;
			break;
		}
	}
}

void USpaceSquadSubsystem::RemoveOpenSquad(int32 TeamId, int32 SquadId)
{
	if (TArray<int32>* List = OpenSquadsByTeam.Find(TeamId))
	{
		List->RemoveSwap(SquadId);
		if (List->Num() == 0)
		{
			OpenSquadsByTeam.Remove(TeamId);
		}
	}
}

void USpaceSquadSubsystem::MarkOpenSquad(int32 TeamId, int32 SquadId)
{
	TArray<int32>& List = OpenSquadsByTeam.FindOrAdd(TeamId);
	if (!List.Contains(SquadId))
	{
		List.Add(SquadId);
	}
}

void USpaceSquadSubsystem::CleanupTailLists(FSquadInfo& Squad, AActor* TargetActor)
{
	if (!TargetActor)
	{
		return;
	}

	if (TargetActor->IsActorBeingDestroyed())
	{
		Squad.TailAttackers.Remove(TargetActor);
		return;
	}

	if (TArray<TWeakObjectPtr<AShipPawn>>* Attackers = Squad.TailAttackers.Find(TargetActor))
	{
		Attackers->RemoveAll([](const TWeakObjectPtr<AShipPawn>& ShipPtr)
		{
			AShipPawn* Ship = ShipPtr.Get();
			return !Ship || Ship->IsActorBeingDestroyed() || !Ship->IsAlive();
		});

		if (Attackers->Num() == 0)
		{
			Squad.TailAttackers.Remove(TargetActor);
		}
	}
}

void USpaceSquadSubsystem::CleanupTailLists(FSquadInfo& Squad, AShipPawn* Ship)
{
	if (!Ship)
	{
		return;
	}

	for (auto It = Squad.TailAttackers.CreateIterator(); It; ++It)
	{
		TArray<TWeakObjectPtr<AShipPawn>>& Attackers = It.Value();
		Attackers.RemoveAll([Ship](const TWeakObjectPtr<AShipPawn>& ShipPtr)
		{
			AShipPawn* S = ShipPtr.Get();
			return !S || S->IsActorBeingDestroyed() || !S->IsAlive() || S == Ship;
		});

		AActor* Target = It.Key().Get();
		const bool bTargetInvalid = !Target || Target->IsActorBeingDestroyed();
		if (Attackers.Num() == 0 || bTargetInvalid)
		{
			It.RemoveCurrent();
		}
	}
}

int32 USpaceSquadSubsystem::RegisterShip(AShipPawn* Ship)
{
	if (!Ship || !Ship->HasAuthority())
	{
		return INDEX_NONE;
	}

	if (const int32* Existing = ShipToSquad.Find(Ship))
	{
		return *Existing;
	}

	const int32 TeamId = Ship->GetTeamId();
	int32 SquadId = INDEX_NONE;

	if (TArray<int32>* OpenList = OpenSquadsByTeam.Find(TeamId))
	{
		for (int32 i = OpenList->Num() - 1; i >= 0; --i)
		{
			const int32 CandidateId = (*OpenList)[i];
			FSquadInfo* Squad = FindSquad(CandidateId);
			if (!Squad || Squad->TeamId != TeamId)
			{
				OpenList->RemoveAtSwap(i);
				continue;
			}

			CompactSquadMembers(*Squad);

			if (Squad->Members.Num() < MaxSquadSize)
			{
				SquadId = CandidateId;
				break;
			}

			if (Squad->Members.Num() >= MaxSquadSize)
			{
				OpenList->RemoveAtSwap(i);
			}
		}
	}

	if (SquadId == INDEX_NONE)
	{
		SquadId = NextSquadId++;
		FSquadInfo& NewSquad = Squads.Add(SquadId);
		NewSquad.SquadId = SquadId;
		NewSquad.TeamId = TeamId;
		MarkOpenSquad(TeamId, SquadId);
	}

	FSquadInfo* Squad = FindSquad(SquadId);
	if (!Squad)
	{
		return INDEX_NONE;
	}

	Squad->Members.Add(Ship);

	if (!Squad->Leader.IsValid())
	{
		Squad->Leader = Ship;
	}

	ShipToSquad.Add(Ship, SquadId);
	CompactSquadMembers(*Squad);

	if (Squad->Members.Num() >= MaxSquadSize)
	{
		RemoveOpenSquad(TeamId, SquadId);
	}
	else
	{
		MarkOpenSquad(TeamId, SquadId);
	}

	return SquadId;
}

void USpaceSquadSubsystem::UnregisterShip(AShipPawn* Ship)
{
	if (!Ship)
	{
		return;
	}

	int32 SquadId = INDEX_NONE;
	if (int32* IdPtr = ShipToSquad.Find(Ship))
	{
		SquadId = *IdPtr;
		ShipToSquad.Remove(Ship);
	}
	else
	{
		return;
	}

	FSquadInfo* Squad = FindSquad(SquadId);
	if (!Squad)
	{
		return;
	}

	Squad->Members.RemoveAll([Ship](const TWeakObjectPtr<AShipPawn>& Ptr)
	{
		return Ptr.Get() == Ship;
	});
	CleanupTailLists(*Squad, Ship);
	CompactSquadMembers(*Squad);

	if (Squad->Leader.Get() == Ship)
	{
		RefreshLeader(*Squad);
	}

	if (Squad->Members.Num() == 0)
	{
		RemoveOpenSquad(Squad->TeamId, SquadId);
		Squads.Remove(SquadId);
		return;
	}

	if (Squad->Members.Num() < MaxSquadSize)
	{
		MarkOpenSquad(Squad->TeamId, SquadId);
	}
}

int32 USpaceSquadSubsystem::GetSquadIdForShip(AShipPawn* Ship) const
{
	if (!Ship)
	{
		return INDEX_NONE;
	}

	if (const int32* IdPtr = ShipToSquad.Find(Ship))
	{
		return *IdPtr;
	}
	return INDEX_NONE;
}

AShipPawn* USpaceSquadSubsystem::GetSquadLeader(AShipPawn* Ship)
{
	const int32 SquadId = GetSquadIdForShip(Ship);
	FSquadInfo* Squad = FindSquad(SquadId);
	if (!Squad)
	{
		return nullptr;
	}

	CompactSquadMembers(*Squad);
	return Squad->Leader.Get();
}

const TArray<TWeakObjectPtr<AShipPawn>>* USpaceSquadSubsystem::GetSquadMembers(int32 SquadId)
{
	FSquadInfo* Squad = FindSquad(SquadId);
	if (!Squad)
	{
		return nullptr;
	}
	CompactSquadMembers(*Squad);
	return &Squad->Members;
}

void USpaceSquadSubsystem::SetSquadTarget(int32 SquadId, AActor* TargetActor)
{
	FSquadInfo* Squad = FindSquad(SquadId);
	if (!Squad)
	{
		return;
	}

	AActor* OldTarget = Squad->CurrentTarget.Get();
	if (OldTarget && OldTarget != TargetActor)
	{
		Squad->TailAttackers.Remove(OldTarget);
	}

	if (TargetActor && TargetActor->IsActorBeingDestroyed())
	{
		TargetActor = nullptr;
	}

	Squad->CurrentTarget = TargetActor;
	if (TargetActor)
	{
		CleanupTailLists(*Squad, TargetActor);
	}
}

AActor* USpaceSquadSubsystem::GetSquadTarget(int32 SquadId)
{
	FSquadInfo* Squad = FindSquad(SquadId);
	if (!Squad)
	{
		return nullptr;
	}

	AActor* Target = Squad->CurrentTarget.Get();
	if (!Target || Target->IsActorBeingDestroyed())
	{
		if (Target)
		{
			Squad->TailAttackers.Remove(Target);
		}
		Squad->CurrentTarget = nullptr;
		return nullptr;
	}

	CleanupTailLists(*Squad, Target);
	return Target;
}

bool USpaceSquadSubsystem::TryAcquireTailSlot(AShipPawn* Ship, AActor* TargetActor)
{
	if (!Ship || !TargetActor || TargetActor->IsActorBeingDestroyed() || !Ship->HasAuthority())
	{
		return false;
	}

	const int32 SquadId = GetSquadIdForShip(Ship);
	FSquadInfo* Squad = FindSquad(SquadId);
	if (!Squad)
	{
		return false;
	}

	CleanupTailLists(*Squad, TargetActor);

	TArray<TWeakObjectPtr<AShipPawn>>& Attackers = Squad->TailAttackers.FindOrAdd(TargetActor);

	for (const TWeakObjectPtr<AShipPawn>& Ptr : Attackers)
	{
		if (Ptr.Get() == Ship)
		{
			return true;
		}
	}

	if (Attackers.Num() >= MaxTailAttackersPerTarget)
	{
		return false;
	}

	Attackers.Add(Ship);
	return true;
}

void USpaceSquadSubsystem::ReleaseTailSlot(AShipPawn* Ship, AActor* TargetActor)
{
	if (!Ship || !TargetActor || !Ship->HasAuthority())
	{
		return;
	}

	const int32 SquadId = GetSquadIdForShip(Ship);
	FSquadInfo* Squad = FindSquad(SquadId);
	if (!Squad)
	{
		return;
	}

	if (TArray<TWeakObjectPtr<AShipPawn>>* Attackers = Squad->TailAttackers.Find(TargetActor))
	{
		Attackers->RemoveAll([Ship](const TWeakObjectPtr<AShipPawn>& ShipPtr)
		{
			AShipPawn* S = ShipPtr.Get();
			return !S || S->IsActorBeingDestroyed() || !S->IsAlive() || S == Ship;
		});

		if (Attackers->Num() == 0)
		{
			Squad->TailAttackers.Remove(TargetActor);
		}
	}
}
