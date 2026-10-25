#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

// Field-level primitives for sjasmplus .sld.txt files, shared by the trace's
// symbol map (sld.cpp) and the static-analysis index (asm_index.cpp) so the
// two cannot disagree about what a record looks like.
//
// A data line has EIGHT '|'-separated fields:
//   sourceFile | sourceLine | defFile | defLine | page | value | type | data
//     0            1           2         3         4      5       6      7
// The definition-file/line pair (2,3) is easy to overlook - dropping it shifts
// page/value/type/data left by one, so `type` reads as the address and every
// label is silently rejected.
namespace sld
{

// The token at TokenIndex (0-based) into Out; false if the line had fewer
// fields than that.
inline bool SplitField(const std::string& Line, int TokenIndex, std::string& Out)
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

// `$NNNN`, `0xNNNN` or decimal. Returns 0 with OutbOk false on failure, which
// the caller must reject rather than treat as zero.
inline long ParseValue(const std::string& S, bool& OutbOk)
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

// Whole file as lines, CR stripped. False when it cannot be read.
inline bool ReadLines(const std::string& Path, std::vector<std::string>& Out)
{
	FILE* F = fopen(Path.c_str(), "rb");
	if (F == nullptr)
	{
		return false;
	}
	std::string Buf;
	char Chunk[8192];
	size_t Got = 0;
	while ((Got = fread(Chunk, 1, sizeof(Chunk), F)) > 0)
	{
		Buf.append(Chunk, Got);
	}
	fclose(F);
	// A UTF-8 BOM would otherwise become part of the first line's text.
	size_t At = (Buf.size() >= 3 && uint8_t(Buf[0]) == 0xEF && uint8_t(Buf[1]) == 0xBB
					&& uint8_t(Buf[2]) == 0xBF)
		? 3
		: 0;
	Out.clear();
	for (size_t IdxI = At; IdxI <= Buf.size(); ++IdxI)
	{
		if (IdxI == Buf.size() || Buf[IdxI] == '\n')
		{
			if (IdxI == Buf.size() && IdxI == At)
			{
				break;
			}
			std::string Line(Buf, At, IdxI - At);
			if (!Line.empty() && Line.back() == '\r')
			{
				Line.pop_back();
			}
			Out.push_back(Line);
			At = IdxI + 1;
		}
	}
	return true;
}

// The assembling device's page size, off the `Z` record, e.g.
//   main.asm|1||0|-1|-1|Z|pages.size:16384,pages.count:8,...
// 8192 for a Next build, 16384 for ZXSPECTRUM48/128, whose page field is then
// a BANK number and must be expanded to a physical 8 KB page. Defaults to
// 8192 when no Z record is present. Nothing guarantees the Z record precedes
// the labels, so the caller passes every line.
inline long DetectPageSize(const std::vector<std::string>& Lines)
{
	for (const std::string& Line : Lines)
	{
		if (Line.empty() || Line[0] == '|' || Line[0] == ';')
		{
			continue;
		}
		std::string Type, Data;
		if (!SplitField(Line, 6, Type) || Type != "Z" || !SplitField(Line, 7, Data))
		{
			continue;
		}
		const size_t P = Data.find("pages.size:");
		if (P == std::string::npos)
		{
			continue;
		}
		bool bOk = false;
		const long V = ParseValue(Data.substr(P + 11), bOk);
		if (bOk && V > 0)
		{
			return V;
		}
	}
	return 8192;
}

} // namespace sld
