#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Instance.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::RadPad::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // Files on the command line open in a window already open on this virtual desktop, if there is one.
        if (instance::ForwardCommandLineToExistingWindow())
        {
            Exit();
            return;
        }

        instance::SetProcessAppId();
        m_window = make<MainWindow>();
        m_window.Activate();
    }
}
