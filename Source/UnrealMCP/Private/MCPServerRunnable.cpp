#include "MCPServerRunnable.h"
#include "UnrealMCPBridge.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY_STATIC(LogMCPServer, Log, All);

static const int32 GRecvBufBytes = 65536; // 64 KB per client

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

FMCPServerRunnable::FMCPServerRunnable(UUnrealMCPBridge* InBridge,
                                       TSharedPtr<FSocket> InListenerSocket)
    : Bridge(InBridge)
    , ListenerSocket(InListenerSocket)
    , bRunning(true)
{
}

FMCPServerRunnable::~FMCPServerRunnable()
{
}

bool FMCPServerRunnable::Init()
{
    return true;
}

void FMCPServerRunnable::Stop()
{
    bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------

uint32 FMCPServerRunnable::Run()
{
    UE_LOG(LogMCPServer, Display, TEXT("MCPServer: accept loop started on port 55557"));

    while (bRunning)
    {
        bool bPending = false;
        if (ListenerSocket->HasPendingConnection(bPending) && bPending)
        {
            FSocket* RawClient = ListenerSocket->Accept(TEXT("MCPClient"));
            if (RawClient)
            {
                // Log the remote address so we can confirm connections in the output log.
                TSharedRef<FInternetAddr> PeerAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
                RawClient->GetPeerAddress(*PeerAddr);
                UE_LOG(LogMCPServer, Log, TEXT("MCPServer: accepted connection from %s"), *PeerAddr->ToString(true));

                // Wrap with a deleter that calls DestroySocket instead of delete,
                // so the fd is properly closed when the shared ptr drops to zero.
                TSharedPtr<FSocket> Client(RawClient, [](FSocket* S)
                {
                    if (S) { ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(S); }
                });

                Client->SetNoDelay(true);
                int32 ActualBufSize = GRecvBufBytes;
                Client->SetSendBufferSize(GRecvBufBytes, ActualBufSize);
                Client->SetReceiveBufferSize(GRecvBufBytes, ActualBufSize);

                // Hand off to a dedicated thread — accept loop stays free even
                // while a long Python script is blocking on the game thread.
                Async(EAsyncExecution::Thread,
                    [this, Client]() mutable { ServeClient(MoveTemp(Client)); });
            }
            else
            {
                UE_LOG(LogMCPServer, Warning, TEXT("MCPServer: Accept() returned null"));
            }
        }

        FPlatformProcess::Sleep(0.05f);
    }

    UE_LOG(LogMCPServer, Display, TEXT("MCPServer: accept loop stopped"));
    return 0;
}

// ---------------------------------------------------------------------------
// Per-client handler (runs on its own thread)
// ---------------------------------------------------------------------------

void FMCPServerRunnable::ServeClient(TSharedPtr<FSocket> Client)
{
    // Blocking mode: simplifies the recv loop — no EWOULDBLOCK spin needed.
    Client->SetNonBlocking(false);

    TArray<uint8> Buf;
    Buf.SetNumUninitialized(GRecvBufBytes);
    FString MsgBuf;

    while (bRunning)
    {
        int32 BytesRead = 0;
        if (!Client->Recv(Buf.GetData(), Buf.Num() - 1, BytesRead) || BytesRead == 0)
        {
            // Error, timeout, or graceful close.
            break;
        }

        Buf[BytesRead] = 0;
        MsgBuf += UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Buf.GetData()));

        // Dispatch every complete newline-terminated message, then close.
        // One request → one response → close: avoids idle CLOSE-WAIT sockets
        // when the client disconnects between requests.
        int32 NlPos;
        while (MsgBuf.FindChar(TEXT('\n'), NlPos))
        {
            FString Msg = MsgBuf.Left(NlPos).TrimEnd();
            MsgBuf.RightInline(MsgBuf.Len() - NlPos - 1);
            if (!Msg.IsEmpty())
            {
                ProcessMessage(Client, Msg);
                return; // One request per connection. Client reconnects for the next.
            }
        }
    }
    // Shared ptr drops here → custom deleter calls DestroySocket.
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void FMCPServerRunnable::ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message)
{
    TSharedPtr<FJsonObject> JsonMsg;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
    if (!FJsonSerializer::Deserialize(Reader, JsonMsg) || !JsonMsg.IsValid())
    {
        UE_LOG(LogMCPServer, Warning, TEXT("MCPServer: invalid JSON (%.200s)"), *Message);
        return;
    }

    FString CommandType;
    if (!JsonMsg->TryGetStringField(TEXT("type"), CommandType))
    {
        UE_LOG(LogMCPServer, Warning, TEXT("MCPServer: message missing 'type' field — raw: %.200s"), *Message);
        return;
    }

    UE_LOG(LogMCPServer, Log, TEXT("MCPServer: >> %s"), *CommandType);

    // Params are optional — use an empty object if absent.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject>* ParamsPtr;
    if (JsonMsg->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
    {
        Params = *ParamsPtr;
    }

    const FString Response = Bridge->ExecuteCommand(CommandType, Params);

    // Responses are newline-terminated so the client can delimit them.
    const FString WithNewline = Response + TEXT("\n");
    const FTCHARToUTF8 Utf8(*WithNewline);
    int32 BytesSent = 0;
    if (!Client->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), BytesSent))
    {
        UE_LOG(LogMCPServer, Warning, TEXT("MCPServer: failed to send response for '%s' (%d bytes)"), *CommandType, Utf8.Length());
    }
    else
    {
        UE_LOG(LogMCPServer, Log, TEXT("MCPServer: << %s (%d bytes)"), *CommandType, BytesSent);
    }
}
