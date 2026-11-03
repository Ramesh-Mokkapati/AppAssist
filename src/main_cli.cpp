// main_cli.cpp - AppAssistCli: the same engine as the GUI, for scripts and CI.
//
//   AppAssistCli index   <source_dir> <knowledge_base.kdb>
//   AppAssistCli analyze <knowledge_base.kdb> <log_file> [--model <file.gguf>] [--ctx N] [--max-tokens N]
//                        [--threads N] [--http <url> [--http-model <name>]] [--out <report.txt>]
//   AppAssistCli models   [--task <id>] [--kb <file.kdb> | --source <folder>] [--model <file.gguf>] [--dir <folder>]
//   AppAssistCli download <id|first|recommended> [same options as models]
//   AppAssistCli --version
//
// Exit codes: 0 success, 1 usage error, 2 failure, 3 cancelled (Ctrl+C).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "CoreEngine.h"
#include "EmbeddedLlamaEngine.h"
#include "LocalLLMClient.h"
#include "ModelAdvisor.h"
#include "ModelDownloader.h"
#include "Version.h"

using namespace appassist;

namespace
{
    std::atomic<bool> g_cancel(false);
    LocalLLMClient* g_httpClient = nullptr;   // so Ctrl+C can abort an HTTP request in flight
    ModelDownloader* g_downloader = nullptr;  // ... or a model download

    BOOL WINAPI OnConsoleCtrl(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
        {
            g_cancel = true;
            if (g_httpClient)
            {
                g_httpClient->Cancel();
            }

            if (g_downloader)
            {
                g_downloader->Cancel();
            }

            return TRUE;
        }

        return FALSE;
    }

    // Console output in UTF-8 (SetConsoleOutputCP), so Telugu/Unicode paths and text display correctly.
    void Out(const std::string& text)
    {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }

    void Err(const std::string& text)
    {
        std::fwrite(text.data(), 1, text.size(), stderr);
        std::fputc('\n', stderr);
    }

    int Usage()
    {
        Err("AppAssistCli - source-code knowledge base and log root-cause diagnosis\n\n"
            "Usage:\n"
            "  AppAssistCli index   <source_dir> <knowledge_base.kdb>\n"
            "  AppAssistCli analyze <knowledge_base.kdb> <log_file> [options]\n\n"
            "Analyze options (without a model, only the source matches are reported):\n"
            "  --model <file.gguf>    run a local GGUF model in-process (embedded llama.cpp)\n"
            "  --ctx <tokens>         context size (default 8192)\n"
            "  --max-tokens <n>       answer length limit (default 900)\n"
            "  --threads <n>          CPU threads (default: half the logical processors)\n"
            "  --http <url>           use a local server instead, e.g. http://localhost:1234/v1/chat/completions\n"
            "  --http-model <name>    model name for the server (LM Studio / Ollama)\n"
            "  --out <report.txt>     also save the report (UTF-8)\n\n"
            "  AppAssistCli models   [--task <id>] [--kb <file.kdb> | --source <folder>] [--model <file.gguf>] [--dir <folder>]\n"
            "  AppAssistCli download <id|first|recommended> [same options as models]\n"
            "  AppAssistCli --version\n\n"
            "  models    lists the task's models with what fits this PC, what is downloaded, and the advice:\n"
            "            the first (smallest) model to start with, and with --kb or --source the model that\n"
            "            suits that code. --task picks the task profile from models.json (default\n"
            "            log-diagnosis; also code-conversion)\n"
            "  download  downloads (resuming a partial file) and verifies the SHA-256; an existing\n"
            "            file is only verified. --dir is the folder (default: next to --model, else\n"
            "            <exe>\\models, else %LOCALAPPDATA%\\AppAssist\\models)");
        return 1;
    }

    struct AdvisorOptions
    {
        std::string task = "log-diagnosis";
        std::wstring kdbPath;
        std::wstring sourceDir;
        std::wstring modelPath;
        std::wstring dir;
    };

    bool ParseAdvisorOptions(const std::vector<std::wstring>& args, size_t first, AdvisorOptions& options)
    {
        for (size_t i = first; i < args.size(); ++i)
        {
            const std::wstring& a = args[i];
            bool hasValue = i + 1 < args.size();
            if (a == L"--kb" && hasValue)
            {
                options.kdbPath = args[++i];
            }
            else if (a == L"--task" && hasValue)
            {
                options.task = WideToUtf8(args[++i]);
            }
            else if (a == L"--source" && hasValue)
            {
                options.sourceDir = args[++i];
            }
            else if (a == L"--model" && hasValue)
            {
                options.modelPath = args[++i];
            }
            else if (a == L"--dir" && hasValue)
            {
                options.dir = args[++i];
            }
            else
            {
                Err("Unknown or incomplete option: " + WideToUtf8(a));
                return false;
            }
        }

        return true;
    }

    // Everything the advice needs: the task profile, resources, the models on disk, and the workload
    // (from a knowledge base, or measured from a source folder) if given.
    bool Advise(const AdvisorOptions& options, SystemResources& res, std::vector<ModelStatus>& models,
        ModelAdvice& advice)
    {
        const TaskProfile* task = FindTaskProfile(options.task);
        if (!task)
        {
            std::string known;
            for (size_t i = 0; i < TaskProfiles().size(); ++i)
            {
                known += (i ? ", " : "") + TaskProfiles()[i].id;
            }
            Err("Unknown task: " + options.task + " (known: " + known + ")");
            return false;
        }

        KnowledgeBase kb;
        std::string error;
        if (!options.kdbPath.empty() && !kb.Load(options.kdbPath, error))
        {
            Err("Error: " + error);
            return false;
        }

        if (options.kdbPath.empty() && !options.sourceDir.empty())
        {
            SourceCodeParser parser([](const std::string&) {}, &g_cancel);
            if (!parser.BuildKnowledgeBase(options.sourceDir, kb, error))
            {
                Err("Error: " + error);
                return false;
            }
        }

        const bool hasInput = !options.kdbPath.empty() || !options.sourceDir.empty();
        Workload workload;
        if (hasInput)
        {
            workload = AssessSourceWorkload(kb);
        }

        std::wstring dir = options.dir.empty() ? DefaultDownloadDir(options.modelPath) : options.dir;
        res = DetectSystemResources(dir);
        std::wstring modelDir;
        size_t slash = options.modelPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            modelDir = options.modelPath.substr(0, slash);
        }

        std::vector<std::wstring> searchDirs = ModelSearchDirs(modelDir);
        if (!options.dir.empty())
        {
            searchDirs.insert(searchDirs.begin(), options.dir);
        }

        models = ScanModels(searchDirs, res, *task);
        advice = AdviseModel(res, models, *task, hasInput ? &workload : nullptr, options.modelPath);
        return true;
    }

    const char* StateLabel(const ModelStatus& s)
    {
        switch (s.state)
        {
        case ModelState::Downloaded:
            return "downloaded";
        case ModelState::Partial:
            return "partial";
        default:
            return "-";
        }
    }

    const char* FitLabel(MemoryFit fit)
    {
        switch (fit)
        {
        case MemoryFit::Comfortable:
            return "fits";
        case MemoryFit::Tight:
            return "tight";
        default:
            return "too large";
        }
    }

    int Models(const std::vector<std::wstring>& args)
    {
        AdvisorOptions options;
        if (!ParseAdvisorOptions(args, 2, options))
        {
            return Usage();
        }

        SystemResources res;
        std::vector<ModelStatus> models;
        ModelAdvice advice;
        if (!Advise(options, res, models, advice))
        {
            return 2;
        }

        Out(DescribeSystem(res) + "\n");
        Out("Task: " + advice.task->name + " (" + advice.task->id + ", " + std::to_string(advice.task->contextTokens) +
            "-token context); catalog: " + CatalogSource() + "\n");
        if (advice.hasWorkload)
        {
            Out(advice.task->workloadNoun + ": " + DescribeWorkload(advice) + "\n");
        }

        Out("\n");
        char line[512];
        sprintf_s(line, "  %-22s %10s %10s  %-10s %-10s %-11s %s\n", "id", "size", "RAM need", "memory", "speed",
            "status", "advice");
        Out(line);
        for (size_t i = 0; i < models.size(); ++i)
        {
            const ModelStatus& s = models[i];
            std::string mark;
            if (s.model == advice.firstModel)
            {
                mark += "first";
            }

            if (s.model == advice.recommended && advice.hasWorkload)
            {
                mark += mark.empty() ? "recommended" : ", recommended";
            }

            if (s.model == advice.current)
            {
                mark += mark.empty() ? "selected" : ", selected";
            }

            sprintf_s(line, "  %-22s %10s %10s  %-10s %-10s %-11s %s\n", s.model->id.c_str(),
                FormatSize(s.model->sizeBytes).c_str(),
                FormatSize(s.model->RequiredRamMB(advice.task->contextTokens) * 1024ull * 1024).c_str(), FitLabel(s.memory),
                SpeedLabel(*s.model, res), StateLabel(s), mark.c_str());
            Out(line);
        }

        Out("\n");
        for (size_t i = 0; i < advice.notes.size(); ++i)
        {
            Out("- " + advice.notes[i] + "\n");
        }

        std::string others;
        for (size_t i = 0; i < TaskProfiles().size(); ++i)
        {
            if (&TaskProfiles()[i] != advice.task)
            {
                others += (others.empty() ? "" : ", ") + TaskProfiles()[i].id;
            }
        }

        if (!others.empty())
        {
            Out("\nOther tasks (--task): " + others + "\n");
        }

        return 0;
    }

    int Download(const std::vector<std::wstring>& args)
    {
        AdvisorOptions options;
        if (!ParseAdvisorOptions(args, 3, options))
        {
            return Usage();
        }

        SystemResources res;
        std::vector<ModelStatus> models;
        ModelAdvice advice;
        if (!Advise(options, res, models, advice))
        {
            return 2;
        }

        const std::string which = WideToUtf8(args[2]);
        const CatalogModel* model = which == "first"         ? advice.firstModel
                                    : which == "recommended" ? advice.recommended
                                                             : FindCatalogModel(which);
        if (!model)
        {
            Err(which == "first" || which == "recommended"
                    ? "No catalog model fits this PC (see AppAssistCli models)."
                    : "Unknown model id: " + which + " (see AppAssistCli models).");
            return 2;
        }

        // Resume where a partial copy already is; otherwise download into the chosen folder.
        std::wstring dir = res.downloadDir;
        for (size_t i = 0; i < models.size(); ++i)
        {
            if (models[i].model == model && models[i].state != ModelState::NotDownloaded && options.dir.empty())
            {
                dir = models[i].path.substr(0, models[i].path.find_last_of(L"\\/"));
            }
        }

        Err(std::string(model->displayName) + " (" + FormatSize(model->sizeBytes) + ") -> " + WideToUtf8(dir));
        ModelDownloader downloader;
        g_downloader = &downloader;
        std::string lastPhase;
        std::wstring finalPath;
        std::string error;
        bool ok = downloader.Download(*model, dir, [&](const DownloadProgress& p)
        {
            char text[160];
            double percent = p.total ? 100.0 * p.done / p.total : 0.0;
            if (p.phase == "Downloading")
            {
                sprintf_s(text, "\r%s %5.1f%%  %s of %s  %.1f MB/s   ", p.phase.c_str(), percent, FormatSize(p.done).c_str(),
                    FormatSize(p.total).c_str(), p.bytesPerSecond / (1024.0 * 1024.0));
            }
            else
            {
                sprintf_s(text, "%s%s %5.1f%%            ", p.phase == lastPhase ? "\r" : "\n", p.phase.c_str(), percent);
            }

            lastPhase = p.phase;
            std::fputs(text, stderr);
        }, finalPath, error);
        g_downloader = nullptr;
        std::fputs("\n", stderr);
        if (!ok)
        {
            Err("Error: " + error);
            return g_cancel ? 3 : 2;
        }

        Err("Verified (SHA-256 " + std::string(model->sha256).substr(0, 16) + "...).");
        Out(WideToUtf8(finalPath) + "\n");
        return 0;
    }

    int Index(const std::wstring& sourceDir, const std::wstring& kdbPath)
    {
        SourceCodeParser parser([](const std::string& msg) { Err(msg); }, &g_cancel);
        KnowledgeBase kb;
        std::string error;
        if (!parser.BuildKnowledgeBase(sourceDir, kb, error))
        {
            Err("Error: " + error);
            return g_cancel ? 3 : 2;
        }

        if (!kb.Save(kdbPath, error))
        {
            Err("Error: " + error);
            return 2;
        }

        char line[256];
        sprintf_s(line, "Knowledge base saved: %zu files, %zu symbols, %zu messages, %zu calls, %zu settings\n",
            kb.files.size(), kb.SymbolCount(), kb.MessageCount(), kb.CallCount(), kb.SettingCount());
        Out(line);
        Out(WideToUtf8(kdbPath) + "\n");
        return 0;
    }

    int Analyze(const std::vector<std::wstring>& args)
    {
        std::wstring kdbPath = args[2], logPath = args[3], modelPath, httpUrl, outPath;
        std::string httpModel;
        LlamaSettings llama;
        for (size_t i = 4; i < args.size(); ++i)
        {
            const std::wstring& a = args[i];
            bool hasValue = i + 1 < args.size();
            if (a == L"--model" && hasValue)
            {
                modelPath = args[++i];
            }
            else if (a == L"--ctx" && hasValue)
            {
                llama.contextSize = _wtoi(args[++i].c_str());
            }
            else if (a == L"--max-tokens" && hasValue)
            {
                llama.maxNewTokens = _wtoi(args[++i].c_str());
            }
            else if (a == L"--threads" && hasValue)
            {
                llama.threads = _wtoi(args[++i].c_str());
            }
            else if (a == L"--http" && hasValue)
            {
                httpUrl = args[++i];
            }
            else if (a == L"--http-model" && hasValue)
            {
                httpModel = WideToUtf8(args[++i]);
            }
            else if (a == L"--out" && hasValue)
            {
                outPath = args[++i];
            }
            else
            {
                Err("Unknown or incomplete option: " + WideToUtf8(a));
                return Usage();
            }
        }

        if (!modelPath.empty() && !httpUrl.empty())
        {
            Err("Use either --model or --http, not both.");
            return 1;
        }

        KnowledgeBase kb;
        std::string error;
        if (!kb.Load(kdbPath, error))
        {
            Err("Error: " + error);
            return 2;
        }

        std::unique_ptr<EmbeddedLlamaEngine> engine;
        std::unique_ptr<LocalLLMClient> http;
        ILanguageModel* model = nullptr;
        if (!modelPath.empty())
        {
            EmbeddedLlamaEngine::SetLogSink([](const std::string& msg) { Err(msg); });
            engine.reset(new EmbeddedLlamaEngine());
            llama.modelPath = modelPath;
            Err("Loading " + WideToUtf8(modelPath) + " ...");
            DWORD start = GetTickCount();
            if (!engine->Load(llama, error))
            {
                Err("Error: " + error);
                return 2;
            }

            char took[64];
            sprintf_s(took, " (%.1f s)", (GetTickCount() - start) / 1000.0);
            Err("Loaded: " + engine->Describe() + took);
            model = engine.get();
        }
        else if (!httpUrl.empty())
        {
            HttpEndpoint endpoint;
            endpoint.url = httpUrl;
            endpoint.model = httpModel;
            endpoint.maxTokens = llama.maxNewTokens;
            http.reset(new LocalLLMClient(endpoint));
            g_httpClient = http.get();
            model = http.get();
        }

        std::string report;
        DWORD start = GetTickCount();
        bool ok = DiagnosisRunner::Run(kb, logPath, model, [&](const std::string& text)
        {
            report += text;
            Out(text);
        }, &g_cancel, error);

        g_httpClient = nullptr;
        char took[64];
        sprintf_s(took, "\n[finished in %.1f s]\n", (GetTickCount() - start) / 1000.0);
        Err(took);

        if (!outPath.empty())
        {
            std::string saveError;
            if (!WriteFileBytes(outPath, report, saveError))
            {
                Err("Could not save the report: " + saveError);
            }
            else
            {
                Err("Report saved to " + WideToUtf8(outPath));
            }
        }

        if (!ok)
        {
            Err("Error: " + error);
            return g_cancel ? 3 : 2;
        }

        return 0;
    }

}  // namespace

int wmain(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    std::vector<std::wstring> args(argv, argv + argc);
    try
    {
        if (args.size() == 2 && (args[1] == L"--version" || args[1] == L"version"))
        {
            Out("AppAssistCli " APPASSIST_VERSION_FULL "\n");
            return 0;
        }

        if (args.size() == 4 && args[1] == L"index")
        {
            return Index(args[2], args[3]);
        }

        if (args.size() >= 4 && args[1] == L"analyze")
        {
            return Analyze(args);
        }

        if (args.size() >= 2 && args[1] == L"models")
        {
            return Models(args);
        }

        if (args.size() >= 3 && args[1] == L"download")
        {
            return Download(args);
        }

        return Usage();
    }
    catch (const std::exception& e)
    {
        Err(std::string("Unexpected error: ") + e.what());
        return 2;
    }
}
