// ModelAdvisor.cpp - see ModelAdvisor.h.
#include "ModelAdvisor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

#include "ModelCatalogDefault.h"
#include "nlohmann/json.hpp"

namespace appassist
{
    using nlohmann::json;

    namespace
    {
        const unsigned long long kMB = 1024ull * 1024ull;
        const unsigned long long kHeadroomMB = 1024;  // llama.cpp buffers, the app, the knowledge base

        std::wstring ExeDirectory()
        {
            wchar_t path[MAX_PATH * 4];
            DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
            std::wstring full(path, n);
            size_t slash = full.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring(L".") : full.substr(0, slash);
        }

        std::wstring CurrentDirectory()
        {
            wchar_t path[MAX_PATH * 4];
            DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(sizeof(path) / sizeof(path[0])), path);
            return std::wstring(path, n);
        }

        std::wstring ParentDirectory(const std::wstring& path)
        {
            size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
        }

        std::wstring FileNameOf(const std::wstring& path)
        {
            size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? path : path.substr(slash + 1);
        }

        unsigned long long FileSizeOf(const std::wstring& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return 0;
            }
            return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }

        // A folder we may create files in (tested with a temporary file that deletes itself).
        bool IsWritableDirectory(const std::wstring& dir)
        {
            if (!DirectoryExists(dir) && !CreateDirectoryW(dir.c_str(), nullptr))
            {
                return false;
            }
            std::wstring probe = JoinPath(dir, L".appassist-write-test");
            HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            CloseHandle(h);
            return true;
        }

        std::wstring UserModelsDir()
        {
            wchar_t base[MAX_PATH * 2];
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, static_cast<DWORD>(sizeof(base) / sizeof(base[0])));
            std::wstring root = (n > 0 && n < sizeof(base) / sizeof(base[0])) ? std::wstring(base, n) : ExeDirectory();
            return JoinPath(JoinPath(root, L"AppAssist"), L"models");
        }

        // Tier of a model that is not in the catalog, judged by its file size (Q4-class quantisation).
        int TierFromFileSize(unsigned long long bytes)
        {
            if (bytes < 2600ull * kMB)
            {
                return 1;
            }
            if (bytes < 7000ull * kMB)
            {
                return 2;
            }
            return 3;
        }

        std::string Gb(unsigned long long mb)
        {
            char text[32];
            sprintf_s(text, "%.1f GB", mb / 1024.0);
            return text;
        }

        MemoryFit FitFor(const CatalogModel& model, const SystemResources& res, int contextTokens)
        {
            const unsigned long long need = model.RequiredRamMB(contextTokens);
            if (need * 4 <= res.totalRamMB * 3)
            {
                return MemoryFit::Comfortable;
            }
            if (need * 10 <= res.totalRamMB * 9)
            {
                return MemoryFit::Tight;
            }
            return MemoryFit::TooLarge;
        }

        bool IsSha256(const std::string& text)
        {
            if (text.size() != 64)
            {
                return false;
            }
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
                {
                    return false;
                }
            }
            return true;
        }

        // The catalog, loaded once: <exe>\models.json if present and valid, else the built-in copy.
        struct Catalog
        {
            std::vector<CatalogModel> models;
            std::vector<TaskProfile> tasks;
            std::string source;
        };

        const Catalog& LoadedCatalog()
        {
            static const Catalog catalog = []()
            {
                Catalog c;
                std::string error;
                const std::wstring path = JoinPath(ExeDirectory(), L"models.json");
                std::string text;
                if (FileExists(path))
                {
                    if (ReadFileBytes(path, text, error) && ParseModelCatalog(text, c.models, c.tasks, error))
                    {
                        c.source = WideToUtf8(path);
                        return c;
                    }
                    c.models.clear();
                    c.tasks.clear();
                    error = WideToUtf8(path) + " is not usable (" + error + "); using the built-in catalog.";
                }
                std::string builtInError;
                ParseModelCatalog(kDefaultModelCatalogJson, c.models, c.tasks, builtInError);
                c.source = error.empty() ? std::string("built-in") : "built-in. " + error;
                return c;
            }();
            return catalog;
        }
    }  // namespace

    // ----- catalog and task profiles -----

    unsigned long long CatalogModel::RequiredRamMB(int contextTokens) const
    {
        const unsigned long long kv = kvCacheMB * static_cast<unsigned long long>(std::max(contextTokens, 512)) / 8192;
        return sizeBytes / kMB * 115 / 100 + kv + kHeadroomMB;
    }

    bool TaskProfile::UsesModel(const std::string& modelId) const
    {
        for (size_t t = 0; t < tiers.size(); ++t)
        {
            if (std::find(tiers[t].prefer.begin(), tiers[t].prefer.end(), modelId) != tiers[t].prefer.end())
            {
                return true;
            }
        }
        return false;
    }

    bool ParseModelCatalog(const std::string& text, std::vector<CatalogModel>& models,
        std::vector<TaskProfile>& tasks, std::string& error)
    {
        models.clear();
        tasks.clear();
        try
        {
            json root = json::parse(text);
            if (root.value("format", std::string()) != "appassist-models")
            {
                error = "not an appassist-models file";
                return false;
            }
            for (const json& m : root.at("models"))
            {
                CatalogModel model;
                model.id = m.at("id").get<std::string>();
                model.displayName = m.value("name", model.id);
                model.fileName = Utf8ToWide(m.at("file").get<std::string>());
                model.url = m.at("url").get<std::string>();
                model.sizeBytes = m.at("size").get<unsigned long long>();
                model.sha256 = ToLowerAscii(m.at("sha256").get<std::string>());
                model.totalParamsB = m.value("params_b", 0.0);
                model.activeParamsB = m.value("active_params_b", model.totalParamsB);
                model.tier = m.value("tier", 1);
                model.kvCacheMB = m.value("kv_cache_mb_8k", 500ull);
                model.license = m.value("license", std::string("see the model card"));
                model.summary = m.value("summary", std::string());
                if (model.sizeBytes == 0 || !IsSha256(model.sha256) || model.tier < 1 || model.tier > 3 ||
                    model.url.compare(0, 8, "https://") != 0)
                {
                    error = "model \"" + model.id + "\" needs an https url, its exact size, a 64-digit sha256 and a tier 1-3";
                    return false;
                }
                models.push_back(model);
            }
            // Smallest first: the advisor's "first model" is the first one that fits.
            std::stable_sort(models.begin(), models.end(), [](const CatalogModel& a, const CatalogModel& b)
            {
                return a.sizeBytes < b.sizeBytes;
            });

            for (const json& t : root.at("tasks"))
            {
                TaskProfile task;
                task.id = t.at("id").get<std::string>();
                task.name = t.value("name", task.id);
                task.workloadNoun = t.value("workload", std::string("input"));
                task.contextTokens = t.value("context_tokens", 8192);
                task.noWorkloadHint = t.value("no_workload_hint", std::string());
                for (const json& r : t.at("tiers"))
                {
                    TaskTier tier;
                    tier.label = r.value("label", std::string());
                    tier.explain = r.value("explain", std::string());
                    if (r.contains("below"))
                    {
                        for (json::const_iterator it = r["below"].begin(); it != r["below"].end(); ++it)
                        {
                            tier.below[it.key()] = it.value().get<double>();
                        }
                    }
                    for (const json& id : r.at("prefer"))
                    {
                        const std::string modelId = id.get<std::string>();
                        bool known = false;
                        for (size_t i = 0; i < models.size(); ++i)
                        {
                            known = known || models[i].id == modelId;
                        }
                        if (!known)
                        {
                            error = "task \"" + task.id + "\" prefers unknown model \"" + modelId + "\"";
                            return false;
                        }
                        tier.prefer.push_back(modelId);
                    }
                    if (tier.prefer.empty())
                    {
                        error = "task \"" + task.id + "\" has a tier without preferred models";
                        return false;
                    }
                    task.tiers.push_back(tier);
                }
                if (task.tiers.empty())
                {
                    error = "task \"" + task.id + "\" has no tiers";
                    return false;
                }
                tasks.push_back(task);
            }
            if (models.empty() || tasks.empty())
            {
                error = "no models or no tasks";
                return false;
            }
            return true;
        }
        catch (const std::exception& e)
        {
            error = e.what();
            models.clear();
            tasks.clear();
            return false;
        }
    }

    const std::vector<CatalogModel>& ModelCatalog()
    {
        return LoadedCatalog().models;
    }

    const std::vector<TaskProfile>& TaskProfiles()
    {
        return LoadedCatalog().tasks;
    }

    std::string CatalogSource()
    {
        return LoadedCatalog().source;
    }

    const CatalogModel* FindCatalogModel(const std::string& id)
    {
        const std::vector<CatalogModel>& catalog = ModelCatalog();
        for (size_t i = 0; i < catalog.size(); ++i)
        {
            if (id == catalog[i].id)
            {
                return &catalog[i];
            }
        }
        return nullptr;
    }

    const TaskProfile* FindTaskProfile(const std::string& id)
    {
        const std::vector<TaskProfile>& tasks = TaskProfiles();
        for (size_t i = 0; i < tasks.size(); ++i)
        {
            if (id == tasks[i].id)
            {
                return &tasks[i];
            }
        }
        return nullptr;
    }

    // ----- this computer -----

    SystemResources DetectSystemResources(const std::wstring& downloadDir)
    {
        SystemResources res;
        MEMORYSTATUSEX mem;
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem))
        {
            res.totalRamMB = mem.ullTotalPhys / kMB;
            res.availableRamMB = mem.ullAvailPhys / kMB;
        }
        SYSTEM_INFO info;
        GetNativeSystemInfo(&info);
        res.logicalCores = info.dwNumberOfProcessors;
        res.avx2 = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != FALSE;
        res.downloadDir = downloadDir;

        // Free space on the download folder's drive (walk up to an existing folder first).
        std::wstring probe = downloadDir;
        while (!probe.empty() && !DirectoryExists(probe))
        {
            probe = ParentDirectory(probe);
        }
        ULARGE_INTEGER freeBytes;
        if (!probe.empty() && GetDiskFreeSpaceExW(probe.c_str(), &freeBytes, nullptr, nullptr))
        {
            res.freeDiskMB = freeBytes.QuadPart / kMB;
        }
        return res;
    }

    // ----- workload -----

    Workload AssessSourceWorkload(const KnowledgeBase& kb)
    {
        unsigned long long lines = 0;
        size_t symbols = 0;
        size_t messages = 0;
        int largest = 0;
        std::string largestPath;
        std::set<std::string> languages;
        for (size_t i = 0; i < kb.files.size(); ++i)
        {
            const SourceFileInfo& f = kb.files[i];
            lines += static_cast<unsigned long long>(f.lineCount);
            symbols += f.symbols.size();
            messages += f.messages.size();
            languages.insert(f.language);
            if (f.lineCount > largest)
            {
                largest = f.lineCount;
                largestPath = f.path;
            }
        }

        Workload w;
        w.metrics["files"] = static_cast<double>(kb.files.size());
        w.metrics["lines"] = static_cast<double>(lines);
        w.metrics["symbols"] = static_cast<double>(symbols);
        w.metrics["messages"] = static_cast<double>(messages);
        w.metrics["languages"] = static_cast<double>(languages.size());
        w.metrics["largest_file_lines"] = largest;

        std::ostringstream os;
        os << lines << " lines of code, " << symbols << " symbols and " << messages << " messages in " << kb.files.size()
           << " files" << (languages.size() > 1 ? " (more than one language)" : "");
        if (largest > 0)
        {
            os << "; largest file " << largest << " lines (" << largestPath << ")";
        }
        w.summary = os.str();
        return w;
    }

    WorkloadTier ClassifyWorkload(const TaskProfile& task, const Workload& workload)
    {
        WorkloadTier result;
        for (size_t t = 0; t < task.tiers.size(); ++t)
        {
            bool within = true;
            for (std::map<std::string, double>::const_iterator it = task.tiers[t].below.begin();
                 it != task.tiers[t].below.end(); ++it)
            {
                std::map<std::string, double>::const_iterator m = workload.metrics.find(it->first);
                within = within && (m == workload.metrics.end() || m->second < it->second);
            }
            if (within || t + 1 == task.tiers.size())
            {
                result.tier = static_cast<int>(t) + 1;
                result.rule = &task.tiers[t];
                return result;
            }
        }
        return result;
    }

    // ----- models on disk -----

    std::vector<std::wstring> ModelSearchDirs(const std::wstring& extraDir)
    {
        std::vector<std::wstring> dirs;
        const std::wstring candidates[] = {
            extraDir,
            JoinPath(ExeDirectory(), L"models"),
            JoinPath(CurrentDirectory(), L"models"),
            UserModelsDir(),
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
        {
            const std::wstring& dir = candidates[i];
            if (dir.empty())
            {
                continue;
            }
            bool seen = false;
            for (size_t j = 0; j < dirs.size(); ++j)
            {
                seen = seen || _wcsicmp(dirs[j].c_str(), dir.c_str()) == 0;
            }
            if (!seen)
            {
                dirs.push_back(dir);
            }
        }
        return dirs;
    }

    std::wstring DefaultDownloadDir(const std::wstring& currentModelPath)
    {
        if (!currentModelPath.empty())
        {
            std::wstring dir = ParentDirectory(currentModelPath);
            if (!dir.empty() && DirectoryExists(dir))
            {
                return dir;
            }
        }
        std::wstring nextToExe = JoinPath(ExeDirectory(), L"models");
        if (IsWritableDirectory(nextToExe))
        {
            return nextToExe;
        }
        return UserModelsDir();
    }

    std::vector<ModelStatus> ScanModels(const std::vector<std::wstring>& searchDirs, const SystemResources& res,
        const TaskProfile& task)
    {
        std::vector<ModelStatus> result;
        const std::vector<CatalogModel>& catalog = ModelCatalog();
        for (size_t m = 0; m < catalog.size(); ++m)
        {
            if (!task.UsesModel(catalog[m].id))
            {
                continue;
            }
            ModelStatus status;
            status.model = &catalog[m];
            status.memory = FitFor(catalog[m], res, task.contextTokens);
            for (size_t d = 0; d < searchDirs.size() && status.state != ModelState::Downloaded; ++d)
            {
                std::wstring path = JoinPath(searchDirs[d], catalog[m].fileName);
                unsigned long long size = FileSizeOf(path);
                unsigned long long partial = FileSizeOf(path + L".part");
                if (size == catalog[m].sizeBytes)
                {
                    status.state = ModelState::Downloaded;
                    status.path = path;
                    status.bytesOnDisk = size;
                }
                else if ((size > 0 || partial > 0) && status.state == ModelState::NotDownloaded)
                {
                    status.state = ModelState::Partial;  // a download in progress or interrupted
                    status.path = path;
                    status.bytesOnDisk = std::max(size, partial);
                }
            }
            if (status.path.empty())
            {
                status.path = JoinPath(res.downloadDir, catalog[m].fileName);
            }
            const unsigned long long remainingMB = (catalog[m].sizeBytes - std::min(status.bytesOnDisk, catalog[m].sizeBytes)) / kMB;
            status.fitsDisk = status.state == ModelState::Downloaded || res.freeDiskMB >= remainingMB + 1024;
            result.push_back(status);
        }
        return result;
    }

    // ----- advice -----

    ModelAdvice AdviseModel(const SystemResources& res, const std::vector<ModelStatus>& models,
        const TaskProfile& task, const Workload* workload, const std::wstring& currentModelPath)
    {
        ModelAdvice advice;
        advice.task = &task;
        std::ostringstream os;

        if (!res.avx2)
        {
            advice.notes.push_back("This processor has no AVX2, which AppAssist's built-in engine needs. Use a local "
                                   "server (LM Studio or Ollama) or \"No AI\" instead.");
            return advice;
        }

        // The selected model, if any.
        if (!currentModelPath.empty() && FileExists(currentModelPath))
        {
            std::wstring name = FileNameOf(currentModelPath);
            for (size_t i = 0; i < models.size(); ++i)
            {
                if (_wcsicmp(name.c_str(), models[i].model->fileName.c_str()) == 0)
                {
                    advice.current = models[i].model;
                    advice.currentTier = models[i].model->tier;
                }
            }
            if (!advice.current)
            {
                advice.currentTier = TierFromFileSize(FileSizeOf(currentModelPath));
            }
        }

        // First model: the smallest one that runs comfortably here.
        for (size_t i = 0; i < models.size() && !advice.firstModel; ++i)
        {
            if (models[i].memory == MemoryFit::Comfortable)
            {
                advice.firstModel = models[i].model;
            }
        }
        if (!advice.firstModel && !models.empty() && models[0].memory == MemoryFit::Tight)
        {
            advice.firstModel = models[0].model;
        }
        const int ctx = task.contextTokens;
        if (!advice.firstModel)
        {
            advice.notes.push_back("Even the smallest model for " + task.name + " (" +
                                   Gb(models.empty() ? 0 : models[0].model->RequiredRamMB(ctx)) +
                                   " of RAM) is too large for this PC's " + Gb(res.totalRamMB) +
                                   ". Use a local server on another computer, or \"No AI\".");
            return advice;
        }

        const bool noWorkload = !workload || workload->metrics.empty() ||
                                (workload->metrics.count("files") != 0 && workload->metrics.at("files") == 0);
        if (noWorkload)
        {
            advice.recommended = advice.firstModel;
            os << "Start with " << advice.firstModel->displayName << ": the smallest model for " << task.name
               << ", and it runs comfortably here (" << Gb(advice.firstModel->RequiredRamMB(ctx)) << " of your "
               << Gb(res.totalRamMB) << " RAM).";
            advice.notes.push_back(os.str());
            if (!task.noWorkloadHint.empty())
            {
                advice.notes.push_back(task.noWorkloadHint);
            }
        }
        else
        {
            advice.hasWorkload = true;
            advice.workload = *workload;
            advice.workloadTier = ClassifyWorkload(task, *workload);
            const TaskTier& rule = *advice.workloadTier.rule;

            // The task profile's preferred models for this tier, best first; take the first that fits.
            const std::vector<std::string>& preference = rule.prefer;
            const CatalogModel* ideal = FindCatalogModel(preference.front());
            for (int pass = 0; pass < 2 && !advice.recommended; ++pass)
            {
                for (size_t p = 0; p < preference.size() && !advice.recommended; ++p)
                {
                    for (size_t i = 0; i < models.size(); ++i)
                    {
                        const ModelStatus& s = models[i];
                        bool memoryOk = pass == 0 ? s.memory == MemoryFit::Comfortable : s.memory != MemoryFit::TooLarge;
                        if (preference[p] == s.model->id && memoryOk && s.fitsDisk)
                        {
                            advice.recommended = s.model;
                        }
                    }
                }
            }
            if (!advice.recommended)
            {
                advice.recommended = advice.firstModel;
            }

            os << "Your " << task.workloadNoun << " is " << DescribeWorkload(advice) << ". " << rule.explain;
            advice.notes.push_back(os.str());

            if (ideal && advice.recommended != ideal)
            {
                std::ostringstream cap;
                cap << ideal->displayName << " would suit it best but needs " << Gb(ideal->RequiredRamMB(ctx))
                    << " of RAM and " << FormatSize(ideal->sizeBytes) << " of disk; this PC has " << Gb(res.totalRamMB)
                    << " RAM and " << Gb(res.freeDiskMB) << " free. Recommending the best model that fits.";
                advice.notes.push_back(cap.str());
            }
            std::ostringstream rec;
            rec << "Recommended: " << advice.recommended->displayName << " (" << FormatSize(advice.recommended->sizeBytes)
                << ", needs about " << Gb(advice.recommended->RequiredRamMB(ctx)) << " of RAM). " << advice.recommended->summary;
            advice.notes.push_back(rec.str());
        }

        for (size_t i = 0; i < models.size(); ++i)
        {
            if (models[i].model == advice.recommended && models[i].state == ModelState::Downloaded)
            {
                advice.recommendedDownloaded = true;
            }
        }
        advice.upgradeSuggested = advice.currentTier < advice.recommended->tier;

        if (advice.currentTier >= advice.recommended->tier && advice.currentTier > 0)
        {
            advice.notes.push_back("Your selected model is already a good match: no bigger download is needed.");
        }
        else if (advice.recommendedDownloaded)
        {
            advice.notes.push_back("It is already downloaded: select it and press \"Use this model\".");
        }
        else
        {
            const ModelStatus* status = nullptr;
            for (size_t i = 0; i < models.size(); ++i)
            {
                if (models[i].model == advice.recommended)
                {
                    status = &models[i];
                }
            }
            std::ostringstream dl;
            if (status && status->state == ModelState::Partial)
            {
                dl << "A partial download exists (" << FormatSize(status->bytesOnDisk) << "): Download resumes it.";
            }
            else
            {
                dl << "Press Download to get it (" << FormatSize(advice.recommended->sizeBytes)
                   << "); downloads resume if interrupted and are checked against the published SHA-256.";
            }
            advice.notes.push_back(dl.str());
        }
        return advice;
    }

    // ----- text -----

    std::string FormatSize(unsigned long long bytes)
    {
        char text[32];
        if (bytes >= 1024ull * kMB)
        {
            sprintf_s(text, "%.1f GB", bytes / (1024.0 * kMB));
        }
        else
        {
            sprintf_s(text, "%.0f MB", bytes / static_cast<double>(kMB));
        }
        return text;
    }

    std::string DescribeSystem(const SystemResources& res)
    {
        std::ostringstream os;
        os << "RAM " << Gb(res.totalRamMB) << " (" << Gb(res.availableRamMB) << " free now), " << res.logicalCores
           << " logical processors, AVX2 " << (res.avx2 ? "yes" : "NO") << ", " << Gb(res.freeDiskMB)
           << " free disk for models";
        return os.str();
    }

    std::string DescribeWorkload(const ModelAdvice& advice)
    {
        if (!advice.hasWorkload)
        {
            return std::string();
        }
        const std::string label = advice.workloadTier.rule ? advice.workloadTier.rule->label : std::string();
        return label.empty() ? advice.workload.summary : label + ": " + advice.workload.summary;
    }

    const char* SpeedLabel(const CatalogModel& model, const SystemResources& res)
    {
        // Generation speed on a CPU follows the parameters used per token, not the total size.
        double work = model.activeParamsB;
        if (res.logicalCores < 8)
        {
            work *= 2.0;
        }
        if (work <= 4.0)
        {
            return "fast";
        }
        if (work <= 9.0)
        {
            return "moderate";
        }
        return "slow";
    }

}  // namespace appassist
