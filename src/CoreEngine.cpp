// CoreEngine.cpp - see CoreEngine.h for the overall design.
#include "CoreEngine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>

#include "nlohmann/json.hpp"

namespace appassist
{
    using nlohmann::json;

    // =====================================================================================
    // Text and file helpers
    // =====================================================================================

    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return std::string();
        }

        int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
            nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n, nullptr,
            nullptr);
        return out;
    }

    std::wstring Utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return std::wstring();
        }

        int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (n <= 0)
        {
            // not UTF-8: fall back to the ANSI code page rather than losing the text
            n = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
            return out;
        }

        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
        return out;
    }

    std::string ToLowerAscii(std::string text)
    {
        for (size_t i = 0; i < text.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 128)
            {
                text[i] = static_cast<char>(std::tolower(c));
            }
        }

        return text;
    }

    std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
    {
        if (dir.empty())
        {
            return name;
        }

        wchar_t last = dir[dir.size() - 1];
        if (last == L'\\' || last == L'/')
        {
            return dir + name;
        }

        return dir + L"\\" + name;
    }

    std::wstring LowerExtension(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        size_t dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        {
            return std::wstring();
        }

        std::wstring ext = path.substr(dot);
        for (size_t i = 0; i < ext.size(); ++i)
        {
            ext[i] = static_cast<wchar_t>(towlower(ext[i]));
        }

        return ext;
    }

    bool FileExists(const std::wstring& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool DirectoryExists(const std::wstring& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    static std::string LastErrorText(const char* what)
    {
        DWORD code = GetLastError();
        wchar_t* buffer = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        std::string message = buffer ? Trim(WideToUtf8(buffer)) : std::string();
        if (buffer)
        {
            LocalFree(buffer);
        }

        std::ostringstream os;
        os << what << " (Windows error " << code << (message.empty() ? "" : ": ") << message << ")";
        return os.str();
    }

    // Closes a Win32 handle when it goes out of scope.
    class HandleGuard
    {
    public:
        explicit HandleGuard(HANDLE h)
            : h_(h)
        {
        }

        ~HandleGuard()
        {
            if (h_ != INVALID_HANDLE_VALUE && h_ != nullptr)
            {
                CloseHandle(h_);
            }
        }

        HANDLE get() const
        {
            return h_;
        }

    private:
        HandleGuard(const HandleGuard&);
        HandleGuard& operator=(const HandleGuard&);
        HANDLE h_;
    };

    bool ReadFileBytes(const std::wstring& path, std::string& out, std::string& error,
        unsigned long long maxBytes)
    {
        HandleGuard file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE)
        {
            error = LastErrorText(("Cannot open " + WideToUtf8(path)).c_str());
            return false;
        }

        LARGE_INTEGER size;
        if (!GetFileSizeEx(file.get(), &size))
        {
            error = LastErrorText("Cannot read the file size");
            return false;
        }

        unsigned long long total = static_cast<unsigned long long>(size.QuadPart);
        unsigned long long toRead = total;
        if (total > maxBytes)
        {
            // keep the end of a huge file (logs grow at the end)
            LARGE_INTEGER offset;
            offset.QuadPart = static_cast<LONGLONG>(total - maxBytes);
            SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN);
            toRead = maxBytes;
        }

        out.assign(static_cast<size_t>(toRead), '\0');
        size_t done = 0;
        while (done < out.size())
        {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 24));
            DWORD got = 0;
            if (!ReadFile(file.get(), &out[done], chunk, &got, nullptr))
            {
                error = LastErrorText(("Cannot read " + WideToUtf8(path)).c_str());
                return false;
            }

            if (got == 0)
            {
                break;
            }

            done += got;
        }

        out.resize(done);
        if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
            static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
        {
            out.erase(0, 3);  // UTF-8 byte order mark
        }

        return true;
    }

    bool WriteFileBytes(const std::wstring& path, const std::string& data, std::string& error)
    {
        std::wstring temp = path + L".tmp";
        {
            HandleGuard file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.get() == INVALID_HANDLE_VALUE)
            {
                error = LastErrorText(("Cannot create " + WideToUtf8(temp)).c_str());
                return false;
            }

            size_t done = 0;
            while (done < data.size())
            {
                DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 24));
                DWORD wrote = 0;
                if (!WriteFile(file.get(), data.data() + done, chunk, &wrote, nullptr))
                {
                    error = LastErrorText(("Cannot write " + WideToUtf8(temp)).c_str());
                    return false;
                }

                done += wrote;
            }
        }

        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            error = LastErrorText(("Cannot replace " + WideToUtf8(path)).c_str());
            DeleteFileW(temp.c_str());
            return false;
        }

        return true;
    }

    std::vector<std::string> SplitLines(const std::string& text)
    {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
            {
                end = text.size();
            }

            size_t stop = end;
            while (stop > start && text[stop - 1] == '\r')
            {
                --stop;   // "\r\r\n" happens when CRLF text is re-written
            }

            std::string line = text.substr(start, stop - start);
            std::replace(line.begin(), line.end(), '\r', ' ');      // stray carriage returns inside a line
            lines.push_back(line);
            if (end == text.size())
            {
                break;
            }

            start = end + 1;
        }

        if (!lines.empty() && lines.back().empty())
        {
            lines.pop_back();
        }

        return lines;
    }

    std::string Trim(const std::string& text)
    {
        size_t a = 0, b = text.size();
        while (a < b && std::isspace(static_cast<unsigned char>(text[a])))
        {
            ++a;
        }

        while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1])))
        {
            --b;
        }

        return text.substr(a, b - a);
    }

    std::string NowIso8601()
    {
        std::time_t now = std::time(nullptr);
        std::tm local;
        localtime_s(&local, &now);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
        return buffer;
    }

    static std::string Shorten(const std::string& text, size_t maxChars)
    {
        if (text.size() <= maxChars)
        {
            return text;
        }

        return text.substr(0, maxChars) + "...";
    }

    // =====================================================================================
    // Knowledge base (.kdb = JSON)
    // =====================================================================================

    static const char* const kKdbFormat = "appassist-kdb";
    static const int kKdbVersion = 2;   // 2 adds calls, guards, settings and config files

    size_t KnowledgeBase::SymbolCount() const
    {
        size_t n = 0;
        for (size_t i = 0; i < files.size(); ++i)
        {
            n += files[i].symbols.size();
        }

        return n;
    }

    size_t KnowledgeBase::MessageCount() const
    {
        size_t n = 0;
        for (size_t i = 0; i < files.size(); ++i)
        {
            n += files[i].messages.size();
        }

        return n;
    }

    size_t KnowledgeBase::CallCount() const
    {
        size_t n = 0;
        for (size_t i = 0; i < files.size(); ++i)
        {
            for (size_t s = 0; s < files[i].symbols.size(); ++s)
            {
                n += files[i].symbols[s].calls.size();
            }
        }

        return n;
    }

    size_t KnowledgeBase::SettingCount() const
    {
        size_t n = 0;
        for (size_t i = 0; i < files.size(); ++i)
        {
            n += files[i].settings.size();
        }

        for (size_t i = 0; i < configFiles.size(); ++i)
        {
            n += configFiles[i].entries.size();
        }

        return n;
    }

    bool KnowledgeBase::Save(const std::wstring& path, std::string& error) const
    {
        try
        {
            json doc;
            doc["format"] = kKdbFormat;
            doc["version"] = kKdbVersion;
            doc["root"] = root;
            doc["created"] = created;
            if (!registryKey.empty())
            {
                doc["registry_key"] = registryKey;
            }

            json jfiles = json::array();
            for (size_t i = 0; i < files.size(); ++i)
            {
                const SourceFileInfo& f = files[i];
                json jf;
                jf["path"] = f.path;
                jf["language"] = f.language;
                jf["lines"] = f.lineCount;
                json jsyms = json::array();
                for (size_t s = 0; s < f.symbols.size(); ++s)
                {
                    const SymbolInfo& sym = f.symbols[s];
                    json js = { { "kind", sym.kind }, { "name", sym.name }, { "signature", sym.signature },
                        { "line", sym.line } };
                    if (sym.endLine > 0)
                    {
                        js["end"] = sym.endLine;
                    }

                    if (!sym.calls.empty())
                    {
                        json jcalls = json::array();   // [name, line] pairs keep the file compact
                        for (size_t c = 0; c < sym.calls.size(); ++c)
                        {
                            jcalls.push_back(json::array({ sym.calls[c].name, sym.calls[c].line }));
                        }

                        js["calls"] = jcalls;
                    }

                    jsyms.push_back(js);
                }

                jf["symbols"] = jsyms;
                json jmsgs = json::array();
                for (size_t m = 0; m < f.messages.size(); ++m)
                {
                    const MessageInfo& msg = f.messages[m];
                    json jm = { { "text", msg.text }, { "function", msg.function }, { "line", msg.line } };
                    if (!msg.guards.empty())
                    {
                        json jguards = json::array();
                        for (size_t g = 0; g < msg.guards.size(); ++g)
                        {
                            jguards.push_back(json::array({ msg.guards[g].line, msg.guards[g].text }));
                        }

                        jm["guards"] = jguards;
                    }

                    jmsgs.push_back(jm);
                }

                jf["messages"] = jmsgs;
                if (!f.settings.empty())
                {
                    json jsettings = json::array();
                    for (size_t r = 0; r < f.settings.size(); ++r)
                    {
                        const SettingReadInfo& st = f.settings[r];
                        json jst = { { "source", st.source }, { "key", st.key }, { "line", st.line } };
                        if (!st.section.empty())
                        {
                            jst["section"] = st.section;
                        }

                        if (!st.variable.empty())
                        {
                            jst["variable"] = st.variable;
                        }

                        if (!st.value.empty())
                        {
                            jst["value"] = st.value;
                        }

                        if (!st.function.empty())
                        {
                            jst["function"] = st.function;
                        }

                        jsettings.push_back(jst);
                    }

                    jf["settings"] = jsettings;
                }

                jfiles.push_back(jf);
            }

            doc["files"] = jfiles;
            json jconfigs = json::array();
            for (size_t i = 0; i < configFiles.size(); ++i)
            {
                const ConfigFileInfo& cf = configFiles[i];
                json jentries = json::array();
                for (size_t e = 0; e < cf.entries.size(); ++e)
                {
                    jentries.push_back(json::array({ cf.entries[e].key, cf.entries[e].value, cf.entries[e].line }));
                }

                jconfigs.push_back({ { "path", cf.path }, { "format", cf.format }, { "entries", jentries } });
            }

            doc["config_files"] = jconfigs;

            // Source files are not always UTF-8; replace invalid bytes instead of throwing.
            std::string text = doc.dump(1, '\t', false, json::error_handler_t::replace);
            return WriteFileBytes(path, text, error);
        }
        catch (const std::exception& e)
        {
            error = std::string("Cannot save the knowledge base: ") + e.what();
            return false;
        }
    }

    bool KnowledgeBase::Load(const std::wstring& path, std::string& error)
    {
        std::string text;
        if (!ReadFileBytes(path, text, error, 1024ull * 1024 * 1024))
        {
            return false;
        }

        try
        {
            json doc = json::parse(text);
            if (doc.value("format", std::string()) != kKdbFormat)
            {
                error = "Not an AppAssist knowledge base (.kdb) file: " + WideToUtf8(path);
                return false;
            }

            if (doc.value("version", 0) > kKdbVersion)
            {
                error = "This knowledge base was made by a newer version of AppAssist.";
                return false;
            }

            root = doc.value("root", std::string());
            created = doc.value("created", std::string());
            version = doc.value("version", 1);
            registryKey = doc.value("registry_key", std::string());
            files.clear();
            configFiles.clear();

            const json& jfiles = doc.at("files");
            for (json::const_iterator it = jfiles.begin(); it != jfiles.end(); ++it)
            {
                SourceFileInfo f;
                f.path = it->value("path", std::string());
                f.language = it->value("language", std::string());
                f.lineCount = it->value("lines", 0);
                if (it->contains("symbols"))
                {
                    const json& js = (*it)["symbols"];
                    for (json::const_iterator s = js.begin(); s != js.end(); ++s)
                    {
                        SymbolInfo sym;
                        sym.kind = s->value("kind", std::string());
                        sym.name = s->value("name", std::string());
                        sym.signature = s->value("signature", std::string());
                        sym.line = s->value("line", 0);
                        sym.endLine = s->value("end", 0);
                        if (s->contains("calls"))
                        {
                            const json& jc = (*s)["calls"];
                            for (json::const_iterator c = jc.begin(); c != jc.end(); ++c)
                            {
                                CallInfo call;
                                call.name = c->at(0).get<std::string>();
                                call.line = c->at(1).get<int>();
                                sym.calls.push_back(call);
                            }
                        }

                        f.symbols.push_back(sym);
                    }
                }

                if (it->contains("messages"))
                {
                    const json& jm = (*it)["messages"];
                    for (json::const_iterator m = jm.begin(); m != jm.end(); ++m)
                    {
                        MessageInfo msg;
                        msg.text = m->value("text", std::string());
                        msg.function = m->value("function", std::string());
                        msg.line = m->value("line", 0);
                        if (m->contains("guards"))
                        {
                            const json& jg = (*m)["guards"];
                            for (json::const_iterator g = jg.begin(); g != jg.end(); ++g)
                            {
                                GuardInfo guard;
                                guard.line = g->at(0).get<int>();
                                guard.text = g->at(1).get<std::string>();
                                msg.guards.push_back(guard);
                            }
                        }

                        f.messages.push_back(msg);
                    }
                }

                if (it->contains("settings"))
                {
                    const json& jst = (*it)["settings"];
                    for (json::const_iterator r = jst.begin(); r != jst.end(); ++r)
                    {
                        SettingReadInfo st;
                        st.source = r->value("source", std::string());
                        st.section = r->value("section", std::string());
                        st.key = r->value("key", std::string());
                        st.variable = r->value("variable", std::string());
                        st.value = r->value("value", std::string());
                        st.function = r->value("function", std::string());
                        st.line = r->value("line", 0);
                        f.settings.push_back(st);
                    }
                }

                files.push_back(f);
            }

            if (doc.contains("config_files"))
            {
                const json& jconfigs = doc["config_files"];
                for (json::const_iterator c = jconfigs.begin(); c != jconfigs.end(); ++c)
                {
                    ConfigFileInfo cf;
                    cf.path = c->value("path", std::string());
                    cf.format = c->value("format", std::string());
                    const json& je = c->at("entries");
                    for (json::const_iterator e = je.begin(); e != je.end(); ++e)
                    {
                        ConfigEntry entry;
                        entry.key = e->at(0).get<std::string>();
                        entry.value = e->at(1).get<std::string>();
                        entry.line = e->at(2).get<int>();
                        cf.entries.push_back(entry);
                    }

                    configFiles.push_back(cf);
                }
            }

            return true;
        }
        catch (const std::exception& e)
        {
            error = std::string("The knowledge base file is damaged or not valid JSON: ") + e.what();
            return false;
        }
    }

    // =====================================================================================
    // Source code parser
    // =====================================================================================

    SourceCodeParser::SourceCodeParser(ProgressCallback progress, const std::atomic<bool>* cancel)
        : progress_(progress)
        , cancel_(cancel)
    {
    }

    std::string SourceCodeParser::LanguageFor(const std::wstring& path)
    {
        std::wstring ext = LowerExtension(path);
        if (ext == L".py")
        {
            return "python";
        }

        if (ext == L".cpp" || ext == L".hpp" || ext == L".h" || ext == L".cxx" || ext == L".cc")
        {
            return "cpp";
        }

        if (ext == L".java")
        {
            return "java";
        }

        if (ext == L".cs")
        {
            return "csharp";
        }

        if (ext == L".scala" || ext == L".sc")
        {
            return "scala";
        }

        return std::string();
    }

    std::string SourceCodeParser::ConfigFormatFor(const std::wstring& path)
    {
        std::wstring ext = LowerExtension(path);
        if (ext == L".ini" || ext == L".cfg" || ext == L".conf" || ext == L".env" || ext == L".properties")
        {
            return "ini";
        }

        if (ext == L".json")
        {
            return "json";
        }

        if (ext == L".yaml" || ext == L".yml")
        {
            return "yaml";
        }

        if (ext == L".toml")
        {
            return "toml";
        }

        return std::string();
    }

    // Config files that hold tool or package data rather than settings someone would change.
    static bool IsSkippedConfigFile(const std::wstring& name)
    {
        static const wchar_t* const kSkip[] = {
            L"package-lock.json",
            L"package.json",
            L"tsconfig.json",
            L"compile_commands.json",
            L"CMakePresets.json",
            L"CMakeUserPresets.json",
            L"launch.json",
            L"tasks.json",
            L"settings.json",
            L"pyproject.toml",
            L"poetry.lock",
            L"Cargo.toml",
            L".pre-commit-config.yaml",
        };

        for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i)
        {
            if (_wcsicmp(name.c_str(), kSkip[i]) == 0)
            {
                return true;
            }
        }

        return false;
    }

    // Folders that hold tooling, dependencies or build output rather than the application's own code.
    static bool IsSkippedDirectory(const std::wstring& name)
    {
        static const wchar_t* const kSkip[] = {
            L".git",
            L".svn",
            L".hg",
            L".vs",
            L".vscode",
            L".idea",
            L"__pycache__",
            L"node_modules",
            L"build",
            L"out",
            L"bin",
            L"obj",
            L"dist",
            L"x64",
            L"Debug",
            L"Release",
            L"third_party",
            L"vendor",
            L"target",   // Maven output
            L"external",
            L"site-packages",
        };

        if (name.empty() || name[0] == L'.')
        {
            return true;      // hidden folders, incl. .venv
        }

        if (name.compare(0, 4, L"venv") == 0)
        {
            return true;
        }

        for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i)
        {
            if (_wcsicmp(name.c_str(), kSkip[i]) == 0)
            {
                return true;
            }
        }

        return false;
    }

    // A bundled Python installation or virtual environment (its standard library is not the app's code).
    static bool IsPythonInstallation(const std::wstring& dir)
    {
        return FileExists(JoinPath(dir, L"python.exe")) || FileExists(JoinPath(dir, L"pyvenv.cfg")) ||
               FileExists(JoinPath(dir, L"python3.dll"));
    }

    void SourceCodeParser::CollectFiles(const std::wstring& dir, const std::wstring& relative, int depth,
        std::vector<std::wstring>& files, std::vector<std::wstring>& configFiles)
    {
        if (depth > 64 || Cancelled())
        {
            return;
        }

        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }

        do
        {
            std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }

            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                continue;  // avoid junction loops
            }

            std::wstring rel = relative.empty() ? name : relative + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                std::wstring sub = JoinPath(dir, name);
                if (!IsSkippedDirectory(name) && !IsPythonInstallation(sub))
                {
                    CollectFiles(sub, rel, depth + 1, files, configFiles);
                }
            }
            else if (!LanguageFor(name).empty())
            {
                files.push_back(rel);
            }
            else if (!ConfigFormatFor(name).empty() && !IsSkippedConfigFile(name) && data.nFileSizeHigh == 0 &&
                     data.nFileSizeLow <= 256 * 1024)
            {
                configFiles.push_back(rel);   // larger files are data, not settings
            }

        } while (FindNextFileW(find, &data));

        FindClose(find);
    }

    bool SourceCodeParser::BuildKnowledgeBase(const std::wstring& rootDir, KnowledgeBase& kb, std::string& error)
    {
        if (!DirectoryExists(rootDir))
        {
            error = "Source folder not found: " + WideToUtf8(rootDir);
            return false;
        }

        wchar_t full[MAX_PATH * 4];
        DWORD n = GetFullPathNameW(rootDir.c_str(), static_cast<DWORD>(sizeof(full) / sizeof(full[0])), full, nullptr);
        std::wstring root = (n > 0 && n < sizeof(full) / sizeof(full[0])) ? std::wstring(full) : rootDir;

        if (progress_)
        {
            progress_("Scanning " + WideToUtf8(root) + " ...");
        }

        std::vector<std::wstring> files, configFiles;
        CollectFiles(root, std::wstring(), 0, files, configFiles);
        if (Cancelled())
        {
            error = "Cancelled.";
            return false;
        }

        if (files.empty())
        {
            error = "No .py, .java, .cs, .scala, .cpp, .hpp, .h, .cxx or .cc files found under " + WideToUtf8(root);
            return false;
        }

        std::sort(files.begin(), files.end());

        kb.root = WideToUtf8(root);
        kb.created = NowIso8601();
        kb.version = kKdbVersion;
        kb.registryKey.clear();
        kb.files.clear();
        kb.configFiles.clear();
        for (size_t i = 0; i < files.size(); ++i)
        {
            if (Cancelled())
            {
                error = "Cancelled.";
                return false;
            }

            std::string text, readError;
            if (!ReadFileBytes(JoinPath(root, files[i]), text, readError, 8ull * 1024 * 1024))
            {
                if (progress_)
                {
                    progress_("  skipped: " + readError);
                }

                continue;
            }

            std::string rel = WideToUtf8(files[i]);
            std::replace(rel.begin(), rel.end(), '\\', '/');
            kb.files.push_back(ParseText(rel, LanguageFor(files[i]), text));

            // SetRegistryKey("Company") decides where MFC profile settings live; keep it for the advice.
            std::vector<SettingReadInfo>& settings = kb.files.back().settings;
            for (size_t r = 0; r < settings.size();)
            {
                if (settings[r].source == "registry root")
                {
                    kb.registryKey = settings[r].key;
                    settings.erase(settings.begin() + static_cast<std::ptrdiff_t>(r));
                }
                else
                {
                    ++r;
                }
            }

            if (progress_ && ((i + 1) % 25 == 0 || i + 1 == files.size()))
            {
                std::ostringstream os;
                os << "Parsed " << (i + 1) << " of " << files.size() << " files";
                progress_(os.str());
            }
        }

        std::sort(configFiles.begin(), configFiles.end());
        for (size_t i = 0; i < configFiles.size() && !Cancelled(); ++i)
        {
            std::string text, readError;
            if (!ReadFileBytes(JoinPath(root, configFiles[i]), text, readError, 256 * 1024))
            {
                continue;
            }

            std::string rel = WideToUtf8(configFiles[i]);
            std::replace(rel.begin(), rel.end(), '\\', '/');
            ConfigFileInfo cf = ParseConfigText(rel, ConfigFormatFor(configFiles[i]), text);
            if (!cf.entries.empty())
            {
                kb.configFiles.push_back(cf);
            }
        }

        if (progress_ && !kb.configFiles.empty())
        {
            std::ostringstream os;
            os << "Read " << kb.configFiles.size() << " config file(s)";
            progress_(os.str());
        }

        return true;
    }

    namespace
    {
        const size_t kMaxRegexLine = 1500;  // longer lines (minified/generated code) are skipped

        // Words that look like calls but are control flow or operators.
        bool IsCppKeyword(const std::string& name)
        {
            static const char* const kWords[] = {
                "if",
                "for",
                "while",
                "switch",
                "return",
                "sizeof",
                "catch",
                "new",
                "delete",
                "throw",
                "else",
                "case",
                "do",
                "decltype",
                "alignof",
                "static_assert",
                "defined",
                "typeid",
                "co_return",
                "co_await",
                "operator",
                "noexcept",
                "alignas",
                "assert",
                "using",
                "typedef",
                "template",
                // built-in types: "std::function<void(int)>" is not a function named void
                "void",
                "bool",
                "char",
                "int",
                "long",
                "short",
                "float",
                "double",
                "unsigned",
                "signed",
                "auto",
                "const",
            };

            for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i)
            {
                if (name == kWords[i])
                {
                    return true;
                }
            }

            return false;
        }

        // Calls whose string arguments are worth indexing as "messages" the program can print or log.
        const std::regex& CppMessageCall()
        {
            static const std::regex re(
                R"((LOG|Log|log|TRACE|Trace|printf|wprintf|fprintf|sprintf|snprintf|cerr|cout|clog|OutputDebugString|MessageBox|AfxMessageBox|throw|Error|error|Warn|warn|Fail|fail|Exception|exception|assert|ASSERT|report|Report))",
                std::regex::optimize);
            return re;
        }

        const std::regex& CppStringLiteral()
        {
            static const std::regex re(R"re((?:L|u8|u|U)?"((?:\\.|[^"\\])*)")re", std::regex::optimize);
            return re;
        }

        // Removes // and /* */ comments outside string literals; `inBlock` carries /* across lines.
        std::string StripCppComments(const std::string& line, bool& inBlock)
        {
            std::string out;
            out.reserve(line.size());
            bool inString = false, inChar = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                char c = line[i];
                char next = i + 1 < line.size() ? line[i + 1] : '\0';
                if (inBlock)
                {
                    if (c == '*' && next == '/')
                    {
                        inBlock = false;
                        ++i;
                    }

                    continue;
                }

                if (inString || inChar)
                {
                    out += c;
                    if (c == '\\' && i + 1 < line.size())
                    {
                        out += next;
                        ++i;
                        continue;
                    }

                    if (inString && c == '"')
                    {
                        inString = false;
                    }

                    if (inChar && c == '\'')
                    {
                        inChar = false;
                    }

                    continue;
                }

                if (c == '/' && next == '/')
                {
                    break;
                }

                if (c == '/' && next == '*')
                {
                    inBlock = true;
                    ++i;
                    continue;
                }

                // C++11 raw string R"delim(...)delim": quotes, backslashes and braces inside are text. It is
                // passed on as an ordinary string literal so the rest of the parser can skip it.
                bool rawStart = c == 'R' && next == '"' &&
                                (i == 0 || !(std::isalnum(static_cast<unsigned char>(line[i - 1])) || line[i - 1] == '_') ||
                                    (i >= 1 && (line[i - 1] == 'L' || line[i - 1] == 'u' || line[i - 1] == 'U') &&
                                        (i < 2 || !std::isalnum(static_cast<unsigned char>(line[i - 2])))) ||
                                    (i >= 2 && line.compare(i - 2, 2, "u8") == 0));
                size_t open = rawStart ? line.find('(', i + 2) : std::string::npos;
                if (open != std::string::npos && open - (i + 2) <= 16)
                {
                    std::string close = ")" + line.substr(i + 2, open - (i + 2)) + "\"";
                    size_t end = line.find(close, open + 1);
                    std::string body = line.substr(open + 1, end == std::string::npos ? std::string::npos : end - open - 1);
                    std::replace(body.begin(), body.end(), '"', '\'');
                    std::replace(body.begin(), body.end(), '\\', '/');
                    out += "\"" + body + "\"";
                    if (end == std::string::npos)
                    {
                        break;   // continues on the next line: the rest of this one is text
                    }

                    i = end + close.size() - 1;
                    continue;
                }

                if (c == '"')
                {
                    inString = true;
                }

                if (c == '\'')
                {
                    inChar = true;
                }

                out += c;
            }

            return out;
        }

        int CountBraces(const std::string& code)
        {
            // net { minus } outside string/char literals
            int depth = 0;
            bool inString = false, inChar = false;
            for (size_t i = 0; i < code.size(); ++i)
            {
                char c = code[i];
                if (inString || inChar)
                {
                    if (c == '\\')
                    {
                        ++i;
                        continue;
                    }

                    if (inString && c == '"')
                    {
                        inString = false;
                    }

                    if (inChar && c == '\'')
                    {
                        inChar = false;
                    }

                    continue;
                }

                if (c == '"')
                {
                    inString = true;
                }
                else if (c == '\'')
                {
                    inChar = true;
                }
                else if (c == '{')
                {
                    ++depth;
                }
                else if (c == '}')
                {
                    --depth;
                }
            }

            return depth;
        }

        bool IsWordChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }

        // "else if (x)" starts with the word "else"; "elsewhere" does not.
        bool StartsWithWord(const std::string& text, const char* word)
        {
            size_t n = std::strlen(word);
            return text.compare(0, n, word) == 0 && (text.size() == n || !IsWordChar(text[n]));
        }

        // "if (a(b)) {" -> "if (a(b))": the keyword and its balanced parentheses.
        std::string ConditionHeader(const std::string& text)
        {
            size_t open = text.find('(');
            if (open == std::string::npos)
            {
                return Shorten(Trim(text), 160);
            }

            int nesting = 0;
            for (size_t i = open; i < text.size(); ++i)
            {
                if (text[i] == '(')
                {
                    ++nesting;
                }
                else if (text[i] == ')' && --nesting == 0)
                {
                    return Shorten(Trim(text.substr(0, i + 1)), 160);
                }
            }

            return Shorten(Trim(text), 160);   // the condition continues on the next line
        }

        bool IsCppGuardHeader(const std::string& header)
        {
            return StartsWithWord(header, "if") || StartsWithWord(header, "else") || StartsWithWord(header, "while") ||
                   StartsWithWord(header, "catch") || StartsWithWord(header, "switch");
        }

        // "case Kind::Error: Log(...)" -> "case Kind::Error:"
        std::string CaseLabel(const std::string& trimmed)
        {
            for (size_t i = 0; i < trimmed.size(); ++i)
            {
                if (trimmed[i] == ':')
                {
                    if (i + 1 < trimmed.size() && trimmed[i + 1] == ':')
                    {
                        ++i;
                        continue;
                    }

                    return Shorten(trimmed.substr(0, i + 1), 120);
                }
            }

            return Shorten(trimmed, 120);
        }

        // The if/else/while/catch/case blocks that enclose `targetLine`, scanning from the function's first
        // line. A brace-less "if (x)" on the line before, and an "if (x) Log(...)" on the line itself, count.
        std::vector<GuardInfo> CppGuards(const std::vector<std::string>& lines, int fromLine, int targetLine)
        {
            struct Block
            {
                int line;
                std::string header;
                int caseLine;
                std::string caseLabel;
            };

            std::vector<Block> stack;
            std::map<size_t, std::string> lastClosed;   // nesting level -> header of the block that closed there
            std::vector<GuardInfo> guards;
            bool inBlock = false;
            std::string prevCode;
            int prevLine = 0;
            const int from = std::max(1, fromLine), to = std::min(static_cast<int>(lines.size()), targetLine);
            for (int n = from; n <= to; ++n)
            {
                std::string code = StripCppComments(lines[static_cast<size_t>(n - 1)], inBlock);
                std::string trimmed = Trim(code);
                if (trimmed.empty() || trimmed[0] == '#' || code.size() > kMaxRegexLine)
                {
                    continue;
                }

                if (n == to)
                {
                    for (size_t b = 0; b < stack.size(); ++b)
                    {
                        if (IsCppGuardHeader(stack[b].header))
                        {
                            GuardInfo g = { stack[b].line, stack[b].header };
                            guards.push_back(g);
                        }

                        if (!stack[b].caseLabel.empty())
                        {
                            GuardInfo g = { stack[b].caseLine, stack[b].caseLabel };
                            guards.push_back(g);
                        }
                    }

                    char last = prevCode.empty() ? ';' : prevCode[prevCode.size() - 1];
                    if (IsCppGuardHeader(prevCode) && last != ';' && last != '{' && last != '}')
                    {
                        GuardInfo g = { prevLine, ConditionHeader(prevCode) };
                        guards.push_back(g);
                    }

                    std::string own = trimmed[0] == '}' ? Trim(trimmed.substr(1)) : trimmed;
                    if (IsCppGuardHeader(own) && own.find('(') != std::string::npos)
                    {
                        GuardInfo g = { n, ConditionHeader(own) };
                        guards.push_back(g);
                    }

                    break;
                }

                if ((StartsWithWord(trimmed, "case") || StartsWithWord(trimmed, "default")) && !stack.empty())
                {
                    stack.back().caseLabel = CaseLabel(trimmed);
                    stack.back().caseLine = n;
                }

                size_t segment = 0;
                bool inString = false, inChar = false;
                for (size_t i = 0; i < code.size(); ++i)
                {
                    char c = code[i];
                    if (inString || inChar)
                    {
                        if (c == '\\')
                        {
                            ++i;
                        }
                        else if ((inString && c == '"') || (inChar && c == '\''))
                        {
                            inString = inChar = false;
                        }

                        continue;
                    }

                    if (c == '"')
                    {
                        inString = true;
                    }
                    else if (c == '\'')
                    {
                        inChar = true;
                    }
                    else if (c == ';')
                    {
                        segment = i + 1;
                    }
                    else if (c == '{')
                    {
                        std::string header = Trim(code.substr(segment, i - segment));
                        int headerLine = n;
                        if (header.empty())
                        {
                            header = prevCode;   // "{" on a line of its own
                            headerLine = prevLine;
                        }

                        header = ConditionHeader(header);
                        if (StartsWithWord(header, "else") && !StartsWithWord(header.substr(std::min<size_t>(header.size(), 5)), "if"))
                        {
                            std::map<size_t, std::string>::const_iterator closed = lastClosed.find(stack.size());
                            if (closed != lastClosed.end() && (StartsWithWord(closed->second, "if") ||
                                                                  StartsWithWord(closed->second, "else")))
                            {
                                header = Shorten("else (after: " + closed->second + ")", 200);
                            }
                        }

                        Block block = { headerLine, header, 0, std::string() };
                        stack.push_back(block);
                        segment = i + 1;
                    }
                    else if (c == '}')
                    {
                        if (!stack.empty())
                        {
                            lastClosed[stack.size() - 1] = stack.back().header;
                            stack.pop_back();
                        }

                        segment = i + 1;
                    }
                }

                prevCode = trimmed;
                prevLine = n;
            }

            if (guards.size() > 4)
            {
                guards.erase(guards.begin(), guards.end() - 4);   // the innermost conditions matter most
            }

            return guards;
        }

        // Adds a call once per function, by the name as written.
        void AddCall(SymbolInfo& function, const std::string& name, int line)
        {
            for (size_t i = 0; i < function.calls.size(); ++i)
            {
                if (function.calls[i].name == name)
                {
                    return;
                }
            }

            if (function.calls.size() < 200)
            {
                CallInfo call;
                call.name = name;
                call.line = line;
                function.calls.push_back(call);
            }
        }

        // "a::b::Load" -> "b::Load"; "kb.Load" for "kb.Load(" or "kb->Load(" (`at` is where the name starts).
        std::string CallName(const std::string& code, size_t at, const std::string& name)
        {
            size_t scope = name.rfind("::");
            if (scope != std::string::npos)
            {
                size_t before = scope == 0 ? std::string::npos : name.rfind("::", scope - 1);
                return before == std::string::npos ? name : name.substr(before + 2);
            }

            size_t end = at;
            if (end >= 1 && code[end - 1] == '.')
            {
                end -= 1;
            }
            else if (end >= 2 && code[end - 2] == '-' && code[end - 1] == '>')
            {
                end -= 2;
            }
            else
            {
                return name;
            }

            size_t start = end;
            while (start > 0 && IsWordChar(code[start - 1]))
            {
                --start;
            }

            return start == end ? name : code.substr(start, end - start) + "." + name;
        }

        // The text inside a string literal, without _T("..."), L"..." or u8"..." around it.
        std::string LiteralText(const std::string& text)
        {
            size_t open = text.find_first_of("\"'");
            if (open == std::string::npos)
            {
                return Trim(text);
            }

            size_t close = text.find(text[open], open + 1);
            return close == std::string::npos ? Trim(text) : text.substr(open + 1, close - open - 1);
        }

        // A literal worth showing as a tunable value: a number, a string, true/false.
        bool IsLiteralValue(const std::string& value)
        {
            static const std::regex re(
                R"re(^(?:[-+]?(?:0x[0-9a-fA-F]+|[\d.]+(?:[eE][-+]?\d+)?)[uUlLfF]*|(?:_T|L|TEXT|u8)?\s*\(?\s*"[^"]*"\s*\)?|[rRbBuUfF]{0,2}'[^']*'|[rRbBuUfF]{0,2}"[^"]*"|true|false|True|False)$)re",
                std::regex::optimize);
            return std::regex_match(value, re);
        }

        // "CString path = app->GetProfileString(" -> "path"; "options.modelPath = argv[++i]" -> "modelPath".
        std::string AssignedVariable(const std::string& before)
        {
            static const std::regex re(R"(([A-Za-z_]\w*)\s*=(?!=))", std::regex::optimize);
            std::smatch m;
            std::string variable;
            std::string::const_iterator from = before.begin();
            while (std::regex_search(from, before.end(), m, re))
            {
                variable = m[1].str();   // the last assignment before the read
                from = m[0].second;
            }

            return variable;
        }

        void AddSetting(SourceFileInfo& info, const std::string& source, const std::string& section,
            const std::string& key, const std::string& variable, const std::string& value,
            const std::string& function, int line)
        {
            if (key.empty() || info.settings.size() >= 2000)
            {
                return;
            }

            for (size_t i = 0; i < info.settings.size(); ++i)
            {
                const SettingReadInfo& s = info.settings[i];
                if (s.source == source && s.key == key && s.function == function)
                {
                    return;   // read twice in one function
                }
            }

            SettingReadInfo s;
            s.source = source;
            s.section = Shorten(Trim(section), 80);
            s.key = Shorten(key, 120);
            s.variable = variable;
            s.value = Shorten(Trim(value), 80);
            s.function = function;
            s.line = line;
            info.settings.push_back(s);
        }

        // Registry/profile, INI, environment and command-line reads in one line of C++.
        void FindCppSettings(const std::vector<std::string>& lines, size_t index, const std::string& code,
            const std::string& function, SourceFileInfo& info)
        {
            static const std::regex profile(
                R"re(\bGetProfile(?:String|Int|Binary)\s*\(\s*([^,]+?)\s*,\s*(?:_T|L|TEXT)?\s*\(?\s*"([^"]+)"\s*\)?\s*(?:,\s*([^;]*?))?\s*\)\s*[;),])re",
                std::regex::optimize);
            static const std::regex profileKey(
                R"re(\bGetProfile(?:String|Int|Binary)\s*\(\s*([^,]+?)\s*,\s*(?:_T|L|TEXT)?\s*\(?\s*"([^"]+)")re", std::regex::optimize);
            static const std::regex ini(
                R"re(\bGetPrivateProfile(?:String|Int)[AW]?\s*\(\s*([^,]+?)\s*,\s*(?:_T|L|TEXT)?\s*\(?\s*"([^"]+)")re",
                std::regex::optimize);
            static const std::regex reg(R"(\b(?:RegQueryValueEx|RegGetValue)[AW]?\s*\(([^;]*))", std::regex::optimize);
            static const std::regex env(
                R"re(\b(?:std::)?(?:getenv|_wgetenv|_dupenv_s|_wdupenv_s|GetEnvironmentVariable[AW]?)\s*\([^"]*"([^"]+)")re",
                std::regex::optimize);
            static const std::regex cli(
                R"re((?:==\s*(?:L|u8|_T\()?"(--?[A-Za-z][\w-]*)"|"(--?[A-Za-z][\w-]*)"\s*\)?\s*==|(?:strcmp|wcscmp|_stricmp|_wcsicmp|strcasecmp|lstrcmpi[AW]?)\s*\([^"]*"(--?[A-Za-z][\w-]*)"))re",
                std::regex::optimize);
            static const std::regex registryRoot(R"re(\bSetRegistryKey\s*\(\s*(?:_T|L|TEXT)?\s*\(?\s*"([^"]+)")re", std::regex::optimize);
            static const std::regex literal(R"re("((?:\\.|[^"\\])*)")re", std::regex::optimize);

            const int lineNo = static_cast<int>(index) + 1;
            std::smatch m;
            if (std::regex_search(code, m, profile) || std::regex_search(code, m, profileKey))
            {
                std::string value = m.size() > 3 ? m[3].str() : std::string();
                AddSetting(info, "registry", m[1].str(), m[2].str(), AssignedVariable(m.prefix().str()),
                    IsLiteralValue(Trim(value)) ? LiteralText(value) : std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, ini))
            {
                AddSetting(info, "ini", m[1].str(), m[2].str(), AssignedVariable(m.prefix().str()), std::string(),
                    function, lineNo);
            }

            if (std::regex_search(code, m, reg))
            {
                std::vector<std::string> literals;
                std::string args = m[1].str();
                for (std::sregex_iterator it(args.begin(), args.end(), literal), end; it != end; ++it)
                {
                    literals.push_back((*it)[1].str());
                }

                if (!literals.empty())
                {
                    AddSetting(info, "registry value", literals.size() > 1 ? literals.front() : std::string(),
                        literals.back(), std::string(), std::string(), function, lineNo);
                }
            }

            if (std::regex_search(code, m, env))
            {
                AddSetting(info, "environment", std::string(), m[1].str(), AssignedVariable(m.prefix().str()),
                    std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, cli))
            {
                std::string option = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();

                // The option's value usually goes into a variable on this line or the next.
                std::string variable;
                for (size_t k = index; k < lines.size() && k <= index + 2 && variable.empty(); ++k)
                {
                    std::string next = lines[k];
                    size_t eq = next.find('=');
                    while (eq != std::string::npos && eq + 1 < next.size() && (next[eq + 1] == '=' || (eq > 0 && next[eq - 1] == '=')))
                    {
                        eq = next.find('=', eq + 2);
                    }

                    if (eq != std::string::npos)
                    {
                        variable = AssignedVariable(next.substr(0, eq + 1));
                    }
                }

                AddSetting(info, "command line", std::string(), option, variable, std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, registryRoot))
            {
                AddSetting(info, "registry root", std::string(), m[1].str(), std::string(), std::string(), function,
                    lineNo);
            }
        }

        // Named constants with a literal value, outside functions: "static const int kMaxRetries = 3;",
        // "constexpr auto kTimeout = 30;", "#define MAX_PATH_LEN 260".
        void FindCppConstant(const std::string& code, int lineNo, SourceFileInfo& info)
        {
            static const std::regex constant(
                R"(^\s*(?:(?:static|inline|extern)\s+)*(?:constexpr|const)\s+[\w:<>\s\*&]*?\b(k[A-Z]\w*|[A-Z][A-Z0-9_]{2,})\s*(?:\[\s*\])?\s*=\s*([^;{]+);)",
                std::regex::optimize);
            static const std::regex define(R"(^\s*#\s*define\s+([A-Z][A-Z0-9_]{2,})\s+(.+?)\s*$)", std::regex::optimize);
            std::smatch m;
            if (std::regex_search(code, m, constant) || std::regex_search(code, m, define))
            {
                std::string value = Trim(m[2].str());
                if (IsLiteralValue(value))
                {
                    AddSetting(info, "constant", std::string(), m[1].str(), std::string(), value, std::string(), lineNo);
                }
            }
        }

        void ParseCpp(const std::vector<std::string>& lines, SourceFileInfo& info, bool isHeader)
        {
            static const std::regex classRe(
                R"(^\s*(?:template\s*<.*>\s*)?(class|struct)\s+(?:__declspec\([^)]*\)\s+|[A-Z_][A-Z0-9_]*\s+)?([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^;{]*)?(?:\{.*)?$)",
                std::regex::optimize);
            static const std::regex callRe(R"(((?:[A-Za-z_]\w*::)*~?[A-Za-z_]\w*)\s*\()", std::regex::optimize);
            static const std::regex prefixRe(R"(^[\w\s:<>,\*&~]*$)", std::regex::optimize);

            bool inBlock = false;
            int depth = 0;
            std::vector<int> namespaces;    // brace depth at which each open namespace (or extern "C") started
            bool pendingNamespace = false;  // "namespace x" whose { is on the next line (Allman style)
            std::string currentFunction;
            int functionDepth = -1;
            size_t functionIndex = 0;       // in info.symbols, while currentFunction is set
            bool pendingFunction = false;   // definition whose { is on a following line
            std::string pendingName;
            size_t pendingIndex = 0;
            int lastCodeLine = 0;

            for (size_t i = 0; i < lines.size(); ++i)
            {
                const int lineNo = static_cast<int>(i) + 1;
                std::string code = StripCppComments(lines[i], inBlock);
                std::string trimmed = Trim(code);
                int braceDelta = CountBraces(code);
                if (!trimmed.empty() && trimmed[0] == '#' && code.size() <= kMaxRegexLine)
                {
                    FindCppConstant(code, lineNo, info);
                }

                if (trimmed.empty() || trimmed[0] == '#' || code.size() > kMaxRegexLine)
                {
                    depth += braceDelta;
                    continue;
                }

                // Namespace braces are not nesting that matters: a function in "namespace a { namespace {" is
                // still at the top level.
                bool namespaceHeader = currentFunction.empty() &&
                                       (StartsWithWord(trimmed, "namespace") || trimmed.compare(0, 10, "extern \"C\"") == 0);
                bool opensNamespace = (namespaceHeader || (pendingNamespace && trimmed[0] == '{')) &&
                                      code.find('{') != std::string::npos;
                pendingNamespace = namespaceHeader && code.find('{') == std::string::npos && code.find(';') == std::string::npos;
                const int scopeDepth = depth - static_cast<int>(namespaces.size());

                try
                {
                    lastCodeLine = lineNo;
                    std::smatch m;
                    bool isClassLine = false;
                    bool definedHere = false;
                    if (std::regex_match(code, m, classRe) && code.find(';') == std::string::npos &&
                        trimmed.compare(0, 10, "enum class") != 0)
                    {
                        SymbolInfo sym;
                        sym.kind = m[1].str();
                        sym.name = m[2].str();
                        sym.signature = Shorten(trimmed, 200);
                        sym.line = lineNo;
                        info.symbols.push_back(sym);
                        isClassLine = true;
                    }

                    // Function definitions (and declarations in headers): name( ... ) with a type-like prefix.
                    if (!isClassLine && std::regex_search(code, m, callRe))
                    {
                        std::string name = m[1].str();
                        std::string prefix = Trim(code.substr(0, static_cast<size_t>(m.position(0))));
                        std::string simple = name.substr(name.rfind(':') == std::string::npos ? 0 : name.rfind(':') + 1);
                        bool qualified = name.find("::") != std::string::npos;

                        // A return type must precede the name; ": member_(x)," is a constructor initializer list.
                        bool typePrefix = !prefix.empty() && prefix[0] != ':' && prefix[0] != ',' &&
                                          std::regex_match(prefix, prefixRe) &&
                                          prefix.find("return") == std::string::npos &&
                                          prefix.compare(0, 4, "else") != 0 && prefix.find("case") == std::string::npos;
                        bool endsWithSemicolon = !trimmed.empty() && trimmed[trimmed.size() - 1] == ';';
                        bool isDefinition = (typePrefix || (qualified && prefix.empty())) && !IsCppKeyword(simple) &&
                                            (!endsWithSemicolon || isHeader) && scopeDepth <= 1 + (isHeader ? 1 : 0) &&
                                            currentFunction.empty() &&   // C++ has no nested functions
                                            !pendingFunction;            // the lines of a long parameter list

                        if (isDefinition && code.find('=') != std::string::npos &&
                            code.find('=') < static_cast<size_t>(m.position(0)))
                        {
                            isDefinition = false;  // "x = f(...)" is a call
                        }

                        if (isDefinition)
                        {
                            SymbolInfo sym;
                            sym.kind = "function";
                            sym.name = name;
                            sym.signature = Shorten(trimmed, 240);
                            sym.line = lineNo;
                            info.symbols.push_back(sym);
                            definedHere = true;
                            if (!endsWithSemicolon)
                            {
                                if (code.find('{') != std::string::npos)
                                {
                                    currentFunction = name;
                                    functionDepth = depth;
                                    functionIndex = info.symbols.size() - 1;
                                }
                                else
                                {
                                    pendingFunction = true;
                                    pendingName = name;
                                    pendingIndex = info.symbols.size() - 1;
                                }
                            }
                        }
                    }

                    if (pendingFunction && code.find('{') != std::string::npos && currentFunction != pendingName)
                    {
                        currentFunction = pendingName;
                        functionDepth = depth;
                        functionIndex = pendingIndex;
                        pendingFunction = false;
                    }
                    else if (pendingFunction && !definedHere && trimmed[trimmed.size() - 1] == ';')
                    {
                        pendingFunction = false;   // a declaration spread over several lines ended
                    }

                    if (currentFunction.empty() && scopeDepth <= 2)
                    {
                        FindCppConstant(code, lineNo, info);
                    }

                    FindCppSettings(lines, i, code, currentFunction, info);

                    // Calls made by the current function (the definition's own name is not a call).
                    if (!currentFunction.empty())
                    {
                        std::sregex_iterator it(code.begin(), code.end(), callRe), end;
                        for (bool first = true; it != end; ++it, first = false)
                        {
                            if (first && definedHere)
                            {
                                continue;
                            }

                            std::string name = (*it)[1].str();
                            std::string simple = name.substr(name.rfind(':') == std::string::npos ? 0 : name.rfind(':') + 1);
                            if (simple.size() < 2 || IsCppKeyword(simple))
                            {
                                continue;
                            }

                            AddCall(info.symbols[functionIndex], CallName(code, static_cast<size_t>(it->position(1)), name), lineNo);
                        }
                    }

                    // Messages: string literals passed to logging/throw/assert-like calls.
                    if (std::regex_search(code, CppMessageCall()))
                    {
                        std::vector<GuardInfo> guards;
                        bool guardsDone = false;
                        std::sregex_iterator it(code.begin(), code.end(), CppStringLiteral()), end;
                        for (; it != end; ++it)
                        {
                            std::string literal = (*it)[1].str();
                            if (literal.size() < 8)
                            {
                                continue;
                            }

                            if (!guardsDone && !currentFunction.empty())
                            {
                                guards = CppGuards(lines, info.symbols[functionIndex].line, lineNo);
                            }

                            guardsDone = true;
                            MessageInfo msg;
                            msg.text = literal;
                            msg.function = currentFunction;
                            msg.line = lineNo;
                            msg.guards = guards;
                            info.messages.push_back(msg);
                        }
                    }
                }
                catch (const std::regex_error&)
                {
                    // This line exhausted the regex engine (very long or pathological): skip it.
                }

                const int depthBefore = depth;
                depth += braceDelta;
                if (depth < 0)
                {
                    depth = 0;
                }

                if (opensNamespace && depth > depthBefore)
                {
                    namespaces.push_back(depthBefore);
                }

                while (!namespaces.empty() && depth <= namespaces.back())
                {
                    namespaces.pop_back();
                }

                if (!currentFunction.empty() && depth <= functionDepth)
                {
                    info.symbols[functionIndex].endLine = lineNo;
                    currentFunction.clear();
                    functionDepth = -1;
                }
            }

            if (!currentFunction.empty())
            {
                info.symbols[functionIndex].endLine = lastCodeLine;   // unbalanced braces: the file ended first
            }
        }

        int Indentation(const std::string& line)
        {
            int n = 0;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == ' ')
                {
                    ++n;
                }
                else if (line[i] == '\t')
                {
                    n += 4;
                }
                else
                {
                    break;
                }
            }

            return n;
        }

        bool IsPythonGuardHeader(const std::string& header)
        {
            return StartsWithWord(header, "if") || StartsWithWord(header, "elif") || StartsWithWord(header, "else") ||
                   StartsWithWord(header, "while") || StartsWithWord(header, "except") || StartsWithWord(header, "case");
        }

        // Python counterpart of CppGuards: blocks are found by indentation.
        std::vector<GuardInfo> PythonGuards(const std::vector<std::string>& lines, int fromLine, int targetLine)
        {
            struct Block
            {
                int indent;
                int line;
                std::string header;
            };

            std::vector<Block> stack;
            std::map<int, std::string> lastIf;   // indentation -> the latest if/elif header there
            std::vector<GuardInfo> guards;
            const int from = std::max(1, fromLine), to = std::min(static_cast<int>(lines.size()), targetLine);
            for (int n = from; n <= to; ++n)
            {
                const std::string& line = lines[static_cast<size_t>(n - 1)];
                std::string trimmed = Trim(line);
                if (trimmed.empty() || trimmed[0] == '#' || line.size() > kMaxRegexLine)
                {
                    continue;
                }

                int indent = Indentation(line);
                while (!stack.empty() && stack.back().indent >= indent)
                {
                    stack.pop_back();
                }

                if (n == to)
                {
                    for (size_t b = 0; b < stack.size(); ++b)
                    {
                        if (IsPythonGuardHeader(stack[b].header))
                        {
                            GuardInfo g = { stack[b].line, stack[b].header };
                            guards.push_back(g);
                        }
                    }

                    size_t colon = trimmed.find(':');   // "if not ok: raise ..."
                    if (StartsWithWord(trimmed, "if") && colon != std::string::npos && colon + 1 < trimmed.size())
                    {
                        GuardInfo g = { n, Shorten(trimmed.substr(0, colon + 1), 160) };
                        guards.push_back(g);
                    }

                    break;
                }

                if (trimmed[trimmed.size() - 1] == ':')
                {
                    std::string header = Shorten(trimmed, 160);
                    if (StartsWithWord(trimmed, "if") || StartsWithWord(trimmed, "elif"))
                    {
                        lastIf[indent] = header;
                    }
                    else if (StartsWithWord(trimmed, "else") && lastIf.count(indent))
                    {
                        header = Shorten("else (after: " + lastIf[indent] + ")", 200);
                    }

                    Block block = { indent, n, header };
                    stack.push_back(block);
                }
            }

            if (guards.size() > 4)
            {
                guards.erase(guards.begin(), guards.end() - 4);
            }

            return guards;
        }

        // Blanks out string literal contents so "f(x)" inside a message is not taken for a call.
        std::string BlankPythonStrings(const std::string& line)
        {
            std::string out = line;
            char quote = 0;
            for (size_t i = 0; i < out.size(); ++i)
            {
                char c = out[i];
                if (quote)
                {
                    if (c == '\\')
                    {
                        out[i] = ' ';
                        if (i + 1 < out.size())
                        {
                            out[++i] = ' ';
                        }
                    }
                    else if (c == quote)
                    {
                        quote = 0;
                    }
                    else
                    {
                        out[i] = ' ';
                    }
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                }
                else if (c == '#')
                {
                    out.erase(i);
                    break;
                }
            }

            return out;
        }

        bool IsPythonKeyword(const std::string& name)
        {
            static const char* const kWords[] = { "if", "elif", "while", "for", "return", "not", "and", "or", "in",
                "is", "lambda", "with", "assert", "yield", "except", "print", "del", "await", "async", "def", "class",
                "super", "len", "str", "int", "float", "bool", "list", "dict", "set", "tuple", "isinstance", "range",
                "enumerate", "zip", "min", "max", "open" };
            for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i)
            {
                if (name == kWords[i])
                {
                    return true;
                }
            }

            return false;
        }

        // Environment, argparse, configparser and settings-object reads in one line of Python.
        void FindPythonSettings(const std::string& line, int lineNo, const std::string& function, SourceFileInfo& info)
        {
            static const std::regex env(
                R"re(\bos\.(?:environ\.get|getenv)\(\s*['"]([^'"]+)['"]\s*(?:,\s*([^)]+))?\)|\bos\.environ\[\s*['"]([^'"]+)['"]\s*\])re",
                std::regex::optimize);
            static const std::regex argument(R"re(\badd_argument\(\s*(?:['"](-[\w-]+)['"]\s*,\s*)?['"](--?[\w-]+)['"])re",
                std::regex::optimize);
            static const std::regex argumentDefault(R"(\bdefault\s*=\s*([^,)]+))", std::regex::optimize);
            static const std::regex settings(
                R"re(\b(\w*(?:config|cfg|conf|settings|prefs|options|parser|ini)\w*)\s*(?:\.(Read|ReadInt|ReadBool|ReadFloat|ReadDouble|value|get|getint|getfloat|getboolean)\(\s*|\[\s*)['"]([^'"]+)['"](?:\s*,\s*(?:fallback\s*=\s*)?([^)\]]+))?)re",
                std::regex::optimize | std::regex::icase);

            std::smatch m;
            if (std::regex_search(line, m, env))
            {
                std::string key = m[1].matched ? m[1].str() : m[3].str();
                AddSetting(info, "environment", std::string(), key, AssignedVariable(m.prefix().str()),
                    m[2].matched && IsLiteralValue(Trim(m[2].str())) ? LiteralText(m[2].str()) : std::string(), function,
                    lineNo);
                return;
            }

            if (std::regex_search(line, m, argument))
            {
                std::smatch d;
                std::string value = std::regex_search(line, d, argumentDefault) ? Trim(d[1].str()) : std::string();
                AddSetting(info, "command line", std::string(), m[2].str(), std::string(),
                    IsLiteralValue(value) ? LiteralText(value) : value, function, lineNo);
                return;
            }

            if (std::regex_search(line, m, settings))
            {
                std::string object = ToLowerAscii(m[1].str()), method = m[2].str();
                std::string key = m[3].str(), rest = m[4].matched ? Trim(m[4].str()) : std::string();
                std::string section;

                // configparser: parser.get("section", "key") has two names; dict.get("key", "default") one.
                bool twoNames = !rest.empty() && (rest[0] == '"' || rest[0] == '\'') &&
                                ((method.compare(0, 3, "get") == 0 && method.size() > 3) ||
                                    object.find("parser") != std::string::npos || object.find("ini") != std::string::npos);
                if (twoNames)
                {
                    section = key;
                    key = LiteralText(rest);
                    rest.clear();
                }

                AddSetting(info, "settings", section, key, AssignedVariable(m.prefix().str()),
                    IsLiteralValue(rest) ? LiteralText(rest) : rest, function, lineNo);
            }
        }

        void ParsePython(const std::vector<std::string>& lines, SourceFileInfo& info)
        {
            static const std::regex classRe(R"(^(\s*)class\s+([A-Za-z_]\w*)\s*(\([^)]*\))?\s*:)", std::regex::optimize);
            static const std::regex defRe(R"(^(\s*)(?:async\s+)?def\s+([A-Za-z_]\w*)\s*\(([^)]*)\)?)", std::regex::optimize);
            static const std::regex messageCall(
                R"((?:\blog(?:ger|ging)?\.(?:debug|info|warning|warn|error|exception|critical|fatal)|\bprint|\braise\b|\bwarnings\.warn|\.log\(|\berror\(|\bfail|Error\(|Exception\())",
                std::regex::optimize);
            static const std::regex literalRe(R"re((?:[rRbBuUfF]{0,2})("|')((?:\\.|(?!\1).)*)\1)re", std::regex::optimize);
            static const std::regex callRe(R"(([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\s*\()", std::regex::optimize);
            static const std::regex constantRe(R"(^([A-Z][A-Z0-9_]{2,})\s*(?::\s*[\w\[\], .]+)?\s*=\s*(.+?)\s*$)", std::regex::optimize);

            struct Scope
            {
                int indent;
                std::string name;
            };

            std::vector<Scope> classes;
            std::string currentFunction;
            int functionIndent = -1;
            size_t functionIndex = 0;
            int lastCodeLine = 0;

            for (size_t i = 0; i < lines.size(); ++i)
            {
                const std::string& line = lines[i];
                const int lineNo = static_cast<int>(i) + 1;
                std::string trimmed = Trim(line);
                if (trimmed.empty() || trimmed[0] == '#' || line.size() > kMaxRegexLine)
                {
                    continue;
                }

                int indent = Indentation(line);

                while (!classes.empty() && indent <= classes.back().indent)
                    classes.pop_back();
                if (!currentFunction.empty() && indent <= functionIndent)
                {
                    info.symbols[functionIndex].endLine = lastCodeLine;
                    currentFunction.clear();
                    functionIndent = -1;
                }

                try
                {
                    lastCodeLine = lineNo;
                    std::smatch m;
                    if (std::regex_search(line, m, classRe))
                    {
                        SymbolInfo sym;
                        sym.kind = "class";
                        sym.name = classes.empty() ? m[2].str() : classes.back().name + "." + m[2].str();
                        sym.signature = Shorten(trimmed, 200);
                        sym.line = lineNo;
                        info.symbols.push_back(sym);
                        Scope s = { indent, sym.name };
                        classes.push_back(s);
                        continue;
                    }

                    if (std::regex_search(line, m, defRe))
                    {
                        SymbolInfo sym;
                        sym.kind = "function";
                        sym.name = classes.empty() ? m[2].str() : classes.back().name + "." + m[2].str();
                        sym.signature = Shorten(trimmed, 240);
                        sym.line = lineNo;
                        info.symbols.push_back(sym);
                        currentFunction = sym.name;
                        functionIndent = indent;
                        functionIndex = info.symbols.size() - 1;
                        continue;
                    }

                    // Module or class constants with a literal value: TIMEOUT_S = 30, MODEL_NAME = "small".
                    if (currentFunction.empty() && std::regex_search(trimmed, m, constantRe) && IsLiteralValue(m[2].str()))
                    {
                        AddSetting(info, "constant", std::string(), m[1].str(), std::string(), m[2].str(), std::string(), lineNo);
                    }

                    FindPythonSettings(line, lineNo, currentFunction, info);

                    if (!currentFunction.empty())
                    {
                        std::string code = BlankPythonStrings(line);
                        for (std::sregex_iterator it(code.begin(), code.end(), callRe), end; it != end; ++it)
                        {
                            std::string name = (*it)[1].str();
                            size_t dot = name.rfind('.');
                            std::string simple = dot == std::string::npos ? name : name.substr(dot + 1);
                            if (simple.size() < 2 || IsPythonKeyword(simple))
                            {
                                continue;
                            }

                            size_t before = dot == std::string::npos ? std::string::npos : name.rfind('.', dot - 1);
                            AddCall(info.symbols[functionIndex], before == std::string::npos ? name : name.substr(before + 1), lineNo);
                        }
                    }

                    if (std::regex_search(line, messageCall))
                    {
                        std::vector<GuardInfo> guards;
                        bool guardsDone = false;
                        std::sregex_iterator it(line.begin(), line.end(), literalRe), end;
                        for (; it != end; ++it)
                        {
                            std::string literal = (*it)[2].str();
                            if (literal.size() < 8)
                            {
                                continue;
                            }

                            if (!guardsDone && !currentFunction.empty())
                            {
                                guards = PythonGuards(lines, info.symbols[functionIndex].line, lineNo);
                            }

                            guardsDone = true;
                            MessageInfo msg;
                            msg.text = literal;
                            msg.function = currentFunction;
                            msg.line = lineNo;
                            msg.guards = guards;
                            info.messages.push_back(msg);
                        }
                    }
                }
                catch (const std::regex_error&)
                {
                    // This line exhausted the regex engine (very long or pathological): skip it.
                }

            }

            if (!currentFunction.empty())
            {
                info.symbols[functionIndex].endLine = lastCodeLine;
            }
        }

        // ----- configuration files -----

        void AddEntry(ConfigFileInfo& file, const std::string& key, const std::string& value, int line)
        {
            if (key.empty() || file.entries.size() >= 300)
            {
                return;
            }

            ConfigEntry e;
            e.key = Shorten(key, 160);
            e.value = Shorten(value, 120);
            e.line = line;
            file.entries.push_back(e);
        }

        std::string Unquote(const std::string& text)
        {
            std::string t = Trim(text);
            if (t.size() >= 2 && (t[0] == '"' || t[0] == '\'') && t[t.size() - 1] == t[0])
            {
                return t.substr(1, t.size() - 2);
            }

            return t;
        }

        // A value with its trailing " # comment" removed (not inside quotes).
        std::string WithoutComment(const std::string& value)
        {
            char quote = 0;
            for (size_t i = 0; i < value.size(); ++i)
            {
                char c = value[i];
                if (quote)
                {
                    quote = c == quote ? 0 : quote;
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                }
                else if ((c == '#' || c == ';') && i > 0 && std::isspace(static_cast<unsigned char>(value[i - 1])))
                {
                    return Trim(value.substr(0, i));
                }
            }

            return Trim(value);
        }

        void ParseIniLike(const std::vector<std::string>& lines, ConfigFileInfo& file, bool toml)
        {
            static const std::regex section(R"(^\s*\[\[?\s*([^\]]+?)\s*\]\]?\s*$)", std::regex::optimize);
            static const std::regex entry(R"(^\s*(?:export\s+)?([A-Za-z_][\w.\- ]*?)\s*[=:]\s*(.*)$)", std::regex::optimize);
            std::string current;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const std::string& line = lines[i];
                std::string trimmed = Trim(line);
                if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || line.size() > kMaxRegexLine)
                {
                    continue;
                }

                std::smatch m;
                if (std::regex_match(line, m, section))
                {
                    current = m[1].str();
                }
                else if (std::regex_match(line, m, entry))
                {
                    std::string key = Trim(m[1].str());
                    std::string value = toml ? WithoutComment(m[2].str()) : Trim(m[2].str());
                    AddEntry(file, current.empty() ? key : current + "." + key, Unquote(value), static_cast<int>(i) + 1);
                }
            }
        }

        void ParseYaml(const std::vector<std::string>& lines, ConfigFileInfo& file)
        {
            static const std::regex entry(R"(^(\s*)(-\s+)?([A-Za-z_][\w.\-]*)\s*:(?:\s+(.*))?$)", std::regex::optimize);
            std::vector<std::pair<int, std::string>> parents;   // indentation, key
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const std::string& line = lines[i];
                std::string trimmed = Trim(line);
                if (trimmed.empty() || trimmed[0] == '#' || trimmed == "---" || line.size() > kMaxRegexLine)
                {
                    continue;
                }

                std::smatch m;
                if (!std::regex_match(line, m, entry))
                {
                    continue;
                }

                int indent = static_cast<int>(m[1].length()) + static_cast<int>(m[2].length());
                while (!parents.empty() && parents.back().first >= indent)
                {
                    parents.pop_back();
                }

                std::string path;
                for (size_t p = 0; p < parents.size(); ++p)
                {
                    path += parents[p].second + ".";
                }

                path += m[3].str();
                std::string value = m[4].matched ? WithoutComment(m[4].str()) : std::string();
                if (value.empty() || value == "|" || value == ">" || value == "|-" || value == ">-")
                {
                    parents.push_back(std::make_pair(indent, m[3].str()));
                }
                else
                {
                    AddEntry(file, path, Unquote(value), static_cast<int>(i) + 1);
                }
            }
        }

        // JSON leaves as "a.b[0].c" = value; the line is found by searching for each key in document order.
        void ParseJson(const std::string& text, ConfigFileInfo& file)
        {
            nlohmann::ordered_json doc = nlohmann::ordered_json::parse(text, nullptr, false, true);
            if (doc.is_discarded())
            {
                return;
            }

            size_t searchFrom = 0;
            std::function<void(const nlohmann::ordered_json&, const std::string&, const std::string&, int)> walk;
            walk = [&](const nlohmann::ordered_json& node, const std::string& path, const std::string& lastKey, int level)
            {
                if (file.entries.size() >= 300 || level > 6)
                {
                    return;
                }

                if (node.is_object())
                {
                    for (nlohmann::ordered_json::const_iterator it = node.begin(); it != node.end(); ++it)
                    {
                        walk(it.value(), path.empty() ? it.key() : path + "." + it.key(), it.key(), level + 1);
                    }
                }
                else if (node.is_array())
                {
                    for (size_t i = 0; i < node.size() && i < 20; ++i)
                    {
                        walk(node[i], path + "[" + std::to_string(i) + "]", lastKey, level + 1);
                    }
                }
                else if (!path.empty())
                {
                    std::string value = node.is_string() ? node.get<std::string>() : node.dump();
                    size_t at = text.find("\"" + lastKey + "\"", searchFrom);
                    if (at == std::string::npos)
                    {
                        at = text.find("\"" + lastKey + "\"");
                    }
                    else
                    {
                        searchFrom = at + lastKey.size() + 2;
                    }

                    int line = at == std::string::npos ? 0 : 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
                    AddEntry(file, path, value, line);
                }
            };

            walk(doc, std::string(), std::string(), 0);
        }

        // ----- Java -----

        // System properties, environment, Properties/Spring/Preferences reads and command-line options in Java.
        void FindJavaSettings(const std::string& code, int lineNo, const std::string& function, SourceFileInfo& info)
        {
            static const std::regex systemProperty(R"re(\b(?:System\.getProperty|Integer\.getInteger|Long\.getLong|Boolean\.getBoolean)\(\s*"([^"]+)"\s*(?:,\s*([^)]+))?\))re",
                std::regex::optimize);
            static const std::regex env(R"re(\bSystem\.getenv\(\s*"([^"]+)"\s*\))re", std::regex::optimize);
            static const std::regex springValue(R"re(@Value\(\s*"\$\{([^}:]+)(?::([^}]*))?\}"\s*\))re", std::regex::optimize);
            static const std::regex property(
                R"re(\b(\w*(?:props|properties|config|cfg|conf|settings|prefs|preferences|environment|env)\w*)\s*\.\s*(?:getProperty|get|getString|getInt|getInteger|getBoolean|getLong|getDouble|getValue|getRequiredProperty)\(\s*"([^"]+)"\s*(?:,\s*([^)]+))?\))re",
                std::regex::optimize | std::regex::icase);
            static const std::regex cli(
                R"re("(--?[A-Za-z][\w-]*)"\s*\.\s*equals(?:IgnoreCase)?\(|\.equals(?:IgnoreCase)?\(\s*"(--?[A-Za-z][\w-]*)"\s*\)|\bcase\s+"(--?[A-Za-z][\w-]*)"\s*(?::|->))re",
                std::regex::optimize);

            std::smatch m;
            if (std::regex_search(code, m, systemProperty))
            {
                std::string value = m[2].matched ? Trim(m[2].str()) : std::string();
                AddSetting(info, "system property", std::string(), m[1].str(), AssignedVariable(m.prefix().str()),
                    IsLiteralValue(value) ? LiteralText(value) : std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, env))
            {
                AddSetting(info, "environment", std::string(), m[1].str(), AssignedVariable(m.prefix().str()),
                    std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, springValue))
            {
                AddSetting(info, "property", std::string(), m[1].str(), std::string(), m[2].matched ? m[2].str() : std::string(),
                    function, lineNo);
            }
            else if (std::regex_search(code, m, property))
            {
                std::string value = m[3].matched ? Trim(m[3].str()) : std::string();
                AddSetting(info, "property", std::string(), m[2].str(), AssignedVariable(m.prefix().str()),
                    IsLiteralValue(value) ? LiteralText(value) : std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, cli))
            {
                std::string option = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();
                AddSetting(info, "command line", std::string(), option, std::string(), std::string(), function, lineNo);
            }
        }

        // Environment, App.config, IConfiguration (appsettings.json), registry, Settings.Default and
        // command-line options in C#.
        void FindCSharpSettings(const std::string& code, int lineNo, const std::string& function, SourceFileInfo& info)
        {
            static const std::regex env(R"re(\bEnvironment\.GetEnvironmentVariable\(\s*@?"([^"]+)")re", std::regex::optimize);
            static const std::regex appSettings(R"re(\bConfigurationManager\.(AppSettings|ConnectionStrings)\[\s*@?"([^"]+)"\s*\])re",
                std::regex::optimize);
            static const std::regex configuration(
                R"re(\b(\w*(?:config|configuration|settings|options|section)\w*)\s*(?:\[\s*@?"([^"]+)"\s*\]|\.(?:GetValue<[^>]+>|GetSection|GetConnectionString|GetRequiredSection)\(\s*@?"([^"]+)"\s*(?:,\s*([^)]+))?\)))re",
                std::regex::optimize | std::regex::icase);
            static const std::regex registry(R"re(\bRegistry\.GetValue\(\s*@?"([^"]*)"\s*,\s*@?"([^"]*)")re", std::regex::optimize);
            static const std::regex registryKey(R"re(\b\w*[Kk]ey\??\.GetValue\(\s*@?"([^"]+)")re", std::regex::optimize);
            static const std::regex userSettings(R"(\bSettings\.Default\.([A-Za-z_]\w*))", std::regex::optimize);
            static const std::regex cli(
                R"re("(--?[A-Za-z][\w-]*)"\s*\.\s*Equals\(|\.Equals\(\s*"(--?[A-Za-z][\w-]*)"|\bcase\s+"(--?[A-Za-z][\w-]*)"\s*:|==\s*"(--?[A-Za-z][\w-]*)"|"(--?[A-Za-z][\w-]*)"\s*==)re",
                std::regex::optimize);

            std::smatch m;
            if (std::regex_search(code, m, env))
            {
                AddSetting(info, "environment", std::string(), m[1].str(), AssignedVariable(m.prefix().str()), std::string(),
                    function, lineNo);
            }

            if (std::regex_search(code, m, appSettings))
            {
                AddSetting(info, "app setting", m[1].str(), m[2].str(), AssignedVariable(m.prefix().str()), std::string(),
                    function, lineNo);
            }
            else if (std::regex_search(code, m, configuration))
            {
                std::string key = m[2].matched ? m[2].str() : m[3].str();
                std::string value = m[4].matched ? Trim(m[4].str()) : std::string();
                AddSetting(info, "configuration", std::string(), key, AssignedVariable(m.prefix().str()),
                    IsLiteralValue(value) ? LiteralText(value) : std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, registry))
            {
                AddSetting(info, "registry value", m[1].str(), m[2].str(), AssignedVariable(m.prefix().str()), std::string(),
                    function, lineNo);
            }
            else if (code.find("Registry") != std::string::npos || std::regex_search(code, m, registryKey))
            {
                if (std::regex_search(code, m, registryKey))
                {
                    AddSetting(info, "registry value", std::string(), m[1].str(), AssignedVariable(m.prefix().str()),
                        std::string(), function, lineNo);
                }
            }

            if (std::regex_search(code, m, userSettings))
            {
                AddSetting(info, "settings", std::string(), m[1].str(), AssignedVariable(m.prefix().str()), std::string(),
                    function, lineNo);
            }

            if (std::regex_search(code, m, cli))
            {
                std::string option;
                for (size_t g = 1; g < m.size() && option.empty(); ++g)
                {
                    option = m[g].matched ? m[g].str() : std::string();
                }

                AddSetting(info, "command line", std::string(), option, std::string(), std::string(), function, lineNo);
            }
        }

        bool IsJavaKeyword(const std::string& name)
        {
            static const char* const kWords[] = { "if", "for", "while", "switch", "return", "catch", "new", "throw",
                "else", "case", "do", "try", "synchronized", "super", "this", "assert", "instanceof", "yield", "default",
                // C#
                "using", "lock", "foreach", "nameof", "typeof", "sizeof", "await", "is", "as", "in", "out", "ref",
                "checked", "unchecked", "fixed", "stackalloc", "when", "var", "base", "get", "set", "init", "value" };
            for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i)
            {
                if (name == kWords[i])
                {
                    return true;
                }
            }

            return false;
        }

        // Java and C#: classes (also interface/enum/record/struct) and methods named "Outer.Inner.method", like
        // Python's. Brace nesting, comments, string literals and the if/else/catch conditions work as in C++.
        void ParseJava(const std::vector<std::string>& lines, SourceFileInfo& info, bool csharp)
        {
            static const std::regex javaClassRe(
                R"(^\s*(?:@\w+(?:\([^)]*\))?\s+)*(?:(?:public|protected|private|static|final|abstract|sealed|non-sealed|strictfp)\s+)*(class|interface|enum|record)\s+([A-Za-z_$][\w$]*))",
                std::regex::optimize);
            static const std::regex javaMethodRe(
                R"(^\s*(?:@\w+(?:\([^)]*\))?\s+)*((?:(?:public|protected|private|static|final|abstract|synchronized|native|default|strictfp)\s+)*)(?:<[^>]+>\s+)?(?:([\w.$]+(?:<[^()]*>)?(?:\[\])*)\s+)?([A-Za-z_$][\w$]*)\s*\()",
                std::regex::optimize);
            static const std::regex javaConstantRe(
                R"(\bstatic\s+final\s+[\w<>\[\],.\s]+?\s([A-Z][A-Z0-9_]{2,})\s*=\s*([^;]+);)", std::regex::optimize);
            static const std::regex javaMessageCall(
                R"((?:\b(?:log|logger|LOG|LOGGER|Log|Logger)\.(?:trace|debug|info|warn|warning|error|fatal|severe|log|logp)\(|\bthrow\s+new\b|System\.(?:out|err)\.print|\bnew\s+\w*(?:Exception|Error)\(|\bassert\b))",
                std::regex::optimize);
            static const std::regex csClassRe(
                R"(^\s*(?:\[[^\]]*\]\s*)*(?:(?:public|protected|private|internal|static|sealed|abstract|partial|readonly|ref|unsafe|new|file)\s+)*(class|interface|enum|struct|record(?:\s+class|\s+struct)?)\s+([A-Za-z_]\w*))",
                std::regex::optimize);
            static const std::regex csMethodRe(
                R"(^\s*(?:\[[^\]]*\]\s*)*((?:(?:public|protected|private|internal|static|virtual|override|abstract|sealed|async|extern|unsafe|new|partial|readonly)\s+)*)(?:([\w.]+(?:<[^()]*>)?\??(?:\[\])*\??)\s+)?([A-Za-z_]\w*)\s*(?:<[^()]*>)?\s*\()",
                std::regex::optimize);
            static const std::regex csConstantRe(
                R"(\b(?:const|static\s+readonly)\s+[\w<>\[\],.?]+\s+([A-Za-z_]\w*)\s*=\s*([^;]+);)", std::regex::optimize);
            static const std::regex csMessageCall(
                R"((?:\.Log(?:Trace|Debug|Information|Warning|Error|Critical)\(|\b(?:log|logger|_logger|_log|Log|Logger)\.(?:Trace|Debug|Info|Information|Warn|Warning|Error|Fatal|Critical|Verbose)\(|\bthrow\s+new\b|\bConsole\.(?:Error\.)?Write(?:Line)?\(|\b(?:Debug|Trace)\.(?:Write|WriteLine|TraceError|TraceWarning|Fail|Assert)\(|\bnew\s+\w*Exception\())",
                std::regex::optimize);
            static const std::regex callRe(R"(([A-Za-z_$][\w$]*)\s*\()", std::regex::optimize);
            const std::regex& classRe = csharp ? csClassRe : javaClassRe;
            const std::regex& methodRe = csharp ? csMethodRe : javaMethodRe;
            const std::regex& constantRe = csharp ? csConstantRe : javaConstantRe;
            const std::regex& messageCall = csharp ? csMessageCall : javaMessageCall;

            struct Scope
            {
                int depth;   // the class body is at depth + 1
                std::string name;
            };

            std::vector<Scope> classes;
            std::string pendingClass;
            bool inBlock = false;
            int depth = 0;
            std::string currentFunction;
            int functionDepth = -1;
            size_t functionIndex = 0;
            bool pendingFunction = false;
            size_t pendingIndex = 0;
            int lastCodeLine = 0;

            for (size_t i = 0; i < lines.size(); ++i)
            {
                const int lineNo = static_cast<int>(i) + 1;
                std::string code = StripCppComments(lines[i], inBlock);
                std::string trimmed = Trim(code);
                int braceDelta = CountBraces(code);
                bool directive = trimmed.empty() ? false : csharp ? currentFunction.empty() && trimmed[trimmed.size() - 1] == ';' &&
                                              (StartsWithWord(trimmed, "using") || StartsWithWord(trimmed, "namespace"))
                                        : StartsWithWord(trimmed, "import") || StartsWithWord(trimmed, "package");
                if (trimmed.empty() || code.size() > kMaxRegexLine || directive)
                {
                    depth += braceDelta;
                    continue;
                }

                try
                {
                    lastCodeLine = lineNo;
                    std::smatch m;
                    bool definedHere = false;
                    std::string outer = classes.empty() ? std::string() : classes.back().name;
                    if (currentFunction.empty() && std::regex_search(code, m, classRe))
                    {
                        SymbolInfo sym;
                        sym.kind = m[1].str() == "struct" ? "struct" : "class";
                        sym.name = outer.empty() ? m[2].str() : outer + "." + m[2].str();
                        sym.signature = Shorten(trimmed, 200);
                        sym.line = lineNo;
                        info.symbols.push_back(sym);
                        if (code.find('{') != std::string::npos)
                        {
                            Scope s = { depth, sym.name };
                            classes.push_back(s);
                        }
                        else
                        {
                            pendingClass = sym.name;
                        }
                    }
                    else if (!pendingClass.empty() && code.find('{') != std::string::npos)
                    {
                        Scope s = { depth, pendingClass };
                        classes.push_back(s);
                        pendingClass.clear();
                    }
                    else if (currentFunction.empty() && !classes.empty() && depth == classes.back().depth + 1 &&
                             std::regex_search(code, m, methodRe))
                    {
                        // A method sits directly in a class body, has a return type (or is the constructor) and is not
                        // a statement such as "return x(...)" or "new Foo(...)".
                        std::string type = m[2].str(), name = m[3].str();
                        const std::string& scope = classes.back().name;
                        std::string className = scope.substr(scope.rfind('.') == std::string::npos ? 0 : scope.rfind('.') + 1);
                        bool constructor = type.empty() && name == className;
                        bool method = !type.empty() && !IsJavaKeyword(type) && !IsJavaKeyword(name);
                        size_t equals = code.find('=');
                        bool assignment = equals != std::string::npos && equals < static_cast<size_t>(m.position(3));
                        if ((constructor || method) && !assignment)
                        {
                            SymbolInfo sym;
                            sym.kind = "function";
                            sym.name = classes.back().name + "." + name;
                            sym.signature = Shorten(trimmed, 240);
                            sym.line = lineNo;
                            info.symbols.push_back(sym);
                            definedHere = true;
                            bool endsWithSemicolon = trimmed[trimmed.size() - 1] == ';';   // abstract or interface method
                            if (endsWithSemicolon && code.find("=>") != std::string::npos)
                            {
                                info.symbols.back().endLine = lineNo;   // C# expression-bodied member: "int F() => x;"
                            }
                            else if (!endsWithSemicolon)
                            {
                                if (code.find('{') != std::string::npos)
                                {
                                    currentFunction = sym.name;
                                    functionDepth = depth;
                                    functionIndex = info.symbols.size() - 1;
                                }
                                else
                                {
                                    pendingFunction = true;
                                    pendingIndex = info.symbols.size() - 1;
                                }
                            }
                        }
                    }

                    if (pendingFunction && !definedHere)
                    {
                        if (code.find('{') != std::string::npos)
                        {
                            currentFunction = info.symbols[pendingIndex].name;
                            functionDepth = depth;
                            functionIndex = pendingIndex;
                            pendingFunction = false;
                        }
                        else if (code.find(';') != std::string::npos)
                        {
                            pendingFunction = false;   // a multi-line abstract declaration
                        }
                    }

                    if (currentFunction.empty() && std::regex_search(code, m, constantRe) && IsLiteralValue(Trim(m[2].str())))
                    {
                        AddSetting(info, "constant", std::string(), m[1].str(), std::string(), Trim(m[2].str()), std::string(), lineNo);
                    }

                    if (csharp)
                    {
                        FindCSharpSettings(code, lineNo, currentFunction, info);
                    }
                    else
                    {
                        FindJavaSettings(code, lineNo, currentFunction, info);
                    }

                    if (!currentFunction.empty())
                    {
                        std::sregex_iterator it(code.begin(), code.end(), callRe), end;
                        for (bool first = true; it != end; ++it, first = false)
                        {
                            if (first && definedHere)
                            {
                                continue;
                            }

                            std::string name = (*it)[1].str();
                            size_t at = static_cast<size_t>(it->position(1));
                            bool constructed = at >= 4 && code.compare(at - 4, 4, "new ") == 0;
                            if (name.size() < 2 || IsJavaKeyword(name) || (!constructed && std::isupper(static_cast<unsigned char>(name[0])) &&
                                                                             name.size() > 1 && std::isupper(static_cast<unsigned char>(name[1]))))
                            {
                                continue;   // keywords, and CONSTANT-like names
                            }

                            AddCall(info.symbols[functionIndex], CallName(code, at, name), lineNo);
                        }
                    }

                    if (std::regex_search(code, messageCall))
                    {
                        std::vector<GuardInfo> guards;
                        bool guardsDone = false;
                        std::sregex_iterator it(code.begin(), code.end(), CppStringLiteral()), end;
                        for (; it != end; ++it)
                        {
                            std::string literal = (*it)[1].str();
                            if (literal.size() < 8)
                            {
                                continue;
                            }

                            if (!guardsDone && !currentFunction.empty())
                            {
                                guards = CppGuards(lines, info.symbols[functionIndex].line, lineNo);
                            }

                            guardsDone = true;
                            MessageInfo msg;
                            msg.text = literal;
                            msg.function = currentFunction;
                            msg.line = lineNo;
                            msg.guards = guards;
                            info.messages.push_back(msg);
                        }
                    }
                }
                catch (const std::regex_error&)
                {
                    // This line exhausted the regex engine (very long or pathological): skip it.
                }

                depth += braceDelta;
                if (depth < 0)
                {
                    depth = 0;
                }

                if (!currentFunction.empty() && depth <= functionDepth)
                {
                    info.symbols[functionIndex].endLine = lineNo;
                    currentFunction.clear();
                    functionDepth = -1;
                }

                while (!classes.empty() && depth <= classes.back().depth)
                {
                    classes.pop_back();
                }
            }

            if (!currentFunction.empty())
            {
                info.symbols[functionIndex].endLine = lastCodeLine;
            }
        }

        // ----- Scala -----

        bool IsScalaGuardHeader(const std::string& head)
        {
            return StartsWithWord(head, "if") || StartsWithWord(head, "else") || StartsWithWord(head, "while") ||
                   StartsWithWord(head, "case") || StartsWithWord(head, "catch");
        }

        // "if (x) {" -> "if (x)"; "if x then" -> "if x then"; "case e: IOException =>" -> itself; "else {" -> "else".
        std::string ScalaCondition(const std::string& head)
        {
            if (StartsWithWord(head, "case"))
            {
                size_t arrow = head.find("=>");
                return Shorten(Trim(arrow == std::string::npos ? head : head.substr(0, arrow + 2)), 160);
            }

            size_t keyword = StartsWithWord(head, "else") ? (StartsWithWord(Trim(head.substr(4)), "if") ? head.find("if") + 2 : 4)
                                                          : head.find_first_of(" (");
            std::string rest = keyword == std::string::npos ? std::string() : Trim(head.substr(keyword));
            if (!rest.empty() && rest[0] == '(')
            {
                return ConditionHeader(head);
            }

            for (const char* word : { " then", " do" })
            {
                size_t at = head.find(word);
                if (at != std::string::npos)
                {
                    return Shorten(Trim(head.substr(0, at + std::strlen(word))), 160);
                }
            }

            std::string out = Trim(head);
            while (!out.empty() && (out[out.size() - 1] == '{' || out[out.size() - 1] == ':'))
            {
                out = Trim(out.substr(0, out.size() - 1));
            }

            return Shorten(out, 160);
        }

        // Scala blocks are found by indentation, which both brace style and Scala 3's braceless style follow; a
        // brace-less "if (x)" with its statement on the next, deeper line is covered too.
        std::vector<GuardInfo> ScalaGuards(const std::vector<std::string>& lines, int fromLine, int targetLine)
        {
            struct Block
            {
                int indent;
                int line;
                std::string header;
            };

            std::vector<Block> stack;
            std::map<int, std::string> lastIf;
            std::vector<GuardInfo> guards;
            bool inBlock = false;
            const int from = std::max(1, fromLine), to = std::min(static_cast<int>(lines.size()), targetLine);
            for (int n = from; n <= to; ++n)
            {
                const std::string& raw = lines[static_cast<size_t>(n - 1)];
                std::string trimmed = Trim(StripCppComments(raw, inBlock));
                if (trimmed.empty() || raw.size() > kMaxRegexLine)
                {
                    continue;
                }

                int indent = Indentation(raw);
                while (!stack.empty() && stack.back().indent >= indent)
                {
                    stack.pop_back();
                }

                std::string head = trimmed[0] == '}' ? Trim(trimmed.substr(1)) : trimmed;
                if (n == to)
                {
                    for (size_t b = 0; b < stack.size(); ++b)
                    {
                        GuardInfo g = { stack[b].line, stack[b].header };
                        guards.push_back(g);
                    }

                    if (StartsWithWord(head, "if"))   // "if (bad) throw ..." on the line itself
                    {
                        GuardInfo g = { n, ScalaCondition(head) };
                        guards.push_back(g);
                    }

                    break;
                }

                if (IsScalaGuardHeader(head))
                {
                    std::string header = ScalaCondition(head);
                    if (StartsWithWord(head, "if") || (StartsWithWord(head, "else") && header.find("if") != std::string::npos))
                    {
                        lastIf[indent] = header;
                    }
                    else if (StartsWithWord(head, "else") && lastIf.count(indent))
                    {
                        header = Shorten("else (after: " + lastIf[indent] + ")", 200);
                    }

                    Block block = { indent, n, header };
                    stack.push_back(block);
                }
            }

            if (guards.size() > 4)
            {
                guards.erase(guards.begin(), guards.end() - 4);
            }

            return guards;
        }

        // sys.env / sys.props / System.getenv / getProperty, Typesafe Config, scopt and argument matching in Scala.
        void FindScalaSettings(const std::string& code, int lineNo, const std::string& function, SourceFileInfo& info)
        {
            static const std::regex env(
                R"re(\bsys\.env\.(?:get|getOrElse)\(\s*"([^"]+)"(?:\s*,\s*([^)]+))?\)|\bsys\.env\(\s*"([^"]+)"\s*\)|\bSystem\.getenv\(\s*"([^"]+)"\s*\))re",
                std::regex::optimize);
            static const std::regex props(
                R"re(\bsys\.props\.(?:get|getOrElse)\(\s*"([^"]+)"(?:\s*,\s*([^)]+))?\)|\bsys\.props\(\s*"([^"]+)"\s*\)|\bSystem\.getProperty\(\s*"([^"]+)"(?:\s*,\s*([^)]+))?\))re",
                std::regex::optimize);
            static const std::regex typesafe(
                R"re(\b(\w*(?:config|conf|cfg|settings)\w*)\s*\.\s*get(?:String|Int|Long|Double|Boolean|Duration|Bytes|StringList|IntList|Config|ConfigList|Number|AnyRef)\(\s*"([^"]+)")re",
                std::regex::optimize | std::regex::icase);
            static const std::regex scopt(R"re(\bopt\[[^\]]+\]\(\s*(?:'\w'\s*,\s*)?"([\w-]+)")re", std::regex::optimize);
            static const std::regex cli(
                R"re(\bcase\s+"(--?[A-Za-z][\w-]*)"\s*(?:::|=>|\|)|==\s*"(--?[A-Za-z][\w-]*)"|"(--?[A-Za-z][\w-]*)"\s*==)re",
                std::regex::optimize);

            std::smatch m;
            if (std::regex_search(code, m, env))
            {
                std::string key = m[1].matched ? m[1].str() : m[3].matched ? m[3].str() : m[4].str();
                std::string value = m[2].matched ? Trim(m[2].str()) : std::string();
                AddSetting(info, "environment", std::string(), key, AssignedVariable(m.prefix().str()),
                    IsLiteralValue(value) ? LiteralText(value) : std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, props))
            {
                std::string key = m[1].matched ? m[1].str() : m[3].matched ? m[3].str() : m[4].str();
                std::string value = m[2].matched ? Trim(m[2].str()) : m[5].matched ? Trim(m[5].str()) : std::string();
                AddSetting(info, "system property", std::string(), key, AssignedVariable(m.prefix().str()),
                    IsLiteralValue(value) ? LiteralText(value) : std::string(), function, lineNo);
            }

            if (std::regex_search(code, m, typesafe))
            {
                AddSetting(info, "typesafe config", std::string(), m[2].str(), AssignedVariable(m.prefix().str()), std::string(),
                    function, lineNo);
            }

            if (std::regex_search(code, m, scopt))
            {
                AddSetting(info, "command line", std::string(), "--" + m[1].str(), std::string(), std::string(), function, lineNo);
            }
            else if (std::regex_search(code, m, cli))
            {
                std::string option = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();
                AddSetting(info, "command line", std::string(), option, std::string(), std::string(), function, lineNo);
            }
        }

        bool IsScalaKeyword(const std::string& name)
        {
            static const char* const kWords[] = { "if", "while", "for", "match", "case", "new", "throw", "return", "yield",
                "require", "assert", "println", "print", "Some", "Option", "List", "Seq", "Map", "Set", "Vector", "Array",
                "Future", "Try", "Success", "Failure", "Left", "Right", "classOf", "implicitly", "super", "this", "catch" };
            for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i)
            {
                if (name == kWords[i])
                {
                    return true;
                }
            }

            return false;
        }

        // Scala: class/object/trait/enum and their defs ("Outer.method"), by indentation. A body ends at the next
        // line indented no deeper than its "def", which is part of the body when it is the closing "}" or an
        // "end name" marker. Local defs inside a def belong to that def.
        void ParseScala(const std::vector<std::string>& lines, SourceFileInfo& info)
        {
            static const std::regex classRe(
                R"(^\s*(?:(?:private|protected|final|sealed|abstract|implicit|case|override|open|transparent|inline)\s+|(?:private|protected)\[\w+\]\s+)*(class|object|trait|enum)\s+([A-Za-z_]\w*))",
                std::regex::optimize);
            static const std::regex defRe(
                R"(^\s*(?:(?:override|private|protected|final|implicit|inline|transparent|lazy|abstract)\s+|(?:private|protected)\[\w+\]\s+)*def\s+([A-Za-z_]\w*))",
                std::regex::optimize);
            static const std::regex constantRe(
                R"(^\s*(?:(?:private|protected|final|override|inline|lazy)\s+|(?:private|protected)\[\w+\]\s+)*val\s+([A-Z]\w*)\s*(?::\s*[\w\[\]., ]+)?\s*=\s*(.+?)\s*$)",
                std::regex::optimize);
            static const std::regex callRe(R"(([A-Za-z_]\w*)\s*\()", std::regex::optimize);
            static const std::regex messageCall(
                R"((?:\b(?:log|logger|LOG|Log|Logger)\.(?:trace|debug|info|warn|warning|error|fatal)\(|\bthrow\s+(?:new\s+)?\w*(?:Exception|Error)\(|\bsys\.error\(|\brequire\(|\bassert\(|\bprintln\(|System\.(?:out|err)\.print|Console\.err\.print|\bnew\s+\w*(?:Exception|Error)\(|\bFailure\())",
                std::regex::optimize);

            struct Scope
            {
                int indent;
                std::string name;
            };

            std::vector<Scope> classes;
            std::string currentFunction;
            int functionIndent = -1;
            size_t functionIndex = 0;
            int lastCodeLine = 0;
            bool inBlock = false;

            for (size_t i = 0; i < lines.size(); ++i)
            {
                const int lineNo = static_cast<int>(i) + 1;
                const std::string& raw = lines[i];
                std::string code = StripCppComments(raw, inBlock);
                std::string trimmed = Trim(code);
                if (trimmed.empty() || raw.size() > kMaxRegexLine || StartsWithWord(trimmed, "import") ||
                    StartsWithWord(trimmed, "package"))
                {
                    continue;
                }

                const int indent = Indentation(raw);
                const bool closing = trimmed[0] == '}' || StartsWithWord(trimmed, "end");
                if (!currentFunction.empty() && indent <= functionIndent)
                {
                    bool ownEnd = closing && indent == functionIndent;
                    info.symbols[functionIndex].endLine = ownEnd ? lineNo : lastCodeLine;
                    currentFunction.clear();
                    functionIndent = -1;
                    if (ownEnd)
                    {
                        lastCodeLine = lineNo;
                        continue;
                    }
                }

                while (!classes.empty() && indent <= classes.back().indent && !(closing && indent == classes.back().indent))
                {
                    classes.pop_back();
                }

                if (closing && !classes.empty() && indent == classes.back().indent && currentFunction.empty())
                {
                    classes.pop_back();   // the class's own "}" or "end Name"
                    lastCodeLine = lineNo;
                    continue;
                }

                try
                {
                    lastCodeLine = lineNo;
                    std::smatch m;
                    bool definedHere = false;
                    if (currentFunction.empty() && std::regex_search(code, m, classRe))
                    {
                        SymbolInfo sym;
                        sym.kind = "class";
                        sym.name = classes.empty() ? m[2].str() : classes.back().name + "." + m[2].str();
                        sym.signature = Shorten(trimmed, 200);
                        sym.line = lineNo;
                        info.symbols.push_back(sym);
                        Scope s = { indent, sym.name };
                        classes.push_back(s);
                        continue;
                    }

                    if (currentFunction.empty() && std::regex_search(code, m, defRe))
                    {
                        SymbolInfo sym;
                        sym.kind = "function";
                        sym.name = classes.empty() ? m[1].str() : classes.back().name + "." + m[1].str();
                        sym.signature = Shorten(trimmed, 240);
                        sym.line = lineNo;
                        info.symbols.push_back(sym);
                        definedHere = true;

                        // A body follows "=" or "{" (Scala 2 procedure syntax); without either it is abstract.
                        std::string after = code.substr(static_cast<size_t>(m.position(0) + m.length(0)));
                        if (after.find('=') != std::string::npos || after.find('{') != std::string::npos)
                        {
                            currentFunction = sym.name;
                            functionIndent = indent;
                            functionIndex = info.symbols.size() - 1;
                        }
                    }

                    if (currentFunction.empty() && std::regex_search(code, m, constantRe) && IsLiteralValue(m[2].str()))
                    {
                        AddSetting(info, "constant", std::string(), m[1].str(), std::string(), m[2].str(), std::string(), lineNo);
                    }

                    FindScalaSettings(code, lineNo, currentFunction, info);

                    if (!currentFunction.empty())
                    {
                        std::string calls = BlankPythonStrings(code);
                        std::sregex_iterator it(calls.begin(), calls.end(), callRe), end;
                        for (bool first = true; it != end; ++it, first = false)
                        {
                            if (first && definedHere)
                            {
                                continue;
                            }

                            std::string name = (*it)[1].str();
                            if (name.size() < 2 || IsScalaKeyword(name))
                            {
                                continue;
                            }

                            AddCall(info.symbols[functionIndex], CallName(calls, static_cast<size_t>(it->position(1)), name), lineNo);
                        }
                    }

                    if (std::regex_search(code, messageCall))
                    {
                        std::vector<GuardInfo> guards;
                        bool guardsDone = false;
                        std::sregex_iterator it(code.begin(), code.end(), CppStringLiteral()), end;
                        for (; it != end; ++it)
                        {
                            std::string literal = (*it)[1].str();
                            if (literal.size() < 8)
                            {
                                continue;
                            }

                            if (!guardsDone && !currentFunction.empty())
                            {
                                guards = ScalaGuards(lines, info.symbols[functionIndex].line, lineNo);
                            }

                            guardsDone = true;
                            MessageInfo msg;
                            msg.text = literal;
                            msg.function = currentFunction;
                            msg.line = lineNo;
                            msg.guards = guards;
                            info.messages.push_back(msg);
                        }
                    }
                }
                catch (const std::regex_error&)
                {
                    // This line exhausted the regex engine (very long or pathological): skip it.
                }

            }

            if (!currentFunction.empty())
            {
                info.symbols[functionIndex].endLine = lastCodeLine;
            }
        }

        // HOCON (application.conf, Typesafe Config): "a { b = 1 }", "a.b = 1", "a.b: 1", # and // comments.
        bool LooksLikeHocon(const std::string& path, const std::string& text)
        {
            std::string name = ToLowerAscii(path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1));
            if (name == "application.conf" || name == "reference.conf")
            {
                return true;
            }

            return text.find("${") != std::string::npos || text.find("include \"") != std::string::npos ||
                   std::regex_search(text, std::regex(R"((^|\n)\s*[A-Za-z_][\w.\-]*\s*[=:]?\s*\{\s*(\r?\n|$))"));
        }

        void ParseHocon(const std::vector<std::string>& lines, ConfigFileInfo& file)
        {
            static const std::regex open(R"re(^("?[A-Za-z_][\w.\-]*"?)\s*[=:]?\s*\{$)re", std::regex::optimize);
            static const std::regex entry(R"re(^("?[A-Za-z_][\w.\-]*"?)\s*[=:]\s*(.+)$)re", std::regex::optimize);
            std::vector<std::string> path;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                std::string line = lines[i];
                if (line.size() > kMaxRegexLine)
                {
                    continue;
                }

                char quote = 0;   // cut "#" and "//" comments outside quotes
                for (size_t c = 0; c < line.size(); ++c)
                {
                    if (quote)
                    {
                        quote = line[c] == quote ? 0 : quote;
                    }
                    else if (line[c] == '"')
                    {
                        quote = '"';
                    }
                    else if (line[c] == '#' || (line[c] == '/' && c + 1 < line.size() && line[c + 1] == '/'))
                    {
                        line.erase(c);
                        break;
                    }
                }

                std::string t = Trim(line);
                while (!t.empty() && t[t.size() - 1] == ',')
                {
                    t = Trim(t.substr(0, t.size() - 1));
                }

                if (t.empty() || StartsWithWord(t, "include"))
                {
                    continue;
                }

                std::smatch m;
                if (t[0] == '}')
                {
                    for (size_t c = 0; c < t.size() && t[c] == '}' && !path.empty(); ++c)
                    {
                        path.pop_back();
                    }
                }
                else if (std::regex_match(t, m, open))
                {
                    path.push_back(Unquote(m[1].str()));
                }
                else if (std::regex_match(t, m, entry))
                {
                    std::string value = Trim(m[2].str());
                    if (value[0] == '[' && value.find(']') == std::string::npos)
                    {
                        continue;   // a list spread over several lines
                    }

                    std::string key;
                    for (size_t p = 0; p < path.size(); ++p)
                    {
                        key += path[p] + ".";
                    }

                    AddEntry(file, key + Unquote(m[1].str()), Unquote(value), static_cast<int>(i) + 1);
                }
            }
        }

    }  // namespace

    ConfigFileInfo SourceCodeParser::ParseConfigText(const std::string& relativePath, const std::string& format,
        const std::string& text)
    {
        ConfigFileInfo info;
        info.path = relativePath;
        info.format = format;
        try
        {
            std::string body = text;
            if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0)
            {
                body.erase(0, 3);   // UTF-8 byte order mark
            }

            if (format == "json")
            {
                ParseJson(body, info);
            }
            else if (format == "yaml")
            {
                ParseYaml(SplitLines(body), info);
            }
            else if (format == "ini" && ToLowerAscii(relativePath).size() > 5 &&
                     ToLowerAscii(relativePath).compare(relativePath.size() - 5, 5, ".conf") == 0 && LooksLikeHocon(relativePath, body))
            {
                info.format = "hocon";
                ParseHocon(SplitLines(body), info);
            }
            else
            {
                ParseIniLike(SplitLines(body), info, format == "toml");
            }
        }
        catch (const std::exception&)
        {
            // regex_error or a JSON type error: keep what was read so far.
        }

        return info;
    }

    SourceFileInfo SourceCodeParser::ParseText(const std::string& relativePath, const std::string& language,
        const std::string& text)
    {
        SourceFileInfo info;
        info.path = relativePath;
        info.language = language;
        std::vector<std::string> lines = SplitLines(text);
        info.lineCount = static_cast<int>(lines.size());
        try
        {
            if (language == "python")
            {
                ParsePython(lines, info);
            }
            else if (language == "java" || language == "csharp")
            {
                ParseJava(lines, info, language == "csharp");
            }
            else if (language == "scala")
            {
                ParseScala(lines, info);
            }
            else
            {
                std::string lower = ToLowerAscii(relativePath);
                bool isHeader = lower.size() > 2 && (lower.rfind(".h") == lower.size() - 2 ||
                                                        lower.rfind(".hpp") == lower.size() - 4);
                ParseCpp(lines, info, isHeader);
            }
        }
        catch (const std::regex_error&)
        {
            // A pathological line exhausted the regex engine; keep what was parsed so far.
        }

        return info;
    }

    // =====================================================================================
    // Log analysis
    // =====================================================================================

    namespace
    {
        const std::regex& SeverityRe()
        {
            static const std::regex re(
                R"(\b(fatal|critical|crit|severe|error|errors|fail|exception|traceback|failed|failure|panic|segmentation fault|access violation|unhandled|assertion failed|crash|crashed|aborted|abort)\b)",
                std::regex::icase | std::regex::optimize);
            return re;
        }

        const std::regex& WarningRe()
        {
            static const std::regex re(R"(\b(warn|warning)\b)", std::regex::icase | std::regex::optimize);
            return re;
        }

        const std::regex& PythonFrameRe()
        {
            static const std::regex re(R"re(File "([^"]+)", line (\d+)(?:, in ([\w<>.]+))?)re", std::regex::optimize);
            return re;
        }

        const std::regex& GenericFrameRe()
        {
            static const std::regex re(
                R"(((?:[A-Za-z]:)?[\w./\\-]*?[\w-]+\.(?:cpp|cc|cxx|cs|c|h|hpp|hh|py|java|scala))(?:[:(]|:line )(\d+))", std::regex::icase | std::regex::optimize);
            return re;
        }

        // A new log record usually starts with a date/time or a bracketed field.
        const std::regex& RecordStartRe()
        {
            static const std::regex re(
                R"(^\s*(\[?\d{4}[-/.]\d{2}[-/.]\d{2}|\[?\d{2}[-/.]\d{2}[-/.]\d{2,4}|\[?\d{1,2}:\d{2}:\d{2}|\[[^\]]{1,40}\]))",
                std::regex::optimize);
            return re;
        }

        // An explicit DEBUG/INFO level on the record: words like "failed=0" there are not errors.
        bool IsLowSeverityRecord(const std::string& line)
        {
            static const std::regex re(R"(^.{0,60}?(?:\b(?:DEBUG|INFO|TRACE|VERBOSE|NOTICE|FINE|FINER|FINEST|CONFIG)\b|^\s*(?:info|dbug|trce):))", std::regex::optimize);
            return std::regex_search(line, re);
        }

        bool LooksLikeContinuation(const std::string& line)
        {
            if (line.empty())
            {
                return false;
            }

            if (line[0] == ' ' || line[0] == '\t')
            {
                return true;
            }

            static const std::regex re(
                R"(^(Traceback|File "|\s*at |\w+(\.\w+)*(Error|Exception|Exit)\b|#\d+\s|Caused by)|\.(py|java|cs|scala|cpp|cc|h|hpp)(?:[:(]|:line )\d+)",
                std::regex::optimize);
            return std::regex_search(line, re);
        }

        // Normalises a log line so repeated errors that differ only in numbers/ids/times group together.
        std::string NormaliseForGrouping(const std::string& line)
        {
            static const std::regex hex(R"(0x[0-9a-fA-F]+)", std::regex::optimize);
            static const std::regex num(R"(\d+)", std::regex::optimize);
            std::string out = std::regex_replace(line, hex, "#");
            out = std::regex_replace(out, num, "#");
            return ToLowerAscii(Trim(out));
        }

        // Splits a source message literal at format placeholders (%s, %d, {}, {name}, %(x)s) into the fixed
        // text a matching log line must contain, lowercased.
        std::vector<std::string> FixedParts(const std::string& literal)
        {
            static const std::regex placeholder(R"(%\([A-Za-z_]+\)[a-z]|%[-+ #0-9.]*[a-zA-Z]|\$\{[^{}]*\}|\$[A-Za-z_]\w*|\{[^{}]*\}|\\[nrt])",
                std::regex::optimize);
            std::vector<std::string> parts;
            std::sregex_token_iterator it(literal.begin(), literal.end(), placeholder, -1), end;
            for (; it != end; ++it)
            {
                std::string part = ToLowerAscii(Trim(it->str()));
                if (part.size() >= 4)
                {
                    parts.push_back(part);
                }
            }

            return parts;
        }

        std::string NormaliseSlashes(std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            return ToLowerAscii(path);
        }

        std::string BaseName(const std::string& path)
        {
            size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        // "NllbTranslator", "read_file", "Load2" look like code; "cancelled" or "translation" are just words
        // that happen to be function names too, and match ordinary log text far too often.
        bool LooksLikeCodeIdentifier(const std::string& name)
        {
            if (name.size() > 4 && name.compare(0, 2, "__") == 0)
            {
                return false;  // __init__ etc.: in every class
            }

            if (name.find('_') != std::string::npos)
            {
                return true;
            }

            for (size_t i = 1; i < name.size(); ++i)
            {
                unsigned char c = static_cast<unsigned char>(name[i]);
                if (std::isupper(c) || std::isdigit(c))
                {
                    return true;
                }
            }

            return std::isupper(static_cast<unsigned char>(name[0])) != 0 && name.size() >= 6;  // e.g. "Tokenizer"
        }

        std::string LastComponent(const std::string& name)
        {
            size_t cut = name.find_last_of(".:");
            return cut == std::string::npos ? name : name.substr(cut + 1);
        }

        const SourceFileInfo* FindFileForPath(const KnowledgeBase& kb, const std::string& framePath)
        {
            std::string wanted = NormaliseSlashes(framePath);
            const SourceFileInfo* best = nullptr;
            size_t bestLen = 0;
            for (size_t i = 0; i < kb.files.size(); ++i)
            {
                std::string candidate = ToLowerAscii(kb.files[i].path);
                if (wanted.size() >= candidate.size() &&
                    wanted.compare(wanted.size() - candidate.size(), candidate.size(), candidate) == 0 &&
                    (wanted.size() == candidate.size() || wanted[wanted.size() - candidate.size() - 1] == '/'))
                {
                    if (candidate.size() > bestLen)
                    {
                        best = &kb.files[i];
                        bestLen = candidate.size();
                    }
                }
            }

            if (best)
            {
                return best;
            }

            std::string base = BaseName(wanted);   // fall back to a unique file-name match
            const SourceFileInfo* only = nullptr;
            for (size_t i = 0; i < kb.files.size(); ++i)
            {
                if (ToLowerAscii(BaseName(kb.files[i].path)) == base)
                {
                    if (only)
                    {
                        return nullptr;
                    }

                    only = &kb.files[i];
                }
            }

            return only;
        }

        std::string EnclosingFunction(const SourceFileInfo& file, int line)
        {
            std::string name;
            int bestLine = 0;
            for (size_t i = 0; i < file.symbols.size(); ++i)
            {
                const SymbolInfo& s = file.symbols[i];
                if (s.kind == "function" && s.line <= line && s.line > bestLine)
                {
                    bestLine = s.line;
                    name = s.name;
                }
            }

            return name;
        }

        // The lines of a knowledge base file, read once per analysis (empty if the source is not available).
        const std::vector<std::string>& SourceLines(const std::string& root, const std::string& relPath,
            std::map<std::string, std::vector<std::string>>& cache)
        {
            std::map<std::string, std::vector<std::string>>::iterator it = cache.find(relPath);
            if (it == cache.end())
            {
                std::string rel = relPath;
                std::replace(rel.begin(), rel.end(), '/', '\\');
                std::string text, error;
                std::vector<std::string> lines;
                if (!root.empty() && ReadFileBytes(JoinPath(Utf8ToWide(root), Utf8ToWide(rel)), text, error, 8ull * 1024 * 1024))
                {
                    lines = SplitLines(text);
                }

                it = cache.insert(std::make_pair(relPath, lines)).first;
            }

            return it->second;
        }

        std::string Snippet(const std::string& root, const std::string& relPath, int line,
            std::map<std::string, std::vector<std::string>>& cache)
        {
            if (root.empty() || line <= 0)
            {
                return std::string();
            }

            const std::vector<std::string>& lines = SourceLines(root, relPath, cache);
            if (lines.empty() || line > static_cast<int>(lines.size()))
            {
                return std::string();
            }

            int from = std::max(1, line - 5), to = std::min(static_cast<int>(lines.size()), line + 8);
            std::ostringstream os;
            for (int n = from; n <= to; ++n)
            {
                char prefix[16];
                sprintf_s(prefix, "%s%5d | ", n == line ? ">" : " ", n);
                os << prefix << Shorten(lines[static_cast<size_t>(n - 1)], 200) << "\n";
            }

            return os.str();
        }

        // ----- program flow -----

        std::vector<GuardInfo> ComputeGuards(const std::vector<std::string>& lines, const std::string& language,
            int fromLine, int targetLine)
        {
            return language == "python" ? PythonGuards(lines, fromLine, targetLine)
                   : language == "scala" ? ScalaGuards(lines, fromLine, targetLine)
                                         : CppGuards(lines, fromLine, targetLine);
        }

        struct FunctionRef
        {
            size_t file;
            size_t symbol;

            bool operator<(const FunctionRef& o) const
            {
                return file != o.file ? file < o.file : symbol < o.symbol;
            }

            bool operator==(const FunctionRef& o) const
            {
                return file == o.file && symbol == o.symbol;
            }
        };

        // "a.b.Load" / "a::Load" -> "a.b" / "a"; "" if unqualified.
        std::string Qualifier(const std::string& name)
        {
            size_t cut = name.find_last_of(".:");
            if (cut == std::string::npos)
            {
                return std::string();
            }

            size_t start = cut;
            while (start > 0 && name[start - 1] == ':')
            {
                --start;
            }

            return name.substr(0, start);
        }

        // Who calls whom, resolved by name. Calls are matched on the function's last name component; when
        // several functions share it, only calls from the same file or with a matching class qualifier count.
        class FlowIndex
        {
        public:
            explicit FlowIndex(const KnowledgeBase& kb)
                : kb_(kb)
            {
                for (size_t f = 0; f < kb.files.size(); ++f)
                {
                    for (size_t s = 0; s < kb.files[f].symbols.size(); ++s)
                    {
                        const SymbolInfo& sym = kb.files[f].symbols[s];
                        if (sym.kind != "function")
                        {
                            continue;
                        }

                        FunctionRef ref = { f, s };
                        bySimple_[LastComponent(sym.name)].push_back(ref);
                        const std::string suffix = ".__init__";   // Python: calling the class runs __init__
                        if (sym.name.size() > suffix.size() &&
                            sym.name.compare(sym.name.size() - suffix.size(), suffix.size(), suffix) == 0)
                        {
                            bySimple_[LastComponent(sym.name.substr(0, sym.name.size() - suffix.size()))].push_back(ref);
                        }
                    }
                }

                for (size_t f = 0; f < kb.files.size(); ++f)
                {
                    for (size_t s = 0; s < kb.files[f].symbols.size(); ++s)
                    {
                        const SymbolInfo& sym = kb.files[f].symbols[s];
                        FunctionRef self = { f, s };
                        for (size_t c = 0; c < sym.calls.size(); ++c)
                        {
                            std::vector<FunctionRef> targets = Resolve(sym.calls[c], f);
                            for (size_t t = 0; t < targets.size(); ++t)
                            {
                                if (targets[t] == self)
                                {
                                    continue;
                                }

                                callers_[targets[t]].push_back(std::make_pair(self, sym.calls[c].line));
                                std::vector<FunctionRef>& out = callees_[self];
                                if (std::find(out.begin(), out.end(), targets[t]) == out.end())
                                {
                                    out.push_back(targets[t]);
                                }
                            }
                        }
                    }
                }
            }

            const SymbolInfo& Function(const FunctionRef& ref) const
            {
                return kb_.files[ref.file].symbols[ref.symbol];
            }

            const SourceFileInfo& File(const FunctionRef& ref) const
            {
                return kb_.files[ref.file];
            }

            // The function `name` in `file`, or else the one whose body contains `line`.
            bool Find(const std::string& file, const std::string& name, int line, FunctionRef& out) const
            {
                for (size_t f = 0; f < kb_.files.size(); ++f)
                {
                    if (kb_.files[f].path != file)
                    {
                        continue;
                    }

                    const std::vector<SymbolInfo>& syms = kb_.files[f].symbols;
                    int bestLine = -1;
                    for (size_t s = 0; s < syms.size(); ++s)
                    {
                        const SymbolInfo& sym = syms[s];
                        if (sym.kind != "function" || sym.line > line || sym.line <= bestLine)
                        {
                            continue;
                        }

                        bool named = !name.empty() && sym.name == name;
                        bool contains = sym.endLine == 0 || line <= sym.endLine;
                        if (named || (name.empty() && contains) || (contains && sym.endLine > 0))
                        {
                            out.file = f;
                            out.symbol = s;
                            bestLine = sym.line;
                        }
                    }

                    return bestLine >= 0;
                }

                return false;
            }

            std::vector<std::pair<FunctionRef, int>> Callers(const FunctionRef& ref) const
            {
                std::map<FunctionRef, std::vector<std::pair<FunctionRef, int>>>::const_iterator it = callers_.find(ref);
                return it == callers_.end() ? std::vector<std::pair<FunctionRef, int>>() : it->second;
            }

            std::vector<FunctionRef> Callees(const FunctionRef& ref) const
            {
                std::map<FunctionRef, std::vector<FunctionRef>>::const_iterator it = callees_.find(ref);
                return it == callees_.end() ? std::vector<FunctionRef>() : it->second;
            }

            // "main -> RunAnalyze -> KnowledgeBase::Load": one path from a function nobody calls (outermost first).
            std::string CallChain(const FunctionRef& ref) const
            {
                std::vector<FunctionRef> chain(1, ref);
                std::set<FunctionRef> seen;
                seen.insert(ref);
                for (int depth = 0; depth < 5; ++depth)
                {
                    std::vector<std::pair<FunctionRef, int>> up = Callers(chain.back());
                    bool moved = false;
                    for (size_t i = 0; i < up.size() && !moved; ++i)
                    {
                        if (seen.insert(up[i].first).second)
                        {
                            chain.push_back(up[i].first);
                            moved = true;
                        }
                    }

                    if (!moved)
                    {
                        break;
                    }
                }

                if (chain.size() < 2)
                {
                    return std::string();
                }

                std::string text = chain.size() == 6 ? "... -> " : "";
                for (size_t i = chain.size(); i-- > 0;)
                {
                    text += Function(chain[i]).name + (i ? " -> " : "");
                }

                return text;
            }

        private:
            std::vector<FunctionRef> Resolve(const CallInfo& call, size_t callerFile) const
            {
                std::map<std::string, std::vector<FunctionRef>>::const_iterator it = bySimple_.find(LastComponent(call.name));
                if (it == bySimple_.end())
                {
                    return std::vector<FunctionRef>();
                }

                if (it->second.size() == 1)
                {
                    return it->second;
                }

                std::string qualifier = ToLowerAscii(LastComponent(Qualifier(call.name)));
                bool selfCall = qualifier.empty() || qualifier == "self" || qualifier == "this" || qualifier == "cls";

                // A variable is often named after the end of its class: "engine" or "m_engine" for EmbeddedLlamaEngine.
                std::string variable = qualifier.compare(0, 2, "m_") == 0 ? qualifier.substr(2) : qualifier;
                while (!variable.empty() && variable[variable.size() - 1] == '_')
                {
                    variable.erase(variable.size() - 1);
                }

                std::vector<FunctionRef> picked;
                for (size_t i = 0; i < it->second.size(); ++i)
                {
                    const FunctionRef& c = it->second[i];
                    std::string cls = ToLowerAscii(LastComponent(Qualifier(Function(c).name)));
                    bool classNamed = !cls.empty() && !qualifier.empty() &&
                                      (cls == qualifier || (variable.size() >= 4 && cls.size() >= variable.size() &&
                                                               cls.compare(cls.size() - variable.size(), variable.size(), variable) == 0));
                    if ((selfCall && c.file == callerFile) || classNamed)
                    {
                        picked.push_back(c);
                    }
                }

                return picked.size() <= 3 ? picked : std::vector<FunctionRef>();
            }

            const KnowledgeBase& kb_;
            std::map<std::string, std::vector<FunctionRef>> bySimple_;
            std::map<FunctionRef, std::vector<std::pair<FunctionRef, int>>> callers_;
            std::map<FunctionRef, std::vector<FunctionRef>> callees_;
        };

        // ----- settings -----

        // "ModelPath" -> {model, path}; "--http-model" -> {http, model}; "LOCALAPPDATA" -> {localappdata}.
        std::vector<std::string> KeyTokens(const std::string& key)
        {
            std::vector<std::string> tokens;
            std::string current;
            for (size_t i = 0; i < key.size(); ++i)
            {
                char c = key[i];
                bool alnum = std::isalnum(static_cast<unsigned char>(c)) != 0;
                bool boundary = alnum && std::isupper(static_cast<unsigned char>(c)) && i > 0 &&
                                std::islower(static_cast<unsigned char>(key[i - 1]));
                if (!alnum || boundary)
                {
                    if (!current.empty())
                    {
                        tokens.push_back(ToLowerAscii(current));
                    }

                    current.clear();
                }

                if (alnum)
                {
                    current += c;
                }
            }

            if (!current.empty())
            {
                tokens.push_back(ToLowerAscii(current));
            }

            return tokens;
        }

        bool IsGenericToken(const std::string& token)
        {
            static const char* const kWords[] = { "path", "dir", "file", "name", "value", "enable", "enabled", "use",
                "default", "mode", "type", "size", "count", "max", "min", "data", "config", "settings", "setting",
                "option", "options", "the", "and", "for", "with", "from", "app", "home", "user", "local", "temp" };
            for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i)
            {
                if (token == kWords[i])
                {
                    return true;
                }
            }

            return token.size() < 4 || std::isdigit(static_cast<unsigned char>(token[0])) != 0;
        }

        std::string AlnumLower(const std::string& text)
        {
            std::string out;
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (std::isalnum(static_cast<unsigned char>(text[i])))
                {
                    out += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
                }
            }

            return out;
        }

        // Lowercase words of the log text, for matching setting names: "translation", "model", ...
        std::set<std::string> TextWords(const std::string& text)
        {
            std::set<std::string> words;
            std::vector<std::string> tokens = KeyTokens(text);
            for (size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].size() >= 4)
                {
                    words.insert(tokens[i]);
                }
            }

            return words;
        }

        // The distinctive tokens of the key that the log also uses ("translate_en" ~ "translation"): returns the
        // share of them found (0 if none) and the first log word that matched.
        double SharedTokens(const std::vector<std::string>& keyTokens, const std::set<std::string>& words, std::string& firstWord)
        {
            size_t distinctive = 0, found = 0;
            firstWord.clear();
            for (size_t i = 0; i < keyTokens.size(); ++i)
            {
                const std::string& t = keyTokens[i];
                if (IsGenericToken(t))
                {
                    continue;
                }

                ++distinctive;
                for (std::set<std::string>::const_iterator w = words.begin(); w != words.end(); ++w)
                {
                    size_t common = 0;
                    while (common < t.size() && common < w->size() && t[common] == (*w)[common])
                    {
                        ++common;
                    }

                    size_t shorter = std::min(t.size(), w->size());
                    if ((common == shorter && shorter >= 4) || common >= 6)
                    {
                        ++found;
                        if (firstWord.empty())
                        {
                            firstWord = *w;
                        }

                        break;
                    }
                }
            }

            return distinctive == 0 ? 0.0 : static_cast<double>(found) / static_cast<double>(distinctive);
        }

        // The words (identifiers) used in lines [from, to] of a source file.
        std::set<std::string> IdentifiersIn(const std::vector<std::string>& lines, int from, int to)
        {
            static const std::regex ident(R"([A-Za-z_]\w*)", std::regex::optimize);
            std::set<std::string> out;
            for (int n = std::max(1, from); n <= to && n <= static_cast<int>(lines.size()); ++n)
            {
                const std::string& line = lines[static_cast<size_t>(n - 1)];
                if (line.size() > kMaxRegexLine)
                {
                    continue;
                }

                for (std::sregex_iterator it(line.begin(), line.end(), ident), end; it != end; ++it)
                {
                    out.insert(it->str());
                }
            }

            return out;
        }

        bool ContainsWord(const std::string& text, const std::string& word)
        {
            for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
            {
                bool left = at == 0 || !IsWordChar(text[at - 1]);
                bool right = at + word.size() >= text.size() || !IsWordChar(text[at + word.size()]);
                if (left && right)
                {
                    return true;
                }
            }

            return false;
        }

        // A registry/ini section given as a constant (kSection) is looked up among the file's constants.
        std::string SectionName(const SourceFileInfo& file, const std::string& section)
        {
            if (section.find_first_of("\"'") != std::string::npos)
            {
                return LiteralText(section);
            }

            for (size_t i = 0; i < file.settings.size(); ++i)
            {
                if (file.settings[i].source == "constant" && file.settings[i].key == section)
                {
                    return LiteralText(file.settings[i].value);
                }
            }

            return section;
        }

        std::string SettingLabel(const SettingReadInfo& s, const std::string& section)
        {
            if (s.source == "registry" || s.source == "ini" || s.source == "settings")
            {
                return (section.empty() ? "" : section + "\\") + s.key;
            }

            return s.key;
        }

        std::string HowToChange(const KnowledgeBase& kb, const SourceFileInfo& file, const SettingReadInfo& s,
            const std::string& section)
        {
            std::string how;
            if (s.source == "registry")
            {
                how = kb.registryKey.empty()
                          ? "In the program's profile settings (registry or .ini), section [" + section + "], entry \"" + s.key + "\""
                          : "In the registry: HKEY_CURRENT_USER\\Software\\" + kb.registryKey + "\\<program name>\\" + section +
                                ", value \"" + s.key + "\"";
            }
            else if (s.source == "ini")
            {
                how = "In the program's .ini file, section [" + section + "], key \"" + s.key + "\"";
            }
            else if (s.source == "registry value")
            {
                how = "Registry value \"" + s.key + "\"" + (s.section.empty() ? "" : " under the key " + s.section);
            }
            else if (s.source == "environment")
            {
                how = "Set the environment variable " + s.key + " before starting the program";
            }
            else if (s.source == "command line")
            {
                how = "Pass " + s.key + " <value> on the command line";
            }
            else if (s.source == "system property")
            {
                how = "Start Java with -D" + s.key + "=<value>";
            }
            else if (s.source == "app setting")
            {
                how = s.section == "ConnectionStrings"
                          ? "Set the connection string \"" + s.key + "\" in the connectionStrings section of App.config or Web.config"
                          : "Set <add key=\"" + s.key + "\" value=\"...\" /> in the appSettings section of App.config or Web.config";
            }
            else if (s.source == "configuration")
            {
                std::string env = s.key;
                for (size_t p = env.find(':'); p != std::string::npos; p = env.find(':', p + 2))
                {
                    env.replace(p, 1, "__");
                }

                how = "Set \"" + s.key + "\" in appsettings.json (':' separates sections), or the environment variable " + env;
            }
            else if (s.source == "typesafe config")
            {
                how = "Set " + s.key + " in application.conf, or start the JVM with -D" + s.key + "=<value>";
            }
            else if (s.source == "property")
            {
                how = "Set " + s.key + " in the program's properties (for example application.properties or application.yml)";
            }
            else if (s.source == "settings")
            {
                how = "Change the saved setting \"" + s.key + "\"" + (section.empty() ? "" : " in section [" + section + "]");
            }
            else if (s.source == "constant")
            {
                return "Change " + s.key + " (now " + s.value + ") at " + file.path + ":" + std::to_string(s.line) +
                       " and rebuild";
            }

            if (!s.value.empty())
            {
                how += " (default in the code: " + s.value + ")";
            }

            return how;
        }

        // A setting's value appears in the log ("fifty", "C:/Inventory/stock" in "C:\Inventory\stock\north.csv").
        // `lowerLog` is lowercase with '/' separators. Short values must appear as a whole word.
        bool ValueInLog(const std::string& value, const std::string& lowerLog)
        {
            std::string v = ToLowerAscii(Trim(value));
            std::replace(v.begin(), v.end(), '\\', '/');
            while (v.size() > 1 && v[v.size() - 1] == '/')
            {
                v.erase(v.size() - 1);
            }

            if (v.size() < 4 || v == "true" || v == "false" || v == "none" || v == "null")
            {
                return false;
            }

            bool numeric = v.find_first_not_of("0123456789.-") == std::string::npos;
            return v.size() >= 6 && !numeric ? lowerLog.find(v) != std::string::npos : !numeric && ContainsWord(lowerLog, v);
        }

        struct SettingCandidate
        {
            std::string rawKey;   // the setting's own name, without section
            std::string variableKey;   // "file|function|variable" when the value is stored in a variable
            SettingEvidence evidence;
            double locality = 0.0;
            double logMatch = 0.0;
            std::string localReason;
            std::string logReason;
        };

        // Settings that may change the outcome of each problem: read in the failing function or near it in the
        // call graph, used in the conditions that lead to the error, or named (or shown) in the log text.
        void LinkSettings(const KnowledgeBase& kb, const FlowIndex& flow, LogAnalysis& result,
            std::map<std::string, std::vector<std::string>>& cache, const std::set<std::string>& commonWords)
        {
            struct Related
            {
                FunctionRef ref;
                double base;
                std::string reason;
            };

            // Settings that can be referred to by name in other code: constants, and reads stored in a variable.
            static const char* const kVague[] = { "path", "value", "result", "text", "name", "line", "data", "file",
                "error", "args", "self", "options", "config", "settings", "default", "item", "key", "out", "res" };
            struct Named
            {
                size_t file;
                size_t setting;
                std::string word;
            };

            std::vector<Named> named;
            for (size_t f = 0; f < kb.files.size(); ++f)
            {
                for (size_t r = 0; r < kb.files[f].settings.size(); ++r)
                {
                    const SettingReadInfo& s = kb.files[f].settings[r];
                    std::string word = s.source == "constant" ? s.key : LastComponent(s.variable);
                    bool vague = word.size() < 4;
                    for (size_t v = 0; v < sizeof(kVague) / sizeof(kVague[0]) && !vague; ++v)
                    {
                        vague = ToLowerAscii(word) == kVague[v];
                    }

                    if (!vague)
                    {
                        Named n = { f, r, word };
                        named.push_back(n);
                    }
                }
            }

            for (size_t e = 0; e < result.events.size(); ++e)
            {
                // The messages only: stack frames name code ("ConfigLoader" is not the --config option), and
                // words on nearly every log line (the logger or program name) say nothing about this problem.
                const LogEvent& ev = result.events[e];
                static const std::regex frame(R"re(^\s*(?:at\s|File "|#\d+\s))re", std::regex::optimize);
                std::string logText = ev.text;
                for (size_t c = 0; c < ev.context.size(); ++c)
                {
                    if (!std::regex_search(ev.context[c], frame))
                    {
                        logText += "\n" + ev.context[c];
                    }
                }

                std::string lowerLog = ToLowerAscii(logText);
                std::replace(lowerLog.begin(), lowerLog.end(), '\\', '/');
                const std::string alnumLog = AlnumLower(logText);
                std::set<std::string> words = TextWords(logText);
                for (std::set<std::string>::const_iterator w = commonWords.begin(); w != commonWords.end(); ++w)
                {
                    words.erase(*w);
                }

                // Functions around the failure, with how strongly a setting read there relates to it.
                std::vector<Related> related;
                std::string guardText;
                auto addRelated = [&](const FunctionRef& ref, double base, const std::string& reason)
                {
                    for (size_t i = 0; i < related.size(); ++i)
                    {
                        if (related[i].ref == ref)
                        {
                            if (base > related[i].base)
                            {
                                related[i].base = base;
                                related[i].reason = reason;
                            }

                            return;
                        }
                    }

                    Related r = { ref, base, reason };
                    related.push_back(r);
                };

                // The innermost stack frame (or the line that logs the message) is where the error happens; outer
                // frames are on the way there.
                double innermost = -1.0;
                for (size_t i = 0; i < result.evidence.size(); ++i)
                {
                    const Evidence& evd = result.evidence[i];
                    if (evd.eventIndex == static_cast<int>(e) && evd.kind == "location")
                    {
                        innermost = std::max(innermost, evd.score);
                    }
                }

                struct Spot
                {
                    FunctionRef ref;
                    int line;
                    bool primary;
                };

                std::vector<Spot> spots;
                for (size_t i = 0; i < result.evidence.size(); ++i)
                {
                    const Evidence& evd = result.evidence[i];
                    FunctionRef ref;
                    if (evd.eventIndex != static_cast<int>(e) || evd.kind == "symbol" || !flow.Find(evd.file, evd.name, evd.line, ref))
                    {
                        continue;
                    }

                    for (size_t g = 0; g < evd.guards.size(); ++g)
                    {
                        guardText += evd.guards[g].text + "\n";
                    }

                    const bool primary = evd.kind == "message" || evd.score >= innermost;
                    const std::string& name = flow.Function(ref).name;
                    addRelated(ref, primary ? 0.45 : 0.25,
                        "read in " + name + (primary ? ", where the error happens" : ", on the way to the error"));
                    Spot spot = { ref, evd.line, primary };
                    spots.push_back(spot);
                    if (!primary)
                    {
                        continue;   // the call graph is explored only around where the error happens
                    }

                    std::vector<FunctionRef> down = flow.Callees(ref);
                    for (size_t d = 0; d < down.size(); ++d)
                    {
                        addRelated(down[d], 0.2, "read in " + flow.Function(down[d]).name + ", which " + name + " calls");
                    }

                    std::vector<std::pair<FunctionRef, int>> up = flow.Callers(ref);
                    for (size_t u = 0; u < up.size() && u < 6; ++u)
                    {
                        const std::string& callerName = flow.Function(up[u].first).name;
                        addRelated(up[u].first, 0.25, "read in " + callerName + ", which calls " + name);
                        std::vector<FunctionRef> siblings = flow.Callees(up[u].first);
                        for (size_t s = 0; s < siblings.size() && s < 20; ++s)
                        {
                            addRelated(siblings[s], 0.2, "read in " + flow.Function(siblings[s]).name + ", which " +
                                                             callerName + " also calls before or after " + name);
                        }

                        std::vector<std::pair<FunctionRef, int>> up2 = flow.Callers(up[u].first);
                        for (size_t v = 0; v < up2.size() && v < 4; ++v)
                        {
                            addRelated(up2[v].first, 0.2, "read in " + flow.Function(up2[v].first).name + ", which leads to " + name);
                        }
                    }
                }

                std::map<std::string, SettingCandidate> candidates;
                auto candidate = [&](size_t f, const SettingReadInfo& s) -> SettingCandidate&
                {
                    const SourceFileInfo& file = kb.files[f];
                    std::string section = SectionName(file, s.section);
                    std::string id = s.source + "|" + section + "|" + s.key;
                    std::map<std::string, SettingCandidate>::iterator it = candidates.find(id);
                    if (it == candidates.end())
                    {
                        SettingCandidate c;
                        c.rawKey = s.key;
                        if (!s.variable.empty())
                        {
                            c.variableKey = file.path + "|" + s.function + "|" + s.variable;
                        }

                        c.evidence.eventIndex = static_cast<int>(e);
                        c.evidence.source = s.source;
                        c.evidence.key = SettingLabel(s, section);
                        c.evidence.file = file.path;
                        c.evidence.line = s.line;
                        c.evidence.function = s.function;
                        c.evidence.value = s.value;
                        c.evidence.howToChange = HowToChange(kb, file, s, section);
                        it = candidates.insert(std::make_pair(id, c)).first;
                    }

                    return it->second;
                };

                auto local = [&](SettingCandidate& c, double score, const std::string& reason)
                {
                    if (score > c.locality)
                    {
                        c.locality = score;
                        c.localReason = reason;
                    }
                };

                // 1. Read in a related function.
                for (size_t i = 0; i < related.size(); ++i)
                {
                    const SourceFileInfo& file = flow.File(related[i].ref);
                    const SymbolInfo& fn = flow.Function(related[i].ref);
                    for (size_t r = 0; r < file.settings.size(); ++r)
                    {
                        if (file.settings[r].function == fn.name && file.settings[r].source != "constant")
                        {
                            local(candidate(related[i].ref.file, file.settings[r]), related[i].base, related[i].reason);
                        }
                    }
                }

                // A variable name means the same thing within its file, or along the call path (settings.modelPath
                // read in the caller, tested in the function that fails).
                std::set<std::pair<size_t, std::string>> relatedFunctions;
                for (size_t i = 0; i < related.size(); ++i)
                {
                    relatedFunctions.insert(std::make_pair(related[i].ref.file, flow.Function(related[i].ref).name));
                }

                auto visible = [&](const Named& n, size_t file)
                {
                    const SettingReadInfo& s = kb.files[n.file].settings[n.setting];
                    return s.source == "constant" || n.file == file || relatedFunctions.count(std::make_pair(n.file, s.function)) > 0;
                };

                // 2. A setting's variable used on the failing line itself, or on a call on the way to it
                //    ("parsePageSize(pageSizeText)" where pageSizeText holds -Dinventory.pageSize or page.size).
                for (size_t p = 0; p < spots.size(); ++p)
                {
                    const SourceFileInfo& file = flow.File(spots[p].ref);
                    const std::vector<std::string>& lines = SourceLines(kb.root, file.path, cache);
                    if (spots[p].line <= 0 || spots[p].line > static_cast<int>(lines.size()))
                    {
                        continue;
                    }

                    const std::string& text = lines[static_cast<size_t>(spots[p].line - 1)];
                    std::string where = file.path + ":" + std::to_string(spots[p].line) +
                                        (spots[p].primary ? ", where the error happens" : ", on the way to the error");
                    for (size_t n = 0; n < named.size(); ++n)
                    {
                        const SettingReadInfo& s = kb.files[named[n].file].settings[named[n].setting];
                        if (visible(named[n], spots[p].ref.file) && ContainsWord(text, named[n].word))
                        {
                            local(candidate(named[n].file, s), s.source == "constant" ? 0.4 : 0.5,
                                "its value (" + named[n].word + ") is used at " + where);
                        }
                    }
                }

                // 3. A constant or settings variable used in the conditions, or in the failing function's body.
                for (size_t i = 0; i < related.size(); ++i)
                {
                    if (related[i].base < 0.45)
                    {
                        continue;   // only the functions where the error happens
                    }

                    const SourceFileInfo& file = flow.File(related[i].ref);
                    const SymbolInfo& fn = flow.Function(related[i].ref);
                    const std::vector<std::string>& lines = SourceLines(kb.root, file.path, cache);
                    std::set<std::string> body = IdentifiersIn(lines, fn.line, fn.endLine > 0 ? fn.endLine : fn.line + 60);
                    for (size_t n = 0; n < named.size(); ++n)
                    {
                        const SettingReadInfo& s = kb.files[named[n].file].settings[named[n].setting];
                        if (!visible(named[n], related[i].ref.file))
                        {
                            continue;
                        }

                        if (ContainsWord(guardText, named[n].word))
                        {
                            std::string guard;
                            std::vector<std::string> guardLines = SplitLines(guardText);
                            for (size_t g = 0; g < guardLines.size() && guard.empty(); ++g)
                            {
                                guard = ContainsWord(guardLines[g], named[n].word) ? guardLines[g] : std::string();
                            }

                            local(candidate(named[n].file, s), 0.6, "decides the condition \"" + Shorten(guard, 100) + "\"");
                        }
                        else if (body.count(named[n].word))
                        {
                            // A constant used somewhere in the function is too loose a link to show on its own.
                            local(candidate(named[n].file, s), s.source == "constant" ? 0.25 : 0.4, "used in " + fn.name);
                        }
                    }
                }

                // 4. Named or shown in the log.
                for (size_t f = 0; f < kb.files.size(); ++f)
                {
                    for (size_t r = 0; r < kb.files[f].settings.size(); ++r)
                    {
                        const SettingReadInfo& s = kb.files[f].settings[r];
                        if (s.source == "constant")
                        {
                            continue;   // there are too many to match by name alone
                        }

                        std::string key = AlnumLower(s.key), reason;
                        double score = 0.0;
                        if (key.size() >= 6 && alnumLog.find(key) != std::string::npos)
                        {
                            score = 0.5;
                            reason = "the log names it";
                        }
                        else
                        {
                            // "ModelPath" for "Model file not found" (all of its distinctive words) beats
                            // "HttpModel" (half of them), which stays below the threshold on its own.
                            std::string shared;
                            double share = SharedTokens(KeyTokens(s.key), words, shared);
                            if (share > 0.0)
                            {
                                score = 0.3 * share;
                                reason = "the log mentions \"" + shared + "\"";
                            }
                        }

                        if (score > 0.0)
                        {
                            SettingCandidate& c = candidate(f, s);
                            if (score > c.logMatch)
                            {
                                c.logMatch = score;
                                c.logReason = reason;
                            }
                        }
                    }
                }

                // 5. Config file entries: matched to a setting above by name, or named or shown in the log.
                std::vector<SettingCandidate> list;
                for (std::map<std::string, SettingCandidate>::iterator it = candidates.begin(); it != candidates.end(); ++it)
                {
                    list.push_back(it->second);
                }

                for (size_t cf = 0; cf < kb.configFiles.size(); ++cf)
                {
                    const ConfigFileInfo& file = kb.configFiles[cf];
                    for (size_t i = 0; i < file.entries.size(); ++i)
                    {
                        const ConfigEntry& entry = file.entries[i];
                        std::string full = AlnumLower(entry.key), last = AlnumLower(LastComponent(entry.key));
                        size_t dot = entry.key.find('.');
                        std::string afterSection = dot == std::string::npos ? full : AlnumLower(entry.key.substr(dot + 1));
                        std::string place = file.path + ":" + std::to_string(entry.line);
                        bool shown = ValueInLog(entry.value, lowerLog);
                        bool attached = false;
                        for (size_t c = 0; c < list.size(); ++c)
                        {
                            // Environment variables are set in .env files only; options and constants not in files.
                            const std::string& source = list[c].evidence.source;
                            bool envFile = ToLowerAscii(file.path).size() >= 4 &&
                                           ToLowerAscii(file.path).compare(file.path.size() - 4, 4, ".env") == 0;
                            bool settable = source == "environment" ? envFile : source != "constant" && source != "command line";
                            const std::string key = AlnumLower(list[c].rawKey);
                            bool same = key.size() >= 4 && (key == full || key == afterSection || (key == last && last.size() >= 6));
                            if (!settable || !same || list[c].evidence.howToChange.find(place) != std::string::npos)
                            {
                                continue;
                            }

                            list[c].evidence.howToChange += "; it is set in " + place + " (now \"" + entry.value + "\")";
                            if (shown && list[c].logMatch < 0.6)
                            {
                                list[c].logMatch = 0.6;
                                list[c].logReason = "the log shows its value \"" + entry.value + "\" from " + place;
                            }

                            attached = true;
                        }

                        if (attached)
                        {
                            continue;
                        }

                        double score = 0.0;
                        std::string reason;
                        if (shown)
                        {
                            score = 0.6;
                            reason = "the log shows its value";
                        }
                        else if (last.size() >= 6 && alnumLog.find(last) != std::string::npos)
                        {
                            score = 0.5;
                            reason = "the log names it";
                        }

                        if (score > 0.0)
                        {
                            SettingCandidate c;
                            c.evidence.eventIndex = static_cast<int>(e);
                            c.evidence.source = "config file";
                            c.evidence.key = entry.key;
                            c.evidence.file = file.path;
                            c.evidence.line = entry.line;
                            c.evidence.value = entry.value;
                            c.evidence.howToChange = "Edit " + place + ": " + entry.key + " = " + entry.value;
                            c.logMatch = score;
                            c.logReason = reason;
                            list.push_back(c);
                        }
                    }
                }

                for (size_t c = 0; c < list.size(); ++c)
                {
                    SettingEvidence& s = list[c].evidence;
                    s.score = std::min(1.0, list[c].locality + list[c].logMatch);
                    s.reason = list[c].localReason;
                    if (!list[c].logReason.empty())
                    {
                        s.reason += (s.reason.empty() ? "" : "; ") + list[c].logReason;
                    }
                }

                // Settings stored in the same variable are alternatives to each other: INVENTORY_DATA_DIR and
                // data.dir both set dataDir, so either one can fix a wrong data folder.
                std::vector<SettingCandidate> before = list;
                for (size_t c = 0; c < list.size(); ++c)
                {
                    for (size_t o = 0; o < before.size(); ++o)
                    {
                        if (o == c || list[c].variableKey.empty() || before[o].variableKey != list[c].variableKey)
                        {
                            continue;
                        }

                        double shared = 0.8 * before[o].evidence.score;
                        if (shared > list[c].evidence.score)
                        {
                            list[c].evidence.score = shared;
                            list[c].evidence.reason = "an alternative to " + before[o].evidence.key + " (both set " +
                                                      list[c].variableKey.substr(list[c].variableKey.rfind('|') + 1) + ")";
                        }
                    }
                }

                // Only links that are clearly there: at least 0.3, and at least half as strong as the best one.
                std::stable_sort(list.begin(), list.end(), [](const SettingCandidate& a, const SettingCandidate& b)
                    { return a.evidence.score > b.evidence.score; });
                const double best = list.empty() ? 0.0 : list.front().evidence.score;
                for (size_t c = 0; c < list.size() && c < LogAnalyzer::kMaxSettingsPerEvent; ++c)
                {
                    if (list[c].evidence.score >= 0.3 && list[c].evidence.score >= 0.5 * best)
                    {
                        result.settings.push_back(list[c].evidence);
                    }
                }
            }
        }

        // Adds the conditions and call chain to each piece of evidence, then links settings to the problems.
        void AddFlowAndSettings(const KnowledgeBase& kb, LogAnalysis& result,
            std::map<std::string, std::vector<std::string>>& cache, const std::set<std::string>& commonWords)
        {
            if (!kb.HasFlow())
            {
                return;
            }

            FlowIndex flow(kb);
            for (size_t i = 0; i < result.evidence.size(); ++i)
            {
                Evidence& ev = result.evidence[i];
                FunctionRef ref;
                bool found = flow.Find(ev.file, ev.name, ev.line, ref);
                if (ev.kind == "message" && found)
                {
                    const SourceFileInfo& file = flow.File(ref);
                    for (size_t m = 0; m < file.messages.size(); ++m)
                    {
                        if (file.messages[m].line == ev.line)
                        {
                            ev.guards = file.messages[m].guards;
                            break;
                        }
                    }
                }

                if (!found)
                {
                    continue;
                }

                const SymbolInfo& fn = flow.Function(ref);
                if (ev.guards.empty() && ev.kind == "location")
                {
                    const std::vector<std::string>& lines = SourceLines(kb.root, ev.file, cache);
                    if (!lines.empty())
                    {
                        ev.guards = ComputeGuards(lines, flow.File(ref).language, fn.line, ev.line);
                    }
                }

                ev.callChain = flow.CallChain(ref);
                std::vector<std::pair<FunctionRef, int>> up = flow.Callers(ref);
                for (size_t u = 0; u < up.size() && u < 3; ++u)
                {
                    ev.callers.push_back(flow.Function(up[u].first).name + " (" + flow.File(up[u].first).path + ":" +
                                         std::to_string(up[u].second) + ")");
                }

                if (up.size() > 3)
                {
                    ev.callers.push_back("and " + std::to_string(up.size() - 3) + " more");
                }
            }

            LinkSettings(kb, flow, result, cache, commonWords);
        }

    }  // namespace

    bool LogAnalyzer::Analyze(const KnowledgeBase& kb, const std::wstring& logPath, LogAnalysis& result,
        std::string& error) const
    {
        std::string text;
        if (!ReadFileBytes(logPath, text, error, 32ull * 1024 * 1024))
        {
            return false;
        }

        std::vector<std::string> lines = SplitLines(text);
        result = LogAnalysis();
        result.logPath = logPath;
        result.totalLines = static_cast<int>(lines.size());

        // If most lines start like log records (a timestamp etc.), every line up to the next record belongs
        // to the previous one: multi-line messages and stack traces become one event, not several.
        size_t nonEmpty = 0, recordStarts = 0;
        for (size_t i = 0; i < lines.size() && i < 2000; ++i)
        {
            if (Trim(lines[i]).empty())
            {
                continue;
            }

            ++nonEmpty;
            if (lines[i].size() < 4000 && std::regex_search(lines[i], RecordStartRe()))
            {
                ++recordStarts;
            }
        }

        // Stack traces can outnumber record lines, so a modest share of record starts is enough.
        const bool structured = recordStarts >= 3 && recordStarts * 10 >= nonEmpty;
        auto isContinuation = [&](const std::string& line)
        {
            if (structured)
            {
                return line.size() >= 4000 || !std::regex_search(line, RecordStartRe());
            }

            return LooksLikeContinuation(line);
        };

        // 1. Error events, grouped by normalised text; fall back to warnings if there are no errors.
        for (int pass = 0; pass < 2 && result.events.empty(); ++pass)
        {
            const std::regex& re = pass == 0 ? SeverityRe() : WarningRe();

            // An uncaught exception printed without a timestamp ("Exception in thread "main" java.lang...",
            // "java.io.IOException: ...") right after an ordinary record is a problem of its own, not its detail.
            static const std::regex uncaught(R"(^(?:Exception in thread "|Unhandled [Ee]xception|[\w$]+(?:\.[\w$]+)+(?:Exception|Error)\b))",
                std::regex::optimize);
            bool recordIsError = false;
            std::map<std::string, size_t> seen;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const std::string& line = lines[i];
                if (line.size() > 4000)
                {
                    continue;
                }

                if (structured && isContinuation(line) &&
                    (recordIsError || pass != 0 || !std::regex_search(line, uncaught)))
                {
                    continue;   // part of an earlier record
                }

                std::smatch m;
                if (!std::regex_search(line, m, re) || IsLowSeverityRecord(line))
                {
                    recordIsError = false;
                    continue;
                }

                recordIsError = true;

                if (pass == 0)
                {
                    ++result.errorLines;
                }

                // The event's context: following lines that belong to the same record / stack trace.
                std::vector<std::string> context;
                size_t j = i + 1;
                for (; j < lines.size() && isContinuation(lines[j]); ++j)
                {
                    if (context.size() < 14 && !Trim(lines[j]).empty())
                    {
                        context.push_back(Shorten(lines[j], 300));
                    }
                }

                size_t next = j;   // lines consumed as context are not scanned again as events

                std::string key = NormaliseForGrouping(line);
                if (!context.empty())
                {
                    key += "\n" + NormaliseForGrouping(context.back());  // e.g. the exception line
                }

                std::map<std::string, size_t>::iterator found = seen.find(key);
                if (found != seen.end())
                {
                    ++result.events[found->second].occurrences;
                }
                else if (result.events.size() < kMaxEvents)
                {
                    LogEvent ev;
                    ev.line = static_cast<int>(i) + 1;
                    ev.severity = pass == 0 ? m[1].str() : "warning";
                    for (size_t c = 0; c < ev.severity.size(); ++c)
                    {
                        ev.severity[c] = static_cast<char>(std::toupper(static_cast<unsigned char>(ev.severity[c])));
                    }

                    ev.text = Shorten(Trim(line), 600);
                    ev.context = context;
                    seen[key] = result.events.size();
                    result.events.push_back(ev);
                }

                if (!structured)
                {
                    // Unstructured logs: skip the stack-trace lines just consumed so they aren't re-reported.
                    if (next > i + 1)
                    {
                        i = next - 1;
                    }
                }
                else
                {
                    i = next - 1;
                }
            }
        }

        // 2. Evidence, without any AI.
        std::vector<Evidence> evidence;
        for (size_t e = 0; e < result.events.size(); ++e)
        {
            const LogEvent& ev = result.events[e];
            std::string all = ev.text;
            for (size_t c = 0; c < ev.context.size(); ++c)
            {
                all += "\n" + ev.context[c];
            }

            std::string lowerAll = ToLowerAscii(all);

            // 2a. Stack frames that point into the knowledge base (exact file and line).
            std::vector<std::string> frameLines(1, ev.text);
            frameLines.insert(frameLines.end(), ev.context.begin(), ev.context.end());
            for (size_t f = 0; f < frameLines.size(); ++f)
            {
                const std::string& fl = frameLines[f];
                std::smatch m;
                std::string path, func;
                int lineNo = 0;
                bool innermostLast = false;   // Python lists the failing frame last; Java and debuggers list it first
                if (std::regex_search(fl, m, PythonFrameRe()))
                {
                    path = m[1].str();
                    lineNo = std::atoi(m[2].str().c_str());
                    func = m[3].str();
                    innermostLast = true;
                }
                else if (std::regex_search(fl, m, GenericFrameRe()))
                {
                    path = m[1].str();
                    lineNo = std::atoi(m[2].str().c_str());
                }

                if (path.empty())
                {
                    continue;
                }

                const SourceFileInfo* file = FindFileForPath(kb, path);
                if (!file)
                {
                    continue;
                }

                Evidence ev2;
                ev2.kind = "location";
                ev2.file = file->path;
                ev2.line = lineNo;
                ev2.name = func.empty() ? EnclosingFunction(*file, lineNo) : func;
                ev2.detail = "stack trace points here: " + Shorten(Trim(fl), 160);
                ev2.eventIndex = static_cast<int>(e);
                // Frames closer to the fault score higher; the position in the trace says which those are.
                const double step = 0.01 * static_cast<double>(std::min<size_t>(f, 9));
                ev2.score = innermostLast ? 0.9 + step : 0.99 - step;
                evidence.push_back(ev2);
            }

            // 2b. Log text that matches a message string in the source (where the error was logged/raised).
            for (size_t fi = 0; fi < kb.files.size(); ++fi)
            {
                const SourceFileInfo& file = kb.files[fi];
                for (size_t mi = 0; mi < file.messages.size(); ++mi)
                {
                    const MessageInfo& msg = file.messages[mi];
                    std::vector<std::string> parts = FixedParts(msg.text);
                    if (parts.empty())
                    {
                        continue;
                    }

                    size_t longest = 0, total = 0, matched = 0, pos = 0;
                    bool inOrder = true;
                    for (size_t p = 0; p < parts.size(); ++p)
                    {
                        longest = std::max(longest, parts[p].size());
                        total += parts[p].size();

                        size_t at = lowerAll.find(parts[p], pos);
                        if (at == std::string::npos)
                        {
                            inOrder = false;
                            continue;
                        }

                        matched += parts[p].size();
                        pos = at + parts[p].size();
                    }

                    if (longest < 8 || matched < 8)
                    {
                        continue;
                    }

                    double coverage = static_cast<double>(matched) / static_cast<double>(total);
                    if (coverage < 0.6)
                    {
                        continue;
                    }

                    Evidence ev2;
                    ev2.kind = "message";
                    ev2.file = file.path;
                    ev2.line = msg.line;
                    ev2.name = msg.function;
                    ev2.detail = "log text matches the message \"" + Shorten(msg.text, 120) + "\"";
                    ev2.eventIndex = static_cast<int>(e);
                    ev2.score = 0.5 + 0.4 * coverage * (inOrder ? 1.0 : 0.8) + std::min(0.05, total / 2000.0);
                    evidence.push_back(ev2);
                }
            }

            // 2c. Identifiers in the error that name a function or class in the source.
            static const std::regex identRe(R"([A-Za-z_]\w{3,}(?:(?:::|\.)[A-Za-z_]\w*)*)", std::regex::optimize);
            std::set<std::string> idents;
            std::sregex_iterator it(all.begin(), all.end(), identRe), end;
            for (; it != end && idents.size() < 200; ++it)
            {
                idents.insert(LastComponent(it->str()));
            }

            for (size_t fi = 0; fi < kb.files.size(); ++fi)
            {
                const SourceFileInfo& file = kb.files[fi];
                for (size_t si = 0; si < file.symbols.size(); ++si)
                {
                    const SymbolInfo& sym = file.symbols[si];
                    std::string simple = LastComponent(sym.name);
                    if (simple.size() < 5 || !LooksLikeCodeIdentifier(simple) || !idents.count(simple))
                    {
                        continue;
                    }

                    Evidence ev2;
                    ev2.kind = "symbol";
                    ev2.file = file.path;
                    ev2.line = sym.line;
                    ev2.name = sym.name;
                    ev2.detail = "the error mentions " + sym.kind + " " + simple;
                    ev2.eventIndex = static_cast<int>(e);
                    ev2.score = 0.3 + std::min(0.1, simple.size() / 200.0);
                    evidence.push_back(ev2);
                }
            }
        }

        // 3. Keep the strongest piece of evidence per source location, best first, with snippets.
        std::sort(evidence.begin(), evidence.end(), [](const Evidence& a, const Evidence& b)
            {
            if (a.score != b.score)
            {
                return a.score > b.score;
            }

            return a.eventIndex < b.eventIndex; });

        std::set<std::string> used;
        std::map<std::string, std::vector<std::string>> cache;
        for (size_t i = 0; i < evidence.size() && result.evidence.size() < kMaxEvidence; ++i)
        {
            std::ostringstream key;
            key << evidence[i].file << ":" << evidence[i].line;
            if (!used.insert(key.str()).second)
            {
                continue;
            }

            Evidence ev = evidence[i];
            ev.snippet = Snippet(kb.root, ev.file, ev.line, cache);
            result.evidence.push_back(ev);
        }

        // 4. Program flow and settings for the evidence that was kept. Words on more than half of the log's
        //    lines (logger and program names) are not used to match settings by name.
        std::map<std::string, size_t> wordLines;
        size_t counted = 0;
        for (size_t i = 0; i < lines.size() && counted < 5000; ++i)
        {
            if (Trim(lines[i]).empty() || lines[i].size() > 4000)
            {
                continue;
            }

            ++counted;
            std::set<std::string> words = TextWords(lines[i]);
            for (std::set<std::string>::const_iterator w = words.begin(); w != words.end(); ++w)
            {
                ++wordLines[*w];
            }
        }

        std::set<std::string> commonWords;
        for (std::map<std::string, size_t>::const_iterator w = wordLines.begin(); w != wordLines.end() && counted >= 4; ++w)
        {
            if (w->second * 2 > counted)
            {
                commonWords.insert(w->first);
            }
        }

        AddFlowAndSettings(kb, result, cache, commonWords);
        return true;
    }

    // =====================================================================================
    // Report and prompt
    // =====================================================================================

    namespace
    {
        // "happens when: ..." and "reached from: ..." lines for one piece of evidence.
        std::string FlowLines(const Evidence& ev, const std::string& indent)
        {
            std::string out;
            if (!ev.guards.empty())
            {
                out += indent + "happens when: ";
                for (size_t g = 0; g < ev.guards.size(); ++g)
                {
                    out += (g ? "; " : "") + ev.guards[g].text + " [line " + std::to_string(ev.guards[g].line) + "]";
                }

                out += "\n";
            }

            if (!ev.callChain.empty())
            {
                out += indent + "reached from: " + ev.callChain + "\n";
            }

            if (ev.callers.size() > 1)
            {
                out += indent + "called from: ";
                for (size_t c = 0; c < ev.callers.size(); ++c)
                {
                    out += (c ? ", " : "") + ev.callers[c];
                }

                out += "\n";
            }

            return out;
        }

        // The prompt gets one short line: small models copy longer flow text into the answer and lose the format.
        std::string PromptFlowLine(const Evidence& ev)
        {
            std::string out;
            if (!ev.guards.empty())
            {
                out += "when " + ev.guards.back().text;
            }

            if (!ev.callChain.empty())
            {
                out += (out.empty() ? "" : "; ") + std::string("via ") + ev.callChain;
            }

            return out.empty() ? out : "      (" + out + ")\n";
        }

    }  // namespace

    std::string FormatDeterministicReport(const KnowledgeBase& kb, const LogAnalysis& a)
    {
        std::ostringstream os;
        os << "=== Log summary ===\n";
        os << "Log: " << WideToUtf8(a.logPath) << "\n";
        os << "Lines: " << a.totalLines << "   error lines: " << a.errorLines
           << "   distinct problems: " << a.events.size() << "\n";
        os << "Knowledge base: " << kb.files.size() << " files, " << kb.SymbolCount() << " symbols, "
           << kb.MessageCount() << " messages";
        if (kb.HasFlow())
        {
            os << ", " << kb.CallCount() << " calls, " << kb.SettingCount() << " settings";
        }

        os << " (source: " << kb.root << ", built " << kb.created << ")\n";
        if (!kb.HasFlow())
        {
            os << "Note: this knowledge base was built by an earlier AppAssist. Rebuild it to see the program flow\n"
                  "and the settings that may resolve each problem.\n";
        }

        os << "\n";

        if (a.events.empty())
        {
            os << "No error, exception or warning lines were found in this log.\n";
            return os.str();
        }

        os << "=== Problems found in the log ===\n";
        for (size_t i = 0; i < a.events.size(); ++i)
        {
            const LogEvent& ev = a.events[i];
            os << "[" << (i + 1) << "] line " << ev.line;
            if (ev.occurrences > 1)
            {
                os << " (x" << ev.occurrences << ")";
            }

            os << "  " << ev.severity << "\n    " << ev.text << "\n";
            for (size_t c = 0; c < ev.context.size() && c < 6; ++c)
            {
                os << "    " << ev.context[c] << "\n";
            }
        }

        os << "\n=== Matching source code (found without AI) ===\n";
        if (a.evidence.empty())
        {
            os << "No log line could be matched to the knowledge base (no message strings, stack frames or\n"
                  "function names in common). The source may have changed since the knowledge base was built,\n"
                  "or the log may come from another component.\n";
        }

        for (size_t i = 0; i < a.evidence.size(); ++i)
        {
            const Evidence& ev = a.evidence[i];
            os << "[E" << (i + 1) << "] " << ev.file << ":" << ev.line;
            if (!ev.name.empty())
            {
                os << "  in " << ev.name;
            }

            os << "  (problem " << (ev.eventIndex + 1) << ", " << ev.kind << ")\n    " << ev.detail << "\n";
            os << FlowLines(ev, "    ");
            if (!ev.snippet.empty())
            {
                os << ev.snippet;
            }

            os << "\n";
        }

        if (kb.HasFlow() && !a.evidence.empty())
        {
            os << "=== Settings that may resolve it (found without AI) ===\n";
            if (a.settings.empty())
            {
                os << "No setting was found near the failing code or named in the log.\n";
            }

            for (size_t i = 0; i < a.settings.size(); ++i)
            {
                const SettingEvidence& s = a.settings[i];
                os << "[C" << (i + 1) << "] problem " << (s.eventIndex + 1) << ": " << s.source << " " << s.key << "  ("
                   << s.file << ":" << s.line << (s.function.empty() ? "" : " in " + s.function) << ")\n"
                   << "    why: " << s.reason << "\n"
                   << "    to change: " << s.howToChange << "\n";
            }

            os << "\n";
        }

        return os.str();
    }

    // The prompt is deliberately rigid: small local models (1-3B) follow an explicit answer template, an
    // exact problem count and evidence placed right under each problem far better than general rules.
    PromptPair BuildDiagnosisPrompt(const LogAnalysis& a, size_t maxUserChars)
    {
        const size_t n = a.events.size();
        PromptPair p;
        std::ostringstream sys;
        sys << "You diagnose software failures using an application log and excerpts of the application's own "
               "source code.\n"
               "Write plain text only: no Markdown, no '#', no '**'.\n"
               "There are exactly "
            << n << " problem(s). Write exactly " << n << " block(s), one per problem, in "
                                                          "this form, then stop:\n\n"
                                                          "Problem <number>: <one-line summary>\n"
                                                          "Root cause: <what went wrong and why, based on the log and the code shown>\n"
                                                          "Evidence: <references only, like: [E1] path/file.py:33, [E2] path/file.py:367; or 'none'>\n"
                                                          "Workaround: <what to do about it; if a listed setting [C#] would avoid the problem, name it and the value to use> (inferred from the code - unverified)\n"
                                                          "Confidence: <high, medium or low>\n\n"
                                                          "Under an evidence item, (when ...) is the condition in the code that leads to the error and (via ...) "
                                                          "the call path to it.\n"
                                                          "Rules: use only the information given. Never invent files, functions, settings, problems or evidence numbers. "
                                                          "Never copy source code or log text into the answer: refer to it by its [E#] reference. Keep each "
                                                          "block under 120 words. If the evidence is not enough, say so in Root cause and give low confidence.";
        p.system = sys.str();

        std::ostringstream os;
        os << "The log has " << a.totalLines << " lines and " << n << " problem(s).\n";
        std::string user = os.str();
        for (size_t i = 0; i < n; ++i)
        {
            const LogEvent& ev = a.events[i];
            std::ostringstream block;
            block << "\nProblem " << (i + 1) << " (log line " << ev.line << ", " << ev.severity
                  << (ev.occurrences > 1 ? ", repeated " + std::to_string(ev.occurrences) + " times" : "") << "):\n"
                  << "  " << ev.text << "\n";

            for (size_t c = 0; c < ev.context.size() && c < 8; ++c)
            {
                block << "  " << ev.context[c] << "\n";
            }

            block << "Evidence for problem " << (i + 1) << ":\n";
            bool any = false;
            for (size_t e = 0; e < a.evidence.size(); ++e)
            {
                const Evidence& evd = a.evidence[e];
                if (evd.eventIndex != static_cast<int>(i))
                {
                    continue;
                }

                any = true;
                std::ostringstream item;
                item << "  [E" << (e + 1) << "] " << evd.file << ":" << evd.line
                     << (evd.name.empty() ? "" : " in " + evd.name) << " - " << evd.detail << "\n"
                     << PromptFlowLine(evd) << evd.snippet;

                // Keep within the context window: drop snippets first, then whole items.
                std::string text = item.str();
                if (user.size() + block.str().size() + text.size() > maxUserChars)
                {
                    std::ostringstream shortItem;
                    shortItem << "  [E" << (e + 1) << "] " << evd.file << ":" << evd.line << " - " << evd.detail << "\n";
                    text = shortItem.str();
                    if (user.size() + block.str().size() + text.size() > maxUserChars)
                    {
                        break;
                    }
                }

                block << text;
            }

            if (!any)
            {
                block << "  none found in the source code\n";
            }

            bool anySetting = false;
            for (size_t s = 0; s < a.settings.size(); ++s)
            {
                const SettingEvidence& st = a.settings[s];
                if (st.eventIndex != static_cast<int>(i))
                {
                    continue;
                }

                std::ostringstream item;
                item << (anySetting ? "" : "Settings that may help with problem " + std::to_string(i + 1) + ":\n")
                     << "  [C" << (s + 1) << "] " << st.source << " " << st.key << ": " << st.howToChange << " ("
                     << st.reason << ")\n";
                if (user.size() + block.str().size() + item.str().size() > maxUserChars)
                {
                    break;
                }

                block << item.str();
                anySetting = true;
            }

            user += block.str();
        }

        user += "\nWrite the " + std::to_string(n) + " block(s) now.";
        p.user = user;
        return p;
    }

    namespace
    {
        // "Problem 7:" / "### Problem 7" / "**Problem 7**" -> 7; 0 if the line is not a problem header.
        int ProblemHeaderNumber(const std::string& line)
        {
            static const std::regex re(R"(^\s*[#*\s]*Problem\s+(\d+)\b)", std::regex::icase | std::regex::optimize);
            std::smatch m;
            return std::regex_search(line, m, re) ? std::atoi(m[1].str().c_str()) : 0;
        }

    }  // namespace

    bool DiagnosisRunner::Run(const KnowledgeBase& kb, const std::wstring& logPath, ILanguageModel* model,
        const std::function<void(const std::string&)>& emit,
        const std::atomic<bool>* cancel, std::string& error)
    {
        LogAnalyzer analyzer;
        LogAnalysis analysis;
        if (!analyzer.Analyze(kb, logPath, analysis, error))
        {
            return false;
        }

        emit(FormatDeterministicReport(kb, analysis));
        if (cancel && cancel->load())
        {
            error = "Cancelled.";
            return false;
        }

        if (!model)
        {
            emit("=== AI analysis ===\nNo language model selected: showing the source matches only.\n");
            return true;
        }

        if (analysis.events.empty())
        {
            return true;
        }

        emit("=== AI analysis (" + model->Describe() + ") ===\n");
        PromptPair prompt = BuildDiagnosisPrompt(analysis);

        // Text is released line by line so two failure modes of small models can be cut off before they
        // reach the report: inventing problems beyond the real count, and repeating the same line.
        const int problemCount = static_cast<int>(analysis.events.size());
        std::string line;
        std::map<std::string, int> repeats;
        bool runaway = false;
        auto releaseLine = [&](const std::string& text) -> bool
        {
            // false = stop generating
            int number = ProblemHeaderNumber(text);
            if (number > problemCount)
            {
                return false;
            }

            std::string key = Trim(text);
            if (key.size() > 25 && ++repeats[key] > 2)
            {
                return false;
            }

            emit(text);
            return true;
        };

        std::string genError;
        bool ok = model->Generate(prompt.system, prompt.user, [&](const std::string& piece)
            {
            if (cancel && cancel->load())
            {
                return false;
            }

            line += piece;
            size_t nl;
            while ((nl = line.find('\n')) != std::string::npos)
            {
                if (!releaseLine(line.substr(0, nl + 1)))
                {
                    runaway = true;
                    line.clear();
                    return false;
                }

                line.erase(0, nl + 1);
            }

            return true; }, genError);

        if (!runaway && !line.empty())
        {
            releaseLine(line);
        }

        emit("\n");
        if (runaway)
        {
            emit("[AppAssist stopped the model: it started repeating itself or inventing problems.]\n");
        }

        if (cancel && cancel->load())
        {
            error = "Cancelled.";
            return false;
        }

        if (!ok)
        {
            error = genError;
            return false;
        }

        emit("\n--- End of report. AI text is a suggestion: check it against the cited code. ---\n");
        return true;
    }

}  // namespace appassist
