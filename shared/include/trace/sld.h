#pragma once
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

// Source-Level Debugging symbol map. Loads sjasmplus .sld.txt files and
// answers (page, address) -> "label" or "label+N" lookups for the trace.
//
// Page-numbering convention: symbols are keyed by PHYSICAL 8 KB page, matching
// the per-instruction page tags the trace carries. sjasmplus states the
// assembling device's geometry in a `Z` record ("pages.size:8192" for
// ZXSPECTRUMNEXT, "pages.size:16384" for ZXSPECTRUM48 / ZXSPECTRUM128); when
// the device uses 16 KB banks, LoadFile expands each bank number B into the
// 8 KB page it really covers - 2B for the bank's low half, 2B+1 for its high
// half, chosen by bit 13 of the label's address.
//
// Formatting/lookups are outside the CPU dispatcher (trace writer or CLI).
struct SldSymbolMap final
{
  public:
	struct Symbol
	{
		int Page = -1;
		uint32_t Value = 0;
	};
	bool Resolve(const std::string& Name, Symbol& Out, std::string& Error) const;
	struct Annotation
	{
		std::string Name;
		uint16_t Address = 0, Offset = 0;
	};
	// Physical-offset lookup also supports a page remapped at another CPU address.
	// Multiple names/assembly windows are preserved, never silently selected.
	std::vector<Annotation> Annotate(uint8_t Page, uint16_t Offset, uint16_t MaxDistance = 256,
		size_t Limit = 8, size_t* Total = nullptr) const;
	SldSymbolMap();

	// Loads one or more SLD files. Each file's parse is reported via stderr:
	// a load summary line, plus warnings for malformed entries / suspicious
	// page numbers / zero parsed labels. Returns true if at least the file
	// was readable; per-line parse warnings don't fail the whole load.
	bool LoadFile(const char* Path, std::string& OutError);

	// Number of label entries successfully parsed across all loaded files.
	size_t SymbolCount() const
	{
		return m_Symbols.size();
	}

	// Number of files loaded.
	size_t FileCount() const
	{
		return m_NumFilesLoaded;
	}

	// Look up the nearest preceding label within MaxOffsetBytes of (Page, Addr).
	// Returns true on hit; OutLabel is filled with "Name" or "Name+N" form.
	// OutLabel is left untouched on miss.
	bool LookupNearest(uint8_t Page8K, uint16_t Addr, int MaxOffsetBytes, std::string& OutLabel) const;

	// Per-line statistics: increments InternalMatches when a lookup succeeded
	// (used for the end-of-run diagnostic).
	void NoteMatch()
	{
		++m_NumMatches;
	}
	void NoteMiss()
	{
		++m_NumMisses;
	}

	uint64_t m_NumMatches = 0;
	uint64_t m_NumMisses = 0;

  private:
	// Key encoding: (Page << 16) | Address.
	static uint32_t MakeKey(uint8_t Page, uint16_t Addr)
	{
		return (uint32_t(Page) << 16) | uint32_t(Addr);
	}

	// Sorted by key for O(log N) nearest-preceding lookup.
	std::map<uint32_t, std::string> m_Symbols;
	std::map<std::string, std::vector<Symbol>> m_ByName;
	std::map<uint32_t, std::vector<Annotation>> m_Physical;

	size_t m_NumFilesLoaded = 0;
	size_t m_NumWarnings = 0;
};
