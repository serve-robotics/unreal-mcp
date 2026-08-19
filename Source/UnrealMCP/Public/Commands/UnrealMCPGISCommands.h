#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Synchronous GIS command handlers.
 * Async landscape import is handled directly on UUnrealMCPBridge (requires UFUNCTION delegates).
 */
class UNREALMCP_API FUnrealMCPGISCommands
{
public:
    FUnrealMCPGISCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleCreateLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleOpenLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetGeoAnchor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetGeoAnchor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleImportOpenDRIVE(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListRoadNetworks(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListRoadPresets(const TSharedPtr<FJsonObject>& Params);

    // GIS Viewer dataset management
    TSharedPtr<FJsonObject> HandleViewerLoadFile(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleViewerListLayers(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleViewerClear(const TSharedPtr<FJsonObject>& Params);

    // Camera. Wire command: gis_focus_landscapes (TODO(rename): viewport_focus_landscapes — not geospatial).
    TSharedPtr<FJsonObject> HandleFocusLandscapes(const TSharedPtr<FJsonObject>& Params);

    // Camera. Wire command: gis_camera_top_down. Mirrors the GIS panel's "Top-Down" button
    // (SServeGISOperationsPanel.cpp) exactly — same landscape-bounds + FOV height calc, same
    // straight-down FRotator(-90,0,0). NOT a parameterization of HandleFocusLandscapes: that
    // function's Right/Up basis is undefined at pitch=-90 (Forward becomes parallel to
    // FVector::UpVector, so CrossProduct(Up, Forward) is zero) — a genuinely separate code path
    // is required for true top-down, not just passing pitch=-90 to it.
    TSharedPtr<FJsonObject> HandleCameraTopDown(const TSharedPtr<FJsonObject>& Params);

    // Report markers. Wire command: gis_screenshot_markers (TODO(rename): viewport_screenshot_markers).
    TSharedPtr<FJsonObject> HandleScreenshotMarkers(const TSharedPtr<FJsonObject>& Params);

    // Zone graph visualization screenshots. Wire command: viewport_screenshot_zone_graph
    // (also handled directly on UUnrealMCPBridge).
    TSharedPtr<FJsonObject> HandleScreenshotZoneGraph(const TSharedPtr<FJsonObject>& Params);

    // Tempo zone graph pipeline. Wire command: gis_build_zone_graph
    // (TODO(rename): roadnet_build_zone_graph — operates on road-network actors, not GIS).
    TSharedPtr<FJsonObject> HandleBuildZoneGraph(const TSharedPtr<FJsonObject>& Params);

    // Road-network diagnostics (roadnet_* wire commands — no GIS dependency).
    // roadnet_summarize_semantic
    TSharedPtr<FJsonObject> HandleSummarizeRoadNetworkSemantic(const TSharedPtr<FJsonObject>& Params);
    // roadnet_summarize_lane_graph
    TSharedPtr<FJsonObject> HandleSummarizeLaneGraph(const TSharedPtr<FJsonObject>& Params);
    // roadnet_validate
    TSharedPtr<FJsonObject> HandleValidateRoadNetwork(const TSharedPtr<FJsonObject>& Params);
    // roadnet_reconcile
    TSharedPtr<FJsonObject> HandleReconcileRoadNetwork(const TSharedPtr<FJsonObject>& Params);
    // roadnet_list_markers
    TSharedPtr<FJsonObject> HandleListReportMarkers(const TSharedPtr<FJsonObject>& Params);

    // City block and building generation (FCityGenerationUtils)
    TSharedPtr<FJsonObject> HandleListDistricts(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGenerateBlockShapes(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssignDistrict(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssignRandomDistricts(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGenerateBuildings(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGenerateFootpaths(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleToggleBlockPreviews(const TSharedPtr<FJsonObject>& Params);

    // Roadside prop-line generation (FSplinePropLineGenerationUtils)
    TSharedPtr<FJsonObject> HandleGeneratePropLines(const TSharedPtr<FJsonObject>& Params);

    // Procedural road network generation (FProceduralRoadGen)
    TSharedPtr<FJsonObject> HandleGenerateProceduralRoads(const TSharedPtr<FJsonObject>& Params);

    // Sidewalk theming
    TSharedPtr<FJsonObject> HandleListSidewalkPresets(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetRoadSidewalk(const TSharedPtr<FJsonObject>& Params);

    // Landscape conformation
    TSharedPtr<FJsonObject> HandleConformLandscapeToRoads(const TSharedPtr<FJsonObject>& Params);

    // World Partition streaming toggle. Wire command: gis_set_world_partition_streaming
    TSharedPtr<FJsonObject> HandleSetWorldPartitionStreaming(const TSharedPtr<FJsonObject>& Params);

    // Merge every externally-packaged actor (OFPA) in the current level back into the main level
    // package. Wire command: gis_disable_external_actors. Needed before a level can reliably
    // serve as a NewLevelFromTemplate source — see HandleCreateLevel's caller notes.
    TSharedPtr<FJsonObject> HandleDisableExternalActors(const TSharedPtr<FJsonObject>& Params);

    // Plain LES->SaveCurrentLevel() (wire command: gis_save_current_level) — use instead of
    // Tempo's SaveLevel RPC during/after road generation; see the implementation's comment.
    TSharedPtr<FJsonObject> HandleSaveCurrentLevel(const TSharedPtr<FJsonObject>& Params);

    // Clear the CURRENT level's landscape spline control-point/segment graph (wire command:
    // gis_clear_landscape_splines). Call after gis_conform_landscape_to_roads and before any save
    // — see the implementation's comment for why this is required to avoid a SaveLevel/GC crash.
    TSharedPtr<FJsonObject> HandleClearLandscapeSplines(const TSharedPtr<FJsonObject>& Params);

    // Session-scoped (not function-scoped) toggle for UEditorValidatorSubsystem's validate-on-save
    // (wire command: gis_set_validate_on_save_disabled, param: disabled bool). See the
    // implementation's comment for why a function-scoped FScopedDisableValidateOnSave cannot fix
    // the SetEnableStreaming/ValidateOnSave Slate-modal-collision crash.
    TSharedPtr<FJsonObject> HandleSetValidateOnSaveDisabled(const TSharedPtr<FJsonObject>& Params);
};
