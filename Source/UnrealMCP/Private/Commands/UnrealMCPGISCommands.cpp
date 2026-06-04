#include "Commands/UnrealMCPGISCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EngineUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "LevelEditorSubsystem.h"
#include "Landscape.h"
#include "LevelEditor.h"
#include "ILevelEditor.h"
#include "SLevelViewport.h"

// ServeGISTools — runtime
#include "Anchor/ServeGeoAnchor.h"

// ServeGISTools — editor
#include "OpenDRIVE/ServeOpenDRIVEImporter.h"
#include "GISViewer/ServeGISViewerSubsystem.h"
#include "GISViewer/GISViewerDataset.h"

// RoadBLD
#include "DynamicRoad/DynamicRoadNetwork.h"
#include "DynamicRoad/DynamicRoadData.h"

#include "Engine/Blueprint.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static UWorld* GetEditorWorld()
{
    if (!GEditor) return nullptr;
    return GEditor->GetEditorWorldContext().World();
}

static FString SerializeErrorResponse(const FString& Msg)
{
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("status"), TEXT("error"));
    Resp->SetStringField(TEXT("error"), Msg);
    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
    return Out;
}

// ---------------------------------------------------------------------------

FUnrealMCPGISCommands::FUnrealMCPGISCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleCommand(
    const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("gis_create_level"))       return HandleCreateLevel(Params);
    if (CommandType == TEXT("gis_open_level"))         return HandleOpenLevel(Params);
    if (CommandType == TEXT("gis_get_geo_anchor"))     return HandleGetGeoAnchor(Params);
    if (CommandType == TEXT("gis_set_geo_anchor"))     return HandleSetGeoAnchor(Params);
    if (CommandType == TEXT("gis_import_opendrive"))   return HandleImportOpenDRIVE(Params);
    if (CommandType == TEXT("gis_list_road_networks")) return HandleListRoadNetworks(Params);
    if (CommandType == TEXT("gis_list_road_presets"))  return HandleListRoadPresets(Params);
    if (CommandType == TEXT("gis_viewer_load_file"))   return HandleViewerLoadFile(Params);
    if (CommandType == TEXT("gis_viewer_list_layers")) return HandleViewerListLayers(Params);
    if (CommandType == TEXT("gis_viewer_clear"))       return HandleViewerClear(Params);
    if (CommandType == TEXT("gis_focus_landscapes"))      return HandleFocusLandscapes(Params);
    return FUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown GIS command: %s"), *CommandType));
}

// ---------------------------------------------------------------------------
// gis_create_level
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleCreateLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString LevelPath;
    if (!Params->TryGetStringField(TEXT("level_path"), LevelPath))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_create_level: level_path required (e.g. /Game/Maps/MyCity)"));

    ULevelEditorSubsystem* LES = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    if (!LES)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_create_level: LevelEditorSubsystem unavailable"));

    if (!LES->NewLevel(LevelPath))
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_create_level: failed to create '%s'"), *LevelPath));

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetStringField(TEXT("level_path"), LevelPath);
    return R;
}

// ---------------------------------------------------------------------------
// gis_open_level
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleOpenLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString LevelPath;
    if (!Params->TryGetStringField(TEXT("level_path"), LevelPath))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_open_level: level_path required"));

    ULevelEditorSubsystem* LES = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    if (!LES)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_open_level: LevelEditorSubsystem unavailable"));

    if (!LES->LoadLevel(LevelPath))
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_open_level: failed to open '%s'"), *LevelPath));

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetStringField(TEXT("level_path"), LevelPath);
    return R;
}

// ---------------------------------------------------------------------------
// gis_get_geo_anchor
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleGetGeoAnchor(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_get_geo_anchor: no editor world"));

    AServeGeoAnchor* Anchor = AServeGeoAnchor::FindInWorld(World);

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetBoolField(TEXT("found"), Anchor != nullptr);

    if (Anchor)
    {
        R->SetStringField(TEXT("actor_name"),           Anchor->GetActorLabel());
        R->SetNumberField(TEXT("epsg"),                  Anchor->ProjectEPSG);
        R->SetNumberField(TEXT("origin_x"),              Anchor->OriginX);
        R->SetNumberField(TEXT("origin_y"),              Anchor->OriginY);
        R->SetNumberField(TEXT("origin_z_meters"),       Anchor->OriginZ_Meters);
        R->SetNumberField(TEXT("min_elevation_meters"),  Anchor->GlobalMinElevation_Meters);
        R->SetNumberField(TEXT("max_elevation_meters"),  Anchor->GlobalMaxElevation_Meters);
        R->SetNumberField(TEXT("meters_per_quad"),       Anchor->PreferredMetersPerQuad);
        if (!Anchor->ProjectCRS_WKT.IsEmpty())
            R->SetStringField(TEXT("crs_wkt"), Anchor->ProjectCRS_WKT);
    }
    return R;
}

// ---------------------------------------------------------------------------
// gis_set_geo_anchor
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleSetGeoAnchor(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_set_geo_anchor: no editor world"));

    AServeGeoAnchor* Anchor = AServeGeoAnchor::FindOrCreate(World);
    if (!Anchor)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_set_geo_anchor: failed to find or create GeoAnchor"));

    FString Val;
    if (Params->TryGetStringField(TEXT("epsg"),           Val)) Anchor->ProjectEPSG                = FCString::Atoi(*Val);

    // If is_geographic=true, the caller passes lon/lat degrees for origin_x/y.
    // Convert to pseudo-meters so ProjectXYMetersToWorldCm and road BuildControlPoints
    // both operate in the same coordinate system.
    bool bIsGeographic = false;
    Params->TryGetBoolField(TEXT("is_geographic"), bIsGeographic);

    if (Params->TryGetStringField(TEXT("origin_x"), Val))
    {
        double RawX = FCString::Atod(*Val);
        if (bIsGeographic && Params->HasField(TEXT("origin_y")))
        {
            FString ValY;
            Params->TryGetStringField(TEXT("origin_y"), ValY);
            double RawY = FCString::Atod(*ValY);
            const double LatRad = FMath::DegreesToRadians(RawY);
            Anchor->OriginX = RawX * (111320.0 * FMath::Cos(LatRad));
        }
        else
        {
            Anchor->OriginX = RawX;
        }
    }
    if (Params->TryGetStringField(TEXT("origin_y"), Val))
    {
        double RawY = FCString::Atod(*Val);
        Anchor->OriginY = bIsGeographic ? RawY * 111320.0 : RawY;
    }

    if (Params->TryGetStringField(TEXT("origin_z"),       Val)) Anchor->OriginZ_Meters             = FCString::Atod(*Val);
    if (Params->TryGetStringField(TEXT("min_elev"),       Val)) Anchor->GlobalMinElevation_Meters  = FCString::Atod(*Val);
    if (Params->TryGetStringField(TEXT("max_elev"),       Val)) Anchor->GlobalMaxElevation_Meters  = FCString::Atod(*Val);
    if (Params->TryGetStringField(TEXT("meters_per_quad"),Val)) Anchor->PreferredMetersPerQuad     = FCString::Atof(*Val);
    Params->TryGetStringField(TEXT("crs_wkt"), Anchor->ProjectCRS_WKT);

    Anchor->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetStringField(TEXT("actor_name"), Anchor->GetActorLabel());
    return R;
}

// ---------------------------------------------------------------------------
// gis_import_opendrive
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleImportOpenDRIVE(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath;
    if (!Params->TryGetStringField(TEXT("file_path"), FilePath))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_import_opendrive: file_path required"));

    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_import_opendrive: no editor world"));

    // Resolve road network actor
    ADynamicRoadNetwork* Network = nullptr;
    FString NetworkName;
    if (Params->TryGetStringField(TEXT("network_name"), NetworkName))
    {
        for (TActorIterator<ADynamicRoadNetwork> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(NetworkName, ESearchCase::IgnoreCase))
            {
                Network = *It;
                break;
            }
        }
        if (!Network)
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("gis_import_opendrive: RoadNetwork '%s' not found"), *NetworkName));
    }
    else
    {
        for (TActorIterator<ADynamicRoadNetwork> It(World); It; ++It)
        {
            Network = *It;
            break;
        }
        if (!Network)
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("gis_import_opendrive: no ADynamicRoadNetwork in level — spawn one first"));
    }

    // Helper: load preset CDO from a Blueprint asset path.
    auto LoadPreset = [](const FString& Path) -> UDynamicRoadDrawPreset*
    {
        UObject* Loaded = UEditorAssetLibrary::LoadAsset(Path);
        if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
        {
            if (BP->GeneratedClass)
                return Cast<UDynamicRoadDrawPreset>(BP->GeneratedClass->GetDefaultObject(false));
        }
        return Cast<UDynamicRoadDrawPreset>(Loaded);
    };

    // Resolve preset asset (optional)
    UDynamicRoadDrawPreset* Preset = nullptr;
    FString PresetPath;
    if (Params->TryGetStringField(TEXT("preset_path"), PresetPath) && !PresetPath.IsEmpty())
    {
        Preset = LoadPreset(PresetPath);
        if (!Preset)
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("gis_import_opendrive: preset not found: %s"), *PresetPath));
    }

    AServeGeoAnchor* Anchor = AServeGeoAnchor::FindInWorld(World);

    float SampleEps = 0.5f;
    FString EpsStr;
    if (Params->TryGetStringField(TEXT("sample_eps"), EpsStr))
        SampleEps = FCString::Atof(*EpsStr);

    FString OutError;
    const int32 Count = FServeOpenDRIVEImporter::ImportFromFile(FilePath, Preset, Network, Anchor, SampleEps, OutError);

    if (Count < 0)
        return FUnrealMCPCommonUtils::CreateErrorResponse(OutError);

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("roads_imported"), Count);
    if (!OutError.IsEmpty())
        R->SetStringField(TEXT("warnings"), OutError);
    return R;
}

// ---------------------------------------------------------------------------
// gis_list_road_networks
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleListRoadNetworks(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_list_road_networks: no editor world"));

    TArray<TSharedPtr<FJsonValue>> Networks;
    for (TActorIterator<ADynamicRoadNetwork> It(World); It; ++It)
    {
        auto N = MakeShared<FJsonObject>();
        N->SetStringField(TEXT("name"), It->GetActorLabel());
        Networks.Add(MakeShared<FJsonValueObject>(N));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetArrayField(TEXT("networks"), Networks);
    return R;
}

// ---------------------------------------------------------------------------
// gis_list_road_presets
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleListRoadPresets(const TSharedPtr<FJsonObject>& Params)
{
    // Presets are Blueprint assets whose GeneratedClass extends UDynamicRoadDrawPreset.
    // Search the known preset directory and verify each one loads as a valid CDO.
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName("/ServeGISTools/Roads/Presets"));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> Presets;
    for (const FAssetData& AD : Assets)
    {
        UObject* Loaded = UEditorAssetLibrary::LoadAsset(AD.GetObjectPathString());
        UBlueprint* BP = Cast<UBlueprint>(Loaded);
        if (!BP || !BP->GeneratedClass) { continue; }
        if (!Cast<UDynamicRoadDrawPreset>(BP->GeneratedClass->GetDefaultObject(false))) { continue; }

        auto P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), AD.AssetName.ToString());
        P->SetStringField(TEXT("path"), AD.GetObjectPathString());
        Presets.Add(MakeShared<FJsonValueObject>(P));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetArrayField(TEXT("presets"), Presets);
    return R;
}

// ---------------------------------------------------------------------------
// gis_viewer_load_file
// ---------------------------------------------------------------------------

static UServeGISViewerSubsystem* GetViewerSubsystem()
{
    return GEditor ? GEditor->GetEditorSubsystem<UServeGISViewerSubsystem>() : nullptr;
}

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleViewerLoadFile(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath;
    if (!Params->TryGetStringField(TEXT("file_path"), FilePath))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_viewer_load_file: file_path required"));

    UServeGISViewerSubsystem* Sub = GetViewerSubsystem();
    if (!Sub)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_viewer_load_file: ServeGISViewerSubsystem unavailable"));

    FString Msg;
    const bool bOk = Sub->LoadFile(FilePath, Msg);

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), bOk);
    R->SetStringField(TEXT("message"), Msg);
    if (bOk)
        R->SetNumberField(TEXT("layer_count"), Sub->GetDataset() ? Sub->GetDataset()->Layers.Num() : 0);
    return R;
}

// ---------------------------------------------------------------------------
// gis_viewer_list_layers
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleViewerListLayers(const TSharedPtr<FJsonObject>& Params)
{
    UServeGISViewerSubsystem* Sub = GetViewerSubsystem();
    if (!Sub || !Sub->GetDataset())
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_viewer_list_layers: ServeGISViewerSubsystem unavailable"));

    const UGISViewerDataset* Ds = Sub->GetDataset();

    TArray<TSharedPtr<FJsonValue>> Layers;
    for (int32 i = 0; i < Ds->Layers.Num(); ++i)
    {
        const FGISViewerLayer& Layer = Ds->Layers[i];
        auto L = MakeShared<FJsonObject>();
        L->SetNumberField(TEXT("index"),       i);
        L->SetStringField(TEXT("name"),        Layer.DisplayName);
        L->SetStringField(TEXT("file_path"),   Layer.FilePath);
        const TCHAR* LayerTypeStr = Layer.LayerType == EGISLayerType::Vector    ? TEXT("vector")
                                  : Layer.LayerType == EGISLayerType::Raster    ? TEXT("raster")
                                  : Layer.LayerType == EGISLayerType::OpenDRIVE ? TEXT("opendrive")
                                                                                : TEXT("unknown");
        L->SetStringField(TEXT("layer_type"), LayerTypeStr);
        L->SetBoolField(TEXT("visible"),       Layer.bVisible);
        L->SetBoolField(TEXT("loading"),       Layer.bLoadingData);
        L->SetNumberField(TEXT("shape_count"), Layer.Shapes.Num());
        Layers.Add(MakeShared<FJsonValueObject>(L));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("layer_count"), Ds->Layers.Num());
    R->SetArrayField(TEXT("layers"), Layers);
    return R;
}

// ---------------------------------------------------------------------------
// gis_viewer_clear
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleViewerClear(const TSharedPtr<FJsonObject>& Params)
{
    UServeGISViewerSubsystem* Sub = GetViewerSubsystem();
    if (!Sub)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_viewer_clear: ServeGISViewerSubsystem unavailable"));

    Sub->ClearDataset();

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    return R;
}

// ---------------------------------------------------------------------------
// gis_focus_landscapes
// Positions the editor viewport so all ServeLandscape actors are just in frame,
// viewed from above at a 3/4 angle (pitch=-45, yaw=45).
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleFocusLandscapes(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_focus_landscapes: no editor world"));

    // Build combined 3D AABB of all landscape actors.
    FBox CombinedBox(ForceInit);
    int32 LandscapeCount = 0;
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        FVector Origin, Extent;
        It->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
        CombinedBox += FBox(Origin - Extent, Origin + Extent);
        ++LandscapeCount;
    }

    if (LandscapeCount == 0)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_focus_landscapes: no Landscape actors in level"));

    if (!GEditor)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_focus_landscapes: GEditor unavailable"));

    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    TSharedPtr<ILevelEditor> LevelEditor = LevelEditorModule.GetFirstLevelEditor();
    if (!LevelEditor.IsValid())
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_focus_landscapes: LevelEditor unavailable"));

    TSharedPtr<SLevelViewport> ActiveViewport = LevelEditor->GetActiveViewportInterface();
    if (!ActiveViewport.IsValid())
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_focus_landscapes: no active viewport"));

    FLevelEditorViewportClient* VC = &ActiveViewport->GetLevelViewportClient();

    // Camera pitch / yaw — 3/4 angle: looking down from above at 45 degrees.
    const float Pitch = Params->HasField(TEXT("pitch")) ? (float)Params->GetNumberField(TEXT("pitch")) : -45.f;
    const float Yaw   = Params->HasField(TEXT("yaw"))   ? (float)Params->GetNumberField(TEXT("yaw"))   :  45.f;

    // Camera basis vectors: Forward points from camera toward the box center.
    // Right = cross(WorldUp, Forward).  Up = cross(Forward, Right).
    const float PitchRad = FMath::DegreesToRadians(Pitch);
    const float YawRad   = FMath::DegreesToRadians(Yaw);
    const FVector Forward(
        FMath::Cos(PitchRad) * FMath::Cos(YawRad),
        FMath::Cos(PitchRad) * FMath::Sin(YawRad),
        FMath::Sin(PitchRad));
    const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();
    const FVector Up    = FVector::CrossProduct(Forward, Right).GetSafeNormal();

    // Viewport FOV from the viewport client (horizontal), derive vertical from aspect ratio.
    const FIntPoint ViewportSize = VC->Viewport ? VC->Viewport->GetSizeXY() : FIntPoint(1920, 1080);
    const float AspectRatio = (ViewportSize.Y > 0)
        ? (float)ViewportSize.X / (float)ViewportSize.Y
        : (16.f / 9.f);
    const float HFovRad  = FMath::DegreesToRadians(FMath::Max(VC->ViewFOV, 10.f));
    const float HalfHTan = FMath::Tan(HFovRad * 0.5f);
    const float VFovRad  = 2.f * FMath::Atan(HalfHTan / AspectRatio);
    const float HalfVTan = FMath::Tan(VFovRad * 0.5f);

    // For a camera at P = BoxCenter - Forward * D, each corner Q projects to:
    //   x_c = dot(Q - BoxCenter, Right)   — independent of D
    //   y_c = dot(Q - BoxCenter, Up)      — independent of D
    //   z_c = dot(Q - BoxCenter, Forward) + D
    //
    // Visibility requires:  |x_c| / z_c <= HalfHTan  and  |y_c| / z_c <= HalfVTan
    // Rearranging:          D >= |x_c| / HalfHTan - z_d
    //                       D >= |y_c| / HalfVTan - z_d   (where z_d = dot(delta, Forward))
    const FVector BoxCenter = CombinedBox.GetCenter();
    const FVector BoxExtent = CombinedBox.GetExtent();

    float DMin = 1.f;
    for (int32 i = 0; i < 8; ++i)
    {
        const FVector Corner = BoxCenter + FVector(
            (i & 1) ? BoxExtent.X : -BoxExtent.X,
            (i & 2) ? BoxExtent.Y : -BoxExtent.Y,
            (i & 4) ? BoxExtent.Z : -BoxExtent.Z);
        const FVector Delta = Corner - BoxCenter;
        const float XC = FVector::DotProduct(Delta, Right);
        const float YC = FVector::DotProduct(Delta, Up);
        const float ZD = FVector::DotProduct(Delta, Forward);
        DMin = FMath::Max(DMin, FMath::Abs(XC) / HalfHTan - ZD);
        DMin = FMath::Max(DMin, FMath::Abs(YC) / HalfVTan - ZD);
    }

    const float Distance  = DMin * 1.05f; // 5% margin so corners aren't flush with frustum edges
    const FVector CamLocation = BoxCenter - Forward * Distance;

    VC->SetViewLocation(CamLocation);
    VC->SetViewRotation(FRotator(Pitch, Yaw, 0.f));
    VC->Invalidate();

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("landscape_count"), LandscapeCount);
    R->SetStringField(TEXT("camera_location"),
        FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), CamLocation.X, CamLocation.Y, CamLocation.Z));
    R->SetStringField(TEXT("camera_rotation"),
        FString::Printf(TEXT("pitch=%.1f yaw=%.1f"), Pitch, Yaw));
    R->SetStringField(TEXT("fov"),
        FString::Printf(TEXT("h=%.1f v=%.1f aspect=%.3f"),
            FMath::RadiansToDegrees(HFovRad),
            FMath::RadiansToDegrees(VFovRad),
            AspectRatio));
    return R;
}

