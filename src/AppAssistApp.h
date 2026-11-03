// AppAssistApp.h - MFC application object.
#pragma once

#include "framework.h"

class CAppAssistApp : public CWinApp
{
public:
    CAppAssistApp();
    BOOL InitInstance() override;

    DECLARE_MESSAGE_MAP()
};

extern CAppAssistApp theApp;
