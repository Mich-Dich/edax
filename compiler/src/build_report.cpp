// The build's console output and build.log (see build_report.hpp).
//
// Patterns: `{name}`, `{name:<N}` (left-aligned in N columns), `{name:>N}` (right-aligned),
// `{name:<*}` (the table's widest value), `{{` and `}}`. The placeholders:
//
//   target_line   mark target bar percent done total running elapsed      (a target still building)
//   done_line     mark target summary elapsed output                       (a finished target)
//   source_row    mark lines size file time                                (a row of the sources table)
//   last_line     targets up_to_date elapsed errors warnings log           (the build worked)
//   failed_line   errors warnings log                                      (it didn't)
//
// `{bar:N}` is a bar of N columns; `{errors}` and `{warnings}` are counts with their noun
// ("1 error", "2 warnings").

#include "edax/build_report.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <cmath>
#include <format>
#include <functional>
#include <sstream>
#include <streambuf>
#include <sys/ioctl.h>
#include <unistd.h>

namespace edax {

std::string default_target_line() { return " {mark} {target:<*} {bar:40} {done:>4}/{total:<4} {elapsed:>8}  {running}"; }
std::string default_done_line() { return " {mark} {target:<*} {summary:<48} {elapsed:>8}  {output}"; }
std::string default_source_row() { return "     {mark} {lines:>12} {size:>10}   {file:<*}  {time:>7}"; }
std::string default_last_line() {
    return "built {targets} in {elapsed} ({up_to_date} up to date): {errors}, {warnings}   details: {log}";
}
std::string default_failed_line() { return "build failed: {errors}, {warnings}   details: {log}"; }

SourceInfo source_info(const std::filesystem::path& path, const std::string& shown) {
    SourceInfo    s{shown, 0, 0};
    std::ifstream in(path, std::ios::binary);
    if (!in) return s;
    char buffer[65536];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        std::streamsize n = in.gcount();
        s.bytes += static_cast<std::size_t>(n);
        s.lines += static_cast<std::size_t>(std::count(buffer, buffer + n, '\n'));
    }
    return s;
}

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string seconds_text(double s) {
    if (s < 60) return std::format("{:.1f} s", s);
    auto whole = static_cast<long>(s);
    return std::format("{} m {:02} s", whole / 60, whole % 60);
}

std::string size_text(std::size_t bytes) {
    double b = static_cast<double>(bytes);
    if (b < 1024) return std::format("{} B", bytes);
    if (b < 1024 * 1024) return std::format("{:.2f} KiB", b / 1024);
    return std::format("{:.2f} MiB", b / (1024 * 1024));
}

std::string count_text(std::size_t n, std::string_view noun) { return std::format("{} {}{}", n, noun, n == 1 ? "" : "s"); }

// Visible width: UTF-8 characters, escape sequences don't count.
std::size_t display_width(std::string_view s) {
    std::size_t width  = 0;
    bool        escape = false;
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (escape) {
            if (c >= 0x40 && c <= 0x7E && c != '[') escape = false;
        } else if (c == 0x1B) {
            escape = true;
        } else if ((c & 0xC0) != 0x80) {
            ++width;
        }
    }
    return width;
}

// `s` shortened to `columns` visible characters (escape sequences kept whole).
std::string cut(std::string_view s, std::size_t columns) {
    std::string out;
    std::size_t width  = 0;
    bool        escape = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (escape) {
            if (c >= 0x40 && c <= 0x7E && c != '[') escape = false;
        } else if (c == 0x1B) {
            escape = true;
        } else if ((c & 0xC0) != 0x80) {
            if (width == columns) break;
            ++width;
        }
        out += static_cast<char>(c);
    }
    if (out.find('\x1b') != std::string::npos) out += "\x1b[0m";
    return out;
}

std::string pad(const std::string& s, std::size_t width, bool right) {
    std::size_t w = display_width(s);
    if (w >= width) return s;
    return right ? std::string(width - w, ' ') + s : s + std::string(width - w, ' ');
}

struct TerminalSize {
    std::size_t columns = 120, rows = 40;
};

TerminalSize terminal_size() {
    winsize w{};
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return TerminalSize{w.ws_col, w.ws_row > 0 ? w.ws_row : std::size_t{40}};
    return {};
}

// A stream buffer that hands complete lines to a function.
class LineBuffer : public std::streambuf {
public:
    explicit LineBuffer(std::function<void(const std::string&)> sink) : sink_(std::move(sink)) {}
    ~LineBuffer() override {
        if (!pending_.empty()) sink_(pending_ + "\n");
    }

protected:
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        put(static_cast<char>(c));
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) put(s[i]);
        return n;
    }

private:
    void put(char c) {
        pending_ += c;
        if (c == '\n') {
            sink_(pending_);
            pending_.clear();
        }
    }
    std::function<void(const std::string&)> sink_;
    std::string                              pending_;
};

const char* green  = "\x1b[32m";
const char* red    = "\x1b[31m";
const char* cyan   = "\x1b[36m";
const char* dim    = "\x1b[2m";
const char* reset  = "\x1b[0m";

} // namespace

std::string size_text_of(std::size_t bytes) { return size_text(bytes); }

BuildReport::BuildReport(ReportOptions options, std::ostream& out, std::ostream& err, bool out_tty, std::filesystem::path log,
                         std::filesystem::path timings, std::string title)
    : o_(std::move(options)), out_(out), err_(err), log_path_(std::move(log)), timings_path_(std::move(timings)),
      title_(std::move(title)), began_(Clock::now()) {
    namespace fs = std::filesystem;
    out_fd_   = out.rdbuf() == std::cout.rdbuf() ? 1 : out.rdbuf() == std::cerr.rdbuf() ? 2 : -1;
    progress_ = o_.progress == Progress::automatic ? (out_tty ? Progress::live : Progress::lines) : o_.progress;
    if (progress_ == Progress::live && !out_tty) progress_ = Progress::lines;
    if (!o_.color || progress_ != Progress::live) o_.color = o_.color && out_tty;
    if (o_.target_line.empty()) o_.target_line = default_target_line();
    if (o_.done_line.empty()) o_.done_line = default_done_line();
    if (o_.source_row.empty()) o_.source_row = default_source_row();
    if (o_.last_line.empty()) o_.last_line = default_last_line();
    if (o_.failed_line.empty()) o_.failed_line = default_failed_line();
    std::error_code ec;
    if (o_.log_file) {
        fs::create_directories(log_path_.parent_path(), ec);
        // The last `keep_logs` logs: build.1.log is the newest of them.
        auto numbered = [&](unsigned k) {
            fs::path p = log_path_;
            p.replace_extension(std::format("{}.log", k));
            return p;
        };
        if (o_.keep_logs > 0 && fs::exists(log_path_, ec)) {
            fs::remove(numbered(o_.keep_logs), ec);
            for (unsigned k = o_.keep_logs; k-- > 1;) fs::rename(numbered(k), numbered(k + 1), ec);
            fs::rename(log_path_, numbered(1), ec);
        }
        log_.open(log_path_);
    }
    if (std::ifstream in(timings_path_); in) {
        std::string line;
        while (std::getline(in, line)) {
            std::size_t tab = line.rfind('\t');
            if (tab == std::string::npos) continue;
            try {
                previous_[line.substr(0, tab)] = std::stod(line.substr(tab + 1));
            } catch (...) {
            }
        }
    }
    out_buf_   = std::make_unique<LineBuffer>([this](const std::string& s) { print(s, false); });
    err_buf_   = std::make_unique<LineBuffer>([this](const std::string& s) { print(s, true); });
    out_proxy_ = std::make_unique<std::ostream>(out_buf_.get());
    err_proxy_ = std::make_unique<std::ostream>(err_buf_.get());
    if (progress_ != Progress::quiet) {
        std::lock_guard<std::mutex> guard(lock_);
        emit_locked(title_ + "\n", false);
    }
    if (live()) drawer_ = std::thread([this] { draw_loop(); });
}

BuildReport::~BuildReport() {
    if (captured_out_) captured_out_->rdbuf(saved_out_);
    if (captured_err_) captured_err_->rdbuf(saved_err_);
    {
        std::lock_guard<std::mutex> guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (drawer_.joinable()) drawer_.join();
    out_proxy_->flush();
    err_proxy_->flush();
    err_proxy_.reset();
    out_proxy_.reset();
    err_buf_.reset();
    out_buf_.reset();
    std::lock_guard<std::mutex> guard(lock_);
    erase_locked();
    out_.flush();
}

std::string BuildReport::log_path_text() const {
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(log_path_, std::filesystem::current_path(ec), ec);
    if (!ec && !rel.empty() && !rel.string().starts_with("..")) return rel.string();
    return log_path_.string();
}

bool BuildReport::shows(const std::string& item_name, const std::string& detail) const {
    if (o_.details.count(detail)) return true;
    if (o_.details_from_command_line) return false;
    auto it = o_.target_details.find(item_name);
    return it != o_.target_details.end() && it->second.count(detail);
}

std::size_t BuildReport::add_item(const std::string& name, const std::string& kind) {
    std::lock_guard<std::mutex> guard(lock_);
    Item fresh;
    fresh.name = name;
    fresh.kind = kind;
    items_.push_back(std::move(fresh));
    dirty_ = true;
    return items_.size() - 1;
}

void BuildReport::set_waiting_text(std::size_t item, const std::string& text) {
    std::lock_guard<std::mutex> guard(lock_);
    items_[item].waiting_text = text;
    dirty_ = true;
}

void BuildReport::start(std::size_t item) {
    std::lock_guard<std::mutex> guard(lock_);
    items_[item].state   = State::running;
    items_[item].started = Clock::now();
    dirty_               = true;
}

void BuildReport::set_sources(std::size_t item, const std::vector<SourceInfo>& sources) {
    std::lock_guard<std::mutex> guard(lock_);
    Item& it = items_[item];
    std::vector<Unit> rest(it.units.begin() + static_cast<std::ptrdiff_t>(it.sources), it.units.end());
    it.units.clear();
    for (const SourceInfo& src : sources) {
        Unit u;
        u.name      = src.path;
        u.is_source = true;
        u.lines     = src.lines;
        u.bytes     = src.bytes;
        it.units.push_back(std::move(u));
    }
    it.sources = sources.size();
    it.units.insert(it.units.end(), rest.begin(), rest.end());
    dirty_ = true;
}

std::size_t BuildReport::add_unit(std::size_t item, const std::string& name) {
    std::lock_guard<std::mutex> guard(lock_);
    Unit u;
    u.name = name;
    items_[item].units.push_back(std::move(u));
    dirty_ = true;
    return items_[item].units.size() - 1;
}

void BuildReport::unit_start(std::size_t item, std::size_t unit) {
    std::lock_guard<std::mutex> guard(lock_);
    Unit& u   = items_[item].units[unit];
    u.state   = State::running;
    u.started = Clock::now();
    dirty_    = true;
}

void BuildReport::unit_done(std::size_t item, std::size_t unit, bool ok) {
    std::lock_guard<std::mutex> guard(lock_);
    Unit& u = items_[item].units[unit];
    if (u.state == State::running) u.seconds = seconds_since(u.started);
    u.state = ok ? State::done : State::failed;
    dirty_  = true;
}

void BuildReport::unit_up_to_date(std::size_t item, std::size_t unit) {
    std::lock_guard<std::mutex> guard(lock_);
    items_[item].units[unit].state = State::up_to_date;
    dirty_                         = true;
}

void BuildReport::add_diagnostics(std::size_t item, std::size_t errors, std::size_t warnings) {
    std::lock_guard<std::mutex> guard(lock_);
    items_[item].errors += errors;
    items_[item].warnings += warnings;
}

std::string BuildReport::mark(State s) const {
    auto paint = [&](const char* color, const char* text) { return o_.color ? std::string(color) + text + reset : std::string(text); };
    switch (s) {
    case State::waiting: return paint(dim, "·");
    case State::running: return paint(cyan, "▸");
    case State::done: return paint(green, "✓");
    case State::up_to_date: return paint(dim, "=");
    case State::failed: return paint(red, "✗");
    case State::skipped: return paint(dim, "-");
    }
    return " ";
}

double BuildReport::percent(const Item& it) const {
    if (it.units.empty()) return 0;
    // Weighted by the last build's times when every unit has one.
    double all = 0, done = 0;
    bool   known = true;
    for (const Unit& u : it.units) {
        auto p = previous_.find(it.name + "/" + u.name);
        if (p == previous_.end()) {
            known = false;
            break;
        }
        double w = std::max(p->second, 0.001);
        all += w;
        if (u.state == State::done || u.state == State::up_to_date) done += w;
    }
    if (known && all > 0) return done / all;
    std::size_t finished = 0;
    for (const Unit& u : it.units) finished += u.state == State::done || u.state == State::up_to_date;
    return static_cast<double>(finished) / static_cast<double>(it.units.size());
}

std::string BuildReport::running_text(const Item& it) const {
    std::vector<std::pair<double, std::string>> running;
    for (const Unit& u : it.units) {
        if (u.state == State::running) running.emplace_back(seconds_since(u.started), std::filesystem::path(u.name).filename().string());
    }
    std::sort(running.begin(), running.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text;
    for (const auto& [s, name] : running) text += (text.empty() ? "" : ", ") + std::format("{} {}", name, seconds_text(s));
    return text;
}

std::string BuildReport::render(const std::string& pattern, const std::map<std::string, std::string>& values, std::size_t star) const {
    std::string out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '{' && i + 1 < pattern.size() && pattern[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        if (c == '}' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            out += '}';
            ++i;
            continue;
        }
        if (c != '{') {
            out += c;
            continue;
        }
        std::size_t close = pattern.find('}', i);
        if (close == std::string::npos) {
            out += pattern.substr(i);
            break;
        }
        std::string field = pattern.substr(i + 1, close - i - 1);
        i                 = close;
        std::string spec;
        if (std::size_t colon = field.find(':'); colon != std::string::npos) {
            spec  = field.substr(colon + 1);
            field = field.substr(0, colon);
        }
        bool        right = false;
        std::size_t width = 0;
        if (!spec.empty() && (spec[0] == '<' || spec[0] == '>')) {
            right = spec[0] == '>';
            spec  = spec.substr(1);
        }
        if (spec == "*") width = star;
        else if (!spec.empty() && std::all_of(spec.begin(), spec.end(), [](char d) { return d >= '0' && d <= '9'; })) width = std::stoul(spec);
        if (field == "bar") {
            double      p      = values.count("bar") ? std::stod(values.at("bar")) : 0;
            std::size_t n      = width ? width : 40;
            std::size_t filled = static_cast<std::size_t>(std::lround(p * static_cast<double>(n)));
            std::string bar;
            for (std::size_t k = 0; k < filled; ++k) bar += "━";
            if (filled < n) {
                bar += o_.color ? std::string(dim) + "╺" : std::string("╺");
                for (std::size_t k = filled + 1; k < n; ++k) bar += o_.color ? "━" : "─";
                if (o_.color) bar += reset;
            }
            out += bar;
            continue;
        }
        auto it = values.find(field);
        out += pad(it == values.end() ? std::string() : it->second, width, right);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string BuildReport::item_line(const Item& it) const {
    std::size_t width = 14;   // the names line up
    for (const Item& other : items_) width = std::max(width, display_width(other.name));
    if (it.state == State::waiting) return std::format(" {} {} {}", mark(it.state), pad(it.name, width, false), it.waiting_text);
    std::size_t done = 0;
    for (const Unit& u : it.units) done += u.state == State::done || u.state == State::up_to_date;
    double elapsed = it.state == State::running ? seconds_since(it.started) : it.seconds;
    if (it.state == State::running) {
        return render(o_.target_line,
                      {{"mark", mark(it.state)}, {"target", it.name}, {"bar", std::to_string(percent(it))},
                       {"percent", std::format("{:.0f}", percent(it) * 100)}, {"done", std::to_string(done)},
                       {"total", it.units.empty() ? std::string("?") : std::to_string(it.units.size())},
                       {"running", running_text(it)}, {"elapsed", seconds_text(elapsed)}},
                      width);
    }
    return render(o_.done_line,
                  {{"mark", mark(it.state)}, {"target", it.name}, {"summary", it.summary},
                   {"elapsed", it.state == State::up_to_date || it.state == State::skipped ? std::string() : seconds_text(elapsed)},
                   {"output", it.output}},
                  width);
}

std::string BuildReport::table(const Item& it, bool running_rows_only, bool marks) const {
    std::size_t total = 0, widest = 0;
    for (std::size_t k = 0; k < it.sources; ++k) {
        total += it.units[k].bytes;
        widest = std::max(widest, display_width(it.units[k].name));
    }
    std::string text = std::format("    source file details: {}, total size {}\n", count_text(it.sources, "source file"), size_text(total));
    std::size_t line_width = 0;
    for (std::size_t k = 0; k < it.sources; ++k) line_width = std::max(line_width, std::to_string(it.units[k].lines).size());
    for (std::size_t k = 0; k < it.sources; ++k) {
        const Unit& u = it.units[k];
        if (running_rows_only && u.state != State::running) continue;
        double      s    = u.state == State::running ? seconds_since(u.started) : u.seconds;
        std::string time = u.state == State::running || u.state == State::done || u.state == State::failed ? seconds_text(s) : std::string();
        text += render(o_.source_row,
                       {{"mark", marks ? mark(u.state) : std::string(" ")}, {"lines", count_text(u.lines, "line")},
                        {"size", size_text(u.bytes)}, {"file", u.name}, {"time", time}},
                       widest) +
                "\n";
    }
    return text;
}

// Live drawing without flicker: the live area is never erased and drawn again in two steps.
// The cursor goes back to the area's first row, every row is overwritten in place and cleared
// to its end (ESC[K), rows left over below are cleared last (ESC[J), all in ONE write(2), inside
// a synchronized update (ESC[?2026h ... l: the terminal shows the whole frame at once; terminals
// without it ignore the codes). The cursor is hidden while the area is live.

void BuildReport::write_out_locked(const std::string& text) {
    if (text.empty()) return;
    out_.flush();
    if (out_fd_ < 0) {
        out_ << text;
        out_.flush();
        return;
    }
    std::fflush(nullptr);   // what went through stdio first stays first
    std::size_t done = 0;
    while (done < text.size()) {
        ssize_t n = ::write(out_fd_, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        done += static_cast<std::size_t>(n);
    }
}

std::string BuildReport::to_top_locked() const {
    if (drawn_rows_ == 0) return {};
    return drawn_rows_ > 1 ? std::format("\r\x1b[{}A", drawn_rows_ - 1) : std::string("\r");
}

std::vector<std::string> BuildReport::frame_locked() const {
    std::vector<std::string> rows;
    if (!live() || stop_) return rows;
    TerminalSize size = terminal_size();
    for (const Item& it : items_) {
        if (it.state != State::waiting && it.state != State::running) continue;
        rows.push_back(item_line(it));
        if (it.state == State::running && it.sources > 0 && shows(it.name, "sources")) {
            std::istringstream table_lines(table(it, false, true));
            std::vector<std::string> all;
            for (std::string l; std::getline(table_lines, l);) all.push_back(l);
            if (all.size() + rows.size() + 2 > size.rows) {   // too tall: the running rows only
                std::istringstream running(table(it, true, true));
                all.clear();
                for (std::string l; std::getline(running, l);) all.push_back(l);
            }
            rows.insert(rows.end(), all.begin(), all.end());
        }
    }
    std::size_t limit = size.rows > 2 ? size.rows - 1 : 1;
    if (rows.size() > limit) rows.resize(limit);
    for (std::string& r : rows) r = cut(r, size.columns > 1 ? size.columns - 1 : 1);
    return rows;
}

void BuildReport::paint_locked(const std::string& above, const std::string& above_err, bool force) {
    if (!live()) {   // plain lines: nothing to rewrite
        if (!above_err.empty()) {
            out_.flush();
            err_ << above_err;
            err_.flush();
        }
        if (!above.empty()) out_ << above;
        out_.flush();
        return;
    }
    std::vector<std::string> rows = frame_locked();
    std::string              frame;
    for (std::size_t k = 0; k < rows.size(); ++k) frame += (k ? "\x1b[K\n" : "") + rows[k];
    if (above.empty() && above_err.empty() && !force && frame == last_frame_ && drawn_rows_ == rows.size()) return;
    std::string text = "\x1b[?2026h";
    if (!cursor_hidden_ && !rows.empty()) {
        text += "\x1b[?25l";
        cursor_hidden_ = true;
    }
    text += to_top_locked();
    if (!above_err.empty()) {   // another stream: clear the area, write it, then the frame
        text += "\x1b[J";
        write_out_locked(text);
        err_ << above_err;
        err_.flush();
        text.clear();
    }
    for (std::size_t at = 0; at < above.size();) {   // overwrite the rows, clearing what's left of each
        std::size_t nl = above.find('\n', at);
        if (nl == std::string::npos) {
            text += above.substr(at) + "\x1b[K\r\n";
            break;
        }
        text += above.substr(at, nl - at) + "\x1b[K\n";
        at = nl + 1;
    }
    if (!rows.empty()) text += frame + "\x1b[K";
    text += "\x1b[J";
    if (rows.empty() && cursor_hidden_) {
        text += "\x1b[?25h";
        cursor_hidden_ = false;
    }
    text += "\x1b[?2026l";
    write_out_locked(text);
    drawn_rows_ = rows.size();
    last_frame_ = frame;
}

void BuildReport::erase_locked() {
    if (drawn_rows_ == 0 && !cursor_hidden_) return;
    std::string text = to_top_locked() + "\x1b[J";
    if (cursor_hidden_) text += "\x1b[?25h";
    cursor_hidden_ = false;
    write_out_locked(text);
    drawn_rows_ = 0;
    last_frame_.clear();
}

void BuildReport::redraw_locked() { paint_locked({}, {}, false); }

void BuildReport::emit_locked(const std::string& text, bool to_err, bool to_log) {
    if (to_log && log_.is_open()) {
        std::string plain;   // build.log has no colors
        bool        escape = false;
        for (char c : text) {
            if (escape) {
                if (c >= 0x40 && c <= 0x7E && c != '[') escape = false;
                continue;
            }
            if (c == '\x1b') {
                escape = true;
                continue;
            }
            plain += c;
        }
        log_ << plain;
    }
    if (progress_ == Progress::quiet && !to_err) return;
    if (to_err) paint_locked({}, text, true);
    else paint_locked(text, {}, true);
}

void BuildReport::print(const std::string& text, bool to_err) {
    std::lock_guard<std::mutex> guard(lock_);
    emit_locked(text, to_err);
}

void BuildReport::print_console(const std::string& text, bool to_err) {
    std::lock_guard<std::mutex> guard(lock_);
    emit_locked(text, to_err, false);
}

void BuildReport::log_only(const std::string& text) {
    std::lock_guard<std::mutex> guard(lock_);
    if (log_.is_open()) log_ << text << (text.ends_with('\n') ? "" : "\n");
}

bool BuildReport::started(std::size_t item) const {
    std::lock_guard<std::mutex> guard(lock_);
    return items_[item].state != State::waiting;
}

std::optional<std::size_t> BuildReport::find(const std::string& name) const {
    std::lock_guard<std::mutex> guard(lock_);
    for (std::size_t k = 0; k < items_.size(); ++k) {
        if (items_[k].name == name) return k;
    }
    return std::nullopt;
}

void BuildReport::capture(std::ostream& out, std::ostream& err) {
    captured_out_ = &out;
    captured_err_ = &err;
    saved_out_    = out.rdbuf(out_buf_.get());
    saved_err_    = err.rdbuf(err_buf_.get());
}

void BuildReport::finish(std::size_t item, State state, const std::string& summary, const std::string& output, double seconds) {
    std::lock_guard<std::mutex> guard(lock_);
    Item& it = items_[item];
    if (it.state == State::running) it.seconds = seconds_since(it.started);
    if (seconds >= 0) it.seconds = seconds;
    it.state   = state;
    it.output  = output;
    std::string s = summary;
    if (s.empty() && state == State::done && !it.units.empty()) {   // "45 units, 556.7 KiB"
        std::size_t bytes = 0;
        for (std::size_t k = 0; k < it.sources; ++k) bytes += it.units[k].bytes;
        s = count_text(it.units.size(), "unit") + (it.sources ? ", " + size_text(bytes) : std::string());
    }
    if (state != State::up_to_date && state != State::skipped) {
        if (it.errors) s += (s.empty() ? "" : ", ") + count_text(it.errors, "error");
        if (it.warnings) s += (s.empty() ? "" : ", ") + count_text(it.warnings, "warning");
    }
    it.summary = s;
    for (Unit& u : it.units) {
        if (u.state == State::running) u.seconds = seconds_since(u.started), u.state = state == State::failed ? State::failed : State::done;
    }
    // The finished line and its details: permanent on the console, everything in build.log.
    std::string console = item_line(it) + "\n";
    std::string logged  = console;
    if (it.sources > 0 && state != State::up_to_date && state != State::skipped) {
        std::string t = table(it, false, true);
        logged += t;
        if (shows(it.name, "sources")) console += t;
    }
    std::string steps;
    for (std::size_t k = it.sources; k < it.units.size(); ++k) {
        const Unit& u = it.units[k];
        std::string row = std::format("     {} {}  {}", mark(u.state), pad(u.name, 12, false), u.state == State::done || u.state == State::failed ? seconds_text(u.seconds) : "");
        while (row.ends_with(' ')) row.pop_back();
        steps += row + "\n";
    }
    if (!steps.empty() && state != State::up_to_date) {
        logged += "    phases:\n" + steps;
        if (shows(it.name, "units")) console += "    phases:\n" + steps;
    }
    if (log_.is_open()) {
        std::string plain;
        bool        escape = false;
        for (char c : logged) {
            if (escape) {
                if (c >= 0x40 && c <= 0x7E && c != '[') escape = false;
                continue;
            }
            if (c == '\x1b') {
                escape = true;
                continue;
            }
            plain += c;
        }
        log_ << plain;
    }
    if (progress_ != Progress::quiet) paint_locked(console, {}, true);
    dirty_ = true;
}

void BuildReport::end(bool ok) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (drawer_.joinable()) drawer_.join();
    if (captured_out_) captured_out_->flush();
    if (captured_err_) captured_err_->flush();
    out_proxy_->flush();
    err_proxy_->flush();
    if (captured_out_) captured_out_->rdbuf(saved_out_), captured_out_ = nullptr;
    if (captured_err_) captured_err_->rdbuf(saved_err_), captured_err_ = nullptr;
    std::lock_guard<std::mutex> guard(lock_);
    erase_locked();
    std::size_t targets = 0, up_to_date = 0, errors = 0, warnings = 0;
    for (const Item& it : items_) {
        if (it.kind == "packages" || it.kind == "build.ed") continue;
        ++targets;
        up_to_date += it.state == State::up_to_date;
        errors += it.errors;
        warnings += it.warnings;
    }
    // The slowest units, for the `timings` detail.
    std::vector<std::tuple<double, std::string, std::string>> slow;
    for (const Item& it : items_) {
        for (const Unit& u : it.units) {
            if (u.state == State::done) slow.emplace_back(u.seconds, it.name, u.name);
        }
    }
    std::sort(slow.begin(), slow.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    std::string timings = "the slowest units:\n";
    for (std::size_t k = 0; k < slow.size() && k < 10; ++k) {
        timings += std::format("  {:>8}  {}: {}\n", seconds_text(std::get<0>(slow[k])), std::get<1>(slow[k]), std::get<2>(slow[k]));
    }
    if (log_.is_open()) log_ << timings;
    bool show_timings = o_.details.count("timings") != 0;
    if (show_timings && progress_ != Progress::quiet) out_ << timings;
    std::map<std::string, std::string> values{{"targets", count_text(targets, "target")},
                                              {"up_to_date", std::to_string(up_to_date)},
                                              {"elapsed", seconds_text(seconds_since(began_))},
                                              {"errors", count_text(errors, "error")},
                                              {"warnings", count_text(warnings, "warning")},
                                              {"log", o_.log_file ? log_path_text() : std::string("(no build.log)")}};
    std::string last = render(ok ? o_.last_line : o_.failed_line, values, 0) + "\n";
    if (log_.is_open()) log_ << last;
    out_ << last;
    out_.flush();
    // The times of this build weigh the next build's progress bars.
    std::error_code ec;
    std::filesystem::create_directories(timings_path_.parent_path(), ec);
    if (std::ofstream t(timings_path_); t) {
        std::map<std::string, double> now = previous_;
        for (const Item& it : items_) {
            for (const Unit& u : it.units) {
                if (u.state == State::done) now[it.name + "/" + u.name] = u.seconds;
            }
        }
        for (const auto& [key, value] : now) t << key << '\t' << value << '\n';
    }
}

void BuildReport::draw_loop() {
    std::unique_lock<std::mutex> guard(lock_);
    while (!stop_) {
        wake_.wait_for(guard, std::chrono::milliseconds(100));
        if (stop_) break;
        bool running = false;
        for (const Item& it : items_) running = running || it.state == State::running;
        if (dirty_ || running) {   // the times of running units change; an unchanged frame isn't written
            redraw_locked();
            dirty_ = false;
        }
    }
}

} // namespace edax
