#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

class UUnrealMCPBridge;

/**
 * FUnrealMCPCLIBridge — stdio transport for the MCP protocol.
 *
 * Activated by passing -MCPStdio on the editor command line, or by calling
 * Start() explicitly.  Runs a background thread that reads newline-delimited
 * JSON-RPC 2.0 messages from stdin and writes responses to stdout, translating
 * between the MCP wire format and the internal UnrealMCP command dispatcher.
 *
 * Wire format (stdin → bridge):
 *   {"jsonrpc":"2.0","id":1,"method":"tools/call",
 *    "params":{"name":"spawn_actor","arguments":{...}}}\n
 *
 * Wire format (bridge → stdout):
 *   {"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{...}"}]}}\n
 *
 * MCP lifecycle messages handled:
 *   initialize       — responds with server capabilities and tool list
 *   initialized      — notification (no response needed)
 *   tools/list       — returns full tool catalogue
 *   tools/call       — dispatches to Bridge->ExecuteCommand()
 *   ping             — responds with empty result
 */
class UNREALMCP_API FUnrealMCPCLIBridge : public FRunnable
{
public:
    explicit FUnrealMCPCLIBridge(UUnrealMCPBridge* InBridge);
    ~FUnrealMCPCLIBridge();

    // Start/stop the stdin reader thread.
    void Start();
    void Stop();
    bool IsRunning() const { return bRunning; }

    // FRunnable
    bool    Init()  override;
    uint32  Run()   override;
    void    Exit()  override;

private:
    // --- MCP message handlers ---
    FString HandleInitialize(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& Id);
    FString HandleToolsList(const TSharedPtr<FJsonValue>& Id);
    FString HandleToolsCall(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& Id);
    FString HandlePing(const TSharedPtr<FJsonValue>& Id);

    // --- JSON-RPC helpers ---
    FString MakeResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result);
    FString MakeTextResult(const TSharedPtr<FJsonValue>& Id, const FString& Text);
    FString MakeError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message);
    FString IdToString(const TSharedPtr<FJsonValue>& Id);

    // --- Tool catalogue ---
    TArray<TSharedPtr<FJsonValue>> BuildToolList();

    // --- I/O ---
    void WriteLine(const FString& Line);

    UUnrealMCPBridge* Bridge = nullptr;
    FRunnableThread*  Thread  = nullptr;
    TAtomic<bool>     bRunning { false };
};
