// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "common/types.h"
#include "core/loader/elf.h"

namespace {

using Core::Loader::Elf;

constexpr u64 SdkVersion = 0x04508101;
constexpr u64 ProcParamOffset = 0x100;

// A minimal little-endian ET_SCE_DYNEXEC ELF with one PT_SCE_PROCPARAM program header whose
// p_filesz is `declared`, followed by `present` bytes of process parameters: size, magic,
// entry count and SDK version, then 0xCD filler.
std::vector<u8> MakeElf(u64 declared, u64 present) {
    std::vector<u8> file(ProcParamOffset + present, 0);
    const auto put = [&](size_t offset, auto value) {
        std::memcpy(file.data() + offset, &value, sizeof(value));
    };
    const u8 ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 9, 0};
    std::memcpy(file.data(), ident, sizeof(ident));
    put(16, u16{0xfe10}); // e_type
    put(18, u16{0x3e});   // e_machine
    put(20, u32{1});      // e_version
    put(32, u64{0x40});   // e_phoff
    put(52, u16{0x40});   // e_ehsize
    put(54, u16{0x38});   // e_phentsize
    put(56, u16{1});      // e_phnum
    put(58, u16{0x40});   // e_shentsize

    const size_t ph = 0x40;
    put(ph + 0, u32{0x61000001}); // PT_SCE_PROCPARAM
    put(ph + 4, u32{4});
    put(ph + 8, ProcParamOffset); // p_offset
    put(ph + 32, declared);       // p_filesz
    put(ph + 40, declared);       // p_memsz
    put(ph + 48, u64{8});

    const u8 params[24] = {};
    const size_t n = std::min<size_t>(present, sizeof(params));
    std::memcpy(file.data() + ProcParamOffset, params, n);
    if (present >= 8) {
        put(ProcParamOffset, declared);
    }
    if (present >= 24) {
        put(ProcParamOffset + 8, u32{0x4942524f}); // "ORBI"
        put(ProcParamOffset + 16, SdkVersion);
    }
    for (size_t i = ProcParamOffset + 24; i < file.size(); ++i) {
        file[i] = 0xcd;
    }
    return file;
}

class ElfProcParamTest : public ::testing::Test {
protected:
    u64 Read(const std::vector<u8>& bytes) {
        const auto path = std::filesystem::temp_directory_path() /
                          ("shadps4_procparam_" + std::to_string(counter++) + ".elf");
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        Elf elf;
        elf.Open(path);
        EXPECT_TRUE(elf.IsElfFile());
        const u64 version = elf.ReadProcParamSdkVersion();
        std::filesystem::remove(path);
        return version;
    }

    static inline int counter = 0;
};

TEST_F(ElfProcParamTest, ReadsSdkVersionFromOpenOrbisSizedSegment) {
    EXPECT_EQ(Read(MakeElf(0x50, 0x50)), SdkVersion);
}

TEST_F(ElfProcParamTest, ReadsSdkVersionFromShortestSegmentThatHoldsIt) {
    EXPECT_EQ(Read(MakeElf(24, 24)), SdkVersion);
}

TEST_F(ElfProcParamTest, SegmentTooShortForSdkVersionReadsZero) {
    EXPECT_EQ(Read(MakeElf(23, 23)), 0u);
    EXPECT_EQ(Read(MakeElf(0, 0)), 0u);
}

// Larger than the fields shadPS4 knows (OrbisProcParam is 0x60 bytes), as newer SDKs' are.
TEST_F(ElfProcParamTest, LargerSegmentReadsSdkVersion) {
    EXPECT_EQ(Read(MakeElf(0x80, 0x80)), SdkVersion);
}

// The copy used to take p_filesz bytes into a stack OrbisProcParam.
TEST_F(ElfProcParamTest, OversizedSegmentDoesNotOverflow) {
    EXPECT_EQ(Read(MakeElf(0x10000, 0x10000)), SdkVersion);
}

TEST_F(ElfProcParamTest, SegmentRunningPastEndOfFileReadsSdkVersion) {
    EXPECT_EQ(Read(MakeElf(0x10000, 0x40)), SdkVersion);
}

TEST_F(ElfProcParamTest, NoProcParamSegmentReadsZero) {
    auto bytes = MakeElf(0x50, 0x50);
    const u32 pt_load = 1;
    std::memcpy(bytes.data() + 0x40, &pt_load, sizeof(pt_load));
    EXPECT_EQ(Read(bytes), 0u);
}

} // namespace
