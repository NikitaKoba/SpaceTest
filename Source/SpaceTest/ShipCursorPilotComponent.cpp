// ShipCursorPilotComponent.cpp

#include "ShipCursorPilotComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"
#include "Engine/Engine.h"
#include "Debug/DebugDrawService.h"

UShipCursorPilotComponent::UShipCursorPilotComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup    = TG_PostPhysics; // после физики
}

void UShipCursorPilotComponent::BeginPlay()
{
	Super::BeginPlay();

	// Регистрируемся на рисование Canvas
	if (!DebugDrawHandle.IsValid())
	{
		DebugDrawHandle = UDebugDrawService::Register(
			TEXT("Game"),
			FDebugDrawDelegate::CreateUObject(this, &UShipCursorPilotComponent::OnDebugDraw));
	}
}

void UShipCursorPilotComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (DebugDrawHandle.IsValid())
	{
		UDebugDrawService::Unregister(DebugDrawHandle);
		DebugDrawHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void UShipCursorPilotComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn || !Pawn->IsLocallyControlled())
		return;

	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	if (!PC) return;

	// 1) собираем дельту мыши и ведём виртуальный курсор относительно центра
	float dx=0.f, dy=0.f;
	PC->GetInputMouseDelta(dx, dy); // dy положительное при движении мыши вверх? UE обычно отдаёт вниз+, инвертируем
	CursorRaw += FVector2D(dx, -dy) * MouseToPixels;

	// Возврат к центру (пружинка)
	if (ReturnToCenterRate > 0.f)
	{
		const float a = FMath::Clamp(ReturnToCenterRate * DeltaTime, 0.f, 1.f);
		CursorRaw = FMath::Lerp(CursorRaw, FVector2D::ZeroVector, a);
	}

	// Кламп по окружности MaxDeflectPx
	const float r = CursorRaw.Size();
	if (r > MaxDeflectPx && r > KINDA_SMALL_NUMBER)
	{
		CursorRaw *= (MaxDeflectPx / r);
	}

	// 2) сглаживание
	const float aSm = AlphaFromHz(SmoothHz, DeltaTime);
	CursorSm = FMath::Lerp(CursorSm, CursorRaw, aSm);

	// 3) нормализованный вектор [-1..1]
	NormalizedDeflect = (MaxDeflectPx > 1e-3f) ? (CursorSm / MaxDeflectPx) : FVector2D::ZeroVector;
	NormalizedDeflect.X = FMath::Clamp(NormalizedDeflect.X, -1.f, 1.f);
	NormalizedDeflect.Y = FMath::Clamp(NormalizedDeflect.Y, -1.f, 1.f);

	// 4) (опционально) — управление кораблём
	if (bDriveShip)
	{
		// здесь можно пробросить в твой Flight/Net компоненты (например yaw/pitch/тяга),
		// сейчас оставлено пустым, чтобы не вмешиваться в твою текущую схему ввода.
	}
}

void UShipCursorPilotComponent::OnDebugDraw(UCanvas* Canvas, APlayerController* PC)
{
	if (!bDebugDraw || !Canvas || !GetOwner()) return;

	APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn || !Pawn->IsLocallyControlled()) return;

	const float SX = Canvas->ClipX;
	const float SY = Canvas->ClipY;
	const FVector2D C(SX * 0.5f, SY * 0.5f);

	// Позиция курсора/точки
	const FVector2D P = C + CursorSm;

	// Цвета
	const FLinearColor ColMain (0.f, 0.9f, 1.f, 1.f);   // голубой
	const FLinearColor ColFill (0.f, 1.f, 0.6f, 0.9f);  // бирюзовый
	const FLinearColor ColHint (0.f, 0.4f, 0.6f, 0.75f);// тускло-голубой

	// 1) Крест в центре + окружности: deadzone и max
	DrawCrosshair(Canvas, C, 8.f, ColHint, 1.5f);
	DrawCircle(Canvas, C, DeadzonePx, 64, ColHint, 1.f);
	DrawCircle(Canvas, C, MaxDeflectPx, 64, ColHint, 1.f);

	// 2) Линия от центра до курсора + хвостик мишени
	{
		FCanvasLineItem line(C, P);
		line.SetColor(ColHint.ToFColor(true));
		line.LineThickness = 1.5f;
		Canvas->DrawItem(line);

		// маленький кружок в точке P
		DrawCircle(Canvas, P, 6.f, 24, ColMain, 2.f);
	}

	// 3) Вертикальные индикаторы (слева/справа): заполняются по |X|
	{
		const float margin = 40.f;       // расстояние от центра
		const float H = 120.f;           // высота полосы
		const float W = 6.f;             // толщина рамки
		const float nx = FMath::Clamp(NormalizedDeflect.X, -1.f, 1.f);
		const float fillX = FMath::Abs(nx);

		const FVector2D LBarPos(C.X - margin - W, C.Y - H * 0.5f);
		const FVector2D RBarPos(C.X + margin     , C.Y - H * 0.5f);

		auto DrawBarV = [&](const FVector2D& Pos, float Fill01, bool bFromBottom)
		{
			const float filled = H * FMath::Clamp(Fill01, 0.f, 1.f);

			// рамка
			FCanvasBoxItem box(Pos, FVector2D(W, H));
			box.SetColor( FLinearColor(0.f,0.8f,1.f,0.5f).ToFColor(true) );
			Canvas->DrawItem(box);

			// заполнение
			const FVector2D P2 = bFromBottom ? FVector2D(Pos.X, Pos.Y + (H - filled)) : Pos;
			FCanvasBoxItem fill(P2, FVector2D(W, filled));
			fill.SetColor( ColFill.ToFColor(true) );
			Canvas->DrawItem(fill);
		};

		DrawBarV(LBarPos, fillX, /*bFromBottom=*/true);
		DrawBarV(RBarPos, fillX, /*bFromBottom=*/false);
	}

	// 4) Горизонтальные индикаторы (сверху/снизу): заполняются по |Y|
	{
		const float margin = 40.f;
		const float H = 120.f;  // длина
		const float W = 6.f;    // толщина
		const float ny = FMath::Clamp(NormalizedDeflect.Y, -1.f, 1.f);
		const float fillY = FMath::Abs(ny);

		const FVector2D TBarPos(C.X - H * 0.5f, C.Y - margin - W);
		const FVector2D BBarPos(C.X - H * 0.5f, C.Y + margin    );

		auto DrawBarH = [&](const FVector2D& Pos, float Fill01, bool bFromRight)
		{
			const float filled = H * FMath::Clamp(Fill01, 0.f, 1.f);

			FCanvasBoxItem box(Pos, FVector2D(H, W));
			box.SetColor( FLinearColor(0.f,0.8f,1.f,0.5f).ToFColor(true) );
			Canvas->DrawItem(box);

			const FVector2D P2 = bFromRight ? FVector2D(Pos.X + (H - filled), Pos.Y) : Pos;
			FCanvasBoxItem fill(P2, FVector2D(filled, W));
			fill.SetColor( ColFill.ToFColor(true) );
			Canvas->DrawItem(fill);
		};

		DrawBarH(TBarPos, fillY, /*bFromRight=*/true);
		DrawBarH(BBarPos, fillY, /*bFromRight=*/false);
	}

	// 5) Маленькая “хвостовая” черта у точки — направление движения по экрану (визуальный штрих)
	{
		const FVector2D dir = (CursorSm.IsNearlyZero()) ? FVector2D(1.f,0.f) : CursorSm.GetSafeNormal();
		const FVector2D tailA = P - dir * 14.f;
		const FVector2D tailB = P + dir * 6.f;

		FCanvasLineItem head(tailA, tailB);
		head.SetColor(ColMain.ToFColor(true));
		head.LineThickness = 2.f;
		Canvas->DrawItem(head);
	}
}

void UShipCursorPilotComponent::DrawCircle(UCanvas* Canvas, const FVector2D& Center, float Radius, int32 Segments, const FLinearColor& Color, float Thickness) const
{
	if (!Canvas || Radius <= 0.f || Segments < 6) return;

	const float da = 2.f * PI / float(Segments);
	FVector2D prev = Center + FVector2D(Radius, 0.f);

	for (int32 i=1; i<=Segments; ++i)
	{
		const float a = da * i;
		const FVector2D cur = Center + FVector2D(FMath::Cos(a)*Radius, FMath::Sin(a)*Radius);
		FCanvasLineItem L(prev, cur);
		L.SetColor(Color.ToFColor(true));
		L.LineThickness = Thickness;
		Canvas->DrawItem(L);
		prev = cur;
	}
}

void UShipCursorPilotComponent::DrawCrosshair(UCanvas* Canvas, const FVector2D& C, float len, const FLinearColor& Color, float Thickness) const
{
	if (!Canvas) return;

	auto Line = [&](FVector2D A, FVector2D B)
	{
		FCanvasLineItem L(A, B);
		L.SetColor(Color.ToFColor(true));
		L.LineThickness = Thickness;
		Canvas->DrawItem(L);
	};

	Line(C + FVector2D(-len, 0), C + FVector2D(+len, 0));
	Line(C + FVector2D(0, -len), C + FVector2D(0, +len));
}
