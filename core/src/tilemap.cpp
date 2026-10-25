#include "tilemap.h"

#include "memory.h"

TilemapParams BuildTilemapParams(const MemoryBus& Mem)
{
	TilemapParams T;
	const uint8_t Ctrl = Mem.GetNextReg(0x6B);
	T.m_bEnabled = (Ctrl & 0x80) != 0;
	if (!T.m_bEnabled)
	{
		return T;
	}

	T.m_bWide = (Ctrl & 0x40) != 0;
	T.m_bNoAttr = (Ctrl & 0x20) != 0;
	T.m_bText = (Ctrl & 0x08) != 0;
	T.m_bB512 = (Ctrl & 0x02) != 0;
	T.m_bForceOverUla = (Ctrl & 0x01) != 0;

	T.m_Pal = Mem.GetPalette((Ctrl & 0x10) ? 7 : 3); // bit 4 selects palette 3/7
	T.m_DefaultAttr = Mem.GetNextReg(0x6C);

	// Map / tile-definition bases: bits 5:0 = MSB of the byte offset inside the
	// 16K bank; bit 7 selects bank 7 instead of bank 5.
	const uint8_t MapBaseReg = Mem.GetNextReg(0x6E);
	const uint8_t DefBaseReg = Mem.GetNextReg(0x6F);
	T.m_Map = Mem.Ram16KBank((MapBaseReg & 0x80) ? 7 : 5) + ((MapBaseReg & 0x3F) << 8);
	T.m_Tiles = Mem.Ram16KBank((DefBaseReg & 0x80) ? 7 : 5) + ((DefBaseReg & 0x3F) << 8);

	T.m_ScrollX = uint16_t(((Mem.GetNextReg(0x2F) & 0x03) << 8) | Mem.GetNextReg(0x30));
	T.m_ScrollY = Mem.GetNextReg(0x31);
	T.m_TransparentIdx = uint8_t(Mem.GetNextReg(0x4C) & 0x0F);

	const uint8_t* Clip = Mem.GetClipWindow(3); // $1B tilemap clip
	if (Mem.IsClipSet(3))
	{
		T.m_ClipX1 = Clip[0] * 2;
		T.m_ClipX2 = Clip[1] * 2 + 1; // X doubled (0..159 -> 0..319)
		T.m_ClipY1 = Clip[2];
		T.m_ClipY2 = Clip[3];
	}
	return T;
}

TilePixel TilemapPixelAt(const TilemapParams& T, int X, int Y)
{
	TilePixel R;
	if (!T.m_bEnabled)
	{
		return R;
	}
	if (X < T.m_ClipX1 || X > T.m_ClipX2 || Y < T.m_ClipY1 || Y > T.m_ClipY2)
	{
		return R;
	}

	const int Cols = T.m_bWide ? 80 : 40;
	const int MapW = Cols * 8; // 320 or 640 tilemap pixels wide
	// 80-column mode packs 640 pixels across the 320-wide canvas: sample the
	// left sub-pixel of each pair (same compromise the L2 640 path makes).
	const int SampleX = T.m_bWide ? (X * 2) : X;
	int MapX = (SampleX + T.m_ScrollX) % MapW;
	int MapY = (Y + T.m_ScrollY) % 256;

	const int TileX = MapX / 8, TileY = MapY / 8;
	int Px = MapX & 7, Py = MapY & 7;

	const int EntryBytes = T.m_bNoAttr ? 1 : 2;
	const int MapOff = (TileY * Cols + TileX) * EntryBytes;
	const uint8_t TileIdxByte = T.m_Map[MapOff];
	const uint8_t Attr = T.m_bNoAttr ? T.m_DefaultAttr : T.m_Map[MapOff + 1];

	// 512-tile mode ($6B bit 1) uses attribute bit 0 as tile-index bit 8, so it
	// has no per-tile ULA-over-tile priority bit (tile priority unless forced).
	const int TileIdx = TileIdxByte | (T.m_bB512 ? ((Attr & 1) << 8) : 0);

	// Mirror / rotate (attribute bits 3/2/1): mirror first, then 90 deg CW.
	// Text mode has no transform bits - the whole attribute (bits 7:1) is the
	// palette offset - so it is skipped there.
	if (!T.m_bText)
	{
		if (Attr & 0x08)
		{
			Px = 7 - Px;
		}
		if (Attr & 0x04)
		{
			Py = 7 - Py;
		}
		if (Attr & 0x02)
		{
			const int Tmp = Px;
			Px = 7 - Py;
			Py = Tmp;
		}
	}

	uint8_t Nib;
	if (T.m_bText)
	{
		// Text mode: 1bpp glyphs, 8 bytes per tile; pixel value 0/1.
		const uint8_t B = T.m_Tiles[TileIdx * 8 + Py];
		Nib = uint8_t((B >> (7 - Px)) & 0x01);
	}
	else
	{
		// 4bpp tiles: 32 bytes each, 2 pixels per byte (high nibble = left).
		const uint8_t B = T.m_Tiles[TileIdx * 32 + Py * 4 + (Px >> 1)];
		Nib = (Px & 1) ? uint8_t(B & 0x0F) : uint8_t((B >> 4) & 0x0F);
	}

	if (Nib == T.m_TransparentIdx)
	{
		return R; // transparent pixel
	}

	// Colour index = (palette offset << 4) | pixel nibble, into palette 3/7.
	// Text mode uses the 7-bit palette offset (attribute bits 7:1) << 1 | pixel.
	uint8_t Entry;
	if (T.m_bText)
	{
		Entry = uint8_t(((Attr >> 1) << 1) | Nib);
	}
	else
	{
		Entry = uint8_t((((Attr >> 4) & 0x0F) << 4) | Nib);
	}

	R.m_Hi = T.m_Pal[Entry][0];
	R.m_Lo = T.m_Pal[Entry][1];
	R.m_bOpaque = true;
	R.m_bUlaOverTile = (!T.m_bB512 && (Attr & 0x01) != 0) && !T.m_bForceOverUla;
	return R;
}
