// AppAssistApp.cpp - MFC application object: shows the diagnostic dialog.
#include "AppAssistApp.h"

#include "LogRootCauseAnalyzerDlg.h"

BEGIN_MESSAGE_MAP(CAppAssistApp, CWinApp)
END_MESSAGE_MAP()

CAppAssistApp theApp;

CAppAssistApp::CAppAssistApp()
{
}

BOOL CAppAssistApp::InitInstance()
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CWinApp::InitInstance();

    // Settings (last folders, model path, engine) live under HKCU\Software\AppAssist.
    SetRegistryKey(_T("AppAssist"));

    CLogRootCauseAnalyzerDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;  // dialog closed: exit the application
}
