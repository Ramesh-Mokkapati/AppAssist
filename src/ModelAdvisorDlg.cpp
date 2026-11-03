// ModelAdvisorDlg.cpp - see ModelAdvisorDlg.h.
#include "ModelAdvisorDlg.h"

using namespace appassist;

namespace
{
    CString FromUtf8(const std::string& text)
    {
        return CString(CA2T(text.c_str(), CP_UTF8));
    }

    std::wstring ParentDir(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    enum Column
    {
        kModel,
        kSize,
        kRam,
        kMemory,
        kSpeed,
        kStatus,
        kAdvice
    };

    const TCHAR* MemoryLabel(MemoryFit fit)
    {
        switch (fit)
        {
        case MemoryFit::Comfortable:
            return _T("fits");
        case MemoryFit::Tight:
            return _T("tight");
        default:
            return _T("too large");
        }
    }

}  // namespace

BEGIN_MESSAGE_MAP(CModelAdvisorDlg, CDialogEx)
    ON_BN_CLICKED(IDC_MA_BROWSE, &CModelAdvisorDlg::OnBrowseDir)
    ON_EN_KILLFOCUS(IDC_MA_DIR, &CModelAdvisorDlg::OnDirChanged)
    ON_BN_CLICKED(IDC_MA_DOWNLOAD, &CModelAdvisorDlg::OnDownload)
    ON_BN_CLICKED(IDC_MA_CANCEL, &CModelAdvisorDlg::OnCancelDownload)
    ON_BN_CLICKED(IDC_MA_USE, &CModelAdvisorDlg::OnUseModel)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_MA_LIST, &CModelAdvisorDlg::OnListSelectionChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_MA_LIST, &CModelAdvisorDlg::OnListDoubleClick)
    ON_MESSAGE(WM_MODEL_ADVISOR, &CModelAdvisorDlg::OnDownloadMessage)
END_MESSAGE_MAP()

CModelAdvisorDlg::CModelAdvisorDlg(const std::string& taskId, std::shared_ptr<KnowledgeBase> kb,
    const std::wstring& currentModelPath, CWnd* parent)
    : CDialogEx(IDD_MODEL_ADVISOR, parent)
    , m_task(FindTaskProfile(taskId))
    , m_kb(kb)
    , m_currentPath(currentModelPath)
    , m_busy(false)
    , m_refreshing(false)
    , m_downloading(nullptr)
{
}

CModelAdvisorDlg::~CModelAdvisorDlg()
{
    m_downloader.Cancel();
    FinishWorker();
}

void CModelAdvisorDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_MA_LIST, m_list);
    DDX_Control(pDX, IDC_MA_PROGRESS, m_progress);
}

BOOL CModelAdvisorDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    CRect r;
    m_list.GetClientRect(&r);
    const int w = r.Width();
    m_list.InsertColumn(kModel, _T("Model"), LVCFMT_LEFT, w * 35 / 100);
    m_list.InsertColumn(kSize, _T("Size"), LVCFMT_RIGHT, w * 8 / 100);
    m_list.InsertColumn(kRam, _T("RAM need"), LVCFMT_RIGHT, w * 10 / 100);
    m_list.InsertColumn(kMemory, _T("Memory"), LVCFMT_LEFT, w * 8 / 100);
    m_list.InsertColumn(kSpeed, _T("Speed"), LVCFMT_LEFT, w * 9 / 100);
    m_list.InsertColumn(kStatus, _T("Status"), LVCFMT_LEFT, w * 12 / 100);
    m_list.InsertColumn(kAdvice, _T("Advice"), LVCFMT_LEFT, w * 18 / 100 - 2);
    m_progress.SetRange32(0, 1000);

    if (!m_task && !TaskProfiles().empty())
    {
        m_task = &TaskProfiles().front();  // an unknown id in an edited models.json
    }

    if (m_kb)
    {
        m_workload = AssessSourceWorkload(*m_kb);
    }

    SetWindowText(FromUtf8("Model Advisor - " + (m_task ? m_task->name : std::string("no task profile"))));
    SetDlgItemText(IDC_MA_DIR, DefaultDownloadDir(m_currentPath).c_str());
    Refresh(nullptr);
    return TRUE;
}

// Re-detects resources and models (cheap: file sizes only) and redraws the list and advice.
void CModelAdvisorDlg::Refresh(const CatalogModel* select)
{
    CString dirText;
    GetDlgItemText(IDC_MA_DIR, dirText);
    dirText.Trim();
    const std::wstring dir(dirText);

    m_res = DetectSystemResources(dir);
    std::vector<std::wstring> searchDirs = ModelSearchDirs(ParentDir(m_currentPath));
    if (!dir.empty())
    {
        searchDirs.insert(searchDirs.begin(), dir);
    }

    if (!m_task)
    {
        SetDlgItemText(IDC_MA_SYSTEM, FromUtf8("models.json has no task profiles: " + CatalogSource()));
        return;
    }

    m_models = ScanModels(searchDirs, m_res, *m_task);
    m_advice = AdviseModel(m_res, m_models, *m_task, m_kb ? &m_workload : nullptr, m_currentPath);

    std::string noun = m_task->workloadNoun;
    if (!noun.empty())
    {
        noun[0] = static_cast<char>(toupper(static_cast<unsigned char>(noun[0])));
    }

    SetDlgItemText(IDC_MA_SYSTEM, FromUtf8("This PC: " + DescribeSystem(m_res)));
    SetDlgItemText(IDC_MA_KB, FromUtf8(noun + ": " + (m_advice.hasWorkload
        ? DescribeWorkload(m_advice)
        : std::string("none yet (the advice is for getting started)."))));

    if (!select)
    {
        select = m_advice.recommended ? m_advice.recommended : m_advice.current;
    }

    m_refreshing = true;
    m_list.DeleteAllItems();
    int selectRow = -1;
    for (size_t i = 0; i < m_models.size(); ++i)
    {
        const ModelStatus& s = m_models[i];
        const int row = static_cast<int>(i);
        m_list.InsertItem(row, FromUtf8(s.model->displayName));
        m_list.SetItemText(row, kSize, FromUtf8(FormatSize(s.model->sizeBytes)));
        m_list.SetItemText(row, kRam, FromUtf8(FormatSize(s.model->RequiredRamMB(m_task->contextTokens) * 1024ull * 1024)));
        m_list.SetItemText(row, kMemory, MemoryLabel(s.memory));
        m_list.SetItemText(row, kSpeed, FromUtf8(SpeedLabel(*s.model, m_res)));

        CString status;
        if (s.model == m_downloading)
        {
            status = _T("downloading...");
        }
        else if (s.state == ModelState::Downloaded)
        {
            status = _T("downloaded");
        }
        else if (s.state == ModelState::Partial)
        {
            status.Format(_T("partial (%d%%)"), static_cast<int>(100.0 * s.bytesOnDisk / s.model->sizeBytes));
        }
        else
        {
            status = s.fitsDisk ? _T("-") : _T("no disk space");
        }

        m_list.SetItemText(row, kStatus, status);

        CString advice;
        if (s.model == m_advice.firstModel)
        {
            advice = _T("first");
        }

        if (s.model == m_advice.recommended && m_advice.hasWorkload)
        {
            advice += advice.IsEmpty() ? _T("recommended") : _T(", recommended");
        }

        if (s.model == m_advice.current)
        {
            advice += advice.IsEmpty() ? _T("selected") : _T(", selected");
        }

        m_list.SetItemText(row, kAdvice, advice);
        if (s.model == select)
        {
            selectRow = row;
        }
    }

    if (selectRow >= 0)
    {
        m_list.SetItemState(selectRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        m_list.EnsureVisible(selectRow, FALSE);
    }

    m_refreshing = false;
    ShowAdvice();
    UpdateButtons();
}

int CModelAdvisorDlg::SelectedIndex() const
{
    POSITION pos = m_list.GetFirstSelectedItemPosition();
    return pos ? m_list.GetNextSelectedItem(pos) : -1;
}

// The advisor's reasons, then a description of the highlighted model.
void CModelAdvisorDlg::ShowAdvice()
{
    std::string text;
    for (size_t i = 0; i < m_advice.notes.size(); ++i)
    {
        text += m_advice.notes[i] + "\r\n";
    }

    int index = SelectedIndex();
    if (index >= 0 && index < static_cast<int>(m_models.size()))
    {
        const ModelStatus& s = m_models[index];
        text += "\r\n" + s.model->displayName + " - " + s.model->summary + " Licence: " +
                s.model->license + ".";
        if (s.memory == MemoryFit::TooLarge)
        {
            text += " It needs more RAM than this PC has.";
        }
        else if (s.memory == MemoryFit::Tight)
        {
            text += " It fits, but close other large programs while it runs.";
        }

        if (s.state != ModelState::NotDownloaded)
        {
            text += "\r\nFile: " + WideToUtf8(s.path);
        }
    }

    SetDlgItemText(IDC_MA_ADVICE, FromUtf8(text));
}

void CModelAdvisorDlg::UpdateButtons()
{
    int index = SelectedIndex();
    const ModelStatus* s = index >= 0 && index < static_cast<int>(m_models.size()) ? &m_models[index] : nullptr;
    const bool downloaded = s && s->state == ModelState::Downloaded;

    GetDlgItem(IDC_MA_DOWNLOAD)->SetWindowText(!s || s->state == ModelState::NotDownloaded ? _T("Download")
        : s->state == ModelState::Partial                                                 ? _T("Resume download")
                                                                                           : _T("Verify file"));
    GetDlgItem(IDC_MA_DOWNLOAD)->EnableWindow(!m_busy && s != nullptr);
    GetDlgItem(IDC_MA_CANCEL)->EnableWindow(m_busy);
    GetDlgItem(IDC_MA_USE)->EnableWindow(!m_busy && downloaded);
    GetDlgItem(IDC_MA_DIR)->EnableWindow(!m_busy);
    GetDlgItem(IDC_MA_BROWSE)->EnableWindow(!m_busy);
    m_list.EnableWindow(!m_busy);
}

void CModelAdvisorDlg::OnListSelectionChanged(NMHDR* nmhdr, LRESULT* result)
{
    const NMLISTVIEW* item = reinterpret_cast<const NMLISTVIEW*>(nmhdr);
    if (!m_refreshing && (item->uChanged & LVIF_STATE) != 0)
    {
        ShowAdvice();
        UpdateButtons();
    }

    *result = 0;
}

void CModelAdvisorDlg::OnListDoubleClick(NMHDR* /*nmhdr*/, LRESULT* result)
{
    if (GetDlgItem(IDC_MA_USE)->IsWindowEnabled())
    {
        OnUseModel();
    }

    *result = 0;
}

void CModelAdvisorDlg::OnBrowseDir()
{
    CString dir;
    GetDlgItemText(IDC_MA_DIR, dir);
    CFolderPickerDialog dlg(dir, 0, this);
    if (dlg.DoModal() == IDOK)
    {
        SetDlgItemText(IDC_MA_DIR, dlg.GetPathName());
        OnDirChanged();
    }
}

void CModelAdvisorDlg::OnDirChanged()
{
    if (!m_busy)
    {
        int index = SelectedIndex();
        Refresh(index >= 0 ? m_models[index].model : nullptr);
    }
}

// ----- download -----

void CModelAdvisorDlg::OnDownload()
{
    int index = SelectedIndex();
    if (m_busy || index < 0)
    {
        return;
    }

    const ModelStatus& s = m_models[index];
    if (s.memory == MemoryFit::TooLarge &&
        AfxMessageBox(_T("This model needs more RAM than this PC has, so it will run very slowly or not at all. ")
                      _T("Download it anyway?"), MB_YESNO | MB_ICONWARNING) != IDYES)
    {
        return;
    }

    // Resume or verify where the file already is; otherwise use the chosen folder.
    CString dirText;
    GetDlgItemText(IDC_MA_DIR, dirText);
    dirText.Trim();
    std::wstring dir = s.state != ModelState::NotDownloaded ? ParentDir(s.path) : std::wstring(dirText);
    if (dir.empty())
    {
        AfxMessageBox(_T("Choose a download folder first."), MB_ICONINFORMATION);
        return;
    }

    const CatalogModel* model = s.model;
    FinishWorker();
    m_busy = true;
    m_downloading = model;
    m_progress.SetPos(0);
    SetDlgItemText(IDC_MA_STATUS, _T("Connecting..."));
    m_list.SetItemText(index, kStatus, _T("downloading..."));
    UpdateButtons();
    GetDlgItem(IDC_MA_CANCEL)->SetFocus();

    m_worker = std::thread([this, model, dir]()
    {
        std::unique_ptr<DownloadMessage> done(new DownloadMessage());
        done->finished = true;
        try
        {
            done->ok = m_downloader.Download(*model, dir, [this](const DownloadProgress& p)
            {
                std::unique_ptr<DownloadMessage> msg(new DownloadMessage());
                msg->progress = p;
                Post(std::move(msg));
            }, done->path, done->error);
        }
        catch (const std::exception& e)
        {
            done->error = std::string("Unexpected error: ") + e.what();
        }

        Post(std::move(done));
    });
}

void CModelAdvisorDlg::Post(std::unique_ptr<DownloadMessage> msg)
{
    if (::IsWindow(GetSafeHwnd()) && PostMessage(WM_MODEL_ADVISOR, 0, reinterpret_cast<LPARAM>(msg.get())))
    {
        msg.release();  // the UI thread owns it now
    }
}

LRESULT CModelAdvisorDlg::OnDownloadMessage(WPARAM /*wParam*/, LPARAM lParam)
{
    std::unique_ptr<DownloadMessage> msg(reinterpret_cast<DownloadMessage*>(lParam));
    if (!msg->finished)
    {
        const DownloadProgress& p = msg->progress;
        if (p.total > 0)
        {
            m_progress.SetPos(static_cast<int>(1000.0 * p.done / p.total));
        }

        CString status;
        if (p.phase == "Downloading")
        {
            std::string eta;
            if (p.bytesPerSecond > 0 && p.total > p.done)
            {
                const unsigned long long seconds = static_cast<unsigned long long>((p.total - p.done) / p.bytesPerSecond);
                char text[64];
                sprintf_s(text, ", about %llu min %02llu s left", seconds / 60, seconds % 60);
                eta = text;
            }

            char rate[32];
            sprintf_s(rate, "%.1f MB/s", p.bytesPerSecond / (1024.0 * 1024.0));
            status = FromUtf8("Downloading " + FormatSize(p.done) + " of " + FormatSize(p.total) + " at " + rate + eta);
        }
        else
        {
            status.Format(_T("Verifying the file (SHA-256)... %d%%"), static_cast<int>(100.0 * p.done / (p.total ? p.total : 1)));
        }

        SetDlgItemText(IDC_MA_STATUS, status);
        return 0;
    }

    FinishWorker();
    const CatalogModel* model = m_downloading;
    m_busy = false;
    m_downloading = nullptr;
    m_progress.SetPos(msg->ok ? 1000 : m_progress.GetPos());
    SetDlgItemText(IDC_MA_STATUS, msg->ok ? CString(_T("Downloaded and verified: ")) + msg->path.c_str()
                                          : FromUtf8(msg->error));
    Refresh(model);
    if (!msg->ok && msg->error.find("Cancelled") != 0)
    {
        AfxMessageBox(FromUtf8(msg->error), MB_ICONWARNING);
    }

    return 0;
}

void CModelAdvisorDlg::OnCancelDownload()
{
    if (m_busy)
    {
        SetDlgItemText(IDC_MA_STATUS, _T("Cancelling..."));
        GetDlgItem(IDC_MA_CANCEL)->EnableWindow(FALSE);
        m_downloader.Cancel();
    }
}

void CModelAdvisorDlg::FinishWorker()
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

// ----- closing -----

void CModelAdvisorDlg::OnUseModel()
{
    int index = SelectedIndex();
    if (m_busy || index < 0 || m_models[index].state != ModelState::Downloaded)
    {
        return;
    }

    m_chosenPath = m_models[index].path;
    CDialogEx::OnOK();
}

void CModelAdvisorDlg::OnOK()
{
    if (GetDlgItem(IDC_MA_USE)->IsWindowEnabled())
    {
        OnUseModel();
    }
}

void CModelAdvisorDlg::OnCancel()
{
    if (m_busy)
    {
        if (AfxMessageBox(_T("A download is running. Stop it and close? The partial file is kept, and Download ")
                          _T("resumes it later."), MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            return;
        }

        CWaitCursor wait;
        m_downloader.Cancel();
        FinishWorker();
        m_busy = false;

        // Free the messages the thread posted that will never be handled now.
        MSG pending;
        while (::PeekMessage(&pending, GetSafeHwnd(), WM_MODEL_ADVISOR, WM_MODEL_ADVISOR, PM_REMOVE))
        {
            delete reinterpret_cast<DownloadMessage*>(pending.lParam);
        }
    }

    CDialogEx::OnCancel();
}
