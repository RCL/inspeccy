#include "bmp.h"

#include "memory.h"
#include "sprites.h"
#include "tilemap.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace
{

constexpr int kCanvasW = 352;
constexpr int kCanvasH = 288;
constexpr int kBorder = 48;
constexpr int kPaperW = 256;
constexpr int kPaperH = 192;
constexpr int kRowStride4bpp = kCanvasW / 2; // 176 bytes; 4-byte-aligned, no padding
constexpr int kImageSize = kRowStride4bpp * kCanvasH;
constexpr int kPaletteBytes = 16 * 4;
constexpr int kFileHeaderSz = 14;
constexpr int kInfoHeaderSz = 40;
constexpr int kPixelOffset = kFileHeaderSz + kInfoHeaderSz + kPaletteBytes;
constexpr int kFileSize = kPixelOffset + kImageSize;

// 16-colour Spectrum palette as BGR0 (BMP convention).
// Indices 0..7 = normal; 8..15 = bright (bright black = same as black on most refs).
constexpr uint8_t kPalette[16 * 4] = {
	// B     G     R     0
	0x00, 0x00, 0x00, 0x00, // 0 black
	0xD7, 0x00, 0x00, 0x00, // 1 blue
	0x00, 0x00, 0xD7, 0x00, // 2 red
	0xD7, 0x00, 0xD7, 0x00, // 3 magenta
	0x00, 0xD7, 0x00, 0x00, // 4 green
	0xD7, 0xD7, 0x00, 0x00, // 5 cyan
	0x00, 0xD7, 0xD7, 0x00, // 6 yellow
	0xD7, 0xD7, 0xD7, 0x00, // 7 white
	0x00, 0x00, 0x00, 0x00, // 8 black (bright)
	0xFF, 0x00, 0x00, 0x00, // 9 blue (bright)
	0x00, 0x00, 0xFF, 0x00, // 10 red (bright)
	0xFF, 0x00, 0xFF, 0x00, // 11 magenta (bright)
	0x00, 0xFF, 0x00, 0x00, // 12 green (bright)
	0xFF, 0xFF, 0x00, 0x00, // 13 cyan (bright)
	0x00, 0xFF, 0xFF, 0x00, // 14 yellow (bright)
	0xFF, 0xFF, 0xFF, 0x00, // 15 white (bright)
};

// Timex (TS/TC 2068/2048 class) render variant: on the SCLD the brightness
// is generated independently of the colour, so "bright black" really is a
// visible dark grey on real Timex hardware (unlike the Sinclair ULA, where
// bright black stays black).  Entry 8 models that.
constexpr uint8_t kPaletteTimex[16 * 4] = {
	// B     G     R     0
	0x00, 0x00, 0x00, 0x00, // 0 black
	0xD7, 0x00, 0x00, 0x00, // 1 blue
	0x00, 0x00, 0xD7, 0x00, // 2 red
	0xD7, 0x00, 0xD7, 0x00, // 3 magenta
	0x00, 0xD7, 0x00, 0x00, // 4 green
	0xD7, 0xD7, 0x00, 0x00, // 5 cyan
	0x00, 0xD7, 0xD7, 0x00, // 6 yellow
	0xD7, 0xD7, 0xD7, 0x00, // 7 white
	0x40, 0x40, 0x40, 0x00, // 8 black (bright) - dark grey on real SCLD, see above
	0xFF, 0x00, 0x00, 0x00, // 9 blue (bright)
	0x00, 0x00, 0xFF, 0x00, // 10 red (bright)
	0xFF, 0x00, 0xFF, 0x00, // 11 magenta (bright)
	0x00, 0xFF, 0x00, 0x00, // 12 green (bright)
	0xFF, 0xFF, 0x00, 0x00, // 13 cyan (bright)
	0x00, 0xFF, 0xFF, 0x00, // 14 yellow (bright)
	0xFF, 0xFF, 0xFF, 0x00, // 15 white (bright)
};

inline void WriteU16LE(uint8_t* P, uint16_t V)
{
	P[0] = uint8_t(V);
	P[1] = uint8_t(V >> 8);
}
inline void WriteU32LE(uint8_t* P, uint32_t V)
{
	P[0] = uint8_t(V);
	P[1] = uint8_t(V >> 8);
	P[2] = uint8_t(V >> 16);
	P[3] = uint8_t(V >> 24);
}

// Write nibble (4-bit palette index) into the bottom-up 4bpp image buffer.
// (CanvasX, CanvasY) are top-down canvas coordinates.
inline void PutPixel(uint8_t* Image, int CanvasX, int CanvasY, uint8_t PalIdx)
{
	const int RowFromBottom = kCanvasH - 1 - CanvasY;
	const int RowOff = RowFromBottom * kRowStride4bpp;
	const int ByteOff = RowOff + (CanvasX >> 1);
	if (CanvasX & 1)
	{
		Image[ByteOff] = uint8_t((Image[ByteOff] & 0xF0) | (PalIdx & 0x0F));
	}
	else
	{
		Image[ByteOff] = uint8_t((Image[ByteOff] & 0x0F) | uint8_t((PalIdx & 0x0F) << 4));
	}
}

} // namespace

bool WriteSpectrumScreenBmp(const MemoryBus& Mem, const char* Path, std::string& OutError)
{
	// Pick the displayed bank: 128K shadow screen if 7FFD bit 3 set, else bank 5.
	// On 48K, GetPort7FFD() is 0 so bank 5 always.
	const uint8_t Port7FFD = Mem.GetPort7FFD();
	const int DisplayedBank = (Port7FFD & 0x08) ? 7 : 5;
	const uint8_t* Vram = Mem.Ram16KBank(DisplayedBank);

	// Border colour applied to entire canvas first.
	const uint8_t BorderIdx = uint8_t(Mem.GetBorderColour() & 0x07);

	uint8_t Image[kImageSize];
	// Pre-fill with border colour packed into both nibbles.
	const uint8_t BorderByte = uint8_t((BorderIdx << 4) | BorderIdx);
	memset(Image, BorderByte, sizeof(Image));

	// Paper area: overlay decoded Spectrum pixels.
	for (int Y = 0; Y < kPaperH; ++Y)
	{
		const int Third = Y / 64;
		const int RowInThird = (Y / 8) % 8;
		const int ScanInRow = Y % 8;
		const int CharRow = Y / 8;

		for (int XByte = 0; XByte < kPaperW / 8; ++XByte)
		{
			const int Addr = (Third << 11) | (ScanInRow << 8) | (RowInThird << 5) | XByte;
			const uint8_t PixelByte = Vram[Addr];

			const uint8_t Attr = Vram[0x1800 + CharRow * 32 + XByte];
			const uint8_t Ink = uint8_t(Attr & 0x07);
			const uint8_t Paper = uint8_t((Attr >> 3) & 0x07);
			const bool bBright = (Attr & 0x40) != 0;
			const uint8_t InkIdx = uint8_t(bBright ? (Ink + 8) : Ink);
			const uint8_t PaperIdx = uint8_t(bBright ? (Paper + 8) : Paper);

			for (int Bit = 0; Bit < 8; ++Bit)
			{
				const bool bSet = ((PixelByte >> (7 - Bit)) & 1) != 0;
				const uint8_t PalIdx = bSet ? InkIdx : PaperIdx;
				const int CanvasX = kBorder + XByte * 8 + Bit;
				const int CanvasY = kBorder + Y;
				PutPixel(Image, CanvasX, CanvasY, PalIdx);
			}
		}
	}

	// BMP headers.
	uint8_t Header[kFileHeaderSz + kInfoHeaderSz] = {};
	// BITMAPFILEHEADER
	Header[0] = 'B';
	Header[1] = 'M';
	WriteU32LE(Header + 2, kFileSize);
	WriteU32LE(Header + 10, kPixelOffset);
	// BITMAPINFOHEADER
	WriteU32LE(Header + 14, kInfoHeaderSz);
	WriteU32LE(Header + 18, kCanvasW);
	WriteU32LE(Header + 22, kCanvasH); // positive -> bottom-up
	WriteU16LE(Header + 26, 1); // planes
	WriteU16LE(Header + 28, 4); // bits per pixel
	WriteU32LE(Header + 30, 0); // BI_RGB (no compression)
	WriteU32LE(Header + 34, kImageSize);
	WriteU32LE(Header + 38, 2835); // x ppm (72 dpi)
	WriteU32LE(Header + 42, 2835); // y ppm
	WriteU32LE(Header + 46, 16); // colours used
	WriteU32LE(Header + 50, 0); // colours important (0 = all)

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open bmp output: ") + Path;
		return false;
	}
	const bool bOk = fwrite(Header, 1, sizeof(Header), F) == sizeof(Header) &&
					 fwrite(kPalette, 1, sizeof(kPalette), F) == sizeof(kPalette) &&
					 fwrite(Image, 1, sizeof(Image), F) == sizeof(Image);
	fclose(F);
	if (!bOk)
	{
		OutError = std::string("short write to bmp output: ") + Path;
		return false;
	}
	return true;
}

// Emit an indexed (one byte per pixel, top-down) buffer as a 4bpp bottom-up BMP
// with the given 16-entry BGRx palette. Shared by all ZX-Evo video modes, whose
// canvas sizes differ (352x288 for the ZX-class modes, wider for the ATM modes).
static bool EmitIndexedBmp(const char* Path, const std::vector<uint8_t>& Pixels, int Width, int Height,
	const uint8_t* Pal, std::string& OutError)
{
	const int Stride = ((Width + 1) / 2 + 3) & ~3;
	std::vector<uint8_t> Image(size_t(Stride) * size_t(Height), 0);
	for (int Y = 0; Y < Height; ++Y)
	{
		const int RowFromBottom = (Height - 1 - Y) * Stride;
		for (int X = 0; X < Width; ++X)
		{
			const uint8_t Idx = uint8_t(Pixels[size_t(Y) * size_t(Width) + size_t(X)] & 0x0F);
			uint8_t& Byte = Image[size_t(RowFromBottom + (X >> 1))];
			if (X & 1)
			{
				Byte = uint8_t((Byte & 0xF0) | Idx);
			}
			else
			{
				Byte = uint8_t((Byte & 0x0F) | (Idx << 4));
			}
		}
	}

	constexpr int FileHdr = 14;
	constexpr int InfoHdr = 40;
	constexpr int PalBytes = 16 * 4;
	const int PixelOffset = FileHdr + InfoHdr + PalBytes;
	const int FileSize = PixelOffset + int(Image.size());
	uint8_t Header[FileHdr + InfoHdr] = {};
	Header[0] = 'B';
	Header[1] = 'M';
	WriteU32LE(Header + 2, uint32_t(FileSize));
	WriteU32LE(Header + 10, uint32_t(PixelOffset));
	WriteU32LE(Header + 14, uint32_t(InfoHdr));
	WriteU32LE(Header + 18, uint32_t(Width));
	WriteU32LE(Header + 22, uint32_t(Height));
	WriteU16LE(Header + 26, 1);
	WriteU16LE(Header + 28, 4);
	WriteU32LE(Header + 34, uint32_t(Image.size()));
	WriteU32LE(Header + 38, 2835);
	WriteU32LE(Header + 42, 2835);
	WriteU32LE(Header + 46, 16);

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open bmp output: ") + Path;
		return false;
	}
	const bool bOk = fwrite(Header, 1, sizeof(Header), F) == sizeof(Header) &&
					 fwrite(Pal, 1, size_t(PalBytes), F) == size_t(PalBytes) &&
					 fwrite(Image.data(), 1, Image.size(), F) == Image.size();
	fclose(F);
	if (!bOk)
	{
		OutError = std::string("short write to bmp output: ") + Path;
		return false;
	}
	return true;
}

// ZX-Evo video mode, decoded from $EFF7 (bit 0 = 4BPP, bit 5 = HWMC) + $xx77[2:0],
// per the BaseConf manual section 7.1. Invalid combinations fall back to standard ZX.
enum EvoVidMode
{
	kEvoVid_StdZx,
	kEvoVid_ZxHwMc,
	kEvoVid_Zx16c,
	kEvoVid_Atm640Mc,
	kEvoVid_Atm320c,
	kEvoVid_AtmText,
	kEvoVid_AtmTextSP,
};

static EvoVidMode EvoVidSelect(uint8_t Eff7, uint8_t Ff77)
{
	const uint8_t Mode = uint8_t(Ff77 & 0x07);
	const bool bB4Bpp = (Eff7 & 0x01) != 0;
	const bool bHwMc = (Eff7 & 0x20) != 0;
	if (Mode == 3)
	{
		if (!bB4Bpp && !bHwMc)
		{
			return kEvoVid_StdZx;
		}
		if (!bB4Bpp && bHwMc)
		{
			return kEvoVid_ZxHwMc;
		}
		if (bB4Bpp && !bHwMc)
		{
			return kEvoVid_Zx16c;
		}
	}
	if (!bB4Bpp && !bHwMc)
	{
		if (Mode == 2)
		{
			return kEvoVid_Atm640Mc;
		}
		if (Mode == 0)
		{
			return kEvoVid_Atm320c;
		}
		if (Mode == 6)
		{
			return kEvoVid_AtmText;
		}
		if (Mode == 7)
		{
			return kEvoVid_AtmTextSP;
		}
	}
	return kEvoVid_StdZx;
}

// ZX Evolution BaseConf screen encoder. Dispatches the BaseConf video modes off
// $EFF7 + $xx77 (section 7.1) and renders each per its documented addressing
// (section 7.4): standard ZX (8x8), ZX hardware-multicolour (8x1 attr at
// pixel+$2000 - also confirmed against UnrealSpeccy draw_pmc), ZX 256x192 16c,
// ATM 320x200 16c, ATM 640x200 hardware-multicolour. Pages follow $7FFD bit 3
// (5->7 / 4->6 / 1->3). The two ATM text modes need the reset-service font ROM
// (not modelled), so they fall back to the standard ZX screen. Programmatic
// (beam-timed) palette is a follow-on; the reset-default ZX 16-colour palette is used.
bool WriteZxEvoScreenBmp(const MemoryBus& Mem, const char* Path, std::string& OutError)
{
	const uint8_t Port7FFD = Mem.GetPort7FFD();
	const int ShadowOffset = (Port7FFD & 0x08) != 0 ? 2 : 0; // 7FFD bit 3: page 5->7 etc.
	const EvoVidMode Mode = EvoVidSelect(Mem.GetEff7(), Mem.GetEvoFf77());
	const uint8_t BorderIdx = uint8_t(Mem.GetBorderColour() & 0x0F);

	auto Page = [&](int Base) -> const uint8_t*
	{
		return Mem.Ram16KBank(Base + ShadowOffset);
	};
	// The two 4-bit palette indices packed in a 16c pixel byte: {b[6],b[2:0]} and
	// {b[7],b[5:3]} (BaseConf 7.4.3 / 7.4.5).
	auto Lo4 = [](uint8_t Byte) -> uint8_t
	{
		return uint8_t((((Byte >> 6) & 1) << 3) | (Byte & 0x07));
	};
	auto Hi4 = [](uint8_t Byte) -> uint8_t
	{
		return uint8_t((((Byte >> 7) & 1) << 3) | ((Byte >> 3) & 0x07));
	};

	int PaperW = 256;
	int PaperH = 192;
	if (Mode == kEvoVid_Atm640Mc)
	{
		PaperW = 640;
		PaperH = 200;
	}
	else if (Mode == kEvoVid_Atm320c)
	{
		PaperW = 320;
		PaperH = 200;
	}

	const int Width = PaperW + 2 * kBorder;
	const int Height = PaperH + 2 * kBorder;
	std::vector<uint8_t> Pixels(size_t(Width) * size_t(Height), BorderIdx);
	auto Put = [&](int PaperX, int PaperY, uint8_t Idx)
	{
		Pixels[size_t(kBorder + PaperY) * size_t(Width) + size_t(kBorder + PaperX)] = uint8_t(Idx & 0x0F);
	};

	if (Mode == kEvoVid_StdZx || Mode == kEvoVid_ZxHwMc || Mode == kEvoVid_AtmText || Mode == kEvoVid_AtmTextSP)
	{
		// Standard ZX 8x8 or hardware-multicolour 8x1. The two text modes need the
		// reset-service font ROM (not modelled), so they fall back to standard ZX.
		const uint8_t* Vram = Page(5);
		const bool bHwMc = (Mode == kEvoVid_ZxHwMc);
		for (int Y = 0; Y < 192; ++Y)
		{
			const int Third = Y / 64;
			const int RowInThird = (Y / 8) % 8;
			const int ScanInRow = Y % 8;
			const int CharRow = Y / 8;
			for (int XByte = 0; XByte < 32; ++XByte)
			{
				const int Addr = (Third << 11) | (ScanInRow << 8) | (RowInThird << 5) | XByte;
				const uint8_t PixelByte = Vram[Addr];
				const uint8_t Attr = bHwMc ? Vram[0x2000 + Addr] : Vram[0x1800 + CharRow * 32 + XByte];
				const uint8_t Ink = uint8_t(Attr & 0x07);
				const uint8_t Paper = uint8_t((Attr >> 3) & 0x07);
				const bool bBright = (Attr & 0x40) != 0;
				const uint8_t InkIdx = uint8_t(bBright ? (Ink + 8) : Ink);
				const uint8_t PaperIdx = uint8_t(bBright ? (Paper + 8) : Paper);
				for (int Bit = 0; Bit < 8; ++Bit)
				{
					const bool bSet = ((PixelByte >> (7 - Bit)) & 1) != 0;
					Put(XByte * 8 + Bit, Y, bSet ? InkIdx : PaperIdx);
				}
			}
		}
	}
	else if (Mode == kEvoVid_Zx16c)
	{
		// ZX 256x192 16-colour: 4 bytes/block from pages 4 and 5 at the standard ZX
		// pixel offset and +$2000, each byte giving two 4-bit pixels (7.4.3).
		const uint8_t* Page4 = Page(4);
		const uint8_t* Page5 = Page(5);
		for (int Y = 0; Y < 192; ++Y)
		{
			const int Third = Y / 64;
			const int RowInThird = (Y / 8) % 8;
			const int ScanInRow = Y % 8;
			for (int XByte = 0; XByte < 32; ++XByte)
			{
				const int Addr = (Third << 11) | (ScanInRow << 8) | (RowInThird << 5) | XByte;
				const uint8_t Bytes[4] = {Page4[Addr], Page5[Addr], Page4[0x2000 + Addr], Page5[0x2000 + Addr]};
				for (int IdxByte = 0; IdxByte < 4; ++IdxByte)
				{
					Put(XByte * 8 + IdxByte * 2, Y, Lo4(Bytes[IdxByte]));
					Put(XByte * 8 + IdxByte * 2 + 1, Y, Hi4(Bytes[IdxByte]));
				}
			}
		}
	}
	else if (Mode == kEvoVid_Atm320c)
	{
		// ATM 320x200 16-colour: 8x1 blocks, 4 bytes from pages 1 and 5 (7.4.5).
		const uint8_t* Page1 = Page(1);
		const uint8_t* Page5 = Page(5);
		for (int Y = 0; Y < 200; ++Y)
		{
			for (int BlockX = 0; BlockX < 40; ++BlockX)
			{
				const int Offset = BlockX + Y * 40;
				const uint8_t Bytes[4] = {Page1[Offset], Page5[Offset], Page1[0x2000 + Offset], Page5[0x2000 + Offset]};
				for (int IdxByte = 0; IdxByte < 4; ++IdxByte)
				{
					Put(BlockX * 8 + IdxByte * 2, Y, Lo4(Bytes[IdxByte]));
					Put(BlockX * 8 + IdxByte * 2 + 1, Y, Hi4(Bytes[IdxByte]));
				}
			}
		}
	}
	else if (Mode == kEvoVid_Atm640Mc)
	{
		// ATM 640x200 hardware-multicolour: 16x1 blocks; the left and right 8 pixels
		// each have a pixel byte (page 5) and an attribute byte (page 1) (7.4.4).
		const uint8_t* Page1 = Page(1);
		const uint8_t* Page5 = Page(5);
		for (int Y = 0; Y < 200; ++Y)
		{
			for (int BlockX = 0; BlockX < 40; ++BlockX)
			{
				const int Offset = BlockX + Y * 40;
				const uint8_t AttrLeft = Page1[Offset];
				const uint8_t PixLeft = Page5[Offset];
				const uint8_t AttrRight = Page1[0x2000 + Offset];
				const uint8_t PixRight = Page5[0x2000 + Offset];
				const uint8_t InkLeft = Lo4(AttrLeft);
				const uint8_t PaperLeft = Hi4(AttrLeft);
				const uint8_t InkRight = Lo4(AttrRight);
				const uint8_t PaperRight = Hi4(AttrRight);
				for (int Bit = 0; Bit < 8; ++Bit)
				{
					Put(BlockX * 16 + Bit, Y, ((PixLeft >> (7 - Bit)) & 1) ? InkLeft : PaperLeft);
				}
				for (int Bit = 0; Bit < 8; ++Bit)
				{
					Put(BlockX * 16 + 8 + Bit, Y, ((PixRight >> (7 - Bit)) & 1) ? InkRight : PaperRight);
				}
			}
		}
	}

	return EmitIndexedBmp(Path, Pixels, Width, Height, kPalette, OutError);
}

// Timex SCLD screen encoder. Dispatches the four SCLD video modes off port $FF
// (Mem.GetScldMode()): standard 8x8 (screen 1 $4000 or screen 2 $6000), extended
// 8x1 colour (per-scanline attributes from $6000), and 512x192 hi-res (two display
// files interleaved, 2 colours from $FF bits 3-5). Mode decode + memory layout
// confirmed against the local Timex demos (example-hires.asm / example-multicolor.asm).
// 4bpp BMP with the 16-colour Spectrum palette; canvas width is runtime (hi-res
// is 512-wide). 48px border like WriteSpectrumScreenBmp.
bool WriteTimexScreenBmp(const MemoryBus& Mem, const char* Path, std::string& OutError)
{
	const uint8_t Mode = Mem.GetScldMode();
	const bool bHires = (Mode & 0x04) != 0;
	const bool bExtColour = !bHires && (Mode & 0x02) != 0;
	const bool bScreen2 = !bHires && !bExtColour && (Mode & 0x01) != 0;
	const uint8_t* Vram = Mem.Ram16KBank(5); // Timex screen lives in bank 5
	const int Base = bScreen2 ? 0x2000 : 0x0000; // standard screen-2 select

	const uint8_t BorderIdx = uint8_t(Mem.GetBorderColour() & 0x07);
	constexpr int Border = 48;
	const int PaperW = bHires ? 512 : 256;
	const int CanvasW = PaperW + 2 * Border;
	const int CanvasH = 192 + 2 * Border;
	const int Stride = ((CanvasW * 4 + 31) / 32) * 4; // 4bpp, 4-byte aligned rows
	std::vector<uint8_t> Image(size_t(Stride) * size_t(CanvasH), uint8_t((BorderIdx << 4) | BorderIdx));

	auto Put = [&](int Cx, int Cy, uint8_t Idx)
	{
		const int Off = (CanvasH - 1 - Cy) * Stride + (Cx >> 1);
		if (Cx & 1)
		{
			Image[size_t(Off)] = uint8_t((Image[size_t(Off)] & 0xF0) | (Idx & 0x0F));
		}
		else
		{
			Image[size_t(Off)] = uint8_t((Image[size_t(Off)] & 0x0F) | uint8_t((Idx & 0x0F) << 4));
		}
	};

	if (bHires)
	{
		// 512x192: for each byte column, 8 pixels from file 1 ($4000+Addr) then 8
		// from file 2 ($6000+Addr). 2 colours: bits 3-5 = paper, ink = complement.
		const uint8_t Col = uint8_t((Mode >> 3) & 0x07);
		const uint8_t PaperI = Col, InkI = uint8_t(Col ^ 0x07);
		for (int Y = 0; Y < 192; ++Y)
		{
			const int Third = Y / 64, RowInThird = (Y / 8) % 8, ScanInRow = Y % 8;
			for (int XB = 0; XB < 32; ++XB)
			{
				const int Addr = (Third << 11) | (ScanInRow << 8) | (RowInThird << 5) | XB;
				const uint8_t kB1 = Vram[Addr], kB2 = Vram[0x2000 + Addr];
				for (int B = 0; B < 8; ++B)
				{
					Put(Border + XB * 16 + B, Border + Y, ((kB1 >> (7 - B)) & 1) ? InkI : PaperI);
				}
				for (int B = 0; B < 8; ++B)
				{
					Put(Border + XB * 16 + 8 + B, Border + Y, ((kB2 >> (7 - B)) & 1) ? InkI : PaperI);
				}
			}
		}
	}
	else
	{
		for (int Y = 0; Y < 192; ++Y)
		{
			const int Third = Y / 64, RowInThird = (Y / 8) % 8, ScanInRow = Y % 8, CharRow = Y / 8;
			for (int XB = 0; XB < 32; ++XB)
			{
				const int Addr = (Third << 11) | (ScanInRow << 8) | (RowInThird << 5) | XB;
				const uint8_t Pix = Vram[Base + Addr];
				// 8x1 extended colour: attribute per scanline at pixel-addr + $2000.
				const uint8_t Attr = bExtColour ? Vram[0x2000 + Addr] : Vram[Base + 0x1800 + CharRow * 32 + XB];
				const bool bBr = (Attr & 0x40) != 0;
				const uint8_t InkI = uint8_t((Attr & 0x07) + (bBr ? 8 : 0));
				const uint8_t PaperI = uint8_t(((Attr >> 3) & 0x07) + (bBr ? 8 : 0));
				for (int B = 0; B < 8; ++B)
				{
					Put(Border + XB * 8 + B, Border + Y, ((Pix >> (7 - B)) & 1) ? InkI : PaperI);
				}
			}
		}
	}

	constexpr int FileHdr = 14, InfoHdr = 40, PalBytes = 16 * 4;
	const int PixelOffset = FileHdr + InfoHdr + PalBytes;
	const int FileSize = PixelOffset + int(Image.size());
	uint8_t Header[FileHdr + InfoHdr] = {};
	Header[0] = 'B';
	Header[1] = 'M';
	WriteU32LE(Header + 2, uint32_t(FileSize));
	WriteU32LE(Header + 10, uint32_t(PixelOffset));
	WriteU32LE(Header + 14, uint32_t(InfoHdr));
	WriteU32LE(Header + 18, uint32_t(CanvasW));
	WriteU32LE(Header + 22, uint32_t(CanvasH));
	WriteU16LE(Header + 26, 1);
	WriteU16LE(Header + 28, 4);
	WriteU32LE(Header + 34, uint32_t(Image.size()));
	WriteU32LE(Header + 38, 2835);
	WriteU32LE(Header + 42, 2835);
	WriteU32LE(Header + 46, 16);

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open bmp output: ") + Path;
		return false;
	}
	const bool bOk = fwrite(Header, 1, sizeof(Header), F) == sizeof(Header) &&
					 fwrite(kPaletteTimex, 1, sizeof(kPaletteTimex), F) == sizeof(kPaletteTimex) &&
					 fwrite(Image.data(), 1, Image.size(), F) == Image.size();
	fclose(F);
	if (!bOk)
	{
		OutError = std::string("short write to bmp output: ") + Path;
		return false;
	}
	return true;
}

// ============================================================================
// ZX Next Layer 2 BMP encoder. NXI-compatible layout
// byte-for-byte so the headless emulator's outputs are diffable against the
// reference Python encoder.
// ============================================================================

namespace
{

// Replicate a 3-bit colour component to 8 bits: 0..7 -> 0..255, matching the
// standard 3-bit colour expansion.
inline uint8_t Expand3Bit(uint8_t V)
{
	return uint8_t((V << 5) | (V << 2) | (V >> 1));
}

// Decode one 9-bit palette entry (Hi = RRRGGGBB, Lo bit 0 = 9th blue bit)
// into 24-bit RGB. Bit ordering verified against the reference NXI decode.
struct Rgb888
{
	uint8_t m_R, m_G, m_B;
};
inline Rgb888 DecodeNxEntry(uint8_t Hi, uint8_t Lo)
{
	const uint8_t kR3 = uint8_t((Hi >> 5) & 0x07);
	const uint8_t kG3 = uint8_t((Hi >> 2) & 0x07);
	const uint8_t kB3 = uint8_t(((Hi & 0x03) << 1) | (Lo & 0x01));
	return {Expand3Bit(kR3), Expand3Bit(kG3), Expand3Bit(kB3)};
}

// NR_43 bit 2 selects the displayed Layer 2 palette: bank 1 or bank 5.
inline int ActiveL2PaletteIndex(const MemoryBus& Mem)
{
	return (Mem.GetNextReg(0x43) & 0x04) ? 5 : 1;
}

// NR $43 bit 1 picks which of the two ULA palettes is DISPLAYED (0 = ULA-1,
// 4 = ULA-2); bits 6:4 pick which one palette WRITES go to. A program that
// writes one and displays the other sees none of its own colours - which was
// invisible here while the display path hardcoded ULA-1.
inline int ActiveUlaPaletteIndex(const MemoryBus& Mem)
{
	return (Mem.GetNextReg(0x43) & 0x02) ? 4 : 0;
}

// Pack a row of width 8bpp indices into a BMP scanline (row_padded bytes,
// 4-byte aligned). Source rows are top-down; BMP storage is bottom-up so
// the caller writes rows in reverse.
inline int RowPadded8Bpp(int Width)
{
	return (Width + 3) & ~3;
}

bool WriteL2Bmp8Bpp(const MemoryBus& Mem, int Width, int Height, const std::vector<uint8_t>& RowMajorIndices,
	const char* Path, std::string& OutError)
{
	// Build BGRA palette from the active L2 9-bit palette.
	const int PalIdx = ActiveL2PaletteIndex(Mem);
	const uint8_t(*Pal)[2] = Mem.GetPalette(PalIdx);
	uint8_t PaletteBytes[256 * 4];
	for (int IdxI = 0; IdxI < 256; ++IdxI)
	{
		const Rgb888 Rgb = DecodeNxEntry(Pal[IdxI][0], Pal[IdxI][1]);
		PaletteBytes[IdxI * 4 + 0] = Rgb.m_B;
		PaletteBytes[IdxI * 4 + 1] = Rgb.m_G;
		PaletteBytes[IdxI * 4 + 2] = Rgb.m_R;
		PaletteBytes[IdxI * 4 + 3] = 0;
	}

	const int RowPadded = RowPadded8Bpp(Width);
	const int ImageSize = RowPadded * Height;
	std::vector<uint8_t> Pixels(size_t(ImageSize), 0);
	// Source is top-down (row 0 first). BMP wants bottom-up.
	for (int R = 0; R < Height; ++R)
	{
		const int SrcRow = (Height - 1 - R); // bottom-up
		memcpy(&Pixels[size_t(R) * size_t(RowPadded)], &RowMajorIndices[size_t(SrcRow) * size_t(Width)], size_t(Width));
	}

	constexpr int FileHeader = 14;
	constexpr int InfoHeader = 40;
	constexpr int PalSize = 256 * 4;
	const int PixelOffset = FileHeader + InfoHeader + PalSize;
	const int FileSize = PixelOffset + ImageSize;

	uint8_t Header[FileHeader + InfoHeader] = {};
	Header[0] = 'B';
	Header[1] = 'M';
	WriteU32LE(Header + 2, uint32_t(FileSize));
	WriteU32LE(Header + 10, uint32_t(PixelOffset));
	WriteU32LE(Header + 14, uint32_t(InfoHeader));
	WriteU32LE(Header + 18, uint32_t(Width));
	WriteU32LE(Header + 22, uint32_t(Height)); // positive -> bottom-up
	WriteU16LE(Header + 26, 1);
	WriteU16LE(Header + 28, 8); // 8 bpp
	WriteU32LE(Header + 30, 0); // BI_RGB
	WriteU32LE(Header + 34, uint32_t(ImageSize));
	WriteU32LE(Header + 38, 2835);
	WriteU32LE(Header + 42, 2835);
	WriteU32LE(Header + 46, 256);
	WriteU32LE(Header + 50, 0);

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open bmp output: ") + Path;
		return false;
	}
	const bool bOk = fwrite(Header, 1, sizeof(Header), F) == sizeof(Header) &&
					 fwrite(PaletteBytes, 1, sizeof(PaletteBytes), F) == sizeof(PaletteBytes) &&
					 fwrite(Pixels.data(), 1, Pixels.size(), F) == Pixels.size();
	fclose(F);
	if (!bOk)
	{
		OutError = std::string("short write to bmp output: ") + Path;
		return false;
	}
	return true;
}

// Collect a contiguous run of L2 framebuffer bytes from RAM, starting at
// NR_L2_BANK (16K-bank index) and walking forward. The L2 hardware addresses
// L2 RAM via 16K bank pointers; 8K page index = 2 * 16K_bank. Page index is
// masked to RAM_PAGE_COUNT-1 to wrap (RAM_PAGE_COUNT must be a power of 2).
std::vector<uint8_t> CollectL2Bytes(const MemoryBus& Mem, int ByteCount)
{
	const int L2Bank16K = Mem.GetNextReg(0x12);
	const int BasePage8K = (L2Bank16K * 2) & (kRamPageCount - 1);
	std::vector<uint8_t> Out;
	Out.resize(size_t(ByteCount));
	for (int IdxI = 0; IdxI < ByteCount; ++IdxI)
	{
		const int PageIdx = (BasePage8K + (IdxI / kSlotSize)) & (kRamPageCount - 1);
		const int Offset = IdxI % kSlotSize;
		Out[size_t(IdxI)] = Mem.Ram8KPage(PageIdx)[Offset];
	}
	return Out;
}

// Column-major to row-major (each column = `Height` contiguous bytes).
// Standard column-major to row-major transpose.
std::vector<uint8_t> ColumnToRowMajor(const std::vector<uint8_t>& Data, int Width, int Height)
{
	std::vector<uint8_t> Out(size_t(Width) * size_t(Height), 0);
	for (int C = 0; C < Width; ++C)
	{
		const int ColBase = C * Height;
		for (int R = 0; R < Height; ++R)
		{
			Out[size_t(R) * size_t(Width) + size_t(C)] = Data[size_t(ColBase + R)];
		}
	}
	return Out;
}

// Layer 2 640x256x16: each byte holds two pixels (top nibble = left, low =
// right). 81920 bytes = 320 chunks of 256 bytes; each chunk spans two screen
// columns. Standard 640-mode 4bpp unpack.
std::vector<uint8_t> Unpack640_4Bpp(const std::vector<uint8_t>& Data)
{
	constexpr int Width = 640, Height = 256;
	std::vector<uint8_t> Out(size_t(Width) * size_t(Height), 0);
	for (int Chunk = 0; Chunk < 320; ++Chunk)
	{
		const int ChunkBase = Chunk * 256;
		for (int R = 0; R < 256; ++R)
		{
			const uint8_t B = Data[size_t(ChunkBase + R)];
			const int RowBase = R * Width;
			Out[size_t(RowBase + Chunk * 2 + 0)] = uint8_t((B >> 4) & 0x0F);
			Out[size_t(RowBase + Chunk * 2 + 1)] = uint8_t(B & 0x0F);
		}
	}
	return Out;
}

} // namespace

bool WriteNextLayer2Bmp(const MemoryBus& Mem, const char* Path, std::string& OutError)
{
	const MemoryBus::ActiveLayer Layer = Mem.DetectActiveLayer();

	// L2_256 (NR_70 mode 0) storage is ROW-MAJOR: 192 rows of 256 bytes.
	//   Per Z80N PIXELAD: HL = L2_base + Y*256 + X, so pixel (x, y) is
	//   at byte offset y*256 + x. Read directly, no transposition.
	// L2_320 (mode 1) storage is COLUMN-MAJOR: 320 columns of 256 bytes
	//   (matches the NXI file layout; the dot-command tests pin against this).
	// L2_640 (mode 2) is 640-wide x 256-tall, 4bpp packed; storage is
	//   also column-major (256 bytes per column x 320 chunks of 2 pixels).
	if (Layer == MemoryBus::ActiveLayer::kActiveLayerL2256)
	{
		constexpr int W = 256, H = 192;
		const std::vector<uint8_t> Raw = CollectL2Bytes(Mem, W * H);
		// Already row-major - just hand it to the encoder verbatim.
		return WriteL2Bmp8Bpp(Mem, W, H, Raw, Path, OutError);
	}

	int W = 0, H = 0;
	bool bB4Bpp = false;
	switch (Layer)
	{
	case MemoryBus::ActiveLayer::kActiveLayerL2320:
		W = 320;
		H = 256;
		break;
	case MemoryBus::ActiveLayer::kActiveLayerL2640:
		W = 640;
		H = 256;
		bB4Bpp = true;
		break;
	default:
		OutError = "WriteNextLayer2Bmp: active layer is not L2";
		return false;
	}
	const int RawSize = bB4Bpp ? 81920 : (W * H);
	const std::vector<uint8_t> Raw = CollectL2Bytes(Mem, RawSize);
	const std::vector<uint8_t> RowMajor = bB4Bpp ? Unpack640_4Bpp(Raw) : ColumnToRowMajor(Raw, W, H);
	return WriteL2Bmp8Bpp(Mem, W, H, RowMajor, Path, OutError);
}

// ============================================================================
// ZX Next 4bpp LoRes BMP encoder (128x96, 6144 bytes, high nibble = left).
// NR_15 bit 7 enables LoRes; NR_6A bit 5 selects the 4bpp variant, whose
// display file can be either half of physical bank 5. The low 4 bits hold a palette offset added
// to each 4-bit pixel index before palette lookup. Colours come from
// palette 0 (ULA-1) entries 0..255 decoded with the same 9-bit format the
// L2 encoder uses.
// ============================================================================

bool WriteNextLoResBmp(const MemoryBus& Mem, const char* Path, std::string& OutError)
{
	constexpr int LoresW = 128, LoresH = 96, LoresBytes = LoresW * LoresH / 2;

	// The video fetch uses physical bank 5, independently of CPU MMU slots.
	// In 4bpp mode NR $6A bit 4 XOR port $FF bit 0 selects the display file.
	const int DisplayOffset = (((Mem.GetNextReg(0x6A) >> 4) ^ Mem.GetScldMode()) & 1) * 0x2000;
	const uint8_t* Vram = Mem.Ram16KBank(5) + DisplayOffset;

	// Decode each byte into two 8bpp indices.
	std::vector<uint8_t> RowMajor(size_t(LoresW) * size_t(LoresH), 0);
	const uint8_t PalOffset = uint8_t(Mem.GetNextReg(0x6A) & 0x0F);
	for (int IdxI = 0; IdxI < LoresBytes; ++IdxI)
	{
		const uint8_t B = Vram[IdxI];
		const uint8_t Left = uint8_t(((B >> 4) & 0x0F) + PalOffset);
		const uint8_t Right = uint8_t((B & 0x0F) + PalOffset);
		const int Y = (IdxI / (LoresW / 2));
		const int XByte = IdxI % (LoresW / 2);
		RowMajor[size_t(Y) * size_t(LoresW) + size_t(XByte * 2 + 0)] = Left;
		RowMajor[size_t(Y) * size_t(LoresW) + size_t(XByte * 2 + 1)] = Right;
	}

	// Build BGRA palette from the active ULA palette. LoRes only reads the
	// first 256 entries; many will be zero in practice but encode all 256
	// so any palette offset works.
	const uint8_t(*Pal)[2] = Mem.GetPalette(ActiveUlaPaletteIndex(Mem));
	uint8_t PaletteBytes[256 * 4];
	for (int IdxI = 0; IdxI < 256; ++IdxI)
	{
		const Rgb888 Rgb = DecodeNxEntry(Pal[IdxI][0], Pal[IdxI][1]);
		PaletteBytes[IdxI * 4 + 0] = Rgb.m_B;
		PaletteBytes[IdxI * 4 + 1] = Rgb.m_G;
		PaletteBytes[IdxI * 4 + 2] = Rgb.m_R;
		PaletteBytes[IdxI * 4 + 3] = 0;
	}

	const int RowPadded = RowPadded8Bpp(LoresW);
	const int ImageSize = RowPadded * LoresH;
	std::vector<uint8_t> Pixels(size_t(ImageSize), 0);
	for (int R = 0; R < LoresH; ++R)
	{
		const int SrcRow = (LoresH - 1 - R);
		memcpy(&Pixels[size_t(R) * size_t(RowPadded)], &RowMajor[size_t(SrcRow) * size_t(LoresW)], size_t(LoresW));
	}

	constexpr int FileHeader = 14;
	constexpr int InfoHeader = 40;
	constexpr int PalSize = 256 * 4;
	const int PixelOffset = FileHeader + InfoHeader + PalSize;
	const int FileSize = PixelOffset + ImageSize;

	uint8_t Header[FileHeader + InfoHeader] = {};
	Header[0] = 'B';
	Header[1] = 'M';
	WriteU32LE(Header + 2, uint32_t(FileSize));
	WriteU32LE(Header + 10, uint32_t(PixelOffset));
	WriteU32LE(Header + 14, uint32_t(InfoHeader));
	WriteU32LE(Header + 18, uint32_t(LoresW));
	WriteU32LE(Header + 22, uint32_t(LoresH));
	WriteU16LE(Header + 26, 1);
	WriteU16LE(Header + 28, 8); // 8 bpp
	WriteU32LE(Header + 30, 0); // BI_RGB
	WriteU32LE(Header + 34, uint32_t(ImageSize));
	WriteU32LE(Header + 38, 2835);
	WriteU32LE(Header + 42, 2835);
	WriteU32LE(Header + 46, 256);
	WriteU32LE(Header + 50, 0);

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open bmp output: ") + Path;
		return false;
	}
	const bool bOk = fwrite(Header, 1, sizeof(Header), F) == sizeof(Header) &&
					 fwrite(PaletteBytes, 1, sizeof(PaletteBytes), F) == sizeof(PaletteBytes) &&
					 fwrite(Pixels.data(), 1, Pixels.size(), F) == Pixels.size();
	fclose(F);
	if (!bOk)
	{
		OutError = std::string("short write to bmp output: ") + Path;
		return false;
	}
	return true;
}

// ZX-Uno Radastan (128x96 4bpp) encoder. From the ZX-Uno Spectrum core
// (ula_radas.v config.vh): the framebuffer is linear in the 16 KB videoram
// (bank 5) at byte address radasoffset + row * (64 + padding) + byteColumn; each
// byte holds two pixels (high nibble = left). The pixel colour is a ULAplus
// 64-entry CRAM lookup at index {RADASPALBANK, nibble}, decoded GRB 3-3-2. A
// 24-bit BMP (the CRAM's 64 colours don't fit a 16-entry palette).
static bool WriteRadastanBmp(const MemoryBus& Mem, const char* Path, std::string& OutError)
{
	const uint8_t* Vram = Mem.Ram16KBank(5);
	const uint16_t Offset = Mem.GetUnoRadasOffset();
	const int RowStride = 64 + Mem.GetUnoRadasPadding();
	const uint8_t PalBank = uint8_t(Mem.GetUnoRadasPalBank() & 0x03);
	const uint8_t* Cram = Mem.GetUlaPlusCram();

	constexpr int Border = 32;
	constexpr int PaperW = 128;
	constexpr int PaperH = 96;
	const int Width = PaperW + 2 * Border;
	const int Height = PaperH + 2 * Border;
	const int Stride = (Width * 3 + 3) & ~3;
	std::vector<uint8_t> Image(size_t(Stride) * size_t(Height), 0); // black border

	for (int Row = 0; Row < PaperH; ++Row)
	{
		const int RowBase = Offset + Row * RowStride;
		for (int ByteCol = 0; ByteCol < 64; ++ByteCol)
		{
			const uint8_t Data = Vram[(RowBase + ByteCol) & 0x3FFF];
			for (int Half = 0; Half < 2; ++Half)
			{
				const uint8_t Nibble = (Half == 0) ? uint8_t(Data >> 4) : uint8_t(Data & 0x0F);
				const uint8_t Entry = Cram[(PalBank << 4) | Nibble];
				const uint8_t Green = uint8_t(((Entry >> 5) & 0x07) * 255 / 7);
				const uint8_t Red = uint8_t(((Entry >> 2) & 0x07) * 255 / 7);
				const uint8_t Blue = uint8_t((Entry & 0x03) * 255 / 3);
				const int CanvasX = Border + ByteCol * 2 + Half;
				const int CanvasY = Border + Row;
				const int Dst = (Height - 1 - CanvasY) * Stride + CanvasX * 3;
				Image[size_t(Dst) + 0] = Blue;
				Image[size_t(Dst) + 1] = Green;
				Image[size_t(Dst) + 2] = Red;
			}
		}
	}

	constexpr int FileHdr = 14;
	constexpr int InfoHdr = 40;
	const int PixelOffset = FileHdr + InfoHdr;
	const int FileSize = PixelOffset + int(Image.size());
	uint8_t Header[FileHdr + InfoHdr] = {};
	Header[0] = 'B';
	Header[1] = 'M';
	WriteU32LE(Header + 2, uint32_t(FileSize));
	WriteU32LE(Header + 10, uint32_t(PixelOffset));
	WriteU32LE(Header + 14, uint32_t(InfoHdr));
	WriteU32LE(Header + 18, uint32_t(Width));
	WriteU32LE(Header + 22, uint32_t(Height));
	WriteU16LE(Header + 26, 1);
	WriteU16LE(Header + 28, 24);
	WriteU32LE(Header + 34, uint32_t(Image.size()));
	WriteU32LE(Header + 38, 2835);
	WriteU32LE(Header + 42, 2835);

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open bmp output: ") + Path;
		return false;
	}
	const bool bOk = fwrite(Header, 1, sizeof(Header), F) == sizeof(Header) &&
					 fwrite(Image.data(), 1, Image.size(), F) == Image.size();
	fclose(F);
	if (!bOk)
	{
		OutError = std::string("short write to bmp output: ") + Path;
		return false;
	}
	return true;
}

bool WriteVisibleScreenBmp(const MemoryBus& Mem, const SpritesState* Sprites, const char* Path, std::string& OutError)
{
	// On Next-family machines the user wants what's on the actual screen,
	// which is the composite (L2 + ULA / LoRes + sprites, per NR_15
	// priority). On 48K/128K the classic ULA encoder is correct.
	if (Mem.IsNext())
	{
		return WriteCompositeBmp(Mem, Sprites, Path, OutError);
	}
	if (Mem.IsTimex())
	{
		return WriteTimexScreenBmp(Mem, Path, OutError);
	}
	if (Mem.IsZxEvo())
	{
		return WriteZxEvoScreenBmp(Mem, Path, OutError);
	}
	// ZX-Uno renders its Timex-style SCLD screen (OUT 255,2 = 8x1 hicolour) via
	// the Timex encoder. (Radastan 128x96 4bpp is a documented follow-on pending
	// the ZX-Uno video register spec.)
	// ZX-Uno: Radastan (128x96 4bpp, ULAplus palette) when RADASCTRL enables it,
	// otherwise its Timex-style SCLD screen (OUT 255,2 = 8x1 hicolour).
	if (Mem.IsZxUno())
	{
		if (Mem.IsRadasEnabled())
		{
			return WriteRadastanBmp(Mem, Path, OutError);
		}
		return WriteTimexScreenBmp(Mem, Path, OutError);
	}
	return WriteSpectrumScreenBmp(Mem, Path, OutError);
}

bool WritePerLayerScreenBmp(const MemoryBus& Mem, const char* Path, std::string& OutError)
{
	// Old behaviour, kept behind --bmp-per-layer for debug. LoRes wins
	// over L2, L2 wins over ULA, otherwise classic ULA.
	if (Mem.IsNext() && (Mem.GetNextReg(0x15) & 0x80) != 0)
	{
		return WriteNextLoResBmp(Mem, Path, OutError);
	}
	if (Mem.DetectActiveLayer() != MemoryBus::ActiveLayer::kActiveLayerUla)
	{
		return WriteNextLayer2Bmp(Mem, Path, OutError);
	}
	return WriteSpectrumScreenBmp(Mem, Path, OutError);
}

// ============================================================================
// RGB888 composite encoder (Phase 6).
// ============================================================================

namespace
{

constexpr int kCompW = 320;
constexpr int kCompH = 256;
static bool g_bL2RightNibble = false; // see RenderCompositeBgrHires
constexpr int kCompBytesPerPixel = 3; // BGR
constexpr int kCompRowPadded = (kCompW * kCompBytesPerPixel + 3) & ~3;
constexpr int kCompImageSize = kCompRowPadded * kCompH;
constexpr uint8_t kSpriteTransparentPixel = 0xE3; // Next default sprite transparent index

// Pixel blending mode for compositor layer passes.
//
//   Overwrite     -- the resolved pixel REPLACES whatever's underneath. Used
//                    for L2 (bottom), sprites (top), and ULA/LoRes when the
//                    NR_15 priority bits don't indicate a blend mode.
//   AdditiveClamp -- result = saturate7(existing + new) per RGB channel.
//                    Selected by NR_15 [4:2] == 110 (UlaBlend). The Next
//                    compositor runs the blend in 3-bit RGB space, so we
//                    convert each channel down to 0..7, sum, clamp, expand.
//   HalfBlend     -- result = (existing + new) / 2 per channel. Selected by
//                    NR_15 [4:2] == 111 (UlaHalfBlend).
//
// Layer 2 is always Overwrite (it's drawn first as the base). The upper
// "ULA slot" layer - classic ULA or LoRes per NR_15 bit 7 - picks up the
// blend mode. Sprites land last and always Overwrite.
enum BlendMode
{
	kBlendModeOverwrite,
	kBlendModeAdditiveClamp7,
	kBlendModeHalfBlend
};

inline uint8_t Expand3(uint8_t Channel3Bit)
{
	// Replicate to 8-bit the same way the Next DAC does: c3 -> (c3<<5)|(c3<<2)|(c3>>1).
	return uint8_t((Channel3Bit << 5) | (Channel3Bit << 2) | (Channel3Bit >> 1));
}

inline uint8_t Reduce3(uint8_t Channel8Bit)
{
	// Inverse: map 8-bit channel back to its 3-bit Next value. The
	// canonical 3-bit values produce 8-bit outputs $00, $24, $48, $6D,
	// $91, $B6, $DA, $FF; reducing by `>> 5` recovers the 3-bit value.
	return uint8_t(Channel8Bit >> 5);
}

inline void PutComp(uint8_t* Image, int X, int Y, Rgb888 C, BlendMode Mode = BlendMode::kBlendModeOverwrite)
{
	if (X < 0 || X >= kCompW || Y < 0 || Y >= kCompH)
	{
		return;
	}
	// BMP is bottom-up.
	const int RowFromBottom = kCompH - 1 - Y;
	uint8_t* Px = Image + RowFromBottom * kCompRowPadded + X * kCompBytesPerPixel;
	if (Mode == BlendMode::kBlendModeOverwrite)
	{
		Px[0] = C.m_B;
		Px[1] = C.m_G;
		Px[2] = C.m_R;
		return;
	}
	if (Mode == BlendMode::kBlendModeAdditiveClamp7)
	{
		const uint8_t ExR = Reduce3(Px[2]), ExG = Reduce3(Px[1]), ExB = Reduce3(Px[0]);
		const uint8_t InR = Reduce3(C.m_R), InG = Reduce3(C.m_G), InB = Reduce3(C.m_B);
		const uint8_t OutR = uint8_t(std::min(7, int(ExR) + int(InR)));
		const uint8_t OutG = uint8_t(std::min(7, int(ExG) + int(InG)));
		const uint8_t OutB = uint8_t(std::min(7, int(ExB) + int(InB)));
		Px[2] = Expand3(OutR);
		Px[1] = Expand3(OutG);
		Px[0] = Expand3(OutB);
		return;
	}
	// HalfBlend: (existing + new) / 2 per 3-bit channel.
	const uint8_t ExR = Reduce3(Px[2]), ExG = Reduce3(Px[1]), ExB = Reduce3(Px[0]);
	const uint8_t InR = Reduce3(C.m_R), InG = Reduce3(C.m_G), InB = Reduce3(C.m_B);
	Px[2] = Expand3(uint8_t((ExR + InR) >> 1));
	Px[1] = Expand3(uint8_t((ExG + InG) >> 1));
	Px[0] = Expand3(uint8_t((ExB + InB) >> 1));
}

// SCLD ULA slot, colour INDEX only: resolves slot-local (X,Y) to an 0..15
// INK/PAPER index under any of the four port-$FF modes (mode decode mirrors
// WriteTimexScreenBmp). Hi-res halves 512 -> 256 with ink winning each pixel
// pair, so 1px verticals stay visible.
//
// Index rather than colour because two different machines want two different
// palettes out of the same decode: a real Timex resolves through PaletteTimex
// (below), while a Next running a Timex display mode resolves through its own
// ULA palette with the NR_14 transparency test, like any other Next ULA pixel.
inline uint8_t TimexUlaSlotIndex(const uint8_t* Vram, uint8_t Scld, int X, int Y)
{
	const bool bHires = (Scld & 0x04) != 0;
	const bool bExtColour = !bHires && (Scld & 0x02) != 0;
	const bool bScreen2 = !bHires && !bExtColour && (Scld & 0x01) != 0;
	const int kY76 = (Y >> 6) & 0x03, kY53 = (Y >> 3) & 0x07, kY20 = Y & 0x07;
	const int PixRow = (kY76 << 11) | (kY20 << 8) | (kY53 << 5);
	const int Col = X >> 3;
	uint8_t Idx;
	if (bHires)
	{
		// Slot pixel X covers hi-res pixels 2X, 2X+1: both in file 1's byte for
		// the left half of the 16px byte pair, both in file 2's for the right.
		const uint8_t Colour = uint8_t((Scld >> 3) & 0x07);
		const int P = (X & 7) * 2;
		const uint8_t Src = (P < 8) ? Vram[PixRow + Col] : Vram[0x2000 + PixRow + Col];
		const bool bInk = ((Src << (P & 7)) & 0xC0) != 0;
		Idx = bInk ? uint8_t(Colour ^ 0x07) : Colour;
	}
	else
	{
		const int Base = bScreen2 ? 0x2000 : 0x0000;
		const uint8_t Pix = Vram[Base + PixRow + Col];
		const uint8_t Attr = bExtColour ? Vram[0x2000 + PixRow + Col] : Vram[Base + 0x1800 + ((Y >> 3) << 5) + Col];
		const bool bOn = ((Pix >> (7 - (X & 7))) & 1) != 0;
		const uint8_t Br = (Attr & 0x40) ? 8 : 0;
		Idx = uint8_t((bOn ? (Attr & 0x07) : ((Attr >> 3) & 0x07)) + Br);
	}
	return Idx;
}

// The Timex machines' variant: the index above through PaletteTimex, so the
// composite agrees with the still encoder (bright black = SCLD dark grey,
// unlike the Sinclair ULA).
inline Rgb888 TimexUlaSlotPixel(const uint8_t* Vram, uint8_t Scld, int X, int Y)
{
	const uint8_t Idx = TimexUlaSlotIndex(Vram, Scld, X, Y);
	// PaletteTimex is BGRx (BMP palette order).
	return {kPaletteTimex[Idx * 4 + 2], kPaletteTimex[Idx * 4 + 1], kPaletteTimex[Idx * 4 + 0]};
}

// Render one hardware sprite onto the composite. Supports 4-byte and
// 5-byte attribute formats. Patterns are 16x16 8bpp; transparent value
// $E3. Scale (5-byte format) is 1x/2x/4x/8x per axis.
//
// On real hardware attribute slots and pattern slots are INDEPENDENT -
// many sprites can share one pattern via attr3 bits 5:0 selecting the
// pattern slot. We honour that by looking up the pattern via `S.Slots[N]`
// rather than the per-sprite `Slot.Pattern` field. (The same field still
// receives upload bytes when the user `out $5B`s to that specific slot;
// `S.Slots[N].Pattern` is the pattern store, indexed by N.)
// Plot sink signature: void(int Cx, int Cy, Rgb888 C). The direct-blit path
// passes a PutComp lambda; the beam compositor passes a buffer-setter so the
// SAME rasterisation (slot order, clip, scale, transparency, last-writer-wins)
// feeds a per-pixel sprite buffer. OffsetX/OffsetY are baked into Cx/Cy here.
template <typename PlotFn>
void RenderOneSprite(const MemoryBus& Mem, const SpritesState& S, int SlotIdx, int OffsetX, int OffsetY, PlotFn Plot,
	bool bRelative = false, int ExtraPalOff = 0, int From = 0, int To = kCompW * kCompH)
{
	const SpriteSlot& Slot = S.m_Slots[SlotIdx];
	const uint8_t Attr3 = Slot.m_Attr.m_Bytes[3];
	if (!(Attr3 & 0x80))
	{
		return; // V bit clear -> invisible
	}
	// Intersect the sprite clip window with the reachable display area.
	// NR_15 bit 1 clear: clip to the ULA area (hardware X 32..287, Y 32..223),
	// with NR_19 coordinates relative to that area. When bit 1 is set, sprites
	// can reach the full 320x256 canvas; NR_19 clips it only if bit 5 is set,
	// with its X coordinates doubled.
	const uint8_t Nr15 = Mem.GetNextReg(0x15);
	const uint8_t* Sc = Mem.GetClipWindow(1);
	int ClipMinX, ClipMaxX, ClipMinY, ClipMaxY;
	if (!(Nr15 & 0x02))
	{
		ClipMinX = 32 + Sc[0];
		ClipMaxX = 32 + std::min<int>(Sc[1], 255);
		ClipMinY = 32 + Sc[2];
		ClipMaxY = 32 + std::min<int>(Sc[3], 191);
	}
	else if (Nr15 & 0x20)
	{
		ClipMinX = Sc[0] * 2;
		ClipMaxX = std::min<int>(Sc[1] * 2 + 1, kCompW - 1);
		ClipMinY = Sc[2];
		ClipMaxY = std::min<int>(Sc[3], kCompH - 1);
	}
	else
	{
		ClipMinX = 0;
		ClipMaxX = kCompW - 1;
		ClipMinY = 0;
		ClipMaxY = kCompH - 1;
	}
	// A beam boundary needs only this span, not an entire sprite image. Clip
	// whole rows first; the first/last row's horizontal ends are checked below.
	ClipMinY = std::max(ClipMinY, From / kCompW);
	ClipMaxY = std::min(ClipMaxY, (To - 1) / kCompW);

	const bool bB5Byte = (Attr3 & 0x40) != 0;
	// NR_14 transparency applies to ULA, Layer 2, LoRes and tilemap pixels.
	// Sprites instead compare the pattern index against NR_4B.
	const uint8_t SpriteTransparentIdx = Mem.GetNextReg(0x4B); // default $E3

	// Anchor coordinates are 9-bit; attribute 4 bit 0 supplies Y8.
	const uint8_t Attr2 = Slot.m_Attr.m_Bytes[2];
	const uint8_t Attr4 = bB5Byte ? Slot.m_Attr.m_Bytes[4] : 0;
	int SprX, SprY;
	if (bRelative)
	{
		// Relative sprite: attr0/attr1 are signed 8-bit
		// offsets from the anchor's position (passed in OffsetX/OffsetY).
		// attr2 bit 0 here is PR (palette-relative), not X8 - see ExtraPalOff.
		SprX = int(int8_t(Slot.m_Attr.m_Bytes[0]));
		SprY = int(int8_t(Slot.m_Attr.m_Bytes[1]));
	}
	else
	{
		SprX = int(Slot.m_Attr.m_Bytes[0]) | (int(Attr2 & 0x01) << 8); // 0..511
		SprY = int(Slot.m_Attr.m_Bytes[1]) | (int(Attr4 & 1) << 8);
		if (SprY > 255)
		{
			SprY -= 512;
		}
		// 9-bit X overflows at 320; the hardware visible window is 0..319.
		if (SprX > 319)
		{
			SprX -= 512;
		}
	}

	// Palette offset: own high nibble, plus the anchor's when PR is set on a
	// relative sprite (ExtraPalOff carries it; 0 otherwise). Wraps in 4 bits.
	const uint8_t PaletteOffset = uint8_t(((Attr2 >> 4) + ExtraPalOff) & 0x0F);

	// Anchor/composite scale fields: X in bits 4:3, Y in bits 2:1.
	// https://wiki.specnext.dev/Sprite_Attribute_4_Register
	int XScale = 1, YScale = 1;
	if (bB5Byte)
	{
		XScale = 1 << ((Attr4 >> 3) & 0x03);
		YScale = 1 << ((Attr4 >> 1) & 0x03);
	}
	if (OffsetX + SprX > ClipMaxX || OffsetX + SprX + 16 * XScale <= ClipMinX ||
		OffsetY + SprY > ClipMaxY || OffsetY + SprY + 16 * YScale <= ClipMinY)
	{
		return;
	}

	// H selects 4-bit colour; N6 selects the half within the 256-byte slot.
	const bool bB4Bit = bB5Byte && (Attr4 & 0x80) != 0;
	const uint8_t* Pat;
	int Pat4BitHalf = 0; // byte offset within the slot (0 or 128)
	if (bB4Bit)
	{
		Pat = S.m_Slots[Attr3 & 0x3F].m_Pattern;
		Pat4BitHalf = ((Attr4 >> 6) & 1) * 128;
	}
	else
	{
		Pat = S.m_Slots[Attr3 & 0x3F].m_Pattern;
	}

	// Use sprite palette 0 (NR_43 bit 3 selects palette slot 2 or 6).
	const int SprPalIdx = (Mem.GetNextReg(0x43) & 0x08) ? 6 : 2;
	const uint8_t(*SprPal)[2] = Mem.GetPalette(SprPalIdx);

	// Attribute 2 transforms apply to both formats: rotate, then mirror.
	const bool bXMir = (Attr2 & 8) != 0;
	const bool bYMir = ((Attr2 >> 2) & 1) != 0;
	const bool bRot = (Attr2 & 2) != 0;
	const bool bTransform = bXMir || bYMir || bRot;

	for (int PatY = 0; PatY < 16; ++PatY)
	{
		for (int PatX = 0; PatX < 16; ++PatX)
		{
			// 4-bit: one nibble per pixel (128-byte pattern, 8 bytes/row, high
			// nibble = left), transparent when the nibble == NR_4B low nibble.
			// 8-bit: one byte per pixel, transparent when the byte == NR_4B.
			uint8_t Idx;
			if (bB4Bit)
			{
				const uint8_t B = Pat[Pat4BitHalf + PatY * 8 + (PatX >> 1)];
				Idx = (PatX & 1) ? uint8_t(B & 0x0F) : uint8_t((B >> 4) & 0x0F);
				if (Idx == (SpriteTransparentIdx & 0x0F))
				{
					continue;
				}
			}
			else
			{
				Idx = Pat[PatY * 16 + PatX];
				// Sprite transparency: pattern byte == NR_4B (default $E3)
				// means the pixel is skipped. This is the ONLY transparency
				// mechanism for sprites - see the NR_14 comment above.
				if (Idx == SpriteTransparentIdx)
				{
					continue;
				}
			}
			// Add the palette offset to the high nibble of the pattern index,
			// wrapping at 256: final_index = (pattern_index + (offset << 4)) & 255.
			const uint8_t PalEntry = uint8_t(Idx + (PaletteOffset << 4));
			const Rgb888 C = DecodeNxEntry(SprPal[PalEntry][0], SprPal[PalEntry][1]);
			// Transformed screen sub-coordinate for this pattern pixel.
			int Ox = PatX, Oy = PatY;
			if (bTransform)
			{
				if (bRot)
				{
					const int OldX = Ox;
					Ox = 15 - Oy;
					Oy = OldX;
				}
				if (bXMir)
				{
					Ox = 15 - Ox;
				}
				if (bYMir)
				{
					Oy = 15 - Oy;
				}
			}
			// Draw the pixel as an XScale x YScale block at the transformed spot.
			for (int Sy = 0; Sy < YScale; ++Sy)
			{
				for (int Sx = 0; Sx < XScale; ++Sx)
				{
					const int Cx = OffsetX + SprX + Ox * XScale + Sx;
					const int Cy = OffsetY + SprY + Oy * YScale + Sy;
					if (Cx < ClipMinX || Cx > ClipMaxX || Cy < ClipMinY || Cy > ClipMaxY)
					{
						continue;
					}
					const int Pixel = Cy * kCompW + Cx;
					if (Pixel < From || Pixel >= To) continue;
					Plot(Cx, Cy, C);
				}
			}
		}
	}
}

// ============================================================================
// The compositor (pixel-major). ComposePixel evaluates one composite pixel from
// LIVE state - border + Layer 2 (256/320/640) + ULA/LoRes/Timex + sprites, in
// the NR_15 order with the two blend codes. WriteCompositeBmp runs it over the
// whole canvas as an end-of-frame snapshot; the eager BeamDrawer runs it per
// span at each beam boundary, so mid-frame register/memory changes land where
// the beam crossed them. This is shared by frame replay and eager capture.
// ============================================================================

// Per-frame layer context, computed once per snapshot / per beam span from the
// register file. Every register the compositor needs is resolved HERE, never in
// the per-pixel functions (the drawer re-derives it at each boundary).
// Border colour: ULA $FE bits 0..2 through the ACTIVE ULA PALETTE, exactly as
// the paper and ink go. On the Next the border is not a fixed colour - it is
// a PAPER entry of the ULA palette, so a program that reprograms the ULA
// palette moves the border with it: in the classic layout the paper colours
// are entries 16..23 (24..31 bright), and the border is paper `$FE & 7` with
// bright clear, i.e. entry 16 + colour. NOT entry 0..7 - those are the inks,
// and a palette whose entry 16 is not black gives a coloured border on the
// board while entries 0..7 say nothing about it (seen with a LoRes logo
// palette: violet border on hardware, black here).
//
// In ULANext mode the attribute's paper half indexes entries 128 upwards, and
// the border goes with the paper: entry 128 + colour. Full-ink mode ($42=255)
// has no paper entries and instead uses the $4A fallback, including black.
//
// The reset palette decodes to level 219; a fixed level-215 table does not.
inline Rgb888 Rgb332ToRgb888(uint8_t V);

Rgb888 BorderRgb(const MemoryBus& Mem)
{
	const uint8_t(*Pal)[2] = Mem.GetPalette(ActiveUlaPaletteIndex(Mem));
	const bool bUlaNext = (Mem.GetNextReg(0x43) & 0x01) != 0;
	if (bUlaNext && Mem.GetNextReg(0x42) == 255)
	{
		return Rgb332ToRgb888(Mem.GetNextReg(0x4A));
	}
	const uint8_t I = uint8_t((bUlaNext ? 128 : 16) + (Mem.GetBorderColour() & 0x07));
	return DecodeNxEntry(Pal[I][0], Pal[I][1]);
}

// A clip rectangle already converted to composite (320x256) coordinates,
// inclusive. A pixel (Cx,Cy) is inside when X1<=Cx<=X2 && Y1<=Cy<=Y2.
struct ClipRect
{
	int m_X1 = 0, m_X2 = kCompW - 1, m_Y1 = 0, m_Y2 = kCompH - 1;
	bool Contains(int Cx, int Cy) const
	{
		return Cx >= m_X1 && Cx <= m_X2 && Cy >= m_Y1 && Cy <= m_Y2;
	}
};

struct FrameCtx
{
	MemoryBus::ActiveLayer m_Layer;
	MemoryBus::LayerPriority m_Priority;
	bool m_bLoRes = false;
	uint8_t m_TransparentHi = 0;

	// Per-layer clip windows ($18/$1A), resolved to composite coordinates.
	// Sprites ($19) clip inside RenderOneSprite. Tilemap ($1B) awaits the layer.
	ClipRect m_L2Clip, m_UlaClip;
	// ULA control ($26/$27 scroll, $68). bUlaOff = $68 bit 7 (ULA transparent).
	uint8_t m_UlaScrollX = 0, m_UlaScrollY = 0;
	bool m_bUlaOff = false;
	// ULANext ($43 bit 0): the attribute byte splits into ink (low `mask` bits)
	// and paper (the rest, into palette entries 128+). $42 = ink mask.
	bool m_bUlaNext = false;
	uint8_t m_UlaNextInkMask = 7;
	// Tilemap (shares the U slot with the ULA). bStencil = $68 bit 0.
	TilemapParams m_Tilemap;
	bool m_bStencil = false;
	// $68 bits 6:5 (core 3.1.1): which layer is the blend colour B in the
	// NR_15 %110/%111 modes "(U|T)S(T|U)(B+L)": 00 ULA, 01 none, 10 the
	// ULA/tilemap mix, 11 tilemap. The layer that does not blend is drawn on
	// its own in front of the sprites.
	uint8_t m_BlendSource = 0;
	// Transparency fallback ($4A, RRRGGGBB) shown where every layer is
	// transparent inside PaperRegion; the border keeps its $FE colour.
	Rgb888 m_FallbackRgb{0, 0, 0};
	bool m_bFallbackSet = false;
	ClipRect m_PaperRegion; // where $4A may paint (256x192 centre, or full canvas for wide L2)

	// Layer 2 ("L" slot). bL2Present = any L2 layer active (Layer != Ula).
	bool m_bL2Present = false;
	int m_L2OffX = 0, m_L2OffY = 0, m_L2w = 0, m_L2h = 0;
	int m_L2BasePage8K = 0, m_L2ScrollX = 0, m_L2ScrollY = 0;
	// Direct per-pixel fetch for EVERY L2 mode from the live 8K pages (no
	// pre-collect / transpose), so a mid-frame page flip or a fill in progress
	// renders exactly where the beam crossed it. 256 is row-major (48K = 6
	// pages); 320/640 are column-major (80K = 10 pages); 640 packs two 4-bit
	// pixels per byte (this canvas shows the high nibble = left of each pair).
	const uint8_t* m_L2Page[10] = {};
	bool m_bL2ColumnMajor = false; // 320 / 640
	bool m_bL2Nibble = false; // 640: high nibble of each byte
	Rgb888 m_Border{0, 0, 0}; // ULA border colour: what shows where no layer paints
	const uint8_t (*m_L2Pal)[2] = nullptr;

	// Upper "ULA slot": classic ULA or LoRes per NR_15 bit 7. Offset (32,32).
	const uint8_t* m_UlaVram = nullptr;
	const uint8_t (*m_UlaPal)[2] = nullptr; // classic ULA palette (0)
	// Timex SCLD: when bTimex, the slot resolves via TimexUlaSlotPixel instead.
	// bNextScld is the Next flavour of the same thing - SCLD decode, but the
	// index finishes through the Next palette rather than PaletteTimex.
	bool m_bTimex = false;
	bool m_bNextScld = false;
	uint8_t m_ScldMode = 0;
	// LoRes
	const uint8_t* m_LoVram = nullptr;
	bool m_bLo16Colour = false;
	uint8_t m_LoPalOffset = 0;
	int m_LoScrollX = 0, m_LoScrollY = 0;

	// Sprites ("S" slot): pre-rasterised once per FrameCtx. Last-writer-wins over
	// slots 0..127.
	bool m_bSpritesActive = false;
	int m_SprFrom = 0; // first composite pixel covered by the sprite buffers
	std::vector<uint8_t> m_SprPresent; // 1 where a sprite pixel lands, within the span
	std::vector<Rgb888> m_SprColor;
};

// Expand an 8-bit RRRGGGBB colour (NR $4A / 9-bit palette high byte) to RGB888.
inline Rgb888 Rgb332ToRgb888(uint8_t V)
{
	const uint8_t kR3 = uint8_t((V >> 5) & 0x07);
	const uint8_t kG3 = uint8_t((V >> 2) & 0x07);
	const uint8_t kB2 = uint8_t(V & 0x03);
	const uint8_t R = uint8_t((kR3 << 5) | (kR3 << 2) | (kR3 >> 1));
	const uint8_t G = uint8_t((kG3 << 5) | (kG3 << 2) | (kG3 >> 1));
	const uint8_t B = uint8_t((kB2 << 6) | (kB2 << 4) | (kB2 << 2) | kB2);
	return Rgb888{R, G, B};
}

inline FrameCtx BuildFrameCtx(const MemoryBus& Mem, const SpritesState* Sprites,
	int From = 0, int To = kCompW * kCompH)
{
	FrameCtx C;
	C.m_Layer = Mem.DetectActiveLayer();
	C.m_Priority = Mem.GetLayerPriority();
	C.m_bLoRes = (Mem.GetNextReg(0x15) & 0x80) != 0;
	C.m_TransparentHi = Mem.GetNextReg(0x14);
	C.m_Border = BorderRgb(Mem);

	// Clip windows ($18 L2 / $1A ULA), ULA scroll/control ($26/$27/$68) and the
	// transparency fallback ($4A). Resolved once per frame into composite coords.
	{
		const uint8_t* L2c = Mem.GetClipWindow(0);
		const uint8_t* Ulac = Mem.GetClipWindow(2);
		const bool bL2Wide = (C.m_Layer == MemoryBus::ActiveLayer::kActiveLayerL2320) ||
							 (C.m_Layer == MemoryBus::ActiveLayer::kActiveLayerL2640);
		if (bL2Wide)
		{
			// 320/640: X doubled, no offset. An untouched $18 (still the 256-mode
			// reset default) means "full canvas for this mode" - the program is
			// expected to set $18 up per mode; only a deliberate write clips.
			if (Mem.IsClipSet(0))
			{
				C.m_L2Clip = {L2c[0] * 2, L2c[1] * 2 + 1, L2c[2], L2c[3]};
			}
			// else leave C.L2Clip at its full-canvas default.
		}
		else
		{
			C.m_L2Clip = {32 + L2c[0], 32 + L2c[1], 32 + L2c[2], 32 + L2c[3]};
		}
		C.m_UlaClip = {32 + Ulac[0], 32 + Ulac[1], 32 + Ulac[2], 32 + Ulac[3]};
		C.m_UlaScrollX = Mem.GetNextReg(0x26);
		C.m_UlaScrollY = Mem.GetNextReg(0x27);
		C.m_bUlaOff = (Mem.GetNextReg(0x68) & 0x80) != 0;
		C.m_bUlaNext = (Mem.GetNextReg(0x43) & 0x01) != 0; // ULANext enable
		C.m_UlaNextInkMask = Mem.GetNextReg(0x42); // ink mask (default 7)
		C.m_bStencil = (Mem.GetNextReg(0x68) & 0x01) != 0; // $68 bit 0 stencil
		C.m_BlendSource = uint8_t((Mem.GetNextReg(0x68) >> 5) & 0x03); // $68 bits 6:5
		// zero is a valid black fallback, not an unset-register sentinel
		const uint8_t Fb = Mem.GetNextReg(0x4A);
		C.m_bFallbackSet = Mem.IsNext();
		C.m_FallbackRgb = Rgb332ToRgb888(Fb);
		// Fallback paints in the layer area only: 256x192 centre normally, the
		// full canvas when a wide L2 (320/640), a tilemap, or over-border sprites
		// reach the border.
		const bool bWideCanvas = bL2Wide || (Mem.GetNextReg(0x15) & 0x02) ||
								 (Mem.GetNextReg(0x6B) & 0x80); // tilemap covers the whole canvas
		C.m_PaperRegion = bWideCanvas ? ClipRect{0, kCompW - 1, 0, kCompH - 1} : ClipRect{32, 287, 32, 223};
	}
	C.m_Tilemap = BuildTilemapParams(Mem);

	const bool bL2FillsCanvas = (C.m_Layer == MemoryBus::ActiveLayer::kActiveLayerL2320) ||
								(C.m_Layer == MemoryBus::ActiveLayer::kActiveLayerL2640);
	C.m_L2OffX = bL2FillsCanvas ? 0 : 32;
	C.m_L2OffY = bL2FillsCanvas ? 0 : 32;

	// Layer 2 setup.
	C.m_bL2Present = (C.m_Layer != MemoryBus::ActiveLayer::kActiveLayerUla);
	if (C.m_bL2Present)
	{
		C.m_L2Pal = Mem.GetPalette(ActiveL2PaletteIndex(Mem));
		C.m_L2ScrollX = ((int(Mem.GetNextReg(0x71)) & 0x03) << 8) | int(Mem.GetNextReg(0x16));
		C.m_L2ScrollY = int(Mem.GetNextReg(0x17));
		C.m_L2BasePage8K = (int(Mem.GetNextReg(0x12)) * 2) & (kRamPageCount - 1);
		int Pages;
		if (C.m_Layer == MemoryBus::ActiveLayer::kActiveLayerL2256)
		{
			C.m_L2w = 256;
			C.m_L2h = 192;
			Pages = 6; // 48 KB, row-major
		}
		else
		{
			C.m_L2w = 320;
			C.m_L2h = 256;
			Pages = 10; // 80 KB, column-major
			C.m_bL2ColumnMajor = true;
			C.m_bL2Nibble = (C.m_Layer == MemoryBus::ActiveLayer::kActiveLayerL2640);
		}
		for (int P = 0; P < Pages; ++P)
		{
			C.m_L2Page[P] = Mem.Ram8KPage((C.m_L2BasePage8K + P) & (kRamPageCount - 1));
		}
	}

	// ULA slot setup.
	if (C.m_bLoRes)
	{
		C.m_LoVram = Mem.Ram16KBank(5);
		const uint8_t Ctrl = Mem.GetNextReg(0x6A);
		C.m_bLo16Colour = (Ctrl & 0x20) != 0;
		if (C.m_bLo16Colour)
			C.m_LoVram += (((Ctrl >> 4) ^ Mem.GetScldMode()) & 1) * 0x2000;
		C.m_LoPalOffset = uint8_t(Ctrl & 0x0F);
		C.m_LoScrollX = int(Mem.GetNextReg(0x32));
		C.m_LoScrollY = int(Mem.GetNextReg(0x33));
		C.m_UlaPal = Mem.GetPalette(ActiveUlaPaletteIndex(Mem)); // LoRes uses the ULA palette
	}
	else
	{
		const int DisplayedBank = (Mem.GetPort7FFD() & 0x08) ? 7 : 5;
		C.m_UlaVram = Mem.Ram16KBank(DisplayedBank);
		C.m_UlaPal = Mem.GetPalette(ActiveUlaPaletteIndex(Mem));
		C.m_bTimex = Mem.IsTimex();
		C.m_bNextScld = Mem.IsNextTimexDisplay();
		if (C.m_bTimex || C.m_bNextScld)
		{
			C.m_ScldMode = Mem.GetScldMode();
		}
	}

	// Pre-rasterise sprites into a per-pixel buffer (NR_15 bit 0 = engine enable).
	if (Sprites && (Mem.GetNextReg(0x15) & 0x01))
	{
		C.m_bSpritesActive = true;
		C.m_SprFrom = From;
		C.m_SprPresent.assign(size_t(To - From), 0);
		C.m_SprColor.assign(size_t(To - From), Rgb888{0, 0, 0});
		// NR_15 bit 6 = sprite Z-priority. Default 0: higher slot on top (draw
		// 0..127, last write wins). Set:
		// sprite 0 on top, so draw in reverse (127..0) and let slot 0 win.
		const bool bSprite0OnTop = (Mem.GetNextReg(0x15) & 0x40) != 0;

		// Resolve relative positions, visibility and palette offsets in slot order.
		// Composite groups are partial: unified inheritance, relative 4-bit
		// patterns and the pattern-relative flag are not implemented.
		struct RelInfo
		{
			bool m_bRel;
			int m_OffX, m_OffY, m_ExtraPal;
			bool m_bAnchorVis;
		};
		RelInfo RI[128];
		int AnchorX = 0, AnchorY = 0, AnchorPal = 0;
		bool bHaveAnchor = false, bAnchorVis = false;
		for (int IdxI = 0; IdxI < 128; ++IdxI)
		{
			const SpriteSlot& Sl = Sprites->m_Slots[IdxI];
			const uint8_t kA2 = Sl.m_Attr.m_Bytes[2], kA3 = Sl.m_Attr.m_Bytes[3];
			const bool bB5 = (kA3 & 0x40) != 0;
			const uint8_t kA4 = bB5 ? Sl.m_Attr.m_Bytes[4] : 0;
			// Relative sprite = {H,N6} == {0,1} (attr4 bit7=0, bit6=1). A 4-bit
			// sprite (bit7=1) is never relative even though its N6 bit may be set.
			const bool bRel = bB5 && (kA4 & 0xC0) == 0x40;
			if (!bRel)
			{
				int Ax = int(Sl.m_Attr.m_Bytes[0]) | (int(kA2 & 1) << 8);
				if (Ax > 319)
				{
					Ax -= 512;
				}
				int Ay = int(Sl.m_Attr.m_Bytes[1]) | (int(kA4 & 1) << 8);
				if (Ay > 255)
				{
					Ay -= 512;
				}
				AnchorX = Ax;
				AnchorY = Ay;
				AnchorPal = (kA2 >> 4) & 0x0F;
				bAnchorVis = (kA3 & 0x80) != 0;
				bHaveAnchor = true;
				RI[IdxI] = {false, 0, 0, 0, true};
			}
			else
			{
				const int Extra = (kA2 & 0x01) ? AnchorPal : 0;
				RI[IdxI] = {true, AnchorX, AnchorY, Extra, bHaveAnchor && bAnchorVis};
			}
		}

		// Pass 2 (priority order): render, skipping relatives under a hidden anchor.
		for (int K = 0; K < 128; ++K)
		{
			const int IdxI = bSprite0OnTop ? (127 - K) : K;
			if (RI[IdxI].m_bRel && !RI[IdxI].m_bAnchorVis)
			{
				continue;
			}
			RenderOneSprite(
				Mem, *Sprites, IdxI, RI[IdxI].m_OffX, RI[IdxI].m_OffY,
				[&](int Cx, int Cy, Rgb888 Col)
				{
					const size_t P = size_t(Cy * kCompW + Cx - From);
					C.m_SprPresent[P] = 1;
					C.m_SprColor[P] = Col;
				},
				RI[IdxI].m_bRel, RI[IdxI].m_ExtraPal, From, To);
		}
	}
	return C;
}

// L2 pixel at composite (Cx,Cy). Returns false if outside L2 / transparent.
inline bool L2PixelAt(const FrameCtx& C, int Cx, int Cy, Rgb888& Out)
{
	if (!C.m_bL2Present)
	{
		return false;
	}
	if (!C.m_L2Clip.Contains(Cx, Cy))
	{
		return false; // $18 clip window
	}
	const int X = Cx - C.m_L2OffX, Y = Cy - C.m_L2OffY;
	if (X < 0 || X >= C.m_L2w || Y < 0 || Y >= C.m_L2h)
	{
		return false;
	}
	const int SrcX = (X + C.m_L2ScrollX) % C.m_L2w;
	const int SrcY = (Y + C.m_L2ScrollY) % C.m_L2h;
	// Byte offset into L2 memory: 256 is row-major (Y*W + X); 320/640 are
	// column-major (X*H + Y = column X's contiguous H-byte run, then row Y).
	const int Off = C.m_bL2ColumnMajor ? (SrcX * C.m_L2h + SrcY) : (SrcY * C.m_L2w + SrcX);
	uint8_t Idx = C.m_L2Page[Off / kSlotSize][Off % kSlotSize];
	if (C.m_bL2Nibble)
	{
		// 640: left pixel of the pair on this 320-wide canvas; the right one when
		// RenderCompositeBgrHires asks for its second pass
		Idx = uint8_t(g_bL2RightNibble ? (Idx & 0x0F) : ((Idx >> 4) & 0x0F));
	}
	if (C.m_L2Pal[Idx][0] == C.m_TransparentHi)
	{
		return false;
	}
	Out = DecodeNxEntry(C.m_L2Pal[Idx][0], C.m_L2Pal[Idx][1]);
	return true;
}

// ULA-slot (classic ULA or LoRes) pixel at composite (Cx,Cy). Offset (32,32),
// covers the 256x192 visible area. Returns false if outside / transparent.
inline bool UlaSlotPixelAt(const FrameCtx& C, int Cx, int Cy, Rgb888& Out)
{
	if (C.m_bUlaOff)
	{
		return false; // $68 bit 7 = ULA output off
	}
	const int X = Cx - 32, Y = Cy - 32;
	if (X < 0 || X >= 256 || Y < 0 || Y >= 192)
	{
		return false;
	}
	if (!C.m_UlaClip.Contains(Cx, Cy))
	{
		return false; // $1A clip window (default = full area)
	}
	uint8_t Idx;
	if (C.m_bLoRes)
	{
		// LoRes is 128x96, scaled 2x into the 256x192 area, in bank 5 (the same
		// memory the ULA reads). NR_6A bit 5 = 1: 16 colours, 2 pixels per byte
		// (high nibble = left), 6144 bytes at the selected bank-5 display file.
		// NR_6A bit 5 = 0: 256 colours, 1 byte per pixel, lines 0..47 at bank-5
		// offset $0000-$17FF and lines 48..95 at $2000-$37FF. NR_6A bits 3:0 =
		// palette offset added to the pixel index; palette 0 (ULA); a pixel is
		// transparent when its palette entry's Hi byte == NR_14. Scroll NR_32/33.
		const int LoX = X >> 1, LoY = Y >> 1;
		const int SrcX = (LoX + C.m_LoScrollX) % 128;
		const int SrcY = (LoY + C.m_LoScrollY) % 96;
		if (C.m_bLo16Colour)
		{
			const int ByteIdx = SrcY * 64 + (SrcX >> 1);
			const uint8_t B = C.m_LoVram[ByteIdx];
			const uint8_t Nib = (SrcX & 1) ? (B & 0x0F) : ((B >> 4) & 0x0F);
			Idx = uint8_t(Nib + C.m_LoPalOffset);
		}
		else
		{
			const int BankOffset = (SrcY < 48) ? (SrcY * 128) : (0x2000 + (SrcY - 48) * 128);
			Idx = uint8_t(C.m_LoVram[BankOffset + SrcX] + C.m_LoPalOffset);
		}
	}
	else if (C.m_bTimex)
	{
		// SCLD slot: PaletteTimex colour, no Next transparency semantics.
		Out = TimexUlaSlotPixel(C.m_UlaVram, C.m_ScldMode, X, Y);
		return true;
	}
	else if (C.m_bNextScld)
	{
		// Next in a Timex display mode: SCLD decode for the index, then the
		// shared Next palette + transparency tail below.
		Idx = TimexUlaSlotIndex(C.m_UlaVram, C.m_ScldMode, X, Y);
	}
	else
	{
		// Classic ULA with NR_26/27 hardware scroll: the pixel and attribute
		// fetch both move by (ScrollX,ScrollY), wrapping at 256 / 192. Scroll 0
		// (the common case) reduces to X,Y directly.
		const int SrcX = (X + C.m_UlaScrollX) & 255;
		const int SrcY = (Y + C.m_UlaScrollY) % 192;
		const int Col = SrcX >> 3, Bit = SrcX & 7;
		const int kY76 = (SrcY >> 6) & 0x03, kY53 = (SrcY >> 3) & 0x07, kY20 = SrcY & 0x07;
		const int PixRow = (kY76 << 11) | (kY20 << 8) | (kY53 << 5);
		const int AttrRow = 0x1800 + ((SrcY >> 3) << 5);
		const uint8_t Pix = C.m_UlaVram[PixRow + Col];
		const uint8_t Attr = C.m_UlaVram[AttrRow + Col];
		const bool bInkPix = ((Pix >> (7 - Bit)) & 0x01) != 0;
		if (C.m_bUlaNext)
		{
			// ULANext: ink = attr & mask -> palette 0..mask; paper = the bits
			// above the mask -> palette entries 128+. Mask is contiguous
			// (1/3/7/.../255), so its bit-length is the paper shift.
			const uint8_t Mask = C.m_UlaNextInkMask;
			int Shift = 0;
			for (uint8_t M = Mask; M; M >>= 1)
			{
				++Shift;
			}
			Idx = bInkPix ? uint8_t(Attr & Mask) : uint8_t(128 + (Attr >> Shift));
		}
		else
		{
			// the ULA palette holds the inks at 0..15 and the papers at
			// 16..31 (bright adds 8 to either); the border is paper 16+
			const uint8_t Ink = uint8_t((Attr & 0x07) | ((Attr & 0x40) >> 3));
			const uint8_t Paper = uint8_t(16 + (((Attr >> 3) & 0x07) | ((Attr & 0x40) >> 3)));
			Idx = bInkPix ? Ink : Paper;
		}
	}
	if (C.m_UlaPal[Idx][0] == C.m_TransparentHi)
	{
		return false;
	}
	Out = DecodeNxEntry(C.m_UlaPal[Idx][0], C.m_UlaPal[Idx][1]);
	return true;
}

// The "U slot": the ULA and the tilemap composed together. The tilemap shares
// this slot and sits above or below the ULA per pixel (attribute bit 0). Returns
// false (transparent) only when BOTH are transparent. With no tilemap it is just
// UlaSlotPixelAt (one branch).
inline bool UlaTilemapPixelAt(const FrameCtx& C, int Cx, int Cy, Rgb888& Out)
{
	Rgb888 Ula;
	const bool bUla = UlaSlotPixelAt(C, Cx, Cy, Ula);
	if (!C.m_Tilemap.m_bEnabled)
	{
		if (bUla)
		{
			Out = Ula;
			return true;
		}
		return false;
	}
	const TilePixel Tp = TilemapPixelAt(C.m_Tilemap, Cx, Cy);
	const Rgb888 Tile = Tp.m_bOpaque ? DecodeNxEntry(Tp.m_Hi, Tp.m_Lo) : Rgb888{0, 0, 0};
	// $68 bit 0 stencil: paint only where BOTH are opaque, colour = ULA AND tile.
	if (C.m_bStencil)
	{
		if (bUla && Tp.m_bOpaque)
		{
			Out = Rgb888{uint8_t(Ula.m_R & Tile.m_R), uint8_t(Ula.m_G & Tile.m_G), uint8_t(Ula.m_B & Tile.m_B)};
			return true;
		}
		return false;
	}
	if (bUla && Tp.m_bOpaque)
	{
		Out = Tp.m_bUlaOverTile ? Ula : Tile;
		return true;
	}
	if (bUla)
	{
		Out = Ula;
		return true;
	}
	if (Tp.m_bOpaque)
	{
		Out = Tile;
		return true;
	}
	return false;
}

// The tilemap on its own (the blend modes split the U slot into its two
// layers): opaque tile pixels only.
inline bool TilemapOnlyPixelAt(const FrameCtx& C, int Cx, int Cy, Rgb888& Out)
{
	if (!C.m_Tilemap.m_bEnabled)
	{
		return false;
	}
	const TilePixel Tp = TilemapPixelAt(C.m_Tilemap, Cx, Cy);
	if (!Tp.m_bOpaque)
	{
		return false;
	}
	Out = DecodeNxEntry(Tp.m_Hi, Tp.m_Lo);
	return true;
}

// Blend `Over` onto `Under` - mirrors PutComp's non-Overwrite math exactly.
inline Rgb888 ApplyBlend(Rgb888 Under, Rgb888 Over, BlendMode Mode)
{
	if (Mode == BlendMode::kBlendModeOverwrite)
	{
		return Over;
	}
	const uint8_t ExR = Reduce3(Under.m_R), ExG = Reduce3(Under.m_G), ExB = Reduce3(Under.m_B);
	const uint8_t InR = Reduce3(Over.m_R), InG = Reduce3(Over.m_G), InB = Reduce3(Over.m_B);
	if (Mode == BlendMode::kBlendModeAdditiveClamp7)
	{
		return {Expand3(uint8_t(std::min(7, ExR + InR))), Expand3(uint8_t(std::min(7, ExG + InG))),
			Expand3(uint8_t(std::min(7, ExB + InB)))};
	}
	return {Expand3(uint8_t((ExR + InR) >> 1)), Expand3(uint8_t((ExG + InG) >> 1)), Expand3(uint8_t((ExB + InB) >> 1))};
}

// Compose one composite pixel from live state, folding the active layers in the
// NR_15 bottom-to-top order; the "S" slot reads the pre-rasterised sprite buffer.
inline Rgb888 ComposePixel(const FrameCtx& C, int Cx, int Cy)
{
	// Bottom-to-top layer steps: which slot + blend mode. 'S' = sprite (skipped),
	// 'U' = the ULA/tilemap composite, 'A' = the ULA alone, 'T' = the tilemap
	// alone, '-' = unused. The blend modes "(U|T)S(T|U)(B+L)" (core 3.1.1) add
	// only the blend layer B chosen by $68 bits 6:5 to Layer 2; the other
	// layer is drawn as-is in front of the sprites.
	struct Step
	{
		char m_Slot;
		BlendMode m_Mode;
	};
	Step Steps[4] = {{'-', BlendMode::kBlendModeOverwrite}, {'-', BlendMode::kBlendModeOverwrite},
		{'-', BlendMode::kBlendModeOverwrite}, {'-', BlendMode::kBlendModeOverwrite}};
	using LP = MemoryBus::LayerPriority;
	switch (C.m_Priority)
	{
	case LP::kLayerPrioritySlu:
		Steps[0] = {'U', BlendMode::kBlendModeOverwrite};
		Steps[1] = {'L', BlendMode::kBlendModeOverwrite};
		Steps[2] = {'S', BlendMode::kBlendModeOverwrite};
		break;
	case LP::kLayerPriorityLsu:
		Steps[0] = {'U', BlendMode::kBlendModeOverwrite};
		Steps[1] = {'S', BlendMode::kBlendModeOverwrite};
		Steps[2] = {'L', BlendMode::kBlendModeOverwrite};
		break;
	case LP::kLayerPrioritySul:
		Steps[0] = {'L', BlendMode::kBlendModeOverwrite};
		Steps[1] = {'U', BlendMode::kBlendModeOverwrite};
		Steps[2] = {'S', BlendMode::kBlendModeOverwrite};
		break;
	case LP::kLayerPriorityLus:
		Steps[0] = {'S', BlendMode::kBlendModeOverwrite};
		Steps[1] = {'U', BlendMode::kBlendModeOverwrite};
		Steps[2] = {'L', BlendMode::kBlendModeOverwrite};
		break;
	case LP::kLayerPriorityUsl:
		Steps[0] = {'L', BlendMode::kBlendModeOverwrite};
		Steps[1] = {'S', BlendMode::kBlendModeOverwrite};
		Steps[2] = {'U', BlendMode::kBlendModeOverwrite};
		break;
	case LP::kLayerPriorityUls:
		Steps[0] = {'S', BlendMode::kBlendModeOverwrite};
		Steps[1] = {'L', BlendMode::kBlendModeOverwrite};
		Steps[2] = {'U', BlendMode::kBlendModeOverwrite};
		break;
	case LP::kLayerPriorityUlaBlend:
	case LP::kLayerPriorityUlaHalfBlend:
	{
		const BlendMode Blend = (C.m_Priority == LP::kLayerPriorityUlaBlend) ? BlendMode::kBlendModeAdditiveClamp7
																			 : BlendMode::kBlendModeHalfBlend;
		Steps[0] = {'L', BlendMode::kBlendModeOverwrite};
		switch (C.m_BlendSource)
		{
		case 0:
			Steps[1] = {'A', Blend};
			Steps[2] = {'S', BlendMode::kBlendModeOverwrite};
			Steps[3] = {'T', BlendMode::kBlendModeOverwrite};
			break; // ULA blends, tilemap in front
		case 1:
			Steps[1] = {'S', BlendMode::kBlendModeOverwrite};
			Steps[2] = {'U', BlendMode::kBlendModeOverwrite};
			break; // no blending
		case 2:
			Steps[1] = {'U', Blend};
			Steps[2] = {'S', BlendMode::kBlendModeOverwrite};
			break; // the mix blends
		default:
			Steps[1] = {'T', Blend};
			Steps[2] = {'S', BlendMode::kBlendModeOverwrite};
			Steps[3] = {'A', BlendMode::kBlendModeOverwrite};
			break; // tilemap blends, ULA in front
		}
		break;
	}
	}
	Rgb888 Acc = C.m_Border;
	bool bPainted = false;
	for (const Step& S : Steps)
	{
		Rgb888 Px;
		if (S.m_Slot == 'L')
		{
			if (L2PixelAt(C, Cx, Cy, Px))
			{
				Acc = ApplyBlend(Acc, Px, S.m_Mode);
				bPainted = true;
			}
		}
		else if (S.m_Slot == 'U')
		{
			if (UlaTilemapPixelAt(C, Cx, Cy, Px))
			{
				Acc = ApplyBlend(Acc, Px, S.m_Mode);
				bPainted = true;
			}
		}
		else if (S.m_Slot == 'A')
		{
			if (UlaSlotPixelAt(C, Cx, Cy, Px))
			{
				Acc = ApplyBlend(Acc, Px, S.m_Mode);
				bPainted = true;
			}
		}
		else if (S.m_Slot == 'T')
		{
			if (TilemapOnlyPixelAt(C, Cx, Cy, Px))
			{
				Acc = ApplyBlend(Acc, Px, S.m_Mode);
				bPainted = true;
			}
		}
		else if (S.m_Slot == 'S' && C.m_bSpritesActive)
		{
			const size_t P = size_t(Cy * kCompW + Cx - C.m_SprFrom);
			if (C.m_SprPresent[P])
			{
				Acc = C.m_SprColor[P];
				bPainted = true;
			} // sprites always Overwrite
		}
	}
	// $4A transparency fallback: where no layer painted, show the fallback colour
	// in the layer/paper area; the border follows BorderRgb's mode rules.
	if (!bPainted && C.m_bFallbackSet && C.m_PaperRegion.Contains(Cx, Cy))
	{
		return C.m_FallbackRgb;
	}
	return Acc;
}

} // namespace

namespace
{
// The plain frame, row by row: Layer 2 (256 or 320 mode), the tilemap and
// the sprites in one of the six overwrite orders, the ULA off.  The same
// pixels ComposePixel gives, without its per-pixel dispatch, its modulo
// arithmetic and its palette decode: the Layer 2 palette is decoded once
// into a table and the source offsets run along the row.  Anything else
// (the blend orders, the ULA, LoRes, Timex, the 640 modes, the stencil) is
// left to ComposePixel.  Returns false when it does not apply.
bool ComposePlainRows(const FrameCtx& C, uint8_t* Out, int RowPitch)
{
	using LP = MemoryBus::LayerPriority;
	if (C.m_bLoRes || C.m_bTimex || C.m_bNextScld || C.m_bStencil || !C.m_bUlaOff || C.m_bL2Nibble)
	{
		return false;
	}
	// bottom to top: 'L', 'U' (the tilemap alone, the ULA being off), 'S'
	char Order[3];
	switch (C.m_Priority)
	{
	case LP::kLayerPrioritySlu: Order[0] = 'U'; Order[1] = 'L'; Order[2] = 'S'; break;
	case LP::kLayerPriorityLsu: Order[0] = 'U'; Order[1] = 'S'; Order[2] = 'L'; break;
	case LP::kLayerPrioritySul: Order[0] = 'L'; Order[1] = 'U'; Order[2] = 'S'; break;
	case LP::kLayerPriorityLus: Order[0] = 'S'; Order[1] = 'U'; Order[2] = 'L'; break;
	case LP::kLayerPriorityUsl: Order[0] = 'L'; Order[1] = 'S'; Order[2] = 'U'; break;
	case LP::kLayerPriorityUls: Order[0] = 'S'; Order[1] = 'L'; Order[2] = 'U'; break;
	default:
		return false;
	}
	// the Layer 2 palette, decoded once: opaque flag and BGR
	struct Entry
	{
		uint8_t m_B, m_G, m_R, m_bOpaque;
	};
	Entry Lut[256];
	if (C.m_bL2Present)
	{
		for (int I = 0; I < 256; ++I)
		{
			const Rgb888 Rgb = DecodeNxEntry(C.m_L2Pal[I][0], C.m_L2Pal[I][1]);
			Lut[I] = {Rgb.m_B, Rgb.m_G, Rgb.m_R, uint8_t(C.m_L2Pal[I][0] != C.m_TransparentHi)};
		}
	}
	const bool bTiles = C.m_Tilemap.m_bEnabled;
	const bool bSprites = C.m_bSpritesActive;
	const uint8_t* SprPresent = bSprites ? C.m_SprPresent.data() : nullptr;
	const Rgb888* SprColor = bSprites ? C.m_SprColor.data() : nullptr;
	for (int Y = 0; Y < kCompH; ++Y)
	{
		uint8_t* Px = Out + size_t(Y) * size_t(RowPitch);
		// this row's Layer 2 span and source row
		const int LY = Y - C.m_L2OffY;
		const bool bL2Row = C.m_bL2Present && LY >= 0 && LY < C.m_L2h && Y >= C.m_L2Clip.m_Y1 && Y <= C.m_L2Clip.m_Y2;
		int SrcY = 0;
		if (bL2Row)
		{
			SrcY = (LY + C.m_L2ScrollY) % C.m_L2h; // once a row
		}
		const size_t RowBase = size_t(Y) * size_t(kCompW);
		for (int X = 0; X < kCompW; ++X, Px += kCompBytesPerPixel)
		{
			Rgb888 Acc = C.m_Border;
			bool bPainted = false;
			for (const char Slot : Order)
			{
				if (Slot == 'L')
				{
					if (!bL2Row || X < C.m_L2Clip.m_X1 || X > C.m_L2Clip.m_X2)
					{
						continue;
					}
					const int LX = X - C.m_L2OffX;
					if (LX < 0 || LX >= C.m_L2w)
					{
						continue;
					}
					int SrcX = LX + C.m_L2ScrollX; // the scroll is below 1024: three wraps at most
					while (SrcX >= C.m_L2w)
					{
						SrcX -= C.m_L2w;
					}
					const int Off = C.m_bL2ColumnMajor ? (SrcX * C.m_L2h + SrcY) : (SrcY * C.m_L2w + SrcX);
					const Entry& E = Lut[C.m_L2Page[Off >> 13][Off & 0x1FFF]];
					if (E.m_bOpaque)
					{
						Acc = Rgb888{E.m_R, E.m_G, E.m_B};
						bPainted = true;
					}
				}
				else if (Slot == 'U')
				{
					if (!bTiles)
					{
						continue;
					}
					const TilePixel Tp = TilemapPixelAt(C.m_Tilemap, X, Y);
					if (Tp.m_bOpaque)
					{
						Acc = DecodeNxEntry(Tp.m_Hi, Tp.m_Lo);
						bPainted = true;
					}
				}
				else if (bSprites && SprPresent[RowBase + size_t(X)])
				{
					Acc = SprColor[RowBase + size_t(X)];
					bPainted = true;
				}
			}
			if (!bPainted && C.m_bFallbackSet && C.m_PaperRegion.Contains(X, Y))
			{
				Acc = C.m_FallbackRgb;
			}
			Px[0] = Acc.m_B;
			Px[1] = Acc.m_G;
			Px[2] = Acc.m_R;
		}
	}
	return true;
}
} // namespace

void RenderCompositeBgr(const MemoryBus& Mem, const SpritesState* Sprites, uint8_t* OutBgrTopDown, int RowPitch)
{
	const FrameCtx Ctx = BuildFrameCtx(Mem, Sprites);
	if (ComposePlainRows(Ctx, OutBgrTopDown, RowPitch))
	{
		return;
	}
	for (int Y = 0; Y < kCompH; ++Y)
	{
		uint8_t* Px = OutBgrTopDown + size_t(Y) * size_t(RowPitch);
		for (int X = 0; X < kCompW; ++X, Px += kCompBytesPerPixel)
		{
			const Rgb888 C = ComposePixel(Ctx, X, Y);
			Px[0] = C.m_B;
			Px[1] = C.m_G;
			Px[2] = C.m_R;
		}
	}
}

void RenderCompositeBgrHires(const MemoryBus& Mem, const SpritesState* Sprites, uint8_t* OutBgrTopDown, int RowPitch)
{
	// The canvas is 320 wide and the 640x256 Layer 2 mode shows only the left
	// pixel of each pair on it. For a 640-wide output the frame is composed
	// twice, once per nibble, and the two are interleaved; every other layer
	// simply doubles. Not the 640 mode: one pass, doubled.
	const bool b640 = Mem.DetectActiveLayer() == MemoryBus::ActiveLayer::kActiveLayerL2640;
	std::vector<uint8_t> Left(size_t(kCompW) * 3 * kCompH), Right;
	RenderCompositeBgr(Mem, Sprites, Left.data(), kCompW * 3);
	if (b640)
	{
		Right.resize(Left.size());
		g_bL2RightNibble = true;
		RenderCompositeBgr(Mem, Sprites, Right.data(), kCompW * 3);
		g_bL2RightNibble = false;
	}
	const uint8_t* R = b640 ? Right.data() : Left.data();
	for (int Y = 0; Y < kCompH; ++Y)
	{
		const uint8_t* L = Left.data() + size_t(Y) * kCompW * 3;
		const uint8_t* Rr = R + size_t(Y) * kCompW * 3;
		uint8_t* O = OutBgrTopDown + size_t(Y) * RowPitch;
		for (int X = 0; X < kCompW; ++X, L += 3, Rr += 3, O += 6)
		{
			O[0] = L[0]; O[1] = L[1]; O[2] = L[2];
			O[3] = Rr[0]; O[4] = Rr[1]; O[5] = Rr[2];
		}
	}
}

bool WriteCompositeBmp(const MemoryBus& Mem, const SpritesState* Sprites, const char* Path, std::string& OutError)
{
	std::vector<uint8_t> Image(size_t(kCompImageSize), 0);

	// Pixel-major compositor: ComposePixel folds every layer from live state in
	// the NR_15 order, one pixel at a time, from a FrameCtx built once per frame.
	// This is the ONLY compositor: --bmp-out uses it as an end-of-frame snapshot,
	// the eager BeamDrawer (--pixel-accurate / --video-out) calls it per span at
	// its beam boundaries.
	{
		const FrameCtx Ctx = BuildFrameCtx(Mem, Sprites);
		for (int Y = 0; Y < kCompH; ++Y)
		{
			for (int X = 0; X < kCompW; ++X)
			{
				PutComp(Image.data(), X, Y, ComposePixel(Ctx, X, Y));
			}
		}
	}
	// Write 24-bit BGR BMP.
	constexpr int FileHeader = 14;
	constexpr int InfoHeader = 40;
	const int PixelOffset = FileHeader + InfoHeader;
	const int FileSize = PixelOffset + kCompImageSize;

	uint8_t Header[FileHeader + InfoHeader] = {};
	Header[0] = 'B';
	Header[1] = 'M';
	WriteU32LE(Header + 2, uint32_t(FileSize));
	WriteU32LE(Header + 10, uint32_t(PixelOffset));
	WriteU32LE(Header + 14, uint32_t(InfoHeader));
	WriteU32LE(Header + 18, uint32_t(kCompW));
	WriteU32LE(Header + 22, uint32_t(kCompH));
	WriteU16LE(Header + 26, 1);
	WriteU16LE(Header + 28, 24); // 24 bpp
	WriteU32LE(Header + 30, 0); // BI_RGB
	WriteU32LE(Header + 34, uint32_t(kCompImageSize));
	WriteU32LE(Header + 38, 2835);
	WriteU32LE(Header + 42, 2835);
	WriteU32LE(Header + 46, 0);
	WriteU32LE(Header + 50, 0);

	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open composite bmp output: ") + Path;
		return false;
	}
	const bool bOk = fwrite(Header, 1, sizeof(Header), F) == sizeof(Header) &&
					 fwrite(Image.data(), 1, Image.size(), F) == Image.size();
	fclose(F);
	if (!bOk)
	{
		OutError = std::string("short write to composite bmp: ") + Path;
		return false;
	}
	return true;
}

// ============================================================================
// BeamDrawer - eager pixel-exact rendering (see bmp.h). Defined here so it can
// reuse the file-local ComposePixel / BuildFrameCtx / PutComp helpers.
// ============================================================================

void BeamDrawer::Configure(uint64_t FrameIntPeriod, int LinesPerFrame, int TopBorderLines)
{
	m_Period = FrameIntPeriod;
	m_Lines = LinesPerFrame ? LinesPerFrame : 312;
	m_TopBorder = TopBorderLines;
	m_CyclesPerLine = m_Period / uint64_t(m_Lines);
	m_Image.assign(size_t(kCompImageSize), 0);
	m_Completed.assign(size_t(kCompImageSize), 0);
	m_Cursor = 0;
	m_FrameOrigin = ~uint64_t(0);
	m_bHaveCompleted = false;
}

void BeamDrawer::Begin(uint64_t Origin)
{
	m_FrameOrigin = Origin;
	m_Cursor = 0;
}

// Composite pixel index the beam is scanning at absolute `Cycle`. Composite Y
// maps to raster line (TopBorder_ - 32) + Y so paper row 0 (composite Y=32)
// lands on raster line TopBorder_; X spreads the line's cycles across the 320px
// canvas. Clamped to [0, COMP_W*COMP_H].
int BeamDrawer::PixelForCycle(uint64_t Cycle) const
{
	if (m_CyclesPerLine == 0 || Cycle <= m_FrameOrigin)
	{
		return 0;
	}
	const uint64_t Rel = Cycle - m_FrameOrigin;
	const int L = int(Rel / m_CyclesPerLine);
	const int Y = L - (m_TopBorder - 32);
	if (Y < 0)
	{
		return 0;
	}
	if (Y >= kCompH)
	{
		return kCompW * kCompH;
	}
	int X = int((Rel % m_CyclesPerLine) * uint64_t(kCompW) / m_CyclesPerLine);
	if (X >= kCompW)
	{
		X = kCompW - 1;
	}
	return Y * kCompW + X;
}

void BeamDrawer::DrawSpan(const MemoryBus& Mem, const SpritesState* Sprites, int From, int To)
{
	const int N = kCompW * kCompH;
	if (From < 0)
	{
		From = 0;
	}
	if (To > N)
	{
		To = N;
	}
	if (To <= From)
	{
		return;
	}
	const FrameCtx Ctx = BuildFrameCtx(Mem, Sprites, From, To);
	for (int Idx = From; Idx < To; ++Idx)
	{
		const int X = Idx % kCompW, Y = Idx / kCompW;
		PutComp(m_Image.data(), X, Y, ComposePixel(Ctx, X, Y));
	}
}

void BeamDrawer::FlushTo(const MemoryBus& Mem, const SpritesState* Sprites, uint64_t Cycle)
{
	if (m_Period == 0)
	{
		return;
	}
	// Track the LIVE frame period: turbo demos change it mid-run (NR $07), so a
	// fixed Configure-time period would mis-map cycles->pixels and mis-roll frames
	// on turbo demos. CLI captures can gate this on BeamArmCycle_; a live
	// frontend leaves it armed for every frame.
	const uint64_t LiveP = Mem.GetFrameIntPeriodHint();
	if (LiveP && LiveP != m_Period)
	{
		m_Period = LiveP;
		m_CyclesPerLine = LiveP / uint64_t(m_Lines);
	}
	if (m_FrameOrigin == ~uint64_t(0))
	{
		Begin(Mem.FrameOriginFor(Cycle, m_Period));
	}
	// Roll completed frames over. Draw the finishing frame's tail from the
	// current (pre-boundary) live state - no display write has happened since its
	// last boundary, so that state is exactly its end-of-frame state. Re-align to
	// the phase grid like the copper (avoids drift when the int phase moves).
	int Guard = 0;
	while (Cycle >= m_FrameOrigin + m_Period && Guard++ < 8)
	{
		DrawSpan(Mem, Sprites, m_Cursor, kCompW * kCompH);
		m_Completed = m_Image;
		m_bHaveCompleted = true;
		Begin(Mem.FrameOriginFor(m_FrameOrigin + m_Period + m_Period / 2, m_Period));
	}
	const int Target = PixelForCycle(Cycle);
	if (Target > m_Cursor)
	{
		DrawSpan(Mem, Sprites, m_Cursor, Target);
		m_Cursor = Target;
	}
}

void BeamDrawer::Finish(const MemoryBus& Mem, const SpritesState* Sprites)
{
	if (m_FrameOrigin == ~uint64_t(0))
	{
		return;
	}
	DrawSpan(Mem, Sprites, m_Cursor, kCompW * kCompH);
	m_Cursor = kCompW * kCompH;
	m_Completed = m_Image;
	m_bHaveCompleted = true;
}

bool BeamDrawer::CopyCompletedBgr(uint8_t* OutBgrTopDown, int RowPitch) const
{
	return m_bHaveCompleted && CopyCaptureBgr(OutBgrTopDown, RowPitch);
}

bool BeamDrawer::CopyCaptureBgr(uint8_t* OutBgrTopDown, int RowPitch) const
{
	const auto& Img = m_bHaveCompleted ? m_Completed : m_Image;
	if (Img.size() != size_t(kCompImageSize) || !OutBgrTopDown || RowPitch < kCompW * 3)
	{
		return false;
	}
	for (int Y = 0; Y < kCompH; ++Y)
	{
		memcpy(OutBgrTopDown + size_t(Y) * RowPitch, Img.data() + size_t(kCompH - 1 - Y) * kCompW * 3, kCompW * 3);
	}
	return true;
}

bool BeamDrawer::WriteBmp(const char* Path, std::string& OutError) const
{
	const std::vector<uint8_t>& Img = m_bHaveCompleted ? m_Completed : m_Image;
	if (Img.size() != size_t(kCompImageSize))
	{
		OutError = "beam drawer has no frame to write";
		return false;
	}
	constexpr int FileHeader = 14, InfoHeader = 40;
	const int PixelOffset = FileHeader + InfoHeader;
	const int FileSize = PixelOffset + kCompImageSize;
	uint8_t Header[FileHeader + InfoHeader] = {};
	Header[0] = 'B';
	Header[1] = 'M';
	WriteU32LE(Header + 2, uint32_t(FileSize));
	WriteU32LE(Header + 10, uint32_t(PixelOffset));
	WriteU32LE(Header + 14, uint32_t(InfoHeader));
	WriteU32LE(Header + 18, uint32_t(kCompW));
	WriteU32LE(Header + 22, uint32_t(kCompH));
	WriteU16LE(Header + 26, 1);
	WriteU16LE(Header + 28, 24);
	WriteU32LE(Header + 34, uint32_t(kCompImageSize));
	WriteU32LE(Header + 38, 2835);
	WriteU32LE(Header + 42, 2835);
	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open beam bmp output: ") + Path;
		return false;
	}
	const bool bOk =
		fwrite(Header, 1, sizeof(Header), F) == sizeof(Header) && fwrite(Img.data(), 1, Img.size(), F) == Img.size();
	fclose(F);
	if (!bOk)
	{
		OutError = std::string("short write to beam bmp: ") + Path;
		return false;
	}
	return true;
}

// ---- Paper-area luminance, for the motion / histogram metrics ---------------
namespace
{
inline uint8_t LumaOf(Rgb888 C)
{
	// Rec.601, integer: (77R + 150G + 29B) >> 8
	return uint8_t((77u * C.m_R + 150u * C.m_G + 29u * C.m_B) >> 8);
}
} // namespace

bool RenderPaperLuma(const MemoryBus& Mem, const SpritesState* Sprites, uint8_t* OutLuma, std::string& OutError)
{
	if (OutLuma == nullptr)
	{
		OutError = "RenderPaperLuma: null buffer";
		return false;
	}

	if (Mem.IsNext())
	{
		const FrameCtx Ctx = BuildFrameCtx(Mem, Sprites);
		for (int Y = 0; Y < 192; ++Y)
		{
			for (int X = 0; X < 256; ++X)
			{
				OutLuma[Y * 256 + X] = LumaOf(ComposePixel(Ctx, X + 32, Y + 32));
			}
		}
		return true;
	}

	const uint8_t Port7FFD = Mem.GetPort7FFD();
	const uint8_t* Vram = Mem.Ram16KBank((Port7FFD & 0x08) ? 7 : 5);

	if (Mem.IsTimex() || Mem.IsZxUno())
	{
		const uint8_t Scld = Mem.GetScldMode();
		for (int Y = 0; Y < 192; ++Y)
		{
			for (int X = 0; X < 256; ++X)
			{
				OutLuma[Y * 256 + X] = LumaOf(TimexUlaSlotPixel(Vram, Scld, X, Y));
			}
		}
		return true;
	}

	// Classic ULA: the same decode as WriteSpectrumScreenBmp, through the
	// classic 8-colour palette (bright = full, else 0xD7).
	static constexpr uint8_t Lvl[2] = {0xD7, 0xFF};
	for (int Y = 0; Y < 192; ++Y)
	{
		const int Third = Y / 64, RowInThird = (Y / 8) % 8, ScanInRow = Y % 8, CharRow = Y / 8;
		for (int XByte = 0; XByte < 32; ++XByte)
		{
			const uint8_t Pix = Vram[(Third << 11) | (ScanInRow << 8) | (RowInThird << 5) | XByte];
			const uint8_t Attr = Vram[0x1800 + CharRow * 32 + XByte];
			const int B = (Attr & 0x40) ? 1 : 0;
			const uint8_t Ink = uint8_t(Attr & 0x07), Paper = uint8_t((Attr >> 3) & 0x07);
			for (int Bit = 0; Bit < 8; ++Bit)
			{
				const uint8_t Col = ((Pix >> (7 - Bit)) & 1) ? Ink : Paper;
				const Rgb888 C{
					uint8_t((Col & 2) ? Lvl[B] : 0), uint8_t((Col & 4) ? Lvl[B] : 0), uint8_t((Col & 1) ? Lvl[B] : 0)};
				OutLuma[Y * 256 + XByte * 8 + Bit] = LumaOf(C);
			}
		}
	}
	return true;
}
