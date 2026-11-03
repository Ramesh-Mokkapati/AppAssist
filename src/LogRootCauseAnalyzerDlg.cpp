// LogRootCauseAnalyzerDlg.cpp - see LogRootCauseAnalyzerDlg.h.
#include "LogRootCauseAnalyzerDlg.h"

#include <algorithm>
#include <chrono>

#include "ModelAdvisor.h"
#include "ModelAdvisorDlg.h"

using namespace appassist;

namespace
{

    const TCHAR kSection[] = _T("Settings");

    // UTF-8 <-> CString (Unicode build) with the MFC/ATL conversion helpers.
    CString FromUtf8(const std::string& text)
    {
        return CString(CA2T(text.c_str(), CP_UTF8));
    }

    std::string ToUtf8(const CString& text)
    {
        return std::string(CT2A(text, CP_UTF8));
    }

    // Edit controls need CRLF line breaks.
    CString WithCrLf(const CString& text)
    {
        CString out(text);
        out.Replace(_T("\r\n"), _T("\n"));
        out.Replace(_T("\n"), _T("\r\n"));
        return out;
    }

    std::wstring ParentDir(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    // AppAssist's task profile in models.json.
    const char kTaskId[] = "log-diagnosis";

    // The advisor's verdict for a knowledge base (or none) and the selected model.
    ModelAdvice CurrentAdvice(const KnowledgeBase* kb, const std::wstring& modelPath)
    {
        const TaskProfile* task = FindTaskProfile(kTaskId);
        if (!task)
        {
            return ModelAdvice();
        }

        SystemResources res = DetectSystemResources(DefaultDownloadDir(modelPath));
        std::vector<ModelStatus> models = ScanModels(ModelSearchDirs(ParentDir(modelPath)), res, *task);
        Workload workload;
        if (kb)
        {
            workload = AssessSourceWorkload(*kb);
        }

        return AdviseModel(res, models, *task, kb ? &workload : nullptr, modelPath);
    }

}  // namespace

BEGIN_MESSAGE_MAP(CLogRootCauseAnalyzerDlg, CDialogEx)
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_BN_CLICKED(IDC_BROWSE_SOURCE, &CLogRootCauseAnalyzerDlg::OnBrowseSource)
    ON_BN_CLICKED(IDC_BROWSE_KB, &CLogRootCauseAnalyzerDlg::OnBrowseKnowledgeBase)
    ON_BN_CLICKED(IDC_BROWSE_LOG, &CLogRootCauseAnalyzerDlg::OnBrowseLog)
    ON_BN_CLICKED(IDC_BROWSE_MODEL, &CLogRootCauseAnalyzerDlg::OnBrowseModel)
    ON_BN_CLICKED(IDC_MODELS, &CLogRootCauseAnalyzerDlg::OnModels)
    ON_BN_CLICKED(IDC_BUILD_KB, &CLogRootCauseAnalyzerDlg::OnBuildKnowledgeBase)
    ON_BN_CLICKED(IDC_ANALYZE, &CLogRootCauseAnalyzerDlg::OnAnalyze)
    ON_BN_CLICKED(IDC_CANCEL_TASK, &CLogRootCauseAnalyzerDlg::OnCancelTask)
    ON_BN_CLICKED(IDC_COPY_REPORT, &CLogRootCauseAnalyzerDlg::OnCopyReport)
    ON_BN_CLICKED(IDC_SAVE_REPORT, &CLogRootCauseAnalyzerDlg::OnSaveReport)
    ON_BN_CLICKED(IDC_ENGINE_EMBEDDED, &CLogRootCauseAnalyzerDlg::OnEngineChanged)
    ON_BN_CLICKED(IDC_ENGINE_HTTP, &CLogRootCauseAnalyzerDlg::OnEngineChanged)
    ON_BN_CLICKED(IDC_ENGINE_NONE, &CLogRootCauseAnalyzerDlg::OnEngineChanged)
    ON_MESSAGE(WM_APP_ASSIST, &CLogRootCauseAnalyzerDlg::OnAppAssistMessage)
END_MESSAGE_MAP()

CLogRootCauseAnalyzerDlg::CLogRootCauseAnalyzerDlg(CWnd* parent)
    : CDialogEx(IDD_APPASSIST_DIALOG, parent)
    , m_busy(false)
    , m_cancel(false)
    , m_http(nullptr)
{
}

CLogRootCauseAnalyzerDlg::~CLogRootCauseAnalyzerDlg()
{
    m_cancel = true;
    if (m_worker.joinable())
    {
        m_worker.join();
    }

    EmbeddedLlamaEngine::SetLogSink(ProgressCallback());  // the sink points at this dialog
}

void CLogRootCauseAnalyzerDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_OUTPUT, m_output);
}

BOOL CLogRootCauseAnalyzerDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    HICON icon = AfxGetApp()->LoadStandardIcon(IDI_APPLICATION);
    SetIcon(icon, TRUE);
    SetIcon(icon, FALSE);

    m_outputFont.CreatePointFont(95, _T("Consolas"));
    m_output.SetFont(&m_outputFont);
    m_output.SetLimitText(0);  // reports can be long

    // Record the initial layout so OnSize can keep controls anchored to the edges.
    GetClientRect(&m_initialClient);
    CRect window;
    GetWindowRect(&window);
    m_minTrackSize = CSize(window.Width(), window.Height() * 3 / 4);
    struct LayoutEntry
    {
        UINT id;
        bool moveX, sizeX, moveY, sizeY;
    };
    const LayoutEntry layout[] =
    {
        { IDC_GROUP_KB, false, true, false, false },         { IDC_SOURCE_DIR, false, true, false, false },
        { IDC_BROWSE_SOURCE, true, false, false, false },    { IDC_KB_PATH, false, true, false, false },
        { IDC_BROWSE_KB, true, false, false, false },        { IDC_KB_INFO, false, true, false, false },
        { IDC_GROUP_LOG, false, true, false, false },        { IDC_LOG_PATH, false, true, false, false },
        { IDC_BROWSE_LOG, true, false, false, false },       { IDC_MODEL_PATH, false, true, false, false },
        { IDC_BROWSE_MODEL, true, false, false, false },     { IDC_MODELS, true, false, false, false },
        { IDC_HTTP_URL, false, true, false, false },
        { IDC_HTTP_MODEL_LABEL, true, false, false, false }, { IDC_HTTP_MODEL, true, false, false, false },
        { IDC_COPY_REPORT, true, false, false, false },      { IDC_SAVE_REPORT, true, false, false, false },
        { IDC_STATUS, false, true, false, false },           { IDC_OUTPUT, false, true, false, true },
    };

    for (size_t i = 0; i < sizeof(layout) / sizeof(layout[0]); ++i)
    {
        CWnd* ctrl = GetDlgItem(layout[i].id);
        if (!ctrl)
        {
            continue;
        }

        Anchor a;
        a.id = layout[i].id;
        a.moveX = layout[i].moveX; a.sizeX = layout[i].sizeX;
        a.moveY = layout[i].moveY; a.sizeY = layout[i].sizeY;
        ctrl->GetWindowRect(&a.rect);
        ScreenToClient(&a.rect);
        m_anchors.push_back(a);
    }

    LoadSettings();
    UpdateEngineControls();
    EmbeddedLlamaEngine::SetLogSink([this](const std::string& msg) { Post(kStatus, msg); });

    AppendOutput(_T("1. Build a knowledge base from your application's source folder (or open an existing .kdb).\r\n")
        _T("2. Pick a log file and an AI engine, then press Analyze log.\r\n\r\n")
        _T("Errors in the log are first matched to the source code without AI (log messages, stack traces, ")
        _T("function names). The AI then explains that evidence and suggests workarounds, citing file:line. ")
        _T("Treat its suggestions as unverified until you have checked them.\r\n"));
    ShowModelHint();

    return TRUE;
}

// At startup without a usable model: point at the Model Advisor's first (smallest) model.
void CLogRootCauseAnalyzerDlg::ShowModelHint()
{
    const std::wstring modelPath(GetText(IDC_MODEL_PATH));
    if (SelectedEngine() != 0 || (!modelPath.empty() && FileExists(modelPath)))
    {
        return;
    }

    ModelAdvice advice = CurrentAdvice(nullptr, modelPath);
    if (!advice.firstModel)
    {
        return;
    }

    std::string hint = "\nNo GGUF model is selected yet. ";
    if (advice.recommendedDownloaded)
    {
        hint += std::string(advice.firstModel->displayName) + " is already on this PC: click Models... and press "
                "\"Use this model\".\n";
    }
    else
    {
        hint += "Click Models... to download " + std::string(advice.firstModel->displayName) + " (" +
                FormatSize(advice.firstModel->sizeBytes) + "), the smallest model that runs well on this PC. "
                "Once you have a knowledge base, the advisor tells you whether a larger model is worth it.\n";
    }

    AppendOutput(FromUtf8(hint));
}

// ----- layout -----

void CLogRootCauseAnalyzerDlg::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    if (nType == SIZE_MINIMIZED || m_anchors.empty())
    {
        return;
    }

    const int dx = cx - m_initialClient.Width();
    const int dy = cy - m_initialClient.Height();
    HDWP defer = BeginDeferWindowPos(static_cast<int>(m_anchors.size()));

    for (size_t i = 0; i < m_anchors.size(); ++i)
    {
        const Anchor& a = m_anchors[i];
        CWnd* ctrl = GetDlgItem(a.id);
        if (!ctrl || !defer)
        {
            continue;
        }

        CRect r = a.rect;
        if (a.moveX)
        {
            r.OffsetRect(dx, 0);
        }

        if (a.sizeX)
        {
            r.right += dx;
        }

        if (a.moveY)
        {
            r.OffsetRect(0, dy);
        }

        if (a.sizeY)
        {
            r.bottom += dy;
        }

        defer = DeferWindowPos(defer, ctrl->GetSafeHwnd(), nullptr, r.left, r.top, r.Width(), r.Height(),
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (defer)
    {
        EndDeferWindowPos(defer);
    }

    Invalidate(FALSE);
}

void CLogRootCauseAnalyzerDlg::OnGetMinMaxInfo(MINMAXINFO* info)
{
    if (m_minTrackSize.cx > 0)
    {
        info->ptMinTrackSize.x = m_minTrackSize.cx;
        info->ptMinTrackSize.y = m_minTrackSize.cy;
    }

    CDialogEx::OnGetMinMaxInfo(info);
}

// ----- settings -----

void CLogRootCauseAnalyzerDlg::LoadSettings()
{
    CWinApp* app = AfxGetApp();
    SetDlgItemText(IDC_SOURCE_DIR, app->GetProfileString(kSection, _T("SourceDir")));
    SetDlgItemText(IDC_KB_PATH, app->GetProfileString(kSection, _T("KnowledgeBase")));
    SetDlgItemText(IDC_LOG_PATH, app->GetProfileString(kSection, _T("LogFile")));
    SetDlgItemText(IDC_MODEL_PATH, app->GetProfileString(kSection, _T("ModelPath")));
    SetDlgItemText(IDC_HTTP_URL, app->GetProfileString(kSection, _T("HttpUrl"),
        _T("http://localhost:1234/v1/chat/completions")));
    SetDlgItemText(IDC_HTTP_MODEL, app->GetProfileString(kSection, _T("HttpModel")));
    int engine = app->GetProfileInt(kSection, _T("Engine"), 0);
    CheckRadioButton(IDC_ENGINE_EMBEDDED, IDC_ENGINE_NONE,
        engine == 1 ? IDC_ENGINE_HTTP : engine == 2 ? IDC_ENGINE_NONE : IDC_ENGINE_EMBEDDED);
}

void CLogRootCauseAnalyzerDlg::SaveSettings()
{
    CWinApp* app = AfxGetApp();
    app->WriteProfileString(kSection, _T("SourceDir"), GetText(IDC_SOURCE_DIR));
    app->WriteProfileString(kSection, _T("KnowledgeBase"), GetText(IDC_KB_PATH));
    app->WriteProfileString(kSection, _T("LogFile"), GetText(IDC_LOG_PATH));
    app->WriteProfileString(kSection, _T("ModelPath"), GetText(IDC_MODEL_PATH));
    app->WriteProfileString(kSection, _T("HttpUrl"), GetText(IDC_HTTP_URL));
    app->WriteProfileString(kSection, _T("HttpModel"), GetText(IDC_HTTP_MODEL));
    app->WriteProfileInt(kSection, _T("Engine"), SelectedEngine());
}

CString CLogRootCauseAnalyzerDlg::GetText(UINT id) const
{
    CString text;
    GetDlgItemText(id, text);
    text.Trim();
    return text;
}

int CLogRootCauseAnalyzerDlg::SelectedEngine() const
{
    if (IsDlgButtonChecked(IDC_ENGINE_HTTP))
    {
        return 1;
    }

    if (IsDlgButtonChecked(IDC_ENGINE_NONE))
    {
        return 2;
    }

    return 0;
}

void CLogRootCauseAnalyzerDlg::OnEngineChanged()
{
    UpdateEngineControls();
}

void CLogRootCauseAnalyzerDlg::UpdateEngineControls()
{
    int engine = SelectedEngine();
    bool idle = !m_busy;
    GetDlgItem(IDC_MODEL_PATH)->EnableWindow(idle && engine == 0);
    GetDlgItem(IDC_BROWSE_MODEL)->EnableWindow(idle && engine == 0);
    GetDlgItem(IDC_HTTP_URL)->EnableWindow(idle && engine == 1);
    GetDlgItem(IDC_HTTP_MODEL)->EnableWindow(idle && engine == 1);
}

// ----- pickers -----

void CLogRootCauseAnalyzerDlg::OnBrowseSource()
{
    CFolderPickerDialog dlg(GetText(IDC_SOURCE_DIR), 0, this);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    CString folder = dlg.GetPathName();
    SetDlgItemText(IDC_SOURCE_DIR, folder);
    if (GetText(IDC_KB_PATH).IsEmpty())
    {
        // suggest a .kdb next to the sources
        CString name = folder.Mid(folder.ReverseFind(_T('\\')) + 1);
        SetDlgItemText(IDC_KB_PATH, folder + _T("\\") + (name.IsEmpty() ? CString(_T("source")) : name) + _T(".kdb"));
    }
}

void CLogRootCauseAnalyzerDlg::OnBrowseKnowledgeBase()
{
    // Not OFN_FILEMUSTEXIST: you may pick a new file name to build into.
    CFileDialog dlg(TRUE, _T("kdb"), GetText(IDC_KB_PATH), OFN_HIDEREADONLY | OFN_PATHMUSTEXIST,
        _T("AppAssist knowledge base (*.kdb)|*.kdb|All files (*.*)|*.*||"), this);
    if (dlg.DoModal() == IDOK)
    {
        SetDlgItemText(IDC_KB_PATH, dlg.GetPathName());
    }
}

void CLogRootCauseAnalyzerDlg::OnBrowseLog()
{
    CFileDialog dlg(TRUE, nullptr, GetText(IDC_LOG_PATH), OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
        _T("Log files (*.log;*.txt;*.out;*.err)|*.log;*.txt;*.out;*.err|All files (*.*)|*.*||"), this);
    if (dlg.DoModal() == IDOK)
    {
        SetDlgItemText(IDC_LOG_PATH, dlg.GetPathName());
    }
}

void CLogRootCauseAnalyzerDlg::OnBrowseModel()
{
    CFileDialog dlg(TRUE, _T("gguf"), GetText(IDC_MODEL_PATH), OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
        _T("GGUF models (*.gguf)|*.gguf|All files (*.*)|*.*||"), this);
    if (dlg.DoModal() == IDOK)
    {
        SetDlgItemText(IDC_MODEL_PATH, dlg.GetPathName());
    }
}

void CLogRootCauseAnalyzerDlg::OnModels()
{
    if (m_busy)
    {
        return;
    }

    // The advice depends on the knowledge base: use the loaded one, or load the one named in the box.
    const std::wstring kbPath(GetText(IDC_KB_PATH));
    if ((!m_kb || _wcsicmp(m_kbPath.c_str(), kbPath.c_str()) != 0) && !kbPath.empty() && FileExists(kbPath))
    {
        CWaitCursor wait;
        std::shared_ptr<KnowledgeBase> kb(new KnowledgeBase());
        std::string error;
        if (kb->Load(kbPath, error))
        {
            m_kb = kb;
            m_kbPath = kbPath;
            CString info;
            info.Format(_T("Loaded: %zu files, %zu symbols, %zu messages, %zu settings (built %s)"), m_kb->files.size(),
                m_kb->SymbolCount(), m_kb->MessageCount(), m_kb->SettingCount(), FromUtf8(m_kb->created).GetString());
            SetDlgItemText(IDC_KB_INFO, info);
        }
    }

    const bool kbMatches = m_kb && _wcsicmp(m_kbPath.c_str(), kbPath.c_str()) == 0;
    CModelAdvisorDlg dlg(kTaskId, kbMatches ? m_kb : std::shared_ptr<KnowledgeBase>(),
        std::wstring(GetText(IDC_MODEL_PATH)), this);
    if (dlg.DoModal() == IDOK && !dlg.ChosenModelPath().empty())
    {
        SetDlgItemText(IDC_MODEL_PATH, dlg.ChosenModelPath().c_str());
        CheckRadioButton(IDC_ENGINE_EMBEDDED, IDC_ENGINE_NONE, IDC_ENGINE_EMBEDDED);
        UpdateEngineControls();
        SaveSettings();
        SetDlgItemText(IDC_STATUS, _T("Model selected: ") + CString(dlg.ChosenModelPath().c_str()));
    }
}

// ----- tasks -----

bool CLogRootCauseAnalyzerDlg::StartTask(const CString& status, std::function<void()> task)
{
    if (m_busy)
    {
        return false;
    }

    FinishWorker();
    SaveSettings();
    m_cancel = false;
    SetBusy(true);
    SetDlgItemText(IDC_STATUS, status);
    m_worker = std::thread([this, task]()
    {
        try
        {
            task();
        }
        catch (const std::exception& e)
        {
            Post(kTaskFinished, std::string("Unexpected error: ") + e.what(), false);
        }
    });

    return true;
}

void CLogRootCauseAnalyzerDlg::FinishWorker()
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

void CLogRootCauseAnalyzerDlg::SetBusy(bool busy)
{
    m_busy = busy;
    const UINT idleOnly[] = { IDC_BUILD_KB, IDC_ANALYZE, IDC_SOURCE_DIR, IDC_BROWSE_SOURCE, IDC_KB_PATH,
                             IDC_BROWSE_KB, IDC_LOG_PATH, IDC_BROWSE_LOG, IDC_ENGINE_EMBEDDED,
                             IDC_ENGINE_HTTP, IDC_ENGINE_NONE, IDC_SAVE_REPORT, IDC_MODELS };

    for (size_t i = 0; i < sizeof(idleOnly) / sizeof(idleOnly[0]); ++i)
    {
        GetDlgItem(idleOnly[i])->EnableWindow(!busy);
    }

    GetDlgItem(IDC_CANCEL_TASK)->EnableWindow(busy);
    UpdateEngineControls();
}

void CLogRootCauseAnalyzerDlg::OnBuildKnowledgeBase()
{
    CString source = GetText(IDC_SOURCE_DIR), kb = GetText(IDC_KB_PATH);
    if (source.IsEmpty() || kb.IsEmpty())
    {
        AfxMessageBox(_T("Choose the source folder and where to save the knowledge base (.kdb)."), MB_ICONINFORMATION);
        return;
    }

    m_output.SetWindowText(_T(""));
    std::wstring sourceDir(source), kbPath(kb), modelPath(GetText(IDC_MODEL_PATH));
    StartTask(_T("Building the knowledge base..."), [this, sourceDir, kbPath, modelPath]()
    {
        BuildKnowledgeBaseTask(sourceDir, kbPath, modelPath);
    });
}

void CLogRootCauseAnalyzerDlg::OnAnalyze()
{
    CString kb = GetText(IDC_KB_PATH), log = GetText(IDC_LOG_PATH);
    int engine = SelectedEngine();
    if (kb.IsEmpty() || log.IsEmpty())
    {
        AfxMessageBox(_T("Choose a knowledge base (.kdb) and a log file first."), MB_ICONINFORMATION);
        return;
    }

    if (engine == 0 && GetText(IDC_MODEL_PATH).IsEmpty())
    {
        AfxMessageBox(_T("Choose a .gguf model file, or select \"Local server\" or \"No AI\"."), MB_ICONINFORMATION);
        return;
    }

    m_output.SetWindowText(_T(""));
    std::wstring kbPath(kb), logPath(log), modelPath(GetText(IDC_MODEL_PATH)), httpUrl(GetText(IDC_HTTP_URL));
    std::string httpModel = ToUtf8(GetText(IDC_HTTP_MODEL));
    StartTask(_T("Analyzing the log..."), [=]()
    {
        AnalyzeTask(kbPath, logPath, engine, modelPath, httpUrl, httpModel);
    });
}

void CLogRootCauseAnalyzerDlg::OnOK()
{
    OnAnalyze();
}

void CLogRootCauseAnalyzerDlg::OnCancelTask()
{
    if (!m_busy)
    {
        return;
    }

    m_cancel = true;
    {
        std::lock_guard<std::mutex> lock(m_httpMutex);
        if (m_http)
        {
            m_http->Cancel();
        }
    }

    SetDlgItemText(IDC_STATUS, _T("Cancelling..."));
    GetDlgItem(IDC_CANCEL_TASK)->EnableWindow(FALSE);
}

void CLogRootCauseAnalyzerDlg::OnCancel()
{
    if (m_busy)
    {
        if (AfxMessageBox(_T("A task is still running. Stop it and close AppAssist?"), MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            return;
        }

        OnCancelTask();
        CWaitCursor wait;
        FinishWorker();  // the worker stops at its next cancellation check
    }

    SaveSettings();
    EmbeddedLlamaEngine::SetLogSink(ProgressCallback());  // no more messages to this window
    CDialogEx::OnCancel();
}

// ----- worker -> UI messages -----

void CLogRootCauseAnalyzerDlg::Post(MessageKind kind, const std::string& utf8Text, bool ok)
{
    std::unique_ptr<UiMessage> msg(new UiMessage());
    msg->kind = kind;
    msg->text = FromUtf8(utf8Text);
    msg->ok = ok;
    if (::IsWindow(GetSafeHwnd()) && PostMessage(WM_APP_ASSIST, 0, reinterpret_cast<LPARAM>(msg.get())))
    {
        msg.release();  // the UI thread owns it now
    }
}

void CLogRootCauseAnalyzerDlg::PostKnowledgeBase(const std::shared_ptr<KnowledgeBase>& kb, const std::wstring& path)
{
    std::unique_ptr<UiMessage> msg(new UiMessage());
    msg->kind = kKnowledgeBaseReady;
    msg->ok = true;
    msg->kb = kb;
    msg->kbPath = path;
    if (::IsWindow(GetSafeHwnd()) && PostMessage(WM_APP_ASSIST, 0, reinterpret_cast<LPARAM>(msg.get())))
    {
        msg.release();
    }
}

LRESULT CLogRootCauseAnalyzerDlg::OnAppAssistMessage(WPARAM /*wParam*/, LPARAM lParam)
{
    std::unique_ptr<UiMessage> msg(reinterpret_cast<UiMessage*>(lParam));
    switch (msg->kind)
    {
    case kStatus:
        SetDlgItemText(IDC_STATUS, msg->text);
        break;
    case kAppend:
        AppendOutput(msg->text);
        break;
    case kKnowledgeBaseReady:
    {
        m_kb = msg->kb;
        m_kbPath = msg->kbPath;
        CString info;
        info.Format(_T("Loaded: %zu files, %zu symbols, %zu messages, %zu settings (built %s)"), m_kb->files.size(),
            m_kb->SymbolCount(), m_kb->MessageCount(), m_kb->SettingCount(), FromUtf8(m_kb->created).GetString());
        SetDlgItemText(IDC_KB_INFO, info);
        break;
    }
    case kTaskFinished:
        FinishWorker();
        SetBusy(false);
        SetDlgItemText(IDC_STATUS, msg->text);
        if (!msg->ok)
        {
            AppendOutput(_T("\r\n") + msg->text + _T("\r\n"));
        }
        break;
    }
    return 0;
}

void CLogRootCauseAnalyzerDlg::AppendOutput(const CString& text)
{
    int end = m_output.GetWindowTextLength();
    m_output.SetSel(end, end);
    m_output.ReplaceSel(WithCrLf(text));
}

// ----- worker tasks (background thread) -----

void CLogRootCauseAnalyzerDlg::BuildKnowledgeBaseTask(std::wstring sourceDir, std::wstring kbPath,
    std::wstring modelPath)
{
    auto progress = [this](const std::string& message) { Post(kStatus, message); };
    SourceCodeParser parser(progress, &m_cancel);
    std::shared_ptr<KnowledgeBase> kb(new KnowledgeBase());
    std::string error;
    auto started = std::chrono::steady_clock::now();
    if (!parser.BuildKnowledgeBase(sourceDir, *kb, error) || !kb->Save(kbPath, error))
    {
        Post(kTaskFinished, error, false);
        return;
    }

    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    PostKnowledgeBase(kb, kbPath);

    // Summary: the files with the most symbols, so you can see what was indexed.
    std::vector<const SourceFileInfo*> files;
    for (size_t i = 0; i < kb->files.size(); ++i)
    {
        files.push_back(&kb->files[i]);
    }

    std::sort(files.begin(), files.end(), [](const SourceFileInfo* a, const SourceFileInfo* b)
    {
        return a->symbols.size() > b->symbols.size();
    });

    char line[512];
    sprintf_s(line, "Knowledge base saved: %zu files, %zu symbols, %zu log/error messages, %zu calls, %zu settings "
                    "(%zu config files) in %.1f s\n",
        kb->files.size(), kb->SymbolCount(), kb->MessageCount(), kb->CallCount(), kb->SettingCount(),
        kb->configFiles.size(), seconds);
    std::string report = std::string(line) + WideToUtf8(kbPath) + "\n\nLargest files:\n";
    for (size_t i = 0; i < files.size() && i < 15; ++i)
    {
        sprintf_s(line, "  %-60s %4zu symbols %4zu messages\n", files[i]->path.c_str(), files[i]->symbols.size(),
            files[i]->messages.size());
        report += line;
    }

    // Does this code base call for a bigger model than the selected one?
    ModelAdvice advice = CurrentAdvice(kb.get(), modelPath);
    report += "\nModel advice: " + DescribeWorkload(advice) + ".\n";
    if (advice.upgradeSuggested && advice.recommended)
    {
        report += std::string(advice.recommended->displayName) + " (" + FormatSize(advice.recommended->sizeBytes) +
                  ") suits it better than the selected model" +
                  (advice.recommendedDownloaded ? std::string(" and is already downloaded") : std::string()) +
                  ": click Models... to " + (advice.recommendedDownloaded ? "select" : "download") + " it.\n";
    }
    else if (advice.recommended)
    {
        report += std::string("Recommended model: ") + advice.recommended->displayName +
                  (advice.current == advice.recommended || (!advice.upgradeSuggested && advice.currentTier > 0)
                      ? " - your selected model is a good match.\n"
                      : " - click Models... to get it.\n");
    }

    Post(kAppend, report);
    Post(kTaskFinished, "Knowledge base ready.", true);
}

void CLogRootCauseAnalyzerDlg::AnalyzeTask(std::wstring kbPath, std::wstring logPath, int engine,
    std::wstring modelPath, std::wstring httpUrl, std::string httpModel)
{
    std::string error;

    // Knowledge base: reuse the loaded one unless the path changed.
    std::shared_ptr<KnowledgeBase> kb = m_kb;
    if (!kb || _wcsicmp(m_kbPath.c_str(), kbPath.c_str()) != 0)
    {
        Post(kStatus, "Loading the knowledge base...");
        kb.reset(new KnowledgeBase());
        if (!kb->Load(kbPath, error))
        {
            Post(kTaskFinished, error, false);
            return;
        }

        PostKnowledgeBase(kb, kbPath);
    }

    // Language model.
    ILanguageModel* model = nullptr;
    std::unique_ptr<LocalLLMClient> http;
    if (engine == 0)
    {
        if (!m_engine)
        {
            m_engine.reset(new EmbeddedLlamaEngine());
        }

        if (!m_engine->IsLoaded() || _wcsicmp(m_engine->Settings().modelPath.c_str(), modelPath.c_str()) != 0)
        {
            Post(kStatus, "Loading the model (this can take a minute the first time)...");
            LlamaSettings settings;
            settings.modelPath = modelPath;
            if (!m_engine->Load(settings, error))
            {
                Post(kTaskFinished, error, false);
                return;
            }
        }

        model = m_engine.get();
    }
    else if (engine == 1)
    {
        HttpEndpoint endpoint;
        endpoint.url = httpUrl;
        endpoint.model = httpModel;
        http.reset(new LocalLLMClient(endpoint));
        {
            std::lock_guard<std::mutex> lock(m_httpMutex);
            m_http = http.get();
        }

        model = http.get();
    }

    if (m_cancel)
    {
        Post(kTaskFinished, "Cancelled.", false);
        return;
    }

    Post(kStatus, model ? "Matching the log to the source, then asking the AI..." : "Matching the log to the source...");

    // Stream the report to the window in small batches to keep the UI responsive.
    std::string pending;
    auto lastFlush = std::chrono::steady_clock::now();
    auto flush = [&]()
    {
        if (!pending.empty())
        {
            Post(kAppend, pending);
            pending.clear();
        }

        lastFlush = std::chrono::steady_clock::now();
    };

    auto started = std::chrono::steady_clock::now();
    bool ok = DiagnosisRunner::Run(*kb, logPath, model, [&](const std::string& text)
    {
        pending += text;
        if (pending.size() > 4000 || std::chrono::steady_clock::now() - lastFlush > std::chrono::milliseconds(100))
        {
            flush();
        }
    }, &m_cancel, error);

    flush();
    {
        std::lock_guard<std::mutex> lock(m_httpMutex);
        m_http = nullptr;
    }

    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    char done[96];
    sprintf_s(done, "Analysis finished in %.1f s.", seconds);
    Post(kTaskFinished, ok ? std::string(done) : error, ok);
}

// ----- report actions -----

void CLogRootCauseAnalyzerDlg::OnCopyReport()
{
    CString text;
    m_output.GetWindowText(text);
    if (text.IsEmpty() || !OpenClipboard())
    {
        return;
    }

    EmptyClipboard();
    size_t bytes = (static_cast<size_t>(text.GetLength()) + 1) * sizeof(TCHAR);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem)
    {
        void* dest = GlobalLock(mem);
        memcpy(dest, text.GetString(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
        {
            GlobalFree(mem);  // the clipboard owns it on success
        }
    }

    CloseClipboard();
    SetDlgItemText(IDC_STATUS, _T("Report copied to the clipboard."));
}

void CLogRootCauseAnalyzerDlg::OnSaveReport()
{
    CString text;
    m_output.GetWindowText(text);
    if (text.IsEmpty())
    {
        return;
    }

    CFileDialog dlg(FALSE, _T("txt"), _T("diagnosis.txt"), OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
        _T("Text files (*.txt)|*.txt|All files (*.*)|*.*||"), this);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    text.Replace(_T("\r\n"), _T("\n"));
    std::string error;
    if (WriteFileBytes(std::wstring(dlg.GetPathName()), ToUtf8(text), error))
    {
        SetDlgItemText(IDC_STATUS, _T("Report saved to ") + dlg.GetPathName());
    }
    else
    {
        AfxMessageBox(FromUtf8(error), MB_ICONERROR);
    }
}
