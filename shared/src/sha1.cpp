#include "sha1.h"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{
inline uint32_t RotL(uint32_t V, int N)
{
	return (V << N) | (V >> (32 - N));
}

void ProcessBlock(uint32_t H[5], const uint8_t* B)
{
	uint32_t W[80];
	for (int IdxI = 0; IdxI < 16; ++IdxI)
	{
		W[IdxI] = (uint32_t(B[IdxI * 4]) << 24) | (uint32_t(B[IdxI * 4 + 1]) << 16) | (uint32_t(B[IdxI * 4 + 2]) << 8) |
				  uint32_t(B[IdxI * 4 + 3]);
	}
	for (int IdxI = 16; IdxI < 80; ++IdxI)
	{
		W[IdxI] = RotL(W[IdxI - 3] ^ W[IdxI - 8] ^ W[IdxI - 14] ^ W[IdxI - 16], 1);
	}
	uint32_t A = H[0], WordB = H[1], C = H[2], D = H[3], E = H[4];
	for (int IdxI = 0; IdxI < 80; ++IdxI)
	{
		uint32_t F, K;
		if (IdxI < 20)
		{
			F = (WordB & C) | (~WordB & D);
			K = 0x5A827999;
		}
		else if (IdxI < 40)
		{
			F = WordB ^ C ^ D;
			K = 0x6ED9EBA1;
		}
		else if (IdxI < 60)
		{
			F = (WordB & C) | (WordB & D) | (C & D);
			K = 0x8F1BBCDC;
		}
		else
		{
			F = WordB ^ C ^ D;
			K = 0xCA62C1D6;
		}
		const uint32_t Temp = RotL(A, 5) + F + E + K + W[IdxI];
		E = D;
		D = C;
		C = RotL(WordB, 30);
		WordB = A;
		A = Temp;
	}
	H[0] += A;
	H[1] += WordB;
	H[2] += C;
	H[3] += D;
	H[4] += E;
}
} // namespace

std::string Sha1Hex(const uint8_t* Data, size_t Length)
{
	uint32_t H[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

	size_t Off = 0;
	while (Off + 64 <= Length)
	{
		ProcessBlock(H, Data + Off);
		Off += 64;
	}

	// Final block(s): copy the tail, append 0x80, pad zeros, append 64-bit length.
	uint8_t Block[64] = {};
	const size_t Tail = Length - Off;
	memcpy(Block, Data + Off, Tail);
	Block[Tail] = 0x80;

	if (Tail + 1 + 8 > 64)
	{
		// Tail + terminator doesn't leave room for length - spill to a second block.
		ProcessBlock(H, Block);
		memset(Block, 0, sizeof(Block));
	}

	const uint64_t BitLen = uint64_t(Length) * 8;
	for (int IdxI = 0; IdxI < 8; ++IdxI)
	{
		Block[64 - 8 + IdxI] = uint8_t((BitLen >> (56 - IdxI * 8)) & 0xFF);
	}
	ProcessBlock(H, Block);

	char Hex[41];
	snprintf(Hex, sizeof(Hex), "%08x%08x%08x%08x%08x", H[0], H[1], H[2], H[3], H[4]);
	return std::string(Hex, 40);
}

std::string Sha1FileHex(const char* Path)
{
	FILE* F = fopen(Path, "rb");
	if (!F)
	{
		return "";
	}
	fseek(F, 0, SEEK_END);
	const long Sz = ftell(F);
	fseek(F, 0, SEEK_SET);
	if (Sz < 0)
	{
		fclose(F);
		return "";
	}
	std::vector<uint8_t> Buf;
	Buf.resize(size_t(Sz));
	const size_t Got = (Sz > 0) ? fread(Buf.data(), 1, size_t(Sz), F) : 0;
	fclose(F);
	if (Got != size_t(Sz))
	{
		return "";
	}
	return Sha1Hex(Buf.data(), Buf.size());
}
