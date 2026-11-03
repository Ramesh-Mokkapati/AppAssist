# Pattern: choosing, downloading and running local models per task

AppAssist picks a local GGUF model for **log diagnosis**. The same machinery serves any task an
offline model can do, such as **code conversion**, test generation or documentation. Each task can
use a different set of models and its own rules. This document describes the pattern, how to
extend it inside AppAssist, and how to carry it into another application.

## The five parts

```
 ┌──────────────┐   ┌────────────────┐   ┌──────────────┐
 │ 1. Catalog   │   │ 2. Task profile│   │ 3. Workload  │   what the job's input is like
 │ (data)       │   │ (data)         │   │ (code)       │   e.g. lines, largest file
 └──────┬───────┘   └───────┬────────┘   └──────┬───────┘
        │   models.json     │                   │  metrics
        └─────────┬─────────┴───────────────────┘
                  ▼
        ┌───────────────────┐  PC resources (RAM, AVX2, disk)
        │ 4. Advisor        │◄──────────────────────────────
        │ (generic code)    │  models already on disk
        └────────┬──────────┘
                 ▼  first model / recommended model / reasons
        ┌───────────────────┐        ┌──────────────────────┐
        │ 5. Downloader     │───────►│ Engine (llama.cpp or │
        │ resume + SHA-256  │  .gguf │ a local HTTP server) │
        └───────────────────┘        └──────────────────────┘
```

| Part | Where | Changes when |
| --- | --- | --- |
| 1. **Catalog**: id, file, URL, exact size, SHA-256, total and active parameters, capability tier (1–3), context-cache size, licence | `config/models.json` → `models` | a model is added or replaced |
| 2. **Task profile**: context size, workload tiers with thresholds, preferred models for each tier, wording | `config/models.json` → `tasks` | a task is added or its rules are tuned |
| 3. **Workload**: measures the task's input as named metrics plus a one-line summary | `AssessSourceWorkload()` in `ModelAdvisor.cpp` | a task's input is not source code (audio, documents, ...) |
| 4. **Advisor**: resources + catalog + profile + workload → first model, recommended model, reasons | `AdviseModel()`, `ScanModels()`, `ClassifyWorkload()` | never, for a new task |
| 5. **Downloader**: WinHTTP range resume, retries, free-disk check, SHA-256 (Windows CNG), `.part` → final name | `ModelDownloader` | never |

The UI and CLI only pass a task id: `CModelAdvisorDlg("code-conversion", kb, modelPath)` or
`AppAssistCli models --task code-conversion --source <folder>`.

## The rules the advisor applies (same for every task)

1. **Eligible models.** A task uses only the models named in its tiers' `prefer` lists.
2. **RAM needed** = file size × 1.15 + context cache × (task `context_tokens` / 8192) + 1 GB.
   - **Comfortable:** at most 75 % of the RAM.
   - **Tight:** at most 90 %.
   - **Too large:** anything more.

   Code conversion uses a 16K-token context, so the same model needs more RAM for it than for log
   diagnosis.
3. **First model:** the smallest eligible model that runs comfortably on this PC (or tightly if
   none does). It is recommended until there is a workload to measure.
4. **Workload tier:** the first tier whose `below` limits all hold. A metric the workload does not
   report is ignored. The last tier catches everything else.
5. **Recommended model:** the first model in that tier's `prefer` list that fits comfortably and
   fits on disk; failing that, the first that fits tightly. When it is not the tier's first
   choice, the advice says why: not enough RAM or disk.
6. **Upgrade:** suggested when the selected model's capability tier is below the recommended
   model's. For a file outside the catalog, the tier is estimated from its size.

## Extending it

### Add a model (no code change)

Add an entry to `models` in `config/models.json` (rebuild to update the built-in copy; CMake
splits it into 8000-character literals, so the file may grow to 64 KB). To try it
without rebuilding, edit the `models.json` next to the exe instead:

```json
{
  "id": "my-model-7b",
  "name": "My Model 7B Instruct (Q4_K_M)",
  "file": "my-model-7b-instruct-q4_k_m.gguf",
  "url": "https://huggingface.co/<org>/<repo>/resolve/main/my-model-7b-instruct-q4_k_m.gguf",
  "size": 4683073536,
  "sha256": "<64 lowercase hex digits>",
  "params_b": 7.6, "active_params_b": 7.6, "tier": 2, "kv_cache_mb_8k": 500,
  "license": "Apache-2.0",
  "summary": "One sentence the user sees."
}
```

- **`size` and `sha256`:** take them from the file's page on Hugging Face (*Files* → the file →
  the "SHA256" line). They must be exact: they are how the advisor tells a complete download from
  a partial one, and how the downloader proves the file is intact. The API lists both for every
  file in a repository: `https://huggingface.co/api/models/<org>/<repo>/tree/main` gives
  `lfs.size` and `lfs.oid` (the SHA-256). Use a repository that is not gated (its API page shows
  `"gated": false`), or the download needs a Hugging Face sign-in that AppAssist does not do.
- **One file:** the downloader fetches a single file, so pick a repository that publishes the
  quantization as one `.gguf`, not split into `-00001-of-0000N` parts.
- **`active_params_b`:** for a mixture-of-experts model, the parameters used per token. The speed
  estimate uses this number.
- **`kv_cache_mb_8k`:** the memory of an 8K-token context. llama.cpp logs the KV cache size when
  it loads a model with an 8192-token context.
- **Then:** add the model's id to the `prefer` lists of the tasks that should use it. A model in
  no task's list is never offered.

A file that fails validation means the whole `models.json` is ignored, and the built-in catalog
is used with the reason shown. Validation requires an https URL, a size, a 64-digit SHA-256, a
tier from 1 to 3, and known model ids in `prefer`.

### Add a task whose input is source code (no code change)

Code conversion is the worked example. Its profile:

```json
{
  "id": "code-conversion",
  "name": "Code conversion (e.g. C++ to C#, Python 2 to 3)",
  "workload": "source to convert",
  "context_tokens": 16384,
  "no_workload_hint": "Point the advisor at the source to convert ...",
  "tiers": [
    { "label": "small",  "below": { "largest_file_lines": 300, "lines": 5000 },
      "explain": "...", "prefer": [ "qwen2.5-coder-7b", "qwen3-coder-30b-a3b" ] },
    { "label": "medium", "below": { "largest_file_lines": 1200 },
      "explain": "...", "prefer": [ "qwen3-coder-30b-a3b", "qwen2.5-coder-14b", "qwen2.5-coder-7b" ] },
    { "label": "large",
      "explain": "...", "prefer": [ "qwen3-coder-30b-a3b", "qwen2.5-coder-14b", "qwen2.5-coder-7b" ] }
  ]
}
```

It differs from log diagnosis in exactly the places where the tasks differ:

| | Log diagnosis | Code conversion |
| --- | --- | --- |
| Size that matters | total code and symbols (the model reasons across the code base) | the **largest file** (the file and its translation must fit in the context) |
| Context | 8K tokens | 16K tokens, so more RAM per model |
| Smallest model | 1.5B is fine for small code bases | 7B at least: small models drop details when rewriting |
| Same VaniScribe code (667-line largest file) | small → 1.5B | medium → 30B-A3B |

The source workload already reports these metrics:

| Metric | Meaning |
| --- | --- |
| `files` | number of source files |
| `lines` | total lines of code |
| `symbols` | functions, classes and structs found |
| `messages` | log, error and exception messages found |
| `languages` | number of languages |
| `largest_file_lines` | lines in the largest file |

A new source-based task, such as test generation, documentation or refactoring, needs only a new
profile. In testing, a `test-generation` task added to the `models.json` next to the exe was
picked up without a rebuild.

### Add a task with a different input (one function)

Write one function that fills the same struct, and pass its result to `AdviseModel`:

```cpp
// e.g. for a transcription task: audio length decides the model
appassist::Workload AssessAudioWorkload(double minutes, int speakers)
{
    appassist::Workload w;
    w.metrics["minutes"] = minutes;
    w.metrics["speakers"] = speakers;
    w.summary = std::to_string(static_cast<int>(minutes)) + " minutes of audio";
    return w;
}
```

The profile then uses those names, for example `"below": { "minutes": 30 }`. Nothing else
changes.

### Run the task

The advisor answers only "which model". Each task still needs its own prompt and output handling
on top of `ILanguageModel` (`EmbeddedLlamaEngine` or `LocalLLMClient`):
- log diagnosis uses `DiagnosisRunner`;
- a code converter would send one file (or one piece of a long file) per request, with the
  target language and conventions in the system prompt, and set `LlamaSettings::contextSize` to
  the profile's `context_tokens`.

## Using the pattern in another application

Copy these files:
- `ModelAdvisor.h/.cpp`
- `ModelDownloader.h/.cpp`
- `ModelCatalogDefault.h.in`, plus the CMake lines that generate it
- `config/models.json`

Two dependencies need small changes:
- **`CoreEngine.h`:** the advisor uses its file helpers and `KnowledgeBase`. Copy the helpers, and
  replace `AssessSourceWorkload` if your input is not source code.
- **The app name:** in the `.appassist-write-test` probe and in `%LOCALAPPDATA%\AppAssist\models`.

Then write your task profiles, keep the UI thin (task id in, advice out, download on a worker
thread), and keep these rules:

- Ship only models whose licence allows your use; record it in the catalog and show it.
- Pin every model by exact size and SHA-256. Never load a file that failed verification.
- Size the model to the PC first, then to the work: recommend the best model that *fits*, and say
  why when it is not the ideal one.
- Start small: offer the smallest adequate model first, and suggest an upgrade only when the
  workload calls for it.
- Keep catalog and rules as data, so tuning them needs no rebuild.

### A prompt to set this up in a new project

> In this C++ application, add a local-model advisor for the task "<task>" whose input is
> "<input>". Use a JSON catalog of GGUF models (id, file, https URL, exact size, SHA-256, total and
> active parameters, capability tier, licence) and a task profile (context tokens; tiers whose
> thresholds are on named workload metrics, each listing preferred model ids best-first). Measure
> the input as named metrics. Recommend the smallest model that fits the PC's RAM (≤ 75 %) at
> first, then the first preferred model of the workload's tier that fits RAM and disk, and explain
> the choice. Download with HTTP range resume and retries, check free disk, verify the SHA-256
> before use, and keep partial files resumable. Keep UI work off the UI thread.
