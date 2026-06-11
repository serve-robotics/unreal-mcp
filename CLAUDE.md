# UnrealMCP — Claude Code Plugin Notes

**Owned plugin** (repo `serve-robotics/unreal-mcp`, working branch `GISintegration`; PR open to merge into `main`). Fixes go here. Bump the micro in `UnrealMCP.uplugin` (`VersionName`, currently **1.0.2**) on any C++ change — see the root `CLAUDE.md` version rules.

## What it is
Model Context Protocol implementation for Unreal Engine: a TCP/JSON-RPC server that runs **inside the editor** and lets agents drive it programmatically. Editor-only plugin (`Type: Editor`, one module `Source/UnrealMCP/`). The bridge starts automatically when the editor opens via the `UUnrealMCPBridge` `UEditorSubsystem`.

## Command reference lives in AGENT_NOTES.md
**`AGENT_NOTES.md` (plugin root) is the authoritative command catalogue and protocol reference.** Do not duplicate it here. It covers the two transports, the full command list (editor/level, blueprint, UMG, project input, GIS), the `execute_python` escape hatch, and procedural gotchas. This file documents the *code structure* so you can edit the plugin; AGENT_NOTES documents *using* it.

## Transports (both share one dispatcher)
- **TCP bridge (default, always on):** `127.0.0.1:55557`, newline-terminated JSON, one request per connection. `{"type": "...", "params": {...}}` → `{"status": "success"|"error", ...}`. Defined by `MCP_SERVER_HOST`/`MCP_SERVER_PORT` in `UnrealMCPBridge.cpp`.
- **stdio MCP (opt-in):** launch the editor with `-MCPStdio` to start `FUnrealMCPCLIBridge` (JSON-RPC 2.0 over stdin/stdout), so the editor can be registered directly as an `.mcp.json` server. Started in `UUnrealMCPBridge::Initialize` when the `MCPStdio` command-line param is present.

## Architecture
- `UUnrealMCPBridge` (`Public/UnrealMCPBridge.h`) — the EditorSubsystem. Owns the TCP server thread and constructs six command-handler objects: `EditorCommands`, `BlueprintCommands`, `BlueprintNodeCommands`, `ProjectCommands`, `UMGCommands`, `GISCommands`. Single public entry point: `ExecuteCommand(CommandType, Params)`.
- `FMCPServerRunnable` (`Public/MCPServerRunnable.h`) — TCP accept loop. Offloads each accepted connection to its own thread so a long-running command never blocks the accept loop; commands are serialized only at the game-thread execution step.
- `FUnrealMCPCLIBridge` — the `-MCPStdio` transport reader thread.
- `Private/Commands/` — one handler file per domain, each a `FUnrealMCP*Commands` class with a `HandleCommand(type, params)` dispatcher:
  - `UnrealMCPEditorCommands` — actors, viewport, `execute_python`, `add_basic_lighting`, `take_screenshot` (stub — see below)
  - `UnrealMCPBlueprintCommands` / `UnrealMCPBlueprintNodeCommands` — blueprint authoring and graph nodes
  - `UnrealMCPUMGCommands` — widget blueprints
  - `UnrealMCPProjectCommands` — input mapping
  - `UnrealMCPGISCommands` — level/anchor management, OpenDRIVE/vector road networks, GIS viewer, markers
  - `UnrealMCPCommonUtils` — shared JSON helpers (`CreateErrorResponse`/`CreateSuccessResponse`, actor lookup, rotation extraction)

## Dispatch routing — important
Most commands are queued to the game thread and dispatched to the matching `FUnrealMCP*Commands` handler. **Three commands are special-cased directly in `UUnrealMCPBridge::ExecuteCommand`** because they need async / non-game-thread handling:
- **`take_screenshot`** — routed in `ExecuteCommand`, **not** in EditorCommands. `FUnrealMCPEditorCommands::HandleTakeScreenshot` is a deliberate stub that returns `"take_screenshot: internal routing error"` if reached. The real implementation fires `HighResShot` on the game thread and polls for the output file on the MCP thread, so the game thread keeps rendering. **If you add screenshot logic, edit `ExecuteCommand`, not the EditorCommands stub.**
- **`gis_import_landscape`** → `StartLandscapeImport` (async, promise/delegate)
- **`gis_import_vector_roads`** → `StartVectorRoadsImport` (async, promise/delegate)

### Async GIS pattern
Both GIS imports use a `TSharedPtr<TPromise<FString>>` (`PendingGISPromise` / `PendingVectorRoadsPromise`): the game thread kicks off a `UServeProcessRasterToLandscape` / `UServeProcessVectorShapes` UObject, `AddToRoot()`s it to survive GC, binds `OnSucceeded`/`OnFailed` dynamic delegates, and the MCP server thread blocks on the future. Only one GIS import runs at a time.

## Design principle: bridge parity with the UI
Bridge commands must produce the same result as the human-operated UI by calling the **same shared implementation**, not a re-implementation. The canonical example: `gis_import_vector_roads` runs the same post-spawn passes (chain-join, carriageway merge) as `SServeGISRoadsPanel` by calling `FServeGISRoadImportUtils` in the ServeGISTools plugin. Any new post-processing pass added to the GIS panel must also be wired into the bridge via the shared utility.

## Dependencies
`UnrealMCP.uplugin` depends on engine plugins `EditorScriptingUtilities` and `PythonScriptPlugin`, plus the local plugins **ServeGISTools** (GIS commands delegate to its utilities) and **RoadBLD** (road network runtime types). Changes to GIS road behavior usually belong in ServeGISTools, not here.

## Gotchas
- **`take_screenshot`** via `HighResShot` may not fire in all editor states. Confirmed-working fallback (see root `CLAUDE.md`): `execute_python` running `unreal.SystemLibrary.execute_console_command(world, "HighResShot 1")`, then read the file from `Saved/Screenshots/LinuxEditor/`.
- **`execute_python`** writes the script to `Saved/Temp/mcp_script.py` and runs it via `ExecuteFile` (avoids `ExecuteStatement` line-length limits). `unreal.log()` goes to the Output Log, **not** the TCP response — write results to a temp file and read it back if you need them.
- **Log categories:** `LogUnrealMCP` (bridge + commands), `LogMCPServer` (TCP thread).
- After a C++ rebuild the editor must be restarted for the new bridge to load.
