// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ConclusionMarkdownView.g.h"

namespace winrt::TerminalApp::implementation
{
    struct ConclusionMarkdownView : ConclusionMarkdownViewT<ConclusionMarkdownView>
    {
        ConclusionMarkdownView();

        DEPENDENCY_PROPERTY(winrt::hstring, Markdown);

    private:
        static void _InitializeProperties();
        static void _onMarkdownChanged(const Windows::UI::Xaml::DependencyObject& object,
                                       const Windows::UI::Xaml::DependencyPropertyChangedEventArgs& args);
        void _renderMarkdown();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(ConclusionMarkdownView);
}
