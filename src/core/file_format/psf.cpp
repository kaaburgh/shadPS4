// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <ranges>
#include <utility>

#include "common/assert.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "core/file_format/psf.h"

static const std::unordered_map<std::string_view, u32> psf_known_max_sizes = {
    {"ACCOUNT_ID", 8},  {"CATEGORY", 4},  {"DETAIL", 1024},       {"FORMAT", 4},
    {"MAINTITLE", 128}, {"PARAMS", 1024}, {"SAVEDATA_BLOCKS", 8}, {"SAVEDATA_DIRECTORY", 32},
    {"SUBTITLE", 128},  {"TITLE_ID", 12},
};
static inline u32 get_max_size(std::string_view key, u32 default_value) {
    if (const auto& v = psf_known_max_sizes.find(key); v != psf_known_max_sizes.end()) {
        return v->second;
    }
    return default_value;
}

bool PSF::Open(const std::filesystem::path& filepath) {
    using namespace std::chrono;
    if (std::filesystem::exists(filepath)) {
        const auto t = std::filesystem::last_write_time(filepath);
        const auto rel =
            duration_cast<seconds>(t - std::filesystem::file_time_type::clock::now()).count();
        const auto tp = system_clock::to_time_t(system_clock::now() + seconds{rel});
        last_write = system_clock::from_time_t(tp);
    }

    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        return false;
    }

    const u64 psfSize = file.GetSize();
    if (psfSize == 0) {
        LOG_ERROR(Core, "Empty PSF file: {}", filepath.string());
        return false;
    }
    std::vector<u8> psf(psfSize);
    file.Seek(0);
    const auto read_size = file.Read(psf);
    file.Close();
    if (read_size != psf.size()) {
        return false;
    }
    return Open(psf);
}

bool PSF::Open(const std::unique_ptr<Core::FileSys::IFile>& file) {
    std::vector<u8> psf_buf(file->Size());
    if (file->Read(psf_buf.data(), psf_buf.size()) != static_cast<s64>(psf_buf.size())) {
        return false;
    }
    return Open(psf_buf);
}

bool PSF::Open(const std::vector<u8>& psf_buffer) {
    entry_list.clear();
    map_binaries.clear();
    map_strings.clear();
    map_integers.clear();

    const auto size = psf_buffer.size();
    if (size < sizeof(PSFHeader)) {
        return false;
    }
    const auto* data = psf_buffer.data();
    PSFHeader header{};
    std::memcpy(&header, data, sizeof(header));

    if (header.magic != PSF_MAGIC) {
        LOG_ERROR(Core, "Invalid PSF magic number");
        return false;
    }
    if (header.version != PSF_VERSION_1_1 && header.version != PSF_VERSION_1_0) {
        LOG_ERROR(Core, "Unsupported PSF version: 0x{:08x}", header.version);
        return false;
    }

    // Validate the index table before converting its count into pointer offsets.
    const size_t count = header.index_table_entries;
    if (count > (size - sizeof(PSFHeader)) / sizeof(PSFRawEntry)) {
        return false;
    }
    const size_t table_end = sizeof(PSFHeader) + count * sizeof(PSFRawEntry);
    const size_t key_start = header.key_table_offset;
    const size_t value_start = header.data_table_offset;
    if (key_start < table_end || key_start > value_start || value_start > size) {
        return false;
    }

    std::vector<Entry> parsed_entries;
    decltype(map_binaries) parsed_binaries;
    decltype(map_strings) parsed_strings;
    decltype(map_integers) parsed_integers;

    for (size_t i = 0; i < count; ++i) {
        PSFRawEntry raw{};
        std::memcpy(&raw, data + sizeof(PSFHeader) + i * sizeof(raw), sizeof(raw));

        // Both offsets are relative to their respective table; subtract first
        // so that invalid offsets cannot overflow when added to the base.
        const size_t key_relative = raw.key_offset;
        if (key_relative >= value_start - key_start) {
            return false;
        }
        const size_t key_offset = key_start + key_relative;
        const char* key = reinterpret_cast<const char*>(data + key_offset);
        const auto* key_end =
            static_cast<const char*>(std::memchr(key, 0, value_start - key_offset));
        if (!key_end) {
            return false;
        }

        const size_t value_relative = raw.data_offset;
        if (value_relative > size - value_start) {
            return false;
        }
        const size_t value_offset = value_start + value_relative;
        const size_t value_size = raw.param_len;
        if (value_size > size - value_offset || value_size > raw.param_max_len) {
            return false;
        }

        const auto format = static_cast<PSFEntryFmt>(raw.param_fmt.Raw());
        const size_t index = parsed_entries.size();
        parsed_entries.push_back({std::string(key, key_end), format, raw.param_max_len});
        const u8* value = data + value_offset;

        switch (format) {
        case PSFEntryFmt::Binary: {
            parsed_binaries.emplace(index, std::vector<u8>(value, value + value_size));
            break;
        }
        case PSFEntryFmt::Text: {
            if (value_size == 0) {
                return false;
            }
            const char* text = reinterpret_cast<const char*>(value);
            const auto* terminator = static_cast<const char*>(std::memchr(text, 0, value_size));
            if (!terminator) {
                return false;
            }
            parsed_strings.emplace(index, std::string(text, terminator));
            break;
        }
        case PSFEntryFmt::Integer: {
            if (value_size != sizeof(s32)) {
                return false;
            }
            s32 number{};
            std::memcpy(&number, value, sizeof(number));
            parsed_integers.emplace(index, number);
            break;
        }
        default:
            return false;
        }
    }

    entry_list = std::move(parsed_entries);
    map_binaries = std::move(parsed_binaries);
    map_strings = std::move(parsed_strings);
    map_integers = std::move(parsed_integers);
    return true;
}

bool PSF::Encode(const std::filesystem::path& filepath) const {
    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    last_write = std::chrono::system_clock::now();

    const auto psf_buffer = Encode();
    const size_t written = file.Write(psf_buffer);
    if (written != psf_buffer.size()) {
        LOG_ERROR(Core, "Failed to write PSF file. Written {} Expected {}", written,
                  psf_buffer.size());
    }
    file.Close();
    return written == psf_buffer.size();
}

std::vector<u8> PSF::Encode() const {
    std::vector<u8> psf_buffer;
    Encode(psf_buffer);
    return psf_buffer;
}

void PSF::Encode(std::vector<u8>& psf_buffer) const {
    psf_buffer.resize(sizeof(PSFHeader) + sizeof(PSFRawEntry) * entry_list.size());

    {
        auto& header = *(PSFHeader*)psf_buffer.data();
        header.magic = PSF_MAGIC;
        header.version = PSF_VERSION_1_1;
        header.index_table_entries = entry_list.size();
    }

    const size_t key_table_offset = psf_buffer.size();
    ((PSFHeader*)psf_buffer.data())->key_table_offset = key_table_offset;
    for (size_t i = 0; i < entry_list.size(); i++) {
        auto& raw_entry = ((PSFRawEntry*)(psf_buffer.data() + sizeof(PSFHeader)))[i];
        const Entry& entry = entry_list[i];
        raw_entry.key_offset = psf_buffer.size() - key_table_offset;
        raw_entry.param_fmt.FromRaw(static_cast<u16>(entry.param_fmt));
        raw_entry.param_max_len = entry.max_len;
        std::ranges::copy(entry.key, std::back_inserter(psf_buffer));
        psf_buffer.push_back(0); // NULL terminator
    }

    const size_t data_table_offset = psf_buffer.size();
    ((PSFHeader*)psf_buffer.data())->data_table_offset = data_table_offset;
    for (size_t i = 0; i < entry_list.size(); i++) {
        if (psf_buffer.size() % 4 != 0) {
            std::ranges::fill_n(std::back_inserter(psf_buffer), 4 - psf_buffer.size() % 4, 0);
        }
        auto& raw_entry = ((PSFRawEntry*)(psf_buffer.data() + sizeof(PSFHeader)))[i];
        const Entry& entry = entry_list[i];
        raw_entry.data_offset = psf_buffer.size() - data_table_offset;

        s32 additional_padding = s32(raw_entry.param_max_len);

        switch (entry.param_fmt) {
        case PSFEntryFmt::Binary: {
            const auto& value = map_binaries.at(i);
            raw_entry.param_len = value.size();
            additional_padding -= s32(raw_entry.param_len);
            std::ranges::copy(value, std::back_inserter(psf_buffer));
        } break;
        case PSFEntryFmt::Text: {
            const auto& value = map_strings.at(i);
            raw_entry.param_len = value.size() + 1;
            additional_padding -= s32(raw_entry.param_len);
            std::ranges::copy(value, std::back_inserter(psf_buffer));
            psf_buffer.push_back(0); // NULL terminator
        } break;
        case PSFEntryFmt::Integer: {
            const auto& value = map_integers.at(i);
            raw_entry.param_len = sizeof(s32);
            additional_padding -= s32(raw_entry.param_len);
            const auto value_bytes = reinterpret_cast<const u8*>(&value);
            std::ranges::copy(value_bytes, value_bytes + sizeof(s32),
                              std::back_inserter(psf_buffer));
        } break;
        default:
            UNREACHABLE_MSG("Unknown PSF entry format");
        }
        ASSERT_MSG(additional_padding >= 0, "PSF entry max size mismatch");
        std::ranges::fill_n(std::back_inserter(psf_buffer), additional_padding, 0);
    }
}

std::optional<std::span<const u8>> PSF::GetBinary(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end()) {
        return {};
    }
    ASSERT(it->param_fmt == PSFEntryFmt::Binary);
    return std::span{map_binaries.at(index)};
}

std::optional<std::string_view> PSF::GetString(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end()) {
        return {};
    }
    ASSERT(it->param_fmt == PSFEntryFmt::Text);
    return std::string_view{map_strings.at(index)};
}

std::optional<s32> PSF::GetInteger(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end()) {
        return {};
    }
    ASSERT(it->param_fmt == PSFEntryFmt::Integer);
    return map_integers.at(index);
}

void PSF::AddBinary(std::string key, std::vector<u8> value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add binary key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Binary, "PSF: Change format is not supported");
        it->max_len = get_max_size(key, value.size());
        map_binaries.at(index) = std::move(value);
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.max_len = get_max_size(key, value.size());
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Binary;
    map_binaries.emplace(entry_list.size() - 1, std::move(value));
}

void PSF::AddBinary(std::string key, uint64_t value, bool update) {
    std::vector<u8> data(8);
    std::memcpy(data.data(), &value, 8);
    return AddBinary(std::move(key), std::move(data), update);
}

void PSF::AddString(std::string key, std::string value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add string key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Text, "PSF: Change format is not supported");
        it->max_len = get_max_size(key, value.size() + 1);
        map_strings.at(index) = std::move(value);
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.max_len = get_max_size(key, value.size() + 1);
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Text;
    map_strings.emplace(entry_list.size() - 1, std::move(value));
}

void PSF::AddInteger(std::string key, s32 value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add integer key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Integer, "PSF: Change format is not supported");
        it->max_len = sizeof(s32);
        map_integers.at(index) = value;
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Integer;
    entry.max_len = sizeof(s32);
    map_integers.emplace(entry_list.size() - 1, value);
}

std::pair<std::vector<PSF::Entry>::iterator, size_t> PSF::FindEntry(std::string_view key) {
    auto entry =
        std::ranges::find_if(entry_list, [&](const auto& entry) { return entry.key == key; });
    return {entry, std::distance(entry_list.begin(), entry)};
}

std::pair<std::vector<PSF::Entry>::const_iterator, size_t> PSF::FindEntry(
    std::string_view key) const {
    auto entry =
        std::ranges::find_if(entry_list, [&](const auto& entry) { return entry.key == key; });
    return {entry, std::distance(entry_list.begin(), entry)};
}
