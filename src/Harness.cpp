#include "Harness.h"
#include "Log.h"
#include "version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <io.h> // _commit / _fileno, used by writeDurable below
#endif

namespace fs = std::filesystem;

namespace harness
{
namespace
{
Options g_opts;
std::mutex g_fileMutex; // results.jsonl / summary.json (the watchdog thread may write the summary)
std::atomic<bool> g_summaryWritten{false};

// Same durability rule as Log::line (docs/HARNESS.md, "Machine-level resets"): results.jsonl and
// summary.json are what a crash is diagnosed from, and a tail left in the page cache commits with
// its size but reads all-NUL — indistinguishable from a command that never ran. One fsync per write
// is affordable here (a line per command, a summary per run). Deliberately NOT used for
// status.json, which is rewritten every frame in --live mode.
bool writeDurable(const fs::path &path, const std::string &bytes, bool append)
{
#ifdef _WIN32
    std::FILE *f = _wfopen(path.wstring().c_str(), append ? L"ab" : L"wb");
#else
    std::FILE *f = std::fopen(path.string().c_str(), append ? "ab" : "wb");
#endif
    if (!f)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fflush(f);
#ifdef _WIN32
    _commit(_fileno(f)); // FlushFileBuffers on the underlying handle; best effort
#endif
    std::fclose(f);
    return ok;
}

std::string resolveAgainstLaunch(const std::string &p)
{
    if (p.empty())
        return p;
    fs::path path(p);
    if (path.is_relative() && !g_opts.launchCwd.empty())
        path = fs::path(g_opts.launchCwd) / path;
    return path.lexically_normal().string();
}

std::string stampNow()
{
    std::time_t t = std::time(nullptr);
    struct tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &lt);
    return buf;
}

std::string readFile(const std::string &path, bool &ok)
{
    std::ifstream f(path, std::ios::binary);
    ok = f.is_open();
    if (!ok)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

Options &options() { return g_opts; }

double nowS()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string usage()
{
    return "Harness options (docs/HARNESS.md):\n"
           "  --script <file>     run the file's commands, then exit\n"
           "  --live <dir>        poll <dir>/inbox/*.satcmd, answer in <dir>/outbox/\n"
           "  --out <dir>         run folder (default harness_runs/<timestamp>)\n"
           "  --window WxH        fixed window size (default 1600x900 under the harness)\n"
           "  --settings <file>   start from this settings.json (default: built-in defaults)\n"
           "  --user-data         use the normal user data dir instead of the run folder\n"
           "  --fixed-dt <s>      frame time fed to the sim (default 1/60; 0 = real time)\n"
           "  --timeout <s>       hard exit after this long\n"
           "  --stay              keep running after the script finishes\n"
           "  --sound             don't mute audio\n";
}

bool parseArgs(int argc, char **argv, std::string &err)
{
    auto need = [&](int &i, const char *flag) -> const char *
    {
        if (i + 1 >= argc)
        {
            err = std::string(flag) + " needs a value";
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        const char *v = nullptr;
        if (a == "--script")
        {
            if (!(v = need(i, "--script")))
                return false;
            g_opts.scriptPath = v;
        }
        else if (a == "--live")
        {
            if (!(v = need(i, "--live")))
                return false;
            g_opts.liveDir = v;
        }
        else if (a == "--out")
        {
            if (!(v = need(i, "--out")))
                return false;
            g_opts.outDir = v;
        }
        else if (a == "--settings")
        {
            if (!(v = need(i, "--settings")))
                return false;
            g_opts.settingsPath = v;
        }
        else if (a == "--window")
        {
            if (!(v = need(i, "--window")))
                return false;
            if (sscanf(v, "%dx%d", &g_opts.winW, &g_opts.winH) != 2 || g_opts.winW < 64 || g_opts.winH < 64)
            {
                err = "--window expects WxH, e.g. 1600x900";
                return false;
            }
        }
        else if (a == "--fixed-dt")
        {
            if (!(v = need(i, "--fixed-dt")))
                return false;
            g_opts.fixedDt = (float)atof(v);
        }
        else if (a == "--timeout")
        {
            if (!(v = need(i, "--timeout")))
                return false;
            g_opts.timeoutS = atof(v);
        }
        else if (a == "--user-data")
            g_opts.useUserData = true;
        else if (a == "--stay")
            g_opts.exitWhenDone = false;
        else if (a == "--sound")
            g_opts.mute = false;
        else if (a == "--help" || a == "-h")
        {
            err = "help";
            return false;
        }
        else
        {
            err = "unknown argument '" + a + "'";
            return false;
        }
        g_opts.enabled = true;
    }
    if (!g_opts.enabled)
        return true;
    if (g_opts.scriptPath.empty() && g_opts.liveDir.empty())
        g_opts.exitWhenDone = false; // nothing to finish: behave like --stay
    if (g_opts.winW == 0)
    {
        // A fixed, known size so captures are comparable run to run (the normal app boots maximized
        // to whatever the monitor's work area is).
        g_opts.winW = 1600;
        g_opts.winH = 900;
    }
    g_opts.scriptPath = resolveAgainstLaunch(g_opts.scriptPath);
    g_opts.liveDir = resolveAgainstLaunch(g_opts.liveDir);
    g_opts.settingsPath = resolveAgainstLaunch(g_opts.settingsPath);
    if (g_opts.outDir.empty())
    {
        std::string stem = g_opts.scriptPath.empty() ? "live" : fs::path(g_opts.scriptPath).stem().string();
        g_opts.outDir = (fs::path(g_opts.launchCwd.empty() ? "." : g_opts.launchCwd) / "harness_runs" /
                         (stampNow() + "_" + stem))
                            .string();
    }
    g_opts.outDir = resolveAgainstLaunch(g_opts.outDir);
    return true;
}

// ── Command ──────────────────────────────────────────────────────────────────────────────────────
std::string Command::str(const std::string &k, const std::string &def) const
{
    auto it = kv.find(k);
    return it == kv.end() ? def : it->second;
}

double Command::num(const std::string &k, double def) const
{
    auto it = kv.find(k);
    if (it == kv.end())
        return def;
    char *end = nullptr;
    double v = strtod(it->second.c_str(), &end);
    if (end == it->second.c_str() || *end != '\0')
        throw std::runtime_error("'" + k + "' expects a number, got '" + it->second + "'");
    return v;
}

bool Command::flag(const std::string &k, bool def) const
{
    auto it = kv.find(k);
    if (it == kv.end())
        return def;
    const std::string &v = it->second;
    if (v == "1" || v == "true" || v == "on" || v == "yes")
        return true;
    if (v == "0" || v == "false" || v == "off" || v == "no")
        return false;
    throw std::runtime_error("'" + k + "' expects on/off, got '" + v + "'");
}

namespace
{
// Tokenizes one statement. Quotes group; key=value splits at the first '=' outside quotes.
bool parseOne(const std::string &stmt, Command &c, std::string &err)
{
    std::vector<std::string> raw;
    std::string cur;
    bool inQ = false, any = false;
    for (size_t i = 0; i < stmt.size(); ++i)
    {
        char ch = stmt[i];
        if (inQ)
        {
            if (ch == '\\' && i + 1 < stmt.size())
                cur += stmt[++i];
            else if (ch == '"')
                inQ = false;
            else
                cur += ch;
        }
        else if (ch == '"')
        {
            inQ = true;
            any = true;
        }
        else if (ch == ' ' || ch == '\t' || ch == '\r')
        {
            if (any)
                raw.push_back(cur);
            cur.clear();
            any = false;
        }
        else
        {
            cur += ch;
            any = true;
        }
    }
    if (inQ)
    {
        err = "unterminated quote";
        return false;
    }
    if (any)
        raw.push_back(cur);
    if (raw.empty())
        return false;
    // Re-scan for key=value: the key part must be a bare identifier.
    c.name = raw[0];
    for (char &ch : c.name)
        ch = (char)tolower((unsigned char)ch);
    for (size_t i = 1; i < raw.size(); ++i)
    {
        const std::string &t = raw[i];
        size_t eq = t.find('=');
        bool isKv = eq != std::string::npos && eq > 0;
        if (isKv)
            for (size_t k = 0; k < eq; ++k)
                if (!(isalnum((unsigned char)t[k]) || t[k] == '_' || t[k] == '.' || t[k] == '-'))
                {
                    isKv = false;
                    break;
                }
        if (isKv)
            c.kv[t.substr(0, eq)] = t.substr(eq + 1);
        else
            c.pos.push_back(t);
    }
    return true;
}
} // namespace

bool parseStatements(const std::string &text, const std::string &sourceName, std::vector<Command> &out,
                     std::string &err)
{
    std::string stmt;
    int line = 1, stmtLine = 1;
    bool inQ = false, inComment = false;
    auto flush = [&]() -> bool
    {
        Command c;
        std::string e;
        size_t first = stmt.find_first_not_of(" \t\r");
        if (first != std::string::npos)
        {
            std::string trimmed = stmt.substr(first);
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r'))
                trimmed.pop_back();
            if (!parseOne(trimmed, c, e))
            {
                if (!e.empty())
                {
                    err = sourceName + ":" + std::to_string(stmtLine) + ": " + e;
                    return false;
                }
            }
            else
            {
                c.text = trimmed;
                c.source = sourceName + ":" + std::to_string(stmtLine);
                out.push_back(std::move(c));
            }
        }
        stmt.clear();
        return true;
    };
    for (size_t i = 0; i < text.size(); ++i)
    {
        char ch = text[i];
        if (inComment)
        {
            if (ch == '\n')
            {
                inComment = false;
                if (!flush())
                    return false;
                ++line;
                stmtLine = line;
            }
            continue;
        }
        if (ch == '"')
            inQ = !inQ;
        if (!inQ && ch == '#')
        {
            inComment = true;
            continue;
        }
        if (!inQ && (ch == '\n' || ch == ';'))
        {
            if (!flush())
                return false;
            if (ch == '\n')
                ++line;
            stmtLine = line;
            continue;
        }
        if (stmt.empty() && (ch == ' ' || ch == '\t'))
            continue;
        stmt += ch;
    }
    if (inQ)
    {
        err = sourceName + ":" + std::to_string(stmtLine) + ": unterminated quote";
        return false;
    }
    return flush();
}

// ── Runner ───────────────────────────────────────────────────────────────────────────────────────
bool Runner::begin(std::string &err)
{
    runDir_ = g_opts.outDir;
    std::error_code ec;
    fs::create_directories(fs::path(runDir_) / "captures", ec);
    if (ec)
    {
        err = "cannot create run folder '" + runDir_ + "': " + ec.message();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        writeDurable(fs::path(runDir_) / "results.jsonl", std::string(), false);
    }
    if (!g_opts.liveDir.empty())
    {
        fs::create_directories(fs::path(g_opts.liveDir) / "inbox", ec);
        fs::create_directories(fs::path(g_opts.liveDir) / "outbox", ec);
        fs::create_directories(fs::path(g_opts.liveDir) / "done", ec);
    }
    if (!g_opts.scriptPath.empty())
    {
        bool ok = false;
        std::string text = readFile(g_opts.scriptPath, ok);
        if (!ok)
        {
            err = "cannot read script '" + g_opts.scriptPath + "'";
            return false;
        }
        std::vector<Command> cmds;
        if (!parseStatements(text, fs::path(g_opts.scriptPath).filename().string(), cmds, err))
            return false;
        enqueue(cmds);
        // Keep a copy of what ran next to its results.
        fs::copy_file(g_opts.scriptPath, fs::path(runDir_) / fs::path(g_opts.scriptPath).filename(),
                      fs::copy_options::overwrite_existing, ec);
    }
    scriptLoaded_ = true;
    Log::line("harness: run folder " + runDir_);
    return true;
}

void Runner::enqueue(const std::vector<Command> &cmds, const std::string &liveFile)
{
    for (const Command &c : cmds)
    {
        Active a;
        a.cmd = c;
        a.liveFile = liveFile;
        queue_.push_back(std::move(a));
    }
}

bool Runner::enqueueText(const std::string &text, const std::string &source, std::string &err)
{
    std::vector<Command> cmds;
    if (!parseStatements(text, source, cmds, err))
        return false;
    enqueue(cmds);
    return true;
}

std::string Runner::capturePath(const std::string &name, const char *ext) const
{
    return (fs::path(runDir_) / "captures" / (name + ext)).string();
}

void Runner::consoleEcho(const std::string &s)
{
    console_.push_back(s);
    while (console_.size() > 200)
        console_.pop_front();
}

void Runner::tick(const ExecFn &exec)
{
    ++frame_;
    double now = nowS();
    if (liveActive() && now - lastLivePoll_ > 0.2)
    {
        lastLivePoll_ = now;
        pollLive();
    }
    if (liveActive() && now - lastStatus_ > 1.0)
    {
        lastStatus_ = now;
        writeStatus();
    }
    // Run statements until one needs more frames. A cap keeps a long run of instant commands from
    // stalling one frame for too long (none of them are expensive, but a script can be thousands).
    for (int guard = 0; guard < 256 && !quit_; ++guard)
    {
        if (!active_)
        {
            if (queue_.empty())
                return;
            active_ = std::make_unique<Active>(std::move(queue_.front()));
            queue_.pop_front();
            active_->startWall = now;
            consoleEcho("> " + active_->cmd.text);
        }
        Status st;
        try
        {
            st = exec(*active_);
        }
        catch (const std::exception &e)
        {
            active_->error = e.what();
            st = Status::Error;
        }
        ++active_->frame;
        if (st == Status::Pending)
            return;
        finish(*active_, st);
        active_.reset();
    }
}

void Runner::finish(Active &a, Status st)
{
    ++executed_;
    const bool ok = st == Status::Done;
    if (!ok)
        ++errors_;
    nlohmann::json j;
    j["i"] = executed_;
    j["src"] = a.cmd.source;
    j["cmd"] = a.cmd.text;
    j["ok"] = ok;
    if (!ok)
        j["error"] = a.error.empty() ? std::string("failed") : a.error;
    j["frames"] = a.frame;
    j["wall_ms"] = (nowS() - a.startWall) * 1000.0;
    j["app_frame"] = frame_;
    if (!a.result.empty())
        j["result"] = a.result;
    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        writeDurable(fs::path(runDir_) / "results.jsonl", j.dump() + "\n", true);
    }
    if (ok)
    {
        std::string brief = a.result.contains("message") ? a.result["message"].get<std::string>() : std::string("ok");
        consoleEcho("  " + brief);
    }
    else
        consoleEcho("  ERROR: " + j["error"].get<std::string>());
    fprintf(stderr, "[harness] %s %s%s\n", ok ? "ok " : "ERR", a.cmd.text.c_str(),
            ok ? "" : (" -> " + j["error"].get<std::string>()).c_str());

    if (!a.liveFile.empty())
    {
        liveResults_[a.liveFile].push_back(j);
        if (--liveRemaining_[a.liveFile] <= 0)
        {
            fs::path out = fs::path(g_opts.liveDir) / "outbox" / (fs::path(a.liveFile).stem().string() + ".json");
            fs::path tmp = out;
            tmp += ".tmp";
            {
                writeDurable(tmp, nlohmann::json{{"file", a.liveFile}, {"results", liveResults_[a.liveFile]}}.dump(2) + "\n", false);
            }
            std::error_code ec;
            fs::rename(tmp, out, ec); // atomic publish: a reader never sees half a file
            liveResults_.erase(a.liveFile);
            liveRemaining_.erase(a.liveFile);
        }
    }
    if (a.cmd.name == "quit")
        quit_ = true;
}

void Runner::pollLive()
{
    std::error_code ec;
    fs::path inbox = fs::path(g_opts.liveDir) / "inbox";
    std::vector<fs::path> files;
    for (auto it = fs::directory_iterator(inbox, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".satcmd")
            files.push_back(it->path());
    std::sort(files.begin(), files.end()); // name order: callers can sequence with a counter prefix
    for (const fs::path &p : files)
    {
        bool ok = false;
        std::string text = readFile(p.string(), ok);
        if (!ok)
            continue; // still being written; next poll
        fs::path done = fs::path(g_opts.liveDir) / "done" / p.filename();
        fs::rename(p, done, ec);
        if (ec)
        {
            fs::remove(done, ec);
            fs::rename(p, done, ec);
            if (ec)
                continue;
        }
        std::vector<Command> cmds;
        std::string err;
        const std::string name = p.filename().string();
        if (!parseStatements(text, "live:" + name, cmds, err) || cmds.empty())
        {
            nlohmann::json j{{"file", name},
                             {"results", nlohmann::json::array({{{"ok", false}, {"error", err.empty() ? "no commands" : err}}})}};
            writeDurable(fs::path(g_opts.liveDir) / "outbox" / (p.stem().string() + ".json"), j.dump(2) + "\n", false);
            continue;
        }
        liveRemaining_[name] = (int)cmds.size();
        liveResults_[name] = nlohmann::json::array();
        enqueue(cmds, name);
    }
}

void Runner::writeStatus()
{
    nlohmann::json j{{"frame", frame_},
                     {"idle", idle()},
                     {"queued", queue_.size()},
                     {"current", active_ ? active_->cmd.text : std::string()},
                     {"executed", executed_},
                     {"errors", errors_},
                     {"run_dir", runDir_},
                     {"wall_s", nowS()}};
    fs::path out = fs::path(g_opts.liveDir) / "status.json";
    fs::path tmp = out;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << j.dump(2) << '\n';
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
}

void Runner::writeSummary(const char *status, const std::string &message)
{
    if (runDir_.empty() || g_summaryWritten.exchange(true))
        return;
    nlohmann::json j{{"status", status},
                     {"executed", executed_},
                     {"errors", errors_},
                     {"queued_unrun", queue_.size() + (active_ ? 1 : 0)},
                     {"frames", frame_},
                     {"app_version", APP_VERSION},
                     {"git_commit", APP_GIT_COMMIT},
                     {"script", g_opts.scriptPath},
                     {"run_dir", runDir_},
                     {"log", Log::path()}};
    if (!message.empty())
        j["message"] = message;
    if (!g_opts.liveDir.empty())
    {
        // The app is leaving: a status.json left behind would read as "alive" for its last seconds.
        std::error_code ec;
        fs::remove(fs::path(g_opts.liveDir) / "status.json", ec);
    }
    std::lock_guard<std::mutex> lock(g_fileMutex);
    writeDurable(fs::path(runDir_) / "summary.json", j.dump(2) + "\n", false);
}

void startWatchdog(Runner *runner)
{
    if (g_opts.timeoutS <= 0.0)
        return;
    std::thread([runner]()
                {
        std::this_thread::sleep_for(std::chrono::duration<double>(g_opts.timeoutS));
        fprintf(stderr, "[harness] timeout after %.0f s — exiting\n", g_opts.timeoutS);
        Log::line("harness: watchdog timeout");
        runner->writeSummary("timeout");
        std::_Exit(3); })
        .detach();
}
} // namespace harness
