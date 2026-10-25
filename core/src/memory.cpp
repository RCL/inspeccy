#include "memory.h"
#include "sprites.h"

#if INSPECCY_TRACE_SUPPORT
#include "trace.h"
#endif

#include <string.h>

DebugMapping MemoryBus::InspectMapping(uint16_t Addr, bool Write) const
{
	const auto& Slot = m_Slots[Addr >> 13];
	const uint8_t* Ptr = Write ? Slot.m_WrPtr : Slot.m_Ptr;
	DebugMapping R;
	R.m_Writable = Slot.m_bWritable && (Write || Ptr == Slot.m_WrPtr);
	R.m_Offset = Addr & 8191;
	if (!Ptr || (Write && !Slot.m_bWritable))
	{
		return R;
	}
	// Integer comparisons avoid relational comparison of unrelated pointers.
	const uintptr_t P = reinterpret_cast<uintptr_t>(Ptr) + R.m_Offset;
	auto Within = [&](const auto& Pool)
	{
		const uintptr_t Base = reinterpret_cast<uintptr_t>(Pool.data());
		return P >= Base && P - Base < Pool.size();
	};
	if (Within(m_Ram) || Within(m_Rom))
	{
		const bool Ram = Within(m_Ram);
		const uintptr_t Base = reinterpret_cast<uintptr_t>(Ram ? m_Ram.data() : m_Rom.data());
		R.m_Kind = Ram ? DebugMapping::Ram : DebugMapping::Rom;
		R.m_Page = int((P - Base) / 8192);
		R.m_Offset = uint16_t((P - Base) % 8192);
	}
	else
	{
		R.m_Kind = Within(m_OpenBusPage) ? DebugMapping::OpenBus : DebugMapping::Other;
	}
	return R;
}

bool MemoryBus::DebugPeek(uint16_t Addr, uint8_t& Value) const
{
	const auto* Ptr = m_Slots[Addr >> 13].m_Ptr;
	if (!Ptr)
	{
		return false;
	}
	Value = Ptr[Addr & 8191];
	return true;
}
MemoryBus::MemoryBus()
{
	InitNextRegHooks();
	m_Ram.fill(0);
	m_Rom.fill(0xFF);
	m_bRomLoaded.fill(false);
	m_OpenBusPage.fill(0xFF); // every slot that maps to an unpopulated page reads $FF

	// Set nonzero Next register reset defaults before loading a program.
	// NR_4B selects the transparent sprite pattern index ($E3).
	m_NextRegs[0x4B] = 0xE3;

	// NR_14 is the global transparency colour. Its reset value is $E3;
	// zero would incorrectly hide zero-valued palette entries.
	m_NextRegs[0x14] = 0xE3;

	// Palettes: hardware reset state instead of all-zero. Every palette
	// resets to the identity mapping (entry i -> 8-bit colour i, 9th bit =
	// OR of the two blue bits); the first 32 entries of the two ULA
	// palettes (0 = PAL1, 4 = PAL2) are the classic Spectrum colours twice
	// over: the ULA palette holds the INKS at 0..15 and the PAPERS at 16..31
	// (attr-derived index: bit0=blue, bit1=red, bit2=green, bit3=bright),
	// and the border is paper 16 + $FE colour. So classic ULA screens
	// composite correctly without any palette writes ever happening, and a
	// program that writes entry 16.. recolours the paper and the border as
	// it does on the board.
	for (int P = 0; P < 8; ++P)
	{
		for (int IdxI = 0; IdxI < 256; ++IdxI)
		{
			m_Palettes[P][IdxI][0] = uint8_t(IdxI);
			m_Palettes[P][IdxI][1] = uint8_t((IdxI & 0x03) ? 0x01 : 0x00);
		}
	}
	for (const int P : {0, 4})
	{
		for (int IdxI = 0; IdxI < 32; ++IdxI)
		{
			const uint8_t Level = (IdxI & 0x08) ? 7 : 6; // bright : normal
			const uint8_t B = (IdxI & 0x01) ? Level : 0;
			const uint8_t R = (IdxI & 0x02) ? Level : 0;
			const uint8_t G = (IdxI & 0x04) ? Level : 0;
			m_Palettes[P][IdxI][0] = uint8_t((R << 5) | (G << 2) | (B >> 1));
			m_Palettes[P][IdxI][1] = uint8_t(B & 0x01);
		}
	}
}

void MemoryBus::SetRom128(const uint8_t* Data)
{
	if (Data == nullptr)
	{
		return;
	}
	memcpy(&m_Rom[kRomPage128kEditor * kSlotSize], Data, 2 * kSlotSize);
	m_bRomLoaded[0] = true;
}

void MemoryBus::SetRom48(const uint8_t* Data)
{
	if (Data == nullptr)
	{
		return;
	}
	memcpy(&m_Rom[kRomPage48kBasic * kSlotSize], Data, 2 * kSlotSize);
	m_bRomLoaded[1] = true;
}

void MemoryBus::SetRomPlus3(int RomIndex, const uint8_t* Data16K)
{
	if (Data16K == nullptr || RomIndex < 0 || RomIndex > 3)
	{
		return;
	}
	memcpy(&m_Rom[(kRomPagePlus30 + RomIndex * 2) * kSlotSize], Data16K, 2 * kSlotSize);
	m_bRomPlus3Loaded = true;
}

// +2A/+3 paging. Two ports: $7FFD (as 128K: RAM bank at $C000 in bits 2:0, low
// ROM-select bit 4, paging lock bit 5) and $1FFD. When $1FFD bit 0 = 0 (normal):
// the ROM at $0000-$3FFF is one of four, selected by $7FFD bit 4 (low) + $1FFD
// bit 2 (high) -> 0=128 editor, 1=128 syntax, 2=+3DOS, 3=48 BASIC; RAM is the
// normal 128K layout (bank 5 at $4000, bank 2 at $8000, the $7FFD bank at $C000).
// When $1FFD bit 0 = 1 (special all-RAM): no ROM; four RAM banks across the whole
// address space chosen by $1FFD bits 2:1. $7FFD bit 5 locks BOTH ports.
// +2A/+3 second paging port. Locked together with $7FFD by $7FFD bit 5.
void MemoryBus::WritePort1FFD(uint8_t Value)
{
	if (m_bPagingLocked)
	{
		return;
	}
	MapPlus3(m_Port7FFD, Value);
}

void MemoryBus::MapPlus3(uint8_t P7FFD, uint8_t P1FFD)
{
	m_bIs128K = true;
	m_bTrdosRomActive = false;
	m_Port7FFD = P7FFD;
	m_Port1FFD = P1FFD;
	m_bPagingLocked = (P7FFD & 0x20) != 0;

	if (P1FFD & 0x01)
	{
		// Special all-RAM configurations (bits 2:1 of $1FFD), $0000/$4000/$8000/$C000.
		static const int Cfg[4][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {4, 5, 6, 3}, {4, 7, 6, 3}};
		const int* B = Cfg[(P1FFD >> 1) & 0x03];
		for (int W = 0; W < 4; ++W)
		{
			SetRamSlot(W * 2, B[W] * 2);
			SetRamSlot(W * 2 + 1, B[W] * 2 + 1);
		}
	}
	else
	{
		const int Rom = ((P7FFD >> 4) & 0x01) | ((P1FFD >> 1) & 0x02); // 0..3
		SetRomSlot(0, kRomPagePlus30 + Rom * 2);
		SetRomSlot(1, kRomPagePlus30 + Rom * 2 + 1);
		SetRamSlot(2, 5 * 2);
		SetRamSlot(3, 5 * 2 + 1); // $4000 = bank 5
		SetRamSlot(4, 2 * 2);
		SetRamSlot(5, 2 * 2 + 1); // $8000 = bank 2
		const int Bank = P7FFD & 0x07;
		SetRamSlot(6, Bank * 2);
		SetRamSlot(7, Bank * 2 + 1); // $C000 = bank N
	}
}

void MemoryBus::SetRomTrd(const uint8_t* Data)
{
	if (Data == nullptr)
	{
		return;
	}
	memcpy(&m_Rom[kRomPageTrdos * kSlotSize], Data, 2 * kSlotSize);
	m_bRomLoaded[2] = true;
}

void MemoryBus::SetRomTimexHome(const uint8_t* Data16K)
{
	if (Data16K == nullptr)
	{
		return;
	}
	memcpy(&m_Rom[kRomPageTimexHome * kSlotSize], Data16K, 2 * kSlotSize);
}

void MemoryBus::SetRomTimexEx(const uint8_t* Data8K)
{
	if (Data8K == nullptr)
	{
		return;
	}
	memcpy(&m_Rom[kRomPageTimexEx * kSlotSize], Data8K, kSlotSize);
}

void MemoryBus::SetDockChunk(int Chunk, const uint8_t* Data8K, bool bWritable)
{
	if (Chunk < 0 || Chunk >= kSlotCount)
	{
		return;
	}
	if (m_DockRam.empty())
	{
		m_DockRam.assign(size_t(kSlotCount) * kSlotSize, 0);
	}
	uint8_t* Dest = &m_DockRam[size_t(Chunk) * kSlotSize];
	if (Data8K != nullptr)
	{
		memcpy(Dest, Data8K, kSlotSize);
	}
	else
	{
		memset(Dest, 0, kSlotSize);
	}
	m_DockKind[Chunk] = bWritable ? 1 : 2;
	ApplyTimexBanking(); // the chunk may already be selected by the HSR
}

bool MemoryBus::HasDock() const
{
	for (uint8_t K : m_DockKind)
	{
		if (K != 0)
		{
			return true;
		}
	}
	return false;
}

void MemoryBus::ApplyTimexBanking()
{
	// TS/TC2068 DOCK/EXROM paging, per Fuse's scld_memory_map():
	//
	//     exrom_dock = altmembank ? timex_exrom : timex_dock;
	//     for (i = 0; i < 8; i++)
	//         if (hsr & (1 << i)) memory_map_8k(i * 0x2000, exrom_dock, i);
	//
	// i.e. HSR ($F4) bit i swaps 8K chunk i away from Home, and $FF bit 7
	// picks which alternate bank answers: the EXROM, or the DOCK cartridge
	// slot. Chunks whose HSR bit is clear stay on the Home map.
	//
	// Two details that matter and are easy to get wrong:
	//   * the 8K EXROM answers EVERY chunk it is selected for, not just chunk
	//     0 - Fuse builds timex_exrom[i] from the same ROM page for all i;
	//   * an unpopulated DOCK reads $FF and swallows writes (Fuse memsets its
	//     empty chunk to $ff with writable = 0), which is exactly what
	//     OpenBusPage already provides here.
	//
	// The TC2048 has no DOCK/EXROM hardware at all, so this is 2068-only.
	if (!m_bIsTimex2068)
	{
		return;
	}

	// Home is the 48K layout: 16K ROM at $0000, then RAM banks 5, 2, 0. The
	// Timex Home ROM is loaded through --rom48 into the 48K ROM pages, so the
	// ROM slots name ROM_PAGE_48K_BASIC rather than ROM_PAGE_TIMEX_HOME.
	static const int HomeRam8K[kSlotCount] = {-1, -1, 5 * 2, 5 * 2 + 1, 2 * 2, 2 * 2 + 1, 0 * 2, 0 * 2 + 1};

	const bool bExrom = (m_ScldMode & 0x80) != 0;

	for (int S = 0; S < kSlotCount; ++S)
	{
		if (((m_ScldBank >> S) & 1) == 0)
		{
			if (HomeRam8K[S] < 0)
			{
				SetRomSlot(S, kRomPage48kBasic + S);
			}
			else
			{
				SetRamSlot(S, HomeRam8K[S]);
			}
		}
		else if (bExrom)
		{
			SetRomSlot(S, kRomPageTimexEx);
		}
		else if (m_DockKind[S] != 0)
		{
			// A populated cartridge chunk. ROM chunks swallow writes; a .dck
			// may also declare a chunk as RAM, which is how an LROS gets
			// workspace (its header has to claim chunk 3, where the machine
			// stack lives) without borrowing Home RAM.
			MemSlot& B = m_BaseSlots[S];
			B.m_Ptr = &m_DockRam[size_t(S) * kSlotSize];
			B.m_bWritable = (m_DockKind[S] == 1);
			B.m_WrPtr = B.m_bWritable ? B.m_Ptr : nullptr;
			m_Slots[S] = B;
		}
		else
		{
			m_BaseSlots[S].m_Ptr = m_OpenBusPage.data();
			m_BaseSlots[S].m_WrPtr = nullptr;
			m_BaseSlots[S].m_bWritable = false;
			m_Slots[S] = m_BaseSlots[S];
		}
	}
	if (m_Port123B & 0x05)
	{
		ApplyLayer2Window();
	}
}

void MemoryBus::SetRamSlot(int SlotIndex, int RamPage8K)
{
	// Page index is taken as-is (no masking) so the stored NR_5x value
	// reflects exactly what the program wrote. Routing depends on whether
	// the page is within the effective physical-RAM range - if not, the
	// slot points at OpenBusPage (all $FF, non-writable) which is how a
	// real Next of that RAM size responds.
	MemSlot& B = m_BaseSlots[SlotIndex];
	if (RamPage8K >= 0 && RamPage8K < m_EffectiveRamPages)
	{
		B.m_Ptr = &m_Ram[RamPage8K * kSlotSize];
		B.m_WrPtr = B.m_Ptr;
		B.m_bWritable = true;
	}
	else
	{
		B.m_Ptr = m_OpenBusPage.data();
		B.m_WrPtr = nullptr;
		B.m_bWritable = false;
	}
	m_Slots[SlotIndex] = B;
	if (SlotIndex < 6 && (m_Port123B & 0x05))
	{
		ApplyLayer2Window();
	}
}

void MemoryBus::SetRomSlot(int SlotIndex, int RomPage8K)
{
	MemSlot& B = m_BaseSlots[SlotIndex];
	B.m_Ptr = &m_Rom[RomPage8K * kSlotSize];
	B.m_WrPtr = nullptr;
	B.m_bWritable = false;
	m_Slots[SlotIndex] = B;
	if (SlotIndex < 6 && (m_Port123B & 0x05))
	{
		ApplyLayer2Window();
	}
}

// ---- Layer 2 Access Port ($123B) ------------------------------------------
// bit 0 write window, bit 1 visible (= NR $69 bit 7), bit 2 read window, bit 3
// map the shadow Layer 2 (NR $13) instead of NR $12, bits 7:6 which 16K third
// of the 48K Layer 2 appears at $0000-$3FFF (%11 = all 48K at $0000-$BFFF).
// A write with bit 4 set instead stores a 16K bank offset in bits 2:0 (core
// 3.0+, for the 320/640-wide layouts) and changes nothing else. The window
// sits above the MMU: whatever NR $50-$55 say, an enabled window wins for
// reads and/or writes in the slots it covers. Reads of the port return the
// last bit-4-clear value.
void MemoryBus::WriteLayer2Port(uint8_t V)
{
	if (V & 0x10)
	{
		m_Layer2Offset = uint8_t(V & 0x07);
	}
	else
	{
		m_Port123B = V;
		m_NextRegs[0x69] = uint8_t((m_NextRegs[0x69] & 0x7F) | ((V & 0x02) ? 0x80 : 0x00));
		m_bL2StateExplicit = true; // same hardware bit as NR $69 bit 7
		if (V & 0x05)
		{
			++m_Layer2WindowOpens;
		}
	}
	ApplyLayer2Window();
}

void MemoryBus::ApplyLayer2Window()
{
	for (int S = 0; S < 6; ++S)
	{
		m_Slots[S] = m_BaseSlots[S];
	}
	if ((m_Port123B & 0x05) == 0)
	{
		return;
	}
	const int Bank16 = int((m_Port123B & 0x08) ? m_NextRegs[0x13] : m_NextRegs[0x12]) + int(m_Layer2Offset);
	const int Third = m_Port123B >> 6;
	const int SlotCount = (Third == 3) ? 6 : 2;
	const int FirstPage = (Third == 3) ? Bank16 * 2 : (Bank16 + Third) * 2;
	for (int S = 0; S < SlotCount; ++S)
	{
		const int Page8K = FirstPage + S;
		if (Page8K < 0 || Page8K >= m_EffectiveRamPages)
		{
			continue; // beyond fitted RAM: base stays
		}
		uint8_t* P = &m_Ram[Page8K * kSlotSize];
		if (m_Port123B & 0x04)
		{
			m_Slots[S].m_Ptr = P;
		}
		if (m_Port123B & 0x01)
		{
			m_Slots[S].m_WrPtr = P;
			m_Slots[S].m_bWritable = true;
		}
	}
}

// ---- Frame-exact replay support --------------------------------------------
// A memberwise copy of the whole bus (RAM, registers, palettes, copper, CTC,
// contention state...) whose slot pointers are rebased from Src's arrays into
// this object's, and whose external taps are severed so the replay cannot
// reach the live run's outputs.
MemoryBus::SlotDesc MemoryBus::DescribeBaseSlot(int SlotIndex) const
{
	const MemSlot& B = m_BaseSlots[SlotIndex & (kSlotCount - 1)];
	SlotDesc D;
	D.m_bWritable = B.m_bWritable;
	const uint8_t* P = B.m_Ptr;
	if (P >= m_Ram.data() && P < m_Ram.data() + m_Ram.size())
	{
		D.m_Store = SlotStore::kRam;
		D.m_Page = uint16_t((P - m_Ram.data()) / kSlotSize);
	}
	else if (P >= m_Rom.data() && P < m_Rom.data() + m_Rom.size())
	{
		D.m_Store = SlotStore::kRom;
		D.m_Page = uint16_t((P - m_Rom.data()) / kSlotSize);
	}
	else if (!m_DockRam.empty() && P >= m_DockRam.data() && P < m_DockRam.data() + m_DockRam.size())
	{
		D.m_Store = SlotStore::kDock;
		D.m_Page = uint16_t((P - m_DockRam.data()) / kSlotSize);
	}
	else
	{
		D.m_Store = SlotStore::kOpenBus;
	}
	return D;
}

bool MemoryBus::RestoreBaseSlot(int SlotIndex, const SlotDesc& Desc)
{
	uint8_t* P = nullptr;
	const size_t Off = size_t(Desc.m_Page) * kSlotSize;
	switch (Desc.m_Store)
	{
	case SlotStore::kRam:
		P = (Off < m_Ram.size()) ? &m_Ram[Off] : nullptr;
		break;
	case SlotStore::kRom:
		P = (Off < m_Rom.size()) ? &m_Rom[Off] : nullptr;
		break;
	case SlotStore::kDock:
		P = (Off < m_DockRam.size()) ? &m_DockRam[Off] : nullptr;
		break;
	case SlotStore::kOpenBus:
		P = m_OpenBusPage.data();
		break;
	}
	if (P == nullptr)
	{
		return false;
	}
	MemSlot& B = m_BaseSlots[SlotIndex & (kSlotCount - 1)];
	B.m_Ptr = P;
	B.m_bWritable = Desc.m_bWritable && Desc.m_Store != SlotStore::kOpenBus;
	B.m_WrPtr = B.m_bWritable ? P : nullptr;
	return true;
}

void MemoryBus::ExportResumeExtras(ResumeExtras& Out) const
{
	Out.m_Port123B = m_Port123B;
	Out.m_Layer2Offset = m_Layer2Offset;
	Out.m_Layer2WindowOpens = m_Layer2WindowOpens;
	Out.m_bL2StateExplicit = m_bL2StateExplicit;
	memcpy(Out.m_ClipWindow, m_ClipWindow, sizeof m_ClipWindow);
	memcpy(Out.m_ClipIndex, m_ClipIndex, sizeof m_ClipIndex);
	memcpy(Out.m_bClipSet, m_bClipSet, sizeof m_bClipSet);
	Out.m_ScldMode = m_ScldMode;
	Out.m_NextCpuSpeed = m_NextCpuSpeed;
	memcpy(Out.m_EvoPal, m_EvoPal, sizeof m_EvoPal);
	Out.m_EvoPalIdx = m_EvoPalIdx;
	memcpy(Out.m_Cmos, m_Cmos, sizeof m_Cmos);
	Out.m_UnoCoreIdIdx = m_UnoCoreIdIdx;
	Out.m_UnoRadasOffPhase = m_UnoRadasOffPhase;
	Out.m_UlaPlusReg = m_UlaPlusReg;
	Out.m_DockRam = m_DockRam;
	Out.m_DockKind = m_DockKind;
	memcpy(Out.m_NextRegWriteHist, m_NextRegWriteHist, sizeof m_NextRegWriteHist);
	memcpy(Out.m_NextRegReadHist, m_NextRegReadHist, sizeof m_NextRegReadHist);
	Out.m_FrameIntPhase = m_FrameIntPhase;
	Out.m_UnhandledPortReads = m_UnhandledPortReads;
	Out.m_UnhandledPortWrites = m_UnhandledPortWrites;
	for (int S = 0; S < 8; ++S)
	{
		Out.m_MmuWriteCycle[S] = m_LastMmuWrite[S].m_Cycle;
		Out.m_MmuWritePc[S] = m_LastMmuWrite[S].m_Pc;
		Out.m_MmuWriteOld[S] = m_LastMmuWrite[S].m_OldValue;
	}
}

void MemoryBus::ImportResumeExtras(const ResumeExtras& In)
{
	m_Port123B = In.m_Port123B;
	m_Layer2Offset = In.m_Layer2Offset;
	m_Layer2WindowOpens = In.m_Layer2WindowOpens;
	m_bL2StateExplicit = In.m_bL2StateExplicit;
	memcpy(m_ClipWindow, In.m_ClipWindow, sizeof m_ClipWindow);
	memcpy(m_ClipIndex, In.m_ClipIndex, sizeof m_ClipIndex);
	memcpy(m_bClipSet, In.m_bClipSet, sizeof m_bClipSet);
	m_ScldMode = In.m_ScldMode;
	m_NextCpuSpeed = uint8_t(In.m_NextCpuSpeed & 0x03);
	m_bNext28 = m_NextCpuSpeed == 0x03;
	memcpy(m_EvoPal, In.m_EvoPal, sizeof m_EvoPal);
	m_EvoPalIdx = In.m_EvoPalIdx;
	memcpy(m_Cmos, In.m_Cmos, sizeof m_Cmos);
	m_UnoCoreIdIdx = In.m_UnoCoreIdIdx;
	m_UnoRadasOffPhase = In.m_UnoRadasOffPhase;
	m_UlaPlusReg = In.m_UlaPlusReg;
	if (!In.m_DockRam.empty())
	{
		m_DockRam = In.m_DockRam;
		m_DockKind = In.m_DockKind;
	}
	memcpy(m_NextRegWriteHist, In.m_NextRegWriteHist, sizeof m_NextRegWriteHist);
	memcpy(m_NextRegReadHist, In.m_NextRegReadHist, sizeof m_NextRegReadHist);
	m_FrameIntPhase = In.m_FrameIntPhase;
	m_UnhandledPortReads = In.m_UnhandledPortReads;
	m_UnhandledPortWrites = In.m_UnhandledPortWrites;
	for (int S = 0; S < 8; ++S)
	{
		m_LastMmuWrite[S].m_Cycle = In.m_MmuWriteCycle[S];
		m_LastMmuWrite[S].m_Pc = In.m_MmuWritePc[S];
		m_LastMmuWrite[S].m_OldValue = In.m_MmuWriteOld[S];
	}
}

void MemoryBus::ApplyRestoredSlots()
{
	for (int S = 0; S < kSlotCount; ++S)
	{
		m_Slots[S] = m_BaseSlots[S];
	}
	if (m_Port123B & 0x05)
	{
		ApplyLayer2Window();
	}
}

void MemoryBus::AdoptForReplay(const MemoryBus& Src)
{
	*this = Src;

	auto Rebase = [&](const uint8_t* P) -> uint8_t*
	{
		if (P == nullptr)
		{
			return nullptr;
		}
		if (P >= Src.m_Ram.data() && P < Src.m_Ram.data() + Src.m_Ram.size())
		{
			return m_Ram.data() + (P - Src.m_Ram.data());
		}
		if (P >= Src.m_Rom.data() && P < Src.m_Rom.data() + Src.m_Rom.size())
		{
			return m_Rom.data() + (P - Src.m_Rom.data());
		}
		if (P >= Src.m_OpenBusPage.data() && P < Src.m_OpenBusPage.data() + Src.m_OpenBusPage.size())
		{
			return m_OpenBusPage.data() + (P - Src.m_OpenBusPage.data());
		}
		// A DOCK cartridge chunk. `*this = Src` above already copied the
		// cartridge into this bus's own DockRam_, so the slot only needs
		// pointing at the copy - without this a replay of a running cartridge
		// fetches through a null slot.
		if (!Src.m_DockRam.empty() && P >= Src.m_DockRam.data() && P < Src.m_DockRam.data() + Src.m_DockRam.size())
		{
			return m_DockRam.data() + (P - Src.m_DockRam.data());
		}
		// Every backing store referenced by a slot must be rebased when cloning
		// the bus. Add new storage regions here as well as to the copy operation;
		// frame replay must never retain pointers into the original bus.
		return nullptr; // unknown origin: fail visibly (a null slot faults fast)
	};
	for (int S = 0; S < kSlotCount; ++S)
	{
		m_Slots[S].m_Ptr = Rebase(m_Slots[S].m_Ptr);
		m_Slots[S].m_WrPtr = Rebase(m_Slots[S].m_WrPtr);
		m_BaseSlots[S].m_Ptr = Rebase(m_BaseSlots[S].m_Ptr);
		m_BaseSlots[S].m_WrPtr = Rebase(m_BaseSlots[S].m_WrPtr);
	}

	// External taps stay with the live run; the replay rebinds its own.
	m_BeamBoundaryHook = nullptr;
	m_BeamBoundaryUser = nullptr;
	m_OnTurboChange = nullptr;
	m_OnTurboChangeUser = nullptr;
	m_OnDeadline = nullptr;
	m_OnDeadlineUser = nullptr;
	m_Coverage = nullptr;
	m_WatchPage8K = -1;
	m_OnPhysicalWatch = nullptr;
	m_OnPhysicalWatchUser = nullptr;
	m_InvariantReadOnly = nullptr;
	m_OnInvariantWrite = nullptr;
	m_OnInvariantWriteUser = nullptr;
	m_CyclePtr = nullptr;
	m_ClkCycles = nullptr;
	m_ClkNextFrameInt = nullptr;
	m_ClkFrameIntPeriod = nullptr;
	m_ContNextPtr = nullptr;
	m_ContPeriodPtr = nullptr;
#if INSPECCY_TRACE_SUPPORT
	m_OnWrite = nullptr;
	m_OnWriteUser = nullptr;
	m_VideoTrace = nullptr;
#endif
}

void MemoryBus::Map48K()
{
	SetRomSlot(0, kRomPage48kBasic);
	SetRomSlot(1, kRomPage48kBasic + 1);
	// 48K RAM laid out using the same 16K bank numbering the SNA uses:
	// $4000 = bank 5, $8000 = bank 2, $C000 = bank 0.
	SetRamSlot(2, 5 * 2);
	SetRamSlot(3, 5 * 2 + 1);
	SetRamSlot(4, 2 * 2);
	SetRamSlot(5, 2 * 2 + 1);
	SetRamSlot(6, 0 * 2);
	SetRamSlot(7, 0 * 2 + 1);
	// Reflect the initial mapping in NR_50..NR_57 so the dump round-trips
	// correctly. Otherwise NextRegs[$50..$57] stays at zero (= default-
	// constructed) and a resume that replays NR writes would re-map every
	// slot to RAM page 0 -> all-zero bank 0 -> no code reachable.
	m_NextRegs[0x50] = uint8_t(kRomPage48kBasic | 0x80); // sentinel-ish; see MMU restore
	m_NextRegs[0x51] = uint8_t((kRomPage48kBasic + 1) | 0x80);
	m_NextRegs[0x52] = uint8_t(5 * 2);
	m_NextRegs[0x53] = uint8_t(5 * 2 + 1);
	m_NextRegs[0x54] = uint8_t(2 * 2);
	m_NextRegs[0x55] = uint8_t(2 * 2 + 1);
	m_NextRegs[0x56] = uint8_t(0 * 2);
	m_NextRegs[0x57] = uint8_t(0 * 2 + 1);

	m_bIs128K = false;
	m_bTrdosRomActive = false;
	m_Port7FFD = 0;
	m_bPagingLocked = true;
}

void MemoryBus::MapTimex(bool bB2068)
{
	// Home ROM (16K) at $0000, then 48K RAM (banks 5,2,0) - same as Map48K but
	// with the Timex Home ROM in slots 0,1. The SCLD ($FF) then overlays extra
	// video modes; the 2068 $F4 banking (DOCK/EXROM) is a later phase.
	SetRomSlot(0, kRomPageTimexHome);
	SetRomSlot(1, kRomPageTimexHome + 1);
	SetRamSlot(2, 5 * 2);
	SetRamSlot(3, 5 * 2 + 1);
	SetRamSlot(4, 2 * 2);
	SetRamSlot(5, 2 * 2 + 1);
	SetRamSlot(6, 0 * 2);
	SetRamSlot(7, 0 * 2 + 1);
	// Mirror into NR_50..57 for dump round-trip (same rationale as Map48K).
	m_NextRegs[0x50] = uint8_t(kRomPageTimexHome | 0x80);
	m_NextRegs[0x51] = uint8_t((kRomPageTimexHome + 1) | 0x80);
	m_NextRegs[0x52] = uint8_t(5 * 2);
	m_NextRegs[0x53] = uint8_t(5 * 2 + 1);
	m_NextRegs[0x54] = uint8_t(2 * 2);
	m_NextRegs[0x55] = uint8_t(2 * 2 + 1);
	m_NextRegs[0x56] = uint8_t(0 * 2);
	m_NextRegs[0x57] = uint8_t(0 * 2 + 1);

	m_bIs128K = false;
	m_bTrdosRomActive = false;
	m_Port7FFD = 0;
	m_bPagingLocked = true;
	m_bIsTimex = true;
	m_bIsTimex2068 = bB2068;
	m_ScldMode = 0;
	m_ScldBank = 0;
}

void MemoryBus::Map128K(uint8_t Port7FFDValue, bool bUseTrdosRom)
{
	m_bIs128K = true;
	m_bTrdosRomActive = bUseTrdosRom;
	m_Port7FFD = Port7FFDValue;
	m_bPagingLocked = (Port7FFDValue & 0x20) != 0;

	const int RomPage = CurrentRomPage();
	SetRomSlot(0, RomPage);
	SetRomSlot(1, RomPage + 1);

	SetRamSlot(2, 5 * 2);
	SetRamSlot(3, 5 * 2 + 1);
	SetRamSlot(4, 2 * 2);
	SetRamSlot(5, 2 * 2 + 1);

	const int Bank = Port7FFDValue & 0x07;
	SetRamSlot(6, Bank * 2);
	SetRamSlot(7, Bank * 2 + 1);

	// Reflect the initial mapping in NR_50..NR_57. ROM slots get $FF
	// (the canonical "ROM page" sentinel on Next); RAM slots get their
	// page index. Without this, NextRegs[$50..$57] stays zero for the
	// initial classic-paging period and dump/resume corrupts the live
	// slot pointers.
	m_NextRegs[0x50] = 0xFF;
	m_NextRegs[0x51] = 0xFF;
	m_NextRegs[0x52] = uint8_t(5 * 2);
	m_NextRegs[0x53] = uint8_t(5 * 2 + 1);
	m_NextRegs[0x54] = uint8_t(2 * 2);
	m_NextRegs[0x55] = uint8_t(2 * 2 + 1);
	m_NextRegs[0x56] = uint8_t(Bank * 2);
	m_NextRegs[0x57] = uint8_t(Bank * 2 + 1);
}

namespace
{
}

void MemoryBus::WriteNextReg(
	uint8_t Index, uint8_t Value, uint16_t WriterPC, uint64_t WriterCycle, [[maybe_unused]] uint8_t Source)
{
#if INSPECCY_TRACE_SUPPORT
	// .btr v4: record display-register writes, whoever made them. Filtered to
	// the registers that decide what the screen looks like, so MMU / DMA /
	// joystick traffic stays out of the stream.
	if (m_VideoTrace != nullptr && BinaryTrace::DisplayRegSlot(Index) >= 0)
	{
		const uint64_t Frm = (m_FrameIntPeriodHint > 0) ? (WriterCycle / m_FrameIntPeriodHint) : 0;
		m_VideoTrace->NoteStateWrite(0 /*NextReg*/, Index, Value, Source, WriterCycle, Frm);
	}
#endif

	// Eager beam drawer: a display-register write is a boundary - flush the beam
	// up to this cycle from the current (pre-write) state before it lands. Fires
	// for the superset IsDisplayReg set (incl. palette-data $41/$44); over-flush
	// is correct. Copper MOVEs pass through here too (with their beam cycle). The
	// >= BeamArmCycle_ test (before IsDisplayReg) makes frames we won't save cost
	// one comparison per write and never reach the drawer.
	if (m_BeamBoundaryHook && WriterCycle >= m_BeamArmCycle && IsDisplayReg(Index))
	{
		FireBeamBoundary(WriterCycle);
	}

	// MMU8K last-write attribution (for invariants/diagnostics). Captured
	// BEFORE the NextRegs[] update so OldValue is the actual pre-write byte.
	if (Index >= 0x50 && Index <= 0x57)
	{
		MmuLastWrite& M = m_LastMmuWrite[Index - 0x50];
		M.m_OldValue = m_NextRegs[Index];
		M.m_Pc = WriterPC;
		M.m_Cycle = WriterCycle;
	}
	m_NextRegs[Index] = Value;
	++m_NextRegWriteHist[Index]; // compatibility sweeps: what did the program write?
	++m_NextRegWrites;

	// Per-register side effect, if the register's owner registered one.
	const NextRegHook& H = m_NextRegHooks[Index];
	if (H.m_Write != nullptr)
	{
		H.m_Write(*this, Index, Value, WriterCycle);
	}

	// Layer-2-in-use hint (flag kNrL2Hint): programs that never write NR_69 bit 7
	// but do touch other L2 registers. An explicit "L2 off" (NR_69 with bit 7
	// clear, NR_70 with the mode bits clear) must NOT latch it - that would lock
	// the compositor into the L2 path for programs that opt out of L2.
	if (H.m_Flags & kNrL2Hint)
	{
		const bool bExplicitDisable = (Index == 0x69 && (Value & 0x80) == 0) || (Index == 0x70 && (Value & 0x30) == 0);
		if (Index == 0x69)
		{
			// Once a program drives NR $69 itself, its bit 7 is the truth: an
			// explicit off drops the hint, and later writes to clip windows,
			// NR $12 or the L2 palette (routine per-frame housekeeping for a
			// program that keeps Layer 2 off) must not re-arm it.
			m_bL2StateExplicit = true;
		}
		if (bExplicitDisable)
		{
			m_bAnyL2Touched = false;
		}
		else if (!m_bL2StateExplicit)
		{
			m_bAnyL2Touched = true;
		}
	}
}

// ---- NextReg read (port $253B) --------------------------------------------
uint8_t MemoryBus::ReadNextReg(uint8_t Index)
{
	++m_NextRegReadHist[Index]; // compatibility sweeps: what does the program poll?
	const NextRegHook& H = m_NextRegHooks[Index];
	return (H.m_Read != nullptr) ? H.m_Read(*this, Index) : m_NextRegs[Index];
}

// ---- The built-in table ----------------------------------------------------
void MemoryBus::InitNextRegHooks()
{
	for (NextRegHook& H : m_NextRegHooks)
	{
		H = NextRegHook{};
	}
	// Compositor inputs: a write is a beam boundary (superset - over-flush is
	// correct). Palette data $40/$41/$44 included; clip $18-$1C; tilemap and
	// ULA control registers included ahead of their implementation.
	for (int R :
		{0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x26, 0x27, 0x2F, 0x30, 0x31, 0x32, 0x33,
			0x40, 0x41, 0x42, 0x43, 0x44, 0x4A, 0x4B, 0x4C, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6E, 0x6F, 0x70, 0x71})
	{
		SetNextRegFlags(uint8_t(R), kNrDisplay);
	}
	// Layer 2 usage hint. Exclude global transparency ($14/$4A) and the
	// shared clip-index reset ($1C): other layers also use those registers.
	// The Layer 2 clip window ($18) is a Layer 2-specific hint.
	for (int R : {0x12, 0x13, 0x16, 0x17, 0x18, 0x69, 0x70, 0x71})
	{
		SetNextRegFlags(uint8_t(R), kNrL2Hint);
	}

	RegisterNextRegWrite(0x07, &NrTurbo);
	RegisterNextRegRead(0x07, &NrReadTurbo);
	RegisterNextRegWrite(0xCA, &NrIntStatus2);
	RegisterNextRegRead(0xCA, &NrReadIntStatus2);
	for (uint8_t R = 0x50; R <= 0x57; ++R)
	{
		RegisterNextRegWrite(R, &NrMmu);
	}
	RegisterNextRegWrite(0x69, &NrDisplayControl);
	RegisterNextRegWrite(0x12, &NrLayer2Bank);
	RegisterNextRegWrite(0x13, &NrLayer2Bank);
	RegisterNextRegWrite(0x40, &NrPaletteIndex);
	RegisterNextRegWrite(0x41, &NrPalette8);
	RegisterNextRegWrite(0x43, &NrPaletteControl);
	RegisterNextRegWrite(0x44, &NrPalette9);
	for (uint8_t R = 0x18; R <= 0x1B; ++R)
	{
		RegisterNextRegWrite(R, &NrClip);
	}
	RegisterNextRegWrite(0x1C, &NrClipControl);
	RegisterNextRegRead(0x1C, &NrReadClipIndex);
	for (uint8_t R = 0x60; R <= 0x63; ++R)
	{
		RegisterNextRegWrite(R, &NrCopper);
	}

	// Sprite attribute mirror. $34 selects the slot (resets the port cursors,
	// not a beam boundary). $35-$39 write attribute bytes 0-4 without advancing;
	// $75-$79 write then increment $34. All of $35-$39/$75-$79 are compositor
	// inputs, so flag them kNrDisplay (the eager drawer flushes before they land).
	RegisterNextRegWrite(0x34, &NrSpriteSlot);
	for (uint8_t R = 0x35; R <= 0x39; ++R)
	{
		RegisterNextRegWrite(R, &NrSpriteAttr);
		SetNextRegFlags(R, kNrDisplay);
	}
	for (uint8_t R = 0x75; R <= 0x79; ++R)
	{
		RegisterNextRegWrite(R, &NrSpriteAttr);
		SetNextRegFlags(R, kNrDisplay);
	}

	RegisterNextRegWrite(0x22, &NrLineInt); // line interrupt control + line[8]
	RegisterNextRegWrite(0x23, &NrLineInt); // line[7:0]

	// Identity: machine id 10 (ZX Spectrum Next), core 3.02.00. Programs
	// may use this version to decide which features are available.
	RegisterNextRegRead(0x00, &NrReadIdentity);
	RegisterNextRegRead(0x01, &NrReadIdentity);
	RegisterNextRegRead(0x0E, &NrReadIdentity);
	RegisterNextRegRead(0x1E, &NrReadRasterLine);
	RegisterNextRegRead(0x1F, &NrReadRasterLine);
}

// ---- TURBO_CONTROL ($07): notify main so it can rescale Budget ----
void MemoryBus::NrIntStatus2(MemoryBus& InOutM, uint8_t, uint8_t Value, uint64_t)
{
	InOutM.m_IntStatus2 &= uint8_t(~Value); // write 1 to clear
}
uint8_t MemoryBus::NrReadIntStatus2(const MemoryBus& M, uint8_t)
{
	return M.m_IntStatus2;
}
void MemoryBus::NrTurbo(MemoryBus& InOutM, uint8_t, uint8_t Value, uint64_t)
{
	InOutM.SetNextCpuSpeed(Value & 0x03); // 28 MHz = one wait state per memory read
	if (InOutM.m_OnTurboChange)
	{
		static constexpr double kNextMhz[4] = {3.5, 7.0, 14.0, 28.0};
		InOutM.m_OnTurboChange(InOutM.m_OnTurboChangeUser, kNextMhz[Value & 0x03]);
	}
}

uint8_t MemoryBus::NrReadTurbo(const MemoryBus& M, uint8_t)
{
	// Actual speed is independent of the programmed low bits. Expansion-bus
	// throttling is not yet modeled; report the speed the CPU actually uses.
	return uint8_t((M.m_NextCpuSpeed << 4) | (M.m_NextRegs[0x07] & 3));
}

// ---- MMU0..7 ($50..$57): re-map 8K slot ----
// The page value is passed as-is (8 bits, 0..255) to SetRamSlot, which decides
// whether to point at real RAM or at OpenBusPage based on the effective RAM
// size; NR_5x itself keeps `Value` unchanged so the dump round-trips exactly
// what the program wrote. MMU0/MMU1 = $FF selects the ROM (whichever $7FFD
// bit 4 / TR-DOS say), as on hardware - it is what NR $50/$51 read back at
// reset, so every program that saves and restores its MMU writes it, and NEX
// programs write it to call 48K ROM routines such as KEY-SCAN. Any other
// value, in any slot, is an 8K RAM page.
void MemoryBus::NrMmu(MemoryBus& InOutM, uint8_t Index, uint8_t Value, uint64_t)
{
	const int SlotIndex = Index - 0x50;
	if (SlotIndex < 2 && Value == 0xFF)
	{
		InOutM.SetRomSlot(SlotIndex, InOutM.CurrentRomPage() + SlotIndex);
	}
	else
	{
		InOutM.SetRamSlot(SlotIndex, int(Value));
	}
}

// ---- DISPLAY_CONTROL_1 ($69) ----
// Bit 7 (Layer 2 visible) is the same hardware bit as $123B bit 1 - mirror it.
// Bits 5:0 are the other half of the port-$FF alias: writing NR $69 sets the
// Timex display mode exactly as an OUT to $FF would; SetScldMode mirrors the
// value straight back into NextRegs[0x69], so the two never disagree.
void MemoryBus::NrDisplayControl(MemoryBus& InOutM, uint8_t, uint8_t Value, uint64_t)
{
	InOutM.m_Port123B = uint8_t((InOutM.m_Port123B & ~0x02) | ((Value & 0x80) ? 0x02 : 0x00));
	// Bit 6 is the same hardware bit as $7FFD bit 3 (ULA shadow screen). Only
	// the display select moves: no paging, and the $7FFD lock does not apply.
	InOutM.m_Port7FFD = uint8_t((InOutM.m_Port7FFD & ~0x08) | ((Value & 0x40) ? 0x08 : 0x00));
	InOutM.SetScldMode(uint8_t((InOutM.m_ScldMode & 0xC0) | (Value & 0x3F)));
}

// ---- LAYER2_RAM_BANK ($12) / shadow ($13): the $123B paging window follows ----
void MemoryBus::NrLayer2Bank(MemoryBus& InOutM, uint8_t, uint8_t, uint64_t)
{
	if (InOutM.m_Port123B & 0x05)
	{
		InOutM.ApplyLayer2Window();
	}
}

// ---- Palette: index, 8-bit write, control, 9-bit two-byte write ----
// NR $43 bit 7 disables the auto-increment of the palette index ($40) that a
// palette write would otherwise do. A program that leaves the bit set and
// writes 256 entries lands all of them on entry 0 - which is exactly what it
// looks like on hardware, and what this used to hide by always incrementing.
static inline bool PaletteAutoIncDisabled(const MemoryBus& M)
{
	return (M.GetNextReg(0x43) & 0x80) != 0;
}

void MemoryBus::NrPaletteIndex(MemoryBus& InOutM, uint8_t, uint8_t, uint64_t)
{
	InOutM.m_PaletteWriteParity = 0;
}

void MemoryBus::NrPalette8(MemoryBus& InOutM, uint8_t, uint8_t Value, uint64_t)
{
	const int PalIdx = (InOutM.m_NextRegs[0x43] >> 4) & 0x07;
	const int Entry = InOutM.m_NextRegs[0x40];
	InOutM.m_Palettes[PalIdx][Entry][0] = Value;
	InOutM.m_Palettes[PalIdx][Entry][1] = uint8_t(Value & 0x01); // duplicate low blue bit
	if (!PaletteAutoIncDisabled(InOutM))
	{
		InOutM.m_NextRegs[0x40] = uint8_t(Entry + 1);
	}
	++InOutM.m_PaletteWrites;
	if (PalIdx == 1 || PalIdx == 5)
	{
		InOutM.m_bAnyL2Touched = true;
	}
}

void MemoryBus::NrPaletteControl(MemoryBus& InOutM, uint8_t, uint8_t, uint64_t)
{
	InOutM.m_PaletteWriteParity = 0;
}

void MemoryBus::NrPalette9(MemoryBus& InOutM, uint8_t, uint8_t Value, uint64_t)
{
	const int PalIdx = (InOutM.m_NextRegs[0x43] >> 4) & 0x07;
	const int Entry = InOutM.m_NextRegs[0x40];
	if (InOutM.m_PaletteWriteParity == 0)
	{
		InOutM.m_Palettes[PalIdx][Entry][0] = Value;
		InOutM.m_PaletteWriteParity = 1;
	}
	else
	{
		InOutM.m_Palettes[PalIdx][Entry][1] = Value;
		if (!PaletteAutoIncDisabled(InOutM))
		{
			InOutM.m_NextRegs[0x40] = uint8_t(Entry + 1);
		}
		InOutM.m_PaletteWriteParity = 0;
		++InOutM.m_PaletteWrites;
	}
	if (PalIdx == 1 || PalIdx == 5)
	{
		InOutM.m_bAnyL2Touched = true;
	}
}

// ---- Clip windows: $18 = Layer 2 (4-value sequence X1,X2,Y1,Y2), $1C control ----
void MemoryBus::NrClip(MemoryBus& InOutM, uint8_t Index, uint8_t Value, uint64_t)
{
	// $18/$19/$1A/$1B -> layer 0/1/2/3. Four sequential writes fill X1,X2,Y1,Y2
	// through a per-layer sub-index that auto-advances (mod 4).
	const int L = Index - 0x18;
	InOutM.m_ClipWindow[L][InOutM.m_ClipIndex[L] & 0x03] = Value;
	InOutM.m_ClipIndex[L] = uint8_t((InOutM.m_ClipIndex[L] + 1) & 0x03);
	InOutM.m_bClipSet[L] = true;
}

void MemoryBus::NrClipControl(MemoryBus& InOutM, uint8_t, uint8_t Value, uint64_t)
{
	// NR_1C bits 0/1/2/3 reset the write sub-index of L2/sprite/ULA/tilemap.
	for (int L = 0; L < 4; ++L)
	{
		if (Value & (1u << L))
		{
			InOutM.m_ClipIndex[L] = 0;
		}
	}
}

uint8_t MemoryBus::NrReadClipIndex(const MemoryBus& M, uint8_t)
{
	// NR_1C read: packed sub-indices, 2 bits each (L2 in 1:0 .. tilemap in 7:6).
	return uint8_t((M.m_ClipIndex[0] & 3) | ((M.m_ClipIndex[1] & 3) << 2) | ((M.m_ClipIndex[2] & 3) << 4) |
				   ((M.m_ClipIndex[3] & 3) << 6));
}

// ---- Copper ($60-$63) ----
// Only acts when the copper is present (Next machine, not --no-copper);
// otherwise the value is just stored.
void MemoryBus::NrCopper(MemoryBus& InOutM, uint8_t Index, uint8_t Value, uint64_t WriterCycle)
{
	if (InOutM.m_Copper.m_bPresent)
	{
		WriteCopper(InOutM.m_Copper, Index, Value, WriterCycle);
		// Starting / restarting the copper (NR $62 mode bits) arms its first
		// WAIT deadline, which can be nearer than the dispatcher's horizon.
		if (Index == 0x62)
		{
			InOutM.NotifyDeadline(InOutM.m_Copper.m_NextFireCycle);
		}
	}
}

// ---- Sprite attribute mirror ($34, $35-$39, $75-$79) ----------------------
// NextRegs[0x34] is already stored by WriteNextReg before this hook runs, so
// $34 only needs to reset the sprite upload cursors. $35-$39/$75-$79 write one
// attribute byte of the slot NR_34 names; the $75-$79 variants then advance
// NR_34 (mod 128) after the write - the "with increment" registers, one step
// per write exactly as the hardware does.
void MemoryBus::NrSpriteSlot(MemoryBus& InOutM, uint8_t, uint8_t Value, uint64_t)
{
	InOutM.m_NextRegs[0x34] = Value & 0x7F;
	if (InOutM.m_Sprites && (InOutM.m_NextRegs[0x09] & 0x10))
	{
		SelectSpriteSlot(*InOutM.m_Sprites, Value);
	}
}

// ---- Line interrupt ($22/$23) ---------------------------------------------
// $22 and $23 are already stored in NextRegs by WriteNextReg; a write to either
// reprograms the line interrupt. Decode both and hand the CPU the enable / line
// / ULA-mask so it can re-arm NextLineIntCycle (the bus never touches Z80State).
void MemoryBus::NrLineInt(MemoryBus& InOutM, uint8_t, uint8_t, uint64_t)
{
	if (InOutM.m_OnLineIntChange == nullptr)
	{
		return;
	}
	const uint8_t kR22 = InOutM.m_NextRegs[0x22];
	const uint8_t kR23 = InOutM.m_NextRegs[0x23];
	const bool bEnabled = (kR22 & 0x02) != 0; // $22 bit 1
	const bool bUlaMask = (kR22 & 0x04) != 0; // $22 bit 2
	const uint32_t Line = (uint32_t(kR22 & 0x01) << 8) | kR23; // $22 bit 0 = bit 8
	InOutM.m_OnLineIntChange(InOutM.m_OnLineIntChangeUser, bEnabled, Line, bUlaMask);
}

void MemoryBus::NrSpriteAttr(MemoryBus& InOutM, uint8_t Index, uint8_t Value, uint64_t)
{
	if (!InOutM.m_Sprites)
	{
		return;
	}
	const bool bInc = Index >= 0x75;
	const int Byte = bInc ? (Index - 0x75) : (Index - 0x35);
	const uint8_t Slot = uint8_t(InOutM.m_NextRegs[0x34] & 0x7F);
	WriteSpriteAttrByte(*InOutM.m_Sprites, Slot, Byte, Value);
	if (bInc)
	{
		InOutM.m_NextRegs[0x34] = uint8_t((InOutM.m_NextRegs[0x34] + 1) & 0x7F);
		if (InOutM.m_NextRegs[0x09] & 0x10)
		{
			InOutM.m_Sprites->m_AttrSlot = InOutM.m_NextRegs[0x34];
			InOutM.m_Sprites->m_AttrByteIdx = 0;
		}
	}
}

// ---- Computed read-backs --------------------------------------------------
uint8_t MemoryBus::NrReadIdentity(const MemoryBus&, uint8_t Index)
{
	switch (Index)
	{
	case 0x00:
		return 0x0A; // machine id: ZX Spectrum Next
	case 0x01:
		return 0x32; // core major.minor 3.02
	default:
		return 0x00; // $0E: sub-minor 00 -> "3.02.00"
	}
}

// Active video line, 9 bits (NR $1E bit 0 = MSB, NR $1F = LSB), 0 = first paper
// row, counting on through the bottom border, vblank and top border. Derived
// from the frame-interrupt phase the same way the copper places WAIT(line): the
// interrupt fires TopBorderLines above paper row 0. Games spin on this for
// vsync (for example, waiting for line 250) - a constant 0
// hangs them in DI forever.
uint8_t MemoryBus::NrReadRasterLine(const MemoryBus& M, uint8_t Index)
{
	if (!M.m_ClkCycles || !M.m_ClkNextFrameInt || !M.m_ClkFrameIntPeriod)
	{
		return 0;
	}
	const uint64_t Period = *M.m_ClkFrameIntPeriod;
	const uint64_t NextInt = *M.m_ClkNextFrameInt;
	if (Period == 0 || NextInt == UINT64_MAX)
	{
		return 0;
	}
	const uint64_t Lines = M.m_Copper.m_LinesPerFrame ? M.m_Copper.m_LinesPerFrame : 312;
	const uint64_t CyclesPerLine = Period / Lines;
	const uint64_t Origin = NextInt - Period;
	const uint64_t Now = *M.m_ClkCycles;
	const uint64_t InFrame = (Now >= Origin) ? (Now - Origin) : 0;
	const uint64_t Raw = (InFrame / (CyclesPerLine ? CyclesPerLine : 1)) % Lines;
	const uint64_t Line = (Raw + Lines - (M.m_Copper.m_TopBorderLines % Lines)) % Lines;
	return (Index == 0x1E) ? uint8_t((Line >> 8) & 1) : uint8_t(Line & 0xFF);
}
namespace
{
// 8K pages making up the always-mapped 16K banks 5 ($4000-$7FFF) and
// 2 ($8000-$BFFF). They form the static Spectrum 128K layout and are never
// available to IDE_BANK.
inline bool IsReservedPage8K(int Page8K)
{
	return Page8K == 10 || Page8K == 11 // bank 5
		   || Page8K == 4 || Page8K == 5; // bank 2
}
} // namespace

int MemoryBus::AllocBank8K()
{
	// Allocate the highest free 8K page first, skipping reserved banks.
	// Limit the search to installed RAM; a 1 MB model must not return pages 128+.
	for (int P = m_EffectiveRamPages - 1; P >= 0; --P)
	{
		if (IsReservedPage8K(P))
		{
			continue;
		}
		const uint64_t Bit = uint64_t(1) << (P & 63);
		uint64_t& Word = m_AllocatedPages8KWord[P >> 6]; // 64 pages per word
		if ((Word & Bit) == 0)
		{
			Word |= Bit;
			return P;
		}
	}
	return -1;
}

void MemoryBus::FreeBank8K(int Page8K)
{
	if (Page8K < 0 || Page8K >= kRamPageCount)
	{
		return;
	}
	const uint64_t Bit = uint64_t(1) << (Page8K & 63);
	m_AllocatedPages8KWord[Page8K >> 6] &= ~Bit;
}

bool MemoryBus::IsPage8KAllocated(int Page8K) const
{
	if (Page8K < 0 || Page8K >= kRamPageCount)
	{
		return false;
	}
	const uint64_t Bit = uint64_t(1) << (Page8K & 63);
	return (m_AllocatedPages8KWord[Page8K >> 6] & Bit) != 0;
}

namespace
{

// Next ZC/TO pulse of a running channel's grid STRICTLY after Now.
uint64_t NextGridPulse(const MemoryBus::CtcChannel& C, uint64_t Now)
{
	if (!C.m_bRunning || C.m_PeriodCycles == 0)
	{
		return UINT64_MAX;
	}
	if (Now < C.m_GridAnchor)
	{
		return C.m_GridAnchor + C.m_PeriodCycles;
	}
	const uint64_t K = (Now - C.m_GridAnchor) / C.m_PeriodCycles + 1;
	return C.m_GridAnchor + K * C.m_PeriodCycles;
}

} // namespace

void MemoryBus::RearmCtcCounter(int N, uint32_t Tc, bool bIrq, uint64_t CurrentCycle)
{
	CtcChannel& C = m_Ctc[N & 7];
	// Counter mode decrements on the PREVIOUS channel's ZC/TO (the Next's
	// cascade wiring). Channel 0 has no predecessor: it can never tick here.
	const CtcChannel& Src = m_Ctc[(N - 1) & 7];
	if (N == 0 || !Src.m_bRunning || Src.m_PeriodCycles == 0)
	{
		C.m_PeriodCycles = 0;
		C.m_bRunning = false;
		C.m_bEnabled = false;
		C.m_NextFireCycle = UINT64_MAX;
		return;
	}
	// Fire on the source's grid: the TC-th pulse strictly after the arming
	// instant. This is what preserves phase when a player reprograms the
	// channel from inside its own ISR - the count restarts, the GRID doesn't.
	// (Edge-select bit 4 only picks which edge of the one-clock-wide ZC/TO
	// pulse counts; at cycle granularity both edges land on the same pulse.)
	const uint64_t FirstPulse = NextGridPulse(Src, CurrentCycle);
	const uint64_t Fire = FirstPulse + uint64_t(Tc - 1) * Src.m_PeriodCycles;
	C.m_PeriodCycles = uint64_t(Tc) * Src.m_PeriodCycles; // effective period (auto-reload + downstream grid)
	C.m_GridAnchor = Fire - C.m_PeriodCycles; // own ZC/TO grid: pulses at Fire, Fire + P, ...
	C.m_bRunning = true;
	C.m_bEnabled = bIrq;
	C.m_NextFireCycle = bIrq ? Fire : UINT64_MAX;
}

void MemoryBus::RearmCtcChain(int From, uint64_t CurrentCycle)
{
	for (int N = From; N < 8; ++N)
	{
		CtcChannel& C = m_Ctc[N];
		if (!C.m_bCounterMode || !C.m_bTcLoaded)
		{
			// A timer's grid is independent of its predecessor, and an
			// unprogrammed channel anchors nothing - nothing deeper moved.
			break;
		}
		RearmCtcCounter(N, C.m_TimeConstant ? C.m_TimeConstant : 256u, (C.m_ControlWord & 0x80) != 0, CurrentCycle);
	}
}

void MemoryBus::WriteCtc(int N, uint8_t Value, uint64_t CurrentCycle)
{
	CtcChannel& C = m_Ctc[N & 7];
	if (C.m_bAwaitingTC)
	{
		// Second of a two-byte sequence - the time constant. Reload-while-
		// running follows the Z80 CTC datasheet ("loads immediately; counting
		// resumes on the next active edge") - the FPGA docs don't say
		// otherwise, so datasheet semantics it is. For a timer that means the
		// grid re-anchors at this write; for a counter the new count starts
		// at the next SOURCE pulse (grid preserved, see RearmCtcCounter).
		C.m_TimeConstant = Value;
		C.m_bAwaitingTC = false;
		C.m_bTcLoaded = true;
		C.m_bCounterMode = (C.m_ControlWord & 0x40) != 0;
		const uint32_t Tc = Value ? Value : 256u;
		const bool bIrq = (C.m_ControlWord & 0x80) != 0;
		if (!C.m_bCounterMode)
		{
			const uint32_t Prescaler = (C.m_ControlWord & 0x20) ? 256u : 16u;
			C.m_PeriodCycles = uint64_t(Prescaler) * Tc;
			C.m_GridAnchor = CurrentCycle;
			C.m_bRunning = C.m_PeriodCycles > 0;
			// The grid runs regardless of the IRQ bit - bit 7 = 0 is the
			// silent-metronome shape a cascade's source channel lives in.
			C.m_bEnabled = C.m_bRunning && bIrq;
			C.m_NextFireCycle = C.m_bEnabled ? (CurrentCycle + C.m_PeriodCycles) : UINT64_MAX;
		}
		else
		{
			RearmCtcCounter(N, Tc, bIrq, CurrentCycle);
		}
		// This channel's grid changed: counters hanging off it move too.
		RearmCtcChain(N + 1, CurrentCycle);
		return;
	}

	// First write: either a control word (bit 0 = 1) or a vector word.
	if (Value & 0x01)
	{
		C.m_ControlWord = Value;
		// Bit 1 = software reset: the channel stops until a new TC arrives.
		if (Value & 0x02)
		{
			C.m_bRunning = false;
			C.m_bEnabled = false;
			C.m_NextFireCycle = UINT64_MAX;
			RearmCtcChain(N + 1, CurrentCycle); // downstream counters lose their source
		}
		// Bit 2 = time constant follows.
		if (Value & 0x04)
		{
			C.m_bAwaitingTC = true;
		}
		if (!(Value & 0x80))
		{
			// IRQ output off. The channel KEEPS counting (bRunning + grid stay
			// live, as on real hardware) - only the interrupt deadline drops.
			C.m_bEnabled = false;
			C.m_NextFireCycle = UINT64_MAX;
		}
		// Bit 7 without TC-follow or reset re-enables IRQ on the live timer grid.
		// Disabling IRQ leaves the counter running, so re-enabling it must use
		// the next grid pulse instead of starting a new period.
		else if ((Value & 0x06) == 0 && C.m_bRunning && !C.m_bAwaitingTC)
		{
			C.m_bEnabled = true;
			C.m_NextFireCycle = NextGridPulse(C, CurrentCycle);
		}
		return;
	}
	// Z80 CTC vector word - bits 7:3 form the interrupt vector. The Spectrum
	// Next routes interrupts via NR_C0 + source index instead (channel N
	// vectors to table entry N+3), so record the byte and ignore it.
	C.m_ControlWord = Value;
}

uint8_t MemoryBus::ReadCtc(int N, uint64_t CurrentCycle) const
{
	const CtcChannel& C = m_Ctc[N & 7];
	if (!C.m_bRunning || C.m_PeriodCycles == 0)
	{
		return C.m_TimeConstant;
	}
	// Live down-counter: distance to the next ZC/TO of this channel's grid,
	// expressed in decrement steps (prescaled clocks for a timer, source
	// pulses for a counter).
	const uint64_t Next = NextGridPulse(C, CurrentCycle);
	if (Next == UINT64_MAX || Next <= CurrentCycle)
	{
		return 0;
	}
	uint64_t Step;
	if (!C.m_bCounterMode)
	{
		Step = (C.m_ControlWord & 0x20) ? 256u : 16u;
	}
	else
	{
		const CtcChannel& Src = m_Ctc[(N - 1) & 7];
		Step = (N > 0 && Src.m_PeriodCycles) ? Src.m_PeriodCycles : 1;
	}
	const uint64_t Counter = (Next - CurrentCycle + Step - 1) / Step;
	return uint8_t(Counter > 255 ? 255 : Counter);
}

void MemoryBus::WritePort7FFD(uint8_t Value)
{
	++m_Port7FFDWrites;
	if (m_bPagingLocked)
	{
		return;
	}
	if (m_bIsPlus3)
	{
		// +2A/+3: the second port ($1FFD) shares the ROM select and the special
		// all-RAM mode, so both ports go through MapPlus3.
		MapPlus3(Value, m_Port1FFD);
		return;
	}
	m_Port7FFD = Value;
	// Mirror the shadow-screen select into NR $69 bit 6 (its read-back alias).
	m_NextRegs[0x69] = uint8_t((m_NextRegs[0x69] & ~0x40) | ((Value & 0x08) ? 0x40 : 0x00));
	// On ZX-Evo the 48K lock (bit5) only bites once EFF7 LOCKMEM (bit2) is set -
	// otherwise paging stays writable, Pentagon-like. Classic machines lock on
	// bit5 alone.
	m_bPagingLocked = m_bIsZxEvo ? ((Value & 0x20) != 0 && (m_Eff7 & 0x04) != 0) : ((Value & 0x20) != 0);

	// RAM bank into $C000. ZX-Evo extends the page to 6 bits (bits 0-2 + 5-7 of
	// $7FFD -> mem7ffd, per PentEvo) = up to 1 MB; the upper reach to 4 MB comes
	// via the $x7F7/$xFF7 window ports. Classic machines use bits 0-2 only.
	const int Bank = m_bIsZxEvo ? int((Value & 0x07) | ((Value & 0xE0) >> 2)) : (Value & 0x07);
	SetRamSlot(6, Bank * 2);
	SetRamSlot(7, Bank * 2 + 1);
	// Mirror 7FFD paging into NR_56/57, as Map128K does at startup.
	// Coverage and dump restoration use these registers to identify the
	// physical page behind each CPU address.
	m_NextRegs[0x56] = uint8_t(Bank * 2);
	m_NextRegs[0x57] = uint8_t(Bank * 2 + 1);

	if (!m_bTrdosRomActive)
	{
		const int RomPage = (Value & 0x10) ? kRomPage48kBasic : kRomPage128kEditor;
		SetRomSlot(0, RomPage);
		SetRomSlot(1, RomPage + 1);
	}

	// A window RAM override (from an $xFF7/$x7F7 write) must survive the bank
	// switch this $7FFD just performed on slots 6/7 (window 3).
	if (m_bIsZxEvo)
	{
		ApplyEvoWindows();
	}
}

// --- ZX Evolution BaseConf (MM_ATM3) --------------------------------------

void MemoryBus::MapZxEvo(uint8_t Port7FFDValue, bool bUseTrdosRom)
{
	// Boot the Pentagon-class 128K base (ROM in slots 0/1, RAM banks 5/2/0 +
	// selected), then arm the BaseConf extensions so the extra ports decode and
	// the ATM renderer runs. The 6-bit $7FFD extension + window paging only kick
	// in on subsequent runtime writes (through WritePort7FFD / WriteEvoWindow).
	m_bIsZxEvo = true;
	m_Eff7 = 0;
	m_EvoFf77 = 0x03;
	m_EvoPalIdx = 0;
	for (int W = 0; W < 4; ++W)
	{
		m_EvoWinPage[W] = -1;
	}
	m_CmosAddr = 0;
	Map128K(Port7FFDValue, bUseTrdosRom);
}

void MemoryBus::FireEvoTurbo()
{
	if (!m_bIsZxEvo || !m_OnTurboChange)
	{
		return;
	}
	// #FF77 bit 3 = 14 MHz; else #EFF7 bit 4 inverted: 1 = 3.5, 0 = 7 MHz
	// (verified against UnrealSpeccy's ATM3 set_turbo; reset state = 7 MHz).
	const double Mhz = (m_EvoFf77 & 0x08) ? 14.0 : (m_Eff7 & 0x10) ? 3.5 : 7.0;
	m_OnTurboChange(m_OnTurboChangeUser, Mhz);
}

void MemoryBus::ApplyEvoWindows()
{
	// A window with an override maps its 16K CPU window straight to a RAM page;
	// -1 means "leave the standard 128K mapping in place". Window 0 ($0000) can
	// override ROM with RAM (the ATM RAM0 feature). RAM pages beyond the pool
	// (>2 MB) resolve to open bus via SetRamSlot - a documented cap.
	for (int W = 0; W < 4; ++W)
	{
		if (m_EvoWinPage[W] < 0)
		{
			continue;
		}
		const int Page8K = int(m_EvoWinPage[W]) * 2; // 16K page -> base 8K page
		SetRamSlot(W * 2, Page8K);
		SetRamSlot(W * 2 + 1, Page8K + 1);
		m_NextRegs[0x50 + W * 2] = uint8_t(Page8K & 0xFF);
		m_NextRegs[0x50 + W * 2 + 1] = uint8_t((Page8K + 1) & 0xFF);
	}
}

void MemoryBus::WriteEvoWindow(uint16_t Port, uint8_t Value)
{
	// Which 16K CPU window this port targets (A15:A14). We model a single window
	// set; the $7FFD-bit4 alternate set is a follow-on.
	const int W = (Port >> 14) & 3;

	if ((Port & 0x3FFF) == 0x37F7)
	{
		// PentEvo 4 MB "always-RAM" manager ($x7F7): page = value XOR 0xFF.
		m_EvoWinPage[W] = int16_t(Value ^ 0xFF);
		ApplyEvoWindows();
		return;
	}

	// ATM window register ($xFF7): bits 7:6 = source-select, bits 5:0 = page.
	// Faithful encode from UnrealSpeccy: (((v&0xC0)<<2)|(v&0x3F)) ^ 0x33F, where
	// bits 9:8 are the source (0=RAM/7FFD, 1=ROM/7FFD, 2=RAM/FFF7, 3=ROM/FFF7).
	const uint16_t Reg = uint16_t((((Value & 0xC0) << 2) | (Value & 0x3F)) ^ 0x33F);
	const uint16_t Source = uint16_t(Reg & 0x300);
	if (Source == 0x200) // RAM from FFF7: full 8-bit page in this window
	{
		m_EvoWinPage[W] = int16_t(Reg & 0xFF);
	}
	else // RAM-from-7FFD (0x000) reverts to the standard
	{ // mapping; ROM sources (0x100/0x300) are a
		m_EvoWinPage[W] = -1; // follow-on (BaseConf ROM banks not modelled).
	}
	ApplyEvoWindows();
}

void MemoryBus::WriteAtmPal(uint8_t Value)
{
	// ATM hardware palette (16 GRB entries). Real hardware derives the write
	// index from the beam/attribute; here we auto-advance a cursor, adequate for
	// the block palette uploads the ATM 16-colour/text modes use. Consumed by
	// the renderer (Phase 1b).
	m_EvoPal[m_EvoPalIdx & 0x0F] = Value;
	m_EvoPalIdx = uint8_t((m_EvoPalIdx + 1) & 0x0F);
}

// --- ZX-Uno register file ($FC3B select / $FD3B data) --------------------

void MemoryBus::WriteUnoData(uint8_t Value)
{
	switch (m_UnoRegSel)
	{
	case 0x0B: // SCANDBL / turbo (bits 7:6)
		m_UnoScanDbl = Value;
		if (m_OnTurboChange)
		{
			// bits 7:6 = 00/01/10/11 -> 3.5 / 7 / 14 / 28 MHz
			static constexpr double kUnoMhz[4] = {3.5, 7.0, 14.0, 28.0};
			m_OnTurboChange(m_OnTurboChangeUser, kUnoMhz[(Value >> 6) & 0x03]);
		}
		break;
	case 0x40: // RADASCTRL: bits 1:0 = 11 enables Radastan
		m_UnoRadasCtrl = uint8_t(Value & 0x03);
		break;
	case 0x41: // RADASOFFSET: low byte then high 6 bits
		if (m_UnoRadasOffPhase == 0)
		{
			m_UnoRadasOffset = uint16_t((m_UnoRadasOffset & 0x3F00) | Value);
			m_UnoRadasOffPhase = 1;
		}
		else
		{
			m_UnoRadasOffset = uint16_t((m_UnoRadasOffset & 0x00FF) | ((Value & 0x3F) << 8));
			m_UnoRadasOffPhase = 0;
		}
		break;
	case 0x42: // RADASPADDING
		m_UnoRadasPadding = Value;
		break;
	case 0x43: // RADASPALBANK (2-bit ULAplus sub-palette)
		m_UnoRadasPalBank = uint8_t(Value & 0x03);
		break;
	default: // other registers accepted + ignored
		break;
	}
}

void MemoryBus::WriteUlaPlusData(uint8_t Value)
{
	// Group is the top two bits of the register-select byte: group 0 writes a
	// CRAM palette entry (low 6 bits = index 0..63); group 1 (reg $40) is the
	// mode register, bit 0 enabling ULAplus rendering. (UnrealSpeccy io.cpp.)
	if ((m_UlaPlusReg & 0xC0) == 0x00)
	{
		m_UlaPlusCram[m_UlaPlusReg & 0x3F] = Value;
	}
	else if ((m_UlaPlusReg & 0xC0) == 0x40)
	{
		m_bUlaPlusMode = (Value & 0x01) != 0;
	}
}

uint8_t MemoryBus::ReadUnoData()
{
	if (m_UnoRegSel == 0xFF)
	{
		// CoreID string, one byte per read, 0-terminated. The invite selects
		// reg $FF then reads printable chars until 0; a real Uno returns its
		// core name here. Cursor was reset by SetUnoRegSel.
		static const char kCoreId[] = "ZX-UNO";
		const uint8_t I = m_UnoCoreIdIdx;
		if (I < uint8_t(sizeof(kCoreId) - 1))
		{
			++m_UnoCoreIdIdx;
			return uint8_t(kCoreId[I]);
		}
		return 0; // terminator
	}
	if (m_UnoRegSel == 0x0B)
	{
		return m_UnoScanDbl;
	}
	return 0xFF;
}

// ---- Classic floating bus (see memory.h) -----------------------------------
uint8_t MemoryBus::FloatingBusByte() const
{
	if (!m_bFloatingBus || m_ClkCycles == nullptr || m_ContNextPtr == nullptr || m_ContPeriodPtr == nullptr)
	{
		return 0xFF;
	}
	const uint64_t Period = *m_ContPeriodPtr;
	const uint64_t NextInt = *m_ContNextPtr;
	if (Period == 0 || NextInt == UINT64_MAX)
	{
		return 0xFF;
	}
	const uint64_t Origin = NextInt - Period; // this frame's interrupt cycle
	const uint64_t Now = *m_ClkCycles;
	if (Now < Origin)
	{
		return 0xFF;
	}
	const uint64_t InFrame = Now - Origin;
	if (InFrame < m_FbFetchStart)
	{
		return 0xFF; // before the first paper fetch
	}
	const uint64_t Rel = InFrame - m_FbFetchStart;
	const uint32_t Line = uint32_t(Rel / m_FbTperLine);
	if (Line >= 192)
	{
		return 0xFF; // below the paper area / retrace
	}
	const uint32_t Tin = uint32_t(Rel % m_FbTperLine);
	if (Tin >= 128)
	{
		return 0xFF; // outside the 128 T fetch window of the line
	}
	// Two char cells are fetched per 8 T (pixel+attr, pixel+attr). Column = the
	// character cell under the beam; return its attribute byte from the displayed
	// bank (bank 5, or bank 7 when $7FFD bit 3 selects the shadow screen).
	const uint32_t Col = Tin / 4; // 0..31
	if (Col >= 32)
	{
		return 0xFF;
	}
	const int DisplayedBank = (m_Port7FFD & 0x08) ? 7 : 5;
	const uint8_t* Vram = Ram16KBank(DisplayedBank);
	const uint32_t AttrAddr = 0x1800 + (Line / 8) * 32 + Col;
	return Vram[AttrAddr];
}
