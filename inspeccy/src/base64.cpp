#include "base64.h"

namespace
{
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
							 "abcdefghijklmnopqrstuvwxyz"
							 "0123456789+/";
}

std::string EncodeBase64(const uint8_t* Data, size_t Length)
{
	std::string Out;
	Out.reserve(((Length + 2) / 3) * 4);

	size_t IdxI = 0;
	while (IdxI + 3 <= Length)
	{
		const uint32_t Group =
			(uint32_t(Data[IdxI]) << 16) | (uint32_t(Data[IdxI + 1]) << 8) | uint32_t(Data[IdxI + 2]);
		Out += kAlphabet[(Group >> 18) & 0x3F];
		Out += kAlphabet[(Group >> 12) & 0x3F];
		Out += kAlphabet[(Group >> 6) & 0x3F];
		Out += kAlphabet[(Group >> 0) & 0x3F];
		IdxI += 3;
	}

	if (IdxI < Length)
	{
		const size_t Remaining = Length - IdxI;
		uint32_t Group = uint32_t(Data[IdxI]) << 16;
		if (Remaining == 2)
		{
			Group |= uint32_t(Data[IdxI + 1]) << 8;
		}
		Out += kAlphabet[(Group >> 18) & 0x3F];
		Out += kAlphabet[(Group >> 12) & 0x3F];
		Out += (Remaining == 2) ? kAlphabet[(Group >> 6) & 0x3F] : '=';
		Out += '=';
	}

	return Out;
}

namespace
{
inline int DecodeChar(char C)
{
	if (C >= 'A' && C <= 'Z')
	{
		return C - 'A';
	}
	if (C >= 'a' && C <= 'z')
	{
		return C - 'a' + 26;
	}
	if (C >= '0' && C <= '9')
	{
		return C - '0' + 52;
	}
	if (C == '+')
	{
		return 62;
	}
	if (C == '/')
	{
		return 63;
	}
	return -1;
}
} // namespace

bool DecodeBase64(const std::string& In, std::vector<uint8_t>& Out, std::string& OutError)
{
	Out.clear();
	Out.reserve(In.size() / 4 * 3);

	uint32_t Group = 0;
	int GroupBits = 0;
	int PadCount = 0;
	for (char C : In)
	{
		if (C == ' ' || C == '\t' || C == '\n' || C == '\r')
		{
			continue;
		}
		if (C == '=')
		{
			++PadCount;
			if (PadCount > 2)
			{
				OutError = "base64: too many '=' chars";
				return false;
			}
			// Shift the group anyway to keep alignment.
			Group <<= 6;
			GroupBits += 6;
		}
		else
		{
			if (PadCount > 0)
			{
				OutError = "base64: non-pad char after '='";
				return false;
			}
			const int V = DecodeChar(C);
			if (V < 0)
			{
				OutError = "base64: invalid character";
				return false;
			}
			Group = (Group << 6) | uint32_t(V);
			GroupBits += 6;
		}

		if (GroupBits == 24)
		{
			const int Emit = 3 - PadCount;
			if (Emit >= 1)
			{
				Out.push_back(uint8_t((Group >> 16) & 0xFF));
			}
			if (Emit >= 2)
			{
				Out.push_back(uint8_t((Group >> 8) & 0xFF));
			}
			if (Emit >= 3)
			{
				Out.push_back(uint8_t(Group & 0xFF));
			}
			Group = 0;
			GroupBits = 0;
		}
	}

	if (GroupBits != 0)
	{
		OutError = "base64: incomplete final group";
		return false;
	}
	return true;
}
