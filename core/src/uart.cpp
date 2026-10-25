#include "uart.h"
#include <algorithm>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
const size_t kRxDepth = 512, kTxDepth = 64, kRxNearFull = 384;
const uint64_t kPollNs = 100000; // host pipe is polled at most every 100 us of emulated time

struct LoopbackBackend final : UartBackend
{
	std::deque<uint8_t> m_Queue;
	int Read(uint8_t* Out, int Max) override
	{
		int N = 0;
		while (N < Max && !m_Queue.empty())
		{
			Out[N++] = m_Queue.front();
			m_Queue.pop_front();
		}
		return N;
	}
	bool Write(const uint8_t* Data, int Count) override
	{
		m_Queue.insert(m_Queue.end(), Data, Data + Count);
		return true;
	}
};

#ifdef _WIN32
struct ProcessBackend final : UartBackend
{
	HANDLE m_Process = nullptr, m_ToChild = nullptr, m_FromChild = nullptr;
	~ProcessBackend() override
	{
		if (m_ToChild) CloseHandle(m_ToChild);
		if (m_FromChild) CloseHandle(m_FromChild);
		if (m_Process)
		{
			if (WaitForSingleObject(m_Process, 500) == WAIT_TIMEOUT) TerminateProcess(m_Process, 1);
			CloseHandle(m_Process);
		}
	}
	bool Start(const std::string& Command, std::string& OutErr)
	{
		SECURITY_ATTRIBUTES Sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
		HANDLE ChildIn = nullptr, ChildOut = nullptr;
		if (!CreatePipe(&ChildIn, &m_ToChild, &Sa, 0) || !CreatePipe(&m_FromChild, &ChildOut, &Sa, 0))
		{
			OutErr = "CreatePipe failed";
			return false;
		}
		SetHandleInformation(m_ToChild, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(m_FromChild, HANDLE_FLAG_INHERIT, 0);
		STARTUPINFOA Si{};
		Si.cb = sizeof(Si);
		Si.dwFlags = STARTF_USESTDHANDLES;
		Si.hStdInput = ChildIn;
		Si.hStdOutput = ChildOut;
		Si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION Pi{};
		std::string Line = "cmd.exe /c " + Command;
		std::vector<char> Buf(Line.begin(), Line.end());
		Buf.push_back(0);
		const BOOL bOk = CreateProcessA(nullptr, Buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &Si, &Pi);
		CloseHandle(ChildIn);
		CloseHandle(ChildOut);
		if (!bOk)
		{
			OutErr = "CreateProcess failed for: " + Command;
			return false;
		}
		CloseHandle(Pi.hThread);
		m_Process = Pi.hProcess;
		return true;
	}
	int Read(uint8_t* Out, int Max) override
	{
		DWORD Avail = 0, Got = 0;
		if (!PeekNamedPipe(m_FromChild, nullptr, 0, nullptr, &Avail, nullptr)) return -1;
		if (Avail == 0) return 0;
		if (!ReadFile(m_FromChild, Out, DWORD(std::min<DWORD>(Avail, DWORD(Max))), &Got, nullptr)) return -1;
		return int(Got);
	}
	bool Write(const uint8_t* Data, int Count) override
	{
		DWORD Put = 0;
		return WriteFile(m_ToChild, Data, DWORD(Count), &Put, nullptr) && int(Put) == Count;
	}
	bool IsAlive() const override { return m_Process && WaitForSingleObject(m_Process, 0) == WAIT_TIMEOUT; }
};
#elif !defined(__EMSCRIPTEN__)
struct ProcessBackend final : UartBackend
{
	pid_t m_Pid = -1;
	int m_ToChild = -1, m_FromChild = -1;
	bool m_bClosed = false;
	~ProcessBackend() override
	{
		if (m_ToChild >= 0) close(m_ToChild);
		if (m_FromChild >= 0) close(m_FromChild);
		if (m_Pid > 0)
		{
			int Status = 0;
			for (int I = 0; I < 50 && waitpid(m_Pid, &Status, WNOHANG) == 0; ++I) usleep(10000);
			if (waitpid(m_Pid, &Status, WNOHANG) == 0)
			{
				kill(m_Pid, SIGTERM);
				waitpid(m_Pid, &Status, 0);
			}
		}
	}
	bool Start(const std::string& Command, std::string& OutErr)
	{
		int In[2], Out[2];
		if (pipe(In) != 0 || pipe(Out) != 0)
		{
			OutErr = "pipe failed";
			return false;
		}
		signal(SIGPIPE, SIG_IGN); // a dead child makes Write fail instead of killing the emulator
		m_Pid = fork();
		if (m_Pid < 0)
		{
			OutErr = "fork failed";
			return false;
		}
		if (m_Pid == 0)
		{
			dup2(In[0], 0);
			dup2(Out[1], 1);
			close(In[0]); close(In[1]); close(Out[0]); close(Out[1]);
			execl("/bin/sh", "sh", "-c", Command.c_str(), (char*)nullptr);
			_exit(127);
		}
		close(In[0]);
		close(Out[1]);
		m_ToChild = In[1];
		m_FromChild = Out[0];
		fcntl(m_FromChild, F_SETFL, fcntl(m_FromChild, F_GETFL) | O_NONBLOCK);
		return true;
	}
	int Read(uint8_t* Out, int Max) override
	{
		if (m_bClosed) return -1;
		const ssize_t N = read(m_FromChild, Out, size_t(Max));
		if (N > 0) return int(N);
		if (N == 0) { m_bClosed = true; return -1; }
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
	}
	bool Write(const uint8_t* Data, int Count) override
	{
		while (Count > 0)
		{
			const ssize_t N = write(m_ToChild, Data, size_t(Count));
			if (N < 0 && errno == EINTR) continue;
			if (N <= 0) return false;
			Data += N;
			Count -= int(N);
		}
		return true;
	}
	bool IsAlive() const override
	{
		int Status = 0;
		return m_Pid > 0 && !m_bClosed && waitpid(m_Pid, &Status, WNOHANG) == 0;
	}
};
#endif
} // namespace

std::unique_ptr<UartBackend> MakeLoopbackUartBackend()
{
	return std::make_unique<LoopbackBackend>();
}
std::unique_ptr<UartBackend> MakeProcessUartBackend(const std::string& Command, std::string& OutErr)
{
#ifdef __EMSCRIPTEN__
	(void)Command;
	OutErr = "child processes are not available in the browser build";
	return nullptr;
#else
	auto P = std::make_unique<ProcessBackend>();
	if (!P->Start(Command, OutErr)) return nullptr;
	return P;
#endif
}
std::unique_ptr<UartBackend> MakeUartBackend(const std::string& Spec, std::string& OutErr)
{
	if (Spec.empty()) return nullptr;
	if (Spec == "loop") return MakeLoopbackUartBackend();
	return MakeProcessUartBackend(Spec, OutErr);
}

uint64_t NextUartChannel::BitNs() const
{
	const uint64_t Ns = uint64_t(double(m_Prescaler ? m_Prescaler : 1) * (1e9 / 28000000.0) + 0.5);
	return Ns ? Ns : 1;
}
uint64_t NextUartChannel::ByteNs() const
{
	const int DataBits = 5 + ((m_Framing >> 3) & 3), Parity = (m_Framing >> 2) & 1, Stop = 1 + (m_Framing & 1);
	return BitNs() * uint64_t(1 + DataBits + Parity + Stop);
}

void NextUart::Reset(bool bHard)
{
	m_bSelectPi = false;
	for (NextUartChannel& C : m_Ch)
	{
		C.m_Rx.clear(); C.m_Tx.clear(); C.m_Pending.clear();
		C.m_bTxInFlight = false; C.m_bOverflow = C.m_bFramingError = false;
		if (bHard)
		{
			C.m_Prescaler = 243;
			C.m_Framing = 0x18;
		}
	}
}

static void AdvanceChannel(NextUart& U, int Ch, uint64_t NowNs, bool bLinked)
{
	NextUartChannel& C = U.m_Ch[Ch];
	if (C.InReset())
	{
		C.m_Rx.clear(); C.m_Tx.clear(); C.m_bTxInFlight = false;
		C.m_bOverflow = C.m_bFramingError = false;
	}
	const uint64_t ByteNs = C.ByteNs();
	// transmitter: the in-flight byte reaches the wire when its stop bit ends
	while (C.m_bTxInFlight && C.m_TxDoneNs <= NowNs)
	{
		if (bLinked && C.m_Backend)
		{
			if (C.m_Backend->Write(&C.m_TxByte, 1)) ++C.m_WireBytesOut;
			else ++C.m_Dropped;
		}
		else ++C.m_Dropped;
		if (C.m_Tx.empty()) C.m_bTxInFlight = false;
		else
		{
			C.m_TxByte = C.m_Tx.front();
			C.m_Tx.pop_front();
			C.m_TxDoneNs += ByteNs;
		}
	}
	// receiver: poll the host, then let bytes arrive one byte time apart
	if (bLinked && C.m_Backend && (C.m_Pending.empty() || NowNs >= C.m_LastPollNs + kPollNs))
	{
		uint8_t Buf[4096];
		const int N = C.m_Backend->Read(Buf, int(sizeof Buf));
		C.m_LastPollNs = NowNs;
		if (N > 0)
		{
			if (C.m_Pending.empty() && C.m_RxNextNs < NowNs) C.m_RxNextNs = NowNs + ByteNs; // idle line: first start bit now
			C.m_Pending.insert(C.m_Pending.end(), Buf, Buf + N);
		}
	}
	while (!C.m_Pending.empty() && C.m_RxNextNs <= NowNs)
	{
		const uint8_t B = C.m_Pending.front();
		C.m_Pending.pop_front();
		++C.m_WireBytesIn;
		if (C.InReset()) {}
		else if (C.m_Rx.size() >= kRxDepth) { C.m_bOverflow = true; ++C.m_Dropped; if (U.m_OnOverflow) U.m_OnOverflow(U.m_OnOverflowUser, Ch); }
		else C.m_Rx.push_back(B);
		C.m_RxNextNs += ByteNs;
	}
}

bool NextUart::RxIntRequest(int Ch, bool bNearFullMode) const
{
	const NextUartChannel& C = m_Ch[Ch & 1];
	return bNearFullMode ? C.m_Rx.size() >= kRxNearFull : !C.m_Rx.empty();
}
void NextUart::Advance(uint64_t NowNs, bool bPiLinked)
{
	AdvanceChannel(*this, kEsp, NowNs, true);
	AdvanceChannel(*this, kPi, NowNs, bPiLinked);
}
void NextUart::Flush(bool bPiLinked)
{
	for (int I = 0; I < 2; ++I)
	{
		NextUartChannel& C = m_Ch[I];
		const bool bLinked = I == kEsp || bPiLinked;
		std::vector<uint8_t> Out;
		if (C.m_bTxInFlight) Out.push_back(C.m_TxByte);
		Out.insert(Out.end(), C.m_Tx.begin(), C.m_Tx.end());
		C.m_Tx.clear(); C.m_bTxInFlight = false;
		if (!Out.empty() && bLinked && C.m_Backend && C.m_Backend->Write(Out.data(), int(Out.size()))) C.m_WireBytesOut += Out.size();
		while (!C.m_Pending.empty())
		{
			if (C.m_Rx.size() < kRxDepth) C.m_Rx.push_back(C.m_Pending.front());
			else C.m_bOverflow = true;
			C.m_Pending.pop_front();
			++C.m_WireBytesIn;
		}
	}
}

uint8_t NextUart::ReadPort(uint16_t Port, uint64_t NowNs, bool bPiLinked)
{
	Advance(NowNs, bPiLinked);
	NextUartChannel& C = m_Ch[m_bSelectPi ? kPi : kEsp];
	switch ((Port >> 8) & 3)
	{
	case 0: // $143B: Rx FIFO
	{
		if (C.m_Rx.empty()) return 0;
		const uint8_t B = C.m_Rx.front();
		C.m_Rx.pop_front();
		return B;
	}
	case 1: // $153B: select + prescaler msb
		return uint8_t((m_bSelectPi ? 0x40 : 0x00) | ((C.m_Prescaler >> 14) & 7));
	case 2: // $163B: framing
		return C.m_Framing;
	default: // $133B: status
	{
		const uint8_t S = uint8_t((C.m_bFramingError ? 0x40 : 0) | ((C.m_Tx.empty() && !C.m_bTxInFlight) ? 0x10 : 0) |
			(C.m_Rx.size() >= kRxNearFull ? 0x08 : 0) | (C.m_bOverflow ? 0x04 : 0) |
			(C.m_Tx.size() >= kTxDepth ? 0x02 : 0) | (!C.m_Rx.empty() ? 0x01 : 0));
		C.m_bOverflow = C.m_bFramingError = false; // cleared by the status read
		return S;
	}
	}
}

void NextUart::WritePort(uint16_t Port, uint8_t Value, uint64_t NowNs, bool bPiLinked)
{
	Advance(NowNs, bPiLinked);
	NextUartChannel& C = m_Ch[m_bSelectPi ? kPi : kEsp];
	switch ((Port >> 8) & 3)
	{
	case 0: // $143B: prescaler low 14 bits, seven at a time
		if (Value & 0x80) C.m_Prescaler = (C.m_Prescaler & ~0x3F80u) | (uint32_t(Value & 0x7F) << 7);
		else C.m_Prescaler = (C.m_Prescaler & ~0x7Fu) | (Value & 0x7F);
		break;
	case 1: // $153B: select, optionally prescaler bits 16:14 of the newly selected channel
		m_bSelectPi = (Value & 0x40) != 0;
		if (Value & 0x10)
		{
			NextUartChannel& Sel = m_Ch[m_bSelectPi ? kPi : kEsp];
			Sel.m_Prescaler = (Sel.m_Prescaler & 0x3FFF) | (uint32_t(Value & 7) << 14);
		}
		break;
	case 2: // $163B: framing (bit 7 holds the FIFOs in reset until cleared)
		C.m_Framing = Value;
		if (C.InReset()) AdvanceChannel(*this, m_bSelectPi ? kPi : kEsp, NowNs, bPiLinked);
		break;
	default: // $133B: Tx FIFO push; a full FIFO drops the byte
		if (C.InReset()) break;
		if (!C.m_bTxInFlight)
		{
			C.m_bTxInFlight = true;
			C.m_TxByte = Value;
			C.m_TxDoneNs = NowNs + C.ByteNs();
		}
		else if (C.m_Tx.size() < kTxDepth) C.m_Tx.push_back(Value);
		else ++C.m_Dropped;
		break;
	}
}
