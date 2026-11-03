// ModelAdvisor.h - which GGUF model to use for a task: what this PC can run, what is already downloaded,
// and what the task's workload needs.
//
// The pattern (see docs/model-advisor-pattern.md), in four parts that change independently:
//   1. Catalog      - the models: file, URL, exact size, SHA-256, parameters, capability tier.   (data)
//   2. Task profile - per task: context size, workload tiers with thresholds, preferred models. (data)
//   3. Workload     - measures the task's input as named metrics ("lines", "largest_file_lines").  (code)
//   4. Advisor      - generic: PC resources + catalog + profile + workload -> first model, recommended
//                     model, reasons. ModelDownloader then fetches and verifies it.               (code)
// Parts 1 and 2 live in config/models.json (built in; a models.json next to the program overrides it),
// so a new model or a new task (code conversion, test generation, ...) needs no code change unless its
// workload is measured differently.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "CoreEngine.h"

namespace appassist
{
    struct SystemResources
    {
        unsigned long long totalRamMB = 0;
        unsigned long long availableRamMB = 0;
        unsigned int logicalCores = 0;
        bool avx2 = false;                      // AppAssist's llama.cpp build requires AVX2
        unsigned long long freeDiskMB = 0;      // on the drive of the download folder
        std::wstring downloadDir;
    };

    SystemResources DetectSystemResources(const std::wstring& downloadDir);

    // ----- 1. catalog -----

    struct CatalogModel
    {
        std::string id;                 // short name for the CLI, e.g. "qwen2.5-coder-7b"
        std::string displayName;
        std::wstring fileName;
        std::string url;
        unsigned long long sizeBytes = 0;   // exact, used to tell complete downloads from partial ones
        std::string sha256;             // published hash, checked after downloading
        double totalParamsB = 0;        // billions of parameters
        double activeParamsB = 0;       // per token (lower for mixture-of-experts models)
        int tier = 1;                   // capability: 1 starter, 2 standard, 3 large
        unsigned long long kvCacheMB = 0;   // working memory for an 8K-token context
        std::string license;
        std::string summary;

        // Memory needed to run it comfortably: the weights, the context cache for `contextTokens`, and headroom.
        unsigned long long RequiredRamMB(int contextTokens = 8192) const;
    };

    // ----- 2. task profiles -----

    struct TaskTier
    {
        std::string label;                          // "small", "medium", "large"
        std::map<std::string, double> below;        // each workload metric must be below its limit; empty = no limit
        std::string explain;                        // why this tier wants these models
        std::vector<std::string> prefer;            // model ids, best first
    };

    struct TaskProfile
    {
        std::string id;                 // "log-diagnosis", "code-conversion"
        std::string name;
        std::string workloadNoun;       // what the input is called: "knowledge base", "source to convert"
        int contextTokens = 8192;
        std::string noWorkloadHint;     // advice shown before there is any input
        std::vector<TaskTier> tiers;    // smallest first; the first whose limits all hold applies

        bool UsesModel(const std::string& modelId) const;   // listed in any tier
    };

    // Loads the catalog and task profiles: <exe>\models.json if present, else the built-in copy.
    const std::vector<CatalogModel>& ModelCatalog();
    const std::vector<TaskProfile>& TaskProfiles();
    const CatalogModel* FindCatalogModel(const std::string& id);
    const TaskProfile* FindTaskProfile(const std::string& id);
    // Where the catalog came from (a file path, or "built-in"), and any error reading models.json.
    std::string CatalogSource();

    // Parses a catalog file (the models.json format); used by ModelCatalog() and by tests.
    bool ParseModelCatalog(const std::string& json, std::vector<CatalogModel>& models,
        std::vector<TaskProfile>& tasks, std::string& error);

    // ----- 3. workload -----

    struct Workload
    {
        std::map<std::string, double> metrics;  // compared with TaskTier::below
        std::string summary;                    // plain language, e.g. "2276 lines of code ... in 11 files"
    };

    // Source-code workload, from a knowledge base: lines, symbols, messages, files, languages,
    // largest_file_lines. Serves both log diagnosis and code conversion; a task with other input
    // (audio minutes, document pages) adds its own Assess... function filling the same struct.
    Workload AssessSourceWorkload(const KnowledgeBase& kb);

    struct WorkloadTier
    {
        int tier = 0;                   // 1-based index into TaskProfile::tiers; 0 = no workload
        const TaskTier* rule = nullptr;
    };

    WorkloadTier ClassifyWorkload(const TaskProfile& task, const Workload& workload);

    // ----- 4. advisor -----

    enum class ModelState
    {
        NotDownloaded,
        Partial,
        Downloaded
    };

    enum class MemoryFit
    {
        Comfortable,    // leaves a quarter of the RAM for Windows and other programs
        Tight,          // runs, but close other programs first
        TooLarge
    };

    struct ModelStatus
    {
        const CatalogModel* model = nullptr;
        ModelState state = ModelState::NotDownloaded;
        std::wstring path;              // where it is (or would be) stored
        unsigned long long bytesOnDisk = 0;
        MemoryFit memory = MemoryFit::TooLarge;
        bool fitsDisk = false;
    };

    // Folders searched for models: next to the program, the user's AppAssist folder, and any extra
    // folder (for example the one holding the currently selected model).
    std::vector<std::wstring> ModelSearchDirs(const std::wstring& extraDir);
    // Where new downloads go: the selected model's folder if any, else <exe>\models if writable, else
    // %LOCALAPPDATA%\AppAssist\models.
    std::wstring DefaultDownloadDir(const std::wstring& currentModelPath);

    // The task's models (smallest first), with download state and fit for this PC at the task's context size.
    std::vector<ModelStatus> ScanModels(const std::vector<std::wstring>& searchDirs, const SystemResources& res,
        const TaskProfile& task);

    struct ModelAdvice
    {
        const TaskProfile* task = nullptr;
        const CatalogModel* firstModel = nullptr;     // smallest of the task's models that runs comfortably here
        const CatalogModel* recommended = nullptr;    // best fit for the workload (or the first model)
        const CatalogModel* current = nullptr;        // selected model, if it is a catalog model
        int currentTier = 0;                          // tier of the selected model (estimated for others)
        bool hasWorkload = false;
        Workload workload;
        WorkloadTier workloadTier;
        bool recommendedDownloaded = false;
        bool upgradeSuggested = false;                // a bigger model would help and fits this PC
        std::vector<std::string> notes;               // plain-language reasons, one per line
    };

    // workload may be null (nothing to measure yet): then the first model is recommended.
    ModelAdvice AdviseModel(const SystemResources& res, const std::vector<ModelStatus>& models,
        const TaskProfile& task, const Workload* workload, const std::wstring& currentModelPath);

    // One line for the workload, e.g. "large: 658640 lines of code, ...".
    std::string DescribeWorkload(const ModelAdvice& advice);
    std::string DescribeSystem(const SystemResources& res);
    std::string FormatSize(unsigned long long bytes);
    const char* SpeedLabel(const CatalogModel& model, const SystemResources& res);

}  // namespace appassist
