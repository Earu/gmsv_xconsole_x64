#include <GarrysMod/Lua/Interface.h>
#include <ByteBuffer.hpp>
#include <dbg.h>
#include <Color.h>
#include <Windows.h>
#include <cstdint>
#include <string>
#include <thread>
#include <logging.h>

static HANDLE server_pipe = INVALID_HANDLE_VALUE;
static volatile bool server_shutdown = false;
static volatile bool server_connected = false;
static std::thread server_thread;

class XConsoleListener : public ILoggingListener
{
public:
	XConsoleListener(bool bQuietPrintf = false, bool bQuietDebugger = false) {}

	virtual void Log(const LoggingContext_t* pContext, const tchar* pMessage)
	{
		if (pContext->m_Flags == LoggingChannelFlags_t::LCF_DO_NOT_ECHO) return;

		const CLoggingSystem::LoggingChannel_t* chan = LoggingSystem_GetChannel(pContext->m_ChannelID);
		const Color* color = &pContext->m_Color;
		MultiLibrary::ByteBuffer buffer;
		buffer.Reserve(512);
		buffer <<
			static_cast<int32_t>(chan->m_ID) <<
			pContext->m_Severity <<
			chan->m_Name <<
			color->GetRawColor() <<
			pMessage;

		

		if (WriteFile(server_pipe, buffer.GetBuffer(), static_cast<DWORD>(buffer.Size()), nullptr, nullptr) == FALSE)
			server_connected = false;
	}
};


static void ServerThread()
{
	while(!server_shutdown)
	{
		if( ConnectNamedPipe(server_pipe, nullptr) == FALSE)
		{
			DWORD error = GetLastError();
			if (error == ERROR_NO_DATA)
			{
				DisconnectNamedPipe(server_pipe);
				server_connected = false;
			}
			else if (error == ERROR_PIPE_CONNECTED)
				server_connected = true;
		}
		else
			server_connected = true;

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

	server_pipe = CreateNamedPipe(
		"\\\\.\\pipe\\garrysmod_console",
		PIPE_ACCESS_OUTBOUND,
		PIPE_TYPE_MESSAGE | PIPE_NOWAIT,
		PIPE_UNLIMITED_INSTANCES,
		8192,
		8192,
		NMPWAIT_USE_DEFAULT_WAIT,
		&sa
	);

	if(server_pipe == INVALID_HANDLE_VALUE)
		LUA->ThrowError( "failed to create named pipe" );

	server_thread = std::thread(ServerThread);

	LoggingSystem_RegisterLoggingListener(listener);

	return 0;
}

GMOD_MODULE_CLOSE()
{
	LoggingSystem_UnregisterLoggingListener(listener);
	delete listener;

	server_shutdown = true;
	server_thread.join();

	FlushFileBuffers(server_pipe);
	DisconnectNamedPipe(server_pipe);
	CloseHandle(server_pipe);

	return 0;
}