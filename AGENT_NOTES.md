# UnrealMCP — Agent Notes

This plugin runs a TCP JSON server inside the Unreal Editor at **127.0.0.1:55557**.
It starts automatically when the editor opens (via `UUnrealMCPBridge` EditorSubsystem).

Agents can connect and drive the editor programmatically without human interaction.

---

## Transports

### TCP Bridge (default)
Always active. Connect to `127.0.0.1:55557`. Send a newline-terminated JSON request,
read a newline-terminated JSON response, then close the connection.

```json
// Request
{"type": "command_name", "params": {...}}

// Response
{"status": "success", "result": {...}}
{"status": "error",   "error":  "..."}
```

### CLI Bridge — MCP stdio transport (opt-in)
Launch the editor with `-MCPStdio` to activate a second transport that speaks
**MCP JSON-RPC 2.0 over stdin/stdout**. This lets Claude Code or Claude Desktop
add the editor directly as an MCP server — no TCP port configuration needed.

Add to your `.mcp.json`:
```json
{
  "mcpServers": {
    "unreal": {
      "command": "/path/to/UnrealEditor",
      "args": [
        "/path/to/YourProject.uproject",
        "-MCPStdio",
        "-nullrhi",
        "-nosplash"
      ]
    }
  }
}
```

Wire format (stdin → editor):
```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"spawn_actor","arguments":{"type":"StaticMeshActor","name":"Box","x":"0","y":"0","z":"100"}}}
```

Wire format (editor → stdout):
```json
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"status\":\"success\",\"result\":{...}}"}]}}
```

Both transports share the same command dispatcher and tool set.

---

## Protocol (TCP)

---

## Commands

### execute_python ⭐ most powerful
Run arbitrary Unreal Python on the game thread. Use this for anything not covered
by the other commands — asset editing, data asset population, batch operations, etc.

```json
{"type": "execute_python", "params": {"script": "import unreal\nunreal.log('hello')"}}
```

### Editor / Level
| command | key params | notes |
|---|---|---|
| `get_actors_in_level` | — | list all actors |
| `find_actors_by_name` | `pattern` | substring search |
| `spawn_actor` | `type`, `name`, `location`, `rotation`, `scale` | types: StaticMeshActor, PointLight, SpotLight, DirectionalLight, CameraActor |
| `delete_actor` | `name` | |
| `set_actor_transform` | `name`, `location`/`rotation`/`scale` | |
| `get_actor_properties` | `name` | |
| `set_actor_property` | `name`, `property`, `value` | |
| `spawn_blueprint_actor` | `blueprint_path`, `name`, `location`/`rotation`/`scale` | |
| `focus_viewport` | `actor_name` or `location` | |
| `take_screenshot` | `filepath` | saves PNG |

### Blueprint Authoring
| command | key params |
|---|---|
| `create_blueprint` | `name`, `parent_class` |
| `add_component_to_blueprint` | `blueprint_name`, `component_type`, `component_name` |
| `set_component_property` | `blueprint_name`, `component_name`, `property`, `value` |
| `set_static_mesh_properties` | `blueprint_name`, `component_name`, `mesh_path`, `material_path` |
| `set_physics_properties` | `blueprint_name`, `component_name`, `simulate_physics`, `mass`, `damping` |
| `set_blueprint_property` | `blueprint_name`, `property`, `value` |
| `set_pawn_properties` | `blueprint_name`, ... |
| `compile_blueprint` | `blueprint_name` |

### Blueprint Graph
| command | key params |
|---|---|
| `add_blueprint_event_node` | `blueprint_name`, `event_name` |
| `add_blueprint_function_node` | `blueprint_name`, `target`, `function_name`, `params` |
| `connect_blueprint_nodes` | `blueprint_name`, `source_node`, `source_pin`, `target_node`, `target_pin` |
| `add_blueprint_variable` | `blueprint_name`, `variable_name`, `variable_type` |
| `add_blueprint_input_action_node` | `blueprint_name`, `action_name` |
| `find_blueprint_nodes` | `blueprint_name`, `node_type` |

### UMG Widgets
| command | key params |
|---|---|
| `create_umg_widget_blueprint` | `name` |
| `add_text_block_to_widget` | `widget_name`, `text`, `position` |
| `add_button_to_widget` | `widget_name`, `text`, `position` |
| `bind_widget_event` | `widget_name`, `widget_component`, `event_name` |
| `set_text_block_binding` | `widget_name`, `text_block_name`, `function_name` |

### Project
| command | key params |
|---|---|
| `create_input_mapping` | `action_name`, `key`, `shift`/`ctrl`/`alt` |

---

## Useful Procedures — Update This Section

### Sandbox note — localhost connections
The Claude Code sandbox blocks localhost TCP by default. All MCP `send_command` calls from
the host terminal must use `dangerouslyDisableSandbox=true` on the Bash tool call.
The port is 55557; verify the editor is running first with `ss -tlnp | grep 55557` (also
needs sandbox disabled — alternatively check `Saved/Logs/VayuSim*.log` for "Server started").

### Populate a UDataAsset via MCP
Use `execute_python` with the `unreal` module. Example: editing a `UServeGISRoadStyleTable`:
```python
import unreal
table = unreal.EditorAssetLibrary.load_asset("/MyPlugin/Data/MyTable")
entries = []
e = unreal.ServeGISRoadStyleEntry()
e.set_editor_property("tag_value", "residential")
e.set_editor_property("two_way_preset", unreal.load_class(None, "/Path/To/Preset_C"))
entries.append(e)
table.set_editor_property("entries", entries)
unreal.EditorAssetLibrary.save_asset("/MyPlugin/Data/MyTable")
```
See `ServeGISTools/Scripts/mcp_setup_road_style_table.py` for a full working example.

### Blueprint/preset CDO properties
Presets are Blueprint classes. Use `unreal.load_class(None, "/Path/Name.Name_C")` then
`unreal.get_default_object(cls)` to get the CDO. Lane structs: `unreal.DynamicRoadLaneProfile`
with properties `type` (unreal.LaneType enum) and `width` (float, cm). Module structs:
`unreal.DynamicRoadDrawPresetModule` with `module_class` (a Blueprint class ref).
Lane type enum values: `NORMAL`, `BORDER`, `SHOULDER`, `MEDIAN`, `CENTER_TURN`, `PARKING`, `RESTRICTED`.

### Read output from Unreal Python
`unreal.log()` goes to the editor Output Log, NOT to the TCP response.
To get data back from `execute_python`, write to a temp file and read it from Python:
```python
# In the execute_python script:
import json
with open("/tmp/result.json", "w") as f:
    json.dump(result, f)

# In the host Python after send_command:
with open("/tmp/result.json") as f:
    data = json.load(f)
```

---

## ServeGISTools Plugin

### GIS MCP Utilities
`ServeGISTools/Scripts/gis_mcp_utils.py` is a Python module with ready-to-use functions.
Execute it directly via MCP to get a world state snapshot, or exec() it to call functions.

**Key functions:**
| function | description |
|---|---|
| `snapshot_world()` | Write GeoAnchors + road networks + style table to `/tmp/gis_state.json` |
| `list_geo_anchors()` | All AServeGeoAnchor actors with EPSG, origin XY, Z |
| `list_road_networks()` | All ADynamicRoadNetwork actors with road counts |
| `delete_road_networks(pattern)` | Delete networks matching name pattern (None = all) |
| `get_style_table_info(path)` | List entries in a UServeGISRoadStyleTable |
| `inspect_preset(name)` | Lane layout + modules for a road preset CDO |
| `list_gpkg_layers(path)` | List layers + feature counts in a GeoPackage file |
| `get_gpkg_layer_schema(path, layer)` | Field names/types for a GeoPackage layer |
| `create_road_network_from_gpkg(...)` | Full road creation pipeline from a GeoPackage layer |

**Road creation example:**
```python
exec(open("/path/to/Scripts/gis_mcp_utils.py").read())
result = create_road_network_from_gpkg(
    gpkg_path="/path/to/data.gpkg",
    layer_name="lines",
    anchor_name=None,            # auto-finds first anchor
    style_table_path="/ServeGISTools/Roads/RoadStyleSet_Default",
    vertical_offset_cm=25.0,
    sample_eps_m=0.5,
)
```

**GDAL binaries** are at `ServeGISTools/Source/ThirdParty/GDAL/_prefix/Linux/x86_64/bin/`.
`ogrinfo` and `ogr2ogr` work via subprocess from execute_python. Set `LD_LIBRARY_PATH` to
the corresponding `lib/` directory, `GDAL_DATA` to `share/gdal`, `PROJ_DATA` to `share/proj`.

### Road preset content paths
All presets: `/ServeGISTools/Roads/Presets/<Name>.<Name>_C`
Style table:  `/ServeGISTools/Roads/RoadStyleSet_Default`
OSM highway= tag → preset mapping is in the style table; modify via MCP script or Content Browser.

### Trace vertical offset default
Default is **25 cm** (set in `SServeGISRoadsPanel.cpp`). Applied as:
`BaseElevation = LandscapeHitZ + VerticalOffsetCm + LayerTagOffsetCm`
where `LayerTagOffsetCm = OSM_layer_value * LayerElevFactorCm` (default 500 cm/unit = 5 m).

### Hot-reload after C++ rebuild
After rebuilding the UnrealMCP plugin, the editor must reload it before new commands
are available. Either restart the editor or use Edit → Plugins to disable/re-enable UnrealMCP.

---

*Update this file whenever you discover a new useful procedure or capability.*
