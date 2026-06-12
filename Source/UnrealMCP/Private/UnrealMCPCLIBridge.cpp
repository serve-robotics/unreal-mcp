#include "UnrealMCPCLIBridge.h"
#include "UnrealMCPBridge.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 error codes
// ---------------------------------------------------------------------------
namespace MCPError
{
    constexpr int32 ParseError     = -32700;
    constexpr int32 InvalidRequest = -32600;
    constexpr int32 MethodNotFound = -32601;
    constexpr int32 InvalidParams  = -32602;
    constexpr int32 InternalError  = -32603;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

FUnrealMCPCLIBridge::FUnrealMCPCLIBridge(UUnrealMCPBridge* InBridge)
    : Bridge(InBridge)
{
}

FUnrealMCPCLIBridge::~FUnrealMCPCLIBridge()
{
    Stop();
}

void FUnrealMCPCLIBridge::Start()
{
    if (bRunning) { return; }
    Thread = FRunnableThread::Create(this, TEXT("UnrealMCPCLIBridge"), 0, TPri_Normal);
}

void FUnrealMCPCLIBridge::Stop()
{
    bRunning = false;
    if (Thread)
    {
        Thread->Kill(false); // don't block — stdin read will unblock on editor exit
        delete Thread;
        Thread = nullptr;
    }
}

bool FUnrealMCPCLIBridge::Init()
{
    bRunning = true;
    UE_LOG(LogTemp, Display, TEXT("MCPCLIBridge: stdio transport started — reading stdin"));
    return true;
}

void FUnrealMCPCLIBridge::Exit()
{
    UE_LOG(LogTemp, Display, TEXT("MCPCLIBridge: stdio transport stopped"));
}

// ---------------------------------------------------------------------------
// Main read loop
// ---------------------------------------------------------------------------

uint32 FUnrealMCPCLIBridge::Run()
{
    // stdin is already open; read line-by-line.
    // FPlatformMisc::GetStdin() returns the C FILE* for stdin.
    // We use fgets which blocks until a newline or EOF.
    FILE* In = stdin;

    constexpr int32 BufSize = 1024 * 256; // 256 KB — large enough for any tool call
    TArray<char> Buf;
    Buf.SetNumUninitialized(BufSize);

    FString Overflow; // accumulates if a message spans multiple fgets calls

    while (bRunning)
    {
        if (!fgets(Buf.GetData(), BufSize, In))
        {
            // EOF or error — the host process closed stdin; clean exit.
            UE_LOG(LogTemp, Display, TEXT("MCPCLIBridge: stdin closed, exiting"));
            break;
        }

        FString Chunk = UTF8_TO_TCHAR(Buf.GetData());
        Overflow += Chunk;

        // Process every complete newline-terminated line in the accumulator.
        int32 NlPos;
        while (Overflow.FindChar(TEXT('\n'), NlPos))
        {
            FString Line = Overflow.Left(NlPos).TrimEnd();
            Overflow.RightInline(Overflow.Len() - NlPos - 1);

            if (Line.IsEmpty()) { continue; }

            // Parse JSON-RPC message
            TSharedPtr<FJsonObject> Msg;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
            if (!FJsonSerializer::Deserialize(Reader, Msg) || !Msg.IsValid())
            {
                UE_LOG(LogTemp, Warning, TEXT("MCPCLIBridge: invalid JSON: %.200s"), *Line);
                // id is unknown — use null
                WriteLine(MakeError(MakeShared<FJsonValueNull>(),
                    MCPError::ParseError, TEXT("Parse error")));
                continue;
            }

            // Extract id (may be number, string, or null — all valid per spec)
            TSharedPtr<FJsonValue> Id;
            if (Msg->HasField(TEXT("id")))
            {
                Id = Msg->TryGetField(TEXT("id"));
            }

            FString Method;
            if (!Msg->TryGetStringField(TEXT("method"), Method))
            {
                // Notifications have no method field sometimes; ignore gracefully
                continue;
            }

            // Params are optional
            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            const TSharedPtr<FJsonObject>* ParamsObj;
            if (Msg->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj)
            {
                Params = *ParamsObj;
            }

            // ----------------------------------------------------------------
            // Dispatch by method
            // ----------------------------------------------------------------
            FString Response;

            if (Method == TEXT("initialize"))
            {
                Response = HandleInitialize(Params, Id);
            }
            else if (Method == TEXT("initialized"))
            {
                // Notification — no response per spec
                UE_LOG(LogTemp, Display, TEXT("MCPCLIBridge: received 'initialized' notification"));
                continue;
            }
            else if (Method == TEXT("tools/list"))
            {
                Response = HandleToolsList(Id);
            }
            else if (Method == TEXT("tools/call"))
            {
                Response = HandleToolsCall(Params, Id);
            }
            else if (Method == TEXT("ping"))
            {
                Response = HandlePing(Id);
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("MCPCLIBridge: unknown method '%s'"), *Method);
                if (Id.IsValid())
                {
                    Response = MakeError(Id, MCPError::MethodNotFound,
                        FString::Printf(TEXT("Method not found: %s"), *Method));
                }
                // else: notification — no error response
            }

            if (!Response.IsEmpty())
            {
                WriteLine(Response);
            }
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// MCP message handlers
// ---------------------------------------------------------------------------

FString FUnrealMCPCLIBridge::HandleInitialize(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& Id)
{
    // Log client info if present
    FString ClientName;
    const TSharedPtr<FJsonObject>* ClientInfo;
    if (Params->TryGetObjectField(TEXT("clientInfo"), ClientInfo))
    {
        (*ClientInfo)->TryGetStringField(TEXT("name"), ClientName);
        UE_LOG(LogTemp, Display, TEXT("MCPCLIBridge: client '%s' connected"), *ClientName);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // Protocol version we speak
    Result->SetStringField(TEXT("protocolVersion"), TEXT("2024-11-05"));

    // Server info
    TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
    ServerInfo->SetStringField(TEXT("name"), TEXT("unreal-mcp"));
    ServerInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
    Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

    // Capabilities — we support tools
    TSharedPtr<FJsonObject> Caps = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
    Caps->SetObjectField(TEXT("tools"), ToolsCap);
    Result->SetObjectField(TEXT("capabilities"), Caps);

    return MakeResult(Id, Result);
}

FString FUnrealMCPCLIBridge::HandleToolsList(const TSharedPtr<FJsonValue>& Id)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("tools"), BuildToolList());
    return MakeResult(Id, Result);
}

FString FUnrealMCPCLIBridge::HandleToolsCall(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& Id)
{
    FString ToolName;
    if (!Params->TryGetStringField(TEXT("name"), ToolName))
    {
        return MakeError(Id, MCPError::InvalidParams, TEXT("'name' field required in tools/call params"));
    }

    // Arguments are optional
    TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject>* ArgsObj;
    if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj)
    {
        Arguments = *ArgsObj;
    }

    // Dispatch through the existing UnrealMCP command system.
    // ToolName maps directly to CommandType (e.g. "spawn_actor", "create_blueprint").
    UE_LOG(LogTemp, Display, TEXT("MCPCLIBridge: tools/call '%s'"), *ToolName);
    const FString CommandResult = Bridge->ExecuteCommand(ToolName, Arguments);

    // Wrap the raw command JSON string in an MCP content array
    return MakeTextResult(Id, CommandResult);
}

FString FUnrealMCPCLIBridge::HandlePing(const TSharedPtr<FJsonValue>& Id)
{
    return MakeResult(Id, MakeShared<FJsonObject>()); // empty result object
}

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------

FString FUnrealMCPCLIBridge::MakeResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result)
{
    TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
    Envelope->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    if (Id.IsValid())
    {
        Envelope->SetField(TEXT("id"), Id);
    }
    else
    {
        Envelope->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
    }
    Envelope->SetObjectField(TEXT("result"), Result);

    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Envelope.ToSharedRef(), W);
    return Out;
}

FString FUnrealMCPCLIBridge::MakeTextResult(const TSharedPtr<FJsonValue>& Id, const FString& Text)
{
    // MCP tools/call result format:
    // { "content": [{ "type": "text", "text": "<json string>" }] }
    TSharedPtr<FJsonObject> ContentItem = MakeShared<FJsonObject>();
    ContentItem->SetStringField(TEXT("type"), TEXT("text"));
    ContentItem->SetStringField(TEXT("text"), Text);

    TArray<TSharedPtr<FJsonValue>> ContentArray;
    ContentArray.Add(MakeShared<FJsonValueObject>(ContentItem));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("content"), ContentArray);

    return MakeResult(Id, Result);
}

FString FUnrealMCPCLIBridge::MakeError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
{
    TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
    Err->SetNumberField(TEXT("code"), Code);
    Err->SetStringField(TEXT("message"), Message);

    TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
    Envelope->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    if (Id.IsValid())
    {
        Envelope->SetField(TEXT("id"), Id);
    }
    else
    {
        Envelope->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
    }
    Envelope->SetObjectField(TEXT("error"), Err);

    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Envelope.ToSharedRef(), W);
    return Out;
}

// ---------------------------------------------------------------------------
// Tool catalogue
// ---------------------------------------------------------------------------
// Each entry follows the MCP tool schema:
//   { "name", "description", "inputSchema": { "type":"object", "properties":{...}, "required":[...] } }

namespace
{
    TSharedPtr<FJsonObject> MakeTool(const FString& Name, const FString& Desc,
        TSharedPtr<FJsonObject> Schema)
    {
        TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
        T->SetStringField(TEXT("name"), Name);
        T->SetStringField(TEXT("description"), Desc);
        T->SetObjectField(TEXT("inputSchema"), Schema);
        return T;
    }

    TSharedPtr<FJsonObject> SimpleSchema(TArray<TPair<FString,FString>> Props,
        TArray<FString> Required = {})
    {
        TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("object"));

        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        for (auto& [PropName, PropDesc] : Props)
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("type"), TEXT("string"));
            P->SetStringField(TEXT("description"), PropDesc);
            PropsObj->SetObjectField(PropName, P);
        }
        Schema->SetObjectField(TEXT("properties"), PropsObj);

        if (!Required.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> Req;
            for (auto& R : Required) { Req.Add(MakeShared<FJsonValueString>(R)); }
            Schema->SetArrayField(TEXT("required"), Req);
        }
        return Schema;
    }
}

TArray<TSharedPtr<FJsonValue>> FUnrealMCPCLIBridge::BuildToolList()
{
    TArray<TSharedPtr<FJsonValue>> Tools;

    auto Add = [&](TSharedPtr<FJsonObject> T)
    {
        Tools.Add(MakeShared<FJsonValueObject>(T));
    };

    // ---- Editor commands ----
    Add(MakeTool(TEXT("ping"),
        TEXT("Test connectivity — returns {\"message\":\"pong\"}"),
        SimpleSchema({})));

    Add(MakeTool(TEXT("get_actors_in_level"),
        TEXT("List all actors currently in the editor level"),
        SimpleSchema({})));

    Add(MakeTool(TEXT("find_actors_by_name"),
        TEXT("Find actors whose name contains the search string"),
        SimpleSchema({{"name", "Actor name substring to search for"}}, {"name"})));

    Add(MakeTool(TEXT("spawn_actor"),
        TEXT("Spawn a new actor at the given location. type: StaticMeshActor | DirectionalLight | PointLight | SpotLight | CameraActor"),
        SimpleSchema({
            {"type",  "Actor type to spawn"},
            {"name",  "Label for the new actor"},
            {"x", "World X position (cm)"}, {"y", "World Y position (cm)"}, {"z", "World Z position (cm)"}
        }, {"type", "name"})));

    Add(MakeTool(TEXT("delete_actor"),
        TEXT("Delete an actor from the level by name"),
        SimpleSchema({{"name", "Actor label to delete"}}, {"name"})));

    Add(MakeTool(TEXT("set_actor_transform"),
        TEXT("Set the world transform of an actor"),
        SimpleSchema({
            {"name", "Actor label"},
            {"x","X"}, {"y","Y"}, {"z","Z"},
            {"pitch","Pitch"}, {"yaw","Yaw"}, {"roll","Roll"},
            {"scale_x","ScaleX"}, {"scale_y","ScaleY"}, {"scale_z","ScaleZ"}
        }, {"name"})));

    Add(MakeTool(TEXT("get_actor_properties"),
        TEXT("Get all properties of a named actor"),
        SimpleSchema({{"name", "Actor label"}}, {"name"})));

    Add(MakeTool(TEXT("execute_python"),
        TEXT("Execute a Python snippet in the Unreal editor context. Use temp files to return data."),
        SimpleSchema({{"code", "Python code string"}}, {"code"})));

    Add(MakeTool(TEXT("focus_viewport"),
        TEXT("Focus the editor viewport on a named actor"),
        SimpleSchema({{"name", "Actor label"}}, {"name"})));

    Add(MakeTool(TEXT("take_screenshot"),
        TEXT("Capture a screenshot and save to the given path"),
        SimpleSchema({{"path", "Output file path (.png)"}}, {"path"})));

    Add(MakeTool(TEXT("add_basic_lighting"),
        TEXT("Add a basic lighting setup to the current level: DirectionalLight (sun), SkyAtmosphere, SkyLight, and ExponentialHeightFog. Skips any actor type already present. Returns a list of spawned actors."),
        SimpleSchema({}, {})));

    Add(MakeTool(TEXT("spawn_blueprint_actor"),
        TEXT("Spawn an instance of a Blueprint class in the level"),
        SimpleSchema({
            {"blueprint_path", "Asset path to the Blueprint class"},
            {"name", "Label for the new actor"},
            {"x","X"}, {"y","Y"}, {"z","Z"}
        }, {"blueprint_path", "name"})));

    // ---- Blueprint commands ----
    Add(MakeTool(TEXT("create_blueprint"),
        TEXT("Create a new Blueprint class asset"),
        SimpleSchema({
            {"name",       "Asset name"},
            {"parent",     "Parent class name (e.g. Actor, Pawn)"},
            {"save_path",  "Content browser path"}
        }, {"name", "parent"})));

    Add(MakeTool(TEXT("add_component_to_blueprint"),
        TEXT("Add a component to a Blueprint"),
        SimpleSchema({
            {"blueprint_name",  "Blueprint asset name"},
            {"component_type",  "Component class (e.g. StaticMeshComponent)"},
            {"component_name",  "Name for the component"}
        }, {"blueprint_name", "component_type", "component_name"})));

    Add(MakeTool(TEXT("compile_blueprint"),
        TEXT("Compile a Blueprint"),
        SimpleSchema({{"blueprint_name", "Blueprint asset name"}}, {"blueprint_name"})));

    Add(MakeTool(TEXT("set_blueprint_property"),
        TEXT("Set a default property on a Blueprint"),
        SimpleSchema({
            {"blueprint_name", "Blueprint asset name"},
            {"property_name",  "Property name"},
            {"property_value", "Value as string"}
        }, {"blueprint_name", "property_name", "property_value"})));

    // ---- Blueprint node commands ----
    Add(MakeTool(TEXT("add_blueprint_event_node"),
        TEXT("Add an event node (e.g. Event BeginPlay) to a Blueprint graph"),
        SimpleSchema({
            {"blueprint_name", "Blueprint asset name"},
            {"event_name",     "Event name (e.g. ReceiveBeginPlay)"}
        }, {"blueprint_name", "event_name"})));

    Add(MakeTool(TEXT("add_blueprint_function_node"),
        TEXT("Add a function call node to a Blueprint graph"),
        SimpleSchema({
            {"blueprint_name",  "Blueprint asset name"},
            {"function_name",   "Function to call"},
            {"target",          "Target object (optional)"}
        }, {"blueprint_name", "function_name"})));

    Add(MakeTool(TEXT("connect_blueprint_nodes"),
        TEXT("Connect two Blueprint graph nodes by pin names"),
        SimpleSchema({
            {"blueprint_name",  "Blueprint asset name"},
            {"source_node",     "Source node name"},
            {"source_pin",      "Source pin name"},
            {"target_node",     "Target node name"},
            {"target_pin",      "Target pin name"}
        }, {"blueprint_name", "source_node", "source_pin", "target_node", "target_pin"})));

    Add(MakeTool(TEXT("add_blueprint_variable"),
        TEXT("Declare a variable on a Blueprint"),
        SimpleSchema({
            {"blueprint_name", "Blueprint asset name"},
            {"variable_name",  "Variable name"},
            {"variable_type",  "Type (bool, int, float, string, vector, object)"}
        }, {"blueprint_name", "variable_name", "variable_type"})));

    // ---- UMG commands ----
    Add(MakeTool(TEXT("create_umg_widget_blueprint"),
        TEXT("Create a new UMG Widget Blueprint asset"),
        SimpleSchema({
            {"name",      "Widget asset name"},
            {"save_path", "Content browser path"}
        }, {"name"})));

    Add(MakeTool(TEXT("add_text_block_to_widget"),
        TEXT("Add a TextBlock widget to a UMG Widget Blueprint"),
        SimpleSchema({
            {"widget_name", "Widget Blueprint name"},
            {"text",        "Default text"},
            {"x","X"}, {"y","Y"}
        }, {"widget_name", "text"})));

    Add(MakeTool(TEXT("add_button_to_widget"),
        TEXT("Add a Button widget to a UMG Widget Blueprint"),
        SimpleSchema({
            {"widget_name", "Widget Blueprint name"},
            {"button_name", "Name for the button"},
            {"x","X"}, {"y","Y"}
        }, {"widget_name", "button_name"})));

    Add(MakeTool(TEXT("add_widget_to_viewport"),
        TEXT("Instantiate a Widget Blueprint and add it to the game viewport"),
        SimpleSchema({{"widget_name", "Widget Blueprint name"}}, {"widget_name"})));

    // ---- GIS commands ----
    Add(MakeTool(TEXT("gis_create_level"),
        TEXT("Create a new empty UE level and open it. Tip: use /Game/Maps/<Name> as level_path."),
        SimpleSchema({
            {"level_path", "Content-browser path for the new level (e.g. /Game/Maps/CityBlock01)"}
        }, {"level_path"})));

    Add(MakeTool(TEXT("gis_open_level"),
        TEXT("Open an existing level by content-browser path"),
        SimpleSchema({
            {"level_path", "Content-browser path (e.g. /Game/Maps/CityBlock01)"}
        }, {"level_path"})));

    Add(MakeTool(TEXT("gis_get_geo_anchor"),
        TEXT("Return the AServeGeoAnchor properties from the current level (EPSG, origin, elevation range, m/quad)"),
        SimpleSchema({})));

    Add(MakeTool(TEXT("gis_set_geo_anchor"),
        TEXT("Create or update the AServeGeoAnchor in the current level. All params optional except at least one must be set."),
        SimpleSchema({
            {"epsg",           "EPSG code of project CRS (e.g. 32618 for UTM zone 18N)"},
            {"origin_x",       "World-origin easting/longitude in project CRS"},
            {"origin_y",       "World-origin northing/latitude in project CRS"},
            {"origin_z",       "World-origin elevation in meters"},
            {"min_elev",       "Global minimum elevation (meters) for landscape encoding"},
            {"max_elev",       "Global maximum elevation (meters) for landscape encoding"},
            {"meters_per_quad","Preferred landscape meters-per-quad (XY scale)"},
            {"crs_wkt",        "Optional full WKT for the project CRS (overrides EPSG if set)"}
        }, {})));

    Add(MakeTool(TEXT("gis_import_landscape"),
        TEXT("Import a raster file (GeoTIFF, etc.) as one or more UE Landscape actors. Async — may take 30-120 s for large tiles."),
        SimpleSchema({
            {"dataset_path",   "Absolute path to the raster file (GeoTIFF, .img, .asc, etc.)"},
            {"meters_per_quad","Landscape horizontal scale in meters per quad (default 1.0)"},
            {"elevation_band", "1-based band index for elevation (default: auto-detect)"},
            {"min_elev",       "Min elevation in meters — if omitted, computed from data"},
            {"max_elev",       "Max elevation in meters — if omitted, computed from data"}
        }, {"dataset_path"})));

    Add(MakeTool(TEXT("gis_import_vector_roads"),
        TEXT("Import line-string features from a vector file (.gpkg, .shp, .geojson, etc.) as RoadBLD roads. Async — may take 10-60 s for large datasets."),
        SimpleSchema({
            {"dataset_path",   "Absolute path to the vector file (.gpkg, .shp, .geojson, etc.)"},
            {"network_name",   "Label of the ADynamicRoadNetwork actor to use (optional — uses first found, or creates one)"},
            {"preset_path",    "Content-browser path to a UDynamicRoadDrawPreset asset (optional)"},
            {"is_geographic",  "true if source CRS is geographic (degrees), e.g. EPSG:4326/4269 (default: auto-detect)"},
            {"merge_segments", "true to stitch disjoint line segments into longer chains (default: false — merging can produce degenerate shapes in some gpkg files)"},
            {"sample_eps",     "RDP simplification tolerance in meters (default: disabled)"}
        }, {"dataset_path"})));

    Add(MakeTool(TEXT("gis_import_opendrive"),
        TEXT("Import roads from an OpenDRIVE (.xodr) file into an ADynamicRoadNetwork actor"),
        SimpleSchema({
            {"file_path",    "Absolute path to the .xodr file"},
            {"network_name", "Label of the ADynamicRoadNetwork actor (optional — uses first found if omitted)"},
            {"preset_path",  "Content-browser path to a UDynamicRoadDrawPreset asset (optional)"},
            {"sample_eps",   "Chord-error tolerance in meters for reference-line sampling (default 0.5)"}
        }, {"file_path"})));

    Add(MakeTool(TEXT("gis_list_road_networks"),
        TEXT("List all ADynamicRoadNetwork actors in the current level"),
        SimpleSchema({})));

    Add(MakeTool(TEXT("gis_list_road_presets"),
        TEXT("List all UDynamicRoadDrawPreset assets in the project (use path with gis_import_opendrive)"),
        SimpleSchema({})));

    Add(MakeTool(TEXT("gis_viewer_load_file"),
        TEXT("Load a GIS file (GeoTIFF, .gpkg, .shp, .xodr, etc.) into the GIS Viewer dataset so it appears in the viewer panel"),
        SimpleSchema({
            {"file_path", "Absolute path to the GIS file to load"}
        }, {"file_path"})));

    Add(MakeTool(TEXT("gis_viewer_list_layers"),
        TEXT("List all layers currently loaded in the GIS Viewer dataset"),
        SimpleSchema({})));

    Add(MakeTool(TEXT("gis_viewer_clear"),
        TEXT("Remove all layers from the GIS Viewer dataset"),
        SimpleSchema({})));

    Add(MakeTool(TEXT("gis_screenshot_markers"),
        TEXT("Iterate GIS Report Marker actors in the level and capture two screenshots per marker: "
             "a perspective view (pitch=-45, yaw=45) and a top-down view. "
             "The framing box is the actor's own bounds inflated by the inflate factor. "
             "Returns an array of entries with actor name, label, lat/lon, and screenshot paths."),
        SimpleSchema({
            {"tag",     "Actor tag filter — only process markers with this tag (omit to include all)"},
            {"inflate", "Multiplier applied to the actor's bounds extent for the framing box (default 5)"}
        }, {})));

    Add(MakeTool(TEXT("gis_build_zone_graph"),
        TEXT("Run the full Tempo zone graph pipeline on the current level: "
             "SetupZoneGraphBuilder → TryGenerateZoneShapeComponents → BuildZoneGraph. "
             "GIS-imported road actors (ADynamicRoad) implement ITempoRoadInterface and will "
             "receive UZoneShapeComponents; the ZoneGraph is then rebuilt. "
             "Returns road_actor_count and a success message, or an error with diagnostic info."),
        SimpleSchema({}, {})));

    return Tools;
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------

void FUnrealMCPCLIBridge::WriteLine(const FString& Line)
{
    // Write newline-terminated UTF-8 to stdout.
    const FTCHARToUTF8 Utf8(*(Line + TEXT("\n")));
    fwrite(Utf8.Get(), 1, Utf8.Length(), stdout);
    fflush(stdout);
}
