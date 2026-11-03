// LogRootCauseAnalyzerDlg.h - main window: build a knowledge base from source, then diagnose logs.
//
// Long tasks run on a std::thread. The worker never touches controls: it posts WM_APP_ASSIST
// (WM_USER + 101) with a heap-allocated UiMessage, which the UI thread takes ownership of.
#pragma once

#include "framework.h"
#include "resource.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CoreEngine.h"
#include "EmbeddedLlamaEngine.h"
#include "LocalLLMClient.h"

#define WM_APP_ASSIST (WM_USER + 101)

class CLogRootCauseAnalyzerDlg : public CDialogEx
{
public:
    explicit CLogRootCauseAnalyzerDlg(CWnd* parent = nullptr);
    ~CLogRootCauseAnalyzerDlg() override;

    enum { IDD = IDD_APPASSIST_DIALOG };

protected:
    // What a worker thread sends to the UI thread.
    enum MessageKind
    {
        kStatus,
        kAppend,
        kKnowledgeBaseReady,
        kTaskFinished
    };

    struct UiMessage
    {
        MessageKind kind;
        CString text;
        bool ok;
        std::shared_ptr<appassist::KnowledgeBase> kb;
        std::wstring kbPath;
    };

    // Control layout for resizing: how each control follows the window's right/bottom edges.
    struct Anchor
    {
        UINT id;
        bool moveX;    // keep distance to the right edge
        bool sizeX;    // stretch with the width
        bool moveY;    // keep distance to the bottom edge
        bool sizeY;    // stretch with the height
        CRect rect;    // original position (client coordinates)
    };

    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override;      // Enter = Analyze
    void OnCancel() override;  // Esc / close: confirm if a task is running

    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);
    afx_msg void OnBrowseSource();
    afx_msg void OnBrowseKnowledgeBase();
    afx_msg void OnBrowseLog();
    afx_msg void OnBrowseModel();
    afx_msg void OnModels();
    afx_msg void OnBuildKnowledgeBase();
    afx_msg void OnAnalyze();
    afx_msg void OnCancelTask();
    afx_msg void OnCopyReport();
    afx_msg void OnSaveReport();
    afx_msg void OnEngineChanged();
    afx_msg LRESULT OnAppAssistMessage(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    // Worker side (any thread).
    void Post(MessageKind kind, const std::string& utf8Text, bool ok = true);
    void PostKnowledgeBase(const std::shared_ptr<appassist::KnowledgeBase>& kb, const std::wstring& path);
    void BuildKnowledgeBaseTask(std::wstring sourceDir, std::wstring kbPath, std::wstring modelPath);
    void AnalyzeTask(std::wstring kbPath, std::wstring logPath, int engine, std::wstring modelPath,
        std::wstring httpUrl, std::string httpModel);

    // UI side.
    bool StartTask(const CString& status, std::function<void()> task);
    void FinishWorker();
    void SetBusy(bool busy);
    void AppendOutput(const CString& text);
    void UpdateEngineControls();
    void ShowModelHint();
    void LoadSettings();
    void SaveSettings();
    CString GetText(UINT id) const;
    int SelectedEngine() const;     // 0 embedded, 1 http, 2 none

    CEdit m_output;
    CFont m_outputFont;
    std::vector<Anchor> m_anchors;
    CRect m_initialClient;
    CSize m_minTrackSize;
    bool m_busy;

    std::thread m_worker;
    std::atomic<bool> m_cancel;

    // Owned by the worker while a task runs; the UI thread only touches them when idle.
    std::shared_ptr<appassist::KnowledgeBase> m_kb;
    std::wstring m_kbPath;
    std::unique_ptr<appassist::EmbeddedLlamaEngine> m_engine;
    std::mutex m_httpMutex;
    appassist::LocalLLMClient* m_http;   // in-flight HTTP client, for Cancel
};
