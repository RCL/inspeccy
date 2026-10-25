// nextpi-sup: the NextPi supervisor as seen from the Next's UART, for running
// a Next program against a native stand-in of its Pi-side binary.
//
// stdin/stdout are the wire (inspeccy --uart-pi "nextpi-sup ..."). The real
// supervisor is a root bash on a serial console: it echoes what it is sent,
// answers an Enter on an empty line with "SUP> ", runs nextpi-* commands and
// anything else it finds, and hands the console to a foreground program until
// that exits. The behaviour modelled here comes from the public NextPi
// documents (docs/rpi/linux/Supervisor.txt) and from what the Next-side
// PiTools client (PIPUT/PIVER) waits for; the Pi-side scripts themselves are
// not public, so every reply they give is a guess marked in the log. That is
// the point: the same Next program run on real hardware writes the same
// SD-card log, and the two logs show where the guess was wrong.
//
//   nextpi-sup --root DIR [--native "/ram/prog=HOST_PATH [extra args]"]... [--log FILE]
//              [--version 2.01a] [--receive-dir ram] [--cooked]
//
// The sandbox DIR stands in for the Pi filesystem: /ram, /PCache and /NextPi
// are directories under it. A received file lands in DIR/<receive-dir>/. A
// command whose program is a --native guest path runs HOST_PATH instead (the
// ARMv6 binary cannot run here), with any extra words of the mapping inserted
// before the guest's own arguments; the bytes it reads and writes are the wire.
// Programs run raw (no echo, no CR translation, Ctrl-C is data), as a program
// that sets its tty raw at once would see; --cooked keeps echo on and lets
// Ctrl-C kill the program instead.
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#ifdef _WIN32
int main() { fputs("nextpi-sup: POSIX only\n", stderr); return 2; }
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

namespace
{
struct Options
{
	std::string m_Root, m_LogPath, m_Version = "2.01a", m_ReceiveDir = "ram";
	std::vector<std::pair<std::string, std::string>> m_Native;
	bool m_bCooked = false;
	bool m_bNoHead = false; // pretend coreutils' head is missing (exercises the client's NBN fallback)
	bool m_bRedraw = false; // readline's habit on a real console: "SUP> <command>" again before the output
};

struct Log
{
	FILE* m_File = nullptr;
	std::chrono::steady_clock::time_point m_Start = std::chrono::steady_clock::now();
	double Now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_Start).count(); }
	void Event(const char* Fmt, ...)
	{
		if (!m_File) return;
		va_list Ap;
		va_start(Ap, Fmt);
		fprintf(m_File, "%10.6f ev ", Now());
		vfprintf(m_File, Fmt, Ap);
		fputc('\n', m_File);
		va_end(Ap);
		fflush(m_File);
	}
	void Bytes(const char* Dir, const uint8_t* Data, size_t Count)
	{
		if (!m_File) return;
		for (size_t Off = 0; Off < Count; Off += 32)
		{
			const size_t N = std::min<size_t>(32, Count - Off);
			fprintf(m_File, "%10.6f %s ", Now(), Dir);
			for (size_t I = 0; I < N; ++I) fprintf(m_File, "%02x", Data[Off + I]);
			fputs("  |", m_File);
			for (size_t I = 0; I < N; ++I) fputc(Data[Off + I] >= 32 && Data[Off + I] < 127 ? Data[Off + I] : '.', m_File);
			fputs("|\n", m_File);
		}
		fflush(m_File);
	}
};

struct Supervisor
{
	Options m_Opt;
	Log m_Log;
	std::string m_Line;
	bool m_bEcho = true;
	uint64_t m_BytesIn = 0, m_BytesOut = 0;
	std::vector<uint8_t> m_Unread; // wire bytes read ahead of a synchronous protocol step

	void Send(const void* Data, size_t Count)
	{
		const uint8_t* P = static_cast<const uint8_t*>(Data);
		m_Log.Bytes("tx", P, Count);
		m_BytesOut += Count;
		while (Count)
		{
			const ssize_t N = write(1, P, Count);
			if (N < 0 && errno == EINTR) continue;
			if (N <= 0) exit(0); // the emulator went away
			P += N;
			Count -= size_t(N);
		}
	}
	void SendText(const std::string& S) { Send(S.data(), S.size()); }
	void Prompt() { SendText("SUP> "); }

	// one byte from the wire, or -1 after `Seconds` without one
	int ReadByte(double Seconds)
	{
		if (!m_Unread.empty())
		{
			const int B = m_Unread.front();
			m_Unread.erase(m_Unread.begin());
			return B;
		}
		fd_set Set;
		timeval Tv{long(Seconds), long((Seconds - long(Seconds)) * 1e6)};
		FD_ZERO(&Set);
		FD_SET(0, &Set);
		const int R = select(1, &Set, nullptr, nullptr, &Tv);
		if (R <= 0) return -1;
		uint8_t Buf[4096];
		const ssize_t N = read(0, Buf, sizeof Buf);
		if (N <= 0) exit(0);
		m_Log.Bytes("rx", Buf, size_t(N));
		m_BytesIn += size_t(N);
		m_Unread.assign(Buf + 1, Buf + N);
		return Buf[0];
	}
	bool ReadExact(uint8_t* Out, size_t Count, double Seconds)
	{
		for (size_t I = 0; I < Count; ++I)
		{
			const int B = ReadByte(Seconds);
			if (B < 0) return false;
			Out[I] = uint8_t(B);
		}
		return true;
	}

	std::string GuestToHost(const std::string& Guest) const
	{
		std::string Path = Guest.empty() || Guest[0] != '/' ? "/" + Guest : Guest;
		// no escaping the sandbox
		std::vector<std::string> Parts;
		size_t Pos = 0;
		while (Pos <= Path.size())
		{
			const size_t Next = Path.find('/', Pos);
			const std::string Part = Path.substr(Pos, Next == std::string::npos ? std::string::npos : Next - Pos);
			if (Part == "..") { if (!Parts.empty()) Parts.pop_back(); }
			else if (!Part.empty() && Part != ".") Parts.push_back(Part);
			if (Next == std::string::npos) break;
			Pos = Next + 1;
		}
		std::string Host = m_Opt.m_Root;
		for (const std::string& P : Parts) Host += "/" + P;
		return Host;
	}
	static bool Exists(const std::string& Host, int Kind) // 0 any, 1 file, 2 dir, 3 executable
	{
		struct stat St;
		if (stat(Host.c_str(), &St) != 0) return false;
		if (Kind == 1) return S_ISREG(St.st_mode);
		if (Kind == 2) return S_ISDIR(St.st_mode);
		if (Kind == 3) return access(Host.c_str(), X_OK) == 0;
		return true;
	}

	static std::vector<std::string> Tokens(const std::string& S)
	{
		std::vector<std::string> Out;
		std::string Cur;
		bool bIn = false, bQuote = false;
		char Q = 0;
		for (char C : S)
		{
			if (bQuote) { if (C == Q) bQuote = false; else Cur += C; continue; }
			if (C == '"' || C == '\'') { bQuote = true; Q = C; bIn = true; continue; }
			if (C == ' ' || C == '\t') { if (bIn) { Out.push_back(Cur); Cur.clear(); bIn = false; } continue; }
			Cur += C;
			bIn = true;
		}
		if (bIn) Out.push_back(Cur);
		return Out;
	}

	// ---- the NBN receive the PIPUT client speaks (Pi side reconstructed from the client) ----
	bool FileReceive(const std::vector<std::string>& Args)
	{
		m_Log.Event("nextpi-file_receive %s: replies are reconstructed from the PIPUT client, not the Pi script",
			Args.size() > 1 ? Args[1].c_str() : "");
		SendText("OK\r\n");
		uint8_t Header[10];
		if (!ReadExact(Header, sizeof Header, 60)) { m_Log.Event("receive: header timeout"); SendText("ERROR\r\n"); return false; }
		const uint32_t Size = (uint32_t(Header[0]) << 24) | (uint32_t(Header[1]) << 16) | (uint32_t(Header[2]) << 8) | Header[3];
		const uint32_t Blocks = (uint32_t(Header[4]) << 24) | (uint32_t(Header[5]) << 16) | (uint32_t(Header[6]) << 8) | Header[7];
		const uint32_t Final = (uint32_t(Header[8]) << 8) | Header[9];
		std::string Name;
		for (;;)
		{
			const int B = ReadByte(60);
			if (B < 0) { m_Log.Event("receive: name timeout"); SendText("ERROR\r\n"); return false; }
			if (B == 0) break;
			Name += char(B);
		}
		m_Log.Event("receive: name '%s' size %u blocks %u final %u", Name.c_str(), Size, Blocks, Final);
		if (Name.empty() || Name.find('/') != std::string::npos || Blocks * 16384u + Final != Size)
		{
			m_Log.Event("receive: header rejected");
			SendText("ERROR\r\n");
			return false;
		}
		SendText("OK\r\n");
		std::vector<uint8_t> File;
		File.reserve(Size);
		std::vector<uint8_t> Block(16384 + 1);
		uint32_t Retries = 0;
		for (uint32_t N = 0; N <= Blocks; ++N)
		{
			const size_t Len = N < Blocks ? 16384 : Final;
			for (int Attempt = 0;; ++Attempt)
			{
				if (!ReadExact(Block.data(), Len + 1, 60)) { m_Log.Event("receive: block %u timeout", N); SendText("ERROR\r\n"); return false; }
				uint8_t Sum = 0;
				for (size_t I = 0; I < Len; ++I) Sum = uint8_t(Sum + Block[I]);
				if (Sum == Block[Len]) { Send("!", 1); break; }
				m_Log.Event("receive: block %u checksum %02x != %02x (attempt %d)", N, Block[Len], Sum, Attempt + 1);
				++Retries;
				Send("?", 1);
				if (Attempt >= 2) { m_Log.Event("receive: block %u given up", N); return false; }
			}
			File.insert(File.end(), Block.begin(), Block.begin() + long(Len));
		}
		const std::string Dir = GuestToHost("/" + m_Opt.m_ReceiveDir);
		mkdir(Dir.c_str(), 0755);
		const std::string Host = Dir + "/" + Name;
		FILE* F = fopen(Host.c_str(), "wb");
		if (!F || fwrite(File.data(), 1, File.size(), F) != File.size()) { m_Log.Event("receive: cannot write %s", Host.c_str()); if (F) fclose(F); SendText("ERROR\r\n"); return false; }
		fclose(F);
		chmod(Host.c_str(), 0755);
		m_Log.Event("receive: stored /%s/%s, %zu bytes, %u block retries", m_Opt.m_ReceiveDir.c_str(), Name.c_str(), File.size(), Retries);
		SendText("OK\r\n");
		return true;
	}

	// ---- a foreground program: the wire becomes its stdin/stdout until it exits ----
	int RunProgram(const std::string& HostExe, const std::vector<std::string>& Args)
	{
		int In[2], Out[2];
		if (pipe(In) != 0 || pipe(Out) != 0) return 127;
		const pid_t Pid = fork();
		if (Pid < 0) return 127;
		if (Pid == 0)
		{
			dup2(In[0], 0);
			dup2(Out[1], 1);
			close(In[0]); close(In[1]); close(Out[0]); close(Out[1]);
			if (chdir(m_Opt.m_Root.c_str()) != 0) _exit(126);
			std::vector<char*> Argv;
			for (const std::string& A : Args) Argv.push_back(const_cast<char*>(A.c_str()));
			Argv.push_back(nullptr);
			execv(HostExe.c_str(), Argv.data());
			_exit(127);
		}
		close(In[0]);
		close(Out[1]);
		m_Log.Event("program started: pid %d, %s", int(Pid), HostExe.c_str());
		// anything read ahead belongs to the program
		if (!m_Unread.empty()) { (void)!write(In[1], m_Unread.data(), m_Unread.size()); m_Unread.clear(); }
		uint64_t ToProgram = 0, FromProgram = 0;
		bool bOpen = true;
		while (bOpen)
		{
			fd_set Set;
			FD_ZERO(&Set);
			FD_SET(0, &Set);
			FD_SET(Out[0], &Set);
			if (select(Out[0] + 1, &Set, nullptr, nullptr, nullptr) < 0) { if (errno == EINTR) continue; break; }
			if (FD_ISSET(Out[0], &Set))
			{
				uint8_t Buf[4096];
				const ssize_t N = read(Out[0], Buf, sizeof Buf);
				if (N <= 0) { bOpen = false; break; }
				FromProgram += size_t(N);
				Send(Buf, size_t(N));
			}
			if (FD_ISSET(0, &Set))
			{
				uint8_t Buf[4096];
				const ssize_t N = read(0, Buf, sizeof Buf);
				if (N <= 0) exit(0);
				m_Log.Bytes("rx", Buf, size_t(N));
				m_BytesIn += size_t(N);
				size_t Start = 0;
				if (m_Opt.m_bCooked)
				{
					for (ssize_t I = 0; I < N; ++I) if (Buf[I] == 3) { m_Log.Event("Ctrl-C: SIGINT to pid %d", int(Pid)); kill(Pid, SIGINT); Start = size_t(I) + 1; }
					if (Start < size_t(N)) Send(Buf + Start, size_t(N) - Start); // cooked echo
				}
				if (Start < size_t(N)) { (void)!write(In[1], Buf + Start, size_t(N) - Start); ToProgram += size_t(N) - Start; }
			}
		}
		close(In[1]);
		close(Out[0]);
		int Status = 0;
		waitpid(Pid, &Status, 0);
		const int Code = WIFEXITED(Status) ? WEXITSTATUS(Status) : 128 + WTERMSIG(Status);
		m_Log.Event("program exited: code %d, %llu bytes to it, %llu from it", Code, (unsigned long long)ToProgram, (unsigned long long)FromProgram);
		return Code;
	}

	// ---- the shell: builtins, nextpi-* stand-ins, and programs ----
	int Simple(std::vector<std::string> A)
	{
		// output redirection: "> path" or ">path" anywhere in the simple command
		std::string Redirect;
		for (size_t I = 0; I < A.size();)
		{
			if (A[I] == ">" && I + 1 < A.size()) { Redirect = A[I + 1]; A.erase(A.begin() + long(I), A.begin() + long(I) + 2); }
			else if (A[I].size() > 1 && A[I][0] == '>') { Redirect = A[I].substr(1); A.erase(A.begin() + long(I)); }
			else ++I;
		}
		if (A.empty()) return 0;
		const std::string& Cmd = A[0];
		if (Cmd == "stty") return 0; // the line discipline is whatever the client assumes; nothing to change here
		if (Cmd == "head")
		{
			// head -c N [> file]: N raw bytes from the wire, verbatim
			if (m_Opt.m_bNoHead) { SendText("bash: head: command not found\r\n"); return 127; }
			long N = -1;
			for (size_t I = 1; I + 1 < A.size(); ++I) if (A[I] == "-c") N = atol(A[I + 1].c_str());
			if (N < 0) { SendText("head: usage\r\n"); return 1; }
			std::vector<uint8_t> Data(static_cast<size_t>(N));
			if (!ReadExact(Data.data(), size_t(N), 60)) { m_Log.Event("head: timeout after fewer than %ld bytes", N); return 1; }
			if (!Redirect.empty())
			{
				const std::string Host = GuestToHost(Redirect);
				FILE* F = fopen(Host.c_str(), "wb");
				if (!F || fwrite(Data.data(), 1, Data.size(), F) != Data.size()) { if (F) fclose(F); SendText("head: cannot write\r\n"); return 1; }
				fclose(F);
				m_Log.Event("head: %ld bytes into %s", N, Redirect.c_str());
			}
			else Send(Data.data(), int(Data.size()));
			return 0;
		}
		if (Cmd == "sum")
		{
			// sum -s: System V checksum and 512-byte block count
			if (A.size() < 2) return 1;
			const std::string& Guest = A.back();
			FILE* F = fopen(GuestToHost(Guest).c_str(), "rb");
			if (!F) { SendText("sum: " + Guest + ": No such file or directory\r\n"); return 2; }
			uint32_t S = 0;
			unsigned long Bytes = 0;
			int Ch;
			while ((Ch = fgetc(F)) != EOF) { S += uint32_t(Ch); ++Bytes; }
			fclose(F);
			uint32_t R = (S & 0xFFFF) + (S >> 16);
			R = (R & 0xFFFF) + (R >> 16);
			SendText(std::to_string(R) + " " + std::to_string((Bytes + 511) / 512) + " " + Guest + "\r\n");
			return 0;
		}
		if (Cmd == "echo")
		{
			std::string S;
			for (size_t I = 1; I < A.size(); ++I) S += (I > 1 ? " " : "") + A[I];
			SendText(S + "\r\n");
			return 0;
		}
		if (Cmd == "true") return 0;
		if (Cmd == "false") return 1;
		if (Cmd == "test" || Cmd == "[")
		{
			if (A.size() < 3) return 1;
			const std::string& Flag = A[1];
			const std::string Host = GuestToHost(A[2]);
			const int Kind = Flag == "-f" ? 1 : Flag == "-d" ? 2 : Flag == "-x" ? 3 : 0;
			return Exists(Host, Kind) ? 0 : 1;
		}
		if (Cmd == "ls" && A.size() > 2 && A[1] == "-l")
		{
			struct stat St;
			if (stat(GuestToHost(A[2]).c_str(), &St) != 0) { SendText("ls: cannot access '" + A[2] + "': No such file or directory\r\n"); return 2; }
			SendText(std::string(S_ISDIR(St.st_mode) ? "d" : "-") + ((St.st_mode & S_IXUSR) ? "rwxr-xr-x" : "rw-r--r--") +
				" 1 root root " + std::to_string((long)St.st_size) + " Jan  1 00:00 " + A[2] + "\r\n");
			return 0;
		}
		if (Cmd == "grep" && A.size() >= 3 && A.back() == "/proc/mounts")
		{
			// the sandbox's RAM disk, as a Linux tmpfs would report it
			if (A[1].find("/ram") != std::string::npos) SendText("tmpfs /ram tmpfs rw,relatime 0 0\r\n");
			return 0;
		}
		if (Cmd == "cp" && A.size() == 3)
		{
			std::string Dst = GuestToHost(A[2]);
			if (Exists(Dst, 2)) Dst += "/" + A[1].substr(A[1].rfind('/') + 1);
			FILE* In = fopen(GuestToHost(A[1]).c_str(), "rb");
			FILE* Out = In ? fopen(Dst.c_str(), "wb") : nullptr;
			if (!In || !Out) { if (In) fclose(In); SendText("cp: cannot copy\r\n"); return 1; }
			std::vector<uint8_t> Buf(4096);
			size_t N;
			while ((N = fread(Buf.data(), 1, Buf.size(), In)) > 0) fwrite(Buf.data(), 1, N, Out);
			fclose(In); fclose(Out); chmod(Dst.c_str(), 0755);
			return 0;
		}
		if (Cmd == "ls")
		{
			const std::string Host = GuestToHost(A.size() > 1 ? A[1] : "/");
			if (Exists(Host, 1)) { SendText(A[1] + "\r\n"); return 0; }
			DIR* D = opendir(Host.c_str());
			if (!D) { SendText("ls: cannot access '" + (A.size() > 1 ? A[1] : "/") + "': No such file or directory\r\n"); return 2; }
			std::vector<std::string> Names;
			while (dirent* E = readdir(D)) if (E->d_name[0] != '.') Names.push_back(E->d_name);
			closedir(D);
			std::sort(Names.begin(), Names.end());
			for (const std::string& N : Names) SendText(N + "\r\n");
			return 0;
		}
		if (Cmd == "rm" || Cmd == "nextpi-file_delete")
		{
			if (A.size() < 2) return 1;
			const int R = unlink(GuestToHost(A[1]).c_str());
			if (Cmd != "rm") SendText(R == 0 ? "OK\r\n" : "ERROR\r\n");
			return R == 0 ? 0 : 1;
		}
		if (Cmd == "chmod")
		{
			if (A.size() < 3) return 1;
			const std::string Host = GuestToHost(A[2]);
			struct stat St;
			if (stat(Host.c_str(), &St) != 0) { SendText("chmod: cannot access '" + A[2] + "': No such file or directory\r\n"); return 1; }
			if (A[1] == "+x") chmod(Host.c_str(), St.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
			else if (A[1] == "-x") chmod(Host.c_str(), St.st_mode & ~(S_IXUSR | S_IXGRP | S_IXOTH));
			return 0;
		}
		if (Cmd == "cat")
		{
			if (A.size() < 2) return 1;
			FILE* F = fopen(GuestToHost(A[1]).c_str(), "rb");
			if (!F) { SendText("cat: " + A[1] + ": No such file or directory\r\n"); return 1; }
			std::vector<uint8_t> Buf(4096);
			size_t N;
			while ((N = fread(Buf.data(), 1, Buf.size(), F)) > 0) Send(Buf.data(), N);
			fclose(F);
			return 0;
		}
		if (Cmd == "nextpi-admin_version") { SendText(m_Opt.m_Version + "\r\n"); return 0; }
		if (Cmd == "nextpi-echo_disable") { m_bEcho = false; return 0; }
		if (Cmd == "nextpi-echo_enable") { m_bEcho = true; return 0; }
		if (Cmd == "nextpi-file_receive") return FileReceive(A) ? 0 : 1;
		if (Cmd.rfind("nextpi-", 0) == 0)
		{
			m_Log.Event("unsupported nextpi command: %s", Cmd.c_str());
			SendText("ERROR\r\n");
			return 1;
		}
		for (const auto& M : m_Opt.m_Native)
		{
			if (M.first != Cmd) continue;
			const std::vector<std::string> Host = Tokens(M.second); // HOST_PATH [extra args]
			std::vector<std::string> Args{A[0]};
			Args.insert(Args.end(), Host.begin() + 1, Host.end());
			Args.insert(Args.end(), A.begin() + 1, A.end());
			return RunProgram(Host[0], Args);
		}
		const std::string Host = GuestToHost(Cmd);
		if (Cmd[0] == '/' && Exists(Host, 3)) return RunProgram(Host, A);
		SendText("bash: " + Cmd + ": command not found\r\n");
		return 127;
	}
	void Execute(const std::string& Line)
	{
		m_Log.Event("command: %s", Line.c_str());
		if (m_Opt.m_bRedraw && !Line.empty()) SendText("SUP> " + Line + "\r\n");
		// && || ; chains, left to right
		size_t Pos = 0;
		int Last = 0;
		std::string Op;
		while (Pos <= Line.size())
		{
			size_t Next = std::string::npos;
			std::string NextOp;
			for (const char* Candidate : {"&&", "||", ";"})
			{
				const size_t F = Line.find(Candidate, Pos);
				if (F != std::string::npos && F < Next) { Next = F; NextOp = Candidate; }
			}
			const std::string Seg = Line.substr(Pos, Next == std::string::npos ? std::string::npos : Next - Pos);
			const bool bRun = Op.empty() || (Op == "&&" && Last == 0) || (Op == "||" && Last != 0) || Op == ";";
			if (bRun) Last = Simple(Tokens(Seg));
			if (Next == std::string::npos) break;
			Op = NextOp;
			Pos = Next + NextOp.size();
		}
	}

	void Run()
	{
		m_Log.Event("nextpi-sup root=%s version=%s receive-dir=%s", m_Opt.m_Root.c_str(), m_Opt.m_Version.c_str(), m_Opt.m_ReceiveDir.c_str());
		for (;;)
		{
			const int B = ReadByte(3600);
			if (B < 0) continue;
			if (B == 3) // Ctrl-C at the prompt
			{
				if (m_bEcho) SendText("^C\r\n");
				m_Line.clear();
				Prompt();
			}
			else if (B == 4) // Ctrl-D: EOF to bash, the supervisor restarts it; a later Enter brings the prompt back
			{
				m_Log.Event("Ctrl-D: shell restarted");
				m_Line.clear();
				m_bEcho = true;
			}
			else if (B == 8 || B == 127)
			{
				if (!m_Line.empty()) { m_Line.pop_back(); if (m_bEcho) SendText("\b \b"); }
			}
			else if (B == '\n' || B == '\r')
			{
				if (m_bEcho) SendText("\r\n");
				const std::string Line = m_Line;
				m_Line.clear();
				Execute(Line);
				Prompt();
			}
			else
			{
				if (m_bEcho) Send(&B, 1);
				m_Line += char(B);
			}
		}
	}
};
} // namespace

int main(int Argc, char** Argv)
{
	Supervisor S;
	Options& O = S.m_Opt;
	for (int I = 1; I < Argc; ++I)
	{
		const std::string A = Argv[I];
		auto Value = [&]() -> const char* { return I + 1 < Argc ? Argv[++I] : nullptr; };
		if (A == "--root") { const char* V = Value(); if (!V) return 2; O.m_Root = V; }
		else if (A == "--log") { const char* V = Value(); if (!V) return 2; O.m_LogPath = V; }
		else if (A == "--version") { const char* V = Value(); if (!V) return 2; O.m_Version = V; }
		else if (A == "--receive-dir") { const char* V = Value(); if (!V) return 2; O.m_ReceiveDir = V; }
		else if (A == "--cooked") O.m_bCooked = true;
		else if (A == "--no-head") O.m_bNoHead = true;
		else if (A == "--redraw") O.m_bRedraw = true;
		else if (A == "--native")
		{
			const char* V = Value();
			if (!V) return 2;
			const std::string M = V;
			const size_t Eq = M.find('=');
			if (Eq == std::string::npos) { fprintf(stderr, "--native wants GUEST=HOST\n"); return 2; }
			O.m_Native.emplace_back(M.substr(0, Eq), M.substr(Eq + 1));
		}
		else
		{
			fprintf(stderr, "usage: nextpi-sup --root DIR [--native /ram/prog=HOST]... [--log FILE] [--version V] [--receive-dir ram] [--cooked] [--no-head]\n");
			return 2;
		}
	}
	if (O.m_Root.empty()) { fputs("nextpi-sup: --root is required\n", stderr); return 2; }
	while (O.m_Root.size() > 1 && O.m_Root.back() == '/') O.m_Root.pop_back();
	mkdir(O.m_Root.c_str(), 0755);
	for (const char* D : {"/ram", "/PCache", "/NextPi", "/tmp"}) mkdir((O.m_Root + D).c_str(), 0755);
	if (!O.m_LogPath.empty())
	{
		S.m_Log.m_File = fopen(O.m_LogPath.c_str(), "w");
		if (!S.m_Log.m_File) { perror(O.m_LogPath.c_str()); return 2; }
	}
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "nextpi-sup: ready, sandbox %s%s\n", O.m_Root.c_str(), O.m_LogPath.empty() ? "" : (", log " + O.m_LogPath).c_str());
	S.Run();
	return 0;
}
#endif
