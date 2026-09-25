#pragma once
// ── Automation harness ────────────────────────────────────────────────────────────────────────────
// A small command language that drives the running app: teleport, set the time, change settings
// and knockout bits, select / follow satellites, capture screenshots with a JSON state sidecar,
// sample GPU/CPU timings, run the knockout sweep. See docs/HARNESS.md for the command reference and
// tools/harness/ for the drivers.
//
// This file is the app-independent half: command-line options, the statement parser, the queue
// that runs one command at a time across frames, the run directory (results.jsonl, summary.json)
// and the live inbox. The commands themselves live in SatelliteSimHarness.cpp.
//
// Three ways in, one parser:
//   --script <file>   batch: run the file's statements, write the run folder, exit
//   --live <dir>      poll <dir>/inbox/*.satcmd, answer in <dir>/outbox/<name>.json
//   the ~ console     (typed by a person; same statements)
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace harness
{
struct Options
{
    bool enabled = false;       // any harness flag was given
    std::string scriptPath;     // --script
    std::string outDir;         // --out: the run folder (created); default <launch cwd>/harness_runs/<stamp>
    std::string liveDir;        // --live: inbox/outbox directory
    std::string settingsPath;   // --settings: settings.json copied into the run folder before init
    bool useUserData = false;   // --user-data: read/write the normal user data dir (default: the run folder)
    int winW = 0, winH = 0;     // --window WxH (0 = the app's normal maximized window)
    bool mute = true;           // --sound turns it back on
    bool exitWhenDone = true;   // --stay keeps the app open when the script ends
    float fixedDt = 1.0f / 60.0f; // --fixed-dt <s>, 0 = real frame time
    double timeoutS = 0.0;      // --timeout <s>: watchdog, hard exit with summary.json "timeout"
    std::string launchCwd;      // cwd before main() chdirs to the exe dir (relative paths resolve here)
};

Options &options();
inline bool active() { return options().enabled; }

// Parses argv (argv[0] skipped). Returns false with `err` set on a bad flag. Unknown arguments are
// an error, so a typo never silently runs the normal app.
bool parseArgs(int argc, char **argv, std::string &err);
std::string usage();

// One statement: `name pos1 pos2 key=value key="quoted value"`.
struct Command
{
    std::string text;   // the statement as written
    std::string source; // "file.satcmd:12", "live:name:3", "console"
    std::string name;
    std::vector<std::string> pos;
    std::map<std::string, std::string> kv;

    bool has(const std::string &k) const { return kv.count(k) != 0; }
    std::string str(const std::string &k, const std::string &def = "") const;
    double num(const std::string &k, double def) const; // throws std::runtime_error on a bad number
    bool flag(const std::string &k, bool def) const;
};

// Splits text into statements (newlines and ';', '#' comments, quotes respected) and parses each.
// Returns false and sets err (with the source line) on the first malformed statement.
bool parseStatements(const std::string &text, const std::string &sourceName, std::vector<Command> &out,
                     std::string &err);

enum class Status
{
    Done,
    Pending, // call again next frame
    Error,
};

// The command being executed. `frame` counts the calls for THIS command (0 on the first), so a
// command can do its setup once and then poll; `scratch` is its own state between calls.
struct Active
{
    Command cmd;
    int frame = 0;
    double startWall = 0.0;
    nlohmann::json scratch;
    nlohmann::json result = nlohmann::json::object();
    std::string error;
    std::string liveFile; // non-empty: this command came from a live inbox file
};

using ExecFn = std::function<Status(Active &)>;

class Runner
{
public:
    // Creates the run folder and opens results.jsonl; loads --script. Call once after init.
    bool begin(std::string &err);
    // Once per frame. Runs queued statements until one is Pending, or the queue is empty.
    void tick(const ExecFn &exec);
    void enqueue(const std::vector<Command> &cmds, const std::string &liveFile = "");
    // A statement typed into the console; the parse error (if any) is returned instead of queued.
    bool enqueueText(const std::string &text, const std::string &source, std::string &err);

    bool idle() const { return !active_ && queue_.empty(); }
    bool scriptFinished() const { return scriptLoaded_ && idle() && !liveActive(); }
    bool liveActive() const { return !options().liveDir.empty(); }
    bool quitRequested() const { return quit_; }
    void requestQuit() { quit_ = true; }
    const std::string &runDir() const { return runDir_; }
    std::string capturePath(const std::string &name, const char *ext) const;
    int errorCount() const { return errors_; }
    int commandCount() const { return executed_; }
    uint64_t frame() const { return frame_; }
    const Active *current() const { return active_ ? &*active_ : nullptr; }

    // Recent result lines, for the console window.
    const std::deque<std::string> &consoleLines() const { return console_; }
    void consoleEcho(const std::string &s);

    void writeSummary(const char *status, const std::string &message = "");

private:
    void finish(Active &a, Status st);
    void pollLive();
    void writeStatus();

    std::string runDir_;
    std::deque<Active> queue_; // pending (cmd only)
    std::unique_ptr<Active> active_;
    bool scriptLoaded_ = false;
    bool quit_ = false;
    int errors_ = 0;
    int executed_ = 0;
    uint64_t frame_ = 0;
    double lastLivePoll_ = 0.0;
    double lastStatus_ = 0.0;
    std::deque<std::string> console_;
    // Live file bookkeeping: remaining commands and the collected results per inbox file.
    std::map<std::string, int> liveRemaining_;
    std::map<std::string, nlohmann::json> liveResults_;
};

// Wall clock in seconds (steady).
double nowS();

// Watchdog: hard-exits after options().timeoutS with summary.json {"status":"timeout"}.
void startWatchdog(Runner *runner);
} // namespace harness
