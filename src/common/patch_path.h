// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <system_error>

namespace MemoryPatcher {

struct ExplicitPatchLookup {
    std::filesystem::path path;
    bool ambiguous = false;
};

// Resolve a user-specified patch without guessing which provider to prefer.
// Preserve explicit paths and direct PatchesDir entries, then search exactly
// one provider-directory level for bare filenames only.
inline ExplicitPatchLookup ResolveExplicitPatchPath(const std::filesystem::path& patch_dir,
                                                    const std::filesystem::path& requested) {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (fs::is_regular_file(requested, ec)) {
        return {.path = requested};
    }
    const auto direct = patch_dir / requested;
    if (fs::is_regular_file(direct, ec)) {
        return {.path = direct};
    }

    // No provider fallback for absolute paths or anything with path components.
    if (requested.empty() || requested.has_parent_path() || !fs::is_directory(patch_dir, ec)) {
        return {};
    }

    ExplicitPatchLookup result;
    fs::directory_iterator it(patch_dir, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    while (!ec && it != end) {
        std::error_code entry_error;
        const auto& entry = *it;
        // Like the automatic patch scan, follow symlinked provider folders.
        if (entry.is_directory(entry_error)) {
            const auto candidate = entry.path() / requested;
            if (fs::is_regular_file(candidate, entry_error)) {
                if (!result.path.empty()) {
                    return {.ambiguous = true};
                }
                result.path = candidate;
            }
        }
        it.increment(ec);
    }
    // A partial scan must not silently choose the first provider.
    if (ec) {
        return {};
    }
    return result;
}

} // namespace MemoryPatcher
