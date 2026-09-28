// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/file_sys/directories/pfs_directory.h"
#include "core/file_sys/fs.h"

namespace {

u32 g_entry_count = 0;

} // Anonymous namespace

// Minimal stand-in for the VFS: PfsDirectory only needs the directory listing.
namespace Core::FileSys {

void MntPoints::IterateDirectory(std::string_view guest_directory,
                                 const IterateDirectoryCallback& callback) {
    for (u32 i = 0; i < g_entry_count; ++i) {
        callback(std::filesystem::path{"/host/app0"} / ("file_" + std::to_string(i) + ".bin"),
                 true);
    }
}

} // namespace Core::FileSys

namespace {

using Core::Directories::BaseDirectory;
using Core::Directories::PfsDirectory;

constexpr s32 SeekSet = 0;
constexpr s32 SeekCur = 1;

std::shared_ptr<BaseDirectory> OpenDirectory(u32 entry_count) {
    g_entry_count = entry_count;
    return PfsDirectory::Create("/app0");
}

// Reads the whole directory with reads of `chunk` bytes, checking the read() contract on the way.
std::vector<u8> ReadInChunks(BaseDirectory& dir, u64 chunk) {
    std::vector<u8> out;
    std::vector<u8> buf(chunk);
    for (int i = 0; i < 1000; ++i) {
        const s64 offset_before = dir.lseek(0, SeekCur);
        const s64 result = dir.read(buf.data(), buf.size());
        EXPECT_GE(result, 0);
        EXPECT_LE(result, static_cast<s64>(chunk)) << "read returned more than requested";
        EXPECT_EQ(dir.lseek(0, SeekCur), offset_before + result)
            << "offset did not advance by the returned byte count";
        if (result <= 0 || result > static_cast<s64>(chunk) || ::testing::Test::HasFailure()) {
            return out;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + result);
    }
    ADD_FAILURE() << "read() never reached end of directory";
    return out;
}

TEST(PfsDirectoryTest, SmallReadsMakeProgressAndReachEnd) {
    auto dir = OpenDirectory(64);
    ReadInChunks(*dir, 512);
}

TEST(PfsDirectoryTest, SmallReadsReturnSameBytesAsOneLargeRead) {
    auto dir = OpenDirectory(64);
    const auto whole = ReadInChunks(*dir, 0x10000);
    ASSERT_EQ(dir->lseek(0, SeekSet), 0);
    const auto chunked = ReadInChunks(*dir, 512);
    ASSERT_FALSE(whole.empty());
    ASSERT_GE(chunked.size(), 64 * 16);
    const size_t common = std::min(whole.size(), chunked.size());
    EXPECT_TRUE(std::equal(chunked.begin(), chunked.begin() + common, whole.begin()));
}

TEST(PfsDirectoryTest, ReadAfterEndReturnsZero) {
    auto dir = OpenDirectory(64);
    ReadInChunks(*dir, 512);
    std::vector<u8> buf(512);
    EXPECT_EQ(dir->read(buf.data(), buf.size()), 0);
}

} // Anonymous namespace
