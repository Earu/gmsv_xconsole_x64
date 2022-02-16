#include <GarrysMod/Lua/Interface.h>
#include <ByteBuffer.hpp>
#include <dbg.h>
#include <Color.h>
#include <Windows.h>
#include <cstdint>
#include <string>
#include <thread>
#include <logging.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <eiface.h>

static HANDLE serverPipe = INVALID_HANDLE_VALUE;
static volatile bool serverShutdown = false;
static volatile bool serverConnected = false;
static std::thread serverThread;

class XConsoleListener : public ILoggingListener
{
public:
	XConsoleListener(bool bQuietPrintf = false, bool bQuietDebugger = false) {}

	void Log(const LoggingContext_t* pContext, const char* pMessage) override
	{
		const CLoggingSystem::LoggingChannel_t* chan = LoggingSystem_GetChannel(pContext->m_ChannelID);
		const Color* color = &pContext->m_Color;
		MultiLibrary::ByteBuffer buffer;
		buffer.Reserve(8192);
		buffer <<
			static_cast<int32_t>(chan->m_ID) <<
			pContext->m_Severity <<
			chan->m_Name <<
			color->GetRawColor() <<
			pMessage;

		if (WriteFile(serverPipe, buffer.GetBuffer(), static_cast<DWORD>(buffer.Size()), nullptr, nullptr) == FALSE)
			serverConnected = false;
	}
};

static void ReadIncomingCommands() 
{
	MultiLibrary::ByteBuffer buffer;
	buffer.Reserve(255);
	buffer.Resize(255);
	if (ReadFile(serverPipe, buffer.GetBuffer(), static_cast<DWORD>(buffer.Size()), nullptr, nullptr) == TRUE)
	{
		if (buffer.Size() == 0) return;

		std::string cmd;
		buffer >> cmd;

		// in case the command hasnt been passed with a newline
		if (cmd[cmd.length() - 1] != '\n')
			cmd.append("\n");

		SourceSDK::FactoryLoader engine_loader("engine");
		IVEngineServer* engine_server = engine_loader.GetInterface<IVEngineServer>(INTERFACEVERSION_VENGINESERVER);
		engine_server->ServerCommand(cmd.c_str());
	}
}


static void ServerThread()
{
	while (!serverShutdown)
	{
		if (ConnectNamedPipe(serverPipe, nullptr) == FALSE)
		{
			DWORD error = GetLastError();
			if (error == ERROR_NO_DATA)
			{
				DisconnectNamedPipe(serverPipe);
				serverConnected = false;
			}
			else if (error == ERROR_PIPE_CONNECTED) {
				serverConnected = true;
				ReadIncomingCommands();
			}
		}
		else
		{
			serverConnected = true;
			ReadIncomingCommands();
		}

		Sleep(1);
	}
}

ILoggingListener* listener = new XConsoleListener();
GMOD_MODULE_OPEN()
{
	SECURITY_DESCRIPTOR sd;
	InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = &sd;
	sa.bInheritHandle = FALSE;

	serverPipe = CreateNamedPipe(
		"\\\\.\\pipe\\garrysmod_console",
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_MESSAGE | PIPE_NOWAIT,
		PIPE_UNLIMITED_INSTANCES,
		8192,
		8192,
		NMPWAIT_USE_DEFAULT_WAIT,
		&sa
	);

	if (serverPipe == INVALID_HANDLE_VALUE)
		LUA->ThrowError( "failed to create named pipe" );

	serverThread = std::thread(ServerThread);

	LoggingSystem_PushLoggingState(false, false);
	LoggingSystem_RegisterLoggingListener(listener);

	return 0;
}

GMOD_MODULE_CLOSE()
{
	LoggingSystem_UnregisterLoggingListener(listener);
	LoggingSystem_PopLoggingState(false);
	delete listener;

	serverShutdown = true;
	serverThread.join();

	FlushFileBuffers(serverPipe);
	DisconnectNamedPipe(serverPipe);
	CloseHandle(serverPipe);

	return 0;
}