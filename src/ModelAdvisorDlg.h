// ModelAdvisorDlg.h - Model Advisor: what this PC can run, which catalog models are downloaded, and which
// one suits the knowledge base; downloads (resumable, SHA-256 verified) on a background thread.
//
// The download thread never touches controls: it posts WM_MODEL_ADVISOR (WM_USER + 102) with a
// heap-allocated message, which the UI thread takes ownership of.
#pragma once

#include "framework.h"
#include "resource.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "CoreEngine.h"
#include "ModelAdvisor.h"
#include "ModelDownloader.h"

#define WM_MODEL_ADVISOR (WM_USER + 102)

class CModelAdvisorDlg : public CDialogEx
{
public:
    // taskId names a task profile in models.json. kb is the workload (null: nothing to measure yet);
    // currentModelPath may be empty.
    CModelAdvisorDlg(const std::string& taskId, std::shared_ptr<appassist::KnowledgeBase> kb,
        const std::wstring& currentModelPath, CWnd* parent = nullptr);
    ~CModelAdvisorDlg() override;

    enum { IDD = IDD_MODEL_ADVISOR };

    // After IDOK: the model the user chose.
    const std::wstring& ChosenModelPath() const
    {
        return m_chosenPath;
    }

protected:
    struct DownloadMessage
    {
        bool finished = false;
        bool ok = false;
        appassist::DownloadProgress progress;
        std::string error;
        std::wstring path;
    };

    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override;      // Enter = Use this model
    void OnCancel() override;  // Esc / Close: confirm if a download is running

    afx_msg void OnBrowseDir();
    afx_msg void OnDirChanged();
    afx_msg void OnDownload();
    afx_msg void OnCancelDownload();
    afx_msg void OnUseModel();
    afx_msg void OnListSelectionChanged(NMHDR* nmhdr, LRESULT* result);
    afx_msg void OnListDoubleClick(NMHDR* nmhdr, LRESULT* result);
    afx_msg LRESULT OnDownloadMessage(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    void Refresh(const appassist::CatalogModel* select);
    void UpdateButtons();
    void ShowAdvice();
    int SelectedIndex() const;
    void Post(std::unique_ptr<DownloadMessage> msg);
    void FinishWorker();

    const appassist::TaskProfile* m_task;
    std::shared_ptr<appassist::KnowledgeBase> m_kb;
    appassist::Workload m_workload;
    std::wstring m_currentPath;
    std::wstring m_chosenPath;

    appassist::SystemResources m_res;
    std::vector<appassist::ModelStatus> m_models;
    appassist::ModelAdvice m_advice;

    CListCtrl m_list;
    CProgressCtrl m_progress;
    bool m_busy;
    bool m_refreshing;
    const appassist::CatalogModel* m_downloading;
    appassist::ModelDownloader m_downloader;
    std::thread m_worker;
};
