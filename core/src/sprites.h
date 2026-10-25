#pragma once
// Next sprite state: 128 attribute slots and 64 256-byte pattern slots.
// Four-byte attributes use X/Y, palette/transform/X8, and visibility/pattern.
// Extended anchors add H/N6/T, X scale (4:3), Y scale (2:1) and Y8 (0).
// https://wiki.specnext.dev/Sprites
// The upper half of m_Slots retains reserved pattern storage for dump v1.

#include <stdint.h>

struct MemoryBus;

struct SpriteAttr
{
	uint8_t m_Bytes[5] = {0, 0, 0, 0, 0};
};

struct SpriteSlot
{
	SpriteAttr m_Attr;
	uint8_t m_Pattern[256] = {};
};

struct SpritesState
{
	SpriteSlot m_Slots[128];

	// Upload state. AttrSlot + AttrByteIdx track the next port-$57 write
	// target; PatternSlot + PatternByteIdx track the next port-$5B write.
	uint8_t m_AttrSlot = 0;
	uint8_t m_AttrByteIdx = 0;
	uint8_t m_PatternSlot = 0;
	uint8_t m_PatternByteIdx = 0;

	// Counters for dump/status.
	uint64_t m_AttrWrites = 0;
	uint64_t m_PatternWrites = 0;
	uint64_t m_SlotSelects = 0;
};

// Port uploads; mirror index changes only when NR_09 lockstep is set.
void WriteSpriteSlotSelect(SpritesState& InOutSprites, uint8_t Value, MemoryBus& InOutMem);
void WriteSpriteAttr(SpritesState& InOutSprites, uint8_t Value, MemoryBus& InOutMem);
void WriteSpritePattern(SpritesState& InOutS, uint8_t Value);

// SelectSpriteSlot resets both port cursors ($303B or a locked NR_34 write).
// WriteSpriteAttrByte addresses one slot/attribute without moving either cursor.
void SelectSpriteSlot(SpritesState& InOutSprites, uint8_t Value);
void WriteSpriteAttrByte(SpritesState& InOutS, uint8_t Slot, int ByteIndex, uint8_t Value);
