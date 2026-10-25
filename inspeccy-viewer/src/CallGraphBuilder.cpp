#include "CallGraphBuilder.h"
#include "CpuSources.h"

#include "RecordWalker.h"
#include "TraceModel.h"

#include <trace/trace_binary.h>

namespace
{

// CALL / RST opcode classifiers - used to decide WHEN to open a new frame.
// Return-side detection is no longer opcode-driven: we close frames when the
// runtime SP rises above the frame's recorded post-push SP, which covers
// regular RET, conditional RET taken, RETI, RETN, AND non-standard patterns
// like `pop hl; jp (hl)` and `ld sp, $xxxx` that move SP without a literal RET.
bool IsCallUnconditional(uint8_t B0)
{
	return B0 == 0xCD;
}
bool IsCallConditional(uint8_t B0)
{
	return (B0 & 0xC7) == 0xC4;
} // C4/CC/D4/DC/E4/EC/F4/FC
bool IsRst(uint8_t B0)
{
	return (B0 & 0xC7) == 0xC7;
} // C7/CF/D7/DF/E7/EF/F7/FF

uint16_t RstTarget(uint8_t Opcode)
{
	return uint16_t(Opcode & 0x38);
}

} // namespace

void CallGraphBuilder::Build(const TraceModel& Model)
{
	using namespace BinaryTrace;

	m_Frames.clear();
	m_RomIntervals.clear();

	const Header* H = Model.GetHeader();
	if (H == nullptr || Model.BtrData() == nullptr)
	{
		return;
	}

	const auto& Index = Model.Index();
	if (Index.isEmpty())
	{
		return;
	}

	const uint8_t* Base = Model.BtrData();
	const uint8_t* Cur = Base + Index.first().m_FirstByteOffset;
	const uint8_t* Eof = Base + Model.BtrSize();

	uint64_t RunCycle = Index.first().m_StartCycle;
	uint64_t RunFrame = Index.first().m_FrameNumber;

	// ROM-execution interval tracker. We start in "unknown" - the first
	// instr-like record's PageTag tells us where the CPU actually is. A
	// transition closes the current interval (if any) and starts a new
	// one if we just entered ROM.
	bool bInRom = false;
	uint64_t RomIntervalStart = 0;

	// Initial SP from header. The frame index entry also carries SP, so we
	// could re-anchor at each entry boundary if drift becomes a concern;
	// for v1 the per-instruction Diff_SP carries enough fidelity.
	uint16_t RunSP = H->m_InitSP;

	QVector<int> Stack; // indices into m_Frames for currently-open frames
	Stack.reserve(64);

	// Index of a frame that was just opened and still needs its callee page
	// captured from the next executed record. -1 when nothing is pending.
	int PendingPageFrame = -1;

	auto CloseFramesAboveSP = [&](uint16_t Sp, uint64_t Cycle)
	{
		// Strict greater-than: at the moment of CALL itself runSP == SpAfterPush,
		// and we MUSTN'T close the frame we just opened. Only when SP rises
		// ABOVE the post-push value has the return address been consumed
		// (whether via RET, pop+indirect-jp, or SP reload).
		while (!Stack.isEmpty() && uint16_t(Sp) > m_Frames[Stack.last()].m_SpAfterPush)
		{
			m_Frames[Stack.last()].m_EndCycle = Cycle;
			m_Frames[Stack.last()].m_bComplete = true;
			Stack.pop_back();
		}
	};

	while (Cur < Eof)
	{
		const uint8_t* RecStart = Cur;
		ViewerWalk::DecodedRecord Rec;
		if (!ViewerWalk::DecodeOne(Cur, Eof, RunCycle, RunFrame, Rec))
		{
			break;
		}
		if (ViewerWalk::IsStateRecord(Rec.m_Tag))
		{
			continue; // v4 video event, not an instruction
		}

		if (Rec.m_Tag == kRecord_FrameBoundary)
		{
			continue;
		}

		// The first instr-like / interrupt record after a frame opens executes
		// at that frame's callee entry PC, so its PageTag is the page the
		// callee actually lives in. Capture it for SLD symbol resolution - the
		// CALL record's own PageTag names the caller's slot, which differs for
		// cross-page calls. Runs before anything else so a chain of calls
		// (callee's first instruction is itself a CALL) resolves correctly.
		if (PendingPageFrame >= 0)
		{
			m_Frames[PendingPageFrame].m_TargetPageTag = Rec.m_PageTag;
			PendingPageFrame = -1;
		}

		// Track ROM/RAM transitions from the record's page tag. Covers
		// entry via JP / JR / RET - the call-graph alone misses those.
		const bool bRecInRom = BinaryTrace::PageTagIsRom(Rec.m_PageTag);
		if (bRecInRom != bInRom)
		{
			if (bInRom)
			{
				// Closing an in-ROM interval at this record's cycle.
				RomInterval Iv;
				Iv.m_StartCycle = RomIntervalStart;
				Iv.m_EndCycle = Rec.m_Cycle;
				if (Iv.m_EndCycle > Iv.m_StartCycle)
				{
					m_RomIntervals.append(Iv);
				}
			}
			else
			{
				// Entering ROM at this record's cycle.
				RomIntervalStart = Rec.m_Cycle;
			}
			bInRom = bRecInRom;
		}

		// Update running SP from this record. The writer emits Diff_SP for
		// every instruction that touches SP (push, pop, call, ret, ld sp,
		// ex (sp), inc/dec sp, interrupt acceptance, etc.). If Diff_SP is
		// unset, SP is unchanged from the previous record.
		if (Rec.m_Mask & kRegDiffBitDiffSp)
		{
			RunSP = Rec.m_PostSP;
		}

		// PRE-event close pass. The Diff_SP we just absorbed might mean an
		// existing frame's return address has now been popped past. Close
		// before deciding whether to open a new frame, so a CALL's own
		// post-push SP doesn't accidentally trigger a close of itself.
		// Special case for interrupts: kRecord_Interrupt also pushes a
		// return PC, so its Diff_SP is already the post-push value - skip
		// the pre-close in that case (handled below at push time).
		if (Rec.m_Tag != kRecord_Interrupt)
		{
			CloseFramesAboveSP(RunSP, Rec.m_Cycle);
		}

		// Decide whether this record OPENS a new frame.
		bool bPush = false;
		CallFrame NewFrame;

		if (Rec.m_Tag == kRecord_Interrupt)
		{
			bPush = true;
			NewFrame.m_KindV = CallFrame::kKindInterrupt;
			NewFrame.m_InterruptSource = uint8_t(CpuSource::FromIntAck(Rec.m_Opcodes));
			NewFrame.m_TargetPC = Rec.m_PostPC;
			NewFrame.m_CallerPC = Rec.PC;
		}
		else if (Rec.m_Tag == kRecord_Instr)
		{
			const uint8_t kB0 = Rec.m_Opcodes[0];

			if (IsCallUnconditional(kB0))
			{
				bPush = true;
				NewFrame.m_KindV = CallFrame::kKindZ80Call;
				NewFrame.m_TargetPC = Rec.m_PostPC;
				NewFrame.m_CallerPC = Rec.PC;
			}
			else if (IsCallConditional(kB0))
			{
				// Taken iff PostPC differs from the non-taken fall-through PC+3.
				if (Rec.m_PostPC != uint16_t(Rec.PC + 3))
				{
					bPush = true;
					NewFrame.m_KindV = CallFrame::kKindZ80Call;
					NewFrame.m_TargetPC = Rec.m_PostPC;
					NewFrame.m_CallerPC = Rec.PC;
				}
			}
			else if (IsRst(kB0))
			{
				bPush = true;
				NewFrame.m_KindV = CallFrame::kKindZ80Rst;
				NewFrame.m_TargetPC = RstTarget(kB0);
				NewFrame.m_CallerPC = Rec.PC;
			}
		}

		if (bPush)
		{
			NewFrame.m_StartCycle = Rec.m_Cycle;
			NewFrame.m_StartByteOffset = uint64_t(RecStart - Base);
			NewFrame.m_Depth = uint32_t(Stack.size()); // depth = current open count
			NewFrame.m_Parent = Stack.isEmpty() ? -1 : Stack.last();
			NewFrame.m_SpAfterPush = RunSP;
			// Highlight flags. InInterrupt cascades from the parent open
			// frame so nested calls inside an ISR are flagged too. InRom
			// is per-frame because RET-from-ROM-into-user-code exits the
			// ROM region.
			NewFrame.m_bIsInInterrupt = (NewFrame.m_KindV == CallFrame::kKindInterrupt) ||
										(!Stack.isEmpty() && m_Frames[Stack.last()].m_bIsInInterrupt);
			NewFrame.m_bIsInRom = (NewFrame.m_TargetPC < 0x4000);
			// Fallback page = the caller's (correct for same-page calls, and
			// all we have if the trace ends on this CALL). The next record
			// overwrites it with the true callee page via pendingPageFrame.
			NewFrame.m_TargetPageTag = Rec.m_PageTag;
			m_Frames.append(NewFrame);
			Stack.append(m_Frames.size() - 1);
			PendingPageFrame = m_Frames.size() - 1;
		}
	}

	// Close any frames still open at trace end at the header's EndCycle
	// (or last walked cycle when the trace wasn't finalised).
	const uint64_t FinalCycle = (H->m_EndCycle == kSentinel) ? RunCycle : H->m_EndCycle;
	for (int Idx : Stack)
	{
		if (m_Frames[Idx].m_EndCycle == 0)
		{
			m_Frames[Idx].m_EndCycle = FinalCycle;
		}
	}
	// Close a still-open ROM interval, if any.
	if (bInRom)
	{
		RomInterval Iv;
		Iv.m_StartCycle = RomIntervalStart;
		Iv.m_EndCycle = FinalCycle;
		if (Iv.m_EndCycle > Iv.m_StartCycle)
		{
			m_RomIntervals.append(Iv);
		}
	}
}

bool CallGraphBuilder::IsCycleInRom(uint64_t Cycle) const
{
	// Intervals are non-overlapping and sorted by StartCycle (they're
	// appended sequentially as the walker progresses through cycles).
	int Lo = 0, Hi = m_RomIntervals.size();
	while (Lo < Hi)
	{
		const int Mid = (Lo + Hi) / 2;
		if (m_RomIntervals[Mid].m_EndCycle <= Cycle)
		{
			Lo = Mid + 1;
		}
		else
		{
			Hi = Mid;
		}
	}
	return Lo < m_RomIntervals.size() && m_RomIntervals[Lo].m_StartCycle <= Cycle &&
		   Cycle < m_RomIntervals[Lo].m_EndCycle;
}

int CallGraphBuilder::FrameAtCycle(uint64_t Cycle) const
{
	int Best = -1;
	uint32_t BestDepth = 0;
	for (int IdxI = 0; IdxI < m_Frames.size(); ++IdxI)
	{
		const auto& F = m_Frames[IdxI];
		if (F.m_StartCycle <= Cycle && Cycle < F.m_EndCycle)
		{
			if (Best < 0 || F.m_Depth >= BestDepth)
			{
				Best = IdxI;
				BestDepth = F.m_Depth;
			}
		}
	}
	return Best;
}
