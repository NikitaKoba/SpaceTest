// SRG_SpatialHash3D.h
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GameFramework/Actor.h"
#include "SRG_SpatialHash3D.generated.h"

/**
 * USRG_SpatialHash3D — компактный 3D spatial hash для RepGraph:
 * - Хранит актёров по кубическим ячейкам размера CellUU (см).
 * - Поддерживает Bias со снэпом к сетке и полный ре-хеш при смене Bias.
 * - Ленивая самоподчистка бакетов (уборка invalid, пере-хеш «съехавших»).
 * - Быстрые запросы по сфере / K-ближайших (для пузыря интереса).
 * - Хелперы для дедлайн-планирования по угловой заметности (T*).
 *
 * Важно: QuerySphere/QueryKNearest могут МУТИРОВАТЬ внутренние структуры
 * (ленивая подчистка), поэтому они не const.
 */
UCLASS()
class SPACETEST_API USRG_SpatialHash3D : public UObject
{
	GENERATED_BODY()

public:
	/** Инициализация: размер ячейки в UU (см) */
	void Init(float InCellUU)
	{
		SetCellSize(FMath::Max(1.f, InCellUU));
		Bias = FVector::ZeroVector;
	}

	/** Задать новый размер ячейки (полный ре-хеш) */
	void SetCellSize(float InCellUU)
	{
		CellUU    = FMath::Max(1.f, InCellUU);
		InvCellUU = 1.f / CellUU;
		RehashAll();
	}

	/** Сменить Bias (снэп к сетке) и полностью переразложить */
	void SetBias(const FVector& NewBias)
	{
		Bias = FVector(
			FMath::RoundToDouble(NewBias.X * InvCellUU) * CellUU,
			FMath::RoundToDouble(NewBias.Y * InvCellUU) * CellUU,
			FMath::RoundToDouble(NewBias.Z * InvCellUU) * CellUU
		);
		RehashAll();
	}

	/** Добавить актёра в структуру */
	void Add(AActor* A)
	{
		if (!IsValid(A)) return;

		// если уже есть — просто обновить позицию
		if (ActorToCell.Contains(A))
		{
			UpdateActor(A);
			return;
		}

		const FIntVector C = WorldToCell(A->GetActorLocation());
		FBucket& B = Buckets.FindOrAdd(C);
		B.Actors.Add(A);
		ActorToCell.Add(A, C);
	}

	/** Удалить актёра из структуры */
	void Remove(AActor* A)
	{
		if (!A) return;
		if (FIntVector* Found = ActorToCell.Find(A))
		{
			if (FBucket* B = Buckets.Find(*Found))
			{
				B->Actors.RemoveSingleSwap(A);
				if (B->Actors.Num() == 0) Buckets.Remove(*Found);
			}
			ActorToCell.Remove(A);
		}
	}

	/** Явное обновление позиции конкретного актёра (опционально) */
	void UpdateActor(AActor* A)
	{
		if (!IsValid(A)) { Remove(A); return; }
		const FIntVector NewC = WorldToCell(A->GetActorLocation());
		if (FIntVector* OldC = ActorToCell.Find(A))
		{
			if (*OldC == NewC) return;
			// убрать из старого
			if (FBucket* OB = Buckets.Find(*OldC))
			{
				OB->Actors.RemoveSingleSwap(A);
				if (OB->Actors.Num() == 0) Buckets.Remove(*OldC);
			}
			// добавить в новый
			FBucket& NB = Buckets.FindOrAdd(NewC);
			NB.Actors.Add(A);
			*OldC = NewC;
		}
		else
		{
			// не было в карте — добавить
			Add(A);
		}
	}

	/** Быстрый отбор по сфере (uu). Out — без дублей, с ленивой подчисткой */
	void QuerySphere(const FVector& Center, float RadiusUU, TArray<AActor*>& Out)
	{
		Out.Reset();
		if (RadiusUU <= 0.f) return;

		const FVector R(RadiusUU);
		const FIntVector MinC = WorldToCell(Center - R);
		const FIntVector MaxC = WorldToCell(Center + R);

		TSet<AActor*> Unique;
		Unique.Reserve(256);

		const float RadiusSq = RadiusUU * RadiusUU;

		for (int32 cz = MinC.Z; cz <= MaxC.Z; ++cz)
		for (int32 cy = MinC.Y; cy <= MaxC.Y; ++cy)
		for (int32 cx = MinC.X; cx <= MaxC.X; ++cx)
		{
			const FIntVector Key(cx, cy, cz);
			FBucket* B = Buckets.Find(Key);
			if (!B) continue;

			for (int32 i = 0; i < B->Actors.Num(); /*i++ внутри*/)
			{
				AActor* A = B->Actors[i].Get();
				if (!IsValid(A))
				{
					B->Actors.RemoveAtSwap(i);
					continue;
				}

				// Проверим принадлежность ячейке (ленивый релинк)
				const FIntVector CurC = WorldToCell(A->GetActorLocation());
				if (CurC != Key)
				{
					B->Actors.RemoveAtSwap(i);
					Relink(A, CurC);
					continue;
				}

				// Геометрия сферы
				const float DistSq = FVector::DistSquared(A->GetActorLocation(), Center);
				if (DistSq <= RadiusSq)
				{
					if (!Unique.Contains(A))
					{
						Unique.Add(A);
						Out.Add(A);
					}
				}

				++i;
			}

			if (B->Actors.Num() == 0)
			{
				Buckets.Remove(Key);
			}
		}
	}

	/**
	 * K-ближайших актёров к Center (до MaxRadiusUU). Удобно для "все в одной точке":
	 * Мы не бежим по всей карте — берём сферу и режем по K.
	 */
	void QueryKNearest(const FVector& Center, int32 K, float MaxRadiusUU, TArray<AActor*>& Out)
	{
		QuerySphere(Center, MaxRadiusUU, Out);
		if (Out.Num() <= K || K <= 0) return;

		Out.Sort([&](const AActor& L, const AActor& R)
		{
			return FVector::DistSquared(L.GetActorLocation(), Center) <
			       FVector::DistSquared(R.GetActorLocation(), Center);
		});
		Out.SetNum(K, /*bAllowShrinking=*/false);
	}

	/** Убрать все невалидные ссылки (полезно после массовых удалений) */
	void RemoveInvalids()
	{
		// Чистим бакеты
		for (auto It = Buckets.CreateIterator(); It; ++It)
		{
			FBucket& B = It.Value();
			for (int32 i = 0; i < B.Actors.Num(); /*i*/)
			{
				if (!IsValid(B.Actors[i].Get()))
				{
					B.Actors.RemoveAtSwap(i);
				}
				else { ++i; }
			}
			if (B.Actors.Num() == 0) It.RemoveCurrent();
		}
		// Чистим индекс
		for (auto It = ActorToCell.CreateIterator(); It; ++It)
		{
			if (!IsValid(It.Key().Get()))
			{
				It.RemoveCurrent();
			}
		}
	}

	/* ===================== ДЕДЛАЙН-ПЛАНИРОВАНИЕ (угловая заметность) ===================== */

	/** Вход для расчёта дедлайна T* (все величины — в СИ, где требуется) */
	struct FPerceptInput
	{
		// Позиции / скорости / ускорения (UU=см → переведи заранее в см/с и см/с^2, или работай в UU)
		FVector ViewLocUU  = FVector::ZeroVector;
		FVector ViewVelUU  = FVector::ZeroVector; // см/с
		FVector ViewAccUU  = FVector::ZeroVector; // см/с^2

		FVector TargetLocUU = FVector::ZeroVector;
		FVector TargetVelUU = FVector::ZeroVector;
		FVector TargetAccUU = FVector::ZeroVector;

		// Относительная угловая скорость (рад/с), можно дать суммарную оценку (цель + камера)
		FVector RelAngVelRad = FVector::ZeroVector;

		// Порог заметности и рельсы по времени
		float   Theta0Rad = 0.002f; // ~0.11° по умолчанию
		float   TauMin    = 0.05f;  // 50 мс
		float   TauMax    = 2.0f;   // 2 с
	};

	/** Решение: положительный корень квадратики ax^2 + bx - Theta0 = 0 (если a≈0 — линейная оценка) */
	static float ComputeDeadlineSeconds(const FPerceptInput& In)
	{
		const FVector R  = In.TargetLocUU - In.ViewLocUU;
		const float   d  = FMath::Max(1.f, R.Size());   // см, избегаем деления на 0
		const FVector n  = R / d;

		const FVector Vrel = (In.TargetVelUU - In.ViewVelUU);
		const FVector Arel = (In.TargetAccUU - In.ViewAccUU);

		const float v_tan = (Vrel - (Vrel | n) * n).Size(); // см/с (перпендикулярная составляющая)
		const float a_n   = (Arel - (Arel | n) * n).Size(); // см/с^2 (нормальная составляющая)
		const float w_rel = In.RelAngVelRad.Size();         // рад/с

		// Перевод в угловые единицы (рад) на горизонте t:
		// theta(t) ≈ (v_tan/d)*t + 0.5*(a_n/d)*t^2 + w_rel*t
		const float a = 0.5f * (a_n / d);
		const float b = (v_tan / d) + w_rel;
		const float c = -In.Theta0Rad;

		float T = In.TauMax;
		if (FMath::IsNearlyZero(a))
		{
			if (!FMath::IsNearlyZero(b))
			{
				T = FMath::Clamp(In.Theta0Rad / b, In.TauMin, In.TauMax);
			}
		}
		else
		{
			const float D = b*b - 4.f*a*c;
			if (D >= 0.f)
			{
				const float rtD = FMath::Sqrt(D);
				const float t1  = (-b + rtD) / (2.f * a);
				const float t2  = (-b - rtD) / (2.f * a);
				const float tp  = (t1 > 0.f) ? t1 : ((t2 > 0.f) ? t2 : In.TauMax);
				if (tp > 0.f) T = FMath::Clamp(tp, In.TauMin, In.TauMax);
			}
		}
		return T;
	}

	/** Срочность = clamp01( t_since_last / T* ). Удобно подавать в Score как множитель. */
	static float ComputeUrgency(float TimeSinceLastSec, float DeadlineTStarSec)
	{
		if (DeadlineTStarSec <= 0.f) return 1.f;
		return FMath::Clamp(TimeSinceLastSec / DeadlineTStarSec, 0.f, 1.f);
	}

public:
    /** World shift: перестроить бакеты по текущим world-координатам */
    void OnWorldShift()
    {
        RehashAll();
    }

private:
	struct FBucket
	{
		TArray<TWeakObjectPtr<AActor>> Actors;
	};

	// Параметры
	float   CellUU    = 1000.f;
	float   InvCellUU = 1.f / 1000.f;
	FVector Bias      = FVector::ZeroVector;

	// Хранилище
	TMap<FIntVector, FBucket> Buckets;
	// В качестве ключа используем WeakPtr — безопасно для GC; хэш и сравнение поддерживаются UE.
	TMap<TWeakObjectPtr<AActor>, FIntVector> ActorToCell;

	// Вспомогательные
	FORCEINLINE FIntVector WorldToCell(const FVector& P) const
	{
		const FVector Q = (P - Bias) * InvCellUU;
		return FIntVector(
			int32(FMath::FloorToFloat(Q.X)),
			int32(FMath::FloorToFloat(Q.Y)),
			int32(FMath::FloorToFloat(Q.Z))
		);
	}

	void RehashAll()
	{
		TMap<FIntVector, FBucket> NewBuckets;
		for (auto It = ActorToCell.CreateIterator(); It; ++It)
		{
			AActor* A = It.Key().Get();
			if (!IsValid(A))
			{
				It.RemoveCurrent();
				continue;
			}
			const FIntVector C = WorldToCell(A->GetActorLocation());
			NewBuckets.FindOrAdd(C).Actors.Add(A);
			It.Value() = C;
		}
		Buckets = MoveTemp(NewBuckets);
	}

	void Relink(AActor* A, const FIntVector& NewC)
	{
		if (!IsValid(A)) return;

		FBucket& NB = Buckets.FindOrAdd(NewC);
		NB.Actors.Add(A);

		if (FIntVector* CellPtr = ActorToCell.Find(A))
		{
			*CellPtr = NewC;
		}
		else
		{
			ActorToCell.Add(A, NewC);
		}
	}

};

