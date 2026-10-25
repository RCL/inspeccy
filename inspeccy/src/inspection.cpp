#include <inspeccy/version.h>
#include "inspection.h"
#include "diagnostic_json.h"
#include "io.h"
#include "z80.h"
#include "ay.h"
#include "sprites.h"
#include <trace/sld.h>
#include <trace/disasm.h>
#include <cctype>

using namespace DiagnosticJson;

std::string InspectionCpuJson(const Z80State& C)
{
	std::string R = "{";
	auto Add = [&](const char* Name, uint64_t Value)
	{
		if (R.size() > 1)
		{
			R += ',';
		}
		R += Quote(Name) + ':' + std::to_string(Value);
	};
	Add("AF", C.AF);
	Add("BC", C.BC);
	Add("DE", C.DE);
	Add("HL", C.HL);
	Add("AF'", C.AFp);
	Add("BC'", C.BCp);
	Add("DE'", C.DEp);
	Add("HL'", C.HLp);
	Add("IX", C.IX);
	Add("IY", C.IY);
	Add("SP", C.SP);
	Add("PC", C.PC);
	Add("WZ", C.m_MemPtr);
	Add("I", C.I);
	Add("R", C.R);
	Add("IFF1", C.IFF1);
	Add("IFF2", C.IFF2);
	Add("IM", C.IM);
	Add("HALTED", C.m_bHalted);
	Add("EI_PENDING", C.m_bEiPending);
	return R + '}';
}

std::string InspectionIdentityJson(const InspectionIdentity& I, bool BoundSymbols)
{
	return "{\"image_sha1\":" + (I.ImageSha1.empty() ? "null" : Quote(I.ImageSha1)) + ",\"symbols\":" +
		   Quote(!BoundSymbols		  ? "unverified"
				 : I.Override.empty() ? "manifest_bound"
									  : "override") +
		   ",\"override\":" + (I.Override.empty() ? "null" : Quote(I.Override)) + '}';
}

namespace
{
std::string MappingJson(const DebugMapping& M)
{
	const char* K[] = {"ram", "rom", "open_bus", "other", "unavailable"};
	return "{\"kind\":" + Quote(K[M.m_Kind]) + ",\"page\":" + (M.m_Page < 0 ? "null" : std::to_string(M.m_Page)) +
		   ",\"offset\":" + std::to_string(M.m_Offset) + ",\"writable\":" + (M.m_Writable ? "true" : "false") + '}';
}
// Bound symbol text before escaping, preserving UTF-8 boundaries.
std::string ShortName(const std::string& S)
{
	size_t N = std::min(size_t(128), S.size());
	while (N < S.size() && N && (uint8_t(S[N]) & 0xc0) == 0x80)
	{
		--N;
	}
	return S.substr(0, N);
}
std::string PeekJson(const MemoryBus& Mem, uint16_t Addr)
{
	uint8_t V = 0;
	return Mem.DebugPeek(Addr, V) ? std::to_string(V) : "null";
}
} // namespace

std::string InspectionAddressJson(uint16_t Address, const DebugMapping& M, const SldSymbolMap& Symbols)
{
	std::string R = "{\"address\":" + std::to_string(Address) + ",\"mapping\":" + MappingJson(M);
	size_t Total = 0;
	const auto Labels = M.m_Kind == DebugMapping::Ram ? Symbols.Annotate(uint8_t(M.m_Page), M.m_Offset, 256, 4, &Total)
													  : std::vector<SldSymbolMap::Annotation>{};
	R += ",\"labels\":[";
	for (size_t I = 0; I < Labels.size(); ++I)
	{
		const auto& A = Labels[I];
		if (I)
		{
			R += ',';
		}
		const auto Name = ShortName(A.Name);
		R += "{\"name\":" + Quote(Name) + ",\"name_truncated\":" + (Name.size() != A.Name.size() ? "true" : "false") +
			 ",\"assembled_address\":" + std::to_string(A.Address) + ",\"offset\":" + std::to_string(A.Offset) + '}';
	}
	return R + "],\"ambiguous\":" + (Total > 1 ? "true" : "false") +
		   ",\"omitted_labels\":" + std::to_string(Total - Labels.size()) + '}';
}

std::string InspectionStateJson(const Z80State& C, const IoContext& Io, const SldSymbolMap& Symbols, bool BoundSymbols,
	const InspectionIdentity& Identity, unsigned StackWords, unsigned PcBytes)
{
	if (StackWords > 64 || PcBytes > 64)
	{
		throw std::runtime_error("inspection limit is 64 stack words / PC bytes");
	}
	const auto& M = Io.m_Mem;
	auto Address = [&](uint16_t A)
	{
		return InspectionAddressJson(A, M.InspectMapping(A), Symbols);
	};
	std::string R = "{\"version\":1,\"cpu\":" + InspectionCpuJson(C) + ",\"cycles\":" + std::to_string(C.m_Cycles) +
					",\"identity\":" + InspectionIdentityJson(Identity, BoundSymbols) + ",\"pc\":" + Address(C.PC) +
					",\"slots\":[";
	for (unsigned I = 0; I < 8; ++I)
	{
		if (I)
		{
			R += ',';
		}
		R += "{\"slot\":" + std::to_string(I) + ",\"read\":" + MappingJson(M.InspectMapping(uint16_t(I * 8192))) +
			 ",\"write\":" + MappingJson(M.InspectMapping(uint16_t(I * 8192), true)) + '}';
	}
	R += "],\"requested_mmu\":";
	if (M.IsNext())
	{
		R += '[';
		for (unsigned I = 0; I < 8; ++I)
		{
			R += (I ? "," : "") + std::to_string(M.GetNextReg(uint8_t(0x50 + I)));
		}
		R += ']';
	}
	else
	{
		R += "null";
	}
	R += ",\"pc_bytes\":[";
	for (unsigned I = 0; I < PcBytes; ++I)
	{
		R += (I ? "," : "") + PeekJson(M, uint16_t(C.PC + I));
	}
	R += "],\"pc_wraps\":" + std::string(unsigned(C.PC) + PcBytes > 65536 ? "true" : "false") + ",\"instructions\":[";
	bool First = true;
	for (unsigned I = 0; I < PcBytes;)
	{
		uint8_t Bytes[4]{};
		bool Valid = true;
		for (unsigned J = 0; J < 4 && I + J < PcBytes; ++J)
		{
			Valid &= M.DebugPeek(uint16_t(C.PC + I + J), Bytes[J]);
		}
		char Text[128];
		const int N = Disassemble(Bytes, uint16_t(C.PC + I), Text, sizeof(Text));
		if (!Valid || N <= 0 || I + unsigned(N) > PcBytes)
		{
			break;
		}
		if (!First)
		{
			R += ',';
		}
		First = false;
		R += "{\"location\":" + Address(uint16_t(C.PC + I)) + ",\"hex\":" + Hex(Bytes, size_t(N)) +
			 ",\"text\":" + Quote(Text) + '}';
		I += unsigned(N);
	}
	R += "],\"stack_wraps\":" + std::string(unsigned(C.SP) + StackWords * 2 > 65536 ? "true" : "false") +
		 ",\"stack_word_semantics\":\"unclassified; address candidates use current mapping, not a call "
		 "stack\",\"stack\":[";
	for (unsigned I = 0; I < StackWords; ++I)
	{
		if (I)
		{
			R += ',';
		}
		const uint16_t A = uint16_t(C.SP + I * 2);
		uint8_t Lo = 0, Hi = 0;
		const bool Valid = M.DebugPeek(A, Lo) && M.DebugPeek(uint16_t(A + 1), Hi);
		R += "{\"location\":" + Address(A) +
			 ",\"high_byte_mapping\":" + MappingJson(M.InspectMapping(uint16_t(A + 1))) + ",\"bytes\":[" +
			 PeekJson(M, A) + ',' + PeekJson(M, uint16_t(A + 1)) +
			 "],\"value\":" + (Valid ? std::to_string(unsigned(Lo) | (unsigned(Hi) << 8)) : "null") +
			 ",\"address_candidate\":" + (Valid ? Address(uint16_t(Lo | (Hi << 8))) : "null") + '}';
	}
	return R + "]}";
}

DeviceQuery ParseDeviceQuery(const JsonValue& V, const IoContext& Io, bool Expect)
{
	DeviceQuery Q;
	Q.Device = String(V, "device");
	if (Q.Device == "palette")
	{
		Fields(V, {"op", "name", "device", "layer", "bank", "start", "count", "encoding", "values", "hex"});
		if (!Io.m_Mem.IsNext())
		{
			throw std::runtime_error("palette device requires a Next machine");
		}
		Q.Layer = String(V, "layer");
		if (Q.Layer != "ula" && Q.Layer != "layer2" && Q.Layer != "sprites" && Q.Layer != "tilemap")
		{
			throw std::runtime_error("unknown palette layer");
		}
		Q.Bank = Num(V, "bank", 2);
		if (!Q.Bank)
		{
			throw std::runtime_error("palette bank must be 1 or 2");
		}
		Q.Encoding = V.Find("encoding") ? String(V, "encoding") : "rgb333";
		if (Q.Encoding != "rgb333" && Q.Encoding != "raw")
		{
			throw std::runtime_error("palette encoding must be rgb333 or raw");
		}
		Q.Raw = Q.Encoding == "raw";
	}
	else if (Q.Device == "ay")
	{
		Fields(V, {"op", "name", "device", "chip", "start", "count", "values"});
		Q.Chip = Num(V, "chip", 1);
		if (Q.Chip && !Io.m_Ay2)
		{
			throw std::runtime_error("AY chip 1 is unavailable (requires --ts)");
		}
	}
	else if (Q.Device == "sprite_attrs" || Q.Device == "sprite_pattern")
	{
		if (!Io.m_Sprites)
		{
			throw std::runtime_error("sprite device is unavailable");
		}
		Q.Raw = true;
		if (Q.Device == "sprite_attrs")
		{
			Fields(V, {"op", "name", "device", "start", "count", "hex"});
		}
		else
		{
			Fields(V, {"op", "name", "device", "slot", "offset", "length", "hex"});
			Q.Slot = Num(V, "slot", 63);
		}
	}
	else
	{
		throw std::runtime_error("unknown device: " + Q.Device);
	}
	const bool Pattern = Q.Device == "sprite_pattern";
	const unsigned Limit = Q.Device == "ay" ? 16 : Q.Device == "sprite_attrs" ? 128 : 256;
	const unsigned Stride = Q.Device == "sprite_attrs" ? 5 : Q.Device == "palette" && Q.Raw ? 2 : 1;
	Q.Start = Num(V, Pattern ? "offset" : "start", Limit - 1);
	if (Expect)
	{
		if (V.Find("count") || V.Find("length"))
		{
			throw std::runtime_error("expect_device infers length from expected values");
		}
		if (Q.Raw)
		{
			if (V.Find("values"))
			{
				throw std::runtime_error("raw expectations require hex only");
			}
			const auto S = String(V, "hex");
			int High = -1;
			for (unsigned char C : S)
			{
				if (std::isspace(C))
				{
					continue;
				}
				int N = C >= '0' && C <= '9'   ? C - '0'
						: C >= 'a' && C <= 'f' ? C - 'a' + 10
						: C >= 'A' && C <= 'F' ? C - 'A' + 10
											   : -1;
				if (N < 0)
				{
					throw std::runtime_error("invalid hex byte string");
				}
				if (High < 0)
				{
					High = N;
				}
				else
				{
					Q.Expected.push_back(uint16_t(High * 16 + N));
					High = -1;
				}
			}
			if (High >= 0)
			{
				throw std::runtime_error("hex needs complete byte pairs");
			}
		}
		else
		{
			if (V.Find("hex"))
			{
				throw std::runtime_error("numeric expectations require values only");
			}
			const auto& Values = Required(V, "values");
			if (!Values.IsArray())
			{
				throw std::runtime_error("values must be an array");
			}
			for (const auto& N : Values.m_A)
			{
				Q.Expected.push_back(uint16_t(Number(N, Q.Device == "palette" ? 511 : 255)));
			}
		}
		if (Q.Expected.size() % Stride)
		{
			throw std::runtime_error("expect_device needs whole entries/attribute records");
		}
		Q.Count = unsigned(Q.Expected.size() / Stride);
	}
	else
	{
		if (V.Find("values") || V.Find("hex"))
		{
			throw std::runtime_error("read_device does not accept expectations");
		}
		Q.Count = Num(V, Pattern ? "length" : "count", Limit);
	}
	if (!Q.Count || Q.Count > Limit - Q.Start)
	{
		throw std::runtime_error("device range out of bounds");
	}
	return Q;
}

std::vector<uint16_t> InspectDevice(const DeviceQuery& Q, const IoContext& Io)
{
	std::vector<uint16_t> R;
	for (unsigned I = Q.Start; I < Q.Start + Q.Count; ++I)
	{
		if (Q.Device == "palette")
		{
			const unsigned Layer = Q.Layer == "ula" ? 0 : Q.Layer == "layer2" ? 1 : Q.Layer == "sprites" ? 2 : 3;
			const auto* P = Io.m_Mem.GetPalette(int(Layer + (Q.Bank - 1) * 4))[I];
			if (Q.Raw)
			{
				R.push_back(P[0]);
				R.push_back(P[1]);
			}
			else
			{
				R.push_back(uint16_t((P[0] << 1) | (P[1] & 1)));
			}
		}
		else if (Q.Device == "ay")
		{
			R.push_back((Q.Chip ? Io.m_Ay2 : &Io.m_Ay)->m_Registers[I]);
		}
		else if (Q.Device == "sprite_pattern")
		{
			R.push_back(Io.m_Sprites->m_Slots[Q.Slot].m_Pattern[I]);
		}
		else
		{
			for (auto B : Io.m_Sprites->m_Slots[I].m_Attr.m_Bytes)
			{
				R.push_back(B);
			}
		}
	}
	return R;
}

std::string DeviceSelectorJson(const DeviceQuery& Q)
{
	std::string R = "\"device\":" + Quote(Q.Device);
	if (Q.Device == "palette")
	{
		R += ",\"layer\":" + Quote(Q.Layer) + ",\"bank\":" + std::to_string(Q.Bank) +
			 ",\"encoding\":" + Quote(Q.Encoding);
	}
	if (Q.Device == "ay")
	{
		R += ",\"chip\":" + std::to_string(Q.Chip);
	}
	if (Q.Device == "sprite_pattern")
	{
		return R + ",\"slot\":" + std::to_string(Q.Slot) + ",\"offset\":" + std::to_string(Q.Start) +
			   ",\"length\":" + std::to_string(Q.Count);
	}
	return R + ",\"start\":" + std::to_string(Q.Start) + ",\"count\":" + std::to_string(Q.Count);
}

std::string DeviceResultJson(const DeviceQuery& Q, const IoContext& Io, const std::vector<uint16_t>& Values)
{
	std::string R = '{' + DeviceSelectorJson(Q);
	if (Q.Raw)
	{
		std::vector<uint8_t> Bytes(Values.begin(), Values.end());
		R += ",\"hex\":" + Hex(Bytes.data(), Bytes.size());
	}
	else
	{
		R += ",\"values\":[";
		for (size_t I = 0; I < Values.size(); ++I)
		{
			R += (I ? "," : "") + std::to_string(Values[I]);
		}
		R += ']';
	}
	if (Q.Device == "ay")
	{
		R += ",\"available\":true,\"enabled\":" + std::string(Io.m_bAyEnabled ? "true" : "false") +
			 ",\"latch\":" + std::to_string((Q.Chip ? Io.m_Ay2 : &Io.m_Ay)->m_Latch) +
			 ",\"active_chip\":" + std::to_string(Io.m_AyActive);
	}
	if (Q.Device == "sprite_attrs" || Q.Device == "sprite_pattern")
	{
		const auto& S = *Io.m_Sprites;
		R += ",\"attr_writes\":" + std::to_string(S.m_AttrWrites) +
			 ",\"pattern_writes\":" + std::to_string(S.m_PatternWrites) +
			 ",\"slot_selects\":" + std::to_string(S.m_SlotSelects) + ",\"attr_slot\":" + std::to_string(S.m_AttrSlot) +
			 ",\"attr_byte_idx\":" + std::to_string(S.m_AttrByteIdx) +
			 ",\"pattern_slot\":" + std::to_string(S.m_PatternSlot) +
			 ",\"pattern_byte_idx\":" + std::to_string(S.m_PatternByteIdx);
	}
	return R + '}';
}

std::string InspectionCapabilitiesJson()
{
	return "{\"version\":1,\"tool\":\"inspeccy\",\"tool_version\":\"" INSPECCY_VERSION "\",\"capability_revision\":\"2026-09-20\",\"trace_support\":"
#if INSPECCY_TRACE_SUPPORT
		   "true"
#else
		   "false"
#endif
		   ",\"batch_version\":1,\"batch_operations\":[\"set\",\"write\",\"read\",\"write_value\",\"read_value\","
		   "\"expect_memory\",\"expect_value\",\"expect_regs\",\"nextreg\",\"run\",\"call\",\"repeat\",\"read_regs\","
		   "\"read_state\",\"read_device\",\"expect_device\"],\"inspection_version\":1,\"irq_diagnostics_version\":1,"
		   "\"devices\":[\"palette\",\"sprite_attrs\",\"sprite_pattern\",\"ay\"],\"limits\":{\"stack_words\":64,"
		   "\"pc_bytes\":64,\"irq_events\":4096,\"irq_locations\":256,\"batch_observation_bytes\":16777216}}";
}
