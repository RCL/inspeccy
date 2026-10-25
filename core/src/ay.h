#pragma once
#include <stdint.h>

// AY-3-8910 register state. Just the 14 user-visible registers + R14/R15
// (I/O ports, not used on Spectrum but captured for fidelity) + the
// selected-register latch.
//
// Default state is all zeros, matching cold-boot AY (no sound).
struct AyState
{
	uint8_t m_Registers[16] = {}; // 0..13 user; 14..15 I/O ports
	uint8_t m_Latch = 0; // selected register (masked to 4 bits on write)
	uint64_t m_WritesObserved = 0; // total writes to data port (regardless of capture)
	uint64_t m_EnvelopeWrites = 0; // R13 retriggers even when the value is unchanged
	uint16_t m_RegistersTouched = 0; // bit N set if R(N) was ever written this run
};

// Real AY only latches certain bit widths per register. Applied on data writes.
inline constexpr uint8_t kAyWriteMask[16] = {
	0xFF, // R0  tone A period low (8 bits)
	0x0F, // R1  tone A period high (4)
	0xFF, // R2  tone B period low (8)
	0x0F, // R3  tone B period high (4)
	0xFF, // R4  tone C period low (8)
	0x0F, // R5  tone C period high (4)
	0x1F, // R6  noise period (5)
	0x3F, // R7  mixer (6)
	0x1F, // R8  channel A volume (5)
	0x1F, // R9  channel B volume (5)
	0x1F, // R10 channel C volume (5)
	0xFF, // R11 envelope period low (8)
	0xFF, // R12 envelope period high (8)
	0x0F, // R13 envelope shape (4) - write retriggers env
	0xFF, // R14 I/O port A (not used on Spectrum)
	0xFF, // R15 I/O port B (not used on Spectrum)
};
