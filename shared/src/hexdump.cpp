#include "hexdump.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

std::string EncodeBankAsHexdump(const uint8_t* Data, size_t Length, int BankIdx, bool bAllowZeroElision)
{
	if (bAllowZeroElision)
	{
		bool bAllZero = true;
		for (size_t IdxI = 0; IdxI < Length; ++IdxI)
		{
			if (Data[IdxI] != 0)
			{
				bAllZero = false;
				break;
			}
		}
		if (bAllZero)
		{
			char Buf[32];
			snprintf(Buf, sizeof(Buf), "%d:zeros", BankIdx);
			return Buf;
		}
	}

	constexpr int BytesPerLine = 32;
	const size_t NumLines = (Length + BytesPerLine - 1) / BytesPerLine;
	std::string Out;
	// Line ~ 8 (prefix) + 32*3 (bytes) + 3*2 (group separators) ~ 110 chars
	Out.reserve(NumLines * 112);

	char LineBuf[160];
	for (size_t LineIdx = 0; LineIdx < NumLines; ++LineIdx)
	{
		const size_t Offset = LineIdx * BytesPerLine;
		int Pos = snprintf(LineBuf, sizeof(LineBuf), "%d:%04zX ", BankIdx, Offset);

		for (int Col = 0; Col < BytesPerLine; ++Col)
		{
			const bool bGroupStart = (Col % 8) == 0;
			const char* Sep = bGroupStart ? "  " : " ";
			if (Offset + size_t(Col) < Length)
			{
				Pos +=
					snprintf(LineBuf + Pos, sizeof(LineBuf) - size_t(Pos), "%s%02X", Sep, Data[Offset + size_t(Col)]);
			}
			else
			{
				// Should not occur for 16 KB-aligned banks, but guard anyway.
				Pos += snprintf(LineBuf + Pos, sizeof(LineBuf) - size_t(Pos), "%s  ", Sep);
			}
		}

		if (LineIdx > 0)
		{
			Out += '\n';
		}
		Out.append(LineBuf, size_t(Pos));
	}

	return Out;
}

namespace
{
inline int HexNibble(char C)
{
	if (C >= '0' && C <= '9')
	{
		return C - '0';
	}
	if (C >= 'a' && C <= 'f')
	{
		return C - 'a' + 10;
	}
	if (C >= 'A' && C <= 'F')
	{
		return C - 'A' + 10;
	}
	return -1;
}
} // namespace

bool DecodeBankFromHexdump(const std::string& Encoded, uint8_t* Out, size_t Length, std::string& OutError)
{
	// ":zeros" sentinel - accept any prefix before the colon.
	{
		const size_t Colon = Encoded.find(':');
		if (Colon != std::string::npos && Encoded.compare(Colon + 1, std::string::npos, "zeros") == 0)
		{
			memset(Out, 0, Length);
			return true;
		}
	}

	memset(Out, 0, Length);

	size_t Pos = 0;
	while (Pos < Encoded.size())
	{
		// Skip leading whitespace.
		while (Pos < Encoded.size() &&
			   (Encoded[Pos] == ' ' || Encoded[Pos] == '\t' || Encoded[Pos] == '\n' || Encoded[Pos] == '\r'))
		{
			++Pos;
		}
		if (Pos >= Encoded.size())
		{
			break;
		}

		// Expect "<bank>:<offset_hex>"
		size_t Colon = Encoded.find(':', Pos);
		if (Colon == std::string::npos)
		{
			OutError = "hexdump: missing ':' on line header";
			return false;
		}
		Pos = Colon + 1;

		// Parse offset (hex up to next whitespace).
		size_t OffsetStart = Pos;
		while (Pos < Encoded.size() && Encoded[Pos] != ' ' && Encoded[Pos] != '\t' && Encoded[Pos] != '\n' &&
			   Encoded[Pos] != '\r')
		{
			++Pos;
		}
		if (Pos == OffsetStart)
		{
			OutError = "hexdump: empty offset";
			return false;
		}
		size_t Offset = 0;
		for (size_t IdxI = OffsetStart; IdxI < Pos; ++IdxI)
		{
			const int N = HexNibble(Encoded[IdxI]);
			if (N < 0)
			{
				OutError = "hexdump: bad offset digit";
				return false;
			}
			Offset = (Offset << 4) | size_t(N);
		}

		// Read up to 32 hex pairs separated by whitespace (single or multiple spaces).
		for (int B = 0; B < 32; ++B)
		{
			while (Pos < Encoded.size() && (Encoded[Pos] == ' ' || Encoded[Pos] == '\t'))
			{
				++Pos;
			}
			if (Pos >= Encoded.size() || Encoded[Pos] == '\n' || Encoded[Pos] == '\r')
			{
				break;
			}

			if (Pos + 2 > Encoded.size())
			{
				OutError = "hexdump: truncated byte";
				return false;
			}
			const int Hi = HexNibble(Encoded[Pos]);
			const int Lo = HexNibble(Encoded[Pos + 1]);
			if (Hi < 0 || Lo < 0)
			{
				OutError = "hexdump: bad data nibble";
				return false;
			}
			const size_t Dst = Offset + size_t(B);
			if (Dst >= Length)
			{
				OutError = "hexdump: offset out of range";
				return false;
			}
			Out[Dst] = uint8_t((Hi << 4) | Lo);
			Pos += 2;
		}
	}

	return true;
}
