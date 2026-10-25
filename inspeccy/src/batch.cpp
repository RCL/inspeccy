#include "batch.h"
#include "irq_report.h"
#include "io.h"
#include "json_parser.h"
#include "memory.h"
#include "z80.h"
#include <trace/sld.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <vector>

namespace
{
struct Step
{
	std::string Op, Name, Path, Symbol, Type;
	int EntryPage = -1;
	std::vector<std::pair<std::string, uint32_t>> Registers;
	std::vector<uint8_t> Bytes;
	uint64_t Cycles = 0;
	uint32_t Page = 0, Offset = 0, Length = 0, Index = 0, Value = 0, Pc = 0, ReturnPc = 0;
	bool Until = false;
	int UntilPage = -1;
	unsigned StackWords = 16, PcBytes = 16;
	DeviceQuery Device;
};
std::string Quote(const std::string& S)
{
	std::string R = "\"";
	const char* H = "0123456789abcdef";
	for (unsigned char C : S)
	{
		if (C == '"' || C == '\\')
		{
			R += '\\';
			R += char(C);
		}
		else if (C < 32)
		{
			R += "\\u00";
			R += H[C >> 4];
			R += H[C & 15];
		}
		else
		{
			R += char(C);
		}
	}
	return R + '"';
}
void Fields(const JsonValue& V, std::initializer_list<const char*> Allowed)
{
	if (!V.IsObject())
	{
		throw std::runtime_error("expected an object");
	}
	std::set<std::string> Seen;
	for (const auto& K : V.m_O)
	{
		if (!Seen.insert(K.first).second)
		{
			throw std::runtime_error("duplicate field: " + K.first);
		}
		if (std::none_of(Allowed.begin(), Allowed.end(),
				[&](const char* A)
				{
					return K.first == A;
				}))
		{
			throw std::runtime_error("unknown field: " + K.first);
		}
	}
}
const JsonValue& Required(const JsonValue& V, const char* Key)
{
	const auto* P = V.Find(Key);
	if (!P)
	{
		throw std::runtime_error(std::string("missing field: ") + Key);
	}
	return *P;
}
std::string String(const JsonValue& V, const char* Key)
{
	const auto& P = Required(V, Key);
	if (!P.IsString())
	{
		throw std::runtime_error(std::string("expected string: ") + Key);
	}
	return P.m_S;
}
struct Resolver
{
	const SldSymbolMap& Map;
	bool Bound;
	SldSymbolMap::Symbol Get(const std::string& Name) const
	{
		if (!Bound)
		{
			throw std::runtime_error("symbolic batches require --build-manifest (use --allow-build-mismatch REASON "
									 "only for intentional legacy/patch state)");
		}
		SldSymbolMap::Symbol S;
		std::string Error;
		if (!Map.Resolve(Name, S, Error))
		{
			throw std::runtime_error(Error);
		}
		return S;
	}
};
uint64_t Number(const JsonValue& V, uint64_t Max, const Resolver* Symbols = nullptr)
{
	uint64_t N = 0;
	uint32_t Hex = 0;
	if (V.IsObject() && Symbols)
	{
		Fields(V, {"symbol"});
		N = Symbols->Get(String(V, "symbol")).Value;
	}
	else if (V.IsString() && ParseHexQuoted(V.m_S, Hex))
	{
		N = Hex;
	}
	else if (!V.TryAsUInt(N))
	{
		throw std::runtime_error("expected unsigned integer or $hex string");
	}
	if (N > Max)
	{
		throw std::runtime_error("number out of range");
	}
	return N;
}
uint32_t Num(const JsonValue& V, const char* Key, uint32_t Max, const Resolver* Symbols = nullptr)
{
	return uint32_t(Number(Required(V, Key), Max, Symbols));
}
uint32_t Register(Z80State& C, const std::string& Name, bool Write = false, uint32_t Value = 0)
{
#define REG16(N, M)                                                                                                    \
	if (Name == N)                                                                                                     \
	{                                                                                                                  \
		if (Value > 65535)                                                                                             \
			throw std::runtime_error("register value out of range");                                                   \
		if (Write)                                                                                                     \
			C.M = uint16_t(Value);                                                                                     \
		return C.M;                                                                                                    \
	}
#define REG8(N, M, MAX)                                                                                                \
	if (Name == N)                                                                                                     \
	{                                                                                                                  \
		if (Value > MAX)                                                                                               \
			throw std::runtime_error("register value out of range");                                                   \
		if (Write)                                                                                                     \
			C.M = uint8_t(Value);                                                                                      \
		return C.M;                                                                                                    \
	}
	REG16("AF", AF)
	REG16("BC", BC)
	REG16("DE", DE)
	REG16("HL", HL) REG16("AF'", AFp) REG16("BC'", BCp) REG16("DE'", DEp) REG16("HL'", HLp) REG16("IX", IX)
		REG16("IY", IY) REG16("SP", SP) REG16("PC", PC) REG16("WZ", m_MemPtr) REG8("I", I, 255) REG8("R", R, 255)
			REG8("IFF1", IFF1, 1) REG8("IFF2", IFF2, 1) REG8("IM", IM, 2)
#undef REG16
#undef REG8
				if (Name == "HALTED" || Name == "EI_PENDING")
	{
		if (Value > 1)
		{
			throw std::runtime_error("boolean register value out of range");
		}
		bool& B = Name == "HALTED" ? C.m_bHalted : C.m_bEiPending;
		if (Write)
		{
			B = Value != 0;
		}
		return B;
	}
	throw std::runtime_error("unknown register: " + Name);
}
std::vector<uint8_t> Hex(const std::string& S)
{
	std::vector<uint8_t> B;
	int High = -1;
	for (unsigned char C : S)
	{
		if (std::isspace(C))
		{
			continue;
		}
		int V = C >= '0' && C <= '9'   ? C - '0'
				: C >= 'A' && C <= 'F' ? C - 'A' + 10
				: C >= 'a' && C <= 'f' ? C - 'a' + 10
									   : -1;
		if (V < 0)
		{
			throw std::runtime_error("invalid hex byte string");
		}
		if (High < 0)
		{
			High = V;
		}
		else
		{
			B.push_back(uint8_t(High * 16 + V));
			High = -1;
		}
	}
	if (High >= 0 || B.empty())
	{
		throw std::runtime_error("hex needs complete byte pairs");
	}
	return B;
}
void ParseSteps(
	const JsonValue& List, std::vector<Step>& Out, const IoContext& Io, const Resolver& Symbols, unsigned Depth = 0)
{
	const auto& Mem = Io.m_Mem;
	if (!List.IsArray() || Depth > 8)
	{
		throw std::runtime_error("steps must be an array; maximum repeat depth is eight");
	}
	uint64_t ExpandedBytes = 0;
	auto Size = [](const Step& S)
	{
		return sizeof(Step) + S.Name.size() + S.Path.size() + S.Symbol.size() + S.Type.size() + S.Bytes.size() +
			   S.Registers.size() * 64 + S.Device.Expected.size() * 2;
	};
	size_t SourceIndex = 0;
	for (const auto& V : List.m_A)
	{
		Step S;
		S.Path = "steps[" + std::to_string(SourceIndex++) + "]";
		S.Op = String(V, "op");
		if (V.Find("name"))
		{
			S.Name = String(V, "name");
		}
		if (S.Name.size() > 128)
		{
			throw std::runtime_error("step name exceeds 128 UTF-8 bytes");
		}
		if (S.Op == "repeat")
		{
			Fields(V, {"op", "name", "count", "steps"});
			uint32_t Count = Num(V, "count", 100000);
			if (!Count)
			{
				throw std::runtime_error("repeat count must be positive");
			}
			std::vector<Step> Body;
			ParseSteps(Required(V, "steps"), Body, Io, Symbols, Depth + 1);
			if (Body.empty() || Body.size() * Count + Out.size() > 100000)
			{
				throw std::runtime_error("batch exceeds 100000 expanded steps, or repeat is empty");
			}
			for (const auto& B : Body)
			{
				ExpandedBytes += Size(B) * Count;
			}
			if (ExpandedBytes > 32 * 1024 * 1024)
			{
				throw std::runtime_error("expanded batch exceeds 32 MiB");
			}
			for (uint32_t I = 0; I < Count; ++I)
			{
				for (auto B : Body)
				{
					B.Path = S.Path + ".repeat[" + std::to_string(I) + "]." + B.Path;
					Out.push_back(std::move(B));
				}
			}
			continue;
		}
		if (S.Op == "set" || S.Op == "expect_regs")
		{
			Fields(V, {"op", "name", "registers"});
			const auto& R = Required(V, "registers");
			if (!R.IsObject() || R.m_O.empty())
			{
				throw std::runtime_error("registers must be a nonempty object");
			}
			Z80State Dummy;
			std::set<std::string> Seen;
			for (const auto& K : R.m_O)
			{
				uint32_t N = uint32_t(Number(K.second, 65535, &Symbols));
				if (!Seen.insert(K.first).second)
				{
					throw std::runtime_error("duplicate register: " + K.first);
				}
				Register(Dummy, K.first, true, N);
				S.Registers.push_back({K.first, N});
			}
		}
		else if (S.Op == "write" || S.Op == "expect_memory" || S.Op == "read" || S.Op == "write_value" ||
				 S.Op == "expect_value" || S.Op == "read_value")
		{
			const bool Typed = S.Op.find("_value") != std::string::npos;
			const bool Read = S.Op == "read" || S.Op == "read_value";
			if (Typed && Read)
			{
				Fields(V, {"op", "name", "symbol", "page", "offset", "type"});
			}
			else if (Typed)
			{
				Fields(V, {"op", "name", "symbol", "page", "offset", "type", "value"});
			}
			else if (Read)
			{
				Fields(V, {"op", "name", "symbol", "page", "offset", "length"});
			}
			else
			{
				Fields(V, {"op", "name", "symbol", "page", "offset", "hex"});
			}
			if (V.Find("symbol"))
			{
				if (V.Find("page"))
				{
					throw std::runtime_error("use symbol or page, not both");
				}
				S.Symbol = String(V, "symbol");
				const auto Sym = Symbols.Get(S.Symbol);
				if (Sym.Page < 0)
				{
					throw std::runtime_error("memory symbol has no physical page: " + S.Symbol);
				}
				S.Page = uint32_t(Sym.Page);
				S.Offset = (Sym.Value % 8192) + (V.Find("offset") ? Num(V, "offset", 8191) : 0);
			}
			else
			{
				S.Page = Num(V, "page", uint32_t(Mem.GetEffectiveRamPages() - 1), &Symbols);
				S.Offset = Num(V, "offset", 8191);
			}
			if (S.Page >= uint32_t(Mem.GetEffectiveRamPages()) || S.Offset >= 8192)
			{
				throw std::runtime_error("symbol outside physical RAM page");
			}
			if (Typed)
			{
				S.Type = String(V, "type");
				S.Length = S.Type == "u8" ? 1 : S.Type == "u16le" ? 2 : S.Type == "u32le" ? 4 : 0;
				if (!S.Length)
				{
					throw std::runtime_error("type must be u8, u16le or u32le");
				}
				if (!Read)
				{
					S.Value = uint32_t(Number(
						Required(V, "value"), S.Length == 4 ? UINT32_MAX : (1u << (8 * S.Length)) - 1, &Symbols));
					for (uint32_t I = 0; I < S.Length; ++I)
					{
						S.Bytes.push_back(uint8_t(S.Value >> (8 * I)));
					}
				}
			}
			else if (Read)
			{
				S.Length = Num(V, "length", 8192);
			}
			else
			{
				S.Bytes = Hex(String(V, "hex"));
				S.Length = uint32_t(S.Bytes.size());
			}
			if (!S.Length || S.Length > 8192 - S.Offset)
			{
				throw std::runtime_error("memory range crosses an 8K page");
			}
		}
		else if (S.Op == "nextreg")
		{
			Fields(V, {"op", "name", "index", "value"});
			if (!Mem.IsNext())
			{
				throw std::runtime_error("nextreg requires a Next machine");
			}
			S.Index = Num(V, "index", 255);
			S.Value = Num(V, "value", 255, &Symbols);
		}
		else if (S.Op == "run" || S.Op == "call")
		{
			if (S.Op == "call")
			{
				Fields(V, {"op", "name", "pc", "symbol", "return_pc", "cycles"});
			}
			else
			{
				Fields(V, {"op", "name", "cycles", "until_pc", "until_page"});
			}
			S.Cycles = Number(Required(V, "cycles"), UINT64_MAX);
			if (!S.Cycles)
			{
				throw std::runtime_error("cycles must be positive");
			}
			if (S.Op == "call")
			{
				if (V.Find("symbol"))
				{
					if (V.Find("pc"))
					{
						throw std::runtime_error("use symbol or pc, not both");
					}
					S.Symbol = String(V, "symbol");
					const auto Sym = Symbols.Get(S.Symbol);
					if (Sym.Page < 0 || Sym.Value > 65535)
					{
						throw std::runtime_error("call symbol must have a physical address");
					}
					S.Pc = Sym.Value;
					S.EntryPage = Sym.Page;
				}
				else
				{
					S.Pc = Num(V, "pc", 65535);
				}
				S.ReturnPc = Num(V, "return_pc", 65535);
				S.Until = true;
				if (S.Pc == S.ReturnPc)
				{
					throw std::runtime_error("call entry and return PC must differ");
				}
			}
			else if (V.Find("until_pc"))
			{
				S.ReturnPc = Num(V, "until_pc", 65535);
				S.Until = true;
			}
			if (V.Find("until_page"))
			{
				if (!S.Until || !Mem.IsNext())
				{
					throw std::runtime_error("until_page requires until_pc and a Next machine");
				}
				S.UntilPage = int(Num(V, "until_page", uint32_t(Mem.GetEffectiveRamPages() - 1)));
			}
		}
		else if (S.Op == "read_regs")
		{
			Fields(V, {"op", "name"});
		}
		else if (S.Op == "read_state")
		{
			Fields(V, {"op", "name", "stack_words", "pc_bytes"});
			if (V.Find("stack_words"))
			{
				S.StackWords = Num(V, "stack_words", 64);
			}
			if (V.Find("pc_bytes"))
			{
				S.PcBytes = Num(V, "pc_bytes", 64);
			}
		}
		else if (S.Op == "read_device" || S.Op == "expect_device")
		{
			S.Device = ParseDeviceQuery(V, Io, S.Op == "expect_device");
		}
		else
		{
			throw std::runtime_error("unknown operation: " + S.Op);
		}
		if (Out.size() >= 100000)
		{
			throw std::runtime_error("batch exceeds 100000 expanded steps");
		}
		ExpandedBytes += Size(S);
		if (ExpandedBytes > 32 * 1024 * 1024)
		{
			throw std::runtime_error("expanded batch exceeds 32 MiB");
		}
		Out.push_back(std::move(S));
	}
}
std::string BytesJson(const uint8_t* B, uint32_t N)
{
	const char* H = "0123456789ABCDEF";
	std::string R = "\"";
	for (uint32_t I = 0; I < N; ++I)
	{
		R += H[B[I] >> 4];
		R += H[B[I] & 15];
	}
	return R + '"';
}
} // namespace

int RunBatch(const std::string& Path, const std::string& ReportPath, Z80State& Cpu, IoContext& Io, uint64_t& Budget,
	TraceWriter* Trace, RunSummary& Summary, const SldSymbolMap& Symbols, bool BoundSymbols,
	const InspectionIdentity& Identity)
{
	const uint64_t Start = Cpu.m_Cycles;
	std::vector<Step> Steps;
	std::string Error, Observations, FailureDetails;
	size_t Completed = 0;
	int Exit = 0;
	try
	{
		std::ifstream F(Path, std::ios::binary);
		if (!F)
		{
			Exit = 3;
			throw std::runtime_error("cannot open batch: " + Path);
		}
		F.seekg(0, std::ios::end);
		if (F.tellg() > 4 * 1024 * 1024)
		{
			throw std::runtime_error("batch file exceeds 4 MiB");
		}
		F.seekg(0);
		std::string Text((std::istreambuf_iterator<char>(F)), {});
		JsonValue Root;
		std::string ParseError;
		if (!ParseJson(Text, Root, ParseError))
		{
			throw std::runtime_error(ParseError);
		}
		Fields(Root, {"version", "steps"});
		if (Num(Root, "version", 1) != 1)
		{
			throw std::runtime_error("batch version must be 1");
		}
		ParseSteps(Required(Root, "steps"), Steps, Io, Resolver{Symbols, BoundSymbols});
		if (Steps.empty())
		{
			throw std::runtime_error("batch is empty");
		}
		uint64_t OutputBytes = 1024 * 1024; // leave room for bounded failure context
		for (const auto& S : Steps)
		{
			OutputBytes += (S.Op == "read" || S.Op == "read_value") ? 2 * S.Length + 256
						   : S.Op == "read_regs"					? 768
						   : S.Op == "read_state"
							   ? 4096ull + 6 * Identity.Override.size() + (2 * S.StackWords + S.PcBytes + 1) * 4096ull
						   : S.Op == "read_device" ? 8192
												   : 0;
			if (OutputBytes > 16 * 1024 * 1024)
			{
				throw std::runtime_error("requested observations exceed 16 MiB");
			}
		}
	}
	catch (const std::exception& E)
	{
		if (!Exit)
		{
			Exit = 4;
		}
		Error = E.what();
	}
	if (!Exit)
	{
		Io.m_bBudgetInCycles = true;
		try
		{
			for (const Step& S : Steps)
			{
				if (Io.m_IrqDiagnostics)
				{
					Io.m_IrqDiagnostics->BatchStep = int64_t(Completed);
				}
				std::string Observation;
				if (S.Op == "set" || S.Op == "expect_regs")
				{
					for (const auto& K : S.Registers)
					{
						uint32_t Got = Register(Cpu, K.first, S.Op == "set", K.second);
						if (S.Op == "expect_regs" && Got != K.second)
						{
							throw std::runtime_error(
								K.first + ": expected " + std::to_string(K.second) + ", got " + std::to_string(Got));
						}
					}
				}
				else if (S.Op == "nextreg")
				{
					Io.m_Mem.WriteNextReg(uint8_t(S.Index), uint8_t(S.Value), Cpu.PC, Cpu.m_Cycles);
				}
				else if (S.Op == "write" || S.Op == "expect_memory" || S.Op == "read" || S.Op == "write_value" ||
						 S.Op == "expect_value" || S.Op == "read_value")
				{
					uint8_t* P = Io.m_Mem.Ram16KBank(int(S.Page / 2)) + (S.Page % 2) * 8192 + S.Offset;
					if (S.Op == "write" || S.Op == "write_value")
					{
						std::copy(S.Bytes.begin(), S.Bytes.end(), P);
					}
					else if (S.Op == "read" || S.Op == "read_value")
					{
						Observation = "\"hex\":" + BytesJson(P, S.Length);
						if (S.Op == "read_value")
						{
							uint32_t Value = 0;
							for (uint32_t I = 0; I < S.Length; ++I)
							{
								Value |= uint32_t(P[I]) << (I * 8);
							}
							Observation += ",\"value\":" + std::to_string(Value) + ",\"type\":" + Quote(S.Type);
						}
					}
					else if (!std::equal(S.Bytes.begin(), S.Bytes.end(), P))
					{
						uint32_t At = 0;
						while (At < S.Length && S.Bytes[At] == P[At])
						{
							++At;
						}
						uint32_t Count = std::min(32u, S.Length - At);
						FailureDetails = ",\"mismatch_offset\":" + std::to_string(S.Offset + At) +
										 ",\"expected_hex\":" + BytesJson(S.Bytes.data() + At, Count) +
										 ",\"actual_hex\":" + BytesJson(P + At, Count);
						throw std::runtime_error("memory mismatch at page " + std::to_string(S.Page) + " offset " +
												 std::to_string(S.Offset));
					}
				}
				else if (S.Op == "read_regs")
				{
					Observation = "\"cpu\":" + InspectionCpuJson(Cpu);
				}
				else if (S.Op == "read_state")
				{
					Observation = "\"state\":" + InspectionStateJson(
													 Cpu, Io, Symbols, BoundSymbols, Identity, S.StackWords, S.PcBytes);
				}
				else if (S.Op == "read_device" || S.Op == "expect_device")
				{
					const auto Values = InspectDevice(S.Device, Io);
					if (S.Op == "read_device")
					{
						Observation = "\"device_state\":" + DeviceResultJson(S.Device, Io, Values);
					}
					else if (Values != S.Device.Expected)
					{
						size_t At = 0;
						while (Values[At] == S.Device.Expected[At])
						{
							++At;
						}
						FailureDetails = ",\"device_mismatch\":{" + DeviceSelectorJson(S.Device) +
										 ",\"relative_index\":" + std::to_string(At) +
										 ",\"unit\":" + Quote(S.Device.Raw ? "byte" : "entry") +
										 ",\"expected\":" + std::to_string(S.Device.Expected[At]) +
										 ",\"actual\":" + std::to_string(Values[At]) + '}';
						throw std::runtime_error("device mismatch: " + S.Device.Device);
					}
				}
				else if (S.Op == "run" || S.Op == "call")
				{
					if (S.Cycles > UINT64_MAX - Cpu.m_Cycles)
					{
						throw std::runtime_error("cycle budget overflows clock");
					}
					if (S.Op == "call")
					{
						if (Cpu.SP < 2 || Cpu.m_bHalted)
						{
							throw std::runtime_error("call needs a non-wrapping stack and a non-halted CPU");
						}
						if (S.EntryPage >= 0 &&
							(!Io.m_Mem.IsNext() || Io.m_Mem.GetNextReg(uint8_t(0x50 + (S.Pc >> 13))) != S.EntryPage))
						{
							throw std::runtime_error("call target page is not mapped: " + S.Symbol);
						}
						// Match CALL's stack layout, without injecting any guest code.
						Cpu.SP -= 2;
						Io.m_Mem.Wr(Cpu.SP, uint8_t(S.ReturnPc));
						Io.m_Mem.Wr(uint16_t(Cpu.SP + 1), uint8_t(S.ReturnPc >> 8));
						if (Io.m_Mem.Rd(Cpu.SP) != uint8_t(S.ReturnPc) ||
							Io.m_Mem.Rd(uint16_t(Cpu.SP + 1)) != uint8_t(S.ReturnPc >> 8))
						{
							throw std::runtime_error("call stack is not writable");
						}
						Cpu.PC = uint16_t(S.Pc);
					}
					Io.m_bUntilPc = S.Until;
					Io.m_UntilPc = uint16_t(S.ReturnPc);
					Io.m_UntilPcPage = S.UntilPage;
					Budget = Cpu.m_Cycles + S.Cycles;
					RunSummary Part = RunUntil(Cpu, Io, Budget, Trace);
					Summary = Part;
					const RunResult Expected =
						S.Until ? RunResult::kRunResultReachedPc : RunResult::kRunResultBudgetReached;
					if (Part.m_Result != Expected)
					{
						throw std::runtime_error(S.Until ? "run did not reach the requested PC (budget or CPU stop)"
														 : "CPU stopped before the cycle budget");
					}
				}
				if (!Observation.empty())
				{
					if (!Observations.empty())
					{
						Observations += ',';
					}
					Observations += "{\"step\":" + std::to_string(Completed) + ",\"name\":" + Quote(S.Name) +
									",\"cycles\":" + std::to_string(Cpu.m_Cycles - Start) + "," + Observation + "}";
				}
				++Completed;
			}
		}
		catch (const std::exception& E)
		{
			Exit = 9;
			Error = E.what();
		}
	}
	Summary.m_CyclesExecuted = Cpu.m_Cycles; // RunSummary uses the absolute CPU counter
	if (Io.m_IrqDiagnostics)
	{
		Io.m_IrqDiagnostics->BatchStep = -1;
	}
	Summary.m_LastPC = Cpu.PC;
	std::string Report = "{\"version\":1,\"ok\":" + std::string(Exit ? "false" : "true") +
						 ",\"completed_steps\":" + std::to_string(Completed) +
						 ",\"cycles\":" + std::to_string(Cpu.m_Cycles - Start) + ",\"observations\":[" + Observations +
						 "]";
	if (Io.m_IrqDiagnostics)
	{
		Report += ",\"irq_diagnostics\":" + IrqSummaryJson(*Io.m_IrqDiagnostics);
	}
	if (Exit)
	{
		Report += ",\"error\":" + Quote(Error) + ",\"failed_step\":" + std::to_string(Completed);
		if (Exit == 9 && Completed < Steps.size())
		{
			const auto& S = Steps[Completed];
			Report += ",\"failure\":{\"name\":" + Quote(S.Name) + ",\"path\":" + Quote(S.Path) +
					  ",\"symbol\":" + Quote(S.Symbol) + ",\"page\":" + std::to_string(S.Page) +
					  ",\"offset\":" + std::to_string(S.Offset) + ",\"cpu\":" + InspectionCpuJson(Cpu) +
					  FailureDetails +
					  ",\"state\":" + InspectionStateJson(Cpu, Io, Symbols, BoundSymbols, Identity, 8, 8);
			if (Io.m_Mem.IsNext())
			{
				Report += ",\"mmu\":[";
				for (uint8_t I = 0; I < 8; ++I)
				{
					Report += (I ? "," : "") + std::to_string(Io.m_Mem.GetNextReg(0x50 + I));
				}
				Report += ']';
			}
			Report += '}';
		}
	}
	Report += "}\n";
	std::ofstream F(ReportPath, std::ios::binary);
	F << Report;
	F.close();
	if (!F)
	{
		fprintf(stderr, "cannot write batch report: %s\n", ReportPath.c_str());
		return 3;
	}
	fprintf(stderr, "batch: %zu steps, %llu cycles, %s%s\n", Completed,
		static_cast<unsigned long long>(Cpu.m_Cycles - Start), Exit ? "FAILED: " : "ok", Error.c_str());
	return Exit;
}
