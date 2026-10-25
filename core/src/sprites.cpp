#include "sprites.h"

#include "memory.h"

void SelectSpriteSlot(SpritesState& InOutSprites, uint8_t Value)
{
	InOutSprites.m_AttrSlot = Value & 0x7F;
	InOutSprites.m_AttrByteIdx = 0;
	InOutSprites.m_PatternSlot = Value & 0x3F;
	InOutSprites.m_PatternByteIdx = (Value & 0x80) ? 128 : 0;
	++InOutSprites.m_SlotSelects;
}

void WriteSpriteSlotSelect(SpritesState& InOutSprites, uint8_t Value, MemoryBus& InOutMem)
{
	SelectSpriteSlot(InOutSprites, Value);
	InOutMem.SetSpriteIndexFromPort(Value);
}

void WriteSpriteAttrByte(SpritesState& InOutS, uint8_t Slot, int ByteIndex, uint8_t Value)
{
	// NR $35-$39 / $75-$79: write one attribute byte of the slot selected by
	// NR_34. Addresses the slot directly (not through the $57 byte cursor), so
	// it leaves AttrSlot/AttrByteIdx alone. ByteIndex is 0..4.
	if (unsigned(ByteIndex) >= 5)
	{
		return;
	}
	InOutS.m_Slots[Slot & 0x7F].m_Attr.m_Bytes[ByteIndex] = Value;
	++InOutS.m_AttrWrites;
}

void WriteSpriteAttr(SpritesState& InOutSprites, uint8_t Value, MemoryBus& InOutMem)
{
	// The port cursor advances independently; lockstep only mirrors its index.
	const uint8_t Slot = InOutSprites.m_AttrSlot & 0x7F;
	SpriteSlot& Sprite = InOutSprites.m_Slots[Slot];
	Sprite.m_Attr.m_Bytes[InOutSprites.m_AttrByteIdx++] = Value;
	++InOutSprites.m_AttrWrites;
	const uint8_t GroupLength = (Sprite.m_Attr.m_Bytes[3] & 0x40) ? 5 : 4;
	if (InOutSprites.m_AttrByteIdx >= GroupLength)
	{
		InOutSprites.m_AttrByteIdx = 0;
		InOutSprites.m_AttrSlot = uint8_t((Slot + 1) & 0x7F);
		InOutMem.SetSpriteIndexFromPort(InOutSprites.m_AttrSlot);
	}
}

void WriteSpritePattern(SpritesState& InOutS, uint8_t Value)
{
	SpriteSlot& Slot = InOutS.m_Slots[InOutS.m_PatternSlot & 0x3F];
	Slot.m_Pattern[InOutS.m_PatternByteIdx] = Value;
	++InOutS.m_PatternWrites;
	++InOutS.m_PatternByteIdx;
	if (InOutS.m_PatternByteIdx == 0) // wrapped past 255
	{
		InOutS.m_PatternSlot = uint8_t((InOutS.m_PatternSlot + 1) & 0x3F);
	}
}
