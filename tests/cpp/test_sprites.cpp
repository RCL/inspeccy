#include "bmp.h"
#include "memory.h"
#include "sprites.h"

#include <filesystem>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{
int GFailures = 0;

void Check(bool bSuccess, const char* Name)
{
	if (!bSuccess)
	{
		fprintf(stderr, "FAIL: %s\n", Name);
		++GFailures;
	}
}

uint32_t Read32(const uint8_t* Data)
{
	return uint32_t(Data[0]) | (uint32_t(Data[1]) << 8) | (uint32_t(Data[2]) << 16) | (uint32_t(Data[3]) << 24);
}

bool CheckRedRectangle(
	const MemoryBus& Mem, const SpritesState& Sprites, const char* Path, int Left, int Top, int Width, int Height)
{
	std::string Error;
	if (!WriteCompositeBmp(Mem, &Sprites, Path, Error))
	{
		fprintf(stderr, "%s\n", Error.c_str());
		return false;
	}
	FILE* File = fopen(Path, "rb");
	if (!File)
	{
		return false;
	}
	std::vector<uint8_t> Bytes(54 + 320 * 256 * 3);
	const size_t Read = fread(Bytes.data(), 1, Bytes.size(), File);
	fclose(File);
	if (Read != Bytes.size() || Read32(Bytes.data() + 10) != 54 || Read32(Bytes.data() + 18) != 320 ||
		Read32(Bytes.data() + 22) != 256 || Bytes[28] != 24)
	{
		return false;
	}
	for (int Y = 0; Y < 256; ++Y)
	{
		for (int X = 0; X < 320; ++X)
		{
			const size_t Offset = 54 + size_t((255 - Y) * 320 + X) * 3;
			const bool bRed = Bytes[Offset] == 0 && Bytes[Offset + 1] == 0 && Bytes[Offset + 2] == 255;
			const bool bExpected = X >= Left && X < Left + Width && Y >= Top && Y < Top + Height;
			if (bRed != bExpected)
			{
				fprintf(stderr, "red pixel mismatch at (%d,%d)\n", X, Y);
				return false;
			}
		}
	}
	return true;
}
} // namespace

int main(int Argc, char** Argv)
{
	if (Argc != 2)
	{
		return 1;
	}
	auto Mem = std::make_unique<MemoryBus>();
	auto Sprites = std::make_unique<SpritesState>();
	Mem->Map48K();
	Mem->SetIsNext(true);
	Mem->SetSprites(Sprites.get());
	Mem->WriteNextReg(0x15, 3); // Sprites enabled, including the border.
	Mem->WriteNextReg(0x43, 0x20);
	Mem->WriteNextReg(0x40, 1);
	Mem->WriteNextReg(0x44, 0xE0);
	Mem->WriteNextReg(0x44, 0);
	const auto Reset = [&]()
	{
		*Sprites = SpritesState{};
		memset(Sprites->m_Slots[0].m_Pattern, 0xE3, 256);
		Sprites->m_Slots[0].m_Pattern[2 * 16 + 1] = 1;
		Sprites->m_Slots[0].m_Attr = {{64, 64, 0, 0xC0, 0}};
	};

	// Independent expected geometry from the documented attribute fields.
	// https://wiki.specnext.dev/Sprite_Attribute_4_Register
	for (int XPower = 0; XPower < 4; ++XPower)
	{
		for (int YPower = 0; YPower < 4; ++YPower)
		{
			Reset();
			Sprites->m_Slots[0].m_Attr.m_Bytes[4] = uint8_t((XPower << 3) | (YPower << 1));
			Check(CheckRedRectangle(
					  *Mem, *Sprites, Argv[1], 64 + (1 << XPower), 64 + (2 << YPower), 1 << XPower, 1 << YPower),
				"anchor scale fields");
		}
	}
	// Rotation precedes mirroring; these flags belong to attribute 2 in both formats.
	// https://wiki.specnext.dev/Sprites
	for (int Extended = 0; Extended < 2; ++Extended)
	{
		for (int Flags = 0; Flags < 8; ++Flags)
		{
			Reset();
			Sprites->m_Slots[0].m_Attr.m_Bytes[2] = uint8_t(Flags << 1);
			Sprites->m_Slots[0].m_Attr.m_Bytes[3] = uint8_t(0x80 | (Extended << 6));
			int X = (Flags & 1) ? 13 : 1;
			int Y = (Flags & 1) ? 1 : 2;
			if (Flags & 4)
			{
				X = 15 - X;
			}
			if (Flags & 2)
			{
				Y = 15 - Y;
			}
			Check(CheckRedRectangle(*Mem, *Sprites, Argv[1], 64 + X, 64 + Y, 1, 1), "rotation and mirrors");
		}
	}
	Reset();
	Sprites->m_Slots[0].m_Attr.m_Bytes[1] = 255;
	Sprites->m_Slots[0].m_Attr.m_Bytes[4] = 1; // Y=511, so the sprite starts one pixel above the canvas.
	Check(CheckRedRectangle(*Mem, *Sprites, Argv[1], 65, 1, 1, 1), "ninth Y bit and wrap");
	for (int Slot : {0, 1, 63})
	{
		for (int Half = 0; Half < 2; ++Half)
		{
			Reset();
			memset(Sprites->m_Slots[Slot].m_Pattern, 0x33, 256);
			Sprites->m_Slots[Slot].m_Pattern[Half * 128 + 2 * 8] = 0x31;
			Sprites->m_Slots[0].m_Attr.m_Bytes[3] = uint8_t(0xC0 | Slot);
			Sprites->m_Slots[0].m_Attr.m_Bytes[4] = uint8_t(0x80 | (Half << 6));
			Check(CheckRedRectangle(*Mem, *Sprites, Argv[1], 65, 66, 1, 1), "four-bit slot and half");
		}
	}

	// The port and NextReg interfaces are independent unless lockstep is enabled.
	Reset();
	Mem->WriteNextReg(0x09, 0);
	Mem->WriteNextReg(0x34, 7);
	WriteSpriteSlotSelect(*Sprites, 2, *Mem);
	Check(Mem->GetNextReg(0x34) == 7, "port select leaves unlocked NextReg index alone");
	Mem->WriteNextReg(0x35, 19);
	for (int Byte : {10, 11, 0, 0x80, 12, 13, 0, 0x80})
	{
		WriteSpriteAttr(*Sprites, uint8_t(Byte), *Mem);
	}
	Check(Sprites->m_Slots[2].m_Attr.m_Bytes[0] == 10 && Sprites->m_Slots[3].m_Attr.m_Bytes[0] == 12 &&
			  Sprites->m_Slots[7].m_Attr.m_Bytes[0] == 19 && Mem->GetNextReg(0x34) == 7,
		"unlocked streams advance independently");
	Mem->WriteNextReg(0x09, 0x10);
	Mem->WriteNextReg(0x34, 0x85);
	Check(Sprites->m_AttrSlot == 5 && Sprites->m_PatternSlot == 5 && Sprites->m_PatternByteIdx == 128,
		"locked NextReg selects both cursors and pattern half");
	Check(Mem->GetNextReg(0x34) == 5, "NextReg index reads with bit 7 clear");
	Mem->WriteNextReg(0x75, 25);
	Check(Sprites->m_AttrSlot == 6 && Sprites->m_AttrByteIdx == 0 && Sprites->m_PatternSlot == 5 &&
			  Sprites->m_PatternByteIdx == 128,
		"post-increment preserves the pattern cursor");
	WriteSpriteSlotSelect(*Sprites, 63, *Mem);
	for (int Idx = 0; Idx < 256; ++Idx)
	{
		WriteSpritePattern(*Sprites, 0);
	}
	Check(Sprites->m_PatternSlot == 0, "pattern memory wraps at 16 KB");
	return GFailures ? 1 : 0;
}
