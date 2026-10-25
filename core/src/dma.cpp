#include "dma.h"

#include "memory.h"

namespace
{

// Single-byte commands (zxnDMA command bytes).
constexpr uint8_t kCmdDisable = 0x83;
constexpr uint8_t kCmdReset = 0xC3;
constexpr uint8_t kCmdResetPortATiming = 0xC7;
constexpr uint8_t kCmdResetPortBTiming = 0xCB;
constexpr uint8_t kCmdLoad = 0xCF;
constexpr uint8_t kCmdContinue = 0xD3;
constexpr uint8_t kCmdDisableInterrupts = 0xAF;
constexpr uint8_t kCmdEnableInterrupts = 0xAB;
constexpr uint8_t kCmdForceReady = 0xB3;
constexpr uint8_t kCmdEnableAfterReti = 0xB7;
constexpr uint8_t kCmdReadStatusByte = 0xBF;
constexpr uint8_t kCmdReinitStatusByte = 0x8B;
constexpr uint8_t kCmdStartReadSequence = 0xA7;
constexpr uint8_t kCmdEnable = 0x87;
constexpr uint8_t kCmdReadMaskFollows = 0xBB;

// WR-control byte values used by typical demo memset programs. (Bigger
// zxnDMA programs encode more bits in their WR0/WR4 bytes; we only
// recognise these exact values.)
constexpr uint8_t kWr0BlockPortaA2bXfr = 0x7D; // -> 4 follow-up bytes
constexpr uint8_t kWr0BlockPortaB2aXfr = 0x79; // -> 4 follow-up bytes
constexpr uint8_t kWr1PortaFixedMemory = 0x24;
constexpr uint8_t kWr1PortaDecMemory = 0x04;
constexpr uint8_t kWr1PortaIncMemory = 0x14;
constexpr uint8_t kWr2PortbFixedMemory = 0x20;
constexpr uint8_t kWr2PortbDecMemory = 0x00;
constexpr uint8_t kWr2PortbIncMemory = 0x10;
constexpr uint8_t kWr2PortbFixedIo = 0x28; // bit 3 = port B is I/O
constexpr uint8_t kWr2PortbDecIo = 0x08;
constexpr uint8_t kWr2PortbIncIo = 0x18;
constexpr uint8_t kWr4ContMode = 0xAD; // -> 2 follow-up bytes
constexpr uint8_t kWr5StopAtEnd = 0x82;

} // namespace

uint32_t StepZxnDma(ZxnDmaState& InOutDma, MemoryBus& InOutMem, uint32_t MaxBytes)
{
	if (InOutDma.m_BytesRemaining == 0 || MaxBytes == 0)
	{
		return 0;
	}
	uint32_t Done = (MaxBytes < InOutDma.m_BytesRemaining) ? MaxBytes : InOutDma.m_BytesRemaining;
	for (uint32_t IdxI = 0; IdxI < Done; ++IdxI)
	{
		const uint8_t B = InOutMem.Rd(InOutDma.m_CurSrcAddr);
		InOutMem.Wr(InOutDma.m_CurDstAddr, B);
		if (!InOutDma.m_bPortAFixed)
		{
			InOutDma.m_CurSrcAddr = uint16_t(InOutDma.m_CurSrcAddr + (InOutDma.m_bPortADec ? -1 : 1));
		}
		if (InOutDma.m_bPortBInc)
		{
			InOutDma.m_CurDstAddr = uint16_t(InOutDma.m_CurDstAddr + 1);
		}
		else if (InOutDma.m_bPortBDec)
		{
			InOutDma.m_CurDstAddr = uint16_t(InOutDma.m_CurDstAddr - 1);
		}
	}
	InOutDma.m_BytesRemaining -= Done;
	InOutDma.m_BytesTransferred += Done;
	return Done;
}

void WriteZxnDma(ZxnDmaState& InOutDma, uint8_t Value, MemoryBus& InOutMem, uint64_t Cycles)
{
	(void)InOutMem;
	// First handle the "expecting follow-up bytes" modes - these consume
	// raw values into the staged-register buffer regardless of what the
	// byte happens to look like.
	if (InOutDma.m_Mode == ZxnDmaState::WaitMode::kWaitModeWr0Params)
	{
		InOutDma.m_Wr0Buffer[4 - InOutDma.m_Wr0PendingBytes] = Value;
		--InOutDma.m_Wr0PendingBytes;
		if (InOutDma.m_Wr0PendingBytes == 0)
		{
			InOutDma.m_PortASrcAddr = uint16_t(InOutDma.m_Wr0Buffer[0] | (uint16_t(InOutDma.m_Wr0Buffer[1]) << 8));
			InOutDma.m_BlockLength = uint16_t(InOutDma.m_Wr0Buffer[2] | (uint16_t(InOutDma.m_Wr0Buffer[3]) << 8));
			InOutDma.m_Mode = ZxnDmaState::WaitMode::kWaitModeNone;
		}
		return;
	}
	if (InOutDma.m_Mode == ZxnDmaState::WaitMode::kWaitModeWr4Params)
	{
		InOutDma.m_Wr4Buffer[2 - InOutDma.m_Wr4PendingBytes] = Value;
		--InOutDma.m_Wr4PendingBytes;
		if (InOutDma.m_Wr4PendingBytes == 0)
		{
			InOutDma.m_PortBDestAddr = uint16_t(InOutDma.m_Wr4Buffer[0] | (uint16_t(InOutDma.m_Wr4Buffer[1]) << 8));
			InOutDma.m_Mode = ZxnDmaState::WaitMode::kWaitModeNone;
		}
		return;
	}
	if (InOutDma.m_Mode == ZxnDmaState::WaitMode::kWaitModeR6MaskParam)
	{
		InOutDma.m_ReadMask = Value;
		InOutDma.m_Mode = ZxnDmaState::WaitMode::kWaitModeNone;
		return;
	}

	// Single-byte commands first.
	switch (Value)
	{
	case kCmdReset:
		InOutDma.Reset();
		return;
	case kCmdDisable:
	case kCmdResetPortATiming:
	case kCmdResetPortBTiming:
	case kCmdDisableInterrupts:
	case kCmdEnableInterrupts:
	case kCmdEnableAfterReti:
	case kCmdReadStatusByte:
	case kCmdReinitStatusByte:
	case kCmdStartReadSequence:
	case kCmdContinue:
		return;
	case kCmdReadMaskFollows:
		// $BB consumes one follow-up byte = the new R6 read mask.
		InOutDma.m_Mode = ZxnDmaState::WaitMode::kWaitModeR6MaskParam;
		return;
	case kCmdLoad:
		InOutDma.m_bLoaded = true;
		return;
	case kCmdForceReady:
		InOutDma.m_bForceReady = true;
		return;
	case kCmdEnable:
		if (InOutDma.m_bLoaded)
		{
			// Arm the transfer. The dispatcher steps it byte-by-byte
			// (via StepZxnDma) bounded by the next emulator event so
			// interrupts can preempt mid-flight.
			//
			if (InOutDma.m_BytesRemaining > 0)
			{
				++InOutDma.m_NestedArms;
			}
			// BlockLength == 0 means 65536 in the Z80-DMA encoding (full
			// 16-bit address-space transfer), per Zilog Z80-DMA spec.
			InOutDma.m_BytesRemaining = (InOutDma.m_BlockLength == 0) ? 65536u : uint32_t(InOutDma.m_BlockLength);
			InOutDma.m_CurSrcAddr = InOutDma.m_PortASrcAddr;
			InOutDma.m_CurDstAddr = InOutDma.m_PortBDestAddr;
			++InOutDma.m_Transfers;
			InOutDma.m_LastTransferCycle = Cycles;
			if (InOutDma.m_bStopAtEnd)
			{
				InOutDma.m_bLoaded = false;
				InOutDma.m_bForceReady = false;
			}
		}
		return;
	}

	// WR-control bytes.
	switch (Value)
	{
	case kWr0BlockPortaA2bXfr:
	case kWr0BlockPortaB2aXfr:
		// Programs pair this with port A start addr (2 bytes) +
		// block length (2 bytes).
		InOutDma.m_Mode = ZxnDmaState::WaitMode::kWaitModeWr0Params;
		InOutDma.m_Wr0PendingBytes = 4;
		return;
	case kWr1PortaFixedMemory:
		InOutDma.m_bPortAFixed = true;
		InOutDma.m_bPortADec = false;
		return;
	case kWr1PortaDecMemory:
	case kWr1PortaIncMemory:
		InOutDma.m_bPortAFixed = false;
		InOutDma.m_bPortADec = Value == kWr1PortaDecMemory;
		return;
	case kWr2PortbFixedMemory:
	case kWr2PortbDecMemory:
		InOutDma.m_bPortBInc = false;
		InOutDma.m_bPortBDec = Value == kWr2PortbDecMemory;
		InOutDma.m_bPortBIo = false;
		return;
	case kWr2PortbIncMemory:
		InOutDma.m_bPortBInc = true;
		InOutDma.m_bPortBDec = false;
		InOutDma.m_bPortBIo = false;
		return;
	case kWr2PortbFixedIo:
	case kWr2PortbDecIo:
		InOutDma.m_bPortBInc = false;
		InOutDma.m_bPortBDec = Value == kWr2PortbDecIo;
		InOutDma.m_bPortBIo = true;
		return;
	case kWr2PortbIncIo:
		InOutDma.m_bPortBInc = true;
		InOutDma.m_bPortBDec = false;
		InOutDma.m_bPortBIo = true;
		return;
	case kWr4ContMode:
		InOutDma.m_bContinuousMode = true;
		InOutDma.m_Mode = ZxnDmaState::WaitMode::kWaitModeWr4Params;
		InOutDma.m_Wr4PendingBytes = 2;
		return;
	case kWr5StopAtEnd:
		InOutDma.m_bStopAtEnd = true;
		return;
	}

	// Unknown WR byte - surface in dump counters so we can find out
	// which demo uses what we don't model yet.
	++InOutDma.m_UnknownBytes;
	return;
}

uint8_t ReadZxnDma(const ZxnDmaState& Dma)
{
	// Status byte layout per dma.vhd:902 - "00 endofblock_n 1101 atleastone".
	const uint8_t EndOfBlockN = (Dma.m_BytesRemaining > 0) ? 1u : 0u;
	const uint8_t AtLeastOne = (Dma.m_BytesTransferred > 0) ? 1u : 0u;
	return uint8_t(0b00001010u | (EndOfBlockN << 5) | AtLeastOne);
}
