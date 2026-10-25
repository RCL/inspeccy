#include "dma.h"
#include "memory.h"
#include <cstdio>
#include <memory>

// Exercise the command parser and byte stepper together, including wraparound
// and reprogramming without RESET. Native NextAutoTester family 03 covers the
// corresponding hardware transfers with guards around the destination.
int main()
{
    auto mem = std::make_unique<MemoryBus>();
    for (int slot = 0; slot < 8; ++slot)
        mem->WriteNextReg(uint8_t(0x50 + slot), uint8_t(32 + slot), 0, 0, 0);
    ZxnDmaState dma;
    int failures = 0;
    for (int srcStep : {-1, 0, 1}) for (int dstStep : {-1, 0, 1})
    {
        // Source crosses 0xFFFF/0x0000 in either direction. Destination is RAM.
        const uint16_t src = srcStep < 0 ? 1 : 0xFFFE;
        const uint16_t dst = 0x8003;
        for (int i = 0; i < 4; ++i)
            mem->Wr(uint16_t(src + i * srcStep), uint8_t(0x31 + i));
        for (int i = -4; i <= 4; ++i) mem->Wr(uint16_t(dst + i), 0xA5);
        const uint8_t program[] = {0x83, 0x7D, uint8_t(src), uint8_t(src >> 8), 4, 0,
            uint8_t(srcStep < 0 ? 4 : srcStep ? 0x14 : 0x24),
            uint8_t(dstStep < 0 ? 0 : dstStep ? 0x10 : 0x20),
            0xAD, uint8_t(dst), uint8_t(dst >> 8), 0x82, 0xCF, 0x87};
        for (uint8_t byte : program) WriteZxnDma(dma, byte, *mem, 0);
        if (StepZxnDma(dma, *mem, 2) != 2 || StepZxnDma(dma, *mem, 20) != 2) ++failures;
        for (int i = -4; i <= 4; ++i)
        {
            uint8_t expected = 0xA5;
            for (int j = 0; j < 4; ++j)
                if (i == j * dstStep) expected = mem->Rd(uint16_t(src + j * srcStep));
            if (mem->Rd(uint16_t(dst + i)) != expected) ++failures;
        }
        if (dma.m_CurSrcAddr != uint16_t(src + 4 * srcStep)
            || dma.m_CurDstAddr != uint16_t(dst + 4 * dstStep)) ++failures;
    }
    // A fixed I/O destination must not retain a previous decrement mode.
    WriteZxnDma(dma, 0x08, *mem, 0);
    WriteZxnDma(dma, 0x28, *mem, 0);
    if (dma.m_bPortBDec || dma.m_bPortBInc || !dma.m_bPortBIo) ++failures;
    dma.Reset();
    if (dma.m_bPortADec || dma.m_bPortBDec) ++failures;
    if (failures) std::fprintf(stderr, "DMA addressing: %d failures\n", failures);
    return failures ? 1 : 0;
}
