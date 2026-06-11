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

// ServeGISTools — for async landscape + vector roads import
#include "ProcessObjects/ServeProcessRasterToLandscape.h"
#include "ProcessObjects/ServeProcessVectorShapes.h"
#include "Libraries/ServeGDALFunctionLibrary.h"
#include "Anchor/ServeGeoAnchor.h"
#include "Types/ServeGISTypes.h"
#include "Landscape.h" // full ALandscape definition (needed for CreatedLandscapes access)

// RoadBLD — for road creation from vector shapes
#include "DynamicRoad/DynamicRoad.h"
#include "DynamicRoad/DynamicRoadNetwork.h"
#include "Roads/ServeGISRoadStyleTable.h"
#include "GISViewer/ServeGISRoadImportUtils.h"
#include "GISViewer/ServeGISBuildingImportUtils.h"

DEFINE_LOG_CATEGORY(LogUnrealMCP);

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
    UServeGISRoadStyleTable* StyleTable = nullptr;
    {
        FString TablePath;
        if (Params && Params->TryGetStringField(TEXT("style_table_path"), TablePath) && !TablePath.IsEmpty())
        {
            StyleTable = Cast<UServeGISRoadStyleTable>(UEditorAssetLibrary::LoadAsset(TablePath));
            if (!StyleTable)
            {
                UE_LOG(LogTemp, Warning, TEXT("gis_import_vector_roads: style_table_path '%s' could not be loaded"), *TablePath);
            }
        }
        if (!StyleTable)
        {
            StyleTable = LoadObject<UServeGISRoadStyleTable>(nullptr,
                TEXT("/ServeGISTools/Roads/RoadStyleSet_Default.RoadStyleSet_Default"));
        }
        if (!StyleTable)
        {
            UE_LOG(LogTemp, Warning, TEXT("gis_import_vector_roads: no UServeGISRoadStyleTable found — roads will use uninitialized presets"));
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

    // Build road mesh geometry. Without this call roads have control points but no mesh.
    int32 Spawned = 0;
    for (const FServeGISRoadEntry& Info : SpawnedRoads) { if (IsValid(Info.Road)) { ++Spawned; } }

    Network->RebuildRoadNetworkIncremental({}, {}, false);

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
// Async building import (OSM footprint polygons -> CityBLD modular buildings).
// Mirrors the vector-roads async pattern: load the dataset's shapes off the game
// thread via UServeProcessVectorShapes, then spawn buildings in the OnSucceeded
// callback by delegating to the shared FServeGISBuildingImportUtils.
// ---------------------------------------------------------------------------

void UUnrealMCPBridge::StartBuildingsImport(const TSharedPtr<FJsonObject>& Params)
{
    auto FulfillError = [this](const FString& Msg)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("error"));
        Resp->SetStringField(TEXT("error"), Msg);
        FString Out;
        TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
        if (PendingBuildingsPromise.IsValid())
        {
            PendingBuildingsPromise->SetValue(Out);
            PendingBuildingsPromise.Reset();
        }
    };

    FString DatasetPath;
    if (!Params->TryGetStringField(TEXT("dataset_path"), DatasetPath))
    {
        FulfillError(TEXT("gis_import_buildings: dataset_path required"));
        return;
    }

    bool bSuccess;
    FString ErrorMsg;
    UGDALDataset* Dataset = UServeGDALFunctionLibrary::OpenDataset(DatasetPath, /*bReadOnly=*/true, bSuccess, ErrorMsg);
    if (!bSuccess || !Dataset)
    {
        FulfillError(FString::Printf(TEXT("gis_import_buildings: failed to open dataset: %s"), *ErrorMsg));
        return;
    }

    UServeProcessVectorShapes* Proc = NewObject<UServeProcessVectorShapes>();
    Proc->bMergeLineSegments = false; // footprints are polygons; never merge

    // Layer allow-list. Default to the OSM "multipolygons" layer (where building
    // footprints live); an explicit `layer` param overrides it.
    FString LayerName = TEXT("multipolygons");
    Params->TryGetStringField(TEXT("layer"), LayerName);
    Proc->LayerNameFilter.Add(LayerName);

    PendingBuildingsProc   = Proc;
    PendingBuildingsParams = Params;
    Proc->AddToRoot();

    Proc->OnSucceeded.AddDynamic(this, &UUnrealMCPBridge::OnGISBuildingsSucceeded);
    Proc->OnFailed.AddDynamic(this, &UUnrealMCPBridge::OnGISBuildingsFailed);

    Proc->Run(Dataset);
}

void UUnrealMCPBridge::OnGISBuildingsSucceeded()
{
    auto FulfillError = [this](const FString& Msg)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("status"), TEXT("error"));
        Resp->SetStringField(TEXT("error"), Msg);
        FString Out;
        TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
        if (PendingBuildingsProc)
        {
            PendingBuildingsProc->RemoveFromRoot();
            PendingBuildingsProc = nullptr;
        }
        PendingBuildingsParams.Reset();
        if (PendingBuildingsPromise.IsValid())
        {
            PendingBuildingsPromise->SetValue(Out);
            PendingBuildingsPromise.Reset();
        }
    };

    if (!PendingBuildingsProc)
    {
        FulfillError(TEXT("gis_import_buildings: process object lost before callback"));
        return;
    }

    const TArray<FGISShapeFeature>& Shapes = PendingBuildingsProc->Shapes;
    const TSharedPtr<FJsonObject>& Params  = PendingBuildingsParams;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FulfillError(TEXT("gis_import_buildings: no editor world"));
        return;
    }

    AServeGeoAnchor* Anchor = AServeGeoAnchor::FindInWorld(World);

    FServeGISBuildingImportOptions Options;

    // is_geographic: explicit override, else auto-detect from the first point (lon/lat in [-180,180]).
    if (Params)
    {
        bool bGeoOverride = false;
        if (Params->TryGetBoolField(TEXT("is_geographic"), bGeoOverride))
        {
            Options.bIsGeographic = bGeoOverride;
        }
        else
        {
            for (const FGISShapeFeature& S : Shapes)
            {
                if (S.Points.Num() > 0)
                {
                    const double X = S.Points[0].X;
                    Options.bIsGeographic = (X >= -180.0 && X <= 180.0);
                    break;
                }
            }
        }

        int32 MaxB = 0;
        if (Params->TryGetNumberField(TEXT("max_buildings"), MaxB) && MaxB > 0)
        {
            Options.MaxBuildings = MaxB;
        }

        FString FbStr;
        if (Params->TryGetStringField(TEXT("fallback_height"), FbStr))
        {
            Options.FallbackHeightMeters = FCString::Atof(*FbStr);
        }
    }

    // Optional building style asset path; the shared util resolves it (and falls back to the
    // bundled default when empty). Kept as a string so the bridge needs no CityBLD dependency.
    FString StylePath;
    if (Params && Params->TryGetStringField(TEXT("style_path"), StylePath) && !StylePath.IsEmpty())
    {
        Options.StyleAssetPath = StylePath;
    }

    FServeGISBuildingImportResult ImportResult;
    FServeGISBuildingImportUtils::SpawnBuildingsFromShapes(World, Shapes, Anchor, Options, ImportResult);

    PendingBuildingsProc->RemoveFromRoot();
    PendingBuildingsProc = nullptr;
    PendingBuildingsParams.Reset();

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), true);
    R->SetNumberField(TEXT("buildings_spawned"), ImportResult.Spawned);
    R->SetNumberField(TEXT("with_osm_height"), ImportResult.WithOSMHeight);
    R->SetNumberField(TEXT("skipped_non_building"), ImportResult.SkippedNonBld);
    R->SetNumberField(TEXT("skipped_degenerate"), ImportResult.SkippedDegenerate);
    R->SetNumberField(TEXT("over_cap"), ImportResult.CappedAt);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("status"), TEXT("success"));
    Resp->SetObjectField(TEXT("result"), R);

    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Resp.ToSharedRef(), W);

    if (PendingBuildingsPromise.IsValid())
    {
        PendingBuildingsPromise->SetValue(Out);
        PendingBuildingsPromise.Reset();
    }
}

void UUnrealMCPBridge::OnGISBuildingsFailed(const FString& ErrorMessage, int32 ErrorCode)
{
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("status"), TEXT("error"));
    Resp->SetStringField(TEXT("error"), ErrorMessage);

    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Resp.ToSharedRef(), W);

    if (PendingBuildingsProc)
    {
        PendingBuildingsProc->RemoveFromRoot();
        PendingBuildingsProc = nullptr;
    }
    PendingBuildingsParams.Reset();

    if (PendingBuildingsPromise.IsValid())
    {
        PendingBuildingsPromise->SetValue(Out);
        PendingBuildingsPromise.Reset();
    }
}

// ---------------------------------------------------------------------------
// Execute a command received from a client
// ---------------------------------------------------------------------------

FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogUnrealMCP, Log, TEXT(">> cmd=%s"), *CommandType);

    // Async building import (OSM footprints -> CityBLD modular buildings).
    if (CommandType == TEXT("gis_import_buildings"))
    {
        if (PendingBuildingsPromise.IsValid())
        {
            TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
            Err->SetStringField(TEXT("status"), TEXT("error"));
            Err->SetStringField(TEXT("error"), TEXT("gis_import_buildings: another import is already in progress"));
            FString Out;
            TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out);
            FJsonSerializer::Serialize(Err.ToSharedRef(), W2);
            return Out;
        }

        PendingBuildingsPromise = MakeShared<TPromise<FString>>();
        TFuture<FString> BFuture = PendingBuildingsPromise->GetFuture();

        AsyncTask(ENamedThreads::GameThread, [this, Params]()
        {
            StartBuildingsImport(Params);
        });

        constexpr float PollSec  = 0.05f;
        constexpr float LimitSec = 900.f; // hundreds-thousands of modular buildings can be slow
        for (float Elapsed = 0.f; Elapsed < LimitSec; Elapsed += PollSec)
        {
            if (BFuture.IsReady()) return BFuture.Get();
            FPlatformProcess::Sleep(PollSec);
        }

        TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
        T->SetStringField(TEXT("status"), TEXT("error"));
        T->SetStringField(TEXT("error"), TEXT("gis_import_buildings timed out after 900 s"));
        FString Out;
        TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(T.ToSharedRef(), W2);
        PendingBuildingsPromise->SetValue(Out);
        PendingBuildingsPromise.Reset();
        return Out;
    }

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

    // Create a promise to wait for the result
    TPromise<FString> Promise;
    TFuture<FString> Future = Promise.GetFuture();

    // Queue execution on Game Thread
    AsyncTask(ENamedThreads::GameThread, [this, CommandType, Params, Promise = MoveTemp(Promise)]() mutable
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);
        
        try
        {
            TSharedPtr<FJsonObject> ResultJson;
            
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
            else if (CommandType == TEXT("gis_create_level") ||
                     CommandType == TEXT("gis_open_level") ||
                     CommandType == TEXT("gis_get_geo_anchor") ||
                     CommandType == TEXT("gis_set_geo_anchor") ||
                     CommandType == TEXT("gis_import_opendrive") ||
                     CommandType == TEXT("gis_list_road_networks") ||
                     CommandType == TEXT("gis_list_road_presets") ||
                     CommandType == TEXT("gis_viewer_load_file") ||
                     CommandType == TEXT("gis_viewer_list_layers") ||
                     CommandType == TEXT("gis_viewer_clear") ||
                     CommandType == TEXT("gis_focus_landscapes") ||
                     CommandType == TEXT("gis_screenshot_markers") ||
                     CommandType == TEXT("usim_generate_zonegraph"))
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