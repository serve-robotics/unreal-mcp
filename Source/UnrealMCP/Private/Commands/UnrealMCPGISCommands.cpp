#include "Commands/UnrealMCPGISCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "Editor.h"
#include "TempoRoadLaneGraphSubsystem.h"
#include "TempoRoadInterface.h"
#include "ZoneShapeComponent.h"
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
#include "ReportMarker/ServeGISReportMarker.h"

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
    if (CommandType == TEXT("gis_screenshot_markers"))    return HandleScreenshotMarkers(Params);
    if (CommandType == TEXT("gis_build_zone_graph"))      return HandleBuildZoneGraph(Params);

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

// ---------------------------------------------------------------------------
// gis_screenshot_markers
// Iterates all AServeGISReportMarker actors in the level (filtered by tag if
// supplied), frames each with an inflated bounding box, and takes two
// screenshots: one perspective (pitch=-45, yaw=45) and one top-down
// (pitch=-89.9, yaw=0). Both shots are renamed and kept in the screenshot dir.
// Params:
//   tag     (string, optional) — actor tag filter; all markers if absent
//   inflate (number, optional) — extent multiplier for the framing box (default 5)
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleScreenshotMarkers(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_screenshot_markers: no editor world"));

    if (!GEditor)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_screenshot_markers: GEditor unavailable"));

    FString TagFilter;
    Params->TryGetStringField(TEXT("tag"), TagFilter);

    double InflateFactor = 5.0;
    Params->TryGetNumberField(TEXT("inflate"), InflateFactor);
    InflateFactor = FMath::Max(InflateFactor, 1.0);

    TArray<AServeGISReportMarker*> Markers;
    for (TActorIterator<AServeGISReportMarker> It(World); It; ++It)
    {
        if (!TagFilter.IsEmpty() && !It->ActorHasTag(FName(*TagFilter)))
            continue;
        Markers.Add(*It);
    }

    if (Markers.IsEmpty())
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_screenshot_markers: no matching markers in level"));

    FLevelEditorModule& LEM = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    TSharedPtr<ILevelEditor> LE = LEM.GetFirstLevelEditor();
    if (!LE.IsValid())
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_screenshot_markers: LevelEditor unavailable"));

    TSharedPtr<SLevelViewport> VP = LE->GetActiveViewportInterface();
    if (!VP.IsValid())
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_screenshot_markers: no active viewport"));

    FLevelEditorViewportClient* VC = &VP->GetLevelViewportClient();

    // Frustum half-tangents from viewport FOV
    const FIntPoint VPSize = VC->Viewport ? VC->Viewport->GetSizeXY() : FIntPoint(1920, 1080);
    const float Aspect   = VPSize.Y > 0 ? (float)VPSize.X / (float)VPSize.Y : (16.f / 9.f);
    const float HFovRad  = FMath::DegreesToRadians(FMath::Max(VC->ViewFOV, 10.f));
    const float HalfHTan = FMath::Tan(HFovRad * 0.5f);
    const float HalfVTan = FMath::Tan(2.f * FMath::Atan(HalfHTan / Aspect) * 0.5f);

    // Position the viewport camera to frame a box at the given pitch/yaw
    auto FrameBox = [&](const FBox& Box, float Pitch, float Yaw)
    {
        const float PR = FMath::DegreesToRadians(Pitch);
        const float YR = FMath::DegreesToRadians(Yaw);
        const FVector Fwd(FMath::Cos(PR) * FMath::Cos(YR),
                          FMath::Cos(PR) * FMath::Sin(YR),
                          FMath::Sin(PR));
        const FVector Rt  = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
        const FVector Up  = FVector::CrossProduct(Fwd, Rt).GetSafeNormal();
        const FVector Ctr = Box.GetCenter();
        const FVector Ext = Box.GetExtent();

        float DMin = 1.f;
        for (int32 i = 0; i < 8; ++i)
        {
            const FVector C = Ctr + FVector((i&1)?Ext.X:-Ext.X, (i&2)?Ext.Y:-Ext.Y, (i&4)?Ext.Z:-Ext.Z);
            const FVector D = C - Ctr;
            const float XC = FVector::DotProduct(D, Rt);
            const float YC = FVector::DotProduct(D, Up);
            const float ZD = FVector::DotProduct(D, Fwd);
            DMin = FMath::Max(DMin, FMath::Abs(XC) / HalfHTan - ZD);
            DMin = FMath::Max(DMin, FMath::Abs(YC) / HalfVTan - ZD);
        }
        VC->SetViewLocation(Ctr - Fwd * (DMin * 1.05f));
        VC->SetViewRotation(FRotator(Pitch, Yaw, 0.f));
        VC->Invalidate();
    };

    // Fire HighResShot and wait for the new file; copy it to DestPath
    const FString ShotDir = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir());
    IFileManager& FM = IFileManager::Get();

    auto TakeShot = [&](const FString& DestPath) -> bool
    {
        TSet<FString> Before;
        FM.IterateDirectory(*ShotDir, [&Before](const TCHAR* P, bool) -> bool
        {
            FString N = FPaths::GetCleanFilename(P);
            if (N.StartsWith(TEXT("Highres")) && N.EndsWith(TEXT(".png"))) Before.Add(N);
            return true;
        });

        // Brief pause so the game thread renders the repositioned viewport
        FPlatformProcess::Sleep(0.25f);

        AsyncTask(ENamedThreads::GameThread, []()
        {
            UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            GEngine->Exec(W, TEXT("HighResShot 1"));
        });

        FString NewFile;
        const double Deadline = FPlatformTime::Seconds() + 15.0;
        while (FPlatformTime::Seconds() < Deadline)
        {
            FPlatformProcess::Sleep(0.1f);
            FM.IterateDirectory(*ShotDir, [&](const TCHAR* P, bool) -> bool
            {
                FString N = FPaths::GetCleanFilename(P);
                if (N.StartsWith(TEXT("Highres")) && N.EndsWith(TEXT(".png")) && !Before.Contains(N))
                    NewFile = P;
                return true;
            });
            if (!NewFile.IsEmpty()) break;
        }

        if (NewFile.IsEmpty()) return false;
        FM.Copy(*DestPath, *NewFile);
        return true;
    };

    TArray<TSharedPtr<FJsonValue>> Results;

    for (int32 Idx = 0; Idx < Markers.Num(); ++Idx)
    {
        AServeGISReportMarker* M = Markers[Idx];

        FVector Origin, Extent;
        M->GetActorBounds(false, Origin, Extent);
        const FVector InfExt = Extent * (float)InflateFactor;
        const FBox MarkerBox(Origin - InfExt, Origin + InfExt);

        double Lat = 0.0, Lon = 0.0;
        M->GetLatLon(Lat, Lon);

        // Sanitise actor label for use in filenames
        FString Safe = M->GetActorLabel().Replace(TEXT(" "), TEXT("_"));
        for (TCHAR& Ch : Safe)
            if (!FChar::IsAlnum(Ch) && Ch != TEXT('_') && Ch != TEXT('-')) Ch = TEXT('_');

        const FString PerspPath = ShotDir / FString::Printf(TEXT("report_%02d_%s_persp.png"), Idx, *Safe);
        const FString TopPath   = ShotDir / FString::Printf(TEXT("report_%02d_%s_top.png"),   Idx, *Safe);

        FrameBox(MarkerBox, -45.f, 45.f);
        const bool bPerspOk = TakeShot(PerspPath);

        // pitch=-89.9 avoids the gimbal-lock singularity of exactly -90
        FrameBox(MarkerBox, -89.9f, 0.f);
        const bool bTopOk = TakeShot(TopPath);

        auto Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actor"),       M->GetActorLabel());
        Entry->SetStringField(TEXT("label"),       M->Label);
        Entry->SetNumberField(TEXT("lat"),         Lat);
        Entry->SetNumberField(TEXT("lon"),         Lon);
        Entry->SetStringField(TEXT("perspective"), bPerspOk ? PerspPath : TEXT("(failed)"));
        Entry->SetStringField(TEXT("topdown"),     bTopOk   ? TopPath   : TEXT("(failed)"));
        Results.Add(MakeShared<FJsonValueObject>(Entry));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("marker_count"), Markers.Num());
    R->SetArrayField(TEXT("markers"), Results);
    return R;
}

// ---------------------------------------------------------------------------
// gis_build_zone_graph
// Runs the full Tempo zone graph pipeline on the current level:
//   SetupZoneGraphBuilder → TryGenerateZoneShapeComponents → BuildZoneGraph
// GIS-imported ADynamicRoad actors implement ITempoRoadInterface and will
// have UZoneShapeComponents placed on them and the ZoneGraph rebuilt.
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleBuildZoneGraph(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_build_zone_graph: GEditor unavailable"));

    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_build_zone_graph: no editor world"));

    UTempoRoadLaneGraphSubsystem* LaneGraphSubsystem =
        GEditor->GetEditorSubsystem<UTempoRoadLaneGraphSubsystem>();
    if (!LaneGraphSubsystem)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("gis_build_zone_graph: UTempoRoadLaneGraphSubsystem unavailable — is TempoAgents enabled?"));

    // Step 1: register FTempoZoneGraphBuilder with the ZoneGraph subsystem.
    LaneGraphSubsystem->SetupZoneGraphBuilder();

    // Count ITempoRoadInterface actors so the caller gets a sanity-check number.
    int32 RoadCount = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (IsValid(*It) && (*It)->Implements<UTempoRoadInterface>())
            ++RoadCount;
    }

    // Step 2: walk all road/intersection actors and attach UZoneShapeComponents.
    if (!LaneGraphSubsystem->TryGenerateZoneShapeComponents())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(
                TEXT("gis_build_zone_graph: TryGenerateZoneShapeComponents failed (%d road actors found — check log for details)"),
                RoadCount));
    }

    // Step 3: trigger the ZoneGraph rebuild delegate.
    LaneGraphSubsystem->BuildZoneGraph();

    // Count UZoneShapeComponents now attached to actors — definitive proof shapes landed.
    int32 ZoneShapeCount = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (IsValid(*It))
        {
            TArray<UZoneShapeComponent*> Shapes;
            (*It)->GetComponents<UZoneShapeComponent>(Shapes);
            ZoneShapeCount += Shapes.Num();
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("road_actor_count"), RoadCount);
    R->SetNumberField(TEXT("zone_shape_count"), ZoneShapeCount);
    R->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Zone graph built: %d road actors, %d zone shapes placed"), RoadCount, ZoneShapeCount));
    return R;
}

