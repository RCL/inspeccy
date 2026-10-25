#pragma once
// Z80N (zxnDMA) DMA controller, port $6B.
//
// Scope: the memset/memcpy-style byte programs real demos emit.
// zxnDMA is a wide-
// surface device; we model the exact byte sequence the demos write,
// with room to extend.
//
// Programs are streamed to port $6B as a sequence of bytes. The first
// byte of each operation either:
//   (a) is a self-contained command (DMA_RESET, DMA_LOAD, DMA_ENABLE...)
//   (b) is a "WR" byte whose bits indicate that follow-up parameter
//       bytes will arrive immediately after.
// We track an expectation count per WR write - when a WR comes in, we
// move into "expecting N follow-up bytes" mode and route the next N
// port writes into the staged-register slots.
//
// Transfer execution: on DMA_ENABLE we arm `BytesRemaining` and let the
// dispatcher byte-step the transfer via `StepZxnDma` (4 T-states per byte),
// bounded by the next emulator event so interrupts can preempt mid-flight.
// `bPortAFixed` controls whether the source cursor advances per byte -
// the rest of the loop is mode-agnostic. Writes go through `Mem.Wr` so
// MMU8K mapping is respected.

#include <stdint.h>

struct MemoryBus;

struct ZxnDmaState
{
	// ------ command-byte tracking ------
	enum WaitMode : uint8_t
	{
		kWaitModeNone, // expecting next command/WR byte
		kWaitModeWr0Params, // expecting port-A start addr + block length follow-ups
		kWaitModeWr4Params, // expecting port-B start addr follow-up
		kWaitModeR6MaskParam, // expecting one mask byte after CMD_READ_MASK_FOLLOWS ($BB)
	};
	WaitMode m_Mode = WaitMode::kWaitModeNone;
	uint8_t m_Wr0PendingBytes = 0;
	uint8_t m_Wr0Buffer[4] = {0, 0, 0, 0};
	uint8_t m_Wr4PendingBytes = 0;
	uint8_t m_Wr4Buffer[2] = {0, 0};
	uint8_t m_ReadMask = 0x7F; // R6 read mask; reset value per dma.vhd:239

	// ------ staged DMA program ------
	uint16_t m_PortASrcAddr = 0;
	uint16_t m_PortBDestAddr = 0;
	uint16_t m_BlockLength = 0;
	bool m_bPortAFixed = false; // WR1 $24 = port A memory, fixed-address
	bool m_bPortADec = false;
	bool m_bPortBInc = false; // WR2 $10 = port B memory, incrementing
	bool m_bPortBDec = false;
	bool m_bPortBIo = false; // WR2 bit 3 = port B is I/O (writes go to a port, not memory)
	bool m_bContinuousMode = false; // WR4 $AD = continuous mode
	bool m_bStopAtEnd = false; // WR5 $82
	bool m_bLoaded = false; // DMA_LOAD seen since last reset
	bool m_bForceReady = false; // DMA_FORCE_READY seen since last reset

	// ------ counters for status / dump ------
	uint64_t m_Transfers = 0;
	uint64_t m_BytesTransferred = 0;
	uint64_t m_UnknownBytes = 0; // bytes we couldn't interpret
	uint64_t m_LastTransferCycle = 0;
	uint64_t m_NestedArms = 0; // CMD_ENABLE while BytesRemaining > 0

	// ------ in-flight transfer state ------
	// Non-zero BytesRemaining means a CMD_ENABLE has armed a transfer
	// that the dispatcher steps through byte-by-byte, yielding to
	// interrupts between chunks. CurSrcAddr / CurDstAddr are the
	// running cursors; PortASrcAddr / PortBDestAddr from the staged
	// program are NOT mutated so dumps still reflect what was programmed.
	uint32_t m_BytesRemaining = 0;
	uint16_t m_CurSrcAddr = 0;
	uint16_t m_CurDstAddr = 0;

	void Reset()
	{
		m_Mode = WaitMode::kWaitModeNone;
		m_Wr0PendingBytes = m_Wr4PendingBytes = 0;
		m_PortASrcAddr = m_PortBDestAddr = m_BlockLength = 0;
		m_bPortAFixed = m_bPortBInc = m_bPortBIo = m_bContinuousMode = m_bStopAtEnd = false;
		m_bPortADec = m_bPortBDec = false;
		m_bLoaded = m_bForceReady = false;
		m_BytesRemaining = 0;
		m_CurSrcAddr = m_CurDstAddr = 0;
	}
};

// Handle a byte written to port $6B. Cycles tracks when the write
// happened, for the LastTransferCycle field. CMD_ENABLE now ARMS a
// transfer (sets BytesRemaining / CurSrcAddr / CurDstAddr) but does
// not execute or charge cycles - the dispatcher steps the transfer
// byte-by-byte via StepZxnDma so interrupts can preempt mid-flight.
void WriteZxnDma(ZxnDmaState& InOutDma, uint8_t Value, MemoryBus& InOutMem, uint64_t Cycles);

// Advance an armed transfer by up to MaxBytes. Returns bytes actually
// stepped. Caller charges 4 Z80 T-states per stepped byte (2 read +
// 2 write at the post-reset default per-port cycle length). Does not
// poll interrupts itself; the caller bounds MaxBytes so the next
// emulator event lands at or after the chunk boundary.
uint32_t StepZxnDma(ZxnDmaState& InOutDma, MemoryBus& InOutMem, uint32_t MaxBytes);

// Read from port $6B. Returns the DMA status byte per VHDL dma.vhd:902
// ("00 endofblock_n 1101 atleastone"). Bit 5 is 0 when the most recent
// transfer has completed, 1 while in progress. Real hw advances an
// internal read pointer per R6_read_mask after each read; inspeccy
// doesn't model that pointer, so the return is unconditionally status
// - fine in practice because callers pin the mask to status-only via
// `$BB $01 $A7` before polling.
uint8_t ReadZxnDma(const ZxnDmaState& Dma);
