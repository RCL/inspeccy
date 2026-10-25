#include "bmp.h"
#include "memory.h"
#include "sprites.h"
#include <algorithm>
#include <memory>
#include <vector>
#include <cstdio>
#include <cstring>

// Differential coverage for span-sized sprite rasterisation: whole-frame
// composition must agree with many tiny beam spans, including spans crossing
// rows, clipped/wrapped sprites, transforms, scaling, relatives and 4-bit data.
static bool TestSpriteSpans()
{
    auto mem = std::make_unique<MemoryBus>();
    auto sprites = std::make_unique<SpritesState>();
    mem->SetIsNext(true);
    mem->WriteNextReg(0x68, 0x80);
    mem->WriteNextReg(0x4a, 0x1c);
    constexpr uint64_t period = 312 * 224;
    constexpr int split = 100 * 320 + 160;
    constexpr uint64_t splitCycle = 132 * 224 + 112;
    mem->SetFrameIntPeriodHint(period);
    for (uint8_t bank : {uint8_t(0x20), uint8_t(0x60)}) {
        mem->WriteNextReg(0x43, bank);
        mem->WriteNextReg(0x40, 0);
        for (int i = 0; i < 256; ++i) mem->WriteNextReg(0x41, uint8_t(i * 5 + bank));
    }
    for (int s = 0; s < 128; ++s) {
        for (int p = 0; p < 256; ++p)
            sprites->m_Slots[s].m_Pattern[p] = uint8_t((p * 7 + s * 19) ^ (p / 16));
        auto* a = sprites->m_Slots[s].m_Attr.m_Bytes;
        a[0] = uint8_t(s * 23); a[1] = uint8_t(s * 37);
        a[2] = uint8_t(s * 11); a[3] = uint8_t(0x80 | (s & 0x7f));
        a[4] = uint8_t(s * 13);
    }
    std::vector<uint8_t> expected(320 * 256 * 3), after(expected.size()), actual(expected.size());
    for (int variant = 0; variant < 6; ++variant) {
        mem->WriteNextReg(0x15, uint8_t(1 | (variant & 1 ? 2 : 0) | (variant & 2 ? 0x60 : 0)));
        mem->WriteNextReg(0x43, uint8_t(variant & 4 ? 8 : 0));
        for (uint8_t v : {uint8_t(17), uint8_t(119), uint8_t(11), uint8_t(203)})
            mem->WriteNextReg(0x19, v);
        RenderCompositeBgr(*mem, sprites.get(), expected.data(), 960);
        BeamDrawer beam;
        beam.Configure(period, 312, 64);
        for (uint64_t c = 0; c < splitCycle; c += 137) beam.FlushTo(*mem, sprites.get(), c);
        beam.FlushTo(*mem, sprites.get(), splitCycle);
        // Change live inputs after flushing. Already-drawn pixels must keep
        // their old colour even when the next span is in the very same row.
        sprites->m_Slots[70].m_Attr.m_Bytes[0] ^= 0x7f;
        sprites->m_Slots[70].m_Attr.m_Bytes[1] = 96;
        sprites->m_Slots[6].m_Pattern[8 * 16] ^= 0xff;
        mem->WriteNextReg(0x4b, uint8_t(variant * 17));
        mem->WriteNextReg(0x43, uint8_t(variant & 4 ? 0 : 8));
        RenderCompositeBgr(*mem, sprites.get(), after.data(), 960);
        std::copy(after.begin() + split * 3, after.end(), expected.begin() + split * 3);
        for (uint64_t c = splitCycle + 59; c < period; c += 137) beam.FlushTo(*mem, sprites.get(), c);
        beam.FlushTo(*mem, sprites.get(), period);
        if (!beam.CopyCompletedBgr(actual.data(), 960) || actual != expected) {
            std::fprintf(stderr, "sprite span mismatch, variant %d\n", variant);
            return false;
        }
    }
    return true;
}

// Radastan display-file selection is independent of CPU paging. Red Supremacy
// alternates these files while drawing; ignoring NR $6A bit 4 tears its output.
static bool TestLoResDisplayFile()
{
    auto mem = std::make_unique<MemoryBus>();
    mem->SetIsNext(true);
    mem->WriteNextReg(0x15, 0x80);
    mem->WriteNextReg(0x43, 0);
    mem->WriteNextReg(0x40, 1); mem->WriteNextReg(0x41, 0xe0); // red
    mem->WriteNextReg(0x40, 2); mem->WriteNextReg(0x41, 3);    // blue
    std::memset(mem->Ram16KBank(5), 0x11, 6144);
    std::memset(mem->Ram16KBank(5) + 0x2000, 0x22, 6144);
    std::vector<uint8_t> frame(320 * 256 * 3);
    auto matches = [&](int x, int y, bool blue) {
        const auto* p = frame.data() + (y * 320 + x) * 3;
        return blue ? (p[0] > 0 && p[1] == 0 && p[2] == 0)
                    : (p[0] == 0 && p[1] == 0 && p[2] == 255);
    };
    for (int ff = 0; ff < 2; ++ff) for (int nr = 0; nr < 2; ++nr) {
        mem->SetScldMode(uint8_t(ff));
        mem->WriteNextReg(0x6a, uint8_t(0x20 | (nr << 4)));
        RenderCompositeBgr(*mem, nullptr, frame.data(), 960);
        if (!matches(40, 40, (ff ^ nr) != 0) || !matches(280, 216, (ff ^ nr) != 0)) return false;
    }
    mem->SetScldMode(0);
    mem->WriteNextReg(0x6a, 0x20);
    constexpr uint64_t period = 312 * 224;
    mem->SetFrameIntPeriodHint(period);
    BeamDrawer beam;
    beam.Configure(period, 312, 64);
    struct Context { BeamDrawer* b; MemoryBus* m; } ctx{&beam, mem.get()};
    mem->SetBeamBoundaryHook([](void* p, uint64_t cycle) {
        auto& c = *static_cast<Context*>(p); c.b->FlushTo(*c.m, nullptr, cycle);
    }, &ctx);
    beam.FlushTo(*mem, nullptr, 0);
    mem->WriteNextReg(0x6a, 0x30, 0, 160 * 224);
    // The now-hidden file can be overwritten without changing the lower half.
    std::memset(mem->Ram16KBank(5), 0x22, 6144);
    beam.FlushTo(*mem, nullptr, period);
    if (!beam.CopyCompletedBgr(frame.data(), 960) || !matches(40, 40, false) || !matches(40, 216, true)) return false;
    // 8bpp LoRes uses both halves for one image; bit 4 must not move its base.
    std::memset(mem->Ram16KBank(5), 1, 6144);
    std::memset(mem->Ram16KBank(5) + 0x2000, 2, 6144);
    mem->SetBeamBoundaryHook(nullptr, nullptr);
    mem->WriteNextReg(0x6a, 0x10);
    RenderCompositeBgr(*mem, nullptr, frame.data(), 960);
    return matches(40, 40, false) && matches(40, 216, true);
}

// Exercise a display register write at a known raster position, then the
// top-down live-frame API. This must retain the split after the register is
// changed again in the following, incomplete frame.
int main()
{
    auto mem = std::make_unique<MemoryBus>();
    mem->SetIsNext(true);
    mem->WriteNextReg(0x68, 0x80, 0, 0); // transparent ULA, no other layers
    mem->WriteNextReg(0x4a, 0xe0, 0, 0); // fallback red
    constexpr uint64_t period = 312 * 224;
    constexpr int pitch = 320 * 3 + 15;
    mem->SetFrameIntPeriodHint(period);
    BeamDrawer beam;
    beam.Configure(period, 312, 64);
    struct Context { BeamDrawer* b; MemoryBus* m; } ctx{&beam, mem.get()};
    mem->SetBeamBoundaryHook([](void* p, uint64_t cycle) {
        auto& c = *static_cast<Context*>(p); c.b->FlushTo(*c.m, nullptr, cycle);
    }, &ctx);
    std::vector<uint8_t> frame(pitch * 256, 0xa5);
    if (beam.CopyCompletedBgr(frame.data(), pitch)) return 1;
    if (frame[0] != 0xa5) return 2;
    beam.FlushTo(*mem, nullptr, 0);
    // Composite row 128 = raster line 160, after the 32-row upper margin.
    mem->WriteNextReg(0x4a, 3, 0, 160 * 224); // blue, flushes red first
    beam.FlushTo(*mem, nullptr, period);
    if (!beam.CopyCompletedBgr(frame.data(), pitch)) return 3;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 320; ++x) {
            if (x < 32 || x >= 288 || y < 32 || y >= 224) continue; // fallback paints the paper
            const auto* p = frame.data() + y * pitch + x * 3;
            if (y < 128 ? (p[2] != 255 || p[0] || p[1]) : (p[0] == 0 || p[1] || p[2])) {
                std::fprintf(stderr, "pixel %d,%d BGR=%u,%u,%u\n", x, y, p[0], p[1], p[2]);
                return 4;
            }
        }
        for (int x = 960; x < pitch; ++x) if (frame[y*pitch+x] != 0xa5) return 5;
    }
    auto completed = frame;
    mem->WriteNextReg(0x4a, 0x1c, 0, period + 80 * 224);
    beam.FlushTo(*mem, nullptr, period + 120 * 224);
    if (!beam.CopyCompletedBgr(frame.data(), pitch) || frame != completed) return 6;
    if (beam.CopyCompletedBgr(frame.data(), 959) || beam.CopyCompletedBgr(nullptr, pitch)) return 7;
    if (!TestSpriteSpans()) return 8;
    if (!TestLoResDisplayFile()) return 9;
    puts("PASS: beam split, completed-frame isolation, orientation, stride and sprite spans");
}
