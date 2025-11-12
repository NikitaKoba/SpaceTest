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
// ShipCursorPilotComponent.cpp
// ShipCursorPilotComponent.cpp
// ==============================
// ShipCursorPilotComponent.cpp
// ==============================

bool UShipCursorPilotComponent::MakeAimRay(FVector& OutOrigin, FVector& OutDir) const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!Pawn || !PC || !Pawn->IsLocallyControlled()) return false;

	int32 VW = 0, VH = 0;
	PC->GetViewportSize(VW, VH);
	if (VW <= 0 || VH <= 0) return false;

	// Масштаб Canvas -> Viewport (DPI, windowed и т.п.)
	const float scaleX = (LastCanvasW > 0) ? float(VW) / float(LastCanvasW) : 1.f;
	const float scaleY = (LastCanvasH > 0) ? float(VH) / float(LastCanvasH) : 1.f;

	const float sx = (LastCanvasW > 0) ? LastScreenAimPx.X * scaleX : (VW * 0.5f + CursorSm.X);
	const float sy = (LastCanvasH > 0) ? LastScreenAimPx.Y * scaleY : (VH * 0.5f + CursorSm.Y);

	FVector O, D;
	if (!PC->DeprojectScreenPositionToWorld(sx, sy, O, D)) return false;

	OutOrigin = O;
	OutDir    = D.GetSafeNormal();
	return true;
}




bool UShipCursorPilotComponent::GetAimRay(FVector& OutWorldOrigin, FVector& OutWorldDir) const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!Pawn || !PC || !Pawn->IsLocallyControlled()) return false;

	int32 VW=0, VH=0;
	PC->GetViewportSize(VW, VH);

	const FVector2D Center(VW * 0.5f, VH * 0.5f);
	const FVector2D ScreenPos = Center + CursorSm;   // << твоя реальная HUD-точка

	FVector O, D;
	if (!PC->DeprojectScreenPositionToWorld(ScreenPos.X, ScreenPos.Y, O, D))
		return false;

	OutWorldOrigin = O;
	OutWorldDir    = D.GetSafeNormal();
	return true;
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

// ==============================
// ShipCursorPilotComponent.cpp
// ==============================

void UShipCursorPilotComponent::OnDebugDraw(UCanvas* Canvas, APlayerController* PC)
{
    if (!bDebugDraw || !Canvas || !GetOwner()) return;

    APawn* Pawn = Cast<APawn>(GetOwner());
    if (!Pawn || !Pawn->IsLocallyControlled()) return;

    // 1) запомним реальные размеры Canvas и пиксельную точку прицела
    LastCanvasW = int32(Canvas->ClipX);
    LastCanvasH = int32(Canvas->ClipY);

    const FVector2D C(LastCanvasW * 0.5f, LastCanvasH * 0.5f);
    const FVector2D P = C + CursorSm;       // та же формула, что и для отрисовки
    LastScreenAimPx  = P;                    // сохраняем для MakeAimRay()

    // --- ниже просто отрисовка, можешь оставить как тебе нравится ---

    const FLinearColor ColMain (0.f, 0.9f, 1.f, 1.f);
    const FLinearColor ColFill (0.f, 1.f, 0.6f, 0.9f);
    const FLinearColor ColHint (0.f, 0.4f, 0.6f, 0.75f);

    // крест и окружности
    DrawCrosshair(Canvas, C, 8.f, ColHint, 1.5f);
    DrawCircle(Canvas,   C, DeadzonePx,   64, ColHint, 1.f);
    DrawCircle(Canvas,   C, MaxDeflectPx, 64, ColHint, 1.f);

    // линия от центра к точке + маленький кружок в точке
    {
        FCanvasLineItem L(C, P);
        L.SetColor(ColHint.ToFColor(true));
        L.LineThickness = 1.5f;
        Canvas->DrawItem(L);
        DrawCircle(Canvas, P, 6.f, 24, ColMain, 2.f);
    }

    // вертикальные индикаторы (|X|)
    {
        const float margin = ReticleGapPx;
        const float H = ReticleBarLengthPx;
        const float W = ReticleBarThicknessPx;
        const float nx = FMath::Clamp(NormalizedDeflect.X, -1.f, 1.f);
        const float fillX = FMath::Abs(nx);

        const FVector2D LBarPos(C.X - margin - W, C.Y - H * 0.5f);
        const FVector2D RBarPos(C.X + margin     , C.Y - H * 0.5f);

        auto DrawBarV = [&](const FVector2D& Pos, float Fill01, bool bFromBottom)
        {
            const float filled = H * FMath::Clamp(Fill01, 0.f, 1.f);
            FCanvasBoxItem box(Pos, FVector2D(W, H));
            box.SetColor(FLinearColor(0.f,0.8f,1.f,0.5f).ToFColor(true));
            Canvas->DrawItem(box);

            const FVector2D P2 = bFromBottom ? FVector2D(Pos.X, Pos.Y + (H - filled)) : Pos;
            FCanvasBoxItem fill(P2, FVector2D(W, filled));
            fill.SetColor(ColFill.ToFColor(true));
            Canvas->DrawItem(fill);
        };

        DrawBarV(LBarPos, fillX, /*bFromBottom=*/true);
        DrawBarV(RBarPos, fillX, /*bFromBottom=*/false);
    }

    // горизонтальные индикаторы (|Y|)
    {
        const float margin = ReticleGapPx;
        const float H = ReticleBarLengthPx;
        const float W = ReticleBarThicknessPx;
        const float ny = FMath::Clamp(NormalizedDeflect.Y, -1.f, 1.f);
        const float fillY = FMath::Abs(ny);

        const FVector2D TBarPos(C.X - H * 0.5f, C.Y - margin - W);
        const FVector2D BBarPos(C.X - H * 0.5f, C.Y + margin    );

        auto DrawBarH = [&](const FVector2D& Pos, float Fill01, bool bFromRight)
        {
            const float filled = H * FMath::Clamp(Fill01, 0.f, 1.f);
            FCanvasBoxItem box(Pos, FVector2D(H, W));
            box.SetColor(FLinearColor(0.f,0.8f,1.f,0.5f).ToFColor(true));
            Canvas->DrawItem(box);

            const FVector2D P2 = bFromRight ? FVector2D(Pos.X + (H - filled), Pos.Y) : Pos;
            FCanvasBoxItem fill(P2, FVector2D(filled, W));
            fill.SetColor(ColFill.ToFColor(true));
            Canvas->DrawItem(fill);
        };

        DrawBarH(TBarPos, fillY, /*bFromRight=*/true);
        DrawBarH(BBarPos, fillY, /*bFromRight=*/false);
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
