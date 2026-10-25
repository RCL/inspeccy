#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

enum TrdosMode
{
	kTrdosModeOff,
	kTrdosModeOn,
	kTrdosModeAuto,
	kTrdosModeSna,
};

struct TrdImage
{
	std::vector<uint8_t> m_Data;
	int m_Sides = 0;
	int m_Tracks = 0;
	bool m_bMounted = false;
};

// Beta Disk Interface FDC (WD1793 / KP1818VG93) + .TRD/.SCL image, Phase 1.
// Read-only; serves a single drive (A:). Auto page-in/out of TR-DOS ROM is
// driven by the dispatcher in z80.cpp; this class only provides:
//   - disk image load (.TRD raw or .SCL converted to in-memory TRD)
//   - WD1793 register state + Type I / Type II Read Sector / Force Interrupt
//   - "drive not ready" handling when no disk is mounted
struct Trdos final
{
  public:
	Trdos();

	// Mounts a disk image at Path. Format auto-detected by signature:
	//   "SINCLAIR" -> SCL -> converted to in-memory TRD.
	//   655360 / 327680 bytes -> raw TRD.
	bool MountDisk(const char* Path, std::string& OutError);

	bool IsDiskMounted() const
	{
		return m_Image.m_bMounted;
	}

	// Auto page-in/page-out enabled flag. Set by main.cpp from the --trdos
	// flag plus the SNA's TR-DOS trailer state. The dispatcher checks this
	// and skips the page-swap test entirely when false.
	void SetSupportActive(bool bB)
	{
		m_bSupportActive = bB;
	}
	bool IsSupportActive() const
	{
		return m_bSupportActive;
	}

	// FDC port routing. The dispatcher hands every OUT / IN to these helpers
	// when bSupportActive is true; they return true if the port matched an
	// FDC port and was handled, false to fall through to other handlers.
	bool MaybeWritePort(uint16_t Port, uint8_t Value);
	bool MaybeReadPort(uint16_t Port, uint8_t& OutValue);

	// Reset FDC state.
	void Reset();

	// Inspection / dump accessors - read-only view of the FDC register file.
	uint8_t GetCommandReg() const
	{
		return m_Command;
	}
	uint8_t GetStatusReg() const
	{
		return m_Status;
	}
	uint8_t GetTrackReg() const
	{
		return m_Track;
	}
	uint8_t GetSectorReg() const
	{
		return m_Sector;
	}
	uint8_t GetDataReg() const
	{
		return m_Data;
	}
	uint8_t GetSystemReg() const
	{
		return m_System;
	}
	uint8_t GetHeadTrack() const
	{
		return m_HeadTrack;
	}
	uint8_t GetSide() const
	{
		return m_Side;
	}
	const uint8_t* GetBuffer() const
	{
		return m_Buffer;
	}
	int GetBufferPos() const
	{
		return m_BufferPos;
	}
	int GetBufferLen() const
	{
		return m_BufferLen;
	}
	bool IsBusy() const
	{
		return m_bBusy;
	}
	bool HasIrq() const
	{
		return m_bIrq;
	}
	bool HasDrq() const
	{
		return m_bDrq;
	}
	const char* GetLastClassName() const
	{
		switch (m_LastClass)
		{
		case CommandClass::kCommandClassTypeI:
			return "TypeI";
		case CommandClass::kCommandClassTypeII:
			return "TypeII";
		case CommandClass::kCommandClassTypeIII:
			return "TypeIII";
		case CommandClass::kCommandClassTypeIV:
			return "TypeIV";
		}
		return "?";
	}

	// Resume support: the disk's rotation phase (the index pulse a status
	// read reports) and the sector the next Read Address returns.
	uint32_t GetIndexPulseCounter() const
	{
		return m_IndexPulseCounter;
	}
	uint8_t GetReadAddrSector() const
	{
		return m_ReadAddrSector;
	}
	void RestorePhase(uint32_t IndexPulseCounter, uint8_t ReadAddrSector)
	{
		m_IndexPulseCounter = IndexPulseCounter;
		m_ReadAddrSector = ReadAddrSector;
	}

	// Resume support - restore the FDC register file + buffer.
	void RestoreFromDump(uint8_t Cmd, uint8_t Stat, uint8_t Trk, uint8_t Sec, uint8_t Dat, uint8_t Sys, uint8_t HeadTrk,
		uint8_t Sd, const uint8_t* Buf, int BufPos, int BufLen, bool bBusyIn, bool bIrqIn, bool bDrqIn,
		const std::string& LastClassName)
	{
		m_Command = Cmd;
		m_Status = Stat;
		m_Track = Trk;
		m_Sector = Sec;
		m_Data = Dat;
		m_System = Sys;
		m_HeadTrack = HeadTrk;
		m_Side = Sd;
		if (Buf)
		{
			for (int IdxI = 0; IdxI < 256; ++IdxI)
			{
				m_Buffer[IdxI] = Buf[IdxI];
			}
		}
		m_BufferPos = BufPos;
		m_BufferLen = BufLen;
		m_bBusy = bBusyIn;
		m_bIrq = bIrqIn;
		m_bDrq = bDrqIn;
		if (LastClassName == "TypeI")
		{
			m_LastClass = CommandClass::kCommandClassTypeI;
		}
		else if (LastClassName == "TypeII")
		{
			m_LastClass = CommandClass::kCommandClassTypeII;
		}
		else if (LastClassName == "TypeIII")
		{
			m_LastClass = CommandClass::kCommandClassTypeIII;
		}
		else
		{
			m_LastClass = CommandClass::kCommandClassTypeIV;
		}
	}

  private:
	void CommandWrite(uint8_t Cmd);
	bool ReadSectorIntoBuffer(int Track1Based, int Side, int Sector1Based);

	bool m_bSupportActive = false;
	TrdImage m_Image;

	// FDC register file.
	uint8_t m_Command = 0;
	uint8_t m_Status = 0;
	uint8_t m_Track = 0;
	uint8_t m_Sector = 0;
	uint8_t m_Data = 0;
	uint8_t m_System = 0;

	// Internal head position (head can step independently of Track register
	// during Type I motion commands; for our instant model they stay aligned).
	uint8_t m_HeadTrack = 0;
	uint8_t m_Side = 0;

	// Sector data buffer for Type II Read Sector.
	uint8_t m_Buffer[256] = {};
	int m_BufferPos = 0;
	int m_BufferLen = 0;

	bool m_bBusy = false;
	bool m_bIrq = false;
	bool m_bDrq = false;

	enum CommandClass
	{
		kCommandClassTypeI,
		kCommandClassTypeII,
		kCommandClassTypeIII,
		kCommandClassTypeIV
	};
	CommandClass m_LastClass = CommandClass::kCommandClassTypeI;

	// Index-hole pulse emulation. A real drive pulses the Type I status INDEX bit
	// once per revolution while a disk spins; TR-DOS polls it to confirm the disk
	// is present. Advanced on each status read (no cycle clock needed here).
	uint32_t m_IndexPulseCounter = 0;

	// Rotating sector number returned by successive Read Address commands.
	uint8_t m_ReadAddrSector = 1;
};

// Builds a 655,360-byte (80-track DS DD) raw TRD buffer from an in-memory
// .SCL archive. Returns false and sets OutError on a malformed or oversized
// archive. The resulting TRD always has Sides=2, Tracks=80.
bool ConvertSclToTrd(const std::vector<uint8_t>& Scl, std::vector<uint8_t>& OutTrd, std::string& OutError);
