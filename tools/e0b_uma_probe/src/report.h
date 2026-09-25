// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace e0b {

enum class Status {
    Pass,        // functional check ran and saw no mismatches
    Fail,        // functional check ran and saw mismatches (a real correctness problem)
    Unsupported, // the device/driver/kernel cannot do this (an answer, not a bug)
    Skip,        // not applicable or disabled by options
    Error,       // unexpected API error; the result is inconclusive
    Info,        // informational measurement, no pass/fail meaning
};

const char* StatusName(Status s);

class Report {
public:
    struct Fact {
        std::string section;
        std::string key;
        std::string value;
    };
    struct Result {
        std::string test;
        std::string variant;
        Status status;
        std::string detail;
        std::vector<std::pair<std::string, double>> metrics;
    };

    void Section(const std::string& name);
    void Fact(const std::string& key, const std::string& value);
    void Note(const std::string& text);
    void Add(const std::string& test, const std::string& variant, Status status,
             const std::string& detail, std::vector<std::pair<std::string, double>> metrics = {});

    int Count(Status s) const;
    const std::vector<Result>& Results() const {
        return results_;
    }
    std::string Json() const;
    void PrintSummary() const;

private:
    std::string section_ = "general";
    std::vector<struct Fact> facts_;
    std::vector<Result> results_;
};

} // namespace e0b
