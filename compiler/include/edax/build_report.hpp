#pragma once
// What `edaxc build`, `run` and `test` print about a project build (docs/design/build/build-output.md):
// a checklist with a line per target (and for build.ed and the packages), each target's details
// (its source files, ...) when asked for, the diagnostics in full, and a last line. On a terminal
// the lines of the targets still running are live: rewritten in place under what is finished.
// Everything also goes to build/<mode>/.edax/logs/build.log.
//
// The reporter is thread-safe: C/C++ units finish on worker threads.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace edax {

enum class Progress { automatic, live, lines, quiet };

struct ReportOptions {
    Progress progress = Progress::automatic;
    // Details shown on the console for every target ("sources", "units", "commands",
    // "packages", "toolchain", "timings"); build.log always has them all.
    std::set<std::string>                        details;
    std::map<std::string, std::set<std::string>> target_details;   // g.details(t, ...)
    bool                                         details_from_command_line = false;
    bool                                         log_file  = true;
    unsigned                                     keep_logs = 5;
    // The lines, as patterns (see build_report.cpp for the placeholders).
    std::string target_line;
    std::string done_line;
    std::string source_row;
    std::string last_line;
    std::string failed_line;
    bool        color = false;
};

// The default patterns.
std::string default_target_line();
std::string default_done_line();
std::string default_source_row();
std::string default_last_line();
std::string default_failed_line();

// A source file of a target: a row of its `sources` table.
struct SourceInfo {
    std::string path;    // as shown: relative to the target's root
    std::size_t lines = 0;
    std::size_t bytes = 0;
};

// "1.5 KiB"
std::string size_text_of(std::size_t bytes);

// Lines and bytes of the file at `path` (zeros when it can't be read).
SourceInfo source_info(const std::filesystem::path& path, const std::string& shown);

class BuildReport {
public:
    enum class State { waiting, running, done, up_to_date, failed, skipped };

    // `out_tty`: whether `out` is a terminal (live lines only there). `log`: build.log's path.
    BuildReport(ReportOptions options, std::ostream& out, std::ostream& err, bool out_tty, std::filesystem::path log,
                std::filesystem::path timings, std::string title);
    ~BuildReport();
    BuildReport(const BuildReport&)            = delete;
    BuildReport& operator=(const BuildReport&) = delete;

    // A line of the checklist, in the order they are added: "packages", "build.ed", the targets.
    std::size_t add_item(const std::string& name, const std::string& kind);
    void        set_waiting_text(std::size_t item, const std::string& text);
    void        start(std::size_t item);
    // The item's source files: each is a unit and a row of its `sources` table.
    void        set_sources(std::size_t item, const std::vector<SourceInfo>& sources);
    // Another unit (a phase: "check", "code", "link"); returns its index.
    std::size_t add_unit(std::size_t item, const std::string& name);
    void        unit_start(std::size_t item, std::size_t unit);
    void        unit_done(std::size_t item, std::size_t unit, bool ok = true);
    // A unit that needed no work (an up-to-date object).
    void        unit_up_to_date(std::size_t item, std::size_t unit);
    void        add_diagnostics(std::size_t item, std::size_t errors, std::size_t warnings);
    // The item is finished: its line, and its details, become permanent.
    void        finish(std::size_t item, State state, const std::string& summary, const std::string& output = {}, double seconds = -1);
    // Whether the item was started (a target built on the way, as a dependency, is added late).
    bool        started(std::size_t item) const;
    // The item of a name, if there is one.
    std::optional<std::size_t> find(const std::string& name) const;
    // Swaps the streams' buffers so their text goes through print(); undone by the destructor.
    void        capture(std::ostream& out, std::ostream& err);
    // Text above the live lines, kept (diagnostics, messages); also in build.log.
    void        print(const std::string& text, bool to_err);
    // Text for the console only (what build.log has in full, written there with log_only).
    void        print_console(const std::string& text, bool to_err);
    // Text for build.log only (commands, details the console doesn't show).
    void        log_only(const std::string& text);
    // The last line; returns it.
    void        end(bool ok);

    // Streams whose text goes through print(): what the rest of the driver writes.
    std::ostream& out_stream() { return *out_proxy_; }
    std::ostream& err_stream() { return *err_proxy_; }

    bool        shows(const std::string& item_name, const std::string& detail) const;
    std::string log_path_text() const;

private:
    struct Unit {
        std::string                           name;
        State                                 state = State::waiting;
        std::chrono::steady_clock::time_point started;
        double                                seconds = 0;
        bool                                  is_source = false;
        std::size_t                           lines = 0, bytes = 0;
    };
    struct Item {
        std::string                           name, kind, waiting_text = "waiting", summary, output;
        State                                 state = State::waiting;
        std::chrono::steady_clock::time_point started;
        double                                seconds = 0;
        std::vector<Unit>                     units;
        std::size_t                           sources = 0;   // the first `sources` units are source files
        std::size_t                           errors = 0, warnings = 0;
    };

    void        redraw_locked();
    // One frame: `above` (stdout) / `above_err` printed over the live area, the live area under it.
    void        paint_locked(const std::string& above, const std::string& above_err, bool force);
    std::vector<std::string> frame_locked() const;
    std::string to_top_locked() const;
    void        write_out_locked(const std::string& text);
    void        erase_locked();
    void        emit_locked(const std::string& text, bool to_err, bool to_log = true);   // above the live area
    std::string item_line(const Item& it) const;
    std::string table(const Item& it, bool running_rows_only, bool marks) const;
    std::string render(const std::string& pattern, const std::map<std::string, std::string>& values, std::size_t star_width) const;
    std::string mark(State s) const;
    double      percent(const Item& it) const;
    std::string running_text(const Item& it) const;
    bool        live() const { return progress_ == Progress::live; }
    void        draw_loop();

    ReportOptions                                 o_;
    Progress                                      progress_;
    std::ostream&                                 out_;
    std::ostream&                                 err_;
    std::ofstream                                 log_;
    std::filesystem::path                         log_path_;
    std::filesystem::path                         timings_path_;
    std::map<std::string, double>                 previous_;   // "item/unit" -> seconds of the last build
    std::string                                   title_;
    std::vector<Item>                             items_;
    mutable std::mutex                            lock_;
    std::size_t                                   drawn_rows_ = 0;
    std::string                                   last_frame_;
    bool                                          cursor_hidden_ = false;
    int                                           out_fd_ = -1;   // write(2) target for whole frames
    bool                                          dirty_ = false;
    bool                                          stop_ = false;
    std::condition_variable                       wake_;
    std::thread                                   drawer_;
    std::chrono::steady_clock::time_point         began_;
    std::unique_ptr<std::ostream>                 out_proxy_, err_proxy_;
    std::unique_ptr<std::streambuf>               out_buf_, err_buf_;
    std::ostream*                                 captured_out_ = nullptr;
    std::ostream*                                 captured_err_ = nullptr;
    std::streambuf*                               saved_out_ = nullptr;
    std::streambuf*                               saved_err_ = nullptr;
};

} // namespace edax
