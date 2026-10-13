// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <gtest/gtest.h>

#include "common/patch_path.h"

namespace {

namespace fs = std::filesystem;

class PatchPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() /
               ("shadps4-patch-lookup-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path Create(const fs::path& rel) {
        const auto file = root / rel;
        fs::create_directories(file.parent_path());
        std::ofstream out(file);
        out << "<Patch/>";
        out.close();
        return file;
    }

    fs::path root;
};

TEST_F(PatchPathTest, FindsPatchInProviderSubdirectory) {
    const auto expected = Create("shadPS4/God_of_War_III_Remastered.xml");
    const auto result =
        MemoryPatcher::ResolveExplicitPatchPath(root, "God_of_War_III_Remastered.xml");
    EXPECT_FALSE(result.ambiguous);
    EXPECT_EQ(result.path, expected);
}

TEST_F(PatchPathTest, DirectUserPatchTakesPrecedence) {
    Create("shadPS4/Game.xml");
    const auto direct = Create("Game.xml");
    const auto result = MemoryPatcher::ResolveExplicitPatchPath(root, "Game.xml");
    EXPECT_FALSE(result.ambiguous);
    EXPECT_EQ(result.path, direct);
}

TEST_F(PatchPathTest, PreservesAbsoluteFileAndExplicitProviderPaths) {
    const auto file = Create("other/Game.xml");
    const auto absolute = MemoryPatcher::ResolveExplicitPatchPath(root, file);
    EXPECT_EQ(absolute.path, file);
    EXPECT_FALSE(absolute.ambiguous);

    const auto relative = MemoryPatcher::ResolveExplicitPatchPath(root, "other/Game.xml");
    EXPECT_EQ(relative.path, file);
    EXPECT_FALSE(relative.ambiguous);
}

TEST_F(PatchPathTest, RefusesAmbiguousBasenames) {
    Create("providerA/Game.xml");
    Create("providerB/Game.xml");
    const auto result = MemoryPatcher::ResolveExplicitPatchPath(root, "Game.xml");
    EXPECT_TRUE(result.ambiguous);
    EXPECT_TRUE(result.path.empty());
}

TEST_F(PatchPathTest, MissingPatchAndMissingProviderAreNotFound) {
    Create("providerA/Another.xml");
    EXPECT_TRUE(MemoryPatcher::ResolveExplicitPatchPath(root, "Game.xml").path.empty());
    EXPECT_TRUE(MemoryPatcher::ResolveExplicitPatchPath(root / "missing", "Game.xml").path.empty());
}

TEST_F(PatchPathTest, NoProviderFallbackForTraversingNames) {
    Create("providerA/Game.xml");
    const auto result = MemoryPatcher::ResolveExplicitPatchPath(root, "../Game.xml");
    EXPECT_TRUE(result.path.empty());
    EXPECT_FALSE(result.ambiguous);
}

TEST_F(PatchPathTest, NoRecursiveProviderLookup) {
    Create("providerA/nested/Game.xml");
    EXPECT_TRUE(MemoryPatcher::ResolveExplicitPatchPath(root, "Game.xml").path.empty());
}

} // namespace
