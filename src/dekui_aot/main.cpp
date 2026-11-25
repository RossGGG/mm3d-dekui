// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// dekui_aot: offline analysis and C++ generation for MM3D's guest code.
//
//   dekui_aot analyze <work> [--coverage blocks.txt]... [--no-scan]
//   dekui_aot emit <work> <output-directory> [--coverage blocks.txt]... [--no-scan] [--shards N]
//
// <work> holds code.bin and layout.json from tools/aot/extract_code.py. Every
// output is derived from the user's dump and is written back into <work>.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <json.hpp>
#include "dekui_aot/discover.h"
#include "dekui_aot/emit_cpp.h"
#include "dekui_aot/frontend.h"
#include "dekui_aot/image.h"
#include <dynarmic/ir/basic_block.h>

namespace {

using namespace DekuiAot;
using u32 = std::uint32_t;

const char* SeedName(Seed seed) {
    switch (seed) {
    case Seed::Entrypoint:
        return "entrypoint";
    case Seed::Coverage:
        return "coverage";
    case Seed::Pointer:
        return "pointer";
    case Seed::Successor:
        return "successor";
    }
    return "?";
}

struct Arguments {
    std::string command;
    std::filesystem::path work;
    std::filesystem::path output;
    std::vector<std::string> coverage;
    bool scan = true;
    std::size_t shards = 64;
};

void WriteBlocks(const std::filesystem::path& path, const Program& program) {
    std::ofstream out(path);
    for (const auto& [descriptor, info] : program.blocks)
        out << fmt::format("{:08x} {:08x} {:08x} {}\n", u32(descriptor), u32(descriptor >> 32),
                           info.end, SeedName(info.seed));
}

nlohmann::json Report(const Image& image, const Program& program,
                      const std::vector<std::uint64_t>& coverage) {
    std::size_t indirect = 0, interpret = 0, exception = 0, bytes = 0;
    std::map<std::string, std::size_t> by_seed, by_segment, by_mode;
    for (const auto& [descriptor, info] : program.blocks) {
        indirect += info.indirect_exit;
        interpret += info.interpret;
        exception += info.exception;
        bytes += info.end - info.start;
        ++by_seed[SeedName(info.seed)];
        const auto* segment = image.SegmentAt(info.start);
        ++by_segment[segment ? segment->name : "outside"];
        ++by_mode[fmt::format("{:08x}", u32(descriptor >> 32))];
    }
    std::size_t covered = 0;
    std::vector<std::string> missing;
    for (const auto descriptor : coverage) {
        if (program.blocks.contains(descriptor))
            ++covered;
        else if (missing.size() < 64)
            missing.push_back(fmt::format("{:08x}:{:08x}", u32(descriptor), u32(descriptor >> 32)));
    }

    std::vector<std::pair<std::size_t, std::string>> opcodes;
    for (const auto& [name, count] : program.opcode_counts)
        opcodes.emplace_back(count, name);
    std::sort(opcodes.rbegin(), opcodes.rend());
    nlohmann::json opcode_json = nlohmann::json::array();
    for (const auto& [count, name] : opcodes)
        opcode_json.push_back({name, count});

    return {
        {"code_sha256", image.Sha256()},
        {"blocks", program.blocks.size()},
        {"guest_bytes", bytes},
        {"indirect_exits", indirect},
        {"interpret_terminals", interpret},
        {"exception_blocks", exception},
        {"rejected_pointer_seeds", program.rejected_pointers},
        {"zero_fill_skipped", program.zero_fill_skipped},
        {"by_seed", by_seed},
        {"by_segment", by_segment},
        {"by_mode", by_mode},
        {"coverage_blocks", coverage.size()},
        {"coverage_found", covered},
        {"coverage_missing_sample", missing},
        {"opcodes", opcode_json},
    };
}

DiscoverOptions Options(const Arguments& args) {
    DiscoverOptions options;
    options.scan_pointers = args.scan;
    for (const auto& path : args.coverage) {
        auto blocks = ReadCoverageBlocks(path);
        options.coverage.insert(options.coverage.end(), blocks.begin(), blocks.end());
    }
    return options;
}

int Emit(const Arguments& args) {
    const Image image{args.work};
    const Program program = Discover(image, Options(args));
    const EmitStats stats = EmitProgram(image, program, args.output, args.shards);
    nlohmann::json summary{{"discovered", program.blocks.size()},
                           {"emitted", stats.emitted},
                           {"unsupported", stats.unsupported}};
    std::ofstream(args.output / "emit-report.json") << summary.dump(2) << '\n';
    fmt::print("{}\n", summary.dump(2));
    return 0;
}

int Dump(const Arguments& args) {
    const Image image{args.work};
    const std::uint64_t descriptor = std::stoull(args.output.string(), nullptr, 16);
    const auto block = TranslateBlock(image, descriptor);
    fmt::print("{}\n", Dynarmic::IR::DumpBlock(block));
    EmittedBlock emitted;
    std::string unsupported;
    if (EmitBlock(block, {}, emitted, unsupported))
        fmt::print("{}", emitted.code);
    else
        fmt::print("unsupported: {}\n", unsupported);
    return 0;
}

int Analyze(const Arguments& args) {
    const Image image{args.work};
    const DiscoverOptions options = Options(args);
    const Program program = Discover(image, options);
    WriteBlocks(args.work / "aot-blocks.txt", program);
    const auto report = Report(image, program, options.coverage);
    std::ofstream(args.work / "aot-report.json") << report.dump(2) << '\n';
    auto summary = report;
    summary.erase("opcodes");
    summary["distinct_opcodes"] = report["opcodes"].size();
    fmt::print("{}\n", summary.dump(2));
    return 0;
}

void Usage() {
    std::fputs("usage: dekui_aot analyze <work> [--coverage blocks.txt]... [--no-scan]\n"
               "       dekui_aot emit <work> <output> [--coverage blocks.txt]... [--no-scan] [--shards N]\n",
               stderr);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        Usage();
        return 2;
    }
    Arguments args;
    args.command = argv[1];
    args.work = argv[2];
    int first_option = 3;
    if (args.command == "emit" || args.command == "dump") {
        if (argc < 4) {
            Usage();
            return 2;
        }
        args.output = argv[3];
        first_option = 4;
    }
    for (int i = first_option; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--coverage" && i + 1 < argc)
            args.coverage.push_back(argv[++i]);
        else if (arg == "--no-scan")
            args.scan = false;
        else if (arg == "--shards" && i + 1 < argc)
            args.shards = std::max(1, std::stoi(argv[++i]));
        else {
            Usage();
            return 2;
        }
    }
    try {
        if (args.command == "analyze")
            return Analyze(args);
        if (args.command == "emit")
            return Emit(args);
        if (args.command == "dump")
            return Dump(args);
    } catch (const std::exception& error) {
        fmt::print(stderr, "dekui_aot: {}\n", error.what());
        return 1;
    }
    Usage();
    return 2;
}
