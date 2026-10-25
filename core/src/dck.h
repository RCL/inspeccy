#pragma once
// dck.h - Timex cartridge images (Warajevo .DCK).
//
// A .DCK file describes what a TS/TC2068 sees in its DOCK and EXROM banks. It
// is a flat sequence of blocks, each naming one bank and the eight 8K chunks
// that make it up, so a cartridge that only populates chunk 0 costs 8201 bytes
// rather than 64K.
//
// Layout of one block (all of it little-endian / byte-wise, no alignment):
//
//     +0        bank id: 0 = DOCK, 254 = EXROM, 255 = HOME
//     +1 .. +8  one kind byte per 8K chunk, chunk 0 first:
//                   0 = not populated  (reads float, writes are swallowed)
//                   1 = RAM, blank     (present and writable, no bytes in file)
//                   2 = ROM            (8192 bytes follow)
//                   3 = RAM            (8192 bytes follow)
//     +9 ...    8192 bytes for each chunk marked 2 or 3, in chunk order
//
// Blocks repeat until end of file. Chunk N of a bank answers addresses
// N*$2000..N*$2000+$1FFF when SCLD HSR bit N selects it.
//
// This is a from-scratch reader written against that format description; it
// shares no code with any other implementation.

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace Dck
{

constexpr size_t kChunkSize = 8192;
constexpr int kChunksPerBank = 8;

enum Bank : uint8_t
{
	kEBankDock = 0,
	kEBankExrom = 254,
	kEBankHome = 255,
};

enum ChunkKind : uint8_t
{
	kEKindAbsent = 0,
	kEKindRamBlank = 1,
	kEKindRom = 2,
	kEKindRam = 3,
};

struct Chunk
{
	ChunkKind m_Kind = ChunkKind::kEKindAbsent;
	std::vector<uint8_t> m_Data; // CHUNK_SIZE bytes unless Kind is Absent
};

struct Block
{
	Bank m_Bank = Bank::kEBankDock;
	Chunk m_Chunks[kChunksPerBank];
};

struct Cartridge
{
	std::vector<Block> m_Blocks;

	// The last block naming this bank, or nullptr. Later blocks win, which is
	// how a file that redefines a bank is meant to read.
	const Block* FindBank(Bank Bank) const;
};

// Parses an in-memory image. On failure returns false and puts a one-line
// reason in Error. Compressed (gzip/bzip) images are rejected rather than
// silently mis-parsed - callers should decompress first.
bool Parse(const uint8_t* Data, size_t Size, Cartridge& Out, std::string& OutError);

// Reads and parses a file.
bool LoadFile(const char* Path, Cartridge& Out, std::string& OutError);

// "DOCK" / "EXROM" / "HOME" / "bank <n>", for messages.
const char* BankName(Bank Bank);

} // namespace Dck
