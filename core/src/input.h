#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// Spectrum input: keyboard matrix on port $FE + Kempston joystick on port $1F.
// Driven by a frame-timed event script. When the
// dispatcher's free-running frame counter advances past an event's scheduled
// frame, the event applies and the corresponding bits flip in the kept state.

struct KeyTarget
{
	bool m_bKempston; // false = keyboard row/bit, true = kempston bit
	uint8_t m_Row; // keyboard row 0..7 (ignored for kempston)
	uint8_t m_Bit; // bit 0..4 within row, or kempston bit 0..4
};

struct InputEvent
{
	uint64_t m_Frame;
	bool m_bPress; // true = press, false = release
	std::string m_Name; // user-facing label for the log
	std::vector<KeyTarget> m_Targets; // one for simple key, two for composites
};

struct InputState final
{
  public:
	InputState();

	// Parse a script file. Returns false and sets OutError on a malformed line.
	// DefaultTapFrames is the press-then-release hold for the `tap` keyword.
	bool LoadScript(const char* Path, int DefaultTapFrames, std::string& OutError);

	bool HasScript() const
	{
		return !m_Events.empty();
	}

	// Append programmatically-built events (e.g. an auto-typed LOAD "" sequence
	// for --tap boot), keeping the event list sorted by frame. Safe to call
	// whether or not a script was loaded.
	void AddEvents(const std::vector<InputEvent>& More);

	// Peek the next event whose Frame <= CurrentFrame. Returns nullptr when
	// there are no more events scheduled for this frame or earlier. Caller
	// should call ApplyAndAdvance() to commit the event's state change.
	const InputEvent* PeekPending(uint64_t CurrentFrame) const;

	// Apply the pending event (the one PeekPending would return), advancing
	// the internal cursor. Idempotent if PeekPending returns null.
	void ApplyAndAdvance();

	// Port-read intercept. Handles $FE (keyboard, low byte == 0xFE) and $1F
	// (Kempston). Returns true if the port matched and OutValue is set;
	// false to let the dispatcher fall through to other handlers.
	bool MaybeReadPort(uint16_t Port, uint8_t& OutValue) const;

	void Reset();

	// Inspection / dump accessors.
	const uint8_t* GetKeyboardRows() const
	{
		return m_KeyboardRows;
	}
	uint8_t GetKempstonState() const
	{
		return m_KempstonState;
	}
	size_t GetNextEventIndex() const
	{
		return m_NextEvent;
	}
	size_t GetEventCount() const
	{
		return m_Events.size();
	}

	// The whole event list (script and auto-typed keys merged), for a dump to
	// carry and a resume to put back before RestoreFromDump sets the cursor.
	const std::vector<InputEvent>& Events() const
	{
		return m_Events;
	}
	void SetEvents(std::vector<InputEvent> Events)
	{
		m_Events = std::move(Events);
	}

	// Resume support - set cursor + bit-state directly, bypassing the event
	// application logic. Used after LoadScript on resume.
	void RestoreFromDump(const uint8_t Rows[8], uint8_t KempState, size_t Cursor)
	{
		for (int IdxI = 0; IdxI < 8; ++IdxI)
		{
			m_KeyboardRows[IdxI] = Rows[IdxI];
		}
		m_KempstonState = KempState;
		m_NextEvent = Cursor;
	}

  private:
	void ApplyEvent(const InputEvent& Ev);

	uint8_t m_KeyboardRows[8]; // each: bits 0..4 key state (0=pressed); bits 5..7 floating
	uint8_t m_KempstonState; // bits 0..4 (active HIGH on Kempston)

	std::vector<InputEvent> m_Events; // sorted ascending by Frame
	size_t m_NextEvent = 0;
};
