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

    // Camera
    TSharedPtr<FJsonObject> HandleFocusLandscapes(const TSharedPtr<FJsonObject>& Params);

    // Report markers
    TSharedPtr<FJsonObject> HandleScreenshotMarkers(const TSharedPtr<FJsonObject>& Params);
};
