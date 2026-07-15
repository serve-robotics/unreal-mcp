#include "Commands/UnrealMCPGISCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "CityGenerationUtils.h"
#include "ProceduralRoadGen.h"

#include "Editor.h"
#include "TempoRoadLaneGraphSubsystem.h"
#include "TempoRoadInterface.h"
#include "ZoneShapeComponent.h"
#include "EditorAssetLibrary.h"
#include "EngineUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "LevelEditorSubsystem.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeStreamingProxy.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSplinesComponent.h"
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
#include "GISViewer/ServeRoadNetworkDiagnostics.h"

// RoadBLD
#include "DynamicRoad/DynamicRoadNetwork.h"
#include "DynamicRoad/DynamicRoad.h"
#include "DynamicRoad/DynamicRoadData.h"
#include "DynamicRoad/DynamicRoadIntersection.h"
#include "DynamicRoad/ClothoidSplineComponent.h"

#include "Engine/Blueprint.h"
#include "Settings/EditorLoadingSavingSettings.h"

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
    // --- Genuinely geospatial (GIS): keep the gis_ prefix ---
    if (CommandType == TEXT("gis_create_level"))       return HandleCreateLevel(Params);
    // TODO(rename): not geospatial — level lifecycle. Rename to level_open (has existing callers).
    if (CommandType == TEXT("gis_open_level"))         return HandleOpenLevel(Params);
    if (CommandType == TEXT("gis_get_geo_anchor"))     return HandleGetGeoAnchor(Params);
    if (CommandType == TEXT("gis_set_geo_anchor"))     return HandleSetGeoAnchor(Params);
    if (CommandType == TEXT("gis_import_opendrive"))   return HandleImportOpenDRIVE(Params);
    if (CommandType == TEXT("gis_list_road_networks")) return HandleListRoadNetworks(Params);
    if (CommandType == TEXT("gis_list_road_presets"))  return HandleListRoadPresets(Params);
    if (CommandType == TEXT("gis_viewer_load_file"))   return HandleViewerLoadFile(Params);
    if (CommandType == TEXT("gis_viewer_list_layers")) return HandleViewerListLayers(Params);
    if (CommandType == TEXT("gis_viewer_clear"))       return HandleViewerClear(Params);

    // TODO(rename): viewport framing, not geospatial. Rename to viewport_focus_landscapes (has existing callers).
    if (CommandType == TEXT("gis_focus_landscapes"))      return HandleFocusLandscapes(Params);
    // TODO(rename): viewport capture, not geospatial. Rename to viewport_screenshot_markers (has existing callers).
    if (CommandType == TEXT("gis_screenshot_markers"))    return HandleScreenshotMarkers(Params);
    // TODO(rename): operates on road-network actors, not GIS. Rename to roadnet_build_zone_graph (has existing callers).
    if (CommandType == TEXT("gis_build_zone_graph"))      return HandleBuildZoneGraph(Params);

    // --- Road-network / ZoneGraph diagnostics: no GIS dependency (work on grid-town levels too) ---
    if (CommandType == TEXT("roadnet_summarize_semantic"))  return HandleSummarizeRoadNetworkSemantic(Params);
    if (CommandType == TEXT("roadnet_summarize_lane_graph")) return HandleSummarizeLaneGraph(Params);
    if (CommandType == TEXT("roadnet_validate"))            return HandleValidateRoadNetwork(Params);
    if (CommandType == TEXT("roadnet_reconcile"))           return HandleReconcileRoadNetwork(Params);
    if (CommandType == TEXT("roadnet_list_markers"))        return HandleListReportMarkers(Params);

    // --- Editor-viewport visualization: pure UI (show flags + framed capture), no GIS ---
    if (CommandType == TEXT("viewport_screenshot_zone_graph")) return HandleScreenshotZoneGraph(Params);

    // City block and building generation
    if (CommandType == TEXT("gis_list_districts"))       return HandleListDistricts(Params);
    if (CommandType == TEXT("gis_generate_block_shapes")) return HandleGenerateBlockShapes(Params);
    if (CommandType == TEXT("gis_assign_district"))      return HandleAssignDistrict(Params);
    if (CommandType == TEXT("gis_generate_buildings"))   return HandleGenerateBuildings(Params);
    if (CommandType == TEXT("gis_generate_procedural_roads")) return HandleGenerateProceduralRoads(Params);

    // Sidewalk theming
    if (CommandType == TEXT("gis_list_sidewalk_presets")) return HandleListSidewalkPresets(Params);
    if (CommandType == TEXT("gis_set_road_sidewalk"))     return HandleSetRoadSidewalk(Params);

    // Landscape conformation
    if (CommandType == TEXT("gis_conform_landscape_to_roads")) return HandleConformLandscapeToRoads(Params);

    return FUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown GIS command: %s"), *CommandType));
}

// ---------------------------------------------------------------------------
// gis_create_level
// ---------------------------------------------------------------------------

// Built-in Open World template: WorldPartition + landscape + sky/lighting already configured.
static const TCHAR* kDefaultLevelTemplate = TEXT("/Engine/Maps/Templates/OpenWorld");

// Break the cyclic ControlPoint<->Segment ring graph on all landscape spline data in
// the current world. Must be called before loading a new level to avoid
// FArchiveGatherExternalActorRefs stack-overflowing during world destruction.
//
// We clear the CP/segment arrays rather than calling Destroy() on the actors.
// Destroy() during an MCP game-thread tick task modifies ALandscapeSplineActor tick
// registration, which then collides with the TickTaskManager state during the subsequent
// NewLevelFromTemplate, triggering a !LevelList.Contains(TickTaskLevel) assertion crash.
static void ClearLandscapeSplineGraph(UWorld* World)
{
    if (!IsValid(World)) return;

    // Path 1: ALandscapeSplineActor (UE5 WP-style standalone spline actors).
    for (TActorIterator<ALandscapeSplineActor> It(World); It; ++It)
    {
        if (!IsValid(*It)) continue;
        if (ULandscapeSplinesComponent* SC = (*It)->GetSplinesComponent())
        {
            SC->GetControlPoints().Empty();
            SC->GetSegments().Empty();
        }
    }

    // Path 2: ULandscapeSplinesComponent on ALandscapeProxy actors (legacy path).
    for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
    {
        if (!IsValid(*It)) continue;
        if (ULandscapeSplinesComponent* SC = It->GetSplinesComponent())
        {
            if (SC->HasAnyControlPointsOrSegments())
            {
                SC->GetControlPoints().Empty();
                SC->GetSegments().Empty();
            }
        }
    }
}

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleCreateLevel(const TSharedPtr<FJsonObject>& Params)
{
    FString LevelPath;
    if (!Params->TryGetStringField(TEXT("level_path"), LevelPath))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_create_level: level_path required (e.g. /Game/Maps/MyCity)"));

    ULevelEditorSubsystem* LES = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    if (!LES)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_create_level: LevelEditorSubsystem unavailable"));

    // Optional template. Defaults to the Open World template. Pass template_path:"open_world" or the
    // full engine path. Pass template_path:"" (empty) for a truly blank level.
    FString TemplatePath = kDefaultLevelTemplate;
    Params->TryGetStringField(TEXT("template_path"), TemplatePath);
    if (TemplatePath.Equals(TEXT("open_world"), ESearchCase::IgnoreCase))
        TemplatePath = TEXT("/Engine/Maps/Templates/OpenWorld");

    // NewLevel/NewLevelFromTemplate refuse to overwrite an existing asset. Pass overwrite:true to
    // delete the existing level first. (Default false so a typo can't silently nuke a level.)
    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bDeletedExisting = false;
    if (UEditorAssetLibrary::DoesAssetExist(LevelPath))
    {
        if (!bOverwrite)
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("gis_create_level: '%s' already exists (pass overwrite:true to replace it)"), *LevelPath));

        // Move off the current level first if it's the target, so nothing holds the package open.
        if (UWorld* CurrentWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
        {
            const FString CurrentPkg = CurrentWorld->GetOutermost()->GetName();
            const FString TargetPkg = FPackageName::ObjectPathToPackageName(LevelPath);
            if (CurrentPkg == TargetPkg)
            {
                LES->NewLevel(TEXT(""));
            }
        }

        if (!UEditorAssetLibrary::DeleteAsset(LevelPath))
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("gis_create_level: failed to delete existing '%s' for overwrite "
                                     "(package may still be loaded/referenced)"), *LevelPath));
        bDeletedExisting = true;
    }

    // Break the landscape spline ring graph before loading the new level.
    // Without this, EditorDestroyWorld hits FArchiveGatherExternalActorRefs recursion on
    // the cyclic ControlPoint<->Segment graph left behind by RebuildRoadNetworkIncremental.
    ClearLandscapeSplineGraph(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr);

    bool bOk;
    if (TemplatePath.IsEmpty())
    {
        bOk = LES->NewLevel(LevelPath);
    }
    else
    {
        bOk = LES->NewLevelFromTemplate(LevelPath, TemplatePath);
    }

    if (!bOk)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_create_level: failed to create '%s'%s"),
                *LevelPath,
                TemplatePath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" from template '%s'"), *TemplatePath)));

    // Always disable OFPA (One File Per Actor) so all actors live in the main package.
    // With OFPA on, saving triggers FArchiveGatherExternalActorRefs which recursively
    // serializes ULandscapeSplineSegment/ULandscapeSplineControlPoint cycles (ring graph
    // from the road rebuild) and stack-overflows. Actors are still WP-streamable without
    // OFPA; streaming is cell-based, not file-based.
    if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
        if (ULevel* Level = World->GetCurrentLevel())
            Level->bUseExternalActors = false;

    // Optionally strip the template's landscape. GIS levels import their own terrain via
    // gis_import_landscape, so keeping the template landscape leaves two overlapping landscapes.
    // Opt-in (default off) — procedural levels reuse the template landscape as their terrain.
    int32 StrippedLandscapes = 0;
    bool bStripLandscape = false;
    Params->TryGetBoolField(TEXT("strip_landscape"), bStripLandscape);
    if (bStripLandscape)
    {
        if (UWorld* World = GetEditorWorld())
        {
            TArray<ALandscapeProxy*> ToDestroy;
            for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
            {
                ToDestroy.Add(*It);
            }
            for (ALandscapeProxy* Proxy : ToDestroy)
            {
                if (IsValid(Proxy) && World->EditorDestroyActor(Proxy, /*bShouldModifyLevel=*/true))
                {
                    ++StrippedLandscapes;
                }
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetStringField(TEXT("level_path"), LevelPath);
    if (!TemplatePath.IsEmpty())
        R->SetStringField(TEXT("template_path"), TemplatePath);
    R->SetBoolField(TEXT("overwrote_existing"), bDeletedExisting);
    if (bStripLandscape)
        R->SetNumberField(TEXT("stripped_landscapes"), StrippedLandscapes);
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
// viewport_screenshot_zone_graph
// Takes perspective (-45°/45°) and top-down (-89.9°/0°) screenshots of the
// current level with the Navigation show flag enabled so ZoneGraph lane shapes
// are visible (Walkable=red, DrivingLane=purple, ClosedLane=blue, etc.).
// The show flag is restored to its previous state after the shots are taken.
// Returns: { "perspective": "<path>", "topdown": "<path>" }
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleScreenshotZoneGraph(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("viewport_screenshot_zone_graph: no editor world"));
    if (!GEditor)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("viewport_screenshot_zone_graph: GEditor unavailable"));

    // Build landscape bounding box (same logic as gis_focus_landscapes)
    FBox SceneBox(ForceInit);
    int32 LandscapeCount = 0;
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        FVector O, E;
        It->GetActorBounds(false, O, E);
        SceneBox += FBox(O - E, O + E);
        ++LandscapeCount;
    }
    if (LandscapeCount == 0)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("viewport_screenshot_zone_graph: no Landscape actors in level"));

    FLevelEditorModule& LEM = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    TSharedPtr<ILevelEditor> LE = LEM.GetFirstLevelEditor();
    if (!LE.IsValid())
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("viewport_screenshot_zone_graph: LevelEditor unavailable"));

    TSharedPtr<SLevelViewport> VP = LE->GetActiveViewportInterface();
    if (!VP.IsValid())
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("viewport_screenshot_zone_graph: no active viewport"));

    FLevelEditorViewportClient* VC = &VP->GetLevelViewportClient();

    // Enable Navigation show flag (drives ZoneGraph lane rendering) and remember previous state
    const bool bWasNavigation = VC->EngineShowFlags.Navigation != 0;
    VC->EngineShowFlags.SetNavigation(true);
    VC->Invalidate();

    // Optional close-up: if "location" and "extent" are supplied, frame a custom box instead of the landscape.
    const TSharedPtr<FJsonObject>* LocObj;
    if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
    {
        double X = 0, Y = 0, Z = 0, Ext = 2000;
        (*LocObj)->TryGetNumberField(TEXT("x"), X);
        (*LocObj)->TryGetNumberField(TEXT("y"), Y);
        (*LocObj)->TryGetNumberField(TEXT("z"), Z);
        Params->TryGetNumberField(TEXT("extent"), Ext);
        SceneBox = FBox(FVector(X - Ext, Y - Ext, Z - Ext*0.5), FVector(X + Ext, Y + Ext, Z + Ext*0.5));
    }

    // Viewport FOV helpers
    const FIntPoint VPSize  = VC->Viewport ? VC->Viewport->GetSizeXY() : FIntPoint(1920, 1080);
    const float Aspect      = VPSize.Y > 0 ? (float)VPSize.X / (float)VPSize.Y : (16.f / 9.f);
    const float HFovRad     = FMath::DegreesToRadians(FMath::Max(VC->ViewFOV, 10.f));
    const float HalfHTan    = FMath::Tan(HFovRad * 0.5f);
    const float HalfVTan    = FMath::Tan(2.f * FMath::Atan(HalfHTan / Aspect) * 0.5f);

    auto FrameBox = [&](const FBox& Box, float Pitch, float Yaw)
    {
        const float PR = FMath::DegreesToRadians(Pitch);
        const float YR = FMath::DegreesToRadians(Yaw);
        const FVector Fwd(FMath::Cos(PR)*FMath::Cos(YR), FMath::Cos(PR)*FMath::Sin(YR), FMath::Sin(PR));
        const FVector Rt = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
        const FVector Up = FVector::CrossProduct(Fwd, Rt).GetSafeNormal();
        const FVector Ctr = Box.GetCenter();
        const FVector Ext = Box.GetExtent();
        float DMin = 1.f;
        for (int32 i = 0; i < 8; ++i)
        {
            const FVector C = Ctr + FVector((i&1)?Ext.X:-Ext.X,(i&2)?Ext.Y:-Ext.Y,(i&4)?Ext.Z:-Ext.Z);
            const FVector D = C - Ctr;
            DMin = FMath::Max(DMin, FMath::Abs(FVector::DotProduct(D, Rt)) / HalfHTan - FVector::DotProduct(D, Fwd));
            DMin = FMath::Max(DMin, FMath::Abs(FVector::DotProduct(D, Up)) / HalfVTan - FVector::DotProduct(D, Fwd));
        }
        VC->SetViewLocation(Ctr - Fwd * (DMin * 1.05f));
        VC->SetViewRotation(FRotator(Pitch, Yaw, 0.f));
        VC->Invalidate();
    };

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
        FPlatformProcess::Sleep(0.3f); // let the viewport render with nav flag enabled
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

    const FString PerspPath = ShotDir / TEXT("zonegraph_persp.png");
    const FString TopPath   = ShotDir / TEXT("zonegraph_top.png");

    FrameBox(SceneBox, -45.f, 45.f);
    const bool bPerspOk = TakeShot(PerspPath);

    FrameBox(SceneBox, -89.9f, 0.f);
    const bool bTopOk = TakeShot(TopPath);

    // Restore Navigation show flag
    VC->EngineShowFlags.SetNavigation(bWasNavigation);
    VC->Invalidate();

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), bPerspOk || bTopOk);
    R->SetStringField(TEXT("perspective"), bPerspOk ? PerspPath : TEXT("(failed)"));
    R->SetStringField(TEXT("topdown"),     bTopOk   ? TopPath   : TEXT("(failed)"));
    R->SetStringField(TEXT("framing"),     Params->HasField(TEXT("location")) ? TEXT("closeup") : TEXT("landscape"));
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

    // Disable autosave while ZoneGraph runs.  DetectAndStoreEdgeIntersections /
    // CreatePerimeterCuts read road spline geometry, which can trigger mirror-spline
    // refreshes that re-fire RebuildRoadNetworkIncremental in the background.
    // A concurrent autosave would race with those workers on the spline objects.
    // gis_rebuild_road_networks (step 7.5) re-enables autosave after the rebuild drains.
    if (UEditorLoadingSavingSettings* Cfg = GetMutableDefault<UEditorLoadingSavingSettings>())
    {
        Cfg->bAutoSaveEnable = false;
        UE_LOG(LogUnrealMCP, Log, TEXT("gis_build_zone_graph: autosave disabled until post-ZoneGraph rebuild drains"));
    }

    // Step 1: ensure perimeter cuts are up-to-date on all road networks.
    // The modern async rebuild (RebuildRoadNetworkIncremental) stores its intersection data
    // in a temporary FRebuildContext and never writes to the legacy RoadNetworkCorners /
    // RoadNetworkPerimeterCuts arrays that SpawnTempoIntersectionActors reads.
    // Calling DetectAndStoreEdgeIntersections populates RoadNetworkCorners from committed
    // road edge geometry; CreatePerimeterCuts then derives RoadNetworkPerimeterCuts from those.
    for (TActorIterator<ADynamicRoadNetwork> It(World); It; ++It)
    {
        if (IsValid(*It))
        {
            (*It)->DetectAndStoreEdgeIntersections();
            (*It)->CreatePerimeterCuts();
        }
    }

    // Step 2: spawn one ADynamicRoadIntersection actor per road junction so Tempo's
    // zone graph builder can generate intersection polygon zones and connect road lanes.
    int32 IntersectionCount = 0;
    for (TActorIterator<ADynamicRoadNetwork> It(World); It; ++It)
    {
        if (IsValid(*It))
            IntersectionCount += (*It)->SpawnTempoIntersectionActors(World);
    }

    // Step 2: register FTempoZoneGraphBuilder with the ZoneGraph subsystem.
    LaneGraphSubsystem->SetupZoneGraphBuilder();

    // Count ITempoRoadInterface actors so the caller gets a sanity-check number.
    int32 RoadCount = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (IsValid(*It) && (*It)->Implements<UTempoRoadInterface>())
            ++RoadCount;
    }

    // Step 3: walk all road/intersection actors and attach UZoneShapeComponents.
    if (!LaneGraphSubsystem->TryGenerateZoneShapeComponents())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(
                TEXT("gis_build_zone_graph: TryGenerateZoneShapeComponents failed (%d road actors found — check log for details)"),
                RoadCount));
    }

    // Step 4: trigger the ZoneGraph rebuild delegate.
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
    R->SetNumberField(TEXT("intersection_actor_count"), IntersectionCount);
    R->SetNumberField(TEXT("zone_shape_count"), ZoneShapeCount);
    R->SetStringField(TEXT("message"),
        FString::Printf(TEXT("Zone graph built: %d road actors, %d intersection actors, %d zone shapes placed"),
            RoadCount, IntersectionCount, ZoneShapeCount));
    return R;
}

// ---------------------------------------------------------------------------
// Road-network diagnostics — shared JSON helpers
// ---------------------------------------------------------------------------

namespace DiagJson
{
    static TSharedPtr<FJsonObject> IssueToJson(const FServeRoadDiagIssue& Issue)
    {
        static auto SevStr = [](EServeRoadDiagSeverity S) -> FString {
            switch (S) {
                case EServeRoadDiagSeverity::Info:    return TEXT("Info");
                case EServeRoadDiagSeverity::Warning: return TEXT("Warning");
                case EServeRoadDiagSeverity::Error:   return TEXT("Error");
            }
            return TEXT("Unknown");
        };
        static auto CatStr = [](EServeRoadDiagCategory C) -> FString {
            switch (C) {
                case EServeRoadDiagCategory::Topology:       return TEXT("Topology");
                case EServeRoadDiagCategory::Geometry:       return TEXT("Geometry");
                case EServeRoadDiagCategory::Tagging:        return TEXT("Tagging");
                case EServeRoadDiagCategory::Connectivity:   return TEXT("Connectivity");
                case EServeRoadDiagCategory::LaneCount:      return TEXT("LaneCount");
                case EServeRoadDiagCategory::Reconciliation: return TEXT("Reconciliation");
                case EServeRoadDiagCategory::Subsystem:      return TEXT("Subsystem");
            }
            return TEXT("Unknown");
        };

        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("severity"),  SevStr(Issue.Severity));
        J->SetStringField(TEXT("category"),  CatStr(Issue.Category));
        J->SetStringField(TEXT("message"),   Issue.Message);
        J->SetStringField(TEXT("entity_id"), Issue.EntityId);

        TArray<TSharedPtr<FJsonValue>> Loc;
        Loc.Add(MakeShared<FJsonValueNumber>(Issue.Location.X));
        Loc.Add(MakeShared<FJsonValueNumber>(Issue.Location.Y));
        Loc.Add(MakeShared<FJsonValueNumber>(Issue.Location.Z));
        J->SetArrayField(TEXT("location"), Loc);
        return J;
    }

    static TSharedPtr<FJsonObject> ReportToJson(const FServeRoadDiagReport& R)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("errors"),   R.ErrorCount);
        J->SetNumberField(TEXT("warnings"), R.WarningCount);
        J->SetNumberField(TEXT("infos"),    R.InfoCount);

        TArray<TSharedPtr<FJsonValue>> IssueArr;
        for (const FServeRoadDiagIssue& Issue : R.Issues)
            IssueArr.Add(MakeShared<FJsonValueObject>(IssueToJson(Issue)));
        J->SetArrayField(TEXT("issues"), IssueArr);
        return J;
    }

    static TSharedPtr<FJsonObject> Vec3ToJson(const FVector& V)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("x"), V.X);
        J->SetNumberField(TEXT("y"), V.Y);
        J->SetNumberField(TEXT("z"), V.Z);
        return J;
    }

    /** Parse common filter params from command params. */
    static FServeRoadDiagFilter ParseFilter(const TSharedPtr<FJsonObject>& Params)
    {
        FServeRoadDiagFilter F;

        FString NetLabel;
        if (Params->TryGetStringField(TEXT("network_name"), NetLabel))
            F.NetworkLabel = NetLabel;

        bool bSummaryOnly = false;
        if (Params->TryGetBoolField(TEXT("summary_only"), bSummaryOnly))
            F.bIncludePerEntityArrays = !bSummaryOnly;

        double MaxEnt = 0.0;
        if (Params->TryGetNumberField(TEXT("max_entities"), MaxEnt))
            F.MaxEntities = (int32)MaxEnt;

        FString TagStr;
        if (Params->TryGetStringField(TEXT("tag"), TagStr) && !TagStr.IsEmpty())
            F.RequiredTag = FName(*TagStr);

        const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
        if (Params->TryGetObjectField(TEXT("bounds"), BoundsObj) && BoundsObj)
        {
            const TSharedPtr<FJsonObject>* MinObj = nullptr;
            const TSharedPtr<FJsonObject>* MaxObj = nullptr;
            if ((*BoundsObj)->TryGetObjectField(TEXT("min"), MinObj) &&
                (*BoundsObj)->TryGetObjectField(TEXT("max"), MaxObj))
            {
                double MinX = 0, MinY = 0, MinZ = 0, MaxX = 0, MaxY = 0, MaxZ = 0;
                (*MinObj)->TryGetNumberField(TEXT("x"), MinX);
                (*MinObj)->TryGetNumberField(TEXT("y"), MinY);
                (*MinObj)->TryGetNumberField(TEXT("z"), MinZ);
                (*MaxObj)->TryGetNumberField(TEXT("x"), MaxX);
                (*MaxObj)->TryGetNumberField(TEXT("y"), MaxY);
                (*MaxObj)->TryGetNumberField(TEXT("z"), MaxZ);
                F.Bounds    = FBox(FVector(MinX, MinY, MinZ), FVector(MaxX, MaxY, MaxZ));
                F.bHasBounds = true;
            }
        }

        return F;
    }
} // namespace DiagJson

// ---------------------------------------------------------------------------
// roadnet_summarize_semantic
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleSummarizeRoadNetworkSemantic(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("roadnet_summarize_semantic: no editor world"));

    FServeRoadDiagFilter Filter = DiagJson::ParseFilter(Params);
    FServeSemanticSummary Sum   = FServeRoadNetworkDiagnostics::GatherSemanticSummary(World, Filter);
    FServeRoadDiagReport  Val   = FServeRoadNetworkDiagnostics::ValidateSemantic(World, Filter);

    bool bSpawnMarkers = true;
    Params->TryGetBoolField(TEXT("spawn_markers"), bSpawnMarkers);
    int32 SpawnedCount = 0;
    if (bSpawnMarkers)
    {
        static const FName Tag(TEXT("ServeRoadDiag:Semantic"));
        FServeRoadNetworkDiagnostics::ClearDiagMarkers(World, Tag);
        SpawnedCount = FServeRoadNetworkDiagnostics::SpawnDiagMarkers(World, Tag, Val);
    }

    // --- Build JSON summary block ---
    auto SumJ = MakeShared<FJsonObject>();
    SumJ->SetStringField(TEXT("network_label"),              Sum.NetworkLabel);
    SumJ->SetNumberField(TEXT("road_count"),                 Sum.RoadCount);
    SumJ->SetNumberField(TEXT("intersection_actor_count"),   Sum.IntersectionActorCount);
    SumJ->SetNumberField(TEXT("perimeter_cut_count"),        Sum.PerimeterCutCount);
    SumJ->SetNumberField(TEXT("corner_count"),               Sum.CornerCount);
    SumJ->SetNumberField(TEXT("intersection_mask_count"),    Sum.IntersectionMaskCount);
    SumJ->SetNumberField(TEXT("total_road_length_cm"),       Sum.TotalRoadLength);
    SumJ->SetNumberField(TEXT("sidewalk_road_count"),        Sum.SidewalkRoadCount);
    SumJ->SetNumberField(TEXT("total_crosswalk_count"),      Sum.TotalCrosswalkCount);
    SumJ->SetBoolField  (TEXT("intersection_actors_present"), Sum.bIntersectionActorsPresent);

    // Per-entity road array
    TArray<TSharedPtr<FJsonValue>> RoadsArr;
    for (const FServeSemRoadInfo& Road : Sum.Roads)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("road_id"),              Road.RoadId);
        J->SetStringField(TEXT("actor_label"),          Road.ActorLabel);
        J->SetStringField(TEXT("road_type"),            Road.RoadType);
        J->SetNumberField(TEXT("length_cm"),            Road.Length);
        J->SetNumberField(TEXT("left_lane_count"),      Road.LeftLaneCount);
        J->SetNumberField(TEXT("right_lane_count"),     Road.RightLaneCount);
        J->SetNumberField(TEXT("navigable_lane_count"), Road.NavigableLaneCount);
        J->SetBoolField  (TEXT("has_left_sidewalk"),    Road.bHasLeftSidewalk);
        J->SetBoolField  (TEXT("has_right_sidewalk"),   Road.bHasRightSidewalk);
        J->SetObjectField(TEXT("start"),                DiagJson::Vec3ToJson(Road.StartLocation));
        J->SetObjectField(TEXT("end"),                  DiagJson::Vec3ToJson(Road.EndLocation));
        RoadsArr.Add(MakeShared<FJsonValueObject>(J));
    }

    // Per-entity intersection array
    TArray<TSharedPtr<FJsonValue>> IntsArr;
    for (const FServeSemIntersectionInfo& Int : Sum.Intersections)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("intersection_id"),      Int.IntersectionId);
        J->SetStringField(TEXT("actor_label"),          Int.ActorLabel);
        J->SetObjectField(TEXT("centroid"),             DiagJson::Vec3ToJson(Int.Centroid));
        J->SetNumberField(TEXT("approach_count"),       Int.ApproachCount);
        J->SetNumberField(TEXT("left_turn_pairs"),      Int.LeftTurnPairCount);
        J->SetNumberField(TEXT("right_turn_pairs"),     Int.RightTurnPairCount);
        J->SetNumberField(TEXT("through_pairs"),        Int.ThroughPairCount);
        J->SetNumberField(TEXT("crosswalk_count"),      Int.CrosswalkCount);
        J->SetNumberField(TEXT("sidewalk_module_count"), Int.SidewalkModuleCount);

        TArray<TSharedPtr<FJsonValue>> ApproachRoads;
        for (const FString& Id : Int.ApproachRoadIds)
            ApproachRoads.Add(MakeShared<FJsonValueString>(Id));
        J->SetArrayField(TEXT("approach_road_ids"), ApproachRoads);

        IntsArr.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),     true);
    R->SetObjectField(TEXT("summary"),     SumJ);
    R->SetArrayField (TEXT("roads"),       RoadsArr);
    R->SetArrayField (TEXT("intersections"), IntsArr);
    R->SetObjectField(TEXT("validation"),  DiagJson::ReportToJson(Val));
    R->SetBoolField  (TEXT("markers_spawned"), bSpawnMarkers);
    R->SetNumberField(TEXT("marker_count"),    SpawnedCount);
    return R;
}

// ---------------------------------------------------------------------------
// roadnet_summarize_lane_graph
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleSummarizeLaneGraph(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("roadnet_summarize_lane_graph: no editor world"));

    FServeRoadDiagFilter Filter = DiagJson::ParseFilter(Params);
    FServeZoneGraphSummary Sum  = FServeRoadNetworkDiagnostics::GatherZoneGraphSummary(World, Filter);
    FServeRoadDiagReport   Val  = FServeRoadNetworkDiagnostics::ValidateZoneGraph(World, Filter);

    bool bSpawnMarkers = false; // summary commands default off
    Params->TryGetBoolField(TEXT("spawn_markers"), bSpawnMarkers);
    int32 SpawnedCount = 0;
    if (bSpawnMarkers)
    {
        static const FName Tag(TEXT("ServeRoadDiag:ZoneGraph"));
        FServeRoadNetworkDiagnostics::ClearDiagMarkers(World, Tag);
        SpawnedCount = FServeRoadNetworkDiagnostics::SpawnDiagMarkers(World, Tag, Val);
    }

    // Connectivity block
    auto ConnJ = MakeShared<FJsonObject>();
    ConnJ->SetNumberField(TEXT("component_count"),            Sum.Connectivity.ComponentCount);
    ConnJ->SetNumberField(TEXT("largest_component_lane_count"), Sum.Connectivity.LargestComponentLaneCount);
    ConnJ->SetNumberField(TEXT("driving_component_count"),    Sum.Connectivity.DrivingComponentCount);

    TArray<TSharedPtr<FJsonValue>> CompArr;
    for (const FServeZGComponent& C : Sum.Connectivity.Components)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("component_id"),         C.ComponentId);
        J->SetNumberField(TEXT("lane_count"),           C.LaneCount);
        J->SetNumberField(TEXT("driving_lane_count"),   C.DrivingLaneCount);
        J->SetObjectField(TEXT("representative_point"), DiagJson::Vec3ToJson(C.RepresentativePoint));
        CompArr.Add(MakeShared<FJsonValueObject>(J));
    }
    ConnJ->SetArrayField(TEXT("components"), CompArr);

    // Summary block
    auto SumJ = MakeShared<FJsonObject>();
    SumJ->SetBoolField  (TEXT("subsystem_present"),       Sum.bSubsystemPresent);
    SumJ->SetBoolField  (TEXT("data_registered"),         Sum.bDataRegistered);
    SumJ->SetNumberField(TEXT("registered_data_count"),   Sum.RegisteredDataCount);
    SumJ->SetNumberField(TEXT("zone_count"),              Sum.ZoneCount);
    SumJ->SetNumberField(TEXT("lane_count"),              Sum.LaneCount);
    SumJ->SetNumberField(TEXT("lane_link_count"),         Sum.LaneLinkCount);
    SumJ->SetNumberField(TEXT("intersection_zone_count"), Sum.IntersectionZoneCount);
    SumJ->SetNumberField(TEXT("driving_lane_count"),      Sum.DrivingLaneCount);
    SumJ->SetNumberField(TEXT("walkable_lane_count"),     Sum.WalkableLaneCount);
    SumJ->SetNumberField(TEXT("crosswalk_lane_count"),    Sum.CrosswalkLaneCount);
    SumJ->SetNumberField(TEXT("isolated_lane_count"),     Sum.IsolatedLaneCount);
    SumJ->SetNumberField(TEXT("total_lane_length_cm"),    Sum.TotalLaneLength);

    // Per-entity zone array
    TArray<TSharedPtr<FJsonValue>> ZonesArr;
    for (const FServeZGZoneInfo& Z : Sum.Zones)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("zone_index"),     Z.ZoneIndex);
        J->SetStringField(TEXT("zone_id"),        Z.ZoneId);
        J->SetBoolField  (TEXT("is_intersection"), Z.bIsIntersection);
        J->SetNumberField(TEXT("lane_count"),     Z.LaneCount);
        TArray<TSharedPtr<FJsonValue>> TagArr;
        for (const FString& T : Z.Tags) TagArr.Add(MakeShared<FJsonValueString>(T));
        J->SetArrayField (TEXT("tags"), TagArr);
        ZonesArr.Add(MakeShared<FJsonValueObject>(J));
    }

    // Per-entity lane array
    TArray<TSharedPtr<FJsonValue>> LanesArr;
    for (const FServeZGLaneInfo& L : Sum.Lanes)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("lane_index"),     L.LaneIndex);
        J->SetStringField(TEXT("lane_id"),        L.LaneId);
        J->SetNumberField(TEXT("zone_index"),     L.ZoneIndex);
        J->SetNumberField(TEXT("width"),          L.Width);
        J->SetNumberField(TEXT("length_cm"),      L.Length);
        J->SetNumberField(TEXT("outgoing"),       L.OutgoingCount);
        J->SetNumberField(TEXT("incoming"),       L.IncomingCount);
        J->SetNumberField(TEXT("adjacent"),       L.AdjacentCount);
        J->SetNumberField(TEXT("component_id"),   L.ComponentId);
        J->SetObjectField(TEXT("start"),          DiagJson::Vec3ToJson(L.StartPoint));
        J->SetObjectField(TEXT("end"),            DiagJson::Vec3ToJson(L.EndPoint));

        TArray<TSharedPtr<FJsonValue>> TagArr;
        for (const FString& T : L.Tags) TagArr.Add(MakeShared<FJsonValueString>(T));
        J->SetArrayField(TEXT("tags"), TagArr);

        TArray<TSharedPtr<FJsonValue>> OutArr;
        for (int32 D : L.OutgoingDestLanes) OutArr.Add(MakeShared<FJsonValueNumber>(D));
        J->SetArrayField(TEXT("outgoing_dest_lanes"), OutArr);

        LanesArr.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),      true);
    R->SetObjectField(TEXT("summary"),      SumJ);
    R->SetObjectField(TEXT("connectivity"), ConnJ);
    R->SetArrayField (TEXT("zones"),        ZonesArr);
    R->SetArrayField (TEXT("lanes"),        LanesArr);
    R->SetObjectField(TEXT("validation"),   DiagJson::ReportToJson(Val));
    R->SetBoolField  (TEXT("markers_spawned"), bSpawnMarkers);
    R->SetNumberField(TEXT("marker_count"),    SpawnedCount);
    return R;
}

// ---------------------------------------------------------------------------
// roadnet_validate
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleValidateRoadNetwork(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("roadnet_validate: no editor world"));

    FServeRoadDiagFilter Filter = DiagJson::ParseFilter(Params);

    FServeRoadDiagReport SemVal  = FServeRoadNetworkDiagnostics::ValidateSemantic(World, Filter);
    FServeRoadDiagReport ZGVal   = FServeRoadNetworkDiagnostics::ValidateZoneGraph(World, Filter);
    FServeRoadDiagReport CWVal   = FServeRoadNetworkDiagnostics::ValidateCrosswalkConnectivity(World, Filter);
    FServeRoadDiagReport IntVal  = FServeRoadNetworkDiagnostics::ValidateIntersectionSelfContainment(World, Filter);

    bool bSpawnMarkers = true;
    Params->TryGetBoolField(TEXT("spawn_markers"), bSpawnMarkers);

    int32 TotalSpawned = 0;
    if (bSpawnMarkers)
    {
        auto Spawn = [&](FName Tag, const FServeRoadDiagReport& Rep) {
            FServeRoadNetworkDiagnostics::ClearDiagMarkers(World, Tag);
            TotalSpawned += FServeRoadNetworkDiagnostics::SpawnDiagMarkers(World, Tag, Rep);
        };
        Spawn(FName(TEXT("ServeRoadDiag:Semantic")),     SemVal);
        Spawn(FName(TEXT("ServeRoadDiag:ZoneGraph")),    ZGVal);
        Spawn(FName(TEXT("ServeRoadDiag:Crosswalk")),    CWVal);
        Spawn(FName(TEXT("ServeRoadDiag:Intersection")), IntVal);
    }

    const bool bPassed = (SemVal.ErrorCount + ZGVal.ErrorCount +
                          CWVal.ErrorCount + IntVal.ErrorCount) == 0;

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),                true);
    R->SetBoolField  (TEXT("passed"),                 bPassed);
    R->SetObjectField(TEXT("semantic_validation"),    DiagJson::ReportToJson(SemVal));
    R->SetObjectField(TEXT("zonegraph_validation"),   DiagJson::ReportToJson(ZGVal));
    R->SetObjectField(TEXT("crosswalk_validation"),   DiagJson::ReportToJson(CWVal));
    R->SetObjectField(TEXT("intersection_validation"), DiagJson::ReportToJson(IntVal));
    R->SetBoolField  (TEXT("markers_spawned"),        bSpawnMarkers);
    R->SetNumberField(TEXT("marker_count"),           TotalSpawned);

    TArray<TSharedPtr<FJsonValue>> MarkerTagsArr;
    if (bSpawnMarkers)
    {
        for (const TCHAR* T : { TEXT("ServeRoadDiag:Semantic"), TEXT("ServeRoadDiag:ZoneGraph"),
                                 TEXT("ServeRoadDiag:Crosswalk"), TEXT("ServeRoadDiag:Intersection") })
            MarkerTagsArr.Add(MakeShared<FJsonValueString>(T));
    }
    R->SetArrayField(TEXT("marker_tags"), MarkerTagsArr);
    return R;
}

// ---------------------------------------------------------------------------
// roadnet_reconcile
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleReconcileRoadNetwork(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("roadnet_reconcile: no editor world"));

    FServeRoadDiagFilter Filter = DiagJson::ParseFilter(Params);

    double SnapDist = 5000.0;
    Params->TryGetNumberField(TEXT("snap_distance_cm"), SnapDist);

    FServeReconcileSummary Rec = FServeRoadNetworkDiagnostics::Reconcile(World, Filter, SnapDist);

    bool bSpawnMarkers = true;
    Params->TryGetBoolField(TEXT("spawn_markers"), bSpawnMarkers);
    int32 SpawnedCount = 0;
    if (bSpawnMarkers)
    {
        static const FName Tag(TEXT("ServeRoadDiag:Reconcile"));
        FServeRoadNetworkDiagnostics::ClearDiagMarkers(World, Tag);
        SpawnedCount = FServeRoadNetworkDiagnostics::SpawnDiagMarkers(World, Tag, Rec.Report);
    }

    auto CountsJ = MakeShared<FJsonObject>();
    CountsJ->SetNumberField(TEXT("semantic_roads"),             Rec.SemanticRoadCount);
    CountsJ->SetNumberField(TEXT("semantic_intersections"),     Rec.SemanticIntersectionCount);
    CountsJ->SetNumberField(TEXT("zg_zones"),                   Rec.ZGZoneCount);
    CountsJ->SetNumberField(TEXT("zg_intersection_zones"),      Rec.ZGIntersectionZoneCount);
    CountsJ->SetNumberField(TEXT("roads_with_zero_lanes"),      Rec.RoadsWithZeroLanes);
    CountsJ->SetNumberField(TEXT("intersections_without_zone"), Rec.IntersectionsWithoutZone);
    CountsJ->SetNumberField(TEXT("orphan_intersection_zones"),  Rec.OrphanIntersectionZones);

    const bool bPassed = (Rec.Report.ErrorCount == 0);

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),              true);
    R->SetBoolField  (TEXT("passed"),               bPassed);
    R->SetObjectField(TEXT("counts"),               CountsJ);
    R->SetBoolField  (TEXT("reverse_map_available"), Rec.bReverseMapAvailable);
    R->SetNumberField(TEXT("snap_distance_cm"),     Rec.SnapDistanceCm);
    R->SetObjectField(TEXT("report"),               DiagJson::ReportToJson(Rec.Report));
    R->SetBoolField  (TEXT("markers_spawned"),      bSpawnMarkers);
    R->SetNumberField(TEXT("marker_count"),         SpawnedCount);
    return R;
}

// ---------------------------------------------------------------------------
// roadnet_list_markers
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleListReportMarkers(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("roadnet_list_markers: no editor world"));

    FName TagFilter = NAME_None;
    FString TagStr;
    if (Params->TryGetStringField(TEXT("tag"), TagStr) && !TagStr.IsEmpty())
        TagFilter = FName(*TagStr);

    TArray<FServeReportMarkerInfo> Markers =
        FServeRoadNetworkDiagnostics::GatherReportMarkers(World, TagFilter);

    TArray<TSharedPtr<FJsonValue>> MarkerArr;
    for (const FServeReportMarkerInfo& M : Markers)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("actor"),       M.ActorLabel);
        J->SetStringField(TEXT("text"),        M.Text);
        J->SetObjectField(TEXT("location"),    DiagJson::Vec3ToJson(M.Location));
        J->SetBoolField  (TEXT("has_lat_lon"), M.bHasLatLon);
        J->SetNumberField(TEXT("lat"),         M.Lat);
        J->SetNumberField(TEXT("lon"),         M.Lon);

        TArray<TSharedPtr<FJsonValue>> TagArr;
        for (const FString& T : M.Tags) TagArr.Add(MakeShared<FJsonValueString>(T));
        J->SetArrayField(TEXT("tags"), TagArr);

        MarkerArr.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),      true);
    R->SetNumberField(TEXT("marker_count"), Markers.Num());
    R->SetArrayField (TEXT("markers"),      MarkerArr);
    return R;
}

// ---------------------------------------------------------------------------
// gis_list_districts
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleListDistricts(const TSharedPtr<FJsonObject>& /*Params*/)
{
    TArray<TSharedPtr<FJsonValue>> DistrictArr;
    for (const FCityGenerationUtils::FDistrictInfo& Info : FCityGenerationUtils::ListDistricts())
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),       Info.DisplayName);
        J->SetStringField(TEXT("class_name"), Info.ClassName);
        DistrictArr.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),        true);
    R->SetNumberField(TEXT("district_count"), DistrictArr.Num());
    R->SetArrayField (TEXT("districts"),      DistrictArr);
    return R;
}

// ---------------------------------------------------------------------------
// gis_generate_block_shapes
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleGenerateBlockShapes(const TSharedPtr<FJsonObject>& /*Params*/)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_generate_block_shapes: no editor world available"));

    FString Error;
    if (!FCityGenerationUtils::GenerateBlockShapes(World, Error))
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_generate_block_shapes: %s"), *Error));

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"), true);
    R->SetStringField(TEXT("message"), TEXT("Block shapes generated"));
    return R;
}

// ---------------------------------------------------------------------------
// gis_assign_district
// Params: district (string — display name or class name, required)
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleAssignDistrict(const TSharedPtr<FJsonObject>& Params)
{
    FString DistrictName;
    if (!Params->TryGetStringField(TEXT("district"), DistrictName) || DistrictName.IsEmpty())
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("gis_assign_district: 'district' param required (display name or class name). "
                 "Use gis_list_districts to see available values."));

    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_assign_district: no editor world available"));

    FString Error;
    UClass* DistrictClass = FCityGenerationUtils::FindDistrictClass(DistrictName, Error);
    if (!DistrictClass)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_assign_district: %s"), *Error));

    int32 Count = 0;
    if (!FCityGenerationUtils::AssignDistrictToAllBlocks(World, DistrictClass, Count, Error))
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_assign_district: %s"), *Error));

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),        true);
    R->SetStringField(TEXT("district"),       DistrictName);
    R->SetNumberField(TEXT("blocks_assigned"), Count);
    return R;
}

// ---------------------------------------------------------------------------
// gis_generate_buildings
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleGenerateBuildings(const TSharedPtr<FJsonObject>& /*Params*/)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("gis_generate_buildings: no editor world available"));

    int32 Generated = 0, Skipped = 0;
    FString Error;
    if (!FCityGenerationUtils::GenerateBuildingsForAllBlocks(World, Generated, Skipped, Error))
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_generate_buildings: %s"), *Error));

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),           true);
    R->SetNumberField(TEXT("blocks_generated"),  Generated);
    R->SetNumberField(TEXT("blocks_skipped"),    Skipped);
    return R;
}

// ---------------------------------------------------------------------------
// gis_generate_procedural_roads
// Params: seed (int, optional, default 42)
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleGenerateProceduralRoads(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("gis_generate_procedural_roads: no editor world available"));

    int32   Seed        = 42;
    FString Topology    = TEXT("ring_branch");
    float   RingCluster = 3.0f;
    if (Params.IsValid())
    {
        double SeedD = 42.0;
        if (Params->TryGetNumberField(TEXT("seed"), SeedD))
            Seed = static_cast<int32>(SeedD);
        FString TopologyStr;
        if (Params->TryGetStringField(TEXT("topology"), TopologyStr) && !TopologyStr.IsEmpty())
            Topology = TopologyStr;
        double ClusterD = 3.0;
        if (Params->TryGetNumberField(TEXT("ring_cluster"), ClusterD))
            RingCluster = FMath::Max(1.0f, static_cast<float>(ClusterD));
    }

    // Disable autosave for the duration of the road rebuild.  RebuildRoadNetworkIncremental
    // runs worker threads that write ULandscapeSplineControlPoint / ULandscapeSplineSegment;
    // FArchiveGatherExternalActorRefs (invoked by any save, including autosave) serializes
    // those same objects on the game thread → data race → SIGSEGV.
    // Callers MUST follow with gis_rebuild_road_networks; that command re-enables autosave
    // only after the OnComplete callback confirms all workers have finished.
    if (UEditorLoadingSavingSettings* Cfg = GetMutableDefault<UEditorLoadingSavingSettings>())
    {
        Cfg->bAutoSaveEnable = false;
        UE_LOG(LogUnrealMCP, Log, TEXT("gis_generate_procedural_roads: autosave disabled until rebuild completes"));
    }

    FString Message;
    FProceduralRoadGen::Generate(World, Seed, Topology, RingCluster, Message);

    // Generate sets an error-like message when it aborts early (starts with "No ").
    if (Message.StartsWith(TEXT("No ")) || Message.StartsWith(TEXT("Failed")))
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_generate_procedural_roads: %s"), *Message));

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),      true);
    R->SetStringField(TEXT("message"),      Message);
    R->SetNumberField(TEXT("seed"),         Seed);
    R->SetStringField(TEXT("topology"),     Topology);
    R->SetNumberField(TEXT("ring_cluster"), RingCluster);
    return R;
}

// ---------------------------------------------------------------------------
// gis_list_sidewalk_presets
// Lists all URoadBLDSidewalkPreset Blueprint subclasses found under
// /Game/RoadModules/Sidewalks/ and their theme classification.
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleListSidewalkPresets(
    const TSharedPtr<FJsonObject>& /*Params*/)
{
    IAssetRegistry& AR =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName("/Game/RoadModules/Sidewalks"));
    Filter.PackagePaths.Add(FName("/RoadBLD"));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> Presets;
    for (const FAssetData& AD : Assets)
    {
        // Build the _C class path (<PackageName>.<AssetName>_C) and load.
        // Using PackageName (not GetObjectPathString which is already PackageName.AssetName).
        const FString ClassPath = FString::Printf(TEXT("%s.%s_C"),
            *AD.PackageName.ToString(), *AD.AssetName.ToString());
        UClass* PresetClass = LoadClass<URoadBLDSidewalkPreset>(nullptr, *ClassPath);
        if (!PresetClass) continue; // not a URoadBLDSidewalkPreset subclass

        const FString Pkg  = AD.PackageName.ToString();
        FString Theme      = TEXT("other");
        if (Pkg.Contains(TEXT("ModernCityDark"))) Theme = TEXT("dark");
        else if (Pkg.Contains(TEXT("ModernCity"))) Theme = TEXT("modern");

        auto P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"),       AD.AssetName.ToString());
        P->SetStringField(TEXT("class_path"), ClassPath);
        P->SetStringField(TEXT("theme"),      Theme);
        Presets.Add(MakeShared<FJsonValueObject>(P));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField (TEXT("success"),        true);
    R->SetArrayField(TEXT("presets"),        Presets);
    R->SetNumberField(TEXT("preset_count"),  Presets.Num());
    return R;
}

// ---------------------------------------------------------------------------
// gis_set_road_sidewalk
//
// Applies a sidewalk preset to all (or one named) ADynamicRoad actors.
//
// Params:
//   "sidewalk_preset" (string, required):
//     "modern"  — random ModernCity variant
//     "dark"    — random ModernCityDark variant
//     "random"  — random from all available presets
//     "/Game/…" — full Blueprint class path (with or without _C suffix)
//   "road_name" (string, optional, default "all"):
//     Actor label of a specific road to target, or "all" for all roads.
//   "seed" (number, optional):
//     Integer seed for reproducible random theme selection.
//
// Returns: success, applied_class, roads_updated count.
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleSetRoadSidewalk(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("gis_set_road_sidewalk: no editor world available"));

    // Parse params
    FString PresetParam;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("sidewalk_preset"), PresetParam)
        || PresetParam.IsEmpty())
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("gis_set_road_sidewalk: sidewalk_preset required "
                 "(\"modern\", \"dark\", \"random\", or full /Game/… class path)"));

    FString TargetRoadName = TEXT("all");
    if (Params.IsValid()) Params->TryGetStringField(TEXT("road_name"), TargetRoadName);

    int32 Seed = FMath::Rand();
    if (Params.IsValid())
    {
        double SeedD = 0.0;
        if (Params->TryGetNumberField(TEXT("seed"), SeedD))
            Seed = static_cast<int32>(SeedD);
    }

    // Theme keyword → resolve class path using the asset registry
    FString ResolvedClassPath = PresetParam;

    if (PresetParam == TEXT("modern") || PresetParam == TEXT("dark") ||
        PresetParam == TEXT("random"))
    {
        IAssetRegistry& AR =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

        FARFilter Filter;
        Filter.PackagePaths.Add(FName("/Game/RoadModules/Sidewalks"));
        Filter.PackagePaths.Add(FName("/RoadBLD"));
        Filter.bRecursivePaths  = true;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

        TArray<FAssetData> Assets;
        AR.GetAssets(Filter, Assets);

        TArray<FString> Pool;
        for (const FAssetData& AD : Assets)
        {
            // LoadClass forces blueprint compilation; skip if not a URoadBLDSidewalkPreset subclass
            const FString CPath = FString::Printf(TEXT("%s.%s_C"),
                *AD.PackageName.ToString(), *AD.AssetName.ToString());
            UClass* PC = LoadClass<URoadBLDSidewalkPreset>(nullptr, *CPath);
            if (!PC) continue;

            const FString Pkg = AD.PackageName.ToString();
            bool bMatch = (PresetParam == TEXT("random"));
            if (!bMatch && PresetParam == TEXT("modern"))
                bMatch = Pkg.Contains(TEXT("ModernCity")) && !Pkg.Contains(TEXT("Dark"));
            if (!bMatch && PresetParam == TEXT("dark"))
                bMatch = Pkg.Contains(TEXT("ModernCityDark")) || Pkg.Contains(TEXT("Dark"));

            if (bMatch)
                Pool.Add(CPath);
        }

        if (Pool.IsEmpty())
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("gis_set_road_sidewalk: no presets found for theme \"%s\""),
                    *PresetParam));

        FRandomStream Rand(Seed);
        ResolvedClassPath = Pool[Rand.RandRange(0, Pool.Num() - 1)];
    }

    // Ensure the class path has the _C suffix required for Blueprint generated classes
    if (!ResolvedClassPath.EndsWith(TEXT("_C")))
        ResolvedClassPath += TEXT("_C");

    // Load the sidewalk preset class
    UClass* PresetClass = LoadClass<URoadBLDSidewalkPreset>(nullptr, *ResolvedClassPath);
    if (!PresetClass)
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("gis_set_road_sidewalk: could not load class \"%s\""),
                *ResolvedClassPath));

    // Apply to matching road actors
    int32 Updated = 0;
    for (TActorIterator<ADynamicRoad> It(World); It; ++It)
    {
        ADynamicRoad* Road = *It;
        if (!IsValid(Road)) continue;

        const bool bTargetAll  = TargetRoadName.IsEmpty() || TargetRoadName == TEXT("all");
        const bool bNameMatch  = Road->GetActorLabel() == TargetRoadName;
        if (!bTargetAll && !bNameMatch) continue;

        Road->LeftSidewalkPreset  = PresetClass;
        Road->RightSidewalkPreset = PresetClass;

        // InitializeRoad regenerates the road's mesh/module geometry using the updated presets.
        // Uses Road->SourceDrawPreset (the preset assigned at spawning time).
        if (Road->SourceDrawPreset)
        {
            Road->InitializeRoad(Road->SourceDrawPreset, 0.0);
        }
        Road->MarkPackageDirty();
        Updated++;
    }

    if (Updated == 0)
    {
        const bool bIsAll = TargetRoadName.IsEmpty() || TargetRoadName == TEXT("all");
        const FString Err = bIsAll
            ? TEXT("gis_set_road_sidewalk: no ADynamicRoad actors found in level")
            : FString::Printf(TEXT("gis_set_road_sidewalk: no road named \"%s\" found"),
                *TargetRoadName);
        return FUnrealMCPCommonUtils::CreateErrorResponse(Err);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),       true);
    R->SetStringField(TEXT("applied_class"), ResolvedClassPath);
    R->SetNumberField(TEXT("roads_updated"), Updated);
    return R;
}

// ---------------------------------------------------------------------------
// gis_conform_landscape_to_roads
// ---------------------------------------------------------------------------
// Optional params:
//   falloff_multiplier  (number, default 2.5)  — LandscapeFalloffMultiplier per road
//   height_offset       (number, default -25.0) — landscape spline height offset in cm
//                         applied to every control point; negative pushes terrain below road deck
TSharedPtr<FJsonObject> FUnrealMCPGISCommands::HandleConformLandscapeToRoads(
    const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("gis_conform_landscape_to_roads: no editor world"));
    }

    double FalloffMultiplier = 2.5;
    double HeightOffsetCm    = -25.0;
    if (Params.IsValid())
    {
        double V = 0.0;
        if (Params->TryGetNumberField(TEXT("falloff_multiplier"), V)) FalloffMultiplier = V;
        if (Params->TryGetNumberField(TEXT("height_offset"),      V)) HeightOffsetCm    = V;
    }

    int32 Conformed = 0;
    int32 NoSpline  = 0;

    for (TActorIterator<ADynamicRoad> It(World); It; ++It)
    {
        ADynamicRoad* Road = *It;
        if (!IsValid(Road)) { continue; }

        Road->bEnableLandscapeSplineMirroring = true;
        Road->LandscapeFalloffMultiplier      = static_cast<float>(FalloffMultiplier);

        // Apply per-point height offset so the landscape sits below the road deck
        // rather than co-planar with it, preventing terrain poke-through on hills.
        const int32 NumCPs = Road->ControlPoints.Num();
        for (int32 i = 0; i < NumCPs; ++i)
        {
            Road->UpdatePointLandscapeMirrorHeightOffset(i, HeightOffsetCm);
        }

        if (UClothoidSplineComponent* Spline = Road->SplineComponent)
        {
            Spline->EnableSplineMirroring(true);
            Spline->RebuildLandscapeSpline();
            ++Conformed;
        }
        else
        {
            ++NoSpline;
        }
    }

    if (Conformed == 0 && NoSpline == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("gis_conform_landscape_to_roads: no ADynamicRoad actors found in level"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("success"),           true);
    R->SetNumberField(TEXT("roads_conformed"),   Conformed);
    R->SetNumberField(TEXT("roads_no_spline"),   NoSpline);
    R->SetNumberField(TEXT("falloff_multiplier"), FalloffMultiplier);
    R->SetNumberField(TEXT("height_offset_cm"),  HeightOffsetCm);
    return R;
}

