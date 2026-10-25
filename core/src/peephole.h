#pragma once
#if INSPECCY_TRACE_SUPPORT

#include <array>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TraceFrame;
struct SldSymbolMap;

// Peephole optimisation reporter.
//
// This runs over EXECUTED instructions, which is the point: it sees the actual
// register values, so it can say "this `ld a,7` reloads a register that already
// holds 7" - something no static pass can decide. It also knows how often each
// site ran, so findings rank by what they would actually save rather than by
// how many of them there are.
//
// Two things keep dynamic analysis honest, and both are load-bearing:
//
//  1. ADJACENCY, reported rather than required. The executed stream is not the
//     memory layout: two instructions that ran back-to-back may sit in
//     different routines with a jump between them. That does NOT make the
//     finding worthless - if this is the only path that reaches the second
//     instruction, the work really is unnecessary and the code can be moulded
//     to suit. So pair rules fire either way and each finding says how it was
//     linked: `adj` when the first fell through into the second, `path/N` when
//     it did not and the second instruction had N distinct predecessors across
//     the whole run. `path/1` means "you are the only caller" and is nearly as
//     good as adjacency; a large N is the one to distrust.
//
//  2. CONSISTENCY. A value-based hit seen once may be a coincidence of that
//     particular run. Every finding records how many times its site executed
//     alongside how many times the pattern held, and the report defaults to
//     showing only those that held on EVERY execution. A rule that is true 3
//     times out of 5000 is not an optimisation, it is a fluke.
struct PeepholeAnalyzer
{
  public:
	// Rule identity. Order is the report order; keep it stable.
	enum RuleId : uint8_t
	{
		kRuleIdRuleRedundantLoad = 0, // reg already holds that value
		kRuleIdRuleValueInOtherReg, // the value is live in another register
		kRuleIdRuleCouldBeIncDec, // the value is old +/- 1 or 2
		kRuleIdRuleSplitPairLoad, // ld b,N : ld c,M  ->  ld bc,NM
		kRuleIdRuleRedundantFlagReset, // or a / and a with carry already clear
		kRuleIdRuleCallRet, // call X : ret     ->  jp X
		kRuleIdRuleHalfPairLoad, // ld de,nn -> ld e,n: only one half is new
		kRuleIdRulePairIncNoCarry, // inc de -> inc e when the carry never happens
		kRuleIdRuleCount
	};

	// What a rule is good for. A profile filters the report; several rules win
	// on both axes, which is why this is a mask rather than a choice.
	enum Profile : uint8_t
	{
		kProfileProfSpeed = 1 << 0,
		kProfileProfSize = 1 << 1,
		kProfileProfBoth = kProfileProfSpeed | kProfileProfSize,
	};

	void SetSymbols(const SldSymbolMap* S)
	{
		m_Symbols = S;
	}
	// Only report findings that held on at least this fraction of executions.
	void SetMinRatio(double R)
	{
		m_MinRatio = R;
	}
	// Report only rules matching this profile mask.
	void SetProfileMask(uint8_t M)
	{
		m_ProfileMask = M;
	}

	// Called once per executed instruction, on the trace consumer thread.
	void Feed(const TraceFrame& F);

	// Writes the report. Returns false only on a file error.
	bool WriteReport(const char* Path) const;

	uint64_t InstructionsSeen() const
	{
		return m_NumInstructions;
	}

  private:
	struct Finding
	{
		uint64_t m_Hits = 0; // times the pattern held here
		// SIGNED, because a rewrite can win on one axis and lose on the other:
		// `ld hl,nn` (10T/3B) -> `inc hl` twice (12T/2B) saves a byte and costs
		// two T-states. Unsigned arithmetic here wrapped that -2 into 4.29e9
		// and it dominated every report.
		int32_t m_SaveT = 0; // T-states saved per hit (nominal, uncontended)
		int32_t m_SaveB = 0; // bytes saved
		uint8_t m_Len = 0; // length of the instruction at PC
		// Executions this finding covers. 0 = look it up from the site; a
		// merged unrolled chain sets it explicitly, since its Hits span
		// several sites and dividing by one site's count reads as 400%.
		uint64_t m_Execs = 0;
		uint8_t m_Rule = 0;
		uint8_t m_Page = 0;
		uint16_t PC = 0;
		bool m_bPair = false; // spans two instructions
		bool m_bAllAdj = true; // every hit was a straight fall-through
		uint32_t m_Partner = 0; // site of the second instruction (pair rules)
		std::string m_Text; // "ld a,$07  ->  ld a,b   (b already $07)"
	};

	// One executed instruction, reduced to what the rules need.
	struct Slot
	{
		uint16_t PC = 0, m_PostPC = 0;
		uint8_t m_Page = 0, m_Len = 0;
		uint8_t m_Op[4] = {};
		uint16_t m_PreAF = 0, m_PreBC = 0, m_PreDE = 0, m_PreHL = 0;
		uint16_t m_PostAF = 0, m_PostBC = 0, m_PostDE = 0, m_PostHL = 0;
		bool m_bValid = false;
	};

	// A finding is only recorded if it wins on at least one axis; the profile
	// it belongs to is DERIVED from the signs of SaveT / SaveB rather than
	// declared per rule, so a rule that is a size win and a speed loss can
	// never surface under --optimize-profile speed.
	void Record(RuleId R, const Slot& S, int32_t SaveT, int32_t SaveB, std::string Text, bool bPair = false,
		bool bAdjacent = true, uint32_t Partner = 0);
	static uint8_t ProfileOf(int32_t SaveT, int32_t SaveB)
	{
		uint8_t M = 0;
		if (SaveT > 0)
		{
			M |= kProfileProfSpeed;
		}
		if (SaveB > 0)
		{
			M |= kProfileProfSize;
		}
		return M;
	}
	// True if any byte of the instruction at this site was written at any point
	// in the run -- i.e. the "constant" in it is really a variable. Answered
	// against the whole run, so a poke that happens after the last execution
	// still counts.
	bool SiteWasWritten(uint32_t Site) const;
	// Was byte `Index` of the instruction at this site written? Per byte, not
	// per instruction: knowing WHICH byte of a `ld rr,nn` gets poked is what
	// distinguishes "the whole immediate is a variable" from "only the low half
	// is, so the high half is a constant you are reloading for nothing".
	// `SinceCycle` is the site's first execution: a write BEFORE the code ever
	// ran is loading, not self-modification. Without that cut every byte of a
	// tape- or loader-delivered program counts as self-modified, and every
	// finding in the program under test is struck while the ROM's survive --
	// a report that says "nothing to optimise" about the one thing you asked
	// it to look at.
	bool ByteWritten(uint8_t PageTag, uint16_t PC, int Index, uint64_t SinceCycle) const;
	// Index of the first byte of this site's instruction that was written, or
	// -1. A struck finding has to be able to say WHICH byte condemned it --
	// "your program is clean" and "these were suppressed because $8017 was
	// written" are the same report otherwise.
	int FirstWrittenByte(uint32_t Site) const;
	static uint32_t ByteKey(uint8_t PageTag, uint16_t Addr)
	{
		return (uint32_t(PageTag) << 16) | uint32_t(Addr & 0x1FFF);
	}

	static uint64_t KeyOf(uint8_t Page, uint16_t PC, uint8_t Rule)
	{
		return (uint64_t(Page) << 40) | (uint64_t(PC) << 8) | uint64_t(Rule);
	}
	static uint32_t SiteOf(uint8_t Page, uint16_t PC)
	{
		return (uint32_t(Page) << 16) | uint32_t(PC);
	}

	std::unordered_map<uint64_t, Finding> m_Findings;
	std::unordered_map<uint32_t, uint64_t> m_SiteExecs; // (page,pc) -> executions
	// Opcode of every executed instruction, so a RET can ask what sits three
	// bytes behind it without needing memory access.
	std::unordered_map<uint32_t, uint8_t> m_SiteOpcode;
	// Control-flow edges actually taken, so a pair rule can say whether the
	// second instruction is only ever reached from the first.
	std::unordered_set<uint64_t> m_Edges; // pred site << 32 | site
	std::unordered_map<uint32_t, uint32_t> m_PredCount; // site -> distinct preds
	std::unordered_map<uint32_t, uint8_t> m_SiteLen; // site -> instruction length
	std::unordered_map<uint32_t, uint64_t> m_SiteFirstExec; // site -> first exec cycle

	// Self-modifying-code watch. The classic sjasmplus idiom
	//     .Done equ $+1
	//         ld a,0
	// makes that $00 a variable other code pokes; reporting "a already holds
	// $00, drop it" there would silently break the program.
	//
	// Keyed by PHYSICAL page + offset (24 bits), not by CPU address: $C000 is a
	// different byte in every bank, so an address-only key would strike
	// innocent findings whenever an unrelated bank was written at the same
	// address. The write hook resolves the page where the mapping is known.
	//
	// Whether a site is self-modified is only answerable at the END of the run
	// -- code may be poked long after it last executed -- so findings are
	// collected normally and filtered when the report is written.
	// Byte key -> cycle of the LAST write to it. The cycle is what separates
	// loading from self-modification.
	std::unordered_map<uint32_t, uint64_t> m_WrittenBytes; // pagetag<<16 | offset
	// Offsets written in ANY page. Used only for the rare byte of an
	// instruction that straddles an 8K boundary, whose page is not knowable at
	// report time. Erring towards "written" there is the safe direction: a
	// missed strike is a corrupted program, a spurious one is a lost finding.
	std::unordered_map<uint16_t, uint64_t> m_WrittenOffsetsAnyPage;
	// Last-seen opcode bytes per site, so the report can name the halves of an
	// immediate. For a self-modified site this is whatever was poked in last --
	// which is exactly right for the half that never changes.
	std::unordered_map<uint32_t, std::array<uint8_t, 4>> m_SiteBytes;

	// --- `inc de` -> `inc e` soundness -------------------------------------
	// The 8-bit form is 2T cheaper and does not touch the high byte, so the
	// substitution is wrong exactly where the 16-bit form would have carried.
	//
	// Per-SITE carry detection is not enough, because an unrolled pointer walk
	// spreads one logical increment over many `inc de` instructions. Each may
	// look clean on its own while the walk as a whole crosses a page boundary
	// somewhere in the chain -- and nobody converts one `inc de` out of eight
	// identical lines. So carries poison the whole SERIES.
	//
	// A series is a run of inc/dec on one pair, broken as soon as anything else
	// changes that pair. The break is detected by VALUE (the instruction
	// changed the pair but was not an inc/dec of it) rather than by listing
	// opcodes, so `pop de`, `ld de,nn`, `ex de,hl` and any Z80N oddity all end
	// the series without needing to be enumerated.
	static constexpr int kMaxSeriesSites = 64;
	std::vector<uint32_t> m_SeriesSites[3]; // BC, DE, HL
	bool m_bSeriesPoisoned[3] = {false, false, false};
	std::unordered_set<uint32_t> m_PairCarrySites;

	void ResetSeries(int Pair)
	{
		m_SeriesSites[Pair].clear();
		m_bSeriesPoisoned[Pair] = false;
	}

	Slot m_Prev; // previous executed instruction
	uint64_t m_NumInstructions = 0;

	const SldSymbolMap* m_Symbols = nullptr;
	double m_MinRatio = 1.0;
	uint8_t m_ProfileMask = kProfileProfBoth;
};

#endif // INSPECCY_TRACE_SUPPORT
