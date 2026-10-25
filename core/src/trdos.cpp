#include "trdos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{

constexpr size_t kSectorSize = 256;
constexpr int kSectorsPerTrack = 16;
constexpr size_t kTrdDsBytes = 655360; // 80 tracks * 2 sides * 16 sectors * 256
constexpr size_t kTrdSsBytes = 327680; // 80 tracks * 1 side  * 16 sectors * 256

// FDC status bits.
constexpr uint8_t kFdcBusy = 0x01;
constexpr uint8_t kFdcIndex = 0x02; // Type I (pulsed; we leave 0)
constexpr uint8_t kFdcDrq = 0x02; // Type II/III (overlaps Index)
constexpr uint8_t kFdcTrack0 = 0x04; // Type I
constexpr uint8_t kFdcLostdata = 0x04; // Type II/III (overlaps Track0)
constexpr uint8_t kFdcCrcerror = 0x08;
constexpr uint8_t kFdcSeekerror = 0x10; // Type I
constexpr uint8_t kFdcRecnotfound = 0x10; // Type II/III (overlaps SeekError)
constexpr uint8_t kFdcHeadloaded = 0x20; // Type I
constexpr uint8_t kFdcRectype = 0x20; // Type II/III (deleted data marker)
constexpr uint8_t kFdcWriteprot = 0x40;
constexpr uint8_t kFdcNotready = 0x80;

bool ReadEntireFile(const char* Path, std::vector<uint8_t>& OutData, std::string& OutError)
{
	FILE* F = fopen(Path, "rb");
	if (F == nullptr)
	{
		OutError = std::string("cannot open file: ") + Path;
		return false;
	}
	fseek(F, 0, SEEK_END);
	const long Size = ftell(F);
	fseek(F, 0, SEEK_SET);
	if (Size < 0)
	{
		fclose(F);
		OutError = "ftell failed";
		return false;
	}
	OutData.resize(size_t(Size));
	const size_t Got = fread(OutData.data(), 1, size_t(Size), F);
	fclose(F);
	if (Got != size_t(Size))
	{
		OutError = "short read";
		return false;
	}
	return true;
}

inline size_t TrdSectorOffset(int Track, int Side, int Sector1Based, int Sides)
{
	// (Track * Sides + Side) * SECTORS_PER_TRACK + (Sector - 1)
	return ((size_t(Track) * size_t(Sides) + size_t(Side)) * kSectorsPerTrack + size_t(Sector1Based - 1)) * kSectorSize;
}

} // namespace

Trdos::Trdos()
{
	Reset();
}

void Trdos::Reset()
{
	m_Command = m_Status = m_Track = m_Sector = m_Data = m_System = 0;
	m_HeadTrack = 0;
	m_Side = 0;
	m_BufferPos = m_BufferLen = 0;
	m_bBusy = m_bIrq = m_bDrq = false;
	m_LastClass = CommandClass::kCommandClassTypeI;
	m_Status = m_Image.m_bMounted ? kFdcTrack0 | kFdcHeadloaded : kFdcNotready;
}

bool Trdos::MountDisk(const char* Path, std::string& OutError)
{
	std::vector<uint8_t> Raw;
	if (!ReadEntireFile(Path, Raw, OutError))
	{
		return false;
	}

	// Auto-detect by content. SCL signature is "SINCLAIR" (8 bytes).
	const bool bSclSig = Raw.size() >= 9 && memcmp(Raw.data(), "SINCLAIR", 8) == 0;

	if (bSclSig)
	{
		if (!ConvertSclToTrd(Raw, m_Image.m_Data, OutError))
		{
			return false;
		}
		m_Image.m_Sides = 2;
		m_Image.m_Tracks = 80;
	}
	else if (Raw.size() == kTrdDsBytes)
	{
		m_Image.m_Data = std::move(Raw);
		m_Image.m_Sides = 2;
		m_Image.m_Tracks = 80;
	}
	else if (Raw.size() == kTrdSsBytes)
	{
		m_Image.m_Data = std::move(Raw);
		m_Image.m_Sides = 1;
		m_Image.m_Tracks = 80;
	}
	else if (Raw.size() >= 4096 && (Raw.size() % 256) == 0 && Raw.size() < kTrdDsBytes)
	{
		// A truncated TRD holding only the used tracks (common for small disks,
		// e.g. old demo releases). It is a whole number of 256-byte sectors and
		// includes the track-0 catalogue. Pad it to a full double-sided image so
		// the standard DS/80 track->offset mapping still resolves every sector.
		m_Image.m_Data = std::move(Raw);
		m_Image.m_Data.resize(kTrdDsBytes, 0);
		m_Image.m_Sides = 2;
		m_Image.m_Tracks = 80;
	}
	else
	{
		char Buf[160];
		snprintf(Buf, sizeof(Buf),
			"unrecognised disk image: %zu bytes (expected SCL signature or "
			"655360 / 327680 byte TRD)",
			Raw.size());
		OutError = Buf;
		return false;
	}

	m_Image.m_bMounted = true;
	Reset();
	return true;
}

bool Trdos::ReadSectorIntoBuffer(int Track1Based, int SideArg, int Sector1Based)
{
	if (!m_Image.m_bMounted)
	{
		return false;
	}
	if (Track1Based < 0 || Track1Based >= m_Image.m_Tracks)
	{
		return false;
	}
	if (SideArg < 0 || SideArg >= m_Image.m_Sides)
	{
		return false;
	}
	if (Sector1Based < 1 || Sector1Based > kSectorsPerTrack)
	{
		return false;
	}

	const size_t Off = TrdSectorOffset(Track1Based, SideArg, Sector1Based, m_Image.m_Sides);
	if (Off + kSectorSize > m_Image.m_Data.size())
	{
		return false;
	}
	memcpy(m_Buffer, m_Image.m_Data.data() + Off, kSectorSize);
	m_BufferPos = 0;
	m_BufferLen = int(kSectorSize);
	return true;
}

void Trdos::CommandWrite(uint8_t Cmd)
{
	m_Command = Cmd;
	const uint8_t Hi4 = Cmd & 0xF0;

	// Force Interrupt (Type IV) can fire any time, including over a Type II.
	if ((Cmd & 0xF0) == 0xD0)
	{
		m_LastClass = CommandClass::kCommandClassTypeIV;
		m_bBusy = false;
		m_bDrq = false;
		m_BufferPos = m_BufferLen = 0;
		// Bits 0..3 select the interrupt source; bit 3 = immediate IRQ.
		m_bIrq = (Cmd & 0x0F) != 0;
		m_Status = m_Image.m_bMounted ? kFdcHeadloaded : kFdcNotready;
		if (m_HeadTrack == 0 && m_Image.m_bMounted)
		{
			m_Status |= kFdcTrack0;
		}
		return;
	}

	// Drive-not-ready check for all other commands.
	if (!m_Image.m_bMounted)
	{
		m_LastClass = (Hi4 == 0x80 || Hi4 == 0x90 || Hi4 == 0xA0 || Hi4 == 0xB0) ? CommandClass::kCommandClassTypeII
					  : (Hi4 == 0xC0 || Hi4 == 0xE0 || Hi4 == 0xF0)				 ? CommandClass::kCommandClassTypeIII
																				 : CommandClass::kCommandClassTypeI;
		m_Status = kFdcNotready;
		m_bBusy = false;
		m_bDrq = false;
		m_bIrq = true; // signal command completion so TR-DOS can poll and proceed
		return;
	}

	if (Hi4 == 0x00) // Restore
	{
		m_LastClass = CommandClass::kCommandClassTypeI;
		m_HeadTrack = 0;
		m_Track = 0;
		m_Status = kFdcHeadloaded | kFdcTrack0;
		m_bBusy = false;
		m_bIrq = true;
		return;
	}
	if (Hi4 == 0x10) // Seek
	{
		m_LastClass = CommandClass::kCommandClassTypeI;
		// Real WD1793 seeks to the value in the Data register; we just go there.
		m_HeadTrack = m_Data;
		m_Track = m_Data;
		m_Status = kFdcHeadloaded;
		if (m_HeadTrack == 0)
		{
			m_Status |= kFdcTrack0;
		}
		m_bBusy = false;
		m_bIrq = true;
		return;
	}
	if (Hi4 == 0x20 || Hi4 == 0x30) // Step (with/without Update)
	{
		m_LastClass = CommandClass::kCommandClassTypeI;
		// Direction = last (preserved across calls). Simplification: no-op.
		m_Status = kFdcHeadloaded;
		if (m_HeadTrack == 0)
		{
			m_Status |= kFdcTrack0;
		}
		m_bBusy = false;
		m_bIrq = true;
		return;
	}
	if (Hi4 == 0x40 || Hi4 == 0x50) // Step-in (with/without Update)
	{
		m_LastClass = CommandClass::kCommandClassTypeI;
		if (m_HeadTrack < uint8_t(m_Image.m_Tracks - 1))
		{
			++m_HeadTrack;
		}
		if ((Cmd & 0x10) != 0)
		{
			m_Track = m_HeadTrack;
		}
		m_Status = kFdcHeadloaded;
		if (m_HeadTrack == 0)
		{
			m_Status |= kFdcTrack0;
		}
		m_bBusy = false;
		m_bIrq = true;
		return;
	}
	if (Hi4 == 0x60 || Hi4 == 0x70) // Step-out (with/without Update)
	{
		m_LastClass = CommandClass::kCommandClassTypeI;
		if (m_HeadTrack > 0)
		{
			--m_HeadTrack;
		}
		if ((Cmd & 0x10) != 0)
		{
			m_Track = m_HeadTrack;
		}
		m_Status = kFdcHeadloaded;
		if (m_HeadTrack == 0)
		{
			m_Status |= kFdcTrack0;
		}
		m_bBusy = false;
		m_bIrq = true;
		return;
	}
	if (Hi4 == 0x80 || Hi4 == 0x90) // Read Sector
	{
		m_LastClass = CommandClass::kCommandClassTypeII;
		// Side is selected by the System port ($FF bit 3) and possibly by bit 3
		// of the command (compare-side). We trust the System port.
		const int Sec = int(m_Sector);
		if (ReadSectorIntoBuffer(int(m_HeadTrack), int(m_Side), Sec))
		{
			m_bBusy = true;
			m_bDrq = true;
			m_bIrq = false;
			m_Status = kFdcBusy | kFdcDrq;
		}
		else
		{
			m_bBusy = false;
			m_bDrq = false;
			m_bIrq = true;
			m_Status = kFdcRecnotfound;
		}
		return;
	}
	if (Hi4 == 0xA0 || Hi4 == 0xB0) // Write Sector - read-only phase, refuse
	{
		m_LastClass = CommandClass::kCommandClassTypeII;
		m_bBusy = false;
		m_bDrq = false;
		m_bIrq = true;
		m_Status = kFdcWriteprot;
		return;
	}
	if (Hi4 == 0xC0) // Read Address - return the next sector's ID field.
	{
		// TR-DOS issues this to confirm a seek landed on the right track: it
		// reads 6 bytes (track, side, sector, length-code, CRC-hi, CRC-lo) and
		// checks the track. We synthesise an ID for the current head position,
		// rotating through the sectors. Per the WD179x spec the track byte is
		// also copied into the Sector register.
		m_LastClass = CommandClass::kCommandClassTypeIII;
		m_Buffer[0] = m_HeadTrack;
		m_Buffer[1] = uint8_t(m_Side);
		m_Buffer[2] = m_ReadAddrSector;
		m_Buffer[3] = 1; // 256-byte sectors
		m_Buffer[4] = 0; // CRC (TR-DOS does not verify it here)
		m_Buffer[5] = 0;
		m_BufferPos = 0;
		m_BufferLen = 6;
		m_Sector = m_HeadTrack; // WD179x: track addr -> Sector register
		m_ReadAddrSector = uint8_t((m_ReadAddrSector % kSectorsPerTrack) + 1);
		m_bBusy = true;
		m_bDrq = true;
		m_bIrq = false;
		m_Status = kFdcBusy | kFdcDrq;
		return;
	}
	// Remaining Type III (Read Track, Write Track): unimplemented, refuse.
	m_LastClass = CommandClass::kCommandClassTypeIII;
	m_bBusy = false;
	m_bDrq = false;
	m_bIrq = true;
	m_Status = kFdcRecnotfound;
}

bool Trdos::MaybeWritePort(uint16_t Port, uint8_t Value)
{
	const uint8_t Lo = uint8_t(Port & 0xFF);
	switch (Lo)
	{
	case 0x1F:
		CommandWrite(Value);
		return true;
	case 0x3F:
		m_Track = Value;
		return true;
	case 0x5F:
		m_Sector = Value;
		return true;
	case 0x7F:
		m_Data = Value;
		return true; // ignored in Phase 1 read-only
	case 0xFF:
		m_System = Value;
		// Beta-128 system register bit 4 = side select, INVERTED at the drive
		// (bit4=1 -> side 0, bit4=0 -> side 1). TR-DOS reads the side-0
		// catalogue with bit4 set, then side-1 file tracks with it clear.
		m_Side = (Value & 0x10) ? 0 : 1;
		// bit 3 reset (active low): also flush state
		if ((Value & 0x04) == 0)
		{
			Reset();
		}
		return true;
	default:
		return false;
	}
}

bool Trdos::MaybeReadPort(uint16_t Port, uint8_t& OutValue)
{
	const uint8_t Lo = uint8_t(Port & 0xFF);
	switch (Lo)
	{
	case 0x1F:
		OutValue = m_Status;
		// Type I index-hole pulse: while a disk is loaded and the FDC is idle
		// after a Type I command, toggle the INDEX bit so TR-DOS's "disk
		// present?" poll sees the disk spinning (else RESTORE -> "No disk").
		if (m_Image.m_bMounted && m_LastClass == CommandClass::kCommandClassTypeI && !m_bBusy)
		{
			if (((++m_IndexPulseCounter) & 0x08) != 0)
			{
				OutValue |= kFdcIndex;
			}
		}
		return true;
	case 0x3F:
		OutValue = m_Track;
		return true;
	case 0x5F:
		OutValue = m_Sector;
		return true;
	case 0x7F:
		if (m_bBusy && m_BufferPos < m_BufferLen)
		{
			OutValue = m_Buffer[m_BufferPos++];
			if (m_BufferPos >= m_BufferLen)
			{
				// Sector done; raise IRQ, drop BUSY/DRQ.
				m_bBusy = false;
				m_bDrq = false;
				m_bIrq = true;
				m_Status = m_Image.m_bMounted ? 0 : kFdcNotready;
			}
		}
		else
		{
			OutValue = 0xFF;
		}
		return true;
	case 0xFF:
	{
		uint8_t V = 0x3F; // unused bits typically read as 1
		if (m_bIrq)
		{
			V |= 0x80; // INTRQ on bit 7
		}
		if (m_bDrq)
		{
			V |= 0x40; // DRQ on bit 6
		}
		OutValue = V;
		return true;
	}
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------
// .SCL -> TRD conversion
// ---------------------------------------------------------------------------

bool ConvertSclToTrd(const std::vector<uint8_t>& Scl, std::vector<uint8_t>& OutTrd, std::string& OutError)
{
	constexpr size_t kHeaderSize = 9;
	constexpr size_t kDescSize = 14;

	if (Scl.size() < kHeaderSize)
	{
		OutError = "SCL: file too short";
		return false;
	}
	if (memcmp(Scl.data(), "SINCLAIR", 8) != 0)
	{
		OutError = "SCL: bad signature";
		return false;
	}
	const int NumFiles = int(Scl[8]);
	if (NumFiles < 0 || NumFiles > 128)
	{
		OutError = "SCL: invalid file count";
		return false;
	}

	// Validate that the descriptor table fits.
	const size_t DescTableEnd = kHeaderSize + size_t(NumFiles) * kDescSize;
	if (Scl.size() < DescTableEnd)
	{
		OutError = "SCL: descriptors truncated";
		return false;
	}

	// Sum file lengths and validate data fits in SCL plus the disk.
	uint32_t TotalSectors = 0;
	for (int IdxI = 0; IdxI < NumFiles; ++IdxI)
	{
		const uint8_t Len = Scl[kHeaderSize + IdxI * kDescSize + 13];
		TotalSectors += Len;
	}
	const size_t DataExpected = size_t(TotalSectors) * kSectorSize;
	if (Scl.size() < DescTableEnd + DataExpected)
	{
		OutError = "SCL: file data truncated";
		return false;
	}
	// Disk capacity: 80 tracks * 2 sides * 16 sectors = 2560 sectors total;
	// first 16 (track 0 side 0) are reserved for catalog + info. Track 1 onward
	// (logical track 1 = cyl 0 side 1) is free -> 2544 data sectors max.
	if (TotalSectors > 2544)
	{
		OutError = "SCL: file payload exceeds 2544-sector disk capacity";
		return false;
	}

	OutTrd.assign(kTrdDsBytes, 0);

	// Place files starting at logical track 1, sector 1 (absolute sector 16).
	uint32_t NextAbsSector = kSectorsPerTrack; // 16
	size_t SclCursor = DescTableEnd;

	for (int IdxI = 0; IdxI < NumFiles; ++IdxI)
	{
		const uint8_t* Desc = Scl.data() + kHeaderSize + IdxI * kDescSize;
		const uint8_t Len = Desc[13];

		// Catalog entry: 16 bytes. First 14 = descriptor; byte 14 = start
		// sector within track (0-based), byte 15 = start track (0-based).
		// Catalog occupies track 0 sectors 0..7 (logical, 0-based). Sector
		// (i / 16), entry (i % 16).
		const size_t CatSec = size_t(IdxI) / 16;
		const size_t CatEntry = size_t(IdxI) % 16;
		const size_t CatOffset = CatSec * kSectorSize + CatEntry * 16;

		memcpy(OutTrd.data() + CatOffset, Desc, 14);
		OutTrd[CatOffset + 14] = uint8_t(NextAbsSector % kSectorsPerTrack);
		OutTrd[CatOffset + 15] = uint8_t(NextAbsSector / kSectorsPerTrack);

		// Copy file data into the TRD at the absolute sector.
		const size_t FileBytes = size_t(Len) * kSectorSize;
		const size_t DstOffset = size_t(NextAbsSector) * kSectorSize;
		if (FileBytes > 0)
		{
			memcpy(OutTrd.data() + DstOffset, Scl.data() + SclCursor, FileBytes);
		}
		SclCursor += FileBytes;
		NextAbsSector += Len;
	}

	// Disk info sector: logical track 0 sector 8 (1-based) -> absolute sector 8.
	constexpr size_t kInfoOffset = 8 * kSectorSize;
	const uint32_t FreeSectors = 2544 - TotalSectors;
	OutTrd[kInfoOffset + 225] = uint8_t(NextAbsSector % kSectorsPerTrack); // first free sector (0-based)
	OutTrd[kInfoOffset + 226] = uint8_t(NextAbsSector / kSectorsPerTrack); // first free track (0-based)
	OutTrd[kInfoOffset + 227] = 0x16; // disk type: 80T DS DD
	OutTrd[kInfoOffset + 228] = uint8_t(NumFiles);
	OutTrd[kInfoOffset + 229] = uint8_t(FreeSectors & 0xFF);
	OutTrd[kInfoOffset + 230] = uint8_t((FreeSectors >> 8) & 0xFF);
	OutTrd[kInfoOffset + 231] = 0x10; // TR-DOS ID
	// 232..234 reserved zeros (already 0)
	for (int B = 234; B <= 244; ++B)
	{
		OutTrd[kInfoOffset + B] = 0x20; // 11 spaces
	}
	// 245..252 volume label (8 bytes spaces by default)
	for (int B = 245; B <= 252; ++B)
	{
		OutTrd[kInfoOffset + B] = 0x20;
	}
	// 253..255 zeros

	return true;
}
