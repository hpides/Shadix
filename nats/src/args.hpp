// Tiny argument helpers for the rtt/throughput CLIs.
//
// Keeps the rtt and throughput CLIs aligned so the just/*.just wrappers can
// drive both binaries consistently.
//
// Supports "--key value" and "--key=value" forms. Short flag aliases are
// hard-coded per call site (we don't try to be a general clap clone).
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace natsbench {

inline std::vector<std::string> argv_to_vec(int argc, char** argv) {
    std::vector<std::string> out;
    out.reserve(argc);
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

// Returns the argument value following `--<long>` or `-<short>`, accepting
// both space- and `=`-separated forms. nullopt if not present.
inline std::optional<std::string> find_opt(const std::vector<std::string>& args,
                                            std::string_view long_name,
                                            std::string_view short_name = "") {
    std::string lp = std::string("--") + std::string(long_name);
    std::string lpe = lp + "=";
    std::string sp = short_name.empty() ? std::string{}
                                        : std::string("-") + std::string(short_name);
    std::string spe = short_name.empty() ? std::string{} : sp + "=";

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == lp || (!sp.empty() && a == sp)) {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("missing value for " + a);
            }
            return args[i + 1];
        }
        if (a.size() > lpe.size() && a.compare(0, lpe.size(), lpe) == 0) {
            return a.substr(lpe.size());
        }
        if (!spe.empty() && a.size() > spe.size() &&
            a.compare(0, spe.size(), spe) == 0) {
            return a.substr(spe.size());
        }
    }
    return std::nullopt;
}

inline std::string require_opt(const std::vector<std::string>& args,
                                std::string_view long_name,
                                std::string_view short_name = "") {
    auto v = find_opt(args, long_name, short_name);
    if (!v) throw std::runtime_error(std::string("missing required --") +
                                      std::string(long_name));
    return *v;
}

// humantime-compatible duration parser. Accepts "<num><unit>" with unit in
// {ns, us, ms, s, m, h}; defaults to seconds if no unit.
inline std::uint64_t parse_duration_ns(const std::string& s) {
    std::size_t pos = 0;
    long double value = std::stold(s, &pos);
    std::string unit = s.substr(pos);
    long double mult = 1'000'000'000.0L;  // default: seconds
    if (unit == "ns") mult = 1.0L;
    else if (unit == "us" || unit == "µs") mult = 1'000.0L;
    else if (unit == "ms") mult = 1'000'000.0L;
    else if (unit == "s" || unit.empty()) mult = 1'000'000'000.0L;
    else if (unit == "m") mult = 60ULL * 1'000'000'000.0L;
    else if (unit == "h") mult = 3600ULL * 1'000'000'000.0L;
    else throw std::runtime_error("unknown duration unit: " + unit);
    return static_cast<std::uint64_t>(value * mult);
}

inline std::size_t parse_size(const std::string& s) {
    return static_cast<std::size_t>(std::stoull(s));
}
inline std::uint64_t parse_u64(const std::string& s) {
    return static_cast<std::uint64_t>(std::stoull(s));
}
inline double parse_f64(const std::string& s) { return std::stod(s); }

}  // namespace natsbench
