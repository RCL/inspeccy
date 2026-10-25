#pragma once
#include <trace/trace_binary.h>

#include <QColor>
#include <QCoreApplication>
#include <QString>

#include <stdint.h>

// Where a cycle of CPU time went. Every cycle the overview accounts for belongs
// to exactly one bucket: the main flow, or the service routine of one
// identified interrupt source. Ids are small stable integers so a frame's whole
// breakdown fits in a fixed-width row:
//
//   0         main flow (everything not inside an accepted interrupt)
//   1 .. 14   interrupt source index 0..13 (BinaryTrace::IntSourceIndex)
//   15        an interrupt whose source the trace did not record
//
// The source of an interrupt comes from the acknowledge bus value the writer
// stores in a kRecord_Interrupt record's opcode field - see the
// "Interrupt-record opcode field" section of trace_binary.h.
namespace CpuSource
{

constexpr int kMain = 0;
constexpr int kIntBase = 1;
constexpr int kIntUnknown = 15;
constexpr int kCount = 16;

// Bus value recorded on an interrupt record -> bucket id.
inline int FromIntAck(const uint8_t Opcodes[4])
{
	if (Opcodes[1] != BinaryTrace::kIntAckMarker)
	{
		return kIntUnknown; // trace predates the field, or a foreign writer
	}
	const uint8_t Bus = Opcodes[0];
	// $FF means no device drove the bus, which is the classic ULA frame
	// interrupt. Folding it onto the Next's hardware-vectored ULA source makes
	// 48K/128K and Next traces read alike.
	if (Bus == 0xFF)
	{
		return kIntBase + BinaryTrace::kIntSrc_Ula;
	}
	const int Index = (Bus & 0x1F) >> 1;
	if (Index >= BinaryTrace::kIntSrc_Count)
	{
		return kIntUnknown;
	}
	return kIntBase + Index;
}

inline QString Name(int SourceId)
{
	if (SourceId == kMain)
	{
		return QCoreApplication::translate("CpuSource", "main flow");
	}
	if (SourceId == kIntUnknown)
	{
		return QCoreApplication::translate("CpuSource", "interrupt (source not recorded)");
	}

	const int Index = SourceId - kIntBase;
	switch (Index)
	{
	case BinaryTrace::kIntSrc_Line:
		return QCoreApplication::translate("CpuSource", "scanline interrupt");
	case BinaryTrace::kIntSrc_Uart0Rx:
		return QCoreApplication::translate("CpuSource", "UART0 Rx interrupt");
	case BinaryTrace::kIntSrc_Uart1Rx:
		return QCoreApplication::translate("CpuSource", "UART1 Rx interrupt");
	case BinaryTrace::kIntSrc_Ula:
		return QCoreApplication::translate("CpuSource", "frame interrupt");
	case BinaryTrace::kIntSrc_Uart0Tx:
		return QCoreApplication::translate("CpuSource", "UART0 Tx interrupt");
	case BinaryTrace::kIntSrc_Uart1Tx:
		return QCoreApplication::translate("CpuSource", "UART1 Tx interrupt");
	default:
		break;
	}
	if (Index >= BinaryTrace::kIntSrc_Ctc0 && Index <= BinaryTrace::kIntSrc_Ctc7)
	{
		return QCoreApplication::translate("CpuSource", "CTC%1 interrupt").arg(Index - BinaryTrace::kIntSrc_Ctc0);
	}
	return QCoreApplication::translate("CpuSource", "interrupt source %1").arg(Index);
}

// One hue per bucket, chosen to stay separable when stacked in a 1px column.
// Main flow is the cool base everything else sits on top of; the frame
// interrupt is the warm one you look for first.
inline QColor Colour(int SourceId)
{
	if (SourceId == kMain)
	{
		return QColor(0x4a, 0x8f, 0xc7); // steel blue
	}
	if (SourceId == kIntUnknown)
	{
		return QColor(0x8a, 0x8a, 0x8a); // grey
	}

	const int Index = SourceId - kIntBase;
	switch (Index)
	{
	case BinaryTrace::kIntSrc_Ula:
		return QColor(0xf2, 0xb1, 0x3c); // amber
	case BinaryTrace::kIntSrc_Line:
		return QColor(0xa9, 0x76, 0xd8); // violet
	case BinaryTrace::kIntSrc_Uart0Rx:
		return QColor(0x6f, 0x8c, 0xa8);
	case BinaryTrace::kIntSrc_Uart1Rx:
		return QColor(0x54, 0x70, 0x8c);
	case BinaryTrace::kIntSrc_Uart0Tx:
		return QColor(0x9a, 0xa8, 0xb8);
	case BinaryTrace::kIntSrc_Uart1Tx:
		return QColor(0x7c, 0x8a, 0x9a);
	default:
		break;
	}
	if (Index >= BinaryTrace::kIntSrc_Ctc0 && Index <= BinaryTrace::kIntSrc_Ctc7)
	{
		static const QColor kCtc[8] = {
			QColor(0x4c, 0xc0, 0x7a), // CTC0 green
			QColor(0xe0, 0x6c, 0x45), // CTC1 orange
			QColor(0x3f, 0xc4, 0xc4), // CTC2 cyan
			QColor(0xd4, 0x5c, 0x8c), // CTC3 pink
			QColor(0x9b, 0xc4, 0x3f), // CTC4 lime
			QColor(0xc7, 0x8f, 0x4a), // CTC5 bronze
			QColor(0x6d, 0x7a, 0xd8), // CTC6 indigo
			QColor(0xc4, 0x4f, 0x4f), // CTC7 red
		};
		return kCtc[Index - BinaryTrace::kIntSrc_Ctc0];
	}
	return QColor(0xa0, 0xa0, 0xa0);
}

} // namespace CpuSource
