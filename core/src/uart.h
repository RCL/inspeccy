#pragma once
// ZX Spectrum Next UART (core 3.x register interface), modelled from the
// FPGA sources (src/serial/uart.vhd and friends):
//
//   $133B  write: Tx FIFO push          read: status
//   $143B  write: prescaler bits 6:0 (bit7=0) / 13:7 (bit7=1)   read: Rx FIFO pop (0 if empty)
//   $153B  write: bit6 = select (0 ESP, 1 Pi); bit4 = write prescaler bits 16:14 from bits 2:0
//          read: bit6 = select, bits 2:0 = prescaler 16:14
//   $163B  write/read: framing  (bit7 reset, bit6 tx break, bit5 hw flow, bits4:3 data bits-5,
//          bit2 parity enable, bit1 odd parity, bit0 two stop bits); hard reset default $18 = 8N1
//
//   status: bit7 rx break, bit6 rx framing/parity error (sticky, cleared by a status read),
//           bit5 parity error of the head byte, bit4 tx empty (FIFO empty and transmitter idle),
//           bit3 rx FIFO >= 3/4 full, bit2 rx overflow (sticky, cleared by a status read),
//           bit1 tx FIFO full, bit0 rx data available.
//
// Two channels, 512-byte Rx and 64-byte Tx FIFOs each. Bytes cross the link
// at the programmed baud: prescaler system clocks (28 MHz) per bit, 8N1 = 10
// bits. The host side of a channel is a UartBackend: a loopback, or a child
// process whose stdin/stdout stand in for the wire (the "NextPi" shim). The
// Pi channel only moves bytes while NR $A0 bit 4 enables the GPIO UART, as on
// hardware. The Rx interrupt sources of NR $C6 (any byte, or the FIFO 3/4 full)
// are raised through the IM 2 hardware vectoring (z80.cpp); Tx empty is not.
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

struct UartBackend
{
	virtual ~UartBackend() = default;
	virtual int Read(uint8_t* Out, int Max) = 0; // non-blocking; bytes read, 0 none, -1 closed
	virtual bool Write(const uint8_t* Data, int Count) = 0;
	virtual bool IsAlive() const { return true; }
};
std::unique_ptr<UartBackend> MakeLoopbackUartBackend();
// Spawns `Command` through the shell; its stdin receives what the Next transmits,
// its stdout feeds the Next's receiver. nullptr with OutErr set on failure.
std::unique_ptr<UartBackend> MakeProcessUartBackend(const std::string& Command, std::string& OutErr);
// "loop" -> loopback; anything else -> a process. Empty -> nullptr (no link).
std::unique_ptr<UartBackend> MakeUartBackend(const std::string& Spec, std::string& OutErr);

struct NextUartChannel
{
	uint32_t m_Prescaler = 243; // 17 bits; 243 = 115200 baud at 28 MHz
	uint8_t m_Framing = 0x18;
	std::deque<uint8_t> m_Rx, m_Tx; // FIFOs as the CPU sees them
	bool m_bTxInFlight = false; // the byte currently on the wire has left the FIFO
	uint8_t m_TxByte = 0;
	uint64_t m_TxDoneNs = 0; // when the in-flight byte has been sent
	std::deque<uint8_t> m_Pending; // host bytes that have not yet reached the Rx FIFO
	uint64_t m_RxNextNs = 0; // when the next pending byte completes
	uint64_t m_LastPollNs = 0;
	bool m_bOverflow = false, m_bFramingError = false;
	std::unique_ptr<UartBackend> m_Backend;
	uint64_t m_WireBytesIn = 0, m_WireBytesOut = 0, m_Dropped = 0, m_RxInts = 0; // statistics

	uint64_t BitNs() const;
	uint64_t ByteNs() const;
	bool InReset() const { return (m_Framing & 0x80) != 0; }
};

struct NextUart
{
	enum { kEsp = 0, kPi = 1 };
	NextUartChannel m_Ch[2];
	bool m_bSelectPi = false;
	double m_SystemClockHz = 28000000.0;
	// Diagnostic: called when a channel drops a byte (Rx FIFO full); Ch = kEsp/kPi.
	void (*m_OnOverflow)(void* User, int Ch) = nullptr;
	void* m_OnOverflowUser = nullptr;

	static bool IsUartPort(uint16_t Port)
	{
		return (Port & 0xFF) == 0x3B && (Port >> 8) >= 0x13 && (Port >> 8) <= 0x16;
	}
	void Reset(bool bHard);
	// bPiLinked: NR $A0 bit 4 (the GPIO UART function) at the time of the access.
	uint8_t ReadPort(uint16_t Port, uint64_t NowNs, bool bPiLinked);
	void WritePort(uint16_t Port, uint8_t Value, uint64_t NowNs, bool bPiLinked);
	// Moves bytes between FIFOs and backends up to NowNs. Called from every
	// port access and once per frame, so a program that stops touching the
	// UART still gets its last bytes delivered.
	void Advance(uint64_t NowNs, bool bPiLinked);
	// Delivers everything queued, ignoring baud timing (end of a run).
	void Flush(bool bPiLinked);
	// The channel's Rx interrupt request level (NR $C6): the FIFO holds a byte, or
	// in near-full mode at least 384 of its 512 (uart.vhd: "3/4 full", held).
	bool RxIntRequest(int Ch, bool bNearFullMode) const;
};
