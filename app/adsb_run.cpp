// adsb_run - load a GNU Radio 4 graph file, run it, and report what the graph was asked to report.
//
// Nothing here knows what ADS-B is. No block is named, no setting is named and no chain is built: the file names
// the blocks, the plugin directories supply them, and the SDK's own loader puts them together. That is what the
// two halves of the split receiver need -- each is a graph file under `graphs/`, and running one is this program
// and that file, with no program that has to be compiled again when the chain in the file changes.
//
// A graph file is refused by the loader, and its message is printed as it arrives.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>

namespace {

std::atomic<bool> gStop{false};

extern "C" void onSignal(int) { gStop.store(true, std::memory_order_relaxed); }

struct Options {
    std::string              graph{};       ///< the graph file, or "-" for standard input
    std::vector<std::string> pluginDirs{};  ///< the SDK's own first, then every --plugin-dir in order
    std::vector<std::string> show{};        ///< block names whose settings are printed when the run ends
    double                   seconds = 0.0; ///< run bound; 0 runs until the graph ends or the user interrupts
    bool                     verbose = false;
};

void usage() {
    std::println(stderr, "usage: adsb_run --graph FILE [--plugin-dir DIR]... [--seconds S] [--show NAME]... [--verbose]");
    std::println(stderr, "  --graph FILE     the graph file to run; - reads it from standard input");
    std::println(stderr, "  --plugin-dir DIR another directory of block libraries, repeatable; the SDK's own is always searched");
    std::println(stderr, "  --seconds S      stop after S seconds; without it the run ends when the graph does");
    std::println(stderr, "  --show NAME      print the readable settings of the block named NAME when the run ends, repeatable");
    std::println(stderr, "  --verbose        list what the plugin directories loaded and the registry keys they brought");
}

/// @brief The command line, or nothing when it is not usable. Everything it refuses is reported here.
[[nodiscard]] std::optional<Options> parse(int argc, char** argv) {
    Options options;
    options.pluginDirs.emplace_back(GR4ADSB_SDK_PLUGIN_DIR);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (i + 1 >= argc) {
            if (arg != "--help") {
                std::println(stderr, "adsb_run: {} needs a value", arg);
            }
            return std::nullopt;
        }
        const std::string_view value(argv[++i]);
        if (arg == "--graph") {
            options.graph = std::string(value);
        } else if (arg == "--plugin-dir") {
            options.pluginDirs.emplace_back(value);
        } else if (arg == "--show") {
            options.show.emplace_back(value);
        } else if (arg == "--seconds") {
            options.seconds = std::strtod(std::string(value).c_str(), nullptr);
            if (!(options.seconds > 0.0)) {
                std::println(stderr, "adsb_run: --seconds takes a positive number of seconds");
                return std::nullopt;
            }
        } else {
            std::println(stderr, "adsb_run: unknown argument {}", arg);
            return std::nullopt;
        }
    }
    if (options.graph.empty()) {
        std::println(stderr, "adsb_run: --graph names the file to run");
        return std::nullopt;
    }
    return options;
}

/// @brief The graph file's text, or nothing when it could not be read.
[[nodiscard]] std::optional<std::string> readGraph(const std::string& path) {
    if (path == "-") {
        std::ostringstream text;
        text << std::cin.rdbuf();
        return text.str();
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::println(stderr, "adsb_run: {} could not be read", path);
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

/// @brief List what the directories loaded and which registry keys they brought.
void report(const gr::PluginLoader& loader, const std::vector<std::string>& before) {
    for (const gr::PluginLoader::BlockLibrary& library : loader.blockLibraries()) {
        std::println(stderr, "adsb_run: loaded {} ({} registration(s))", library.file, library.nBlockRegistrations);
    }
    for (const auto& [file, reason] : loader.failedPlugins()) {
        std::println(stderr, "adsb_run: {} did not load: {}", file, reason);
    }
    for (const std::string& file : loader.skippedFiles()) {
        std::println(stderr, "adsb_run: {} was not opened; its name only reads as a shared object", file);
    }
    std::vector<std::string> after = gr::globalBlockRegistry().keys();
    std::ranges::sort(after);
    std::vector<std::string> added;
    std::ranges::set_difference(after, before, std::back_inserter(added));
    std::println(stderr, "adsb_run: the registry gained {} key(s):", added.size());
    for (const std::string& key : added) {
        std::println(stderr, "adsb_run:   {}", key);
    }
}

/// @brief Print one block's readable settings, one `name: key = value` line each, in key order.
///
/// A settings map holds what the last refresh put there, and the framework refreshes one only when a settings change
/// has been applied. A block whose observables move while it runs -- a tracked level, a counter -- therefore reports
/// what it started with unless it refreshes itself. The run is over by the time this is called, so asking for a
/// refresh here is safe, and it is what makes such a block's observable readable at all.
void showSettings(gr::BlockModel& block) {
    block.settings().updateActiveParameters();
    std::vector<std::pair<std::string, std::string>> lines;
    for (const auto& [key, value] : block.settings().get()) {
        lines.emplace_back(std::string(key.begin(), key.end()), std::format("{}", value));
    }
    std::ranges::sort(lines);
    for (const auto& [key, value] : lines) {
        std::println("{}: {} = {}", block.name(), key, value);
    }
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    const std::optional<Options> parsed = parse(argc, argv);
    if (!parsed.has_value()) {
        usage();
        return 2;
    }
    const Options& options = *parsed;

    const std::optional<std::string> document = readGraph(options.graph);
    if (!document.has_value()) {
        return 1;
    }

    std::vector<std::string> before = gr::globalBlockRegistry().keys();
    std::ranges::sort(before);
    gr::PluginLoader loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), options.pluginDirs);
    if (options.verbose) {
        report(loader, before);
    }

    // The loader's refusal is the one a graph file earns: a registry key nothing supplies, a setting a block does
    // not declare, a value of the wrong type, a port that is not there. It is printed as it arrives.
    std::optional<gr::meta::indirect<gr::Graph>> graph;
    try {
        graph.emplace(gr::loadGrc(loader, *document));
    } catch (const std::exception& error) {
        std::println(stderr, "adsb_run: {} did not load: {}", options.graph, error.what());
        return 1;
    }

    // The blocks named by --show are found before the graph is handed to the scheduler, because the handles stay
    // valid over the move and the graph itself does not.
    std::vector<std::shared_ptr<gr::BlockModel>> shown;
    bool                                         allFound = true;
    for (const std::string& wanted : options.show) {
        std::shared_ptr<gr::BlockModel> found;
        gr::graph::forEachBlock<gr::block::Category::NormalBlock>(**graph, [&found, &wanted](const std::shared_ptr<gr::BlockModel>& block) {
            if (block->name() == wanted) {
                found = block;
            }
        });
        if (found == nullptr) {
            std::println(stderr, "adsb_run: the graph holds no block named {}", wanted);
            allFound = false;
            continue;
        }
        shown.push_back(std::move(found));
    }
    if (!allFound) {
        return 1;
    }

    std::signal(SIGINT, onSignal);

    gr::scheduler::Simple<> scheduler;
    if (!scheduler.exchange(std::move(**graph)).has_value()) {
        std::println(stderr, "adsb_run: the scheduler refused the graph");
        return 1;
    }

    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
    std::thread       runner([&scheduler, &finished, &failed] {
        if (const auto result = scheduler.runAndWait(); !result.has_value()) {
            std::println(stderr, "adsb_run: the graph stopped: {}", result.error().message);
            failed.store(true, std::memory_order_relaxed);
        }
        finished.store(true, std::memory_order_relaxed);
    });

    const auto start = std::chrono::steady_clock::now();
    while (!finished.load(std::memory_order_relaxed) && !gStop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (options.seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= options.seconds) {
            break;
        }
    }
    const bool endedItself = finished.load(std::memory_order_relaxed);
    if (!endedItself) {
        scheduler.requestStop();
    }
    runner.join();

    for (const std::shared_ptr<gr::BlockModel>& block : shown) {
        showSettings(*block);
    }
    std::println(stderr, "adsb_run: {}", endedItself ? "the graph ended on its own" : "stopped before the graph ended");
    return failed.load(std::memory_order_relaxed) ? 1 : 0;
}
