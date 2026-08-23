// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "winrt/Windows.UI.Xaml.Interop.h"
#include "ConclusionMarkdownView.h"

#include "ConclusionMarkdownView.g.cpp"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    DependencyProperty ConclusionMarkdownView::_MarkdownProperty{ nullptr };

    ConclusionMarkdownView::ConclusionMarkdownView()
    {
        _InitializeProperties();
        HorizontalContentAlignment(HorizontalAlignment::Stretch);
        HorizontalAlignment(HorizontalAlignment::Stretch);
    }

    void ConclusionMarkdownView::_InitializeProperties()
    {
        static auto [[maybe_unused]] registered = [] {
            _MarkdownProperty = DependencyProperty::Register(
                L"Markdown",
                xaml_typename<winrt::hstring>(),
                xaml_typename<winrt::TerminalApp::ConclusionMarkdownView>(),
                PropertyMetadata{ nullptr, ConclusionMarkdownView::_onMarkdownChanged });
            return true;
        }();
    }

    void ConclusionMarkdownView::_onMarkdownChanged(const DependencyObject& object,
                                                    const DependencyPropertyChangedEventArgs&)
    {
        if (const auto view = object.try_as<winrt::TerminalApp::ConclusionMarkdownView>())
        {
            winrt::get_self<ConclusionMarkdownView>(view)->_renderMarkdown();
        }
    }

    void ConclusionMarkdownView::_renderMarkdown()
    {
        const auto markdown = Markdown();
        if (markdown.empty())
        {
            Content(nullptr);
            return;
        }

        try
        {
            auto rendered = Microsoft::Terminal::UI::Markdown::Builder::ConvertForCompactContent(markdown);
            rendered.HorizontalAlignment(HorizontalAlignment::Stretch);
            Content(rendered);
        }
        catch (...)
        {
            // Rendering must never make the conclusion unavailable. Preserve a
            // selectable plain-text fallback if parsing or XAML creation fails.
            TextBlock fallback;
            fallback.ContextFlyout(Microsoft::Terminal::UI::TextMenuFlyout{});
            fallback.IsTextSelectionEnabled(true);
            fallback.Text(markdown);
            fallback.TextWrapping(TextWrapping::WrapWholeWords);
            Content(fallback);
        }
    }
}
