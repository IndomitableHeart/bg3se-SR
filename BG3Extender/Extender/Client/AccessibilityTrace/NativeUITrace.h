#pragma once

BEGIN_NS(ecl)

namespace AccessibilityTrace
{
#if defined(BG3ACCESS_NATIVE_UI_TRACE)
    void Initialize();
    void LogStartupSummary();
    void Shutdown();
#endif
}

END_NS()
