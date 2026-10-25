#include "ay.h"
#include "io.h"
#include "memory.h"
#include "z80.h"
#include <memory>
#include <cstdio>

// Actual Z80 OUT/IN paths: third-chip effects must not overwrite either
// music chip, and legacy two-chip TurboSound must retain its selects.
int main()
{
    auto mem = std::make_unique<MemoryBus>();
    mem->SetIsNext(true);
    mem->WriteNextReg(0x54, 32, 0, 0);
    mem->WriteNextReg(0x08, 2, 0, 0);
    AyState ay[3];
    IoContext io{*mem, nullptr, nullptr, ay[0], nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, true, false, false};
    io.m_Ay2 = &ay[1]; io.m_Ay3 = &ay[2];
    auto write = [&](uint8_t select, uint8_t value) {
        // select chip; select volume A; write; read back to A; HALT.
        uint8_t code[] = {0x01,0xfd,0xff,0x3e,select,0xed,0x79,
            0x3e,8,0xed,0x79,0x06,0xbf,0x3e,value,0xed,0x79,
            0x06,0xff,0xed,0x78,0x76};
        for (unsigned i = 0; i < sizeof code; ++i) mem->Wr(uint16_t(0x8000+i), code[i]);
        Z80State cpu; cpu.PC = 0x8000;
        uint64_t budget = 500;
        RunUntil(cpu, io, budget, nullptr);
        return uint8_t(cpu.AF >> 8);
    };
    for (int chip = 0; chip < 3; ++chip)
        if (write(uint8_t(255-chip), uint8_t(chip+3)) != chip+3) return 1;
    for (int chip = 0; chip < 3; ++chip)
        if (ay[chip].m_Registers[8] != chip+3 || ay[chip].m_WritesObserved != 1) return 2;
    mem->WriteNextReg(0x08, 0, 0, 0);
    if (write(255, 9) != 9 || ay[0].m_Registers[8] != 3 || ay[2].m_Registers[8] != 9) return 3;
    io.m_Ay3 = nullptr; io.m_AyActive = 0;
    mem->SetIsNext(false);
    if (write(0xf8, 7) != 7 || ay[1].m_Registers[8] != 7) return 4;
    if (write(0xfd, 8) != 8 || ay[0].m_Registers[8] != 8) return 5;
    puts("PASS: independent Next AY chips, readback, TurboSound gate and classic selects");
}
