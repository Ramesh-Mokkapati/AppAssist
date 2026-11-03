// CoreEngine.h - source-code knowledge base and log root-cause analysis (no MFC; shared by GUI and CLI).
//
// Pipeline (deterministic first, AI last):
//   1. SourceCodeParser walks a source tree with Win32 FindFirstFileW/FindNextFileW and extracts, with
//      C++11 <regex>, class/struct definitions, function signatures and the string literals passed to
//      logging/throw/raise calls. For program flow it records each function's calls and the conditions
//      (if/else/catch/case) that guard each message; for workarounds, the settings the code reads
//      (registry, INI, environment, command line, settings objects, constants) and the keys of config
//      files in the tree. The result is a KnowledgeBase saved as a .kdb (JSON) file.
//   2. LogAnalyzer reads a log, finds error events (deduplicated), and matches them against the
//      knowledge base WITHOUT any AI: log text vs. source message strings, stack-trace file:line,
//      and identifiers vs. symbol names. Matches become Evidence with source snippets, the conditions
//      that lead there and the call chain; settings near that code become SettingEvidence.
//   3. DiagnosisRunner asks a language model (ILanguageModel: embedded llama.cpp or a local HTTP
//      server) to explain the evidence. The prompt requires a file:line citation for every claim and
//      labels workarounds as unverified; with no model, the deterministic report is still produced.
//
// Language level: compiled as C++14, using only C++11 library features (no <filesystem>).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace appassist
{

    // ----- text and file helpers (UTF-8 internally, UTF-16 at the Win32 boundary) -----

    std::string WideToUtf8(const std::wstring& text);
    std::wstring Utf8ToWide(const std::string& text);
    std::string ToLowerAscii(std::string text);
    std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
    std::wstring LowerExtension(const std::wstring& path);   // ".cpp", "" if none
    bool FileExists(const std::wstring& path);
    bool DirectoryExists(const std::wstring& path);

    // Reads a whole file (or, if larger than maxBytes, its last maxBytes) into `out`.
    bool ReadFileBytes(const std::wstring& path, std::string& out, std::string& error,
        unsigned long long maxBytes = 64ull * 1024 * 1024);

    // Writes atomically: temp file next to the target, then MoveFileExW(REPLACE_EXISTING).
    bool WriteFileBytes(const std::wstring& path, const std::string& data, std::string& error);
    std::vector<std::string> SplitLines(const std::string& text);
    std::string Trim(const std::string& text);
    std::string NowIso8601();

    typedef std::function<void(const std::string&)> ProgressCallback;

    // ----- knowledge base -----

    struct CallInfo
    {
        std::string name;       // as written, at most "qualifier.name": "Load", "kb.Load", "KnowledgeBase::Load"
        int line = 0;
    };

    struct SymbolInfo
    {
        std::string kind;       // "class", "struct", "function"
        std::string name;       // e.g. "Downloader", "Downloader::Run", "TranscribeJob._run"
        std::string signature;  // the declaration line, trimmed
        int line = 0;
        int endLine = 0;        // last line of a function body (0: a declaration, or unknown)
        std::vector<CallInfo> calls;  // functions this function calls, first call site of each
    };

    // A condition the program must meet to reach a line: an enclosing if/else/while/catch/case.
    struct GuardInfo
    {
        int line = 0;
        std::string text;       // e.g. "if (!FileExists(path))", "else (after: if (ok))", "except OSError:"
    };

    struct MessageInfo
    {
        std::string text;       // string literal from a log/throw/raise/print call
        std::string function;   // enclosing function, if known
        int line = 0;
        std::vector<GuardInfo> guards;  // innermost last
    };

    // A place where the program reads a setting a user or administrator can change.
    struct SettingReadInfo
    {
        std::string source;     // "registry", "ini", "environment", "command line", "settings", "constant"
        std::string section;    // registry/ini section, if any
        std::string key;        // the setting's name: "ModelPath", "--model", "LOCALAPPDATA", "kMaxEvents"
        std::string variable;   // the variable it is stored in, if assigned ("modelPath", "self.model_dir")
        std::string value;      // default (or, for a constant, its value) as written in the code
        std::string function;
        int line = 0;
    };

    struct SourceFileInfo
    {
        std::string path;       // relative to the knowledge base root, '/' separators, UTF-8
        std::string language;   // "cpp" or "python"
        int lineCount = 0;
        std::vector<SymbolInfo> symbols;
        std::vector<MessageInfo> messages;
        std::vector<SettingReadInfo> settings;
    };

    // A key in a configuration file found in the source tree (.ini, .cfg, .conf, .env, .json, .yaml, .toml).
    struct ConfigEntry
    {
        std::string key;        // "section.key", "a.b[0].c"
        std::string value;
        int line = 0;
    };

    struct ConfigFileInfo
    {
        std::string path;       // relative to the knowledge base root
        std::string format;     // "ini", "json", "yaml", "toml"
        std::vector<ConfigEntry> entries;
    };

    struct KnowledgeBase
    {
        std::string root;       // absolute source root, UTF-8
        std::string created;
        int version = 0;        // format version it was saved with; below 2 it has no flow or settings
        std::string registryKey;  // SetRegistryKey(...) company name, if the code sets one
        std::vector<SourceFileInfo> files;
        std::vector<ConfigFileInfo> configFiles;

        size_t SymbolCount() const;
        size_t MessageCount() const;
        size_t CallCount() const;
        size_t SettingCount() const;   // settings read in code plus config file entries
        bool HasFlow() const
        {
            return version >= 2;
        }

        bool Save(const std::wstring& path, std::string& error) const;
        bool Load(const std::wstring& path, std::string& error);
    };

    class SourceCodeParser
    {
    public:
        SourceCodeParser(ProgressCallback progress, const std::atomic<bool>* cancel);

        // Walks rootDir recursively and fills kb. Returns false on error or cancel (error says which).
        bool BuildKnowledgeBase(const std::wstring& rootDir, KnowledgeBase& kb, std::string& error);

        // Parses one file's text; exposed for testing.
        static SourceFileInfo ParseText(const std::string& relativePath, const std::string& language,
            const std::string& text);
        static ConfigFileInfo ParseConfigText(const std::string& relativePath, const std::string& format,
            const std::string& text);
        static std::string LanguageFor(const std::wstring& path);   // "" if not a supported source file
        static std::string ConfigFormatFor(const std::wstring& path);  // "" if not a config file

    private:
        void CollectFiles(const std::wstring& dir, const std::wstring& relative, int depth,
            std::vector<std::wstring>& files, std::vector<std::wstring>& configFiles);
        bool Cancelled() const
        {
            return cancel_ && cancel_->load();
        }

        ProgressCallback progress_;
        const std::atomic<bool>* cancel_;
    };

    // ----- log analysis -----

    struct LogEvent
    {
        int line = 0;                 // 1-based line of the first occurrence
        int occurrences = 1;
        std::string severity;         // "ERROR", "FATAL", "EXCEPTION", ...
        std::string text;             // the line itself
        std::vector<std::string> context;  // following lines (stack trace etc.)
    };

    struct Evidence
    {
        std::string kind;     // "message" | "location" | "symbol"
        std::string file;     // relative path in the knowledge base
        int line = 0;
        std::string name;     // function or symbol
        std::string detail;   // why it matched
        int eventIndex = -1;  // which LogEvent it explains
        double score = 0.0;
        std::string snippet;  // numbered source lines around `line` (empty if source unavailable)
        std::vector<GuardInfo> guards;     // conditions that lead to `line`, innermost last
        std::string callChain;             // "main -> RunAnalyze -> KnowledgeBase::Load" (outermost first)
        std::vector<std::string> callers;  // "RunAnalyze (main_cli.cpp:171)", at most a few
    };

    // A setting that may change the outcome of a problem, with how to change it.
    struct SettingEvidence
    {
        int eventIndex = -1;
        std::string source;     // as in SettingReadInfo, or "config file"
        std::string key;
        std::string file;       // where it is read (or, for a config file, where it is set)
        int line = 0;
        std::string function;
        std::string value;      // default or current value, if known
        std::string howToChange;
        std::string reason;     // why it is linked to the problem
        double score = 0.0;
    };

    struct LogAnalysis
    {
        std::wstring logPath;
        int totalLines = 0;
        int errorLines = 0;
        std::vector<LogEvent> events;
        std::vector<Evidence> evidence;
        std::vector<SettingEvidence> settings;
    };

    class LogAnalyzer
    {
    public:
        static const size_t kMaxEvents = 20;
        static const size_t kMaxEvidence = 12;
        static const size_t kMaxSettingsPerEvent = 5;

        bool Analyze(const KnowledgeBase& kb, const std::wstring& logPath, LogAnalysis& result,
            std::string& error) const;
    };

    // ----- language models and the end-to-end diagnosis -----

    // Called with each piece of generated text; return false to stop generation.
    typedef std::function<bool(const std::string&)> TokenCallback;

    class ILanguageModel
    {
    public:
        virtual ~ILanguageModel() {}
        virtual std::string Describe() const = 0;
        virtual bool Generate(const std::string& systemPrompt, const std::string& userPrompt,
            const TokenCallback& onText, std::string& error) = 0;
    };

    struct PromptPair
    {
        std::string system;
        std::string user;
    };

    PromptPair BuildDiagnosisPrompt(const LogAnalysis& analysis, size_t maxUserChars = 14000);
    std::string FormatDeterministicReport(const KnowledgeBase& kb, const LogAnalysis& analysis);

    class DiagnosisRunner
    {
    public:
        // Produces the report through `emit` (called repeatedly with text to append). `model` may be
        // null: then only the deterministic (no-AI) report is produced.
        static bool Run(const KnowledgeBase& kb, const std::wstring& logPath, ILanguageModel* model,
            const std::function<void(const std::string&)>& emit,
            const std::atomic<bool>* cancel, std::string& error);
    };

}  // namespace appassist
