#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUnrealMCP, Log, All);
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Http.h"
#include "Json.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPBlueprintCommands.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPGISCommands.h"
#include "UnrealMCPBridge.generated.h"

class FMCPServerRunnable;
class FUnrealMCPCLIBridge;
class UServeProcessRasterToLandscape;
class UServeProcessVectorShapes;
class ADynamicRoadNetwork;

/**
 * Editor subsystem for MCP Bridge
 * Handles communication between external tools and the Unreal Editor
 * through a TCP socket connection. Commands are received as JSON and
 * routed to appropriate command handlers.
 */
UCLASS()
class UNREALMCP_API UUnrealMCPBridge : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UUnrealMCPBridge();
	virtual ~UUnrealMCPBridge();

	// UEditorSubsystem implementation
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Server functions
	void StartServer();
	void StopServer();
	bool IsRunning() const { return bIsRunning; }

	// Command execution
	FString ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// Server state
	bool bIsRunning;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ConnectionSocket;
	FRunnableThread* ServerThread;

	// Server configuration
	FIPv4Address ServerAddress;
	uint16 Port;

	// Command handler instances
	TSharedPtr<FUnrealMCPEditorCommands> EditorCommands;
	TSharedPtr<FUnrealMCPBlueprintCommands> BlueprintCommands;
	TSharedPtr<FUnrealMCPBlueprintNodeCommands> BlueprintNodeCommands;
	TSharedPtr<FUnrealMCPProjectCommands> ProjectCommands;
	TSharedPtr<FUnrealMCPUMGCommands> UMGCommands;
	TSharedPtr<FUnrealMCPGISCommands> GISCommands;

	// stdio MCP transport — started when -MCPStdio is on the command line
	TUniquePtr<FUnrealMCPCLIBridge> CLIBridge;

	// Async landscape import state — only one GIS import runs at a time
	TSharedPtr<TPromise<FString>> PendingGISPromise;
	UServeProcessRasterToLandscape* PendingLandscapeProc = nullptr;

	// Called on game thread when async landscape import completes
	UFUNCTION()
	void OnGISLandscapeSucceeded();

	UFUNCTION()
	void OnGISLandscapeFailed(const FString& ErrorMessage, int32 ErrorCode);

	// Start an async landscape import; fires OnGISLandscape* when done
	void StartLandscapeImport(const TSharedPtr<FJsonObject>& Params);

	// Async vector roads import state
	TSharedPtr<TPromise<FString>> PendingVectorRoadsPromise;
	UServeProcessVectorShapes* PendingVectorRoadsProc = nullptr;
	TSharedPtr<FJsonObject> PendingVectorRoadsParams;

	UFUNCTION()
	void OnGISVectorRoadsSucceeded();

	UFUNCTION()
	void OnGISVectorRoadsFailed(const FString& ErrorMessage, int32 ErrorCode);

	void StartVectorRoadsImport(const TSharedPtr<FJsonObject>& Params);

	// Async building import state (OSM footprints -> CityBLD modular buildings)
	TSharedPtr<TPromise<FString>> PendingBuildingsPromise;
	UServeProcessVectorShapes* PendingBuildingsProc = nullptr;
	TSharedPtr<FJsonObject> PendingBuildingsParams;

	UFUNCTION()
	void OnGISBuildingsSucceeded();

	UFUNCTION()
	void OnGISBuildingsFailed(const FString& ErrorMessage, int32 ErrorCode);

	void StartBuildingsImport(const TSharedPtr<FJsonObject>& Params);
};