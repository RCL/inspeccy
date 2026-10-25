#pragma once
// ZX Spectrum Next tilemap layer.
//
// A character-cell layer that shares the "U slot" with the classic ULA and can
// sit above or below it per tile. It has NO state of its own beyond NextRegs and
// the two RAM regions it points at (the map and the tile definitions, in bank 5
// or 7) - so this module is pure functions over MemoryBus state, resolved once
// per frame into TilemapParams and sampled per pixel by TilemapPixelAt.
//
// Registers: $6B control, $6C default attribute, $6E map base, $6F tile-def base,
// $2F/$30/$31 scroll, $4C transparency index, palette 3 (or 7 via $6B bit 4).

#include <stdint.h>

struct MemoryBus;

struct TilemapParams
{
	bool m_bEnabled = false;
	bool m_bWide = false; // $6B bit 6: 80x32 (else 40x32)
	bool m_bNoAttr = false; // $6B bit 5: 1 byte/entry, attribute from $6C
	bool m_bText = false; // $6B bit 3: 1bpp text tiles
	bool m_bB512 = false; // $6B bit 1: attribute bit 0 = tile index bit 8
	bool m_bForceOverUla = false; // $6B bit 0: tilemap always over ULA
	const uint8_t* m_Map = nullptr; // host ptr into bank 5/7 + $6E offset
	const uint8_t* m_Tiles = nullptr; // host ptr into bank 5/7 + $6F offset
	const uint8_t (*m_Pal)[2] = nullptr; // palette 3 or 7
	uint8_t m_DefaultAttr = 0; // $6C
	uint16_t m_ScrollX = 0; // $2F:$30
	uint8_t m_ScrollY = 0; // $31
	uint8_t m_TransparentIdx = 15; // $4C & 15
	int m_ClipX1 = 0, m_ClipX2 = 319, m_ClipY1 = 0, m_ClipY2 = 255; // composite coords
};

// One composed tilemap pixel. Hi/Lo are the 9-bit palette entry (decode with the
// same DecodeNxEntry the other layers use). bUlaOverTile = the ULA wins this
// pixel when both are opaque (attribute bit 0 set and not forced over).
struct TilePixel
{
	uint8_t m_Hi = 0, m_Lo = 0;
	bool m_bOpaque = false;
	bool m_bUlaOverTile = false;
};

TilemapParams BuildTilemapParams(const MemoryBus& Mem);
TilePixel TilemapPixelAt(const TilemapParams& T, int X, int Y);
