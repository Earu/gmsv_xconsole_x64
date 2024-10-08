#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <cstring>
#include <cstdint>
#include <string>
#include <thread>
#include <chrono>

#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <ByteBuffer.hpp>
#include <Platform.hpp>
#include <color.h>
#include <eiface.h>
#include <tier0/dbg.h>

#if ARCHITECTURE_IS_X86_64
#include <logging.h>
#endif

const int BUFFER_SIZE = 8192;

#ifdef _WIN32
static HANDLE serverPipe = INVALID_HANDLE_VALUE;
#else
const char* EOL_SEQUENCE = "<EOL>\0";
const char* PIPE_NAME_OUT = "/tmp/garrysmod_console";
const char* PIPE_NAME_IN = "/tmp/garrysmod_console_in";

static int serverPipe = -1;
static int serverPipeIn = -1;
#endif

static volatile bool serverShutdown = false;
static volatile bool serverConnected = false;
static std::thread serverThread;

#if ARCHITECTURE_IS_X86_64
class XConsoleListener : public ILoggingListener
{
public:
	XConsoleListener(bool bQuietPrintf = false, bool bQuietDebugger = false) {}

	void Log(const LoggingContext_t* pContext, const char* pMessage) override
	{
		const CLoggingSystem::LoggingChannel_t* chan = LoggingSystem_GetChannel(pContext->m_ChannelID);
		const Color* color = &pContext->m_Color;

		MultiLibrary::ByteBuffer buffer;
		buffer.Reserve(BUFFER_SIZE);

#ifdef _WIN32
		buffer <<
			static_cast<int32_t>(chan->m_ID) <<
			pContext->m_Severity <<
			chan->m_Name <<
			color->GetRawColor() <<
			pMessage;

		if (WriteFile(serverPipe, buffer.GetBuffer(), static_cast<DWORD>(buffer.Size()), nullptr, nullptr) == FALSE)
			serverConnected = false;
#else
		buffer <<
			static_cast<int32_t>(chan->m_ID) <<
			pContext->m_Severity <<
			chan->m_Name <<
			color->GetRawColor() <<
			pMessage <<
			"<EOL>";

		if (serverPipe == -1)
		{
			serverConnected = false;
			return;
		}

		if (write(serverPipe, buffer.GetBuffer(), buffer.Size()) == -1)
			serverConnected = false;
#endif
	}
};

ILoggingListener* listener = new XConsoleListener();
#else
static SpewOutputFunc_t spewFunction = nullptr;
static SpewRetval_t EngineSpewReceiver(SpewType_t type, const char* msg)
{
	if (!serverConnected)
		return spewFunction(type, msg);

	const Color* color = GetSpewOutputColor();
	MultiLibrary::ByteBuffer buffer;
	buffer.Reserve(512);

#ifdef _WIN32
	buffer <<
		static_cast<int32_t>(type) <<
		GetSpewOutputLevel() <<
		GetSpewOutputGroup() <<
		color->GetRawColor() <<
		msg;

	if (WriteFile(serverPipe, buffer.GetBuffer(), static_cast<DWORD>(buffer.Size()), nullptr, nullptr) == FALSE)
		serverConnected = false;
#else
	buffer <<
		static_cast<int32_t>(type) <<
		GetSpewOutputLevel() <<
		GetSpewOutputGroup() <<
		color->GetRawColor() <<
		msg <<
		"<EOL>";

	if (serverPipe == -1)
	{
		serverConnected = false;
		return spewFunction(type, msg);
	}

	if (write(serverPipe, buffer.GetBuffer(), buffer.Size()) == -1)
		serverConnected = false;
#endif

	return spewFunction(type, msg);
}
#endif

static void RunCommand(std::string cmd)
{
	// in case the command hasnt been passed with a newline
	if (cmd[cmd.length() - 1] != '\n')
		cmd.append("\n");

	SourceSDK::FactoryLoader engine_loader("engine");
	IVEngineServer* engine_server = engine_loader.GetInterface<IVEngineServer>(INTERFACEVERSION_VENGINESERVER);
	engine_server->ServerCommand(cmd.c_str());
}

#ifdef _WIN32
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
		RunCommand(cmd);
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

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
#else
static void ServerThread()
{
	std::vector<uint8_t> dataBuffer(BUFFER_SIZE);
	std::vector<uint8_t> buffer(BUFFER_SIZE);

	int eolIndex = 0;
	while (!serverShutdown && serverPipeIn != -1)
	{
		int bytesRead = read(serverPipeIn, buffer.data(), buffer.size() - 1);
		if (bytesRead > 0)
		{
			for (int i = 0; i < bytesRead; i++)
			{
				uint8_t currentByte = buffer[i];
				dataBuffer.push_back(currentByte);

				// Check if the current byte matches the next byte in the EOL sequence
				if (currentByte == EOL_SEQUENCE[eolIndex])
				{
					eolIndex++;
					if (eolIndex == std::strlen(EOL_SEQUENCE)) // Full EOL found
					{
						dataBuffer.erase(dataBuffer.end() - eolIndex, dataBuffer.end());

						std::string cmd(dataBuffer.begin(), dataBuffer.end());
						RunCommand(cmd);

						dataBuffer.clear();
						eolIndex = 0;
					}
				}
				else
				{
					eolIndex = 0;
				}
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

static int CreateNamedPipe(GarrysMod::Lua::ILuaBase *LUA, const char* pipeName)
{
	struct stat sb;
	if (stat(pipeName, &sb) == 0 && !(sb.st_mode & S_IFDIR))
	{
		unlink(pipeName);
	}

   	if (mkfifo(pipeName, 0666) == -1)
	{
		LUA->ThrowError( "failed to create named pipe (mkfifo)" );
		return -1;
	}
	else
	{
		int pipe = open(pipeName, O_RDWR | O_NONBLOCK);
		if (pipe == -1)
		{
			LUA->ThrowError( "failed to create named pipe (open)" );
		}

		serverConnected = pipe != -1;
		return pipe;
	}
}
#endif

GMOD_MODULE_OPEN()
{
#ifdef _WIN32
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
		BUFFER_SIZE,
		BUFFER_SIZE,
		NMPWAIT_USE_DEFAULT_WAIT,
		&sa
	);

	if (serverPipe == INVALID_HANDLE_VALUE)
		LUA->ThrowError( "failed to create named pipe" );
#else
	serverPipe = CreateNamedPipe(LUA, PIPE_NAME_OUT);
	serverPipeIn = CreateNamedPipe(LUA, PIPE_NAME_IN);
#endif

	serverThread = std::thread(ServerThread);

#if ARCHITECTURE_IS_X86_64
	LoggingSystem_PushLoggingState(false, false);
	LoggingSystem_RegisterLoggingListener(listener);
#else
	spewFunction = GetSpewOutputFunc();
	SpewOutputFunc(EngineSpewReceiver);
#endif

	return 0;
}

GMOD_MODULE_CLOSE()
{
#if ARCHITECTURE_IS_X86_64
	LoggingSystem_UnregisterLoggingListener(listener);
	LoggingSystem_PopLoggingState(false);
	delete listener;
#else
	SpewOutputFunc(spewFunction);
#endif

	serverShutdown = true;
	serverThread.join();

#ifdef _WIN32
	FlushFileBuffers(serverPipe);
	DisconnectNamedPipe(serverPipe);
	CloseHandle(serverPipe);
#else
	close(serverPipe);
	unlink(PIPE_NAME_OUT);

	close(serverPipeIn);
	unlink(PIPE_NAME_IN);
#endif

	return 0;
}
