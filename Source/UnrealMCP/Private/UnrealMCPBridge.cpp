#include "UnrealMCPBridge.h"
#include "MCPServerRunnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
// Add Blueprint related includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// UE5.5 correct includes
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint Graph specific includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
// Include our new command handler classes
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPBlueprintCommands.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPGISCommands.h"
#include "UnrealMCPCLIBridge.h"
#include "Misc/CommandLine.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "LevelEditor.h"
#include "ILevelEditor.h"
#include "SLevelViewport.h"

// ServeLevelGenTools — for async landscape + vector roads import
#include "ProcessObjects/ServeProcessRasterToLandscape.h"
#include "ProcessObjects/ServeProcessVectorShapes.h"
#include "Libraries/ServeGDALFunctionLibrary.h"
#include "Anchor/ServeGeoAnchor.h"
#include "Types/ServeGISTypes.h"
#include "Landscape.h" // full ALandscape definition (needed for CreatedLandscapes access)
#include "LandscapeProxy.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeSplineActor.h"

// RoadBLD — for road creation from vector shapes
#include "DynamicRoad/DynamicRoad.h"
#include "DynamicRoad/DynamicRoadNetwork.h"
#include "RoadGeo.h"
#include "LevelGen/ServeLevelGenerationTable.h"
#include "GISViewer/ServeGISRoadImportUtils.h"
#include "Containers/Ticker.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogUnrealMCP);

namespace
{
    /**
     * True when it is legal to look up UObjects by name.
     *
     * Mirrors the exact condition StaticFindAllObjectsFast()/StaticFindFirstObject() assert on
     * (UObjectGlobals.cpp): inside a package save or a hash-table-locking GC, a name lookup is a
     * `Fatal` log, not a soft failure. Python is especially exposed — PyGenUtil::ApplyParamDefaults
     * imports the default value of every object/class-typed parameter on *every* wrapped UFunction
     * call, so e.g. `actor.get_components_by_class(...)` does a name lookup before it runs a line of
     * user code. See Docs/Bugs/mcp_python_during_savepackage.md.
     */
    bool IsUObjectHashSafe()
    {
        return !UE::IsSavingPackage(nullptr) && !IsGarbageCollectingAndLockingUObjectHashTables();
    }

    /**
     * Run Work on the game thread, but never nested inside a package save or GC.
     *
     * A plain AsyncTask(GameThread, ...) is not enough: a long SavePackage pumps the game-thread
     * task queue from its own progress UI (FSlowTask::TickProgress -> TickSlate ->
     * FlushRenderingCommands -> ProcessTasksUntilIdle), so a queued command can execute *inside*
     * the half-written package. When that happens the command is deferred and re-checked each
     * engine tick instead — saves and GCs are always transient, and ExecuteCommand's caller-side
     * poll (300 s) still bounds the wait for the client.
     */
    void RunOnGameThreadWhenUObjectSafe(TUniqueFunction<void()>&& InWork)
    {
        // TUniqueFunction is move-only; share it so the ticker delegate (which must be copyable)
        // can hold on to the same work.
        TSharedPtr<TUniqueFunction<void()>, ESPMode::ThreadSafe> Work =
            MakeShared<TUniqueFunction<void()>, ESPMode::ThreadSafe>(MoveTemp(InWork));

        AsyncTask(ENamedThreads::GameThread, [Work]()
        {
            if (IsUObjectHashSafe())
            {
                (*Work)();
                return;
            }

            UE_LOG(LogUnrealMCP, Warning,
                TEXT("Command dispatched during a package save or GC — deferring until UObject lookups are legal"));

            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Work](float) -> bool
            {
                if (!IsInGameThread() || !IsUObjectHashSafe())
                {
                    return true;  // keep ticking; re-check next frame
                }
                (*Work)();
                return false;     // done — unregister
            }), 0.0f);
        });
    }
}

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

UUnrealMCPBridge::UUnrealMCPBridge()
{
    EditorCommands = MakeShared<FUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FUnrealMCPBlueprintCommands>();
    BlueprintNodeCommands = MakeShared<FUnrealMCPBlueprintNodeCommands>();
    ProjectCommands = MakeShared<FUnrealMCPProjectCommands>();
    UMGCommands = MakeShared<FUnrealMCPUMGCommands>();
    GISCommands = MakeShared<FUnrealMCPGISCommands>();
}

UUnrealMCPBridge::~UUnrealMCPBridge()
{
    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintNodeCommands.Reset();
    ProjectCommands.Reset();
    UMGCommands.Reset();
    GISCommands.Reset();
}

// Initialize subsystem
void UUnrealMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Initializing"));
    
    bIsRunning = false;
    ListenerSocket = nullptr;
    ConnectionSocket = nullptr;
    ServerThread = nullptr;
    Port = MCP_SERVER_PORT;
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);

    // Start the TCP server
    StartServer();

    // If -MCPStdio is present on the command line, also start the stdio transport.
    // This lets Claude Code / Claude Desktop spawn the editor as an MCP server
    // without any TCP port configuration.
    if (FParse::Param(FCommandLine::Get(), TEXT("MCPStdio")))
    {
        UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: -MCPStdio detected — starting CLI bridge"));
        CLIBridge = MakeUnique<FUnrealMCPCLIBridge>(this);
        CLIBridge->Start();
    }

    // Start the permanent autosave watchdog. Fires every 2 s; disables autosave any time
    // RoadGeo actor count is changing (rebuild in flight from any source), re-enables after
    // 60 s of stability. This guards against all rebuild triggers, not just our own commands.
    WatchdogTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UUnrealMCPBridge::WatchdogTickRoadRebuild),
        2.0f);
    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: autosave watchdog started"));
}

// Clean up resources when subsystem is destroyed
void UUnrealMCPBridge::Deinitialize()
{
    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Shutting down"));
    if (CLIBridge.IsValid())
    {
        CLIBridge->Stop();
        CLIBridge.Reset();
    }
    if (WatchdogTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(WatchdogTickerHandle);
        WatchdogTickerHandle.Reset();
    }
    StopServer();
}

// Start the MCP server
void UUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("UnrealMCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address — retry a few times to handle TIME_WAIT from previous session
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    constexpr int32 MaxBindAttempts = 5;
    bool bBound = false;
    for (int32 Attempt = 1; Attempt <= MaxBindAttempts; ++Attempt)
    {
        if (NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
        {
            bBound = true;
            break;
        }
        UE_LOG(LogUnrealMCP, Warning,
            TEXT("UnrealMCPBridge: bind attempt %d/%d failed for %s:%d — retrying in 1 s..."),
            Attempt, MaxBindAttempts, *ServerAddress.ToString(), Port);
        FPlatformProcess::Sleep(1.0f);
    }
    if (!bBound)
    {
        UE_LOG(LogUnrealMCP, Error,
            TEXT("UnrealMCPBridge: could not bind to %s:%d after %d attempts — TCP server NOT started"),
            *ServerAddress.ToString(), Port, MaxBindAttempts);
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(NewListenerSocket.Get());
        return;
    }

    // Start listening — backlog of 16 gives headroom while a slow command executes.
    if (!NewListenerSocket->Listen(16))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // Start server thread
    ServerThread = FRunnableThread::Create(
        new FMCPServerRunnable(this, ListenerSocket),
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("UnrealMCPBridge: Failed to create server thread"));
        StopServer();
        return;
    }
}

// Stop the MCP server
void UUnrealMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    // Clean up thread
    if (ServerThread)
    {
        ServerThread->Kill(true);
        delete ServerThread;
        ServerThread = nullptr;
    }

    // Close sockets
    if (ConnectionSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket.Get());
        ConnectionSocket.Reset();
    }

    if (ListenerSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket.Get());
        ListenerSocket.Reset();
    }

    UE_LOG(LogUnrealMCP, Display, TEXT("UnrealMCPBridge: Server stopped"));
}

// ---------------------------------------------------------------------------
// Async landscape import — delegate handlers (called on game thread)
// ---------------------------------------------------------------------------

void UUnrealMCPBridge::OnGISLandscapeSucceeded()
{
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);

    TArray<TSharedPtr<FJsonValue>> Names;
    if (PendingLandscapeProc)
    {
        R->SetNumberField(TEXT("landscapes_created"), PendingLandscapeProc->CreatedLandscapes.Num());
        for (auto& W : PendingLandscapeProc->CreatedLandscapes)
        {
            if (W.IsValid())
                Names.Add(MakeShared<FJsonValueString>(W->GetActorLabel()));
        }
        PendingLandscapeProc->RemoveFromRoot();
        PendingLandscapeProc = nullptr;
    }
    R->SetArrayField(TEXT("landscape_names"), Names);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("status"), TEXT("success"));
    Resp->SetObjectField(TEXT("result"), R);

    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Resp.ToSharedRef(), W);

    if (PendingGISPromise.IsValid())
    {
        PendingGISPromise->SetValue(Out);
        PendingGISPromise.Reset();
    }
}

void UUnrealMCPBridge::OnGISLandscapeFailed(const FString& ErrorMessage, int32 ErrorCode)
{
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("status"), TEXT("error"));
    Resp->SetStringField(TEXT("error"), ErrorMessage);

    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Resp.ToSharedRef(), W);

    if (PendingLandscapeProc)
    {
        PendingLandscapeProc->RemoveFromRoot();
        PendingLandscapeProc = nullptr;
    }

    if (PendingGISPromise.IsValid())
    {
        PendingGISPromise->SetValue(Out);
        PendingGISPromise.Reset();
    }
}

void UUnrealMCPBridge::StartLandscapeImport(const TSharedPtr<FJsonObject>& Params)
{
    auto FulfillError = [this](const FString& Msg)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("error"));
        Resp->SetStringField(TEXT("error"), Msg);
        FString Out;
        TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
        if (PendingGISPromise.IsValid())
        {
            PendingGISPromise->SetValue(Out);
            PendingGISPromise.Reset();
        }
    };

    FString DatasetPath;
    if (!Params->TryGetStringField(TEXT("dataset_path"), DatasetPath))
    {
        FulfillError(TEXT("gis_import_landscape: dataset_path required"));
        return;
    }

    bool bSuccess;
    FString ErrorMsg;
    UGDALDataset* Dataset = UServeGDALFunctionLibrary::OpenDataset(DatasetPath, /*bReadOnly=*/true, bSuccess, ErrorMsg);
    if (!bSuccess || !Dataset)
    {
        FulfillError(FString::Printf(TEXT("gis_import_landscape: failed to open dataset: %s"), *ErrorMsg));
        return;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FulfillError(TEXT("gis_import_landscape: no editor world"));
        return;
    }

    UServeProcessRasterToLandscape* Proc = NewObject<UServeProcessRasterToLandscape>();
    Proc->TargetWorld = World;

    FString Val;
    if (Params->TryGetStringField(TEXT("meters_per_quad"), Val))
        Proc->MetersPerQuad = FCString::Atof(*Val);

    if (Params->TryGetStringField(TEXT("elevation_band"), Val))
    {
        Proc->bAutoDetectElevationBand = false;
        Proc->ElevationBandIndex = FCString::Atoi(*Val);
    }

    bool bHaveMin = Params->TryGetStringField(TEXT("min_elev"), Val);
    float MinElev = bHaveMin ? FCString::Atof(*Val) : 0.f;
    bool bHaveMax = Params->TryGetStringField(TEXT("max_elev"), Val);
    float MaxElev = bHaveMax ? FCString::Atof(*Val) : 1000.f;
    if (bHaveMin || bHaveMax)
    {
        Proc->bComputeMinMaxFromData = false;
        Proc->UserMinElevation = MinElev;
        Proc->UserMaxElevation = MaxElev;
    }

    // Optional aerial/ortho raster for per-tile texture + material creation.
    FString AerialPath;
    if (Params->TryGetStringField(TEXT("aerial_path"), AerialPath) && !AerialPath.IsEmpty())
    {
        bool bAerialOk;
        FString AerialErr;
        UGDALDataset* AerialDs = UServeGDALFunctionLibrary::OpenDataset(AerialPath, /*bReadOnly=*/true, bAerialOk, AerialErr);
        if (bAerialOk && AerialDs)
        {
            Proc->AerialDataset = AerialDs;
        }
        else
        {
            UE_LOG(LogUnrealMCP, Warning, TEXT("gis_import_landscape: aerial_path open failed (%s) — proceeding without aerial"), *AerialErr);
        }
    }

    // Pin to root so GC cannot collect while async work runs
    PendingLandscapeProc = Proc;
    Proc->AddToRoot();

    Proc->OnSucceeded.AddDynamic(this, &UUnrealMCPBridge::OnGISLandscapeSucceeded);
    Proc->OnFailed.AddDynamic(this, &UUnrealMCPBridge::OnGISLandscapeFailed);

    Proc->Run(Dataset);
}

// ---------------------------------------------------------------------------
// Async vector roads import — delegate handlers (called on game thread)
// ---------------------------------------------------------------------------

void UUnrealMCPBridge::OnGISVectorRoadsSucceeded()
{
    auto FulfillError = [this](const FString& Msg)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("error"));
        Resp->SetStringField(TEXT("error"), Msg);
        FString Out;
        TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
        if (PendingVectorRoadsPromise.IsValid())
        {
            PendingVectorRoadsPromise->SetValue(Out);
            PendingVectorRoadsPromise.Reset();
        }
    };

    if (!PendingVectorRoadsProc)
    {
        FulfillError(TEXT("gis_import_vector_roads: process object lost before callback"));
        return;
    }

    const TArray<FGISShapeFeature>& Shapes = PendingVectorRoadsProc->Shapes;
    const TSharedPtr<FJsonObject>& Params  = PendingVectorRoadsParams;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PendingVectorRoadsProc->RemoveFromRoot();
        PendingVectorRoadsProc = nullptr;
        PendingVectorRoadsParams.Reset();
        FulfillError(TEXT("gis_import_vector_roads: no editor world"));
        return;
    }

    // Resolve or create a road network actor.
    ADynamicRoadNetwork* Network = nullptr;
    {
        FString NetworkName;
        if (Params && Params->TryGetStringField(TEXT("network_name"), NetworkName) && !NetworkName.IsEmpty())
        {
            for (TActorIterator<ADynamicRoadNetwork> It(World); It; ++It)
            {
                if (It->GetActorLabel().Equals(NetworkName, ESearchCase::IgnoreCase))
                {
                    Network = *It;
                    break;
                }
            }
        }
        if (!Network)
        {
            for (TActorIterator<ADynamicRoadNetwork> It(World); It; ++It)
            {
                Network = *It;
                break;
            }
        }
        if (!Network)
        {
            Network = World->SpawnActor<ADynamicRoadNetwork>();
        }
    }

    if (!Network)
    {
        PendingVectorRoadsProc->RemoveFromRoot();
        PendingVectorRoadsProc = nullptr;
        PendingVectorRoadsParams.Reset();
        FulfillError(TEXT("gis_import_vector_roads: could not find or create ADynamicRoadNetwork"));
        return;
    }

    // Resolve geo anchor.
    AServeGeoAnchor* Anchor = AServeGeoAnchor::FindInWorld(World);

    // Load road style table — maps highway= tag values to per-type RoadBLD presets.
    // Accept optional override; fall back to the project default table asset.
    UServeLevelGenerationTable* StyleTable = nullptr;
    {
        FString TablePath;
        if (Params && Params->TryGetStringField(TEXT("style_table_path"), TablePath) && !TablePath.IsEmpty())
        {
            StyleTable = Cast<UServeLevelGenerationTable>(UEditorAssetLibrary::LoadAsset(TablePath));
            if (!StyleTable)
            {
                UE_LOG(LogTemp, Warning, TEXT("gis_import_vector_roads: style_table_path '%s' could not be loaded"), *TablePath);
            }
        }
        if (!StyleTable)
        {
            StyleTable = LoadObject<UServeLevelGenerationTable>(nullptr,
                TEXT("/Game/LevelGenTools/ServeLevelGenerationTable_Default.ServeLevelGenerationTable_Default"));
        }
        if (!StyleTable)
        {
            UE_LOG(LogTemp, Warning, TEXT("gis_import_vector_roads: no UServeLevelGenerationTable found — roads will use uninitialized presets"));
        }
    }

    // Detect if geographic CRS (degrees).  Accept explicit override; otherwise
    // auto-detect: if the first non-empty shape's first point X is in [-180,180].
    bool bIsGeographic = false;
    if (Params)
    {
        bool bGeoOverride = false;
        if (Params->TryGetBoolField(TEXT("is_geographic"), bGeoOverride))
        {
            bIsGeographic = bGeoOverride;
        }
        else
        {
            // Auto-detect from first point.
            for (const FGISShapeFeature& S : Shapes)
            {
                if (S.Points.Num() > 0)
                {
                    const double X = S.Points[0].X;
                    bIsGeographic = (X >= -180.0 && X <= 180.0);
                    break;
                }
            }
        }
    }

    // Center latitude for pseudo-meter conversion (matches landscape panel).
    const double CenterLatRad = (bIsGeographic && Anchor)
        ? FMath::DegreesToRadians(Anchor->OriginY / 111320.0)
        : 0.0;

    // RDP simplification tolerance (cm).
    double SampleEpsCm = 0.0;
    if (Params)
    {
        FString Val;
        if (Params->TryGetStringField(TEXT("sample_eps"), Val))
            SampleEpsCm = FCString::Atof(*Val) * 100.0;
    }

    // Vertical offset above landscape.
    constexpr double VerticalOffsetCm = 10.0;

    // Optional carriageway merge distance (default 15 m, same as GIS panel).
    // Pass merge_dist:0 to disable.
    double MergeDistCm = 1500.0;
    if (Params)
    {
        FString MDVal;
        if (Params->TryGetStringField(TEXT("merge_dist"), MDVal))
        {
            const double ValM = FCString::Atod(*MDVal);
            MergeDistCm = (FMath::IsFinite(ValM) && ValM > 0.0) ? ValM * 100.0 : 0.0;
        }
    }

    FCollisionQueryParams TraceParams(FName(TEXT("GISVectorRoadTrace")), /*bTraceComplex=*/false);
    double LastHitZ = 0.0;

    TArray<FServeGISRoadEntry> SpawnedRoads;
    int32 Skipped = 0;

    for (const FGISShapeFeature& Shape : Shapes)
    {
        if (Shape.Points.Num() < 2)
        {
            ++Skipped;
            continue;
        }

        // Build control points.
        TArray<FRoadControlPoint> ControlPoints;
        ControlPoints.Reserve(Shape.Points.Num());

        if (Shape.GeometryType == EGISVectorGeometryType::MultiLineString && Shape.Parts.Num() >= 2)
        {
            // MultiLineString: first two pts of part 0, then second pt of each subsequent part.
            auto AddPt = [&](const FGISShapePoint& Pt)
            {
                double X = Pt.X, Y = Pt.Y;
                const double Z = Pt.bHasZ ? Pt.Z : 0.0;
                if (bIsGeographic)
                {
                    X = X * (111320.0 * FMath::Cos(CenterLatRad));
                    Y = Y * 111320.0;
                }
                FVector WorldCm = Anchor
                    ? Anchor->ProjectXYMetersToWorldCm(X, Y, Z)
                    : FVector(X * 100.0, -Y * 100.0, Z * 100.0);
                FRoadControlPoint CP;
                CP.Pos           = FVector2D(WorldCm.X, WorldCm.Y);
                CP.BaseElevation = WorldCm.Z;
                ControlPoints.Add(CP);
            };

            const FGISShapePart& Part0 = Shape.Parts[0];
            if (Part0.NumPoints >= 2)
            {
                AddPt(Shape.Points[Part0.StartPointIndex]);
                AddPt(Shape.Points[Part0.StartPointIndex + 1]);
            }
            for (int32 Pi = 1; Pi < Shape.Parts.Num(); ++Pi)
            {
                const FGISShapePart& Part = Shape.Parts[Pi];
                if (Part.NumPoints >= 2)
                    AddPt(Shape.Points[Part.StartPointIndex + 1]);
            }
        }
        else
        {
            for (const FGISShapePoint& Pt : Shape.Points)
            {
                double X = Pt.X, Y = Pt.Y;
                const double Z = Pt.bHasZ ? Pt.Z : 0.0;
                if (bIsGeographic)
                {
                    X = X * (111320.0 * FMath::Cos(CenterLatRad));
                    Y = Y * 111320.0;
                }
                FVector WorldCm = Anchor
                    ? Anchor->ProjectXYMetersToWorldCm(X, Y, Z)
                    : FVector(X * 100.0, -Y * 100.0, Z * 100.0);
                FRoadControlPoint CP;
                CP.Pos           = FVector2D(WorldCm.X, WorldCm.Y);
                CP.BaseElevation = WorldCm.Z;
                ControlPoints.Add(CP);
            }
        }

        if (ControlPoints.Num() < 2)
        {
            ++Skipped;
            continue;
        }

        // Landscape trace to get ground elevation at each control point.
        bool bHadHitThisShape = false;
        for (int32 CpIdx = 0; CpIdx < ControlPoints.Num(); ++CpIdx)
        {
            FRoadControlPoint& CP = ControlPoints[CpIdx];
            FHitResult Hit;
            if (World->LineTraceSingleByChannel(
                    Hit,
                    FVector(CP.Pos.X, CP.Pos.Y,  500000.0),
                    FVector(CP.Pos.X, CP.Pos.Y, -500000.0),
                    ECC_WorldStatic,
                    TraceParams))
            {
                if (!bHadHitThisShape)
                {
                    bHadHitThisShape = true;
                    const double FirstHitZ = Hit.ImpactPoint.Z;
                    for (int32 k = 0; k < CpIdx; ++k)
                        ControlPoints[k].BaseElevation = FirstHitZ + VerticalOffsetCm;
                }
                LastHitZ = Hit.ImpactPoint.Z;
            }
            CP.BaseElevation = LastHitZ + VerticalOffsetCm;
        }

        const double MeanElev = ControlPoints[ControlPoints.Num() / 2].BaseElevation;

        // Per-road preset from style table keyed on highway=, oneway=, and name= attributes.
        FString HwValue;
        FString RoadName;
        bool bOneWay = false;
        for (const FGISShapeAttribute& Attr : Shape.Attributes)
        {
            if (Attr.Name.Equals(TEXT("highway"), ESearchCase::IgnoreCase))
                HwValue = Attr.Value.ToLower();
            else if (Attr.Name.Equals(TEXT("oneway"), ESearchCase::IgnoreCase))
                bOneWay = Attr.Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase);
            else if (Attr.Name.Equals(TEXT("name"), ESearchCase::IgnoreCase))
                RoadName = Attr.Value;
        }
        if (RoadName.IsEmpty()) { RoadName = HwValue; }

        TSubclassOf<UDynamicRoadDrawPreset> PresetClass = StyleTable
            ? StyleTable->ResolvePreset(HwValue, bOneWay)
            : TSubclassOf<UDynamicRoadDrawPreset>();
        UDynamicRoadDrawPreset* RoadPreset = PresetClass ? PresetClass.GetDefaultObject() : nullptr;

        ADynamicRoad* Road = Network->AddRoadToRoadNetwork(RoadPreset, MeanElev);
        if (!Road)
        {
            ++Skipped;
            continue;
        }

        Road->ControlPoints = MoveTemp(ControlPoints);
        Road->UpdateControlSpline();
        if (RoadPreset)
            Road->InitializeRoad(RoadPreset, 0.0);

        SpawnedRoads.Add({ Road, HwValue, RoadName, bOneWay });
    }

    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Post-spawn passes: chain-join then carriageway merge.
    // Runs via FServeGISRoadImportUtils — same shared code as the GIS panel.
    // -----------------------------------------------------------------------
    FServeGISRoadImportUtils::RunChainJoinPass(SpawnedRoads);
    if (MergeDistCm > 0.0)
    {
        int32 P;
        do { P = FServeGISRoadImportUtils::RunCarriagwayMergePass(SpawnedRoads, StyleTable, MergeDistCm); } while (P > 0);
    }

    // Count spawned roads for the response.
    int32 Spawned = 0;
    for (const FServeGISRoadEntry& Info : SpawnedRoads) { if (IsValid(Info.Road)) { ++Spawned; } }

    // The GDAL shapes/params are done with once roads are spawned; release the proc now.
    PendingVectorRoadsProc->RemoveFromRoot();
    PendingVectorRoadsProc = nullptr;
    PendingVectorRoadsParams.Reset();

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("roads_spawned"), Spawned);
    R->SetNumberField(TEXT("roads_skipped"), Skipped);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("status"), TEXT("success"));
    Resp->SetObjectField(TEXT("result"), R);

    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Resp.ToSharedRef(), W);

    // Rebuild is intentionally NOT triggered here.
    // gis_rebuild_road_networks uses RebuildRoadNetworkIncremental's OnComplete
    // callback to block until workers are truly done, preventing the
    // FArchiveGatherExternalActorRefs data race that caused save-time SIGSEGVs.
    if (PendingVectorRoadsPromise.IsValid())
    {
        PendingVectorRoadsPromise->SetValue(Out);
        PendingVectorRoadsPromise.Reset();
    }
}

void UUnrealMCPBridge::OnGISVectorRoadsFailed(const FString& ErrorMessage, int32 ErrorCode)
{
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("status"), TEXT("error"));
    Resp->SetStringField(TEXT("error"), ErrorMessage);

    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Resp.ToSharedRef(), W);

    if (PendingVectorRoadsProc)
    {
        PendingVectorRoadsProc->RemoveFromRoot();
        PendingVectorRoadsProc = nullptr;
    }
    PendingVectorRoadsParams.Reset();

    if (PendingVectorRoadsPromise.IsValid())
    {
        PendingVectorRoadsPromise->SetValue(Out);
        PendingVectorRoadsPromise.Reset();
    }
}

bool UUnrealMCPBridge::PollRoadRebuildComplete(float /*DeltaTime*/)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) return true; // keep ticking

    TArray<AActor*> GeoActors;
    UGameplayStatics::GetAllActorsOfClass(World, ARoadGeo::StaticClass(), GeoActors);
    const int32 CurrentCount = GeoActors.Num();

    if (CurrentCount > 0 && CurrentCount == LastRoadGeoCount)
    {
        RoadGeoStableFrames++;
        // Require 60 consecutive stable 1-second ticks before resolving.
        //
        // "Count stable" means no new RoadGeo actors are being spawned, but
        // RebuildRoadNetworkIncremental worker threads AND ULandscapeMirrorSplineBase::
        // RefreshMirrorSpline calls may still be modifying ULandscapeSplineControlPoint /
        // ULandscapeSplineSegment objects.  FArchiveGatherExternalActorRefs (triggered
        // by any level save) serializes those objects; a concurrent write → SIGSEGV.
        // Additionally, gis_build_zone_graph can re-trigger the rebuild; callers should
        // call gis_rebuild_road_networks again after ZoneGraph to drain that second wave.
        // 60 s of post-count-stability quiescence is conservative but safe.
        if (RoadGeoStableFrames >= 60)
        {
            if (PendingVectorRoadsPromise.IsValid())
            {
                PendingVectorRoadsPromise->SetValue(PendingVectorRoadsResult);
                PendingVectorRoadsPromise.Reset();
            }
            PendingVectorRoadsResult.Empty();
            RoadRebuildTickerHandle.Reset();
            return false; // stop ticking
        }
    }
    else
    {
        RoadGeoStableFrames = 0;
    }
    LastRoadGeoCount = CurrentCount;
    return true; // keep ticking
}

bool UUnrealMCPBridge::WatchdogTickRoadRebuild(float /*DeltaTime*/)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) return true;

    TArray<AActor*> GeoActors;
    UGameplayStatics::GetAllActorsOfClass(World, ARoadGeo::StaticClass(), GeoActors);
    const int32 CurrentCount = GeoActors.Num();

    if (WatchdogLastRoadGeoCount < 0)
    {
        // First tick — initialise without acting.
        WatchdogLastRoadGeoCount = CurrentCount;
        return true;
    }

    UEditorLoadingSavingSettings* Cfg = GetMutableDefault<UEditorLoadingSavingSettings>();

    if (CurrentCount != WatchdogLastRoadGeoCount)
    {
        WatchdogLastRoadGeoCount = CurrentCount;
        WatchdogStableChecks = 0;
        if (Cfg && Cfg->bAutoSaveEnable)
        {
            Cfg->bAutoSaveEnable = false;
            UE_LOG(LogUnrealMCP, Log, TEXT("Watchdog: RoadGeo count changed to %d — autosave disabled"), CurrentCount);
        }
    }
    else
    {
        WatchdogStableChecks++;
        // 30 ticks × 2 s = 60 s stable
        if (WatchdogStableChecks == 30 && Cfg && !Cfg->bAutoSaveEnable)
        {
            Cfg->bAutoSaveEnable = true;
            UE_LOG(LogUnrealMCP, Log, TEXT("Watchdog: RoadGeo stable at %d for 60 s — autosave re-enabled"), CurrentCount);
        }
    }

    return true; // always keep ticking
}

void UUnrealMCPBridge::StartVectorRoadsImport(const TSharedPtr<FJsonObject>& Params)
{
    auto FulfillError = [this](const FString& Msg)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("error"));
        Resp->SetStringField(TEXT("error"), Msg);
        FString Out;
        TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
        if (PendingVectorRoadsPromise.IsValid())
        {
            PendingVectorRoadsPromise->SetValue(Out);
            PendingVectorRoadsPromise.Reset();
        }
    };

    FString DatasetPath;
    if (!Params->TryGetStringField(TEXT("dataset_path"), DatasetPath))
    {
        FulfillError(TEXT("gis_import_vector_roads: dataset_path required"));
        return;
    }

    bool bSuccess;
    FString ErrorMsg;
    UGDALDataset* Dataset = UServeGDALFunctionLibrary::OpenDataset(DatasetPath, /*bReadOnly=*/true, bSuccess, ErrorMsg);
    if (!bSuccess || !Dataset)
    {
        FulfillError(FString::Printf(TEXT("gis_import_vector_roads: failed to open dataset: %s"), *ErrorMsg));
        return;
    }

    UServeProcessVectorShapes* Proc = NewObject<UServeProcessVectorShapes>();
    Proc->bMergeLineSegments = false; // off by default; merging produces degenerate shapes in many gpkg files

    FString Val;
    if (Params->TryGetStringField(TEXT("merge_segments"), Val))
        Proc->bMergeLineSegments = Val.ToBool();

    PendingVectorRoadsProc   = Proc;
    PendingVectorRoadsParams = Params;
    Proc->AddToRoot();

    Proc->OnSucceeded.AddDynamic(this, &UUnrealMCPBridge::OnGISVectorRoadsSucceeded);
    Proc->OnFailed.AddDynamic(this, &UUnrealMCPBridge::OnGISVectorRoadsFailed);

    Proc->Run(Dataset);
}

// ---------------------------------------------------------------------------
// Execute a command received from a client
// ---------------------------------------------------------------------------

FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogUnrealMCP, Log, TEXT(">> cmd=%s"), *CommandType);

    // Async vector roads import (same promise pattern as landscape).
    if (CommandType == TEXT("gis_import_vector_roads"))
    {
        if (PendingVectorRoadsPromise.IsValid())
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("status"), TEXT("error"));
            Err->SetStringField(TEXT("error"), TEXT("gis_import_vector_roads: another import is already in progress"));
            FString Out;
            TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out);
            FJsonSerializer::Serialize(Err.ToSharedRef(), W2);
            return Out;
        }

        PendingVectorRoadsPromise = MakeShared<TPromise<FString>>();
        TFuture<FString> VRFuture = PendingVectorRoadsPromise->GetFuture();

        AsyncTask(ENamedThreads::GameThread, [this, Params]()
        {
            StartVectorRoadsImport(Params);
        });

        constexpr float PollSec  = 0.05f;
        constexpr float LimitSec = 900.f;
        for (float Elapsed = 0.f; Elapsed < LimitSec; Elapsed += PollSec)
        {
            if (VRFuture.IsReady()) return VRFuture.Get();
            FPlatformProcess::Sleep(PollSec);
        }

        TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
        T->SetStringField(TEXT("status"), TEXT("error"));
        T->SetStringField(TEXT("error"), TEXT("gis_import_vector_roads timed out after 900 s"));
        FString Out;
        TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(T.ToSharedRef(), W2);
        PendingVectorRoadsPromise->SetValue(Out);
        PendingVectorRoadsPromise.Reset();
        return Out;
    }

    // Async landscape import: uses a shared promise stored on the bridge so the
    // delegate callbacks can fulfill it after the game-thread lambda exits.
    if (CommandType == TEXT("gis_import_landscape"))
    {
        if (PendingGISPromise.IsValid())
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("status"), TEXT("error"));
            Err->SetStringField(TEXT("error"), TEXT("gis_import_landscape: another import is already in progress"));
            FString Out;
            TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out);
            FJsonSerializer::Serialize(Err.ToSharedRef(), W2);
            return Out;
        }

        PendingGISPromise = MakeShared<TPromise<FString>>();
        TFuture<FString> GISFuture = PendingGISPromise->GetFuture();

        AsyncTask(ENamedThreads::GameThread, [this, Params]()
        {
            StartLandscapeImport(Params);
        });

        constexpr float PollSec  = 0.05f;
        constexpr float LimitSec = 600.f; // large rasters can be slow
        for (float Elapsed = 0.f; Elapsed < LimitSec; Elapsed += PollSec)
        {
            if (GISFuture.IsReady()) return GISFuture.Get();
            FPlatformProcess::Sleep(PollSec);
        }

        TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
        T->SetStringField(TEXT("status"), TEXT("error"));
        T->SetStringField(TEXT("error"), TEXT("gis_import_landscape timed out after 600 s"));
        FString Out;
        TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(T.ToSharedRef(), W2);
        PendingGISPromise->SetValue(Out);
        PendingGISPromise.Reset();
        return Out;
    }

    // gis_rebuild_road_networks: wait for the in-progress road rebuild commit to finish.
    // gis_import_vector_roads triggers a rebuild and returns immediately — call this
    // before gis_generate_block_shapes to ensure RoadGeo actors (with collision) exist.
    // Polls every 1 s until RoadGeo count has been stable for 3 consecutive readings.
    if (CommandType == TEXT("gis_rebuild_road_networks"))
    {
        if (PendingVectorRoadsPromise.IsValid())
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("status"), TEXT("error"));
            Err->SetStringField(TEXT("error"), TEXT("gis_rebuild_road_networks: road import or rebuild already in progress"));
            FString Out;
            TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out);
            FJsonSerializer::Serialize(Err.ToSharedRef(), W2);
            return Out;
        }

        PendingVectorRoadsPromise = MakeShared<TPromise<FString>>();
        TFuture<FString> RebuildFuture = PendingVectorRoadsPromise->GetFuture();

        // Build success result upfront; PollRoadRebuildComplete delivers it when stable.
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetBoolField(TEXT("success"), true);
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("success"));
        Resp->SetObjectField(TEXT("result"), R);
        FString SuccessOut;
        TSharedRef<TJsonWriter<>> W3 = TJsonWriterFactory<>::Create(&SuccessOut);
        FJsonSerializer::Serialize(Resp.ToSharedRef(), W3);

        // Find the road network and trigger rebuild with the OnComplete callback.
        // OnComplete fires when ALL worker threads have truly finished modifying
        // ULandscapeSplineControlPoint / ULandscapeSplineSegment objects.
        // Only after OnComplete is it safe to re-enable autosave.
        AsyncTask(ENamedThreads::GameThread, [this, SuccessOut]()
        {
            // Ensure autosave is off for the rebuild duration (may already be off from
            // gis_generate_procedural_roads or gis_build_zone_graph; belt-and-suspenders).
            if (UEditorLoadingSavingSettings* Cfg = GetMutableDefault<UEditorLoadingSavingSettings>())
                Cfg->bAutoSaveEnable = false;

            UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

            ADynamicRoadNetwork* Network = nullptr;
            if (World)
            {
                for (TActorIterator<ADynamicRoadNetwork> It(World); It; ++It)
                {
                    if (IsValid(*It)) { Network = *It; break; }
                }
            }

            if (!Network)
            {
                // No road network in scene — nothing to rebuild; resolve immediately.
                // Re-enable autosave now since there are no workers to wait for.
                if (UEditorLoadingSavingSettings* Cfg = GetMutableDefault<UEditorLoadingSavingSettings>())
                {
                    Cfg->bAutoSaveEnable = true;
                    UE_LOG(LogUnrealMCP, Log, TEXT("gis_rebuild_road_networks: no network, autosave re-enabled"));
                }
                if (PendingVectorRoadsPromise.IsValid())
                {
                    PendingVectorRoadsPromise->SetValue(SuccessOut);
                    PendingVectorRoadsPromise.Reset();
                }
                return;
            }

            // The 5-arg overload fires OnComplete when workers are truly done.
            // Clear landscape splines and re-enable autosave on game thread, then
            // fulfill the promise — ensures saves are safe before callers proceed.
            Network->RebuildRoadNetworkIncremental({}, {}, false, false,
                [this, SuccessOut](bool bRebuildSucceeded)
                {
                    if (!bRebuildSucceeded)
                    {
                        UE_LOG(LogUnrealMCP, Warning, TEXT("gis_rebuild_road_networks: rebuild aborted"));
                    }
                    // OnComplete may fire on a background thread; dispatch to game thread
                    // for UObject modifications and promise fulfillment.
                    AsyncTask(ENamedThreads::GameThread, [this, SuccessOut]()
                    {
                        // Destroy all landscape spline data to prevent FArchiveGatherExternalActorRefs
                        // from stack-overflowing on the cyclic ControlPoint↔Segment ring graph
                        // during World Partition saves. Road geometry lives in ARoadGeo static meshes;
                        // landscape splines are only used for terrain deformation and are safe to
                        // discard after rebuild. Two spline storage paths must be cleared:
                        //   1. ALandscapeSplineActor (UE5 WP-style standalone spline actors)
                        //   2. ULandscapeSplinesComponent on ALandscapeProxy actors (legacy path)
                        if (UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
                        {
                            // Path 1: destroy ALandscapeSplineActor actors (the primary culprit —
                            // these are NOT ALandscapeProxy subclasses and were missed by the
                            // previous ALandscapeProxy iterator, leaving their ring graph intact).
                            TArray<ALandscapeSplineActor*> SplineActors;
                            for (TActorIterator<ALandscapeSplineActor> It(W); It; ++It)
                            {
                                if (IsValid(*It)) SplineActors.Add(*It);
                            }
                            for (ALandscapeSplineActor* SA : SplineActors)
                            {
                                UE_LOG(LogUnrealMCP, Log,
                                    TEXT("gis_rebuild_road_networks: destroying ALandscapeSplineActor %s"),
                                    *SA->GetName());
                                SA->Destroy();
                            }

                            // Path 2: clear ULandscapeSplinesComponent arrays on ALandscapeProxy actors.
                            for (TActorIterator<ALandscapeProxy> It(W); It; ++It)
                            {
                                if (!IsValid(*It)) continue;
                                if (ULandscapeSplinesComponent* SC = It->GetSplinesComponent())
                                {
                                    if (SC->HasAnyControlPointsOrSegments())
                                    {
                                        SC->Modify();
                                        SC->GetControlPoints().Empty();
                                        SC->GetSegments().Empty();
                                        UE_LOG(LogUnrealMCP, Log,
                                            TEXT("gis_rebuild_road_networks: cleared legacy splines on %s"),
                                            *It->GetName());
                                    }
                                }
                            }
                        }

                        if (UEditorLoadingSavingSettings* Cfg = GetMutableDefault<UEditorLoadingSavingSettings>())
                        {
                            Cfg->bAutoSaveEnable = true;
                            UE_LOG(LogUnrealMCP, Log, TEXT("gis_rebuild_road_networks: workers done, autosave re-enabled"));
                        }

                        if (PendingVectorRoadsPromise.IsValid())
                        {
                            PendingVectorRoadsPromise->SetValue(SuccessOut);
                            PendingVectorRoadsPromise.Reset();
                        }
                    });
                });
        });

        constexpr float PollSec  = 0.1f;
        constexpr float LimitSec = 900.f; // 15 min — generous for large networks
        for (float Elapsed = 0.f; Elapsed < LimitSec; Elapsed += PollSec)
        {
            if (RebuildFuture.IsReady()) return RebuildFuture.Get();
            FPlatformProcess::Sleep(PollSec);
        }

        // Timeout — re-enable autosave so the editor isn't left with it permanently off.
        AsyncTask(ENamedThreads::GameThread, []()
        {
            if (UEditorLoadingSavingSettings* Cfg = GetMutableDefault<UEditorLoadingSavingSettings>())
            {
                Cfg->bAutoSaveEnable = true;
                UE_LOG(LogUnrealMCP, Log, TEXT("gis_rebuild_road_networks: timeout, autosave re-enabled"));
            }
        });

        TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
        T->SetStringField(TEXT("status"), TEXT("error"));
        T->SetStringField(TEXT("error"), TEXT("gis_rebuild_road_networks timed out after 900 s"));
        FString Tout;
        TSharedRef<TJsonWriter<>> Wt = TJsonWriterFactory<>::Create(&Tout);
        FJsonSerializer::Serialize(T.ToSharedRef(), Wt);
        if (PendingVectorRoadsPromise.IsValid())
        {
            PendingVectorRoadsPromise->SetValue(Tout);
            PendingVectorRoadsPromise.Reset();
        }
        return Tout;
    }

    // take_screenshot: fire HighResShot on game thread, poll for the file here so
    // the game thread is free to actually render the frame.
    if (CommandType == TEXT("take_screenshot"))
    {
        FString FilePath;
        Params->TryGetStringField(TEXT("filepath"), FilePath);
        if (!FilePath.IsEmpty() && !FilePath.EndsWith(TEXT(".png")))
            FilePath += TEXT(".png");

        int32 Multiplier = 2;
        Params->TryGetNumberField(TEXT("multiplier"), Multiplier);
        Multiplier = FMath::Clamp(Multiplier, 1, 8);

        const FString ShotDir = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir());
        IFileManager& FM = IFileManager::Get();

        // Snapshot existing files before firing so we detect the new one reliably.
        TSet<FString> Before;
        FM.IterateDirectory(*ShotDir, [&Before](const TCHAR* Path, bool bIsDir) -> bool
        {
            if (!bIsDir)
            {
                FString Name = FPaths::GetCleanFilename(Path);
                if (Name.StartsWith(TEXT("Highres")) && Name.EndsWith(TEXT(".png")))
                    Before.Add(Name);
            }
            return true;
        });

        // Fire on game thread — do NOT wait; the game thread must be free to render.
        const int32 MultiplierCopy = Multiplier;
        AsyncTask(ENamedThreads::GameThread, [MultiplierCopy]()
        {
            UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            GEngine->Exec(World, *FString::Printf(TEXT("HighResShot %d"), MultiplierCopy));
        });

        // Poll for the new file on the MCP server thread (game thread renders in parallel).
        FString NewFile;
        const double Deadline = FPlatformTime::Seconds() + 15.0;
        while (FPlatformTime::Seconds() < Deadline)
        {
            FPlatformProcess::Sleep(0.1f);
            FM.IterateDirectory(*ShotDir, [&](const TCHAR* Path, bool bIsDir) -> bool
            {
                if (!bIsDir)
                {
                    FString Name = FPaths::GetCleanFilename(Path);
                    if (Name.StartsWith(TEXT("Highres")) && Name.EndsWith(TEXT(".png")) && !Before.Contains(Name))
                        NewFile = Path;
                }
                return true;
            });
            if (!NewFile.IsEmpty()) break;
        }

        auto MakeJsonStr = [](TSharedPtr<FJsonObject> Obj) -> FString
        {
            FString S;
            TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&S);
            FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
            return S;
        };

        if (NewFile.IsEmpty())
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("status"), TEXT("error"));
            Err->SetStringField(TEXT("error"), TEXT("take_screenshot: HighResShot did not produce a file within 15 s"));
            return MakeJsonStr(Err);
        }

        if (!FilePath.IsEmpty() && FilePath != NewFile)
            FM.Copy(*FilePath, *NewFile);
        else
            FilePath = NewFile;

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("filepath"), FilePath);
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("success"));
        Resp->SetObjectField(TEXT("result"), Result);
        return MakeJsonStr(Resp);
    }

    // viewport_set_zone_graph_overlay: enable or disable the Navigation show flag in the active viewport.
    // Pure editor-viewport UI (a show flag), no GIS dependency — hence the viewport_ prefix.
    // {"type":"viewport_set_zone_graph_overlay","params":{"enabled":true}}
    if (CommandType == TEXT("viewport_set_zone_graph_overlay"))
    {
        bool bEnable = true;
        Params->TryGetBoolField(TEXT("enabled"), bEnable);
        TPromise<bool> P; TFuture<bool> F = P.GetFuture();
        AsyncTask(ENamedThreads::GameThread, [bEnable, P2 = MoveTemp(P)]() mutable {
            auto& LEM = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
            auto LE = LEM.GetFirstLevelEditor();
            if (LE) {
                auto VP = LE->GetActiveViewportInterface();
                if (VP) {
                    FLevelEditorViewportClient& VC = VP->GetLevelViewportClient();
                    VC.EngineShowFlags.SetNavigation(bEnable);
                    VC.Invalidate();
                }
            }
            P2.SetValue(true);
        });
        F.Get();
        auto R = MakeShared<FJsonObject>();
        R->SetBoolField(TEXT("success"), true);
        R->SetBoolField(TEXT("enabled"), bEnable);
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("success"));
        Resp->SetObjectField(TEXT("result"), R);
        FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
        return Out;
    }

    // viewport_screenshot_zone_graph: enable Navigation show flag (drives ZoneGraph lane rendering),
    // take perspective + top-down screenshots, then leave the flag ON so zone graph stays visible.
    // Editor-viewport capture, no GIS dependency — hence the viewport_ prefix.
    // Handled here (server thread) so the game thread is free to render while we poll for files.
    if (CommandType == TEXT("viewport_screenshot_zone_graph"))
    {
        auto MakeJsonStrZG = [](TSharedPtr<FJsonObject> Obj) -> FString {
            FString S; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&S);
            FJsonSerializer::Serialize(Obj.ToSharedRef(), W); return S;
        };
        auto ErrZG = [&MakeJsonStrZG](const FString& Msg) -> FString {
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("status"), TEXT("error"));
            E->SetStringField(TEXT("error"), Msg);
            return MakeJsonStrZG(E);
        };

        // Step 1 — gather scene bounds, FOV, and enable Navigation show flag (game thread).
        struct FZGContext { FBox Box; float HalfHTan=0; float HalfVTan=0; bool bWasNav=false; FString Err; };
        TPromise<FZGContext> CtxPromise;
        TFuture<FZGContext> CtxFuture = CtxPromise.GetFuture();
        AsyncTask(ENamedThreads::GameThread, [P = MoveTemp(CtxPromise)]() mutable {
            FZGContext C;
            UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            if (!W) { C.Err = TEXT("viewport_screenshot_zone_graph: no editor world"); P.SetValue(C); return; }
            C.Box = FBox(ForceInit);
            for (TActorIterator<ALandscape> It(W); It; ++It) {
                FVector O, E; It->GetActorBounds(false, O, E);
                C.Box += FBox(O-E, O+E);
            }
            if (!C.Box.IsValid) { C.Err = TEXT("viewport_screenshot_zone_graph: no Landscape actors"); P.SetValue(C); return; }
            auto& LEM = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
            auto LE = LEM.GetFirstLevelEditor();
            if (!LE) { C.Err = TEXT("viewport_screenshot_zone_graph: LevelEditor unavailable"); P.SetValue(C); return; }
            auto VP = LE->GetActiveViewportInterface();
            if (!VP) { C.Err = TEXT("viewport_screenshot_zone_graph: no active viewport"); P.SetValue(C); return; }
            FLevelEditorViewportClient* VC = &VP->GetLevelViewportClient();
            C.bWasNav = VC->EngineShowFlags.Navigation != 0;
            VC->EngineShowFlags.SetNavigation(true);
            const FIntPoint Sz = VC->Viewport ? VC->Viewport->GetSizeXY() : FIntPoint(1920,1080);
            const float Asp = Sz.Y > 0 ? (float)Sz.X/Sz.Y : 16.f/9.f;
            const float HFov = FMath::DegreesToRadians(FMath::Max(VC->ViewFOV, 10.f));
            C.HalfHTan = FMath::Tan(HFov * 0.5f);
            C.HalfVTan = FMath::Tan(2.f * FMath::Atan(C.HalfHTan / Asp) * 0.5f);
            VC->Invalidate();
            P.SetValue(C);
        });
        FZGContext Ctx = CtxFuture.Get();
        if (!Ctx.Err.IsEmpty()) return ErrZG(Ctx.Err);

        // Optional close-up: override landscape framing with a custom box.
        // Pass {"location":{"x":...,"y":...,"z":...},"extent":2000} to zoom into a specific area.
        {
            const TSharedPtr<FJsonObject>* LocObj;
            if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
            {
                double X = 0, Y = 0, Z = 0, Ext = 2000;
                (*LocObj)->TryGetNumberField(TEXT("x"), X);
                (*LocObj)->TryGetNumberField(TEXT("y"), Y);
                (*LocObj)->TryGetNumberField(TEXT("z"), Z);
                Params->TryGetNumberField(TEXT("extent"), Ext);
                Ctx.Box = FBox(FVector(X - Ext, Y - Ext, Z - Ext * 0.5),
                               FVector(X + Ext, Y + Ext, Z + Ext * 0.5));
            }
        }

        // Step 2 — per-shot helper: position camera + fire HighResShot on game thread, poll here.
        const FString ShotDir = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir());
        IFileManager& FM2 = IFileManager::Get();

        // If "use_current_camera" is true, skip auto-framing and shoot from the current viewport position.
        bool bUseCurrentCamera = false;
        Params->TryGetBoolField(TEXT("use_current_camera"), bUseCurrentCamera);

        auto TakeZGShot = [&](const FString& DestPath, float Pitch, float Yaw) -> bool {
            TSet<FString> Before;
            FM2.IterateDirectory(*ShotDir, [&Before](const TCHAR* P, bool bDir) -> bool {
                if (!bDir) { FString N = FPaths::GetCleanFilename(P);
                    if (N.StartsWith(TEXT("Highres")) && N.EndsWith(TEXT(".png"))) Before.Add(N); }
                return true;
            });
            const FBox SBox = Ctx.Box;
            const float HHT = Ctx.HalfHTan, HVT = Ctx.HalfVTan;
            AsyncTask(ENamedThreads::GameThread, [SBox, HHT, HVT, Pitch, Yaw, bUseCurrentCamera]() {
                auto& LEM2 = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
                auto LE2 = LEM2.GetFirstLevelEditor(); if (!LE2) return;
                auto VP2 = LE2->GetActiveViewportInterface(); if (!VP2) return;
                FLevelEditorViewportClient* VC2 = &VP2->GetLevelViewportClient();
                if (!bUseCurrentCamera)
                {
                    const float PR = FMath::DegreesToRadians(Pitch), YR = FMath::DegreesToRadians(Yaw);
                    const FVector Fwd(FMath::Cos(PR)*FMath::Cos(YR), FMath::Cos(PR)*FMath::Sin(YR), FMath::Sin(PR));
                    const FVector Rt = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
                    const FVector Up = FVector::CrossProduct(Fwd, Rt).GetSafeNormal();
                    const FVector Ctr = SBox.GetCenter(), Ext = SBox.GetExtent();
                    float DMin = 1.f;
                    for (int32 i = 0; i < 8; ++i) {
                        const FVector C2 = Ctr + FVector((i&1)?Ext.X:-Ext.X,(i&2)?Ext.Y:-Ext.Y,(i&4)?Ext.Z:-Ext.Z);
                        const FVector D2 = C2 - Ctr;
                        DMin = FMath::Max(DMin, FMath::Abs(FVector::DotProduct(D2,Rt))/HHT - FVector::DotProduct(D2,Fwd));
                        DMin = FMath::Max(DMin, FMath::Abs(FVector::DotProduct(D2,Up))/HVT - FVector::DotProduct(D2,Fwd));
                    }
                    VC2->SetViewLocation(Ctr - Fwd*(DMin*1.05f));
                    VC2->SetViewRotation(FRotator(Pitch, Yaw, 0.f));
                    VC2->Invalidate();
                }
                UWorld* W2 = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
                GEngine->Exec(W2, TEXT("HighResShot 1"));
            });
            FString NewFile;
            const double Deadline = FPlatformTime::Seconds() + 20.0;
            while (FPlatformTime::Seconds() < Deadline) {
                FPlatformProcess::Sleep(0.15f);
                FM2.IterateDirectory(*ShotDir, [&](const TCHAR* P, bool bDir) -> bool {
                    if (!bDir) { FString N = FPaths::GetCleanFilename(P);
                        if (N.StartsWith(TEXT("Highres")) && N.EndsWith(TEXT(".png")) && !Before.Contains(N))
                            NewFile = P; }
                    return true;
                });
                if (!NewFile.IsEmpty()) break;
            }
            if (NewFile.IsEmpty()) return false;
            FM2.Copy(*DestPath, *NewFile);
            return true;
        };

        const FString PerspPath = ShotDir / TEXT("zonegraph_persp.png");
        const FString TopPath   = ShotDir / TEXT("zonegraph_top.png");
        const bool bPerspOk = TakeZGShot(PerspPath, -45.f,  45.f);
        const bool bTopOk   = TakeZGShot(TopPath,   -89.9f,  0.f);

        // Step 3 — leave Navigation show flag ON so zone graph stays visible in the editor.
        // (Previously it was restored to the pre-shot state, which turned it off and hid the overlay.)
        AsyncTask(ENamedThreads::GameThread, []() {
            auto& LEM3 = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
            auto LE3 = LEM3.GetFirstLevelEditor(); if (!LE3) return;
            auto VP3 = LE3->GetActiveViewportInterface(); if (!VP3) return;
            FLevelEditorViewportClient* VC3 = &VP3->GetLevelViewportClient();
            VC3->EngineShowFlags.SetNavigation(true);
            VC3->Invalidate();
        });

        if (!bPerspOk && !bTopOk)
            return ErrZG(TEXT("viewport_screenshot_zone_graph: HighResShot did not produce files within timeout"));

        TSharedPtr<FJsonObject> ZGResult = MakeShared<FJsonObject>();
        ZGResult->SetStringField(TEXT("perspective"), bPerspOk ? PerspPath : TEXT("(failed)"));
        ZGResult->SetStringField(TEXT("topdown"),     bTopOk   ? TopPath   : TEXT("(failed)"));
        TSharedPtr<FJsonObject> ZGResp = MakeShared<FJsonObject>();
        ZGResp->SetStringField(TEXT("status"), TEXT("success"));
        ZGResp->SetObjectField(TEXT("result"), ZGResult);
        return MakeJsonStrZG(ZGResp);
    }

    // Create a promise to wait for the result
    TPromise<FString> Promise;
    TFuture<FString> Future = Promise.GetFuture();

    // Queue execution on Game Thread — but never inside a package save or GC (see
    // RunOnGameThreadWhenUObjectSafe; a save's progress UI pumps this very task queue).
    RunOnGameThreadWhenUObjectSafe([this, CommandType, Params, Promise = MoveTemp(Promise)]() mutable
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);
        
        try
        {
            TSharedPtr<FJsonObject> ResultJson;

            // Block actor-modifying commands while a road rebuild is in flight.
            // ULandscapeSplineSegment::Serialize crashes when World Partition gathers
            // external actor refs (triggered by spawning/destroying actors) while
            // RebuildRoadNetworkIncremental workers are still touching spline data.
            static const TSet<FString> RebuildSafeCommands = {
                TEXT("ping"), TEXT("get_actors_in_level"), TEXT("find_actors_by_name"),
                TEXT("get_actor_properties"), TEXT("focus_viewport"),
                TEXT("gis_rebuild_road_networks"), TEXT("gis_import_landscape"),
                TEXT("gis_import_vector_roads"), TEXT("gis_viewer_clear"),
                TEXT("gis_viewer_load_file"), TEXT("gis_viewer_list_layers"),
            };
            if (PendingVectorRoadsPromise.IsValid() && !RebuildSafeCommands.Contains(CommandType))
            {
                ResultJson = MakeShareable(new FJsonObject);
                ResultJson->SetBoolField(TEXT("success"), false);
                ResultJson->SetStringField(TEXT("error"),
                    TEXT("road rebuild in progress — retry after gis_rebuild_road_networks succeeds"));
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
                FString RebuildBusyOut;
                TSharedRef<TJsonWriter<>> RBW = TJsonWriterFactory<>::Create(&RebuildBusyOut);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), RBW);
                Promise.SetValue(RebuildBusyOut);
                return;
            }

            if (CommandType == TEXT("ping"))
            {
                ResultJson = MakeShareable(new FJsonObject);
                ResultJson->SetStringField(TEXT("message"), TEXT("pong"));
            }
            // Editor Commands (including actor manipulation)
            else if (CommandType == TEXT("get_actors_in_level") ||
                     CommandType == TEXT("find_actors_by_name") ||
                     CommandType == TEXT("spawn_actor") ||
                     CommandType == TEXT("create_actor") ||
                     CommandType == TEXT("delete_actor") ||
                     CommandType == TEXT("set_actor_transform") ||
                     CommandType == TEXT("get_actor_properties") ||
                     CommandType == TEXT("set_actor_property") ||
                     CommandType == TEXT("spawn_blueprint_actor") ||
                     CommandType == TEXT("focus_viewport") ||
                     CommandType == TEXT("execute_python") ||
                     CommandType == TEXT("add_basic_lighting"))
            {
                ResultJson = EditorCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Commands
            else if (CommandType == TEXT("create_blueprint") || 
                     CommandType == TEXT("add_component_to_blueprint") || 
                     CommandType == TEXT("set_component_property") || 
                     CommandType == TEXT("set_physics_properties") || 
                     CommandType == TEXT("compile_blueprint") || 
                     CommandType == TEXT("set_blueprint_property") || 
                     CommandType == TEXT("set_static_mesh_properties") ||
                     CommandType == TEXT("set_pawn_properties"))
            {
                ResultJson = BlueprintCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Node Commands
            else if (CommandType == TEXT("connect_blueprint_nodes") || 
                     CommandType == TEXT("add_blueprint_get_self_component_reference") ||
                     CommandType == TEXT("add_blueprint_self_reference") ||
                     CommandType == TEXT("find_blueprint_nodes") ||
                     CommandType == TEXT("add_blueprint_event_node") ||
                     CommandType == TEXT("add_blueprint_input_action_node") ||
                     CommandType == TEXT("add_blueprint_function_node") ||
                     CommandType == TEXT("add_blueprint_get_component_node") ||
                     CommandType == TEXT("add_blueprint_variable"))
            {
                ResultJson = BlueprintNodeCommands->HandleCommand(CommandType, Params);
            }
            // Project Commands
            else if (CommandType == TEXT("create_input_mapping"))
            {
                ResultJson = ProjectCommands->HandleCommand(CommandType, Params);
            }
            // UMG Commands
            else if (CommandType == TEXT("create_umg_widget_blueprint") ||
                     CommandType == TEXT("add_text_block_to_widget") ||
                     CommandType == TEXT("add_button_to_widget") ||
                     CommandType == TEXT("bind_widget_event") ||
                     CommandType == TEXT("set_text_block_binding") ||
                     CommandType == TEXT("add_widget_to_viewport"))
            {
                ResultJson = UMGCommands->HandleCommand(CommandType, Params);
            }
            // GIS Commands (synchronous; gis_import_landscape and gis_import_vector_roads handled above)
            // roadnet_* diagnostics and viewport_* visualization route through the same
            // FUnrealMCPGISCommands handler.
            else if (CommandType == TEXT("gis_create_level") ||
                     // TODO(rename): gis_open_level -> level_open (not geospatial)
                     CommandType == TEXT("gis_open_level") ||
                     CommandType == TEXT("gis_get_geo_anchor") ||
                     CommandType == TEXT("gis_set_geo_anchor") ||
                     CommandType == TEXT("gis_import_opendrive") ||
                     CommandType == TEXT("gis_list_road_networks") ||
                     CommandType == TEXT("gis_list_road_presets") ||
                     CommandType == TEXT("gis_viewer_load_file") ||
                     CommandType == TEXT("gis_viewer_list_layers") ||
                     CommandType == TEXT("gis_viewer_clear") ||
                     // TODO(rename): gis_focus_landscapes -> viewport_focus_landscapes
                     CommandType == TEXT("gis_focus_landscapes") ||
                     CommandType == TEXT("gis_camera_top_down") ||
                     // TODO(rename): gis_screenshot_markers -> viewport_screenshot_markers
                     CommandType == TEXT("gis_screenshot_markers") ||
                     // TODO(rename): gis_build_zone_graph -> roadnet_build_zone_graph
                     CommandType == TEXT("gis_build_zone_graph") ||
                     CommandType == TEXT("roadnet_summarize_semantic") ||
                     CommandType == TEXT("roadnet_summarize_lane_graph") ||
                     CommandType == TEXT("roadnet_validate") ||
                     CommandType == TEXT("roadnet_reconcile") ||
                     CommandType == TEXT("roadnet_list_markers") ||
                     CommandType == TEXT("gis_list_districts") ||
                     CommandType == TEXT("gis_generate_block_shapes") ||
                     CommandType == TEXT("gis_assign_district") ||
                     CommandType == TEXT("gis_assign_random_districts") ||
                     CommandType == TEXT("gis_generate_buildings") ||
                     CommandType == TEXT("gis_generate_footpaths") ||
                     CommandType == TEXT("gis_generate_procedural_roads") ||
                     CommandType == TEXT("gis_toggle_block_previews") ||
                     CommandType == TEXT("gis_list_sidewalk_presets") ||
                     CommandType == TEXT("gis_set_road_sidewalk") ||
                     CommandType == TEXT("gis_conform_landscape_to_roads") ||
                     CommandType == TEXT("gis_set_world_partition_streaming") ||
                     CommandType == TEXT("gis_disable_external_actors") ||
                     CommandType == TEXT("gis_clear_landscape_splines") ||
                     CommandType == TEXT("gis_save_current_level") ||
                     CommandType == TEXT("gis_set_validate_on_save_disabled") ||
                     CommandType == TEXT("gis_flush_rendering_commands"))
            {
                ResultJson = GISCommands->HandleCommand(CommandType, Params);
            }
            else
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown command: %s"), *CommandType));
                
                FString ResultString;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
                Promise.SetValue(ResultString);
                return;
            }
            
            // Check if the result contains an error
            bool bSuccess = true;
            FString ErrorMessage;
            
            if (ResultJson->HasField(TEXT("success")))
            {
                bSuccess = ResultJson->GetBoolField(TEXT("success"));
                if (!bSuccess && ResultJson->HasField(TEXT("error")))
                {
                    ErrorMessage = ResultJson->GetStringField(TEXT("error"));
                }
            }
            
            if (bSuccess)
            {
                // Set success status and include the result
                ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
            else
            {
                // Set error status and include the error message
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
            }
        }
        catch (const std::exception& e)
        {
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), UTF8_TO_TCHAR(e.what()));
        }
        
        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        Promise.SetValue(ResultString);
    });
    
    // Poll instead of blocking indefinitely so a disconnected client's
    // CLOSE-WAIT socket eventually resolves (ServeClient unblocks when we return).
    constexpr float PollSec  = 0.05f;
    constexpr float LimitSec = 300.f;
    for (float Elapsed = 0.f; Elapsed < LimitSec; Elapsed += PollSec)
    {
        if (Future.IsReady())
        {
            return Future.Get();
        }
        FPlatformProcess::Sleep(PollSec);
    }

    TSharedPtr<FJsonObject> Timeout = MakeShared<FJsonObject>();
    Timeout->SetStringField(TEXT("status"), TEXT("error"));
    Timeout->SetStringField(TEXT("error"), TEXT("Game thread did not respond within 300 s"));
    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Timeout.ToSharedRef(), W);
    return Out;
}