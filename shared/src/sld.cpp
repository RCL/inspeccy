#include <trace/sld.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{

// Splits a line on '|'. Returns the token at TokenIndex (0-based) into Out;
// returns false if the line had fewer fields than required.
bool SplitField(const std::string& Line, int TokenIndex, std::string& Out)
{
	int Count = 0;
	size_t Start = 0;
	for (size_t IdxI = 0; IdxI <= Line.size(); ++IdxI)
	{
		if (IdxI == Line.size() || Line[IdxI] == '|')
		{
			if (Count == TokenIndex)
			{
				Out.assign(Line, Start, IdxI - Start);
				return true;
			}
			++Count;
			Start = IdxI + 1;
		}
	}
	return false;
}

// Parses a value field - may be hex (`$NNNN` or `0xNNNN`) or decimal. Returns
// 0 on parse failure (caller should reject).
long ParseValue(const std::string& S, bool& OutbOk)
{
	OutbOk = false;
	if (S.empty())
	{
		return 0;
	}
	const char* P = S.c_str();
	int Base = 10;
	if (*P == '$')
	{
		++P;
		Base = 16;
	}
	else if (S.size() >= 2 && P[0] == '0' && (P[1] == 'x' || P[1] == 'X'))
	{
		P += 2;
		Base = 16;
	}
	char* End = nullptr;
	const long V = strtol(P, &End, Base);
	if (End == P)
	{
		return 0;
	}
	OutbOk = true;
	return V;
}

// Rejoin an sjasmplus L-record data tuple into a dotted label name.
// The data field is "<scope>,<label>,<local>,<+flag>,..." - we drop the empty
// components and the +keyword flags and join the rest with '.', reproducing
// the fully-qualified form the F record for the same address already carries
// (e.g. ",code_begin,Loop,+used" -> "code_begin.Loop").
std::string CleanLabelName(const std::string& Data)
{
	std::string Out;
	size_t Start = 0;
	for (size_t IdxI = 0; IdxI <= Data.size(); ++IdxI)
	{
		if (IdxI == Data.size() || Data[IdxI] == ',')
		{
			const std::string Tok(Data, Start, IdxI - Start);
			Start = IdxI + 1;
			if (Tok.empty() || Tok[0] == '+')
			{
				continue; // skip empty scope + flag tokens
			}
			if (!Out.empty())
			{
				Out += '.';
			}
			Out += Tok;
		}
	}
	return Out;
}

} // namespace

SldSymbolMap::SldSymbolMap() = default;

bool SldSymbolMap::LoadFile(const char* Path, std::string& OutError)
{
	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		OutError = std::string("sld: cannot open file: ") + Path;
		return false;
	}

	std::vector<char> Buf;
	Buf.reserve(64 * 1024);
	int C;
	while ((C = std::fgetc(F)) != EOF)
	{
		Buf.push_back(char(C));
	}
	fclose(F);

	if (Buf.empty())
	{
		fprintf(stderr, "sld %s: file empty or unreadable\n", Path);
		++m_NumWarnings;
		return true;
	}

	int LinesParsed = 0;
	int LabelsLoaded = 0;

	// Split the buffer into lines and hand each one to Fn. Called twice: once
	// to read the device geometry off the Z record, then once to parse labels
	// with that geometry known. A Z record is emitted where the source's DEVICE
	// directive sits, which is normally line 1 - but nothing in the format
	// guarantees it precedes every label, so we don't rely on the order.
	auto ForEachLine = [&](auto&& Fn)
	{
		size_t LineStart = 0;
		for (size_t IdxI = 0; IdxI < Buf.size(); ++IdxI)
		{
			if (Buf[IdxI] == '\n')
			{
				std::string Line(Buf.data() + LineStart, IdxI - LineStart);
				// Strip trailing CR
				if (!Line.empty() && Line.back() == '\r')
				{
					Line.pop_back();
				}
				Fn(Line);
				LineStart = IdxI + 1;
			}
		}
		if (LineStart < Buf.size())
		{
			std::string Line(Buf.data() + LineStart, Buf.size() - LineStart);
			if (!Line.empty() && Line.back() == '\r')
			{
				Line.pop_back();
			}
			Fn(Line);
		}
	};

	// Device page geometry. sjasmplus emits a `Z` record describing the
	// assembling device, e.g.
	//   main.asm|1||0|-1|-1|Z|pages.size:16384,pages.count:8,slots.count:4,...
	// Inspeccy keys symbols by PHYSICAL 8 KB page, because that is what the
	// emulator's per-instruction page tags carry. A source assembled with
	// DEVICE ZXSPECTRUM128 (or 48) declares 16 KB BANKS, so its page field is a
	// bank number and must be expanded before it can be used as a key.
	// Next builds already use 8 KB pages.
	long SldPageSize = 0;
	ForEachLine(
		[&](const std::string& Line)
		{
			if (SldPageSize != 0 || Line.empty() || Line[0] == '|' || Line[0] == ';')
			{
				return;
			}
			std::string Type;
			if (!SplitField(Line, 6, Type) || Type != "Z")
			{
				return;
			}
			std::string Data;
			if (!SplitField(Line, 7, Data))
			{
				return;
			}
			const size_t P = Data.find("pages.size:");
			if (P == std::string::npos)
			{
				return;
			}
			bool bOk = false;
			// ParseValue stops at the ',' that ends the field's first key:value.
			const long V = ParseValue(Data.substr(P + 11), bOk);
			if (bOk && V > 0)
			{
				SldPageSize = V;
			}
		});
	const bool bB16KBanks = (SldPageSize == 16384);

	auto ParseLine = [&](const std::string& Line)
	{
		if (Line.empty())
		{
			return;
		}
		// sjasmplus header lines start with `|` (e.g. "|SLD.data.version|...").
		if (Line[0] == '|')
		{
			return;
		}
		// Comment lines start with `;` per sjasmplus convention (rare).
		if (Line[0] == ';')
		{
			return;
		}
		++LinesParsed;

		// sjasmplus SLD data line has EIGHT fields:
		//   sourceFile | sourceLine | defFile | defLine | page | value | type | data
		//     0            1           2         3         4      5       6      7
		// The definition-file/line pair (2,3) is easy to overlook - dropping it
		// shifts page/value/type/data left by one, so `type` reads as the address
		// and every label is silently rejected (0 labels loaded).
		std::string Type, PageS, ValS, Data;
		if (!SplitField(Line, 6, Type))
		{
			return;
		}
		if (Type != "L" && Type != "F" && Type != "D")
		{
			return; // L=label record, F=fully-qualified name
		}
		if (!SplitField(Line, 4, PageS))
		{
			return;
		}
		if (!SplitField(Line, 5, ValS))
		{
			return;
		}
		if (!SplitField(Line, 7, Data))
		{
			return;
		}

		bool bOk = false;
		const long Page = ParseValue(PageS, bOk);
		if (!bOk || Page < -1 || Page > 255)
		{
			return;
		}
		const long Val = ParseValue(ValS, bOk);
		if (!bOk || Val < 0 || (Page >= 0 && Val > 0xFFFF))
		{
			return;
		}

		// Bank -> physical 8 KB page. A 16 KB bank B covers 8 KB pages 2B (its
		// low half) and 2B+1 (its high half); bit 13 of the address picks the
		// half, and it does so regardless of which 16 KB slot the bank is
		// mapped into, since every slot base is 16 KB-aligned.
		const long PageKey = Page < 0 ? -1 : bB16KBanks ? (Page * 2 + ((Val >> 13) & 1)) : Page;
		if (PageKey > 255)
		{
			return;
		}

		// Trim trailing whitespace/CR from Data
		while (
			!Data.empty() && (Data.back() == '\r' || Data.back() == '\n' || Data.back() == ' ' || Data.back() == '\t'))
		{
			Data.pop_back();
		}
		if (Data.empty())
		{
			return;
		}

		// F already holds the clean fully-qualified name; L needs rejoining.
		const std::string Name = (Type == "L") ? CleanLabelName(Data) : Data;
		if (Name.empty())
		{
			return;
		}

		auto& Aliases = m_ByName[Name];
		bool Found = false;
		for (const auto& S : Aliases)
		{
			Found |= S.Page == PageKey && S.Value == uint32_t(Val);
		}
		if (!Found)
		{
			Aliases.push_back({int(PageKey), uint32_t(Val)});
		}
		if (PageKey < 0)
		{
			return; // Constants are available by name, never trace addresses.
		}
		if (!Found)
		{
			m_Physical[(uint32_t(PageKey) << 13) | (uint32_t(Val) & 8191)].push_back(
				{Name, uint16_t(Val), 0});
		}
		const uint32_t Key = (uint32_t(uint8_t(PageKey)) << 16) | uint32_t(uint16_t(Val));
		// sjasmplus emits an F (fully-qualified) and an L record per label at the
		// same address. F is authoritative for display, so it overrides an L we
		// saw first; an L only fills a key no F provided. Distinct labels that
		// genuinely share an address (equ aliases) keep first-seen.
		auto It = m_Symbols.find(Key);
		if (It == m_Symbols.end())
		{
			m_Symbols[Key] = Name;
			++LabelsLoaded;
		}
		else if (Type == "F")
		{
			It->second = Name;
		}
	};

	ForEachLine(ParseLine);

	fprintf(stderr, "sld %s: loaded %d labels (%d data lines parsed)\n", Path, LabelsLoaded, LinesParsed);
	if (LabelsLoaded == 0 && LinesParsed > 0)
	{
		fprintf(stderr,
			"sld %s: WARNING - %d data lines parsed but no labels "
			"extracted; not a sjasmplus .sld.txt?\n",
			Path, LinesParsed);
		++m_NumWarnings;
	}
	if (bB16KBanks)
	{
		fprintf(stderr,
			"sld %s: NOTE - assembled with 16 KB pages "
			"(DEVICE ZXSPECTRUM48 / ZXSPECTRUM128); bank numbers "
			"expanded to physical 8 KB pages for lookup.\n",
			Path);
	}
	else if (SldPageSize != 0 && SldPageSize != 8192)
	{
		fprintf(stderr,
			"sld %s: WARNING - device declares %ld-byte pages; "
			"inspeccy only knows 8192 and 16384, so page numbers are "
			"used unconverted and symbols may not resolve.\n",
			Path, SldPageSize);
		++m_NumWarnings;
	}

	++m_NumFilesLoaded;
	return true;
}

bool SldSymbolMap::LookupNearest(uint8_t Page8K, uint16_t Addr, int MaxOffsetBytes, std::string& OutLabel) const
{
	if (m_Symbols.empty())
	{
		return false;
	}

	const uint32_t Key = MakeKey(Page8K, Addr);

	// upper_bound returns the first entry strictly greater than Key.
	auto It = m_Symbols.upper_bound(Key);
	if (It == m_Symbols.begin())
	{
		return false;
	}
	--It;

	// Confirm the candidate is in the same page.
	const uint8_t CandPage = uint8_t(It->first >> 16);
	if (CandPage != Page8K)
	{
		return false;
	}

	const uint16_t CandAddr = uint16_t(It->first & 0xFFFF);
	const int Offset = int(Addr) - int(CandAddr);
	if (Offset < 0 || Offset > MaxOffsetBytes)
	{
		return false;
	}

	if (Offset == 0)
	{
		OutLabel = It->second;
	}
	else
	{
		char Buf[8];
		snprintf(Buf, sizeof(Buf), "+%d", Offset);
		OutLabel = It->second + Buf;
	}
	return true;
}

// Exact lookup deliberately refuses ambiguous names, including symbols from
// different builds loaded together. Aliases at one address remain distinct.
bool SldSymbolMap::Resolve(const std::string& Name, Symbol& Out, std::string& Error) const
{
	const auto It = m_ByName.find(Name);
	if (It == m_ByName.end())
	{
		Error = "unknown symbol: " + Name;
		return false;
	}
	if (It->second.size() != 1)
	{
		Error = "ambiguous symbol: " + Name;
		return false;
	}
	Out = It->second.front();
	return true;
}

std::vector<SldSymbolMap::Annotation> SldSymbolMap::Annotate(
	uint8_t Page, uint16_t Offset, uint16_t MaxDistance, size_t Limit, size_t* Total) const
{
	if (Total)
	{
		*Total = 0;
	}
	const uint32_t Key = (uint32_t(Page) << 13) | (Offset & 8191);
	auto It = m_Physical.upper_bound(Key);
	if (It == m_Physical.begin())
	{
		return {};
	}
	--It;
	if ((It->first >> 13) != Page || Key - It->first > MaxDistance)
	{
		return {};
	}
	if (Total)
	{
		*Total = It->second.size();
	}
	std::vector<Annotation> Result;
	for (const auto& A : It->second)
	{
		if (Result.size() == Limit)
		{
			break;
		}
		Result.push_back({A.Name, A.Address, uint16_t(Key - It->first)});
	}
	return Result;
}
