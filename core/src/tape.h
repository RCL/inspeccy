#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct Z80State;
struct MemoryBus;

// .tap tape player with ROM-trap "instant load". A .tap file is a sequence of
// length-prefixed blocks; each block body is [flag] [data...] [checksum], flag
// $00 = header, $FF = data. Instead of modelling tape pulse timing, we trap the
// 48K ROM's LD-BYTES routine ($0556) and satisfy each load from the current
// block directly (see HandleLdBytes). Standard-speed ROM-loader tapes only;
// custom/turbo loaders (which don't call $0556) would need pulse-level tape.
struct TapePlayer
{
  public:
	// Parse a .tap file into blocks. Returns false + sets OutError on a
	// malformed file (bad length prefix / truncated block).
	bool Load(const char* Path, std::string& OutError);

	size_t BlockCount() const
	{
		return m_Blocks.size();
	}
	size_t Cursor() const
	{
		return m_Cursor;
	}
	void SetCursor(size_t C)
	{
		m_Cursor = C;
	}
	bool AtEnd() const
	{
		return m_Cursor >= m_Blocks.size();
	}

	// LD-BYTES ($0556) trap handler. Reads the expected flag from A, the
	// destination from IX and the length from DE, copies the current block's
	// data into memory, advances the block cursor, and simulates the routine's
	// RET (pops the return address into PC) with the carry flag reflecting
	// success (CF=1 loaded, CF=0 error / no block / flag mismatch). Always
	// consumes the PC-return so the trapped call cannot re-enter.
	void HandleLdBytes(Z80State& InOutCpu, MemoryBus& InOutMem);

	uint64_t BytesDelivered() const
	{
		return m_BytesDelivered;
	}
	uint64_t BlocksDelivered() const
	{
		return m_BlocksDelivered;
	}
	// Resume: the counters as a dump recorded them.
	void RestoreCounters(uint64_t BytesDelivered, uint64_t BlocksDelivered)
	{
		m_BytesDelivered = BytesDelivered;
		m_BlocksDelivered = BlocksDelivered;
	}

  private:
	std::vector<std::vector<uint8_t>> m_Blocks; // each = one block body (flag..checksum)
	size_t m_Cursor = 0;
	uint64_t m_BytesDelivered = 0;
	uint64_t m_BlocksDelivered = 0;
};
