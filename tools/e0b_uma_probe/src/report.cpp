// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstdio>

#include "report.h"

namespace e0b {

const char* StatusName(Status s) {
    switch (s) {
    case Status::Pass:
        return "PASS";
    case Status::Fail:
        return "FAIL";
    case Status::Unsupported:
        return "UNSUPPORTED";
    case Status::Skip:
        return "SKIP";
    case Status::Error:
        return "ERROR";
    case Status::Info:
        return "INFO";
    }
    return "?";
}

static std::string Escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

void Report::Section(const std::string& name) {
    section_ = name;
    std::printf("\n=== %s ===\n", name.c_str());
    std::fflush(stdout);
}

void Report::Fact(const std::string& key, const std::string& value) {
    facts_.push_back({section_, key, value});
    std::printf("  %-44s %s\n", key.c_str(), value.c_str());
    std::fflush(stdout);
}

void Report::Note(const std::string& text) {
    std::printf("  # %s\n", text.c_str());
    std::fflush(stdout);
}

void Report::Add(const std::string& test, const std::string& variant, Status status,
                 const std::string& detail, std::vector<std::pair<std::string, double>> metrics) {
    std::printf("  [%-11s] %-34s %-14s %s\n", StatusName(status), test.c_str(), variant.c_str(),
                detail.c_str());
    for (const auto& [k, v] : metrics) {
        std::printf("  %60s %s = %.3f\n", "", k.c_str(), v);
    }
    std::fflush(stdout);
    results_.push_back({test, variant, status, detail, std::move(metrics)});
}

int Report::Count(Status s) const {
    int n = 0;
    for (const auto& r : results_) {
        n += r.status == s;
    }
    return n;
}

std::string Report::Json() const {
    std::string j = "{\n  \"tool\": \"e0b_uma_probe\",\n  \"format\": 1,\n  \"facts\": [\n";
    for (size_t i = 0; i < facts_.size(); ++i) {
        const auto& f = facts_[i];
        j += "    {\"section\": \"" + Escape(f.section) + "\", \"key\": \"" + Escape(f.key) +
             "\", \"value\": \"" + Escape(f.value) + "\"}";
        j += i + 1 < facts_.size() ? ",\n" : "\n";
    }
    j += "  ],\n  \"results\": [\n";
    for (size_t i = 0; i < results_.size(); ++i) {
        const auto& r = results_[i];
        j += "    {\"test\": \"" + Escape(r.test) + "\", \"variant\": \"" + Escape(r.variant) +
             "\", \"status\": \"" + StatusName(r.status) + "\", \"detail\": \"" + Escape(r.detail) +
             "\", \"metrics\": {";
        for (size_t m = 0; m < r.metrics.size(); ++m) {
            char num[64];
            const double v = r.metrics[m].second;
            std::snprintf(num, sizeof(num), "%.6g", std::isfinite(v) ? v : -1.0);
            j += "\"" + Escape(r.metrics[m].first) + "\": " + num;
            j += m + 1 < r.metrics.size() ? ", " : "";
        }
        j += "}}";
        j += i + 1 < results_.size() ? ",\n" : "\n";
    }
    j += "  ]\n}\n";
    return j;
}

void Report::PrintSummary() const {
    std::printf("\n=== summary ===\n");
    for (const auto& r : results_) {
        if (r.status == Status::Info) {
            continue;
        }
        std::printf("  %-11s %-34s %s\n", StatusName(r.status), r.test.c_str(), r.variant.c_str());
    }
    std::printf("  totals: PASS=%d FAIL=%d UNSUPPORTED=%d ERROR=%d SKIP=%d INFO=%d\n",
                Count(Status::Pass), Count(Status::Fail), Count(Status::Unsupported),
                Count(Status::Error), Count(Status::Skip), Count(Status::Info));
}

} // namespace e0b
