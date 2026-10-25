#include <asm/index.h>

#include "json_parser.h"

#include <stdio.h>
#include <string.h>
#include <string>

// The assembly index, the layout table and the MMU lint, against a fixture
// small enough to reason about by hand: one macro, one local label, one
// backward branch, one indirect access and two pages named by symbol.
//
// The lint's value is entirely in what it refuses to conclude, so the cases
// below pin the unverified notes just as firmly as the errors.

namespace
{
int GFailures = 0;

void Check(bool bSuccess, const char* Description)
{
	if (!bSuccess)
	{
		fprintf(stderr, "FAIL: %s\n", Description);
		++GFailures;
	}
}

void Put(const char* Path, const char* Text)
{
	FILE* F = fopen(Path, "wb");
	if (F == nullptr)
	{
		fprintf(stderr, "FAIL: cannot write %s\n", Path);
		++GFailures;
		return;
	}
	fwrite(Text, 1, strlen(Text), F);
	fclose(F);
}

// Addresses below are hand-assigned to match the instruction lengths in
// asmfix.asm; nothing assembles the fixture, so they only need to agree with
// each other.
const char* kSource = "; fixture for the assembly index\n"
					  "    MACRO PAGEIN slot, page\n"
					  "        nextreg slot, page\n"
					  "    ENDM\n"
					  "Start:\n"
					  "    ld a, (Table)          ; a trailing comment\n"
					  "    PAGEIN $57, PAGE_A\n"
					  "    call Helper\n"
					  "    ld b, (hl)\n"
					  "    jr .Again\n"
					  ".Again:\n"
					  "    ret\n"
					  "Helper:\n"
					  "    nextreg $57, PAGE_B\n"
					  "    ret\n"
					  "Table:\n"
					  "    db 0\n";

const char* kSld = "|SLD.data.version|1\n"
				   "asmfix.asm|1||0|-1|-1|Z|pages.size:8192,pages.count:224,slots.count:8\n"
				   "asmfix.asm|1||0|-1|33|D|PAGE_A\n"
				   "asmfix.asm|1||0|-1|40|D|PAGE_B\n"
				   "asmfix.asm|5||0|4|32768|F|Start\n"
				   "asmfix.asm|11||0|4|32781|F|Start.Again\n"
				   "asmfix.asm|13||0|4|32782|F|Helper\n"
				   "asmfix.asm|16||0|4|32787|F|Table\n"
				   "asmfix.asm|6||0|4|32768|T|\n"
				   "asmfix.asm|7|asmfix.asm|3|4|32771|T|\n"
				   "asmfix.asm|8||0|4|32775|T|\n"
				   "asmfix.asm|9||0|4|32778|T|\n"
				   "asmfix.asm|10||0|4|32779|T|\n"
				   "asmfix.asm|12||0|4|32781|T|\n"
				   "asmfix.asm|14||0|4|32782|T|\n"
				   "asmfix.asm|15||0|4|32786|T|\n";

bool Banks(const AsmIndex& Index, const char* Json, AsmBankReport& Out, std::string& OutError)
{
	JsonValue Config;
	if (!ParseJson(Json, Config, OutError))
	{
		return false;
	}
	return AsmCheckBanks(Index, Config, Out, OutError);
}

// Whether any note in Bucket mentions Fragment, so a wording tweak does not
// need every case rewritten but the substance stays pinned.
bool Mentions(const std::vector<AsmBankNote>& Bucket, const char* Fragment)
{
	for (const AsmBankNote& N : Bucket)
	{
		if (N.Message.find(Fragment) != std::string::npos)
		{
			return true;
		}
	}
	return false;
}
} // namespace

int main()
{
	Put("asmfix.asm", kSource);
	Put("asmfix.sld", kSld);

	AsmIndex Index;
	std::string Error;
	if (!Index.Load("asmfix.sld", ".", Error))
	{
		fprintf(stderr, "FAIL: load: %s\n", Error.c_str());
		return 1;
	}

	// ------------------------------------------------------------ symbols
	const AsmIndex::Record* Start = Index.Symbol("Start");
	Check(Start != nullptr && Start->Page == 4 && Start->Address == 0x8000, "symbol page/address");
	Check(Index.Symbol("PAGE_A") != nullptr && Index.Symbol("PAGE_A")->Page == -1,
		"a define is unbanked");
	Check(Index.Symbol("Nowhere") == nullptr, "unknown symbol");

	long V = 0;
	Check(Index.Evaluate("Table", V, Error) && V == 0x8013, "bare symbol");
	Check(Index.Evaluate("Table-$10", V, Error) && V == 0x8003, "symbol minus hex");
	Check(Index.Evaluate("PAGE_A*2+1", V, Error) && V == 67, "define arithmetic");
	Check(Index.Evaluate("@Helper", V, Error) && V == 0x800E, "leading @ is ignored");
	Check(!Index.Evaluate("Nowhere+1", V, Error), "unknown name in an expression");
	Check(!Index.Evaluate("Table/2", V, Error), "division is refused, not guessed");

	Check(Index.Owner(4, 0x8004) == "Start", "owner is the nearest preceding label");
	Check(Index.Owner(4, 0x8010) == "Helper", "owner after the next label");
	Check(Index.ResolveRef(".Again", "Start") == "Start.Again", "local reference resolves in scope");
	Check(Index.ResolveRef(".Again", "Helper").empty(), "local reference out of scope");

	// ------------------------------------------- source text and macros
	const std::vector<const AsmIndex::Record*> Body = Index.Body("Start");
	Check(Body.size() == 6, "body ends at the next top-level label, locals included");
	if (Body.size() == 6)
	{
		Check(Body[0]->Text == "ld a, (Table)", "comment and label stripped");
		Check(Body[1]->Text == "nextreg $57, PAGE_A", "macro arguments substituted");
		Check(Body[5]->Address == 0x800D, "last instruction of the body");
	}
	Check(Index.Body("Start", 0x8003).size() == 1, "an explicit end bounds the body");

	const std::vector<AsmIndex::Edge>& Edges = Index.Edges();
	Check(Edges.size() == 2, "two branches in the fixture");
	if (Edges.size() == 2)
	{
		Check(Edges[0].Operation == "call" && Edges[0].Target == "Helper" && Edges[0].TargetPage == 4,
			"call edge");
		Check(Edges[1].Operation == "jr" && Edges[1].Target == "Start.Again",
			"local branch edge");
	}

	// ------------------------------------------------------------- layout
	{
		JsonValue Config;
		Check(ParseJson(R"({"version":1,"regions":[
				{"name":"Code","page":4,"start":"Start","end":"Table","limit":"Start+64"}]})",
				  Config, Error),
			"layout config parses");
		std::vector<AsmLayoutRow> Rows;
		Check(AsmLayout(Index, Config, Rows, Error) && Rows.size() == 1, "layout accepts a fitting region");
		if (Rows.size() == 1)
		{
			Check(Rows[0].Used == 0x13 && Rows[0].Free == 64 - 0x13, "used and free");
		}
	}
	{
		JsonValue Config;
		ParseJson(R"({"version":1,"regions":[
				{"name":"Code","page":4,"start":"Start","end":"Table","limit":"Start+8"}]})",
			Config, Error);
		std::vector<AsmLayoutRow> Rows;
		Check(!AsmLayout(Index, Config, Rows, Error), "layout rejects an over-limit region");
	}

	// -------------------------------------------------------------- banks
	{
		AsmBankReport Rep;
		Check(Banks(Index,
				  R"({"version":1,"routines":{
						"Start":{"end":"Helper"},
						"Helper":{"end":"Table"}}})",
				  Rep, Error),
			"clean lint runs");
		Check(Rep.Checked.size() == 2 && Rep.Errors.empty(), "nothing is wrong with the fixture");
		Check(Rep.Unverified.size() == 1 && Mentions(Rep.Unverified, "indirect access: (hl)"),
			"the one thing it cannot follow is the (hl) read");
		Check(Rep.DirectSites == 2 && Rep.CrossPageSites == 0 && Rep.AnnotatedRoutines == 2,
			"inventory");
	}
	{
		// Helper now demands a slot 7 that Start has just pointed elsewhere.
		AsmBankReport Rep;
		Banks(Index,
			R"({"version":1,"routines":{
					"Start":{"end":"Helper","returns":{"7":"entry"}},
					"Helper":{"end":"Table","requires":{"7":40}}}})",
			Rep, Error);
		Check(Mentions(Rep.Errors, "Helper precondition: slot 7 maps 33, needs 40"),
			"a violated precondition is an error");
		Check(Mentions(Rep.Unverified, "cannot prove restoration of dynamic slot 7"),
			"a return to an unknown entry mapping is unverified, not assumed");
	}
	{
		// An unlisted callee widens the state instead of being trusted.
		AsmBankReport Rep;
		Banks(Index, R"({"version":1,"routines":{"Start":{"end":"Helper"}}})", Rep, Error);
		Check(Rep.ExternalAssumptions.empty() && Rep.Checked.size() == 1, "only Start is checked");
		Check(Mentions(Rep.Unverified, "no mapping contract for Helper"),
			"an undeclared call is reported");
		Check(Mentions(Rep.Unverified, "instruction fetch: slot 4 mapping unknown"),
			"and widens the state rather than carrying it through");
	}
	{
		// Declaring it external states an assumption; the lint says so and
		// keeps the caller's state.
		AsmBankReport Rep;
		Banks(Index,
			R"({"version":1,"routines":{
					"Start":{"end":"Helper"},
					"Helper":{"external":true,"returns":{"4":4}}}})",
			Rep, Error);
		Check(Rep.ExternalAssumptions.size() == 1 && Rep.ExternalAssumptions[0] == "Helper",
			"external contracts are listed as assumptions");
		Check(!Mentions(Rep.Unverified, "instruction fetch: slot 4 mapping unknown"),
			"a declared return keeps the caller verifiable");
	}
	{
		AsmBankReport Rep;
		Check(!Banks(Index, R"({"version":1,"routines":{"Start":{}}})", Rep, Error),
			"a checked routine without an end is refused");
	}

	remove("asmfix.asm");
	remove("asmfix.sld");
	if (GFailures != 0)
	{
		fprintf(stderr, "%d check(s) failed\n", GFailures);
		return 1;
	}
	printf("all assembly index checks passed\n");
	return 0;
}
