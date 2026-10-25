#include "irq_report.h"
#include "diagnostic_json.h"
#include <trace/sld.h>

using namespace DiagnosticJson;
namespace
{
IrqDiagnostics::Source Source(const std::string& Name)
{
	if (Name == "frame")
	{
		return IrqDiagnostics::Frame;
	}
	if (Name == "line")
	{
		return IrqDiagnostics::Line;
	}
	throw std::runtime_error("IRQ source must be frame or line");
}
const char* SourceName(IrqDiagnostics::Source S)
{
	return S == IrqDiagnostics::Frame ? "frame" : "line";
}
const char* KindName(IrqDiagnostics::Kind K)
{
	return K == IrqDiagnostics::FramePulseExpired  ? "frame_pulse_expired"
		   : K == IrqDiagnostics::LinePulseExpired ? "line_pulse_expired"
												   : "guard_busy_at_accept";
}
std::string EventJson(const IrqDiagnostics::Event& E, const SldSymbolMap& Symbols)
{
	return "{\"kind\":" + Quote(KindName(E.Type)) + ",\"source\":" + Quote(SourceName(E.Interrupt)) +
		   ",\"cycles\":" + std::to_string(E.Cycles) + ",\"emulated_ns\":" + std::to_string(E.Nanos) +
		   ",\"scheduled_cycle\":" +
		   (E.Type == IrqDiagnostics::GuardBusyAtAccept ? "null" : std::to_string(E.Scheduled)) +
		   ",\"deadline_cycle\":" +
		   (E.Type == IrqDiagnostics::GuardBusyAtAccept ? "null" : std::to_string(E.Deadline)) +
		   ",\"batch_step\":" + (E.BatchStep < 0 ? "null" : std::to_string(E.BatchStep)) +
		   ",\"pc\":" + InspectionAddressJson(E.PC, E.PcMapping, Symbols) +
		   ",\"sp\":" + InspectionAddressJson(E.SP, E.SpMapping, Symbols) + ",\"iff1\":" + std::to_string(E.IFF1) +
		   ",\"iff2\":" + std::to_string(E.IFF2) + ",\"im\":" + std::to_string(E.IM) +
		   ",\"ei_pending\":" + (E.EiPending ? "true" : "false") + ",\"halted\":" + (E.Halted ? "true" : "false") +
		   ",\"guard_index\":" + (E.GuardIndex < 0 ? "null" : std::to_string(E.GuardIndex)) +
		   ",\"guard_value\":" + (E.GuardIndex < 0 ? "null" : std::to_string(E.GuardValue)) + '}';
}
} // namespace

void LoadIrqDiagnostics(
	const std::string& Path, const MemoryBus& Mem, const SldSymbolMap& Symbols, bool BoundSymbols, IrqDiagnostics& Out)
{
	const auto Root = Read(Path, 65536);
	Fields(Root, {"version", "sources", "max_events", "guards"});
	if (Num(Root, "version", 1) != 1)
	{
		throw std::runtime_error("IRQ diagnostics version must be 1");
	}
	IrqDiagnostics D;
	if (Root.Find("max_events"))
	{
		D.MaxEvents = Num(Root, "max_events", 4096);
	}
	if (const auto* Sources = Root.Find("sources"))
	{
		if (!Sources->IsArray() || Sources->m_A.empty())
		{
			throw std::runtime_error("sources must be a nonempty array");
		}
		D.Sources = {{false, false}};
		for (const auto& S : Sources->m_A)
		{
			if (!S.IsString())
			{
				throw std::runtime_error("source must be a string");
			}
			const auto Index = Source(S.m_S);
			if (D.Sources[Index])
			{
				throw std::runtime_error("duplicate IRQ source");
			}
			D.Sources[Index] = true;
		}
	}
	if (const auto* Guards = Root.Find("guards"))
	{
		if (!Guards->IsArray() || Guards->m_A.size() > 16)
		{
			throw std::runtime_error("guards must be an array of at most 16 probes");
		}
		std::set<std::string> Names;
		for (const auto& V : Guards->m_A)
		{
			Fields(V, {"name", "source", "page", "offset", "symbol", "type", "idle_value"});
			IrqDiagnostics::Guard G;
			G.Name = String(V, "name");
			if (G.Name.empty() || G.Name.size() > 128 || !Names.insert(G.Name).second)
			{
				throw std::runtime_error("guard names must be unique, 1..128 bytes");
			}
			G.Interrupt = Source(String(V, "source"));
			if (!D.Sources[G.Interrupt])
			{
				throw std::runtime_error("guard source is not selected");
			}
			const auto Type = String(V, "type");
			G.Width = Type == "u8" ? 1 : Type == "u16le" ? 2 : 0;
			if (!G.Width)
			{
				throw std::runtime_error("guard type must be u8 or u16le");
			}
			G.Idle = Num(V, "idle_value", G.Width == 1 ? 255 : 65535);
			if (V.Find("symbol"))
			{
				if (V.Find("page") || V.Find("offset"))
				{
					throw std::runtime_error("guard uses symbol or physical page/offset, not both");
				}
				if (!BoundSymbols)
				{
					throw std::runtime_error("symbolic guard requires --build-manifest");
				}
				SldSymbolMap::Symbol S;
				std::string Error;
				if (!Symbols.Resolve(String(V, "symbol"), S, Error))
				{
					throw std::runtime_error(Error);
				}
				if (S.Page < 0)
				{
					throw std::runtime_error("guard symbol has no physical page; use explicit page/offset");
				}
				G.Page = unsigned(S.Page);
				G.Offset = S.Value % 8192;
			}
			else
			{
				G.Page = Num(V, "page", unsigned(Mem.GetEffectiveRamPages() - 1));
				G.Offset = Num(V, "offset", 8191);
			}
			if (G.Page >= unsigned(Mem.GetEffectiveRamPages()) || G.Offset + G.Width > 8192)
			{
				throw std::runtime_error("guard outside physical RAM page");
			}
			D.Guards.push_back(G);
		}
	}
	D.Events.reserve(D.MaxEvents);
	Out = std::move(D);
}

std::string IrqSummaryJson(const IrqDiagnostics& D)
{
	return "{\"frame_pulse_expired\":" + std::to_string(D.Counts[0]) +
		   ",\"line_pulse_expired\":" + std::to_string(D.Counts[1]) +
		   ",\"guard_busy_at_accept\":" + std::to_string(D.Counts[2]) +
		   ",\"retained_events\":" + std::to_string(D.Events.size()) +
		   ",\"omitted_events\":" + std::to_string(D.OmittedEvents) +
		   ",\"location_overflow_events\":" + std::to_string(D.OverflowLocations) + '}';
}

std::string IrqReportJson(
	const IrqDiagnostics& D, const SldSymbolMap& Symbols, bool BoundSymbols, const InspectionIdentity& Identity)
{
	std::string R =
		"{\"version\":1,\"initial_cycle\":" + std::to_string(D.InitialCycle) +
		",\"identity\":" + InspectionIdentityJson(Identity, BoundSymbols) +
		",\"pc_semantics\":\"location at observed instruction boundary; not an exact deadline culprit\","
		"\"guard_semantics\":\"non-idle memory at acceptance; not proof of a skipped guest update\",\"summary\":" +
		IrqSummaryJson(D) + ",\"sources\":[";
	bool First = true;
	for (unsigned I = 0; I < 2; ++I)
	{
		if (D.Sources[I])
		{
			if (!First)
			{
				R += ',';
			}
			First = false;
			R += Quote(SourceName(IrqDiagnostics::Source(I)));
		}
	}
	R += "],\"guards\":[";
	for (size_t I = 0; I < D.Guards.size(); ++I)
	{
		const auto& G = D.Guards[I];
		if (I)
		{
			R += ',';
		}
		R += "{\"name\":" + Quote(G.Name) + ",\"source\":" + Quote(SourceName(G.Interrupt)) +
			 ",\"page\":" + std::to_string(G.Page) + ",\"offset\":" + std::to_string(G.Offset) +
			 ",\"type\":" + Quote(G.Width == 1 ? "u8" : "u16le") + ",\"idle_value\":" + std::to_string(G.Idle) + '}';
	}
	R += "],\"events\":[";
	for (size_t I = 0; I < D.Events.size(); ++I)
	{
		if (I)
		{
			R += ',';
		}
		R += EventJson(D.Events[I], Symbols);
	}
	R += "],\"locations\":[";
	std::vector<const IrqDiagnostics::Bucket*> Ordered;
	for (const auto& B : D.Locations)
	{
		Ordered.push_back(&B.second);
	}
	std::stable_sort(Ordered.begin(), Ordered.end(),
		[](const auto* A, const auto* B)
		{
			return A->Count > B->Count;
		});
	for (size_t I = 0; I < Ordered.size(); ++I)
	{
		if (I)
		{
			R += ',';
		}
		R += "{\"count\":" + std::to_string(Ordered[I]->Count) + ",\"first\":" + EventJson(Ordered[I]->First, Symbols) +
			 '}';
	}
	return R + "]}";
}
