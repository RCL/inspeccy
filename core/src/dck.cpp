// dck.cpp - see dck.h for the file format.

#include "dck.h"

#include <stdio.h>

namespace Dck
{

namespace
{

constexpr size_t kHeaderSize = 1 + kChunksPerBank; // bank id + 8 kind bytes

bool IsKnownBank(uint8_t Id)
{
	return Id == uint8_t(Bank::kEBankDock) || Id == uint8_t(Bank::kEBankExrom) || Id == uint8_t(Bank::kEBankHome);
}

bool IsKnownKind(uint8_t Id)
{
	return Id <= uint8_t(ChunkKind::kEKindRam);
}

// A .DCK has no magic number, so the only thing that separates it from an
// arbitrary file is that the first block header has to be well formed. Catching
// the common compressed containers by their own magic gives a far better
// message than "unknown page type 139".
bool LooksCompressed(const uint8_t* Data, size_t Size)
{
	if (Size >= 2 && Data[0] == 0x1F && Data[1] == 0x8B)
	{
		return true; // gzip
	}
	if (Size >= 3 && Data[0] == 'B' && Data[1] == 'Z' && Data[2] == 'h')
	{
		return true;
	}
	if (Size >= 4 && Data[0] == 'P' && Data[1] == 'K')
	{
		return true; // zip
	}
	return false;
}

} // namespace

const char* BankName(Bank Bank)
{
	switch (Bank)
	{
	case Bank::kEBankDock:
		return "DOCK";
	case Bank::kEBankExrom:
		return "EXROM";
	case Bank::kEBankHome:
		return "HOME";
	}
	return "?";
}

const Block* Cartridge::FindBank(Bank Bank) const
{
	const Block* Found = nullptr;
	for (const Block& B : m_Blocks)
	{
		if (B.m_Bank == Bank)
		{
			Found = &B;
		}
	}
	return Found;
}

bool Parse(const uint8_t* Data, size_t Size, Cartridge& Out, std::string& OutError)
{
	Out.m_Blocks.clear();
	OutError.clear();

	if (Data == nullptr || Size == 0)
	{
		OutError = "empty file";
		return false;
	}
	if (LooksCompressed(Data, Size))
	{
		OutError = "file looks compressed; decompress it first";
		return false;
	}

	size_t Pos = 0;
	while (Pos < Size)
	{
		if (Size - Pos < kHeaderSize)
		{
			OutError = "truncated block header";
			return false;
		}

		const uint8_t BankId = Data[Pos];
		if (!IsKnownBank(BankId))
		{
			char Buf[96];
			snprintf(Buf, sizeof(Buf), "unknown bank id %u at offset %zu", BankId, Pos);
			OutError = Buf;
			return false;
		}

		Block B;
		B.m_Bank = Bank(BankId);

		// Validate every kind byte and total up the payload before copying any
		// of it, so a malformed file is rejected whole rather than half-loaded.
		size_t Payload = 0;
		for (int IdxI = 0; IdxI < kChunksPerBank; ++IdxI)
		{
			const uint8_t Kind = Data[Pos + 1 + size_t(IdxI)];
			if (!IsKnownKind(Kind))
			{
				char Buf[96];
				snprintf(Buf, sizeof(Buf), "unknown page type %u for chunk %d at offset %zu", Kind, IdxI, Pos);
				OutError = Buf;
				return false;
			}
			B.m_Chunks[IdxI].m_Kind = ChunkKind(Kind);
			if (Kind == uint8_t(ChunkKind::kEKindRom) || Kind == uint8_t(ChunkKind::kEKindRam))
			{
				Payload += kChunkSize;
			}
		}

		if (Size - Pos - kHeaderSize < Payload)
		{
			OutError = "truncated block data";
			return false;
		}

		size_t Src = Pos + kHeaderSize;
		for (int IdxI = 0; IdxI < kChunksPerBank; ++IdxI)
		{
			Chunk& C = B.m_Chunks[IdxI];
			switch (C.m_Kind)
			{
			case ChunkKind::kEKindAbsent:
				break;

			case ChunkKind::kEKindRamBlank:
				// Present and writable but carries no bytes in the file:
				// the machine powers up seeing zeroes there.
				C.m_Data.assign(kChunkSize, 0);
				break;

			case ChunkKind::kEKindRom:
			case ChunkKind::kEKindRam:
				C.m_Data.assign(Data + Src, Data + Src + kChunkSize);
				Src += kChunkSize;
				break;
			}
		}

		Out.m_Blocks.push_back(std::move(B));
		Pos = Src;
	}

	if (Out.m_Blocks.empty())
	{
		OutError = "no blocks";
		return false;
	}
	return true;
}

bool LoadFile(const char* Path, Cartridge& Out, std::string& OutError)
{
	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		OutError = "cannot open file";
		return false;
	}
	fseek(F, 0, SEEK_END);
	const long Len = ftell(F);
	fseek(F, 0, SEEK_SET);
	if (Len <= 0)
	{
		fclose(F);
		OutError = "empty file";
		return false;
	}
	std::vector<uint8_t> Buf(static_cast<size_t>(Len));
	const size_t Got = fread(Buf.data(), 1, Buf.size(), F);
	fclose(F);
	if (Got != Buf.size())
	{
		OutError = "short read";
		return false;
	}
	return Parse(Buf.data(), Buf.size(), Out, OutError);
}

} // namespace Dck
