#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Sockets.h"

class UUnrealMCPBridge;

/**
 * Accept loop for the MCP TCP bridge.
 *
 * Run() accepts incoming connections and immediately offloads each one to a
 * dedicated thread (Async/Thread) so the accept loop is never blocked by a
 * long-running command (e.g. a Python script that saves many assets).
 * Multiple clients can therefore connect and queue commands concurrently;
 * they are serialised only at the game-thread execution step.
 */
class FMCPServerRunnable : public FRunnable
{
public:
	FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket);
	virtual ~FMCPServerRunnable();

	// FRunnable interface
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	// Serves one accepted client connection on the calling thread.
	// Owns the socket lifetime via the shared ptr; exits when the client
	// disconnects or bRunning becomes false.
	void ServeClient(TSharedPtr<FSocket> Client);

	// Parses and dispatches a single complete (newline-stripped) JSON message.
	void ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message);

	UUnrealMCPBridge* Bridge;
	TSharedPtr<FSocket> ListenerSocket;
	bool bRunning;
};
