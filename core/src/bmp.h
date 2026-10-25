#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct MemoryBus;
struct SpritesState;

// Eager beam drawer (pixel-exact rendering). Instead of compositing the whole
// frame from an end-of-frame snapshot, the drawer paints the composite in beam
// order, advancing a pixel cursor. The emulation core fires a boundary at every
// write to displayed state (see MemoryBus::SetBeamBoundaryHook); FlushTo() then
// draws every pixel up to that write's beam position from the CURRENT (pre-write)
// live state, so per-scanline / mid-scanline / mid-frame changes render where the
// beam actually crossed them - including writes INTO displayed memory, with no
// shadow buffer (we read live RAM at the instant we draw each pixel).
//
// Configure() once with the frame timing; drive the lifecycle by pointing the
// boundary hook at FlushTo(). The last fully-scanned frame is the coherent one to
// emit (WriteBmp).
struct BeamDrawer
{
  public:
	void Configure(uint64_t FrameIntPeriod, int LinesPerFrame, int TopBorderLines);
	bool IsConfigured() const
	{
		return m_Period != 0;
	}

	// Flush the beam up to `Cycle` (rolling frames over as needed), drawing spans
	// from the current live state of Mem/Sprites. Call before a boundary write lands.
	void FlushTo(const MemoryBus& Mem, const SpritesState* Sprites, uint64_t Cycle);
	// Complete the in-progress frame (draw its remaining pixels) and promote it.
	void Finish(const MemoryBus& Mem, const SpritesState* Sprites);
	// Write the last completed frame (or the in-progress one if none completed).
	bool WriteBmp(const char* Path, std::string& OutError) const;
	// Copy only a completed frame to a live frontend, BGR top-down. Leaves
	// the destination untouched until the first frame completes. Pitch may
	// include padding, which is also left untouched.
	bool CopyCompletedBgr(uint8_t* OutBgrTopDown, int RowPitch) const;
	/** The same completed/first-partial selection as WriteBmp, for live sinks. */
	bool CopyCaptureBgr(uint8_t* OutBgrTopDown, int RowPitch) const;
	bool HasFrame() const
	{
		return m_bHaveCompleted || m_Cursor > 0;
	}
	bool HasCompletedFrame() const
	{
		return m_bHaveCompleted;
	}

  private:
	void Begin(uint64_t Origin);
	int PixelForCycle(uint64_t Cycle) const;
	void DrawSpan(const MemoryBus& Mem, const SpritesState* Sprites, int From, int To);

	std::vector<uint8_t> m_Image; // current frame (COMP_IMAGE_SIZE, BGR bottom-up)
	std::vector<uint8_t> m_Completed; // last fully-scanned frame
	bool m_bHaveCompleted = false;
	int m_Cursor = 0; // next composite pixel index to draw
	uint64_t m_FrameOrigin = ~uint64_t(0);
	uint64_t m_Period = 0;
	uint64_t m_CyclesPerLine = 0;
	int m_Lines = 312;
	int m_TopBorder = 64;
};

// Render the Spectrum-displayed screen (paper area + 48-pixel border) to a
// 16-colour 4bpp Windows BMP file at Path. 352 x 288 pixels.
//
// The displayed bank is chosen by Mem.GetPort7FFD() bit 3 (shadow screen on
// 128K); 48K SNAs always render from bank 5. Border colour = the last value
// written to ULA $FE (bits 0..2), tracked in Mem.GetBorderColour().
//
// Flash bit is ignored - the BMP is a single-instant snapshot.
//
// Returns false and sets OutError on a file-open or write failure.
bool WriteSpectrumScreenBmp(const MemoryBus& Mem, const char* Path, std::string& OutError);

// Render a Timex SCLD screen to a 16-colour 4bpp Windows BMP. The active mode is
// read from Mem.GetScldMode() (port $FF): standard 8x8 (screen 1 $4000 / screen 2
// $6000), extended 8x1 colour (per-scanline attributes from $6000), or 512x192
// hi-res (two display files interleaved, 2 colours from mode bits 3-5). Canvas is
// 352x288 for the 256-wide modes and 608x288 for hi-res, with a 48px border.
//
// Returns false and sets OutError on a file-open or write failure.
bool WriteTimexScreenBmp(const MemoryBus& Mem, const char* Path, std::string& OutError);

// Render the ZX Next Layer 2 framebuffer as an 8bpp indexed Windows BMP.
//
// The L2 mode (256x192 / 320x256 / 640x256x4bpp) is read from NR_L2_CTRL
// ($70) and the framebuffer base from NR_L2_BANK ($12) - the bank pointer
// covers a contiguous run of 8K RAM pages (6, 10 or 10 pages depending on
// mode). Pixel storage is column-major (matching the NXI layout); for 4bpp
// 640-mode, each byte holds two horizontally-adjacent pixels.
//
// Palette: the active L2 palette (index 2 or 3 per NR_PALETTE_CTRL bit 2)
// is decoded from the 9-bit RRRGGGBBb format the hardware stores.
//
// Returns false and sets OutError on file-open / write failure, or if
// Mem.DetectActiveLayer() does not report an L2 mode.
bool WriteNextLayer2Bmp(const MemoryBus& Mem, const char* Path, std::string& OutError);

// Render the ZX Next LoRes framebuffer as an 8bpp indexed BMP. LoRes is
// 128x96 pixels, 2 pixels per byte (top nibble = left, low = right), and
// uses palette 0 (ULA-1) entries 0..15 plus the 4-bit offset that lives
// in NR_6A's low nibble. The data lives in physical bank 5, at offset $0000
// or $2000 (6144 bytes). NR_6A bit 4 XOR port $FF bit 0 selects that file.
bool WriteNextLoResBmp(const MemoryBus& Mem, const char* Path, std::string& OutError);

// Layer-aware dispatcher used by main.cpp's --bmp-out path: picks the
// best-matching encoder for whatever the dot command (or SNA) chose to
// display. On Next-family machines the RGB888 composite (see
// WriteCompositeBmp) is the default - it's what the user sees on real
// hardware, with Layer 2 + ULA / LoRes + sprites combined per NR_15
// priority. On 48K/128K the classic 16-colour 4bpp ULA encoder is used.
//
// SpritesState may be null when the caller doesn't have access to the
// sprite engine (resume of a non-Next dump, etc.) - sprites just won't
// render in that case.
struct SpritesState;
bool WriteVisibleScreenBmp(const MemoryBus& Mem, const SpritesState* Sprites, const char* Path, std::string& OutError);

// Per-layer debug encoders. Reachable via the --bmp-per-layer flag.
// Useful for diagnosing exactly which layer is contributing what to the
// composite.
bool WritePerLayerScreenBmp(const MemoryBus& Mem, const char* Path, std::string& OutError);

// Render a composite frame - Layer 2 (256/320/640), ULA / LoRes / Timex modes
// and hardware sprites in the NR_15 order incl. the two blend codes - as an
// end-of-frame snapshot of live state. 320x256 canvas, 24-bit RGB888 BMP.
// Same pixel function as the BeamDrawer (see above); the drawer is the
// pixel-exact variant, this is the cheap one.
//
// Returns false / sets OutError on file failure.
struct SpritesState;
// Render the 256x192 PAPER area (no border) as 8-bit luminance, top-down,
// into a caller-supplied 256*192 buffer. Machine-aware: the Next composite on
// Next-family machines, the SCLD decode on Timex/ZX-Uno, the classic ULA
// elsewhere.
//
// The border is deliberately excluded. Border width differs per encoder (48 px
// classic, 32 px composite), so any cross-machine measurement taken off the
// full canvas is silently misaligned - which is exactly the trap the video
// frames set. Paper-only is the same 256x192 window on every machine, so a
// port and its original are directly comparable.
bool RenderPaperLuma(const MemoryBus& Mem, const SpritesState* Sprites, uint8_t* OutLuma, std::string& OutError);

bool WriteCompositeBmp(const MemoryBus& Mem, const SpritesState* Sprites, const char* Path, std::string& OutError);

// The same end-of-frame composite into memory, for a live display: 320x256
// top-down rows of packed 24-bit BGR, RowPitch bytes apart (at least 960).
// Nothing but the one FrameCtx build and the per-pixel compose; no file.
void RenderCompositeBgr(const MemoryBus& Mem, const SpritesState* Sprites, uint8_t* OutBgrTopDown, int RowPitch);
// The same frame 640 pixels wide: the 640x256 Layer 2 mode at full horizontal
// resolution (both nibbles of every byte), every other layer pixel-doubled.
// OutBgrTopDown holds 256 rows of 640 BGR pixels at RowPitch bytes each.
void RenderCompositeBgrHires(const MemoryBus& Mem, const SpritesState* Sprites, uint8_t* OutBgrTopDown, int RowPitch);
