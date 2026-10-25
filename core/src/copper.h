#pragma once
// ZX Spectrum Next "Copper" - a beam-synchronized coprocessor that writes
// NextRegs at raster positions, in parallel with the Z80 and WITHOUT stalling
// it (it lives in FPGA logic, not on the CPU bus). Controlled via NextRegs
// $60-$63. Used for per-scanline effects: screen shake, palette flips, LoRes
// vertical doubling, mid-frame mode changes.
//
// Design (the vAmiga "bus-ownership" lesson, not UAE's retrofit-timings trap):
// the copper is an event-scheduled agent on the emulator's single T-state
// timeline, the same shape as the zxnDMA (see dma.h). Programming (WriteCopper)
// only STAGES state cheaply; execution (StepCopper) is driven by the dispatcher
// when the copper's next WAIT deadline is crossed. Between WAITs the copper
// costs nothing; when stopped/absent NextFireCycle stays UINT64_MAX so
// RecomputeNextEvent never schedules it and the dispatcher never wakes it.
//
// Instruction memory = 1024 big-endian 16-bit instructions (2 KB):
//   MOVE (bit15=0): 0RRRRRRR VVVVVVVV  -> nextreg R (0..127) = V, then advance.
//   WAIT (bit15=1): 1HHHHHHV VVVVVVVV  -> stall until the beam reaches
//                                         (vertical line = 9-bit V, horizontal
//                                         = 6-bit H). WAIT (511,63) (= $FFFF)
//                                         parks until the frame restart.
//
// Hardware semantics confirmed against the Next FPGA VHDL (cores/zxnext,
// device/copper.vhd + video/zxula_timing.vhd), Jul 2026:
//   * WAIT matches on `vcount == V` (EQUALITY) and `hcount >= H*8 + 12`, where
//     vcount is the copper's PAPER-RELATIVE vertical counter (`cvc`, = paper
//     line 0 at the first active display line + NR $64 offset) and hcount is the
//     raw horizontal counter. So the WAIT line field is display-row-relative
//     (line 0 == display row 0), and H is an 8-pixel-granular horizontal unit
//     offset +12 raw-hcount into the line. TopBorderLines below bridges the
//     copper/display line 0 to the frame interrupt (which fires ~a top-border
//     above it, in the border).
//   * A MOVE writes its NextReg at the exact beam position it executes (1-clock
//     pulse; reg 0 is a NOP-guarded no-write). LoRes/Layer2 consume scroll (NR
//     $32/$33, $16/$17), the L2 bank (NR $12) and palette-offset LIVE PER PIXEL
//     (combinational / unconditional per-pixel-clock capture - NOT latched per
//     line), so a mid-scanline copper write takes effect from that pixel on the
//     CURRENT line. This is why per-column register sampling in the compositor
//     is faithful, not a fiction.

#include <stdint.h>

struct MemoryBus;
struct Z80State;

struct CopperState
{
	// Present only on Next machines and not suppressed by --no-copper. When
	// false, $60-$63 writes are ignored (values still land in NextRegs[]) and
	// the copper is never armed, so it costs nothing.
	bool m_bPresent = false;

	// 2 KB instruction memory = 1024 instructions, byte-addressed. Instruction
	// N is the big-endian pair Mem2K[2N] (hi), Mem2K[2N+1] (lo).
	uint8_t m_Mem2K[2048] = {};

	// ---- programming state (NR $60-$63) ----
	uint16_t m_WriteAddr = 0; // 11-bit byte cursor into Mem2K (auto-inc)
	uint8_t m_Data16Hi = 0; // NR $63: buffered high byte
	bool m_bData16HaveHi = false; // NR $63: high byte latched, low byte pending

	// ---- run state ----
	uint8_t m_Mode = 0; // NR $62[7:6]: 0=stop 1=run(+reset) 2=run 3=run+frame-restart
	bool m_bRunning = false;
	bool m_bRestartEachFrame = false; // Mode 3
	uint16_t m_Cpc = 0; // program counter (instruction index 0..1023)
	uint64_t m_NextFireCycle = UINT64_MAX; // dispatcher deadline; MAX = parked/stopped
	uint64_t m_FrameOrigin = UINT64_MAX; // F0 of the frame the current run belongs to
	uint16_t m_LinesPerFrame = 312; // machine constant (Next50=312, Next60=262); set at setup
	// Raster lines between the frame interrupt (raster line 0 / frame origin) and
	// display row 0 (first paper line) = the top border height. On hardware the
	// copper's vertical counter is paper-relative and the interrupt fires a
	// border above it, so this both (a) maps a copper WAIT(line D) to display row
	// D and (b) parks the interrupt ISR in the top border. From zxula_timing.vhd
	// c_min_vactive - c_int_v (48K-class 50 Hz = 64, 60 Hz = 40). Set at setup.
	uint16_t m_TopBorderLines = 0;

	// ---- diagnostics (dump/status/tests) ----
	uint64_t m_Moves = 0; // total MOVEs executed
	uint64_t m_Waits = 0; // total WAITs consumed
	uint64_t m_Restarts = 0; // frame restarts (mode 3)

	// StepCopperTo is running: a MOVE it applies fires a beam boundary, and the
	// boundary must not start a nested step (MemoryBus::FireBeamBoundary).
	bool m_bStepping = false;

	void ResetProgram()
	{
		m_WriteAddr = 0;
		m_Data16Hi = 0;
		m_bData16HaveHi = false;
	}
	void Stop()
	{
		m_Mode = 0;
		m_bRunning = false;
		m_bRestartEachFrame = false;
		m_Cpc = 0;
		m_NextFireCycle = UINT64_MAX;
		m_FrameOrigin = UINT64_MAX;
	}
};

// Program one byte written to a copper control register ($60/$61/$62/$63).
// Cheap: stages instruction memory or control, arms/stops the run. Never
// executes copper instructions. CurrentCycle is used to seed NextFireCycle
// when the copper is armed so the dispatcher schedules it promptly.
void WriteCopper(CopperState& InOutCop, uint8_t RegIndex, uint8_t Value, uint64_t CurrentCycle);

// Advance the copper from its current Cpc: apply MOVEs (via Mem.WriteNextReg)
// and consume already-satisfied WAITs until the next WAIT lands in the future,
// then set NextFireCycle to that WAIT's cycle and return. Called by the
// dispatcher when Cpu.Cycles >= Cop.NextFireCycle. Derives cycles-per-line
// from Cpu.FrameIntPeriod / Cop.LinesPerFrame so it scales with turbo.
void StepCopper(CopperState& InOutCop, MemoryBus& InOutMem, Z80State& InOutCpu);
// The same, caught up to Now with the given frame period: for callers without
// the CPU state at hand (the memory bus, catching the copper up before a beam
// flush).
void StepCopperTo(CopperState& InOutCop, MemoryBus& InOutMem, uint64_t Now, uint64_t InFrameIntPeriod);
