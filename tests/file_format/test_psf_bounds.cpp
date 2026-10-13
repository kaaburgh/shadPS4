// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <vector>
#include <gtest/gtest.h>

#include "core/file_format/psf.h"

namespace {

constexpr size_t kHeaderSize = sizeof(PSFHeader);
constexpr size_t kEntrySize = sizeof(PSFRawEntry);
constexpr size_t kKeyStart = kHeaderSize + kEntrySize;
constexpr size_t kValueStart = 48;

std::vector<u8> ValidTextPsf() {
    std::vector<u8> bytes(60, 0);
    PSFHeader header{};
    header.magic = PSF_MAGIC;
    header.version = PSF_VERSION_1_1;
    header.index_table_entries = 1;
    header.key_table_offset = kKeyStart;
    header.data_table_offset = kValueStart;
    std::memcpy(bytes.data(), &header, sizeof(header));

    PSFRawEntry entry{};
    entry.key_offset = 0;
    entry.param_fmt.FromRaw(static_cast<u16>(PSFEntryFmt::Text));
    entry.param_len = 10;
    entry.param_max_len = 12;
    entry.data_offset = 0;
    std::memcpy(bytes.data() + kHeaderSize, &entry, sizeof(entry));
    std::memcpy(bytes.data() + kKeyStart, "TITLE_ID", 9);
    std::memcpy(bytes.data() + kValueStart, "CUSA00000", 10);
    return bytes;
}

template <typename Fn>
std::vector<u8> ModifyHeader(Fn&& fn) {
    auto bytes = ValidTextPsf();
    PSFHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    fn(header);
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

template <typename Fn>
std::vector<u8> ModifyEntry(Fn&& fn) {
    auto bytes = ValidTextPsf();
    PSFRawEntry entry{};
    std::memcpy(&entry, bytes.data() + kHeaderSize, sizeof(entry));
    fn(entry);
    std::memcpy(bytes.data() + kHeaderSize, &entry, sizeof(entry));
    return bytes;
}

TEST(PsfBounds, ValidTextAndRoundTrip) {
    PSF psf;
    ASSERT_TRUE(psf.Open(ValidTextPsf()));
    ASSERT_TRUE(psf.GetString("TITLE_ID").has_value());
    EXPECT_EQ(*psf.GetString("TITLE_ID"), "CUSA00000");

    PSF decoded;
    ASSERT_TRUE(decoded.Open(psf.Encode()));
    ASSERT_TRUE(decoded.GetString("TITLE_ID").has_value());
    EXPECT_EQ(*decoded.GetString("TITLE_ID"), "CUSA00000");
}

// An SFO as shadPS4 writes it, with every entry type, must parse back to the same values and
// re-encode to the same bytes.
TEST(PsfBounds, EncodedSfoWithEveryEntryTypeRoundTrips) {
    PSF psf;
    psf.AddString("CATEGORY", "gd");
    psf.AddString("CONTENT_ID", "IV0000-TEST00001_00-SHADTESTPSFTEST0");
    psf.AddString("TITLE", "Generated title");
    psf.AddString("TITLE_ID", "TEST00001");
    psf.AddInteger("ATTRIBUTE", -1);
    psf.AddInteger("SYSTEM_VER", 0x04500000);
    psf.AddBinary("SAVEDATA_BLOCKS", u64{0x1234567890abcdef});
    psf.AddBinary("PARAMS", std::vector<u8>{1, 2, 3, 0, 5});
    const auto bytes = psf.Encode();

    PSF decoded;
    ASSERT_TRUE(decoded.Open(bytes));
    EXPECT_EQ(decoded.GetEntries().size(), psf.GetEntries().size());
    EXPECT_EQ(decoded.GetString("CATEGORY"), "gd");
    EXPECT_EQ(decoded.GetString("CONTENT_ID"), "IV0000-TEST00001_00-SHADTESTPSFTEST0");
    EXPECT_EQ(decoded.GetString("TITLE"), "Generated title");
    EXPECT_EQ(decoded.GetInteger("ATTRIBUTE"), -1);
    EXPECT_EQ(decoded.GetInteger("SYSTEM_VER"), 0x04500000);
    const auto params = decoded.GetBinary("PARAMS");
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(std::vector<u8>(params->begin(), params->end()), (std::vector<u8>{1, 2, 3, 0, 5}));
    EXPECT_EQ(decoded.Encode(), bytes);
}

TEST(PsfBounds, ShortAndEmptyHeaders) {
    PSF psf;
    for (const size_t n : {size_t(0), size_t(1), kHeaderSize - 1}) {
        EXPECT_FALSE(psf.Open(std::vector<u8>(n, 0))) << n;
    }
}

TEST(PsfBounds, RejectsOversizedEntryTable) {
    PSF psf;
    EXPECT_FALSE(psf.Open(ModifyHeader([](PSFHeader& h) { h.index_table_entries = 0x80000000; })));
    EXPECT_FALSE(psf.Open(ModifyHeader([](PSFHeader& h) { h.index_table_entries = 4; })));
}

TEST(PsfBounds, RejectsOffsetsOutsideTables) {
    PSF psf;
    EXPECT_FALSE(psf.Open(ModifyHeader([](PSFHeader& h) { h.key_table_offset = 0x7fffffff; })));
    EXPECT_FALSE(psf.Open(ModifyHeader([](PSFHeader& h) { h.data_table_offset = 0x7fffffff; })));
    EXPECT_FALSE(psf.Open(ModifyHeader([](PSFHeader& h) { h.key_table_offset = 12; })));
    EXPECT_FALSE(psf.Open(ModifyHeader([](PSFHeader& h) { h.data_table_offset = 20; })));
    EXPECT_FALSE(psf.Open(ModifyEntry([](PSFRawEntry& e) { e.key_offset = 0x7fff; })));
    EXPECT_FALSE(psf.Open(ModifyEntry([](PSFRawEntry& e) { e.data_offset = 0x7fffffff; })));
}

TEST(PsfBounds, RejectsUnterminatedKeysAndValues) {
    PSF psf;
    auto key = ValidTextPsf();
    std::memset(key.data() + kKeyStart, 'X', kValueStart - kKeyStart);
    EXPECT_FALSE(psf.Open(key));
    auto value = ValidTextPsf();
    value[kValueStart + 9] = 'X';
    EXPECT_FALSE(psf.Open(value));
}

TEST(PsfBounds, RejectsBadLengthsAndTypes) {
    PSF psf;
    EXPECT_FALSE(psf.Open(ModifyEntry([](PSFRawEntry& e) { e.param_len = 16; })));
    EXPECT_FALSE(psf.Open(ModifyEntry([](PSFRawEntry& e) { e.param_max_len = 9; })));
    EXPECT_FALSE(psf.Open(ModifyEntry([](PSFRawEntry& e) { e.param_len = 0; })));
    EXPECT_FALSE(psf.Open(ModifyEntry([](PSFRawEntry& e) {
        e.param_fmt.FromRaw(static_cast<u16>(PSFEntryFmt::Integer));
        e.param_len = 3;
    })));
    EXPECT_FALSE(psf.Open(ModifyEntry([](PSFRawEntry& e) { e.param_fmt.FromRaw(0xffff); })));
}

TEST(PsfBounds, SupportsBoundedBinaryAndIntegerEntries) {
    PSF psf;
    auto binary = ModifyEntry([](PSFRawEntry& e) { e.param_fmt.FromRaw(static_cast<u16>(PSFEntryFmt::Binary)); });
    ASSERT_TRUE(psf.Open(binary));
    auto bytes = psf.GetBinary("TITLE_ID");
    ASSERT_TRUE(bytes.has_value());
    ASSERT_EQ(bytes->size(), 10);
    EXPECT_EQ((*bytes)[0], static_cast<u8>('C'));

    auto integer = ModifyEntry([](PSFRawEntry& e) {
        e.param_fmt.FromRaw(static_cast<u16>(PSFEntryFmt::Integer));
        e.param_len = sizeof(s32);
    });
    ASSERT_TRUE(psf.Open(integer));
    EXPECT_TRUE(psf.GetInteger("TITLE_ID").has_value());
}

TEST(PsfBounds, InvalidSecondParseDoesNotExposeStaleOrPartialValues) {
    PSF psf;
    ASSERT_TRUE(psf.Open(ValidTextPsf()));
    auto invalid = ModifyEntry([](PSFRawEntry& e) { e.data_offset = 0x7fffffff; });
    EXPECT_FALSE(psf.Open(invalid));
    EXPECT_FALSE(psf.GetString("TITLE_ID").has_value());
    EXPECT_TRUE(psf.GetEntries().empty());
}

} // namespace
