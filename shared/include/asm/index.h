#pragma once
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

// A bounded index of one sjasmplus build: its symbols, its instructions and
// the source text behind them. Where SldSymbolMap answers "what label is at
// this address" for the trace, this answers the questions static analysis
// asks - where does a routine end, what does it call, what does this
// expression evaluate to, does every page still fit.
//
// It keeps the SLD's `T` (trace/instruction) records and the file/line pairs
// that SldSymbolMap discards, and it reads the .asm files themselves to
// recover each instruction's text, including substituting a MACRO's arguments
// at the invocation site. Nothing here needs an assembler or an emulator.
//
// Page numbering follows the same convention as SldSymbolMap: physical 8 KB
// pages, with a 16 KB-bank device's bank numbers expanded on load. A page of
// -1 means the symbol is not banked.
struct AsmIndex final
{
  public:
	struct Record
	{
		int Page = -1;
		uint32_t Address = 0;
		std::string File; // where it was assembled
		int Line = 0;
		std::string DefFile; // where it was written, if that differs (a macro body)
		int DefLine = 0;
		std::string Text;  // instructions: the source line, comments and label removed
		std::string Owner; // instructions: the top-level label it belongs to
	};

	struct Edge
	{
		int Page = -1;
		uint32_t Address = 0;
		std::string File;
		int Line = 0;
		std::string Owner;
		std::string Operation; // call, jp, jr or djnz
		std::string Operand;   // as written
		std::string Target;    // resolved symbol, empty when it could not be
		int TargetPage = -1;   // -1 when unresolved or unbanked
		bool bResolved = false;
	};

	// Reads the SLD and every source file it names. SourceRoot may be empty,
	// in which case the SLD's own directory is used. Returns false with a
	// diagnostic; a source file that cannot be read is not fatal (the
	// instruction simply has no text, which the bank lint reports as
	// unverified rather than safe).
	bool Load(const std::string& SldPath, const std::string& SourceRoot, std::string& OutError);

	// Null when the name is unknown.
	const Record* Symbol(const std::string& Name) const;

	// `$hex`, decimal, symbol names (dotted locals and a leading @ included),
	// and + - * between them. Deliberately no division, shifts or parentheses:
	// the configurations this reads have never needed them, and refusing is
	// safer than half-supporting an expression language.
	bool Evaluate(const std::string& Expression, long& OutValue, std::string& OutError) const;

	// The nearest preceding label on that page, or empty.
	std::string Owner(int Page, uint32_t Address) const;

	// A reference as written ("Loop", ".Next", "@Thing") to a symbol name,
	// resolving a leading dot against the owning label's scope. Empty when it
	// does not name a known symbol.
	std::string ResolveRef(const std::string& Text, const std::string& OwnerName) const;

	// Every call/jp/jr/djnz found in the instruction stream. Built on first
	// use and cached.
	const std::vector<Edge>& Edges() const;

	const std::vector<Record>& Instructions() const
	{
		return m_Instructions;
	}
	const std::map<std::string, Record>& Symbols() const
	{
		return m_Symbols;
	}

	// Instructions of the body that starts at Name, ending at the next
	// top-level label on the same page (a local label of Name's own scope
	// belongs to the body) or at End when one is given.
	std::vector<const Record*> Body(const std::string& Name, long End = -1) const;

  private:
	std::string SourceText(const Record& Rec) const;
	const std::vector<std::string>* Lines(const std::string& File) const;

	std::map<std::string, Record> m_Symbols;
	std::vector<Record> m_Instructions;
	// Per page, (address, name) sorted, for the nearest-preceding lookup.
	std::map<int, std::vector<std::pair<uint32_t, std::string>>> m_Labels;
	std::string m_Root;
	mutable std::map<std::string, std::vector<std::string>> m_Files;
	mutable std::vector<Edge> m_Edges;
	mutable bool m_bEdgesBuilt = false;
};

// A configuration value that may be written as a number or as an expression
// over the build's symbols. Shared by the layout table and the bank lint so
// both accept the same notation.
bool AsmEvalValue(const AsmIndex& Index, const struct JsonValue& V, long& Out, std::string& OutError);

// ---------------------------------------------------------------- layout
// One declared region of the image, and whether it still fits.
struct AsmLayoutRow
{
	std::string Name;
	long Page = 0, Start = 0, End = 0, Limit = 0, Used = 0, Free = 0;
};

// Reads the `regions` array of a build configuration. Every field may be a
// number or an expression over the build's symbols. Returns false with a
// diagnostic if a region is inverted or over its limit.
bool AsmLayout(const AsmIndex& Index, const struct JsonValue& Config, std::vector<AsmLayoutRow>& Out,
	std::string& OutError);

// ----------------------------------------------------------------- banks
// A conservative MMU lint over explicitly bounded routines. External
// contracts are assumptions, not proofs: unknown calls, port-driven mapping
// changes and indirect references are reported, never treated as safe.
struct AsmBankNote
{
	std::string Routine, File, Message;
	uint32_t Address = 0;
	int Line = 0;
};

struct AsmBankReport
{
	std::vector<std::string> Checked, ExternalAssumptions;
	std::vector<AsmBankNote> Errors, Unverified;
	size_t OmittedUnverified = 0;
	size_t DirectSites = 0, CrossPageSites = 0, AnnotatedRoutines = 0;
};

bool AsmCheckBanks(const AsmIndex& Index, const struct JsonValue& Config, AsmBankReport& Out,
	std::string& OutError);
