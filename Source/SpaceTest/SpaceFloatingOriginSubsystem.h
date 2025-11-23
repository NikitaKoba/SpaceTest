// SpaceFloatingOriginSubsystem.h
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SpaceGlobalCoords.h"
#include "SpaceFloatingOriginSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpaceFloatingOrigin, Log, All);

class AActor;

/**
 * Floating Origin:
 *  - РҐСЂР°РЅРёС‚ РіР»РѕР±Р°Р»СЊРЅС‹Рµ РєРѕРѕСЂРґРёРЅР°С‚С‹ world-origin O (OriginGlobal).
 *  - РЈРјРµРµС‚ РєРѕРЅРІРµСЂС‚РёСЂРѕРІР°С‚СЊ World <-> Global (С‡РµСЂРµР· G = O + (World - WorldOriginUU) / 100).
 *  - РџСЂРё СѓС…РѕРґРµ СЏРєРѕСЂСЏ РґР°Р»РµРєРѕ РѕС‚ (0,0,0) СЃРґРІРёРіР°РµС‚ Р’РЎР• Р°РєС‚РѕСЂС‹ РЅР° О” = -AnchorLoc.
 */
UCLASS()
class SPACETEST_API USpaceFloatingOriginSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    // --- РќРћР’Р«Р™ API ---

    // Р•СЃС‚СЊ Р»Рё РІР°Р»РёРґРЅС‹Р№ origin (СЃРµСЂРІРµСЂ РµРіРѕ РїРѕСЃС‡РёС‚Р°Р» Рё/РёР»Рё РїСЂРёС€С‘Р» РѕС‚ ASpaceWorldOriginActor)
    bool HasValidOrigin() const { return bHasValidOrigin; }

    // РџСЂРёРјРµРЅРµРЅРёРµ СЂРµРїР»РёС†РёСЂРѕРІР°РЅРЅРѕРіРѕ origin'Р° РЅР° РєР»РёРµРЅС‚Р°С… Рё СЃРµСЂРІРµСЂРµ
    void ApplyReplicatedOrigin(const FVector3d& NewOriginGlobal,
                               const FVector3d& NewWorldOriginUU);

    // Р’С‹Р·С‹РІР°С‚СЊ РўРћР›Р¬РљРћ РЅР° СЃРµСЂРІРµСЂРµ, РєРѕРіРґР° С‚С‹ СЂРµС€Р°РµС€СЊ В«СЃРґРІРёРЅСѓС‚СЊВ» origin
    void ServerShiftWorldTo(const FVector& NewWorldOriginUU);

    // Р“РµС‚С‚РµСЂС‹, С‡С‚РѕР±С‹ РІ РґСЂСѓРіРёС… РјРµСЃС‚Р°С… РјРѕР¶РЅРѕ Р±С‹Р»Рѕ СЃРїРѕРєРѕР№РЅРѕ С‡РёС‚Р°С‚СЊ origin
    const FVector3d& GetOriginGlobal() const  { return OriginGlobal; }
    const FVector3d& GetWorldOriginUU() const { return WorldOriginUU; }

    // РњР°РєСЃРёРјР°Р»СЊРЅС‹Р№ СЃРґРІРёРі origin Р·Р° РѕРґРёРЅ С‚РёРє (РІ UU). РћРіСЂР°РЅРёС‡РёРІР°РµРј, С‡С‚РѕР±С‹ РёР·Р±РµРіР°С‚СЊ СЂРµР·РєРѕРіРѕ В«С‚РµР»РµРїРѕСЂС‚Р°В»
    UPROPERTY(EditAnywhere, Category="FloatingOrigin")
    double MaxShiftPerTickUU = 0.0; // 0 = без лимита шага; иначе ограничение в UU за тик // 10 РєРј Р·Р° С‚РёРє

    // USubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // UTickableWorldSubsystem
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    /** Р’РєР»СЋС‡РёС‚СЊ/РІС‹РєР»СЋС‡РёС‚СЊ СЃРґРІРёРі РјРёСЂР° */
    void SetEnabled(bool bInEnabled);
    bool IsEnabled() const { return bEnabled; }

    /** Р—Р°РґР°С‚СЊ СЏРєРѕСЂСЊ (РѕР±С‹С‡РЅРѕ вЂ” РєРѕСЂР°Р±Р»СЊ РёРіСЂРѕРєР°) */
    void SetAnchor(AActor* InAnchor);

    /** World -> Global (double) */
    FVector3d WorldToGlobalVector(const FVector& WorldLoc) const;

    /** World -> FGlobalPos */
    void WorldToGlobal(const FVector& WorldLoc, FGlobalPos& OutPos) const;

    /** FGlobalPos -> World */
    FVector GlobalToWorld(const FGlobalPos& GP) const;

    /** Global FVector3d -> World */
    FVector GlobalToWorld_Vector(const FVector3d& Global) const;

    // Р¤Р»Р°Рі С‚РѕР»СЊРєРѕ РґР»СЏ Р»РѕРіРёСЂРѕРІР°РЅРёСЏ (РѕРґРёРЅ СЂР°Р· РїРѕРєР°Р·Р°С‚СЊ, РєР°РєРёРµ СЂРµР°Р»СЊРЅС‹Рµ Р·РЅР°С‡РµРЅРёСЏ РІР·СЏР»Рё РёР· CVars)
    bool bLoggedRuntimeSettings = false;

private:
    // Р’РєР»СЋС‡С‘РЅ Р»Рё РІРѕРѕР±С‰Рµ РјРµС…Р°РЅРёР·Рј
    bool bEnabled = false;

    // Р¤Р»Р°Рі: origin РёРЅРёС†РёР°Р»РёР·РёСЂРѕРІР°РЅ Рё СЃРѕРіР»Р°СЃРѕРІР°РЅ
    bool bHasValidOrigin = false;

    // РЇРєРѕСЂРЅС‹Р№ Р°РєС‚РѕСЂ (РєРѕСЂР°Р±Р»СЊ/РёРіСЂРѕРє), РІРѕРєСЂСѓРі РєРѕС‚РѕСЂРѕРіРѕ СЃС‡РёС‚Р°РµРј origin
    TWeakObjectPtr<AActor> Anchor;

    // Р Р°РґРёСѓСЃ, РїРѕСЃР»Рµ РєРѕС‚РѕСЂРѕРіРѕ РїРµСЂРµСЃС‚СЂР°РёРІР°РµРј origin (РІ UU)
    UPROPERTY(EditAnywhere, Category="FloatingOrigin")
    double RecenterRadiusUU = 2000000.0; // 2 РјР»РЅ UU
    UPROPERTY(EditAnywhere, Category="FloatingOrigin")
    double HyperRecenterRadiusScale = 20.0;

    // Р“Р»РѕР±Р°Р»СЊРЅС‹Рµ РєРѕРѕСЂРґРёРЅР°С‚С‹ origin'Р° (РІ РјРµС‚СЂР°С…, double)
    FVector3d OriginGlobal = FVector3d::ZeroVector;

    // World-origin РІ UU (РµСЃР»Рё РЅСѓР¶РЅРѕ СЃРјРµС‰Р°С‚СЊ СЃР°Рј world origin, СЃРµР№С‡Р°СЃ РјРѕР¶РµС€СЊ РґРµСЂР¶Р°С‚СЊ = (0,0,0))
    FVector3d WorldOriginUU = FVector3d::ZeroVector;

    /** РћСЃРЅРѕРІРЅР°СЏ С„СѓРЅРєС†РёСЏ: СЃРјРµРЅР° origin, СЃРґРІРёРі РІСЃРµРіРѕ РјРёСЂР° РЅР° О” */
    void ApplyOriginShift(const FVector3d& NewOriginGlobal);
};

