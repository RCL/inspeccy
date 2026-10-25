
#include <trace/trace_binary.h>

#include <array>
#include <string.h>

namespace BinaryTrace
{

namespace
{

// Precomputed CRC32 table (polynomial 0xEDB88320 - zlib/IEEE 802.3 standard).
// Static-init runs once; the alternative is constexpr-init which trades binary
// size for startup cost. ~1 KB of rodata is cheaper than a constexpr build
// dependency when the table is touched on every header write.
struct Crc32Table
{
	std::array<uint32_t, 256> m_T;
	Crc32Table()
	{
		for (uint32_t IdxI = 0; IdxI < 256; ++IdxI)
		{
			uint32_t C = IdxI;
			for (int K = 0; K < 8; ++K)
			{
				C = (C & 1u) ? (0xEDB88320u ^ (C >> 1)) : (C >> 1);
			}
			m_T[IdxI] = C;
		}
	}
};

const Crc32Table kCrcTable;

} // namespace

uint32_t Crc32(const void* Data, size_t Length, uint32_t Seed)
{
	uint32_t C = Seed ^ 0xFFFFFFFFu;
	const uint8_t* P = static_cast<const uint8_t*>(Data);
	for (size_t IdxI = 0; IdxI < Length; ++IdxI)
	{
		C = kCrcTable.m_T[(C ^ P[IdxI]) & 0xFFu] ^ (C >> 8);
	}
	return C ^ 0xFFFFFFFFu;
}

} // namespace BinaryTrace
