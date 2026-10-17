// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/TerminalPage.h"
#include "../TerminalApp/WorkspaceFeatureSet.h"
#include "../TerminalApp/WorkspaceAutomation.h"
#include "../TerminalApp/TerminalWindow.h"
#include "../TerminalApp/MinMaxCloseControl.h"
#include "../TerminalApp/TabRowControl.h"
#include "../TerminalApp/WorkspaceItemViewModel.h"
#include "../TerminalApp/WorkspaceGitBranch.h"
#include "../TerminalApp/WorkspaceAgentResume.h"
#include "../TerminalApp/WorkspaceSidebar.h"
#include "../TerminalApp/ConclusionMarkdownView.h"
#include "../TerminalApp/ShortcutActionDispatch.h"
#include "../TerminalApp/Tab.h"
#include "../TerminalApp/CommandPalette.h"
#include "../TerminalApp/ContentManager.h"
#include "CppWinrtTailored.h"

using namespace Microsoft::Console;
using namespace TerminalApp;
using namespace winrt::TerminalApp;
using namespace winrt::Microsoft::Terminal::Settings::Model;

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace TerminalAppLocalTests
{
    namespace
    {
        struct WorkspaceDirectoryConnection :
            winrt::implements<WorkspaceDirectoryConnection, winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection>
        {
            void Initialize(const winrt::Windows::Foundation::Collections::ValueSet&) {}
            void Start() noexcept {}
            void WriteInput(const winrt::array_view<const char16_t> input) { Input.append(input.begin(), input.end()); }
            std::wstring Input;
            void Resize(uint32_t, uint32_t) noexcept {}
            void Close() noexcept {}
            winrt::guid SessionId() const noexcept { return {}; }
            winrt::Microsoft::Terminal::TerminalConnection::ConnectionState State() const noexcept
            {
                return winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Connected;
            }

            void ReportDirectory(const std::wstring_view path)
            {
                const auto output = std::wstring{ L"\x1b]9;9;" } + std::wstring{ path } + L"\x7";
                TerminalOutput.raise(winrt_wstring_to_array_view(output));
            }

            til::event<winrt::Microsoft::Terminal::TerminalConnection::TerminalOutputHandler> TerminalOutput;
            til::typed_event<winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection, winrt::Windows::Foundation::IInspectable> StateChanged;
        };

        winrt::Windows::Data::Json::JsonObject MakeWorkspaceAutomationRequest(
            const winrt::hstring& method,
            const winrt::guid& workspaceId,
            const std::optional<winrt::guid> surfaceId = std::nullopt,
            const std::optional<uint32_t> paneId = std::nullopt)
        {
            using namespace winrt::Windows::Data::Json;
            JsonObject params;
            params.Insert(L"workspaceId", JsonValue::CreateStringValue(Utils::GuidToPlainString(workspaceId)));
            if (surfaceId)
            {
                params.Insert(L"surfaceId", JsonValue::CreateStringValue(Utils::GuidToPlainString(surfaceId.value())));
            }
            if (paneId)
            {
                params.Insert(L"paneId", JsonValue::CreateNumberValue(paneId.value()));
            }
            JsonObject message;
            message.Insert(L"version", JsonValue::CreateNumberValue(1));
            message.Insert(L"id", JsonValue::CreateStringValue(L"surface-test"));
            message.Insert(L"method", JsonValue::CreateStringValue(method));
            message.Insert(L"params", params);
            return message;
        }

        struct MarkdownInspection
        {
            std::wstring Text;
            std::vector<winrt::hstring> LinkSchemes;
            size_t CodeBlockCount{};
            size_t ImageCount{};
            bool SawBold{};
            bool SawItalic{};
            bool SawInlineCode{};
            bool SawStrikethrough{};
        };

        void InspectMarkdownInline(const winrt::Windows::UI::Xaml::Documents::Inline& inlineElement,
                                   MarkdownInspection& inspection)
        {
            namespace Documents = winrt::Windows::UI::Xaml::Documents;

            if (const auto run = inlineElement.try_as<Documents::Run>())
            {
                inspection.Text.append(run.Text());
                inspection.SawBold = inspection.SawBold || run.FontWeight().Weight >= FontWeights::Bold().Weight;
                inspection.SawItalic = inspection.SawItalic || run.FontStyle() == FontStyle::Italic;
                inspection.SawStrikethrough = inspection.SawStrikethrough ||
                                              (static_cast<uint32_t>(run.TextDecorations()) &
                                               static_cast<uint32_t>(TextDecorations::Strikethrough)) != 0;
                if (const auto font = run.FontFamily())
                {
                    const auto source = font.Source();
                    const std::wstring_view sourceView{ source.c_str(), source.size() };
                    inspection.SawInlineCode = inspection.SawInlineCode || sourceView.find(L"Cascadia Mono") != std::wstring_view::npos;
                }
            }
            else if (const auto hyperlink = inlineElement.try_as<Documents::Hyperlink>())
            {
                if (const auto uri = hyperlink.NavigateUri())
                {
                    inspection.LinkSchemes.emplace_back(uri.SchemeName());
                }
                for (const auto& child : hyperlink.Inlines())
                {
                    InspectMarkdownInline(child, inspection);
                }
            }
            else if (const auto span = inlineElement.try_as<Documents::Span>())
            {
                for (const auto& child : span.Inlines())
                {
                    InspectMarkdownInline(child, inspection);
                }
            }
            else if (const auto container = inlineElement.try_as<Documents::InlineUIContainer>())
            {
                const auto child = container.Child();
                inspection.CodeBlockCount += child.try_as<winrt::Microsoft::Terminal::UI::Markdown::CodeBlock>() ? 1 : 0;
                inspection.ImageCount += child.try_as<Image>() ? 1 : 0;
            }
            else if (inlineElement.try_as<Documents::LineBreak>())
            {
                inspection.Text.append(L"\n");
            }
        }

        MarkdownInspection InspectMarkdown(const RichTextBlock& richText)
        {
            MarkdownInspection inspection;
            for (const auto& block : richText.Blocks())
            {
                if (const auto paragraph = block.try_as<winrt::Windows::UI::Xaml::Documents::Paragraph>())
                {
                    for (const auto& inlineElement : paragraph.Inlines())
                    {
                        InspectMarkdownInline(inlineElement, inspection);
                    }
                    inspection.Text.append(L"\n");
                }
            }
            return inspection;
        }
    }

    // TODO:microsoft/terminal#3838:
    // Unfortunately, these tests _WILL NOT_ work in our CI. We're waiting for
    // an updated TAEF that will let us install framework packages when the test
    // package is deployed. Until then, these tests won't deploy in CI.

    class TabTests
    {
        // For this set of tests, we need to activate some XAML content. For
        // release builds, the application runs as a centennial application,
        // which lets us run full trust, and means that we need to use XAML
        // Islands to host our UI. However, in these tests, we don't really need
        // to run full trust - we just need to get some UI elements created. So
        // we can just rely on the normal UWP activation to create us.
        //
        // IMPORTANTLY! When tests need to make XAML objects, or do XAML things,
        // make sure to use RunOnUIThread. This helper will dispatch a lambda to
        // be run on the UI thread.

        BEGIN_TEST_CLASS(TabTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        // These four tests act as canary tests. If one of them fails, then they
        // can help you identify if something much lower in the stack has
        // failed.
        TEST_METHOD(EnsureTestsActivate);
        TEST_METHOD(TryCreateConnectionType);
        TEST_METHOD(TryCreateXamlObjects);

        TEST_METHOD(TryInitializePage);

        TEST_METHOD(CreateSimpleTerminalXamlType);
        TEST_METHOD(CreateTerminalMuxXamlType);

        TEST_METHOD(CreateTerminalPage);
        TEST_METHOD(CreateWorkspaceSidebarXamlType);
        TEST_METHOD(ConclusionMarkdownRendersCompactCommonMark);
        TEST_METHOD(ConclusionMarkdownRendersGfmExtensions);
        TEST_METHOD(ConclusionMarkdownRestrictsExternalContent);
        TEST_METHOD(WorkspaceSidebarAdaptsToWindowModes);
        TEST_METHOD(WorkspaceItemViewModelTracksTab);
        TEST_METHOD(WorkspaceGroupsMultipleTerminalSurfaces);
        TEST_METHOD(WorkspaceSurfaceShortcutActions);
        TEST_METHOD(WorkspaceAutomationRequiresSurfaceId);
        TEST_METHOD(WorkspaceAutomationTargetsExactSurface);
        TEST_METHOD(WorkspaceNamesSurviveSurfaceChangesAndRestore);
        TEST_METHOD(WorkspaceEnvironmentUsesAuthoritativeRoutingMetadata);
        TEST_METHOD(WorkspaceWorkingDirectoryDisplayIsFolderName);
        TEST_METHOD(WorkspaceWorkingDirectoryTracksFirstSurface);
        TEST_METHOD(WorkspaceWorkingDirectoryNewTerminalFollowsActiveSurface);
        TEST_METHOD(WorkspaceWorkingDirectoryNewTerminalFollowsActivePane);
        TEST_METHOD(WorkspaceWorkingDirectoryNewTerminalHonorsLaunchContext);
        TEST_METHOD(WorkspaceVimModeInputRouting);
        TEST_METHOD(WorkspaceClosingTerminalsKeepsSelectionValid);
        TEST_METHOD(WorkspaceGitBranchParsesStructuredOutput);
        TEST_METHOD(WorkspaceListeningPortsProjectMetadata);
        TEST_METHOD(WorkspaceAgentResumeRejectsUntrustedInput);
        TEST_METHOD(WorkspaceSnapshotOmitsRuntimeProcessState);
        TEST_METHOD(WorkspaceSnapshotOpenIsNonConsuming);
        TEST_METHOD(WorkspaceSnapshotReopensInOwnWorkspace);
        TEST_METHOD(WorkspaceSnapshotRejectsUnsafeActions);
        TEST_METHOD(WorkspaceSnapshotSourceSurvivesContentAttach);
        TEST_METHOD(WorkspaceSnapshotStaleSourceIsCleared);
        TEST_METHOD(WorkspaceStatusPillTracksStructuredState);
        TEST_METHOD(WorkspaceLayoutPersistsMetadata);
        TEST_METHOD(WorkspaceNotificationStateTracksUnread);
        TEST_METHOD(WorkspaceNotificationRefreshIsBounded);
        TEST_METHOD(PaneAttentionState);

        TEST_METHOD(TryDuplicateBadTab);
        TEST_METHOD(TryDuplicateBadPane);

        TEST_METHOD(TryZoomPane);
        TEST_METHOD(MoveFocusFromZoomedPane);
        TEST_METHOD(CloseZoomedPane);

        TEST_METHOD(SwapPanes);

        TEST_METHOD(NextMRUTab);
        TEST_METHOD(VerifyCommandPaletteTabSwitcherOrder);

        TEST_METHOD(TestWindowRenameSuccessful);
        TEST_METHOD(TestWindowRenameFailure);

        TEST_METHOD(TestPreviewCommitScheme);
        TEST_METHOD(TestPreviewDismissScheme);
        TEST_METHOD(TestPreviewSchemeWhilePreviewing);

        TEST_METHOD(TestClampSwitchToTab);

        TEST_CLASS_SETUP(ClassSetup)
        {
            return true;
        }

        TEST_METHOD_CLEANUP(MethodCleanup)
        {
            return true;
        }

    private:
        void _initializeTerminalPage(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
                                     CascadiaSettings initialSettings);
        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> _commonSetup();
        winrt::com_ptr<winrt::TerminalApp::implementation::WindowProperties> _windowProperties;
        winrt::com_ptr<winrt::TerminalApp::implementation::ContentManager> _contentManager;
    };

    template<typename TFunction>
    void TestOnUIThread(const TFunction& function)
    {
        const auto result = RunOnUIThread(function);
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::EnsureTestsActivate()
    {
        // This test was originally used to ensure that XAML Islands was
        // initialized correctly. Now, it's used to ensure that the tests
        // actually deployed and activated. This test _should_ always pass.
        VERIFY_IS_TRUE(true);
    }

    void TabTests::TryCreateConnectionType()
    {
        // Verify we can create a WinRT type we authored
        // Just creating it is enough to know that everything is working.
        winrt::Microsoft::Terminal::TerminalConnection::EchoConnection conn{};
        VERIFY_IS_NOT_NULL(conn);
    }

    void TabTests::TryCreateXamlObjects()
    {
        auto result = RunOnUIThread([]() {
            VERIFY_IS_TRUE(true, L"Congrats! We're running on the UI thread!");

            auto v = winrt::Windows::ApplicationModel::Core::CoreApplication::GetCurrentView();
            VERIFY_IS_NOT_NULL(v, L"Ensure we have a current view");
            // Verify we can create a some XAML objects
            // Just creating all of them is enough to know that everything is working.
            winrt::Windows::UI::Xaml::Controls::UserControl controlRoot;
            VERIFY_IS_NOT_NULL(controlRoot, L"Try making a UserControl");
            winrt::Windows::UI::Xaml::Controls::Grid root;
            VERIFY_IS_NOT_NULL(root, L"Try making a Grid");
            winrt::Windows::UI::Xaml::Controls::SwapChainPanel swapChainPanel;
            VERIFY_IS_NOT_NULL(swapChainPanel, L"Try making a SwapChainPanel");
            winrt::Windows::UI::Xaml::Controls::Primitives::ScrollBar scrollBar;
            VERIFY_IS_NOT_NULL(scrollBar, L"Try making a ScrollBar");
        });

        VERIFY_SUCCEEDED(result);
    }

    void TabTests::CreateSimpleTerminalXamlType()
    {
        winrt::com_ptr<winrt::TerminalApp::implementation::MinMaxCloseControl> mmcc{ nullptr };

        auto result = RunOnUIThread([&mmcc]() {
            mmcc = winrt::make_self<winrt::TerminalApp::implementation::MinMaxCloseControl>();
            VERIFY_IS_NOT_NULL(mmcc);
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::CreateTerminalMuxXamlType()
    {
        winrt::com_ptr<winrt::TerminalApp::implementation::TabRowControl> tabRowControl{ nullptr };

        auto result = RunOnUIThread([&tabRowControl]() {
            tabRowControl = winrt::make_self<winrt::TerminalApp::implementation::TabRowControl>();
            VERIFY_IS_NOT_NULL(tabRowControl);
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::CreateTerminalPage()
    {
        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };

        _windowProperties = winrt::make_self<winrt::TerminalApp::implementation::WindowProperties>();
        winrt::TerminalApp::WindowProperties props = *_windowProperties;

        _contentManager = winrt::make_self<winrt::TerminalApp::implementation::ContentManager>();
        winrt::TerminalApp::ContentManager contentManager = *_contentManager;

        auto result = RunOnUIThread([&page, props, contentManager]() {
            page = winrt::make_self<winrt::TerminalApp::implementation::TerminalPage>(props, contentManager);
            VERIFY_IS_NOT_NULL(page);
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::CreateWorkspaceSidebarXamlType()
    {
        auto result = RunOnUIThread([]() {
            const winrt::TerminalApp::WorkspaceSidebar sidebar{};
            VERIFY_IS_NOT_NULL(sidebar);
            VERIFY_IS_NOT_NULL(sidebar.WorkspaceList());
            VERIFY_IS_NOT_NULL(sidebar.NotificationHistory());
            VERIFY_ARE_EQUAL(0u, sidebar.NotificationHistory().Size());
            VERIFY_IS_TRUE(sidebar.ExpandedWidth() >= 152.0);
            VERIFY_IS_TRUE(sidebar.ExpandedWidth() <= 240.0);
            const auto resizeHandle = sidebar.FindName(L"ResizeHandle").as<winrt::Windows::UI::Xaml::Controls::Primitives::Thumb>();
            VERIFY_IS_NOT_NULL(resizeHandle);
            VERIFY_IS_TRUE(resizeHandle.IsTabStop());
            VERIFY_IS_FALSE(winrt::Windows::UI::Xaml::Automation::AutomationProperties::GetName(resizeHandle).empty());
            VERIFY_IS_FALSE(winrt::Windows::UI::Xaml::Automation::AutomationProperties::GetHelpText(resizeHandle).empty());
            VERIFY_IS_FALSE(winrt::Windows::UI::Xaml::Automation::AutomationProperties::GetHelpText(sidebar.WorkspaceList()).empty());
            const auto highContrastResources = sidebar.WorkspaceList().Resources().ThemeDictionaries().Lookup(winrt::box_value(L"HighContrast")).as<ResourceDictionary>();
            VERIFY_IS_NOT_NULL(highContrastResources);
            VERIFY_IS_TRUE(highContrastResources.HasKey(winrt::box_value(L"ListViewItemBackgroundSelected")));
            VERIFY_IS_TRUE(highContrastResources.HasKey(winrt::box_value(L"ListViewItemForegroundSelected")));
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::ConclusionMarkdownRendersCompactCommonMark()
    {
        const auto result = RunOnUIThread([]() {
            const winrt::TerminalApp::ConclusionMarkdownView view{};
            view.Markdown(L"# Heading\n\n3. First\n4. Second\n\n**bold** *italic* `inline`\n\n---\n\n```powershell\nGet-Date\n```");

            const auto richText = view.Content().as<RichTextBlock>();
            VERIFY_ARE_EQUAL(12.0, richText.FontSize());

            double maximumParagraphFontSize = 0.0;
            for (const auto& block : richText.Blocks())
            {
                if (const auto paragraph = block.try_as<winrt::Windows::UI::Xaml::Documents::Paragraph>())
                {
                    maximumParagraphFontSize = std::max(maximumParagraphFontSize, paragraph.FontSize());
                }
            }
            VERIFY_IS_TRUE(maximumParagraphFontSize <= 16.0);

            const auto inspection = InspectMarkdown(richText);
            VERIFY_IS_TRUE(inspection.Text.find(L"3. First") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"4. Second") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"────────") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.SawBold);
            VERIFY_IS_TRUE(inspection.SawItalic);
            VERIFY_IS_TRUE(inspection.SawInlineCode);
            VERIFY_ARE_EQUAL(1u, inspection.CodeBlockCount);

            for (const auto& block : richText.Blocks())
            {
                if (const auto paragraph = block.try_as<winrt::Windows::UI::Xaml::Documents::Paragraph>())
                {
                    for (const auto& inlineElement : paragraph.Inlines())
                    {
                        if (const auto container = inlineElement.try_as<winrt::Windows::UI::Xaml::Documents::InlineUIContainer>())
                        {
                            if (const auto codeBlock = container.Child().try_as<winrt::Microsoft::Terminal::UI::Markdown::CodeBlock>())
                            {
                                VERIFY_ARE_EQUAL(Visibility::Collapsed, codeBlock.PlayButtonVisibility());
                            }
                        }
                    }
                }
            }
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::ConclusionMarkdownRestrictsExternalContent()
    {
        const auto result = RunOnUIThread([]() {
            const winrt::TerminalApp::ConclusionMarkdownView view{};
            view.Markdown(L"[web](https://example.com) [mail](mailto:test@example.com) [unsafe](javascript:alert(1)) ![diagram](https://example.com/image.png) <b>html</b>");

            const auto richText = view.Content().as<RichTextBlock>();
            const auto inspection = InspectMarkdown(richText);
            VERIFY_ARE_EQUAL(2u, inspection.LinkSchemes.size());
            VERIFY_IS_TRUE(til::equals_insensitive_ascii(inspection.LinkSchemes.at(0), L"https"));
            VERIFY_IS_TRUE(til::equals_insensitive_ascii(inspection.LinkSchemes.at(1), L"mailto"));
            VERIFY_ARE_EQUAL(0u, inspection.ImageCount);
            VERIFY_IS_TRUE(inspection.Text.find(L"unsafe") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"diagram") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"html") != std::wstring::npos);
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::ConclusionMarkdownRendersGfmExtensions()
    {
        const auto result = RunOnUIThread([]() {
            const winrt::TerminalApp::ConclusionMarkdownView view{};
            view.Markdown(L"| Name | Status |\n| --- | --- |\n| build | **ready** |\n\n- [ ] todo\n- [x] done\n\n7. [x] numbered\n8. regular\n\n~~obsolete~~\n\nVisit https://example.org/docs.");

            const auto richText = view.Content().as<RichTextBlock>();
            const auto inspection = InspectMarkdown(richText);
            VERIFY_IS_TRUE(inspection.Text.find(L"Name  │  Status") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"build  │  ready") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"☐ todo") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"☑ done") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"7. ☑ numbered") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.Text.find(L"8. regular") != std::wstring::npos);
            VERIFY_IS_TRUE(inspection.SawBold);
            VERIFY_IS_TRUE(inspection.SawStrikethrough);
            VERIFY_ARE_EQUAL(1u, inspection.LinkSchemes.size());
            VERIFY_IS_TRUE(til::equals_insensitive_ascii(inspection.LinkSchemes.at(0), L"https"));
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::WorkspaceSidebarAdaptsToWindowModes()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "tabLayout": "workspaceSidebar",
            "showTabsInTitlebar": false,
            "profiles": [
                {
                    "name": "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPage(page, settings);

        TestOnUIThread([&]() {
            VERIFY_IS_TRUE(page->_SidebarController().Enabled);
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_workspaceSidebar.Visibility());
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_workspaceSurfaceTabBar.Visibility());
            VERIFY_IS_TRUE(page->_tabRow.WorkspaceSidebarMode());
            VERIFY_ARE_EQUAL(0.0, page->_tabView.Width());

            const auto sidebar = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceSidebar>(page->_workspaceSidebar);
            const auto tabRow = winrt::get_self<winrt::TerminalApp::implementation::TabRowControl>(page->_tabRow);
            VERIFY_IS_NOT_NULL(tabRow->WorkspaceNewTabButton());
            VERIFY_IS_NOT_NULL(tabRow->WorkspaceNotificationButton());
            VERIFY_IS_NOT_NULL(tabRow->WorkspaceSidebarToggleButton());
            VERIFY_ARE_EQUAL(Visibility::Visible, tabRow->WorkspaceNewTabButton().Visibility());
            VERIFY_IS_NULL(page->_workspaceSidebar.FindName(L"NewWorkspaceButton"));
            const auto regularNewTabFlyout = page->_newTabButton.Flyout().as<MenuFlyout>();
            const auto workspaceNewTabFlyout = tabRow->WorkspaceNewTabButton().Flyout().as<MenuFlyout>();
            VERIFY_IS_NOT_NULL(workspaceNewTabFlyout);
            VERIFY_ARE_EQUAL(regularNewTabFlyout.Items().Size(), workspaceNewTabFlyout.Items().Size());

            const auto workspaceItem = page->_WorkspaceItems().GetAt(0);
            VERIFY_ARE_EQUAL(workspaceItem.Surfaces(), page->_workspaceSurfaceTabs.ItemsSource().as<IObservableVector<winrt::TerminalApp::Tab>>());
            VERIFY_ARE_EQUAL(workspaceItem.SourceTab(), page->_workspaceSurfaceTabs.SelectedItem().as<winrt::TerminalApp::Tab>());
            VERIFY_IS_NOT_NULL(page->_workspaceSurfaceNewTabButton);
            const auto newTerminalGlyph = winrt::unbox_value<winrt::hstring>(page->_workspaceSurfaceNewTabButton.Content());
            VERIFY_IS_TRUE(std::wstring_view{ newTerminalGlyph } == L"\uE756");
            VERIFY_IS_NOT_NULL(page->_workspaceToolTabs);
            VERIFY_ARE_EQUAL(1u, page->_workspaceToolTabs.Items().Size());
            const auto conclusionItem = page->_workspaceToolTabs.Items().GetAt(0).as<ListViewItem>();
            const auto conclusionContent = conclusionItem.Content().as<Grid>();
            VERIFY_ARE_EQUAL(36.0, conclusionItem.Width());
            VERIFY_ARE_EQUAL(1u, conclusionContent.Children().Size());
            VERIFY_IS_NOT_NULL(conclusionContent.Children().GetAt(0).try_as<FontIcon>());
            VERIFY_ARE_EQUAL(-1, page->_workspaceToolTabs.SelectedIndex());
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_tabContent.Visibility());
            VERIFY_ARE_EQUAL(Visibility::Collapsed, page->_conclusionSidebar.Visibility());

            page->_workspaceToolTabs.SelectedIndex(0);
            VERIFY_IS_TRUE(page->_workspaceToolTabSelected);
            VERIFY_ARE_EQUAL(-1, page->_workspaceSurfaceTabs.SelectedIndex());
            VERIFY_ARE_EQUAL(Visibility::Collapsed, page->_tabContent.Visibility());
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_conclusionSidebar.Visibility());

            page->_workspaceSurfaceTabs.SelectedItem(workspaceItem.SourceTab());
            VERIFY_IS_FALSE(page->_workspaceToolTabSelected);
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_tabContent.Visibility());
            VERIFY_ARE_EQUAL(Visibility::Collapsed, page->_conclusionSidebar.Visibility());
            sidebar->ShowSwitchHints(true);
            const std::wstring_view shortcut{ workspaceItem.SwitchShortcutText() };
            const auto separator = shortcut.find_last_of(L'+');
            const std::wstring_view expectedHint = shortcut.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
            VERIFY_IS_TRUE(expectedHint == std::wstring_view{ workspaceItem.SwitchShortcutHintText() });
            sidebar->ShowSwitchHints(false);
            VERIFY_IS_TRUE(workspaceItem.SwitchShortcutHintText().empty());

            const auto toggleSidebar = [&]() {
                return page->_actionDispatch->DoAction(ActionAndArgs{ ShortcutAction::ToggleWorkspaceSidebar, nullptr });
            };
            VERIFY_IS_TRUE(page->_workspaceSidebar.IsExpanded());
            VERIFY_IS_TRUE(toggleSidebar());
            VERIFY_IS_FALSE(page->_workspaceSidebar.IsExpanded());
            VERIFY_ARE_EQUAL(0.0, page->_workspaceSidebar.Width());
            VERIFY_ARE_EQUAL(Visibility::Collapsed, page->_workspaceSidebar.WorkspaceList().Visibility());
            VERIFY_IS_TRUE(toggleSidebar());
            VERIFY_IS_TRUE(page->_workspaceSidebar.IsExpanded());
            page->_UpdateWorkspaceSidebarForWindowWidth(winrt::TerminalApp::implementation::WorkspaceSidebar::NarrowWindowThreshold - 1.0);
            VERIFY_IS_TRUE(sidebar->IsWindowNarrow());
            VERIFY_IS_TRUE(page->_workspaceSidebar.IsExpanded());
            VERIFY_IS_FALSE(sidebar->IsEffectivelyExpanded());
            VERIFY_IS_FALSE(page->_workspaceSidebar.FindName(L"ToggleButton").as<Button>().IsEnabled());
            VERIFY_IS_FALSE(toggleSidebar());

            page->_UpdateWorkspaceSidebarForWindowWidth(winrt::TerminalApp::implementation::WorkspaceSidebar::NarrowWindowThreshold);
            VERIFY_IS_FALSE(sidebar->IsWindowNarrow());
            VERIFY_IS_TRUE(sidebar->IsEffectivelyExpanded());
            VERIFY_IS_TRUE(page->_workspaceSidebar.FindName(L"ToggleButton").as<Button>().IsEnabled());

            page->SetFocusMode(true);
            VERIFY_ARE_EQUAL(Visibility::Collapsed, page->_workspaceSidebar.Visibility());
            VERIFY_ARE_EQUAL(Visibility::Collapsed, page->_workspaceSurfaceTabBar.Visibility());
            page->SetFocusMode(false);
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_workspaceSidebar.Visibility());
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_workspaceSurfaceTabBar.Visibility());

            page->SetShowTabsFullscreen(false);
            page->SetFullscreen(true);
            VERIFY_ARE_EQUAL(Visibility::Collapsed, page->_workspaceSidebar.Visibility());
            VERIFY_ARE_EQUAL(Visibility::Collapsed, page->_workspaceSurfaceTabBar.Visibility());
            page->SetShowTabsFullscreen(true);
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_workspaceSidebar.Visibility());
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_workspaceSurfaceTabBar.Visibility());
            page->SetFullscreen(false);
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_workspaceSidebar.Visibility());
        });
    }

    void TabTests::WorkspaceItemViewModelTracksTab()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto tab = page->_tabs.GetAt(0);
            const winrt::TerminalApp::WorkspaceItemViewModel item{ tab };

            VERIFY_ARE_EQUAL(tab.Title(), item.Title());
            VERIFY_IS_TRUE(std::wstring_view{ item.AutomationName() }.find(tab.Title()) != std::wstring_view::npos);
            VERIFY_ARE_EQUAL(tab.Icon(), item.Icon());
            VERIFY_IS_FALSE(item.IsPinned());

            tab.IsPinned(true);
            VERIFY_IS_TRUE(item.IsPinned());

            VERIFY_IS_TRUE(item.SwitchShortcutHintText().empty());
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            itemImpl->ShowSwitchShortcutHint(true);
            const std::wstring_view shortcut{ item.SwitchShortcutText() };
            const auto separator = shortcut.find_last_of(L'+');
            const std::wstring_view expectedHint = shortcut.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
            VERIFY_IS_TRUE(expectedHint == std::wstring_view{ item.SwitchShortcutHintText() });
            itemImpl->ShowSwitchShortcutHint(false);
            VERIFY_IS_TRUE(item.SwitchShortcutHintText().empty());
        });
    }

    void TabTests::WorkspaceGroupsMultipleTerminalSurfaces()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            const auto workspaceId = page->_WorkspaceItems().GetAt(0).WorkspaceId();

            page->_OpenNewTab(NewTerminalArgs{ 1 });

            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(1u, page->_WorkspaceItems().Size());
            const auto item = page->_WorkspaceItems().GetAt(0);
            VERIFY_ARE_EQUAL(workspaceId, item.WorkspaceId());
            VERIFY_ARE_EQUAL(2u, item.SurfaceCount());
            VERIFY_ARE_EQUAL(page->_tabs.GetAt(0), item.Surfaces().GetAt(0));
            VERIFY_ARE_EQUAL(page->_tabs.GetAt(1), item.Surfaces().GetAt(1));
            VERIFY_ARE_EQUAL(page->_tabs.GetAt(1), item.SourceTab());
            VERIFY_ARE_EQUAL(2u, page->_workspaceSurfaceTabs.Items().Size());
            VERIFY_ARE_EQUAL(page->_tabs.GetAt(1), page->_workspaceSurfaceTabs.SelectedItem().as<winrt::TerminalApp::Tab>());

            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            const auto firstSurfaceId = itemImpl->SurfaceIdForTab(page->_tabs.GetAt(0));
            const auto secondSurfaceId = itemImpl->SurfaceIdForTab(page->_tabs.GetAt(1));
            VERIFY_IS_TRUE(firstSurfaceId != winrt::guid{});
            VERIFY_IS_TRUE(secondSurfaceId != winrt::guid{});
            VERIFY_IS_TRUE(firstSurfaceId != secondSurfaceId);
            page->_workspaceFeatures->Workspaces().AssertInvariants(page->_tabs);

            const auto layout = page->GetWindowLayout();
            VERIFY_ARE_EQUAL(2u, layout.WorkspaceStateVersion());
            VERIFY_ARE_EQUAL(2u, layout.WorkspaceIds().Size());
            VERIFY_ARE_EQUAL(workspaceId, layout.WorkspaceIds().GetAt(0));
            VERIFY_ARE_EQUAL(workspaceId, layout.WorkspaceIds().GetAt(1));
            VERIFY_ARE_EQUAL(firstSurfaceId, layout.WorkspaceSurfaceIds().GetAt(0));
            VERIFY_ARE_EQUAL(secondSurfaceId, layout.WorkspaceSurfaceIds().GetAt(1));
            VERIFY_ARE_EQUAL(secondSurfaceId, layout.ActiveWorkspaceSurfaceIds().GetAt(0));

            page->_OpenNewWorkspace();
            VERIFY_ARE_EQUAL(3u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(2u, page->_WorkspaceItems().Size());
            VERIFY_ARE_EQUAL(1u, page->_WorkspaceItems().GetAt(1).SurfaceCount());
            page->_workspaceFeatures->Workspaces().AssertInvariants(page->_tabs);
        });
    }


    void TabTests::WorkspaceAutomationRequiresSurfaceId()
    {
        using namespace winrt::Windows::Data::Json;
        using winrt::TerminalApp::implementation::WorkspaceAutomationProtocol;
        const winrt::guid workspaceId{ L"01020304-0506-4708-890a-0b0c0d0e0f10" };
        const winrt::guid surfaceId{ L"11111111-2222-4333-8444-555555555555" };
        for (const auto method : { L"pane.focus", L"pane.split", L"notification.create", L"notification.markRead", L"terminal.sendInput", L"terminal.readContent" })
        {
            const auto message = MakeWorkspaceAutomationRequest(method, workspaceId, surfaceId, 0);
            const auto params = message.GetNamedObject(L"params");
            if (std::wstring_view{ method } == L"notification.create")
            {
                params.Insert(L"title", JsonValue::CreateStringValue(L"Build"));
                params.Insert(L"body", JsonValue::CreateStringValue(L"Finished"));
            }
            if (std::wstring_view{ method } == L"terminal.sendInput")
            {
                params.Insert(L"text", JsonValue::CreateStringValue(L"echo ready\r"));
            }
            const auto parse = [&]() { return WorkspaceAutomationProtocol::ParseRequest(winrt::to_string(message.Stringify())); };
            const auto valid = parse();
            VERIFY_IS_TRUE(static_cast<bool>(valid));
            VERIFY_ARE_EQUAL(surfaceId, valid.Request->SurfaceId.value());
            params.Remove(L"surfaceId");
            VERIFY_ARE_EQUAL(std::string{ "invalid_params" }, parse().ErrorCode);
            for (const auto value : { JsonValue::CreateNullValue(), JsonValue::CreateNumberValue(0),
                                      JsonValue::CreateStringValue(L"not-a-guid"),
                                      JsonValue::CreateStringValue(L"00000000-0000-0000-0000-000000000000") })
            {
                params.Insert(L"surfaceId", value);
                VERIFY_ARE_EQUAL(std::string{ "invalid_params" }, parse().ErrorCode);
            }
        }
        const auto markAll = MakeWorkspaceAutomationRequest(L"notification.markRead", workspaceId);
        VERIFY_IS_TRUE(static_cast<bool>(WorkspaceAutomationProtocol::ParseRequest(winrt::to_string(markAll.Stringify()))));
        const auto unpairedSurface = MakeWorkspaceAutomationRequest(L"notification.markRead", workspaceId, surfaceId);
        VERIFY_ARE_EQUAL(std::string{ "invalid_params" }, WorkspaceAutomationProtocol::ParseRequest(winrt::to_string(unpairedSurface.Stringify())).ErrorCode);
    }

    void TabTests::WorkspaceAutomationTargetsExactSurface()
    {
        using namespace winrt::Windows::Data::Json;
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            const auto firstTab = page->_tabs.GetAt(0);
            page->_OpenNewTab(NewTerminalArgs{ 1 });
            const auto secondTab = page->_GetFocusedTab();
            const auto item = page->_WorkspaceItems().GetAt(0);
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            const auto firstSurfaceId = itemImpl->SurfaceIdForTab(firstTab);
            const auto secondSurfaceId = itemImpl->SurfaceIdForTab(secondTab);
            const auto firstPane = page->_GetTabImpl(firstTab)->GetActivePane();
            const auto secondPane = page->_GetTabImpl(secondTab)->GetActivePane();
            VERIFY_ARE_EQUAL(0u, firstPane->Id().value());
            VERIFY_ARE_EQUAL(0u, secondPane->Id().value());
            const auto firstConnection = winrt::make_self<WorkspaceDirectoryConnection>();
            const auto secondConnection = winrt::make_self<WorkspaceDirectoryConnection>();
            firstPane->GetTerminalControl().Connection(*firstConnection);
            secondPane->GetTerminalControl().Connection(*secondConnection);
            for (const auto& pane : { firstPane, secondPane })
            {
                const auto core = _contentManager->TryLookupCore(pane->GetTerminalControl().ContentId()).Core();
                core.Initialize(600, 400, 1.0f);
            }
            firstConnection->TerminalOutput.raise(winrt_wstring_to_array_view(L"\x1b[2J\x1b[HFIRST-SURFACE"));
            secondConnection->TerminalOutput.raise(winrt_wstring_to_array_view(L"\x1b[2J\x1b[HSECOND-SURFACE"));
            const auto call = [&](const JsonObject& message) {
                return JsonObject::Parse(winrt::to_hstring(page->_workspaceFeatures->Automation()._HandleRequest(winrt::to_string(message.Stringify()))));
            };
            const auto send = MakeWorkspaceAutomationRequest(L"terminal.sendInput", item.WorkspaceId(), firstSurfaceId, 0);
            send.GetNamedObject(L"params").Insert(L"text", JsonValue::CreateStringValue(L"target-first\r"));
            const auto read = MakeWorkspaceAutomationRequest(L"terminal.readContent", item.WorkspaceId(), firstSurfaceId, 0);
            VERIFY_ARE_EQUAL(L"permission_denied", call(send).GetNamedObject(L"error").GetNamedString(L"code"));
            VERIFY_ARE_EQUAL(L"permission_denied", call(read).GetNamedObject(L"error").GetNamedString(L"code"));
            page->_currentWindowSettings().WorkspaceAutomationAllowInput(true);
            page->_currentWindowSettings().WorkspaceAutomationAllowContentRead(true);

            const auto list = call(MakeWorkspaceAutomationRequest(L"pane.list", item.WorkspaceId())).GetNamedObject(L"result");
            const auto panes = list.GetNamedArray(L"panes");
            VERIFY_ARE_EQUAL(2u, panes.Size());
            VERIFY_ARE_EQUAL(Utils::GuidToPlainString(firstSurfaceId), panes.GetObjectAt(0).GetNamedString(L"surfaceId"));
            VERIFY_ARE_EQUAL(Utils::GuidToPlainString(secondSurfaceId), panes.GetObjectAt(1).GetNamedString(L"surfaceId"));
            VERIFY_ARE_EQUAL(Utils::GuidToPlainString(secondSurfaceId), list.GetNamedString(L"activeSurfaceId"));
            VERIFY_IS_TRUE(call(send).GetNamedObject(L"result").GetNamedBoolean(L"accepted"));
            VERIFY_ARE_EQUAL(std::wstring{ L"target-first\r" }, firstConnection->Input);
            VERIFY_IS_TRUE(secondConnection->Input.empty());
            const auto result = call(read).GetNamedObject(L"result");
            VERIFY_ARE_EQUAL(Utils::GuidToPlainString(firstSurfaceId), result.GetNamedString(L"surfaceId"));
            const auto contentText = result.GetNamedString(L"text");
            const std::wstring_view text{ contentText };
            VERIFY_IS_TRUE(text.find(L"FIRST-SURFACE") != std::wstring_view::npos);
            VERIFY_IS_TRUE(text.find(L"SECOND-SURFACE") == std::wstring_view::npos);
            VERIFY_ARE_EQUAL(secondTab, page->_GetFocusedTab());

            // Marking one pane read must not clear a same-numbered pane in another surface.
            item.AddNotification(firstSurfaceId, 0, L"First", L"First notification");
            item.AddNotification(secondSurfaceId, 0, L"Second", L"Second notification");
            VERIFY_IS_TRUE(call(MakeWorkspaceAutomationRequest(L"notification.markRead", item.WorkspaceId(), firstSurfaceId, 0)).HasKey(L"result"));
            VERIFY_ARE_EQUAL(1u, item.UnreadCount());
            VERIFY_ARE_EQUAL(secondSurfaceId, item.LatestUnreadNotification().SurfaceId());

            VERIFY_IS_TRUE(call(MakeWorkspaceAutomationRequest(L"pane.focus", item.WorkspaceId(), firstSurfaceId, 0)).HasKey(L"result"));
            VERIFY_ARE_EQUAL(firstTab, page->_GetFocusedTab());
            page->_OpenNewWorkspace();
            const auto otherItem = page->_WorkspaceItems().GetAt(1);
            const auto otherSurfaceId = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(otherItem)->SurfaceIdForTab(otherItem.SourceTab());
            send.GetNamedObject(L"params").Insert(L"surfaceId", JsonValue::CreateStringValue(Utils::GuidToPlainString(otherSurfaceId)));
            VERIFY_ARE_EQUAL(L"pane_not_found", call(send).GetNamedObject(L"error").GetNamedString(L"code"));

            firstTab.Close();
            VERIFY_ARE_EQUAL(1u, item.SurfaceCount());
            send.GetNamedObject(L"params").Insert(L"surfaceId", JsonValue::CreateStringValue(Utils::GuidToPlainString(firstSurfaceId)));
            VERIFY_ARE_EQUAL(L"pane_not_found", call(send).GetNamedObject(L"error").GetNamedString(L"code"));
            VERIFY_ARE_EQUAL(L"pane_not_found", call(read).GetNamedObject(L"error").GetNamedString(L"code"));
            VERIFY_IS_TRUE(secondConnection->Input.empty(), L"A removed surface must never fall back to the remaining surface.");
        });
    }

    void TabTests::WorkspaceNamesSurviveSurfaceChangesAndRestore()
    {
        auto page = _commonSetup();
        WindowLayout layout{ nullptr };
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            const auto firstTab = page->_GetFocusedTab();
            page->_OpenNewTab(NewTerminalArgs{ 1 });
            const auto secondTab = page->_GetFocusedTab();
            const auto item = page->_WorkspaceItems().GetAt(0);
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            itemImpl->CustomName(L"Project Alpha");
            page->FocusTab(firstTab);
            VERIFY_ARE_EQUAL(L"Project Alpha", item.Title());
            page->FocusTab(secondTab);
            page->_GetTabImpl(secondTab)->SetTabText(L"Terminal Beta");
            VERIFY_ARE_EQUAL(L"Project Alpha", item.Title());
            VERIFY_IS_TRUE(itemImpl->MoveSurface(secondTab, 0));
            page->_SynchronizeFlatTabOrder();
            firstTab.Close();
            VERIFY_ARE_EQUAL(L"Project Alpha", item.Title());
            itemImpl->CustomName(L"");
            VERIFY_ARE_EQUAL(L"Terminal Beta", item.Title());
            itemImpl->CustomName(L"Project Alpha");
            page->_OpenNewWorkspace();
            const auto otherItem = page->_WorkspaceItems().GetAt(1);
            winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(otherItem)->CustomName(L"Project Gamma");
            page->_ToggleWorkspacePin(otherItem.SourceTab());
            layout = WindowLayout::FromJson(WindowLayout::ToJson(page->GetWindowLayout()));
            VERIFY_ARE_EQUAL(L"Project Gamma", layout.WorkspaceNames().GetAt(0));
            VERIFY_ARE_EQUAL(L"Project Alpha", layout.WorkspaceNames().GetAt(1));
            // Window.Content still owns the page. Release our test-thread
            // reference now so replacing Window.Content tears it down on the
            // UI thread, where its XAML timers must be stopped.
            page = nullptr;
        });

        const auto restoredPage = _commonSetup();
        const auto restored = std::make_shared<bool>(false);
        TestOnUIThread([&]() {
            restoredPage->_SidebarController().Enabled = true;
            restoredPage->_tabs.GetAt(0).Close();
            restoredPage->SetStartupWorkspaceState(layout.WorkspaceStateVersion(), wil::to_vector(layout.WorkspaceIds()),
                                                    wil::to_vector(layout.PinnedWorkspaceIndices()), {},
                                                    wil::to_vector(layout.WorkspaceSurfaceIds()), wil::to_vector(layout.ActiveWorkspaceSurfaceIds()),
                                                    wil::to_vector(layout.WorkspaceNames()));
            restoredPage->ProcessStartupActions(wil::to_vector(layout.TabLayout()), {}, {}, [restored]() { *restored = true; });
        });
        bool finished{};
        for (uint32_t attempt = 0; attempt < 500 && !finished; ++attempt)
        {
            TestOnUIThread([&]() { finished = *restored && !restoredPage->_SessionRestore().Restoring; });
            if (!finished)
            {
                Sleep(10);
            }
        }
        VERIFY_IS_TRUE(finished);
        TestOnUIThread([&]() {
            VERIFY_ARE_EQUAL(2u, restoredPage->_WorkspaceItems().Size());
            VERIFY_ARE_EQUAL(L"Project Gamma", restoredPage->_WorkspaceItems().GetAt(0).Title());
            VERIFY_ARE_EQUAL(L"Project Alpha", restoredPage->_WorkspaceItems().GetAt(1).Title());
            const auto alpha = restoredPage->_WorkspaceItems().GetAt(1);
            VERIFY_ARE_EQUAL(L"Terminal Beta", alpha.SourceTab().Title());
            restoredPage->_OpenNewWorkspaceSurface(alpha);
            VERIFY_ARE_EQUAL(L"Project Alpha", alpha.Title());
        });
    }

    void TabTests::WorkspaceSnapshotReopensInOwnWorkspace()
    {
        const auto page = _commonSetup();
        const winrt::guid snapshotId{ L"10101010-2020-4030-8040-505050505050" };
        WorkspaceSnapshot snapshot{};
        winrt::guid unrelatedId{};
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            const auto firstTab = page->_GetFocusedTab();
            page->_OpenNewTab(NewTerminalArgs{ 1 });
            const auto secondTab = page->_GetFocusedTab();
            const auto item = page->_WorkspaceItems().GetAt(0);
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            itemImpl->CustomName(L"Saved Project Alpha");
            page->_ToggleWorkspacePin(firstTab);
            std::vector<ActionAndArgs> actions;
            std::vector<winrt::guid> surfaceIds;
            for (const auto& tab : item.Surfaces())
            {
                const auto tabActions = page->_GetTabImpl(tab)->BuildStartupActions(winrt::TerminalApp::BuildStartupKind::Snapshot);
                actions.insert(actions.end(), tabActions.begin(), tabActions.end());
                surfaceIds.emplace_back(itemImpl->SurfaceIdForTab(tab));
            }
            snapshot.Version(WorkspaceSnapshot::CurrentSchemaVersion());
            snapshot.Id(snapshotId);
            snapshot.WorkspaceId(item.WorkspaceId());
            snapshot.Name(item.Title());
            snapshot.CreatedAt(1);
            snapshot.UpdatedAt(1);
            snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)));
            snapshot.SurfaceIds(winrt::single_threaded_vector<winrt::guid>(std::move(surfaceIds)));
            snapshot.ActiveSurfaceId(itemImpl->SurfaceIdForTab(firstTab));
            snapshot.Pinned(true);
            snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>());
            page->_OpenNewWorkspace();
            unrelatedId = page->_WorkspaceItemForTab(page->_GetFocusedTab()).WorkspaceId();
            firstTab.Close();
            secondTab.Close();
            VERIFY_ARE_EQUAL(1u, page->_WorkspaceItems().Size());
        });
        const auto state = ApplicationState::SharedInstance();
        state.RemoveWorkspaceSnapshot(snapshotId);
        const auto cleanup = wil::scope_exit([&]() { state.RemoveWorkspaceSnapshot(snapshotId); });
        VERIFY_IS_TRUE(state.SaveWorkspaceSnapshot(snapshot));
        TestOnUIThread([&]() { page->OpenWorkspaceSnapshot(snapshotId, false); });
        bool opened{};
        for (uint32_t attempt = 0; attempt < 500 && !opened; ++attempt)
        {
            TestOnUIThread([&]() {
                const auto index = page->_FindWorkspaceIndex(snapshot.WorkspaceId());
                opened = index && page->_WorkspaceItems().GetAt(index.value()).SurfaceCount() == 2 &&
                         !page->_SessionRestore().Restoring && !page->_SessionRestore().SnapshotReplayInFlight;
            });
            if (!opened)
            {
                Sleep(10);
            }
        }
        VERIFY_IS_TRUE(opened);
        TestOnUIThread([&]() {
            VERIFY_ARE_EQUAL(2u, page->_WorkspaceItems().Size());
            const auto item = page->_WorkspaceItems().GetAt(page->_FindWorkspaceIndex(snapshot.WorkspaceId()).value());
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            VERIFY_ARE_EQUAL(L"Saved Project Alpha", item.Title());
            VERIFY_IS_TRUE(item.IsPinned());
            VERIFY_ARE_EQUAL(snapshotId, itemImpl->SourceSnapshotId().value());
            VERIFY_ARE_EQUAL(snapshot.Revision(), itemImpl->SourceSnapshotRevision());
            VERIFY_ARE_EQUAL(snapshot.ActiveSurfaceId(), itemImpl->SurfaceIdForTab(page->_GetFocusedTab()));
            VERIFY_ARE_EQUAL(1u, page->_WorkspaceItems().GetAt(page->_FindWorkspaceIndex(unrelatedId).value()).SurfaceCount());
            page->_workspaceFeatures->Workspaces().AssertInvariants(page->_tabs);
        });
    }

    void TabTests::WorkspaceSurfaceShortcutActions()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            page->_OpenNewTab(NewTerminalArgs{ 1 });
            page->_OpenNewWorkspace();
            page->_OpenNewTab(NewTerminalArgs{ 2 });

            VERIFY_ARE_EQUAL(2u, page->_WorkspaceItems().Size());
            const auto workspace = page->_WorkspaceItems().GetAt(1);
            VERIFY_ARE_EQUAL(2u, workspace.SurfaceCount());

            const auto switchSurface = [&](const uint32_t index) {
                const SwitchToWorkspaceSurfaceArgs shortcutArgs{ index };
                return page->_actionDispatch->DoAction(ActionAndArgs{ ShortcutAction::SwitchToWorkspaceSurface, shortcutArgs });
            };
            const auto switchConclusion = [&]() {
                return page->_actionDispatch->DoAction(ActionAndArgs{ ShortcutAction::SwitchToWorkspaceConclusion, nullptr });
            };

            VERIFY_IS_TRUE(switchConclusion());
            VERIFY_IS_TRUE(page->_workspaceToolTabSelected);
            VERIFY_ARE_EQUAL(Visibility::Visible, page->_conclusionSidebar.Visibility());

            VERIFY_IS_TRUE(switchSurface(0));
            VERIFY_IS_FALSE(page->_workspaceToolTabSelected);
            VERIFY_ARE_EQUAL(workspace.Surfaces().GetAt(0), page->_GetFocusedTab());
            VERIFY_IS_TRUE(switchSurface(1));
            VERIFY_ARE_EQUAL(workspace.Surfaces().GetAt(1), page->_GetFocusedTab());

            VERIFY_IS_TRUE(switchConclusion());
            VERIFY_IS_TRUE(page->_workspaceToolTabSelected);
            VERIFY_IS_TRUE(switchSurface(1));
            VERIFY_IS_FALSE(page->_workspaceToolTabSelected);
            VERIFY_ARE_EQUAL(workspace.Surfaces().GetAt(1), page->_GetFocusedTab());

            const auto selectedTab = page->_GetFocusedTab();
            VERIFY_IS_TRUE(switchSurface(8));
            VERIFY_ARE_EQUAL(selectedTab, page->_GetFocusedTab());

            page->_SidebarController().Enabled = false;
            VERIFY_IS_FALSE(switchSurface(0));
            VERIFY_IS_FALSE(switchConclusion());
            VERIFY_ARE_EQUAL(selectedTab, page->_GetFocusedTab());
            page->_SidebarController().Enabled = true;

            const auto workspaceCount = page->_WorkspaceItems().Size();
            const auto surfaceCount = workspace.SurfaceCount();
            const NewTabArgs newTabArgs{ NewTerminalArgs{} };
            VERIFY_IS_TRUE(page->_actionDispatch->DoAction(ActionAndArgs{ ShortcutAction::NewTab, newTabArgs }));
            VERIFY_ARE_EQUAL(workspaceCount, page->_WorkspaceItems().Size());
            VERIFY_ARE_EQUAL(surfaceCount + 1, workspace.SurfaceCount());

            VERIFY_IS_TRUE(page->_actionDispatch->DoAction(ActionAndArgs{ ShortcutAction::NewWorkspace, nullptr }));
            VERIFY_ARE_EQUAL(workspaceCount + 1, page->_WorkspaceItems().Size());
            const auto newWorkspace = page->_WorkspaceItemForTab(page->_GetFocusedTab());
            VERIFY_IS_NOT_NULL(newWorkspace);
            VERIFY_ARE_EQUAL(1u, newWorkspace.SurfaceCount());
            VERIFY_IS_TRUE(workspace.WorkspaceId() != newWorkspace.WorkspaceId());
        });
    }

    void TabTests::WorkspaceEnvironmentUsesAuthoritativeRoutingMetadata()
    {
        const winrt::guid workspaceId{ 0x12345678, 0x1234, 0x4abc, { 0x8d, 0xef, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc } };
        const std::wstring pipeName{ LR"(\\.\pipe\LOCAL\cmux-terminal-123-42)" };

        std::unordered_map<winrt::hstring, winrt::hstring> profileValues{
            { L"Path", L"C:\\tools" },
            { L"WT_SESSION", L"spoofed-session" },
            { L"wt_profile_id", L"spoofed-profile" },
            { L"wt_workspace_id", L"spoofed-id" },
            { L"WT_WORKSPACE_PIPE", L"spoofed-pipe" },
            { L"Wt_Workspace_Window_Id", L"999" },
            { L"wslenv", L"FOO/p:WT_WORKSPACE_ID/w:wt_session/l:BAR" },
        };
        const auto profileEnvironment = winrt::single_threaded_map(std::move(profileValues)).GetView();

        const auto environment = winrt::TerminalApp::implementation::TerminalPage::_BuildWorkspaceEnvironment(
            profileEnvironment,
            workspaceId,
            pipeName,
            42);

        VERIFY_ARE_EQUAL(L"C:\\tools", environment.Lookup(L"Path"));
        VERIFY_IS_FALSE(environment.HasKey(L"WT_SESSION"));
        VERIFY_IS_FALSE(environment.HasKey(L"wt_profile_id"));
        VERIFY_ARE_EQUAL(::Microsoft::Console::Utils::GuidToPlainString(workspaceId), environment.Lookup(L"WT_WORKSPACE_ID"));
        VERIFY_ARE_EQUAL(pipeName, std::wstring{ environment.Lookup(L"WT_WORKSPACE_PIPE") });
        VERIFY_ARE_EQUAL(L"42", environment.Lookup(L"WT_WORKSPACE_WINDOW_ID"));
        VERIFY_ARE_EQUAL(L"FOO/p:BAR", environment.Lookup(L"WSLENV"));
        VERIFY_IS_FALSE(environment.HasKey(L"wt_workspace_id"));
        VERIFY_IS_FALSE(environment.HasKey(L"Wt_Workspace_Window_Id"));

        const auto environmentWithoutPipe = winrt::TerminalApp::implementation::TerminalPage::_BuildWorkspaceEnvironment(
            profileEnvironment,
            workspaceId,
            {},
            42);
        VERIFY_IS_TRUE(environmentWithoutPipe.HasKey(L"WT_WORKSPACE_PIPE"));
        VERIFY_IS_TRUE(environmentWithoutPipe.Lookup(L"WT_WORKSPACE_PIPE").empty());
    }

    void TabTests::WorkspaceStatusPillTracksStructuredState()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto item = page->_WorkspaceItems().GetAt(0);
            const auto status = item.SourceTab().TabStatus();
            status.IsConnectionClosed(false);
            VERIFY_IS_TRUE(item.StatusText().empty());

            status.ProgressState(1);
            status.ProgressValue(42);
            VERIFY_IS_TRUE(std::wstring_view{ item.StatusText() }.find(L"42") != std::wstring_view::npos);

            status.ProgressState(4);
            VERIFY_IS_FALSE(item.StatusText().empty());

            status.ProgressState(0);
            VERIFY_IS_TRUE(item.StatusText().empty());

            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            itemImpl->SetAutomationProgress(1, 73);
            VERIFY_IS_TRUE(std::wstring_view{ item.StatusText() }.find(L"73") != std::wstring_view::npos);
            itemImpl->SetAutomationStatus(L"Waiting");
            VERIFY_ARE_EQUAL(L"Waiting", item.StatusText());
            VERIFY_IS_TRUE(std::wstring_view{ item.AutomationName() }.find(L"Waiting") != std::wstring_view::npos);

            item.AddNotification(itemImpl->SurfaceIdForTab(item.SourceTab()), 7, L"Agent", L"Input required");
            VERIFY_IS_FALSE(item.StatusText().empty());
            VERIFY_ARE_NOT_EQUAL(L"Waiting", item.StatusText());
            item.MarkAllNotificationsRead();
            VERIFY_ARE_EQUAL(L"Waiting", item.StatusText());

            itemImpl->ClearAutomationStatus();
            VERIFY_IS_TRUE(std::wstring_view{ item.StatusText() }.find(L"73") != std::wstring_view::npos);
            itemImpl->ClearAutomationProgress();
            VERIFY_IS_TRUE(item.StatusText().empty());

            status.IsConnectionClosed(true);
            VERIFY_IS_FALSE(item.StatusText().empty());
        });
    }

    void TabTests::WorkspaceWorkingDirectoryDisplayIsFolderName()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto item = page->_WorkspaceItems().GetAt(0);

            item.WorkingDirectory(L"F:\\RicherLife\\codes\\win_terminal\\src");
            VERIFY_ARE_EQUAL(L"src", item.WorkingDirectoryDisplay());

            item.WorkingDirectory(L"C:\\repo");
            VERIFY_ARE_EQUAL(L"repo", item.WorkingDirectoryDisplay());

            item.WorkingDirectory(L"C:\\");
            VERIFY_ARE_EQUAL(L"C:\\", item.WorkingDirectoryDisplay());

            item.WorkingDirectory(L"C:\\projects\\folder with spaces\\");
            VERIFY_ARE_EQUAL(L"folder with spaces", item.WorkingDirectoryDisplay());

            item.WorkingDirectory(L"/home/user/project/");
            VERIFY_ARE_EQUAL(L"project", item.WorkingDirectoryDisplay());

            item.WorkingDirectory(L"/");
            VERIFY_ARE_EQUAL(L"/", item.WorkingDirectoryDisplay());

            item.WorkingDirectory(L"");
            VERIFY_IS_TRUE(item.WorkingDirectoryDisplay().empty());
        });
    }

    void TabTests::WorkspaceWorkingDirectoryTracksFirstSurface()
    {
        const auto page = _commonSetup();
        winrt::TerminalApp::Tab firstTab{ nullptr };
        winrt::TerminalApp::Tab secondTab{ nullptr };
        winrt::TerminalApp::WorkspaceItemViewModel item{ nullptr };
        winrt::com_ptr<WorkspaceDirectoryConnection> firstConnection;
        winrt::com_ptr<WorkspaceDirectoryConnection> secondConnection;
        bool displayChanged = false;
        winrt::Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker directoryChanges;

        TestOnUIThread([&]() {
            firstTab = page->_tabs.GetAt(0);
            firstConnection = winrt::make_self<WorkspaceDirectoryConnection>();
            page->_GetTabImpl(firstTab)->GetActiveTerminalControl().Connection(*firstConnection);
            page->_OpenNewTab(NewTerminalArgs{ 1 });
            secondTab = page->_tabs.GetAt(1);
            secondConnection = winrt::make_self<WorkspaceDirectoryConnection>();
            page->_GetTabImpl(secondTab)->GetActiveTerminalControl().Connection(*secondConnection);

            // These controls are not attached to a visible XAML tree in this test.
            // Initialize their terminal parsers before reporting shell output.
            for (const auto& tab : { firstTab, secondTab })
            {
                const auto control = page->_GetTabImpl(tab)->GetActiveTerminalControl();
                const auto core = _contentManager->TryLookupCore(control.ContentId()).Core();
                core.Initialize(600, 400, 1.0f); // Already initialized controls return false.
            }

            item = winrt::TerminalApp::WorkspaceItemViewModel{ firstTab };
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            itemImpl->AddSurface(secondTab);
            itemImpl->SetActiveTab(secondTab);
            firstConnection->ReportDirectory(L"C:\\first-workspace");
            secondConnection->ReportDirectory(L"C:\\second-workspace");
        });

        TestOnUIThread([&]() {
            VERIFY_ARE_EQUAL(secondTab, item.SourceTab());
            VERIFY_ARE_EQUAL(L"C:\\first-workspace", item.WorkingDirectory());
            VERIFY_ARE_EQUAL(L"first-workspace", item.WorkingDirectoryDisplay());
            directoryChanges = item.PropertyChanged(winrt::auto_revoke, [&](const auto&, const auto& args) {
                if (args.PropertyName() == L"WorkingDirectoryDisplay")
                {
                    displayChanged = true;
                }
            });
            firstConnection->ReportDirectory(L"C:\\first-workspace\\updated");
        });

        TestOnUIThread([&]() {
            VERIFY_ARE_EQUAL(L"C:\\first-workspace\\updated", item.WorkingDirectory());
            VERIFY_ARE_EQUAL(L"updated", item.WorkingDirectoryDisplay());
            VERIFY_IS_TRUE(displayChanged, L"Background changes in the leftmost terminal must update the sidebar binding.");
            displayChanged = false;
            secondConnection->ReportDirectory(L"C:\\second-workspace\\active");
        });

        TestOnUIThread([&]() {
            VERIFY_ARE_EQUAL(L"updated", item.WorkingDirectoryDisplay());
            VERIFY_IS_FALSE(displayChanged, L"Changing another terminal must leave the workspace directory anchored.");
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            VERIFY_IS_TRUE(itemImpl->MoveSurface(secondTab, 0));
            VERIFY_ARE_EQUAL(L"C:\\second-workspace\\active", item.WorkingDirectory());
            VERIFY_ARE_EQUAL(L"active", item.WorkingDirectoryDisplay());
            VERIFY_IS_TRUE(displayChanged);
            itemImpl->SetActiveTab(firstTab);
            VERIFY_ARE_EQUAL(L"active", item.WorkingDirectoryDisplay());
            VERIFY_IS_TRUE(itemImpl->RemoveSurface(secondTab));
            VERIFY_ARE_EQUAL(L"updated", item.WorkingDirectoryDisplay());
            displayChanged = false;
            secondConnection->ReportDirectory(L"C:\\removed-surface");
        });

        TestOnUIThread([&]() {
            VERIFY_ARE_EQUAL(L"updated", item.WorkingDirectoryDisplay());
            VERIFY_IS_FALSE(displayChanged, L"A removed surface must no longer update directory metadata.");
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            VERIFY_IS_TRUE(itemImpl->RemoveSurface(firstTab));
            VERIFY_IS_TRUE(item.WorkingDirectory().empty());
            VERIFY_IS_TRUE(item.WorkingDirectoryDisplay().empty());
            itemImpl->AddSurface(secondTab);
            VERIFY_ARE_EQUAL(L"removed-surface", item.WorkingDirectoryDisplay());
        });
    }

    void TabTests::WorkspaceWorkingDirectoryNewTerminalFollowsActiveSurface()
    {
        const auto page = _commonSetup();
        const winrt::hstring firstDirectory{ std::filesystem::temp_directory_path().wstring() };
        const winrt::hstring secondDirectory{ std::filesystem::current_path().wstring() };
        VERIFY_ARE_NOT_EQUAL(firstDirectory, secondDirectory);
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            const auto reportDirectory = [&](const winrt::TerminalApp::Tab& tab, const winrt::hstring& directory) {
                const auto control = page->_GetTabImpl(tab)->GetActiveTerminalControl();
                const auto connection = winrt::make_self<WorkspaceDirectoryConnection>();
                control.Connection(*connection);
                _contentManager->TryLookupCore(control.ContentId()).Core().Initialize(600, 400, 1.0f);
                connection->ReportDirectory(directory);
            };
            const auto firstTab = page->_GetFocusedTab();
            const auto item = page->_WorkspaceItemForTab(firstTab);
            reportDirectory(firstTab, firstDirectory);

            NewTerminalArgs reusableArgs{ 1 };
            reusableArgs.TabTitle(L"Inherited directory");
            reusableArgs.AppendCommandLine(true);
            VERIFY_IS_TRUE(reusableArgs.Copy().as<NewTerminalArgs>().AppendCommandLine());
            VERIFY_SUCCEEDED(page->_OpenNewTab(reusableArgs));
            const auto secondTab = page->_GetFocusedTab();
            auto control = page->_GetTabImpl(secondTab)->GetActiveTerminalControl();
            VERIFY_ARE_EQUAL(firstDirectory, control.WorkingDirectory());
            VERIFY_ARE_EQUAL(page->_settings.GetProfileForArgs(reusableArgs).Guid(), page->_GetTabImpl(secondTab)->GetFocusedProfile().Guid());

            reportDirectory(secondTab, secondDirectory);
            VERIFY_SUCCEEDED(page->_OpenNewTab(reusableArgs));
            control = page->_GetActiveControl();
            VERIFY_ARE_EQUAL(secondDirectory, control.WorkingDirectory());
            VERIFY_IS_TRUE(reusableArgs.StartingDirectory().empty(), L"Reusing the shortcut must not retain an earlier directory.");

            page->FocusTab(firstTab);
            VERIFY_SUCCEEDED(page->_OpenNewTab(nullptr));
            VERIFY_ARE_EQUAL(firstDirectory, page->_GetActiveControl().WorkingDirectory());
            VERIFY_ARE_EQUAL(4u, item.SurfaceCount());

            page->_OpenNewWorkspace();
            reportDirectory(page->_GetFocusedTab(), secondDirectory);
            page->_OpenNewWorkspaceSurface(item);
            VERIFY_ARE_EQUAL(firstDirectory, page->_GetActiveControl().WorkingDirectory());
            VERIFY_ARE_EQUAL(item.WorkspaceId(), page->_WorkspaceIdForTab(page->_GetFocusedTab()));
            VERIFY_ARE_EQUAL(5u, item.SurfaceCount());
            VERIFY_ARE_EQUAL(2u, page->_WorkspaceItems().Size());
        });
    }

    void TabTests::WorkspaceWorkingDirectoryNewTerminalFollowsActivePane()
    {
        const auto page = _commonSetup();
        const winrt::hstring firstDirectory{ std::filesystem::temp_directory_path().wstring() };
        const winrt::hstring secondDirectory{ std::filesystem::current_path().wstring() };
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            const auto tab = page->_GetFocusedTab();
            const auto tabImpl = page->_GetTabImpl(tab);
            const auto workspaceId = page->_WorkspaceIdForTab(tab);
            const auto firstPaneId = tabImpl->GetActivePane()->Id().value();
            const auto firstControl = tabImpl->GetActiveTerminalControl();
            const auto firstConnection = winrt::make_self<WorkspaceDirectoryConnection>();
            firstControl.Connection(*firstConnection);
            _contentManager->TryLookupCore(firstControl.ContentId()).Core().Initialize(600, 400, 1.0f);
            firstConnection->ReportDirectory(firstDirectory);

            const auto secondConnection = winrt::make_self<WorkspaceDirectoryConnection>();
            const auto newPane = page->_MakePane(NewTerminalArgs{ 1 }, nullptr, *secondConnection, false, workspaceId);
            page->_SplitPane(nullptr, SplitDirection::Right, 0.5f, newPane, workspaceId);
            VERIFY_ARE_EQUAL(2, tabImpl->GetLeafPaneCount());
            const auto secondControl = tabImpl->GetActiveTerminalControl();
            _contentManager->TryLookupCore(secondControl.ContentId()).Core().Initialize(600, 400, 1.0f);
            secondConnection->ReportDirectory(secondDirectory);

            VERIFY_SUCCEEDED(page->_OpenNewTab(nullptr));
            VERIFY_ARE_EQUAL(secondDirectory, page->_GetActiveControl().WorkingDirectory());
            VERIFY_ARE_EQUAL(page->_settings.GetProfileForArgs(NewTerminalArgs{}).Guid(), page->_GetFocusedTabImpl()->GetFocusedProfile().Guid());

            page->FocusTab(tab);
            VERIFY_IS_TRUE(tabImpl->FocusPane(firstPaneId));
            VERIFY_SUCCEEDED(page->_OpenNewTab(nullptr));
            VERIFY_ARE_EQUAL(firstDirectory, page->_GetActiveControl().WorkingDirectory());
        });
    }

    void TabTests::WorkspaceWorkingDirectoryNewTerminalHonorsLaunchContext()
    {
        const auto page = _commonSetup();
        const winrt::hstring directory{ std::filesystem::temp_directory_path().wstring() };
        const winrt::hstring explicitDirectory{ std::filesystem::current_path().wstring() };
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            const auto sourceTab = page->_GetFocusedTab();
            const auto control = page->_GetActiveControl();
            const auto profileDirectory = control.WorkingDirectory();
            const auto connection = winrt::make_self<WorkspaceDirectoryConnection>();
            control.Connection(*connection);
            _contentManager->TryLookupCore(control.ContentId()).Core().Initialize(600, 400, 1.0f);
            connection->ReportDirectory(directory);

            NewTerminalArgs explicitArgs{};
            explicitArgs.StartingDirectory(explicitDirectory);
            VERIFY_SUCCEEDED(page->_OpenNewTab(explicitArgs));
            VERIFY_ARE_EQUAL(explicitDirectory, page->_GetActiveControl().WorkingDirectory());
            VERIFY_ARE_EQUAL(explicitDirectory, explicitArgs.StartingDirectory());

            page->FocusTab(sourceTab);
            GUID missingDirectoryId{};
            VERIFY_SUCCEEDED(CoCreateGuid(&missingDirectoryId));
            const auto missingDirectory = std::filesystem::path{ directory.c_str() } / L"workspace-directory-missing" / Utils::GuidToPlainString(missingDirectoryId);
            connection->ReportDirectory(missingDirectory.wstring());
            VERIFY_SUCCEEDED(page->_OpenNewTab(nullptr));
            VERIFY_ARE_EQUAL(profileDirectory, page->_GetActiveControl().WorkingDirectory());

            connection->ReportDirectory(directory);
            page->FocusTab(sourceTab);
            page->_OpenNewWorkspace();
            VERIFY_ARE_EQUAL(profileDirectory, page->_GetActiveControl().WorkingDirectory());

            page->FocusTab(sourceTab);
            page->_SidebarController().Enabled = false;
            VERIFY_SUCCEEDED(page->_OpenNewTab(nullptr));
            VERIFY_ARE_EQUAL(profileDirectory, page->_GetActiveControl().WorkingDirectory());

            page->FocusTab(sourceTab);
            page->_SidebarController().Enabled = true;
            page->_SessionRestore().Restoring = true;
            const auto resetRestoring = wil::scope_exit([&]() noexcept { page->_SessionRestore().Restoring = false; });
            VERIFY_SUCCEEDED(page->_OpenNewTab(nullptr));
            VERIFY_ARE_EQUAL(profileDirectory, page->_GetActiveControl().WorkingDirectory());
        });
    }

    void TabTests::WorkspaceVimModeInputRouting()
    {
        const CascadiaSettings settings{ LR"({
            "defaultProfile":"{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "actions":[{"command":"enterVimMode","keys":"f6"}],
            "profiles":{
                "defaults":{"experimental.vimMode":true,"closeOnExit":"never"},
                "list":[
                    {"name":"vim","guid":"{6239a42c-1111-49a3-80bd-e8fdd045185c}"},
                    {"name":"raw","guid":"{6239a42c-2222-49a3-80bd-e8fdd045185c}","experimental.vimMode":false}
                ]
            }
        })", {} };
        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page;
        _initializeTerminalPage(page, settings);
        TestOnUIThread([&]() {
            const auto firstTab = page->_GetFocusedTab();
            const auto control = page->_GetActiveControl();
            const auto core = _contentManager->TryLookupCore(control.ContentId()).Core();
            const auto connection = winrt::make_self<WorkspaceDirectoryConnection>();
            control.Connection(*connection);
            core.Initialize(600, 400, 1.0f);
            connection->TerminalOutput.raise(winrt_wstring_to_array_view(L"abcdef\r"));
            VERIFY_IS_TRUE(core.VimModeAvailable());

            page->_OpenNewTab(NewTerminalArgs{ 1 });
            const auto otherCore = _contentManager->TryLookupCore(page->_GetActiveControl().ContentId()).Core();
            otherCore.Initialize(600, 400, 1.0f);
            VERIFY_IS_FALSE(otherCore.VimModeAvailable());
            page->FocusTab(firstTab);

            auto broadcasts = 0;
            const auto keyToken = control.KeySent([&](auto&&, auto&&) { ++broadcasts; });
            const auto removeHandler = wil::scope_exit([&]() noexcept { control.KeySent(keyToken); });
            const auto press = [&](const uint32_t key) {
                const auto scanCode = static_cast<uint8_t>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
                VERIFY_IS_TRUE(control.OnDirectKeyEvent(key, scanCode, true));
                VERIFY_IS_TRUE(control.OnDirectKeyEvent(key, scanCode, false));
            };
            // Windows generates a character event for an unhandled Escape
            // key-down. Simulate it only after proving entry did not eat it.
            VERIFY_IS_FALSE(control.OnDirectKeyEvent(VK_ESCAPE, 1, true));
            control.RawWriteChar(L'\x1b', 1, {});
            control.OnDirectKeyEvent(VK_ESCAPE, 1, false);
            VERIFY_ARE_EQUAL(std::wstring(L"\x1b"), connection->Input);
            VERIFY_IS_TRUE(core.VimModeState() == winrt::Microsoft::Terminal::Control::VimMode::Insert);
            VERIFY_ARE_EQUAL(2, broadcasts, L"Insert must forward Escape through the original input route.");
            broadcasts = 0;
            connection->Input.clear();
            press(VK_F6);
            press('L');
            press('V');
            press('L');
            VERIFY_IS_TRUE(core.VimModeState() == winrt::Microsoft::Terminal::Control::VimMode::Visual);
            press(VK_ESCAPE);
            press('I');
            VERIFY_IS_TRUE(core.VimModeState() == winrt::Microsoft::Terminal::Control::VimMode::Insert);
            VERIFY_ARE_EQUAL(0, broadcasts);
            VERIFY_IS_TRUE(connection->Input.empty());
            VERIFY_IS_TRUE(otherCore.VimModeState() == winrt::Microsoft::Terminal::Control::VimMode::Insert);
            control.RawWriteChar(L'x', 0, {});
            VERIFY_ARE_EQUAL(std::wstring(L"x"), connection->Input);

            press(VK_F6);
            const auto insertScan = static_cast<uint8_t>(MapVirtualKeyW('I', MAPVK_VK_TO_VSC));
            VERIFY_IS_TRUE(control.OnDirectKeyEvent('I', insertScan, true));
            settings.ActiveProfiles().GetAt(0).VimModeEnabled(false);
            page->SetSettings(settings, true);
            VERIFY_IS_FALSE(core.VimModeAvailable());
            VERIFY_IS_TRUE(control.OnDirectKeyEvent('I', insertScan, false));
            VERIFY_ARE_EQUAL(0, broadcasts, L"Disabling Vim must not leak the release of an already consumed key.");
            control.OnDirectKeyEvent('I', insertScan, true);
            VERIFY_ARE_EQUAL(1, broadcasts, L"Fresh input must pass through after disabling Vim.");
            control.OnDirectKeyEvent('I', insertScan, false);
        });
    }

    void TabTests::WorkspaceClosingTerminalsKeepsSelectionValid()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            page->_SidebarController().Enabled = true;
            const auto firstTab = page->_tabs.GetAt(0);
            page->_OpenNewWorkspace();
            const auto secondTab = page->_tabs.GetAt(1);
            VERIFY_ARE_EQUAL(secondTab, page->_GetFocusedTab());

            firstTab.Close();
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(1u, page->_WorkspaceItems().Size());
            VERIFY_ARE_EQUAL(secondTab, page->_GetFocusedTab());
            VERIFY_ARE_EQUAL(0u, page->_GetFocusedTabIndex().value());

            secondTab.Close();
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(0u, page->_WorkspaceItems().Size());
            VERIFY_IS_FALSE(page->_GetFocusedTabIndex().has_value());
            VERIFY_IS_NULL(page->_GetFocusedTab());
        });
    }

    void TabTests::WorkspaceGitBranchParsesStructuredOutput()
    {
        const auto result = winrt::TerminalApp::implementation::WorkspaceGitBranch::ParseOutputForTests(
            "F:/RicherLife/codes/win_terminal\r\nfeature/sidebar\r\n");
        VERIFY_IS_TRUE(result.IsRepository);
        VERIFY_ARE_EQUAL(L"F:/RicherLife/codes/win_terminal", result.RepositoryRoot);
        VERIFY_ARE_EQUAL(L"feature/sidebar", result.Branch);

        const auto invalid = winrt::TerminalApp::implementation::WorkspaceGitBranch::ParseOutputForTests("fatal: not a repository\r\n");
        VERIFY_IS_FALSE(invalid.IsRepository);
        VERIFY_IS_TRUE(invalid.Branch.empty());

        VERIFY_IS_FALSE(winrt::TerminalApp::implementation::WorkspaceGitBranch::IsSupportedDirectory(L"\\\\wsl.localhost\\Ubuntu\\home\\repo"));
    }

    void TabTests::WorkspaceListeningPortsProjectMetadata()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto item = page->_WorkspaceItems().GetAt(0);
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);

            itemImpl->UpdateListeningPorts({ 5173, 80, 65535, 3000, 5173 });
            VERIFY_ARE_EQUAL(L"80 · 3000 · 5173 · +1", item.ListeningPortsDisplay());
            VERIFY_IS_TRUE(std::wstring_view{ item.ListeningPortsTooltip() }.find(L"65535") != std::wstring_view::npos);
            VERIFY_IS_TRUE(std::wstring_view{ item.AutomationName() }.find(L"65535") != std::wstring_view::npos);
            VERIFY_ARE_EQUAL(static_cast<size_t>(4), itemImpl->ListeningPorts().size());

            itemImpl->UpdateListeningPorts({});
            VERIFY_IS_TRUE(item.ListeningPortsDisplay().empty());
            VERIFY_IS_TRUE(item.ListeningPortsTooltip().empty());
        });
    }

    void TabTests::WorkspaceAgentResumeRejectsUntrustedInput()
    {
        using namespace winrt::TerminalApp::implementation;

        VERIFY_IS_TRUE(WorkspaceAgentResume::IsValidCodexSessionId(L"12345678-1234-ABCD-9876-123456789ABC"));
        VERIFY_ARE_EQUAL(L"12345678-1234-abcd-9876-123456789abc",
                         WorkspaceAgentResume::NormalizeCodexSessionId(L"12345678-1234-ABCD-9876-123456789ABC"));
        VERIFY_IS_FALSE(WorkspaceAgentResume::IsValidCodexSessionId(L"session-name"));
        VERIFY_IS_FALSE(WorkspaceAgentResume::IsValidCodexSessionId(L"12345678-1234-1234-1234-123456789ab&"));

        const winrt::guid workspaceId{ 0x71c3b33d, 0x1a76, 0x4ea0, { 0x8d, 0x8d, 0x5b, 0x1c, 0x96, 0xf5, 0x4c, 0x43 } };
        VERIFY_IS_TRUE(WorkspaceAgentResume::IsSupported(WorkspaceAgentResume::CurrentVersion,
                                                         workspaceId,
                                                         L"codex",
                                                         L"12345678-1234-abcd-9876-123456789abc",
                                                         true));
        VERIFY_ARE_EQUAL(LR"("C:\Windows\System32\cmd.exe" /d /v:off /s /c ""C:\Tools\codex.cmd" resume 12345678-1234-abcd-9876-123456789abc")",
                         WorkspaceAgentResume::BuildCodexCommandline(L"12345678-1234-abcd-9876-123456789abc", L"C:\\Windows\\System32\\cmd.exe", L"C:\\Tools\\codex.cmd"));
        VERIFY_IS_TRUE(WorkspaceAgentResume::BuildCodexCommandline(L"invalid & calc", L"C:\\Windows\\System32\\cmd.exe", L"C:\\Tools\\codex.cmd").empty());
        VERIFY_IS_TRUE(WorkspaceAgentResume::BuildCodexCommandline(L"12345678-1234-abcd-9876-123456789abc", L"C:\\Windows\\System32\\cmd.exe", L"C:\\%PATH%\\codex.cmd").empty());
        VERIFY_IS_FALSE(WorkspaceAgentResume::IsSupported(WorkspaceAgentResume::CurrentVersion,
                                                          workspaceId,
                                                          L"custom",
                                                          L"12345678-1234-abcd-9876-123456789abc",
                                                          true));
        VERIFY_IS_FALSE(WorkspaceAgentResume::IsSupported(WorkspaceAgentResume::CurrentVersion,
                                                          workspaceId,
                                                          L"codex",
                                                          L"12345678-1234-abcd-9876-123456789abc",
                                                          false));
        VERIFY_IS_FALSE(WorkspaceAgentResume::IsSupported(WorkspaceAgentResume::CurrentVersion + 1,
                                                          workspaceId,
                                                          L"codex",
                                                          L"12345678-1234-abcd-9876-123456789abc",
                                                          true));
        VERIFY_IS_FALSE(WorkspaceAgentResume::IsSupported(WorkspaceAgentResume::CurrentVersion,
                                                          winrt::guid{},
                                                          L"codex",
                                                          L"12345678-1234-abcd-9876-123456789abc",
                                                          true));
    }

    void TabTests::WorkspaceSnapshotOmitsRuntimeProcessState()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto tab = page->_GetFocusedTabImpl();
            VERIFY_IS_NOT_NULL(tab);

            const auto actions = tab->BuildStartupActions(winrt::TerminalApp::BuildStartupKind::Snapshot);
            VERIFY_IS_TRUE(!actions.empty());
            VERIFY_ARE_EQUAL(ShortcutAction::NewTab, actions.front().Action());

            const auto newTabArgs = actions.front().Args().try_as<NewTabArgs>();
            VERIFY_IS_NOT_NULL(newTabArgs);
            const auto terminalArgs = newTabArgs.ContentArgs().try_as<NewTerminalArgs>();
            VERIFY_IS_NOT_NULL(terminalArgs);
            VERIFY_IS_TRUE(!terminalArgs.Profile().empty());
            VERIFY_IS_TRUE(terminalArgs.Commandline().empty());
            VERIFY_ARE_EQUAL(winrt::guid{}, terminalArgs.SessionId());
            VERIFY_ARE_EQUAL(0ull, terminalArgs.ContentId());
        });
    }

    void TabTests::WorkspaceSnapshotOpenIsNonConsuming()
    {
        const auto page = _commonSetup();
        const winrt::guid snapshotId{ 0xd19875d5, 0x294e, 0x47e8, { 0xa1, 0x4e, 0x4f, 0x20, 0xea, 0xbd, 0x55, 0xb5 } };
        WorkspaceSnapshot snapshot{};
        uint32_t tabCount{};

        TestOnUIThread([&]() {
            const auto item = page->_WorkspaceItems().GetAt(0);
            const auto tab = page->_GetFocusedTabImpl();
            VERIFY_IS_NOT_NULL(tab);

            snapshot.Version(2);
            snapshot.Id(snapshotId);
            snapshot.WorkspaceId(item.WorkspaceId());
            snapshot.Name(L"Snapshot test");
            snapshot.CreatedAt(1);
            snapshot.UpdatedAt(1);
            snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>(tab->BuildStartupActions(winrt::TerminalApp::BuildStartupKind::Snapshot)));
            snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>());
            tabCount = page->_tabs.Size();
        });

        // Repository setup and verification intentionally stay off the UI
        // thread. OpenWorkspaceSnapshot itself must return before its durable
        // Load/Touch operations finish.
        const auto state = ApplicationState::SharedInstance();
        state.RemoveWorkspaceSnapshot(snapshotId);
        VERIFY_IS_TRUE(state.SaveWorkspaceSnapshot(snapshot));
        const auto revision = snapshot.Revision();

        TestOnUIThread([&]() {
            page->OpenWorkspaceSnapshot(snapshotId, false);
            VERIFY_ARE_EQUAL(tabCount, page->_tabs.Size());
        });

        bool bound{};
        bool touched{};
        for (uint32_t attempt = 0; attempt < 500 && (!bound || !touched); ++attempt)
        {
            TestOnUIThread([&]() {
                VERIFY_ARE_EQUAL(tabCount, page->_tabs.Size());
                const auto item = page->_WorkspaceItems().GetAt(0);
                const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
                bound = itemImpl->SourceSnapshotId() == std::optional{ snapshotId } &&
                        itemImpl->SourceSnapshotRevision() == revision;
            });

            const auto retained = state.WorkspaceSnapshotById(snapshotId);
            touched = retained && retained.LastOpenedAt() > 0;
            if (!bound || !touched)
            {
                Sleep(10);
            }
        }

        VERIFY_IS_TRUE(bound);
        VERIFY_IS_TRUE(touched);
        TestOnUIThread([&]() {
            const auto item = page->_WorkspaceItems().GetAt(0);
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            VERIFY_ARE_EQUAL(snapshotId, itemImpl->SourceSnapshotId().value());
            VERIFY_ARE_EQUAL(revision, itemImpl->SourceSnapshotRevision());
        });
        VERIFY_IS_TRUE(state.RemoveWorkspaceSnapshot(snapshotId));
    }

    void TabTests::WorkspaceSnapshotRejectsUnsafeActions()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const winrt::guid snapshotId{ 0xf3152b3f, 0x88bf, 0x4fd6, { 0xad, 0x93, 0x92, 0x34, 0x73, 0xdf, 0x90, 0x4d } };
            const winrt::guid workspaceId{ 0x40a7643d, 0x5bc5, 0x476c, { 0x90, 0x3f, 0xd5, 0x3e, 0xf0, 0x61, 0xca, 0xeb } };
            WorkspaceSnapshot snapshot{};
            snapshot.Version(2);
            snapshot.Id(snapshotId);
            snapshot.WorkspaceId(workspaceId);
            snapshot.Name(L"Unsafe snapshot");
            snapshot.CreatedAt(1);
            snapshot.UpdatedAt(1);
            snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>({ ActionAndArgs{ ShortcutAction::CloseWindow, nullptr } }));
            snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>());

            const auto state = ApplicationState::SharedInstance();
            state.RemoveWorkspaceSnapshot(snapshotId);
            VERIFY_IS_FALSE(state.SaveWorkspaceSnapshot(snapshot));
            const auto tabCount = page->_tabs.Size();

            page->OpenWorkspaceSnapshot(snapshotId, false);

            VERIFY_ARE_EQUAL(tabCount, page->_tabs.Size());
            VERIFY_IS_NULL(state.WorkspaceSnapshotById(snapshotId));
            VERIFY_IS_FALSE(state.RemoveWorkspaceSnapshot(snapshotId));
        });
    }

    void TabTests::WorkspaceSnapshotSourceSurvivesContentAttach()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto sourceTab = page->_GetFocusedTabImpl();
            VERIFY_IS_NOT_NULL(sourceTab);
            const winrt::guid workspaceId{ 0x19191919, 0x1919, 0x4919, { 0x89, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19 } };
            const winrt::guid snapshotId{ 0x29292929, 0x2929, 0x4929, { 0x89, 0x29, 0x29, 0x29, 0x29, 0x29, 0x29, 0x29 } };
            WorkspaceSnapshot snapshot{};
            snapshot.Version(WorkspaceSnapshot::CurrentSchemaVersion());
            snapshot.Id(snapshotId);
            snapshot.WorkspaceId(workspaceId);
            snapshot.Name(L"Move source");
            snapshot.CreatedAt(1);
            snapshot.UpdatedAt(1);
            snapshot.Actions(winrt::single_threaded_vector<ActionAndArgs>(sourceTab->BuildStartupActions(winrt::TerminalApp::BuildStartupKind::Snapshot)));
            snapshot.AgentResumeEntries(winrt::single_threaded_vector<AgentResumeInfo>());
            const auto state = ApplicationState::SharedInstance();
            state.RemoveWorkspaceSnapshot(snapshotId);
            VERIFY_IS_TRUE(state.SaveWorkspaceSnapshot(snapshot));

            auto actions = sourceTab->BuildStartupActions(winrt::TerminalApp::BuildStartupKind::Content);
            page->AttachContent(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)),
                                0,
                                workspaceId,
                                snapshotId,
                                snapshot.Revision());

            const auto index = page->_FindWorkspaceIndex(workspaceId);
            VERIFY_IS_TRUE(index.has_value());
            const auto item = page->_WorkspaceItems().GetAt(index.value());
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            VERIFY_IS_TRUE(itemImpl->SourceSnapshotId().has_value());
            VERIFY_ARE_EQUAL(snapshotId, itemImpl->SourceSnapshotId().value());
            VERIFY_ARE_EQUAL(snapshot.Revision(), itemImpl->SourceSnapshotRevision());
            VERIFY_IS_TRUE(state.RemoveWorkspaceSnapshot(snapshotId));
        });
    }

    void TabTests::WorkspaceSnapshotStaleSourceIsCleared()
    {
        const auto page = _commonSetup();
        const winrt::guid workspaceId{ 0x59595959, 0x5959, 0x4959, { 0x89, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59 } };
        const winrt::guid missingSnapshotId{ 0x69696969, 0x6969, 0x4969, { 0x89, 0x69, 0x69, 0x69, 0x69, 0x69, 0x69, 0x69 } };

        TestOnUIThread([&]() {
            const auto sourceTab = page->_GetFocusedTabImpl();
            VERIFY_IS_NOT_NULL(sourceTab);
            auto actions = sourceTab->BuildStartupActions(winrt::TerminalApp::BuildStartupKind::Content);
            page->AttachContent(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)),
                                0,
                                workspaceId,
                                missingSnapshotId,
                                7);

            const auto index = page->_FindWorkspaceIndex(workspaceId);
            VERIFY_IS_TRUE(index.has_value());
            const auto item = page->_WorkspaceItems().GetAt(index.value());
            const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
            VERIFY_IS_TRUE(itemImpl->SourceSnapshotId() == std::optional{ missingSnapshotId });
            VERIFY_ARE_EQUAL(7ull, itemImpl->SourceSnapshotRevision());
        });

        bool cleared{};
        for (uint32_t attempt = 0; attempt < 500 && !cleared; ++attempt)
        {
            TestOnUIThread([&]() {
                const auto index = page->_FindWorkspaceIndex(workspaceId);
                VERIFY_IS_TRUE(index.has_value());
                const auto item = page->_WorkspaceItems().GetAt(index.value());
                const auto itemImpl = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item);
                cleared = !itemImpl->SourceSnapshotId().has_value() && itemImpl->SourceSnapshotRevision() == 0;
            });
            if (!cleared)
            {
                Sleep(10);
            }
        }
        VERIFY_IS_TRUE(cleared);
    }

    void TabTests::WorkspaceLayoutPersistsMetadata()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            page->_ToggleWorkspacePin(page->_tabs.GetAt(0));
            NewTerminalArgs newTerminalArgs{ 1 };
            page->_OpenNewTab(newTerminalArgs);

            const auto secondTab = page->_GetFocusedTabImpl();
            VERIFY_IS_NOT_NULL(secondTab);
            secondTab->SetTabText(L"Persisted workspace");
            page->_WorkspaceItems().GetAt(1).ConfigureAgentResume(L"codex",
                                                                 L"12345678-1234-1234-1234-123456789abc",
                                                                 L"C:\\src\\repo");
            const winrt::guid sourceSnapshotId{ 0x39393939, 0x3939, 0x4939, { 0x89, 0x39, 0x39, 0x39, 0x39, 0x39, 0x39, 0x39 } };
            page->_workspaceFeatures->Snapshots().BindItem(page->_WorkspaceItems().GetAt(1), sourceSnapshotId, 9);

            const auto layout = page->GetWindowLayout();
            VERIFY_IS_NOT_NULL(layout);
            VERIFY_ARE_EQUAL(2u, layout.WorkspaceStateVersion());
            VERIFY_IS_NOT_NULL(layout.WorkspaceIds());
            VERIFY_ARE_EQUAL(2u, layout.WorkspaceIds().Size());
            VERIFY_ARE_EQUAL(page->_WorkspaceItems().GetAt(0).WorkspaceId(), layout.WorkspaceIds().GetAt(0));
            VERIFY_ARE_EQUAL(page->_WorkspaceItems().GetAt(1).WorkspaceId(), layout.WorkspaceIds().GetAt(1));
            VERIFY_IS_NOT_NULL(layout.WorkspaceSurfaceIds());
            VERIFY_ARE_EQUAL(2u, layout.WorkspaceSurfaceIds().Size());
            VERIFY_IS_NOT_NULL(layout.ActiveWorkspaceSurfaceIds());
            VERIFY_ARE_EQUAL(2u, layout.ActiveWorkspaceSurfaceIds().Size());
            VERIFY_IS_NOT_NULL(layout.PinnedWorkspaceIndices());
            VERIFY_ARE_EQUAL(1u, layout.PinnedWorkspaceIndices().Size());
            VERIFY_ARE_EQUAL(0u, layout.PinnedWorkspaceIndices().GetAt(0));
            VERIFY_IS_NOT_NULL(layout.AgentResumeEntries());
            VERIFY_ARE_EQUAL(1u, layout.AgentResumeEntries().Size());
            VERIFY_ARE_EQUAL(page->_WorkspaceItems().GetAt(1).WorkspaceId(), layout.AgentResumeEntries().GetAt(0).WorkspaceId);
            VERIFY_IS_NOT_NULL(layout.WorkspaceSnapshotSources());
            VERIFY_ARE_EQUAL(1u, layout.WorkspaceSnapshotSources().Size());
            VERIFY_ARE_EQUAL(page->_WorkspaceItems().GetAt(1).WorkspaceId(), layout.WorkspaceSnapshotSources().GetAt(0).WorkspaceId);
            VERIFY_ARE_EQUAL(sourceSnapshotId, layout.WorkspaceSnapshotSources().GetAt(0).SnapshotId);
            VERIFY_ARE_EQUAL(9ull, layout.WorkspaceSnapshotSources().GetAt(0).Revision);

            const winrt::guid restoredId{ 0x4cbcf6dc, 0xa03d, 0x4df7, { 0xa1, 0xd4, 0x8f, 0x13, 0xad, 0x8e, 0x20, 0xa6 } };
            AgentResumeInfo restoredResumeInfo{};
            restoredResumeInfo.Version = winrt::TerminalApp::implementation::WorkspaceAgentResume::CurrentVersion;
            restoredResumeInfo.WorkspaceId = restoredId;
            restoredResumeInfo.Provider = L"codex";
            restoredResumeInfo.SessionId = L"abcdefab-1234-1234-1234-123456789abc";
            restoredResumeInfo.WorkingDirectory = L"C:\\restored";
            restoredResumeInfo.RequiresConfirmation = true;
            page->SetStartupWorkspaceState(1, { restoredId }, { 0 }, { restoredResumeInfo });
            // The restored ID must be available before _MakePane initializes
            // the child connection, not assigned afterward from the CWD or UI.
            VERIFY_ARE_EQUAL(restoredId, page->_ReserveWorkspaceIdForNewTab());
            page->_OpenNewTab(NewTerminalArgs{ 2 });

            const auto workspaceItems = page->_WorkspaceItems();
            const auto restored = std::find_if(workspaceItems.begin(), workspaceItems.end(), [&](const auto& item) {
                return item.WorkspaceId() == restoredId;
            });
            VERIFY_IS_TRUE(restored != workspaceItems.end());
            VERIFY_IS_TRUE((*restored).IsPinned());
            VERIFY_IS_TRUE((*restored).HasAgentResume());
            VERIFY_ARE_EQUAL(L"abcdefab-1234-1234-1234-123456789abc", (*restored).AgentResumeSessionId());
            VERIFY_ARE_EQUAL(2u, page->_PinnedTabCount());
        });
    }

    void TabTests::WorkspaceNotificationStateTracksUnread()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto tab = page->_tabs.GetAt(0);
            const winrt::TerminalApp::WorkspaceItemViewModel item{ tab };
            const auto surfaceId = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item)->SurfaceIdForTab(tab);

            item.AddNotification(surfaceId, 7, L"Build", L"Build completed");
            item.AddNotification(surfaceId, 9, L"Tests", L"Tests completed");

            VERIFY_ARE_EQUAL(2u, item.UnreadCount());
            VERIFY_ARE_EQUAL(L"2", item.UnreadBadgeText());
            VERIFY_ARE_EQUAL(L"Tests completed", item.LatestNotification());
            VERIFY_ARE_EQUAL(2u, item.Notifications().Size());
            VERIFY_ARE_EQUAL(2u, item.NotificationHistoryCount());

            const auto latest = item.LatestUnreadNotification();
            VERIFY_IS_NOT_NULL(latest);
            VERIFY_ARE_EQUAL(item.WorkspaceId(), latest.WorkspaceId());
            VERIFY_ARE_EQUAL(surfaceId, latest.SurfaceId());
            VERIFY_ARE_EQUAL(9u, latest.PaneId());
            VERIFY_ARE_EQUAL(tab.Title(), latest.WorkspaceTitle());
            VERIFY_ARE_EQUAL(L"Tests", latest.Title());
            VERIFY_IS_FALSE(latest.IsRead());
            VERIFY_IS_FALSE(latest.TimestampText().empty());
            VERIFY_IS_TRUE(std::wstring_view{ latest.AutomationName() }.find(L"Tests completed") != std::wstring_view::npos);

            item.MarkNotificationsReadForPane(9);
            VERIFY_ARE_EQUAL(1u, item.UnreadCount());
            VERIFY_ARE_EQUAL(2u, item.NotificationHistoryCount());
            VERIFY_IS_TRUE(latest.IsRead());
            VERIFY_ARE_EQUAL(7u, item.LatestUnreadNotification().PaneId());

            item.MarkAllNotificationsRead();
            VERIFY_ARE_EQUAL(0u, item.UnreadCount());
            VERIFY_IS_NULL(item.LatestUnreadNotification());
            VERIFY_IS_TRUE(item.LatestNotification().empty());
            VERIFY_ARE_EQUAL(2u, item.NotificationHistoryCount());

            item.ClearNotificationHistory();
            VERIFY_ARE_EQUAL(0u, item.NotificationHistoryCount());
            VERIFY_ARE_EQUAL(0u, item.Notifications().Size());

            for (uint32_t paneId = 0; paneId < 101; ++paneId)
            {
                item.AddNotification(surfaceId, paneId, L"Agent", L"History entry");
            }
            VERIFY_ARE_EQUAL(100u, item.UnreadCount());
            VERIFY_ARE_EQUAL(100u, item.NotificationHistoryCount());
            VERIFY_ARE_EQUAL(1u, item.Notifications().GetAt(0).PaneId());
            VERIFY_ARE_EQUAL(100u, item.Notifications().GetAt(99).PaneId());
        });
    }

    void TabTests::WorkspaceNotificationRefreshIsBounded()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto tab = page->_tabs.GetAt(0);
            const auto items = winrt::single_threaded_observable_vector<winrt::TerminalApp::WorkspaceItemViewModel>();
            constexpr uint32_t workspaceCount = 12;
            constexpr uint32_t notificationsPerWorkspace = 125;

            for (uint32_t workspaceIndex = 0; workspaceIndex < workspaceCount; ++workspaceIndex)
            {
                const winrt::TerminalApp::WorkspaceItemViewModel item{ tab };
                const auto surfaceId = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceItemViewModel>(item)->SurfaceIdForTab(tab);
                for (uint32_t notificationIndex = 0; notificationIndex < notificationsPerWorkspace; ++notificationIndex)
                {
                    item.AddNotification(surfaceId, notificationIndex, L"Stress", winrt::to_hstring(notificationIndex));
                }
                VERIFY_ARE_EQUAL(100u, item.NotificationHistoryCount());
                items.Append(item);
            }

            page->_workspaceSidebar.WorkspaceList().ItemsSource(items);
            uint32_t vectorChangeCount = 0;
            const auto vectorChangedRevoker = page->_workspaceSidebar.NotificationHistory().VectorChanged(winrt::auto_revoke, [&](auto&&, auto&&) {
                ++vectorChangeCount;
            });

            constexpr uint32_t refreshCount = 25;
            const auto startedAt = std::chrono::steady_clock::now();
            for (uint32_t refreshIndex = 0; refreshIndex < refreshCount; ++refreshIndex)
            {
                page->_workspaceSidebar.RefreshNotifications();
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

            constexpr uint32_t maximumNotificationHistory = 1000;
            VERIFY_ARE_EQUAL(maximumNotificationHistory, page->_workspaceSidebar.NotificationHistory().Size());
            VERIFY_ARE_EQUAL(refreshCount, vectorChangeCount);
            Log::Comment(NoThrowString().Format(L"25 notification projection refreshes completed in %lld ms", elapsed.count()));
        });
    }

    void TabTests::PaneAttentionState()
    {
        const auto page = _commonSetup();
        TestOnUIThread([&]() {
            const auto tab = page->_GetFocusedTabImpl();
            VERIFY_IS_NOT_NULL(tab);
            const auto pane = tab->GetActivePane();
            VERIFY_IS_NOT_NULL(pane);

            const auto item = page->_WorkspaceItemForTab(*tab);
            VERIFY_IS_NOT_NULL(item);
            VERIFY_IS_TRUE(page->_RecordWorkspaceNotification(tab, pane->GetContent(), L"Agent", L"Input required"));
            VERIFY_IS_TRUE(pane->HasAttention());
            VERIFY_ARE_EQUAL(1u, item.UnreadCount());

            VERIFY_IS_TRUE(page->_FocusWorkspaceNotification(item));
            VERIFY_IS_FALSE(pane->HasAttention());
            VERIFY_ARE_EQUAL(0u, item.UnreadCount());
            VERIFY_ARE_EQUAL(1u, item.NotificationHistoryCount());
            VERIFY_IS_TRUE(item.Notifications().GetAt(0).IsRead());

            page->_RefreshWorkspaceNotifications();
            VERIFY_ARE_EQUAL(1u, page->_workspaceSidebar.NotificationHistory().Size());
            page->_ClearAllWorkspaceNotificationHistory();
            VERIFY_ARE_EQUAL(0u, page->_workspaceSidebar.NotificationHistory().Size());
        });
    }

    // Method Description:
    // - This is a helper to set up a TerminalPage for a unittest. This method
    //   does a couple things:
    //   * Create()'s a TerminalPage with the given settings. Constructing a
    //     TerminalPage so that we can get at its implementation is wacky, so
    //     this helper will do it correctly for you, even if this doesn't make a
    //     ton of sense on the surface. This is also why you need to pass both a
    //     projection and a com_ptr to this method.
    //   * It will use the provided settings object to initialize the TerminalPage
    //   * It will add the TerminalPage to the test Application, so that we can
    //     get actual layout events. Much of the Terminal assumes there's a
    //     non-zero ActualSize to the Terminal window, and adding the Page to
    //     the Application will make it behave as expected.
    //   * It will wait for the TerminalPage to finish initialization before
    //     returning control to the caller. It does this by creating an event and
    //     only setting the event when the TerminalPage raises its Initialized
    //     event, to signal that startup is complete. At this point, there will
    //     be one tab with the default profile in the page.
    //   * It will also ensure that the first tab is focused, since that happens
    //     asynchronously in the application typically.
    // Arguments:
    // - page: a TerminalPage implementation ptr that will receive the new TerminalPage instance
    // - initialSettings: a CascadiaSettings to initialize the TerminalPage with.
    // Return Value:
    // - <none>
    void TabTests::_initializeTerminalPage(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
                                           CascadiaSettings initialSettings)
    {
        // This is super wacky, but we can't just initialize the
        // com_ptr<impl::TerminalPage> in the lambda and assign it back out of
        // the lambda. We'll crash trying to get a weak_ref to the TerminalPage
        // during TerminalPage::Create() below.
        //
        // Instead, create the winrt object, then get a com_ptr to the
        // implementation _from_ the winrt object. This seems to work, even if
        // it's weird.
        winrt::TerminalApp::TerminalPage projectedPage{ nullptr };

        _windowProperties = winrt::make_self<winrt::TerminalApp::implementation::WindowProperties>();
        winrt::TerminalApp::WindowProperties props = *_windowProperties;
        _contentManager = winrt::make_self<winrt::TerminalApp::implementation::ContentManager>();
        winrt::TerminalApp::ContentManager contentManager = *_contentManager;
        Log::Comment(NoThrowString().Format(L"Construct the TerminalPage"));
        auto result = RunOnUIThread([&projectedPage, &page, initialSettings, props, contentManager]() {
            projectedPage = winrt::TerminalApp::TerminalPage(props, contentManager);
            page.copy_from(winrt::get_self<winrt::TerminalApp::implementation::TerminalPage>(projectedPage));
            page->SetSettings(initialSettings, false);
        });
        VERIFY_SUCCEEDED(result);

        VERIFY_IS_NOT_NULL(page);
        VERIFY_IS_NOT_NULL(page->_settings);

        ::details::Event waitForInitEvent;
        if (!waitForInitEvent.IsValid())
        {
            VERIFY_SUCCEEDED(HRESULT_FROM_WIN32(::GetLastError()));
        }
        page->Initialized([&waitForInitEvent](auto&&, auto&&) {
            waitForInitEvent.Set();
        });

        Log::Comment(L"Create() the TerminalPage");

        result = RunOnUIThread([&page]() {
            VERIFY_IS_NOT_NULL(page);
            VERIFY_IS_NOT_NULL(page->_settings);
            page->Create();
            Log::Comment(L"Create()'d the page successfully");

            // Build a NewTab action, to make sure we start with one. The real
            // Terminal will always get one from AppCommandlineArgs.
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs args{ newTerminalArgs };
            ActionAndArgs newTabAction{ ShortcutAction::NewTab, args };
            // push the arg onto the front
            page->_startupActions.push_back(std::move(newTabAction));
            Log::Comment(L"Added a single newTab action");

            auto app = ::winrt::Windows::UI::Xaml::Application::Current();

            winrt::TerminalApp::TerminalPage pp = *page;
            winrt::Windows::UI::Xaml::Window::Current().Content(pp);
            winrt::Windows::UI::Xaml::Window::Current().Activate();
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Wait for the page to finish initializing...");
        VERIFY_SUCCEEDED(waitForInitEvent.Wait());
        Log::Comment(L"...Done");

        result = RunOnUIThread([&page]() {
            // In the real app, this isn't a problem, but doesn't happen
            // reliably in the unit tests.
            Log::Comment(L"Ensure we set the first tab as the selected one.");
            auto tab = page->_tabs.GetAt(0);
            auto tabImpl = page->_GetTabImpl(tab);
            page->_tabView.SelectedItem(tabImpl->TabViewItem());
            page->_UpdatedSelectedTab(tab);
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::TryInitializePage()
    {
        // This is a very simple test to prove we can create settings and a
        // TerminalPage and not only create them successfully, but also create a
        // tab using those settings successfully.

        // - - - IMPORTANT - - -
        // GH#14623: "closeOnExit": "never" is important for all test profiles. Without
        // it, the spawned process exits immediately in the UAP test environment,
        // and the default "automatic" close-on-exit behavior removes the
        // tab/pane asynchronously, racing against test assertions.
        static constexpr std::wstring_view settingsJson0{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                },
                {
                    "name" : "profile1",
                    "guid": "{6239a42c-2222-49a3-80bd-e8fdd045185c}",
                    "historySize": 2,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings0{ settingsJson0, {} };
        VERIFY_IS_NOT_NULL(settings0);

        // This is super wacky, but we can't just initialize the
        // com_ptr<impl::TerminalPage> in the lambda and assign it back out of
        // the lambda. We'll crash trying to get a weak_ref to the TerminalPage
        // during TerminalPage::Create() below.
        //
        // Instead, create the winrt object, then get a com_ptr to the
        // implementation _from_ the winrt object. This seems to work, even if
        // it's weird.
        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPage(page, settings0);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::TryDuplicateBadTab()
    {
        // * Create a tab with a profile with GUID 1
        // * Reload the settings so that GUID 1 is no longer in the list of profiles
        // * Try calling _DuplicateFocusedTab on tab 1
        // * No new tab should be created (and more importantly, the app should not crash)
        //
        // Created to test GH#2455

        static constexpr std::wstring_view settingsJson0{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                },
                {
                    "name" : "profile1",
                    "guid": "{6239a42c-2222-49a3-80bd-e8fdd045185c}",
                    "historySize": 2,
                    "closeOnExit": "never"
                }
            ]
        })" };

        static constexpr std::wstring_view settingsJson1{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile1",
                    "guid": "{6239a42c-2222-49a3-80bd-e8fdd045185c}",
                    "historySize": 2,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings0{ settingsJson0, {} };
        VERIFY_IS_NOT_NULL(settings0);

        CascadiaSettings settings1{ settingsJson1, {} };
        VERIFY_IS_NOT_NULL(settings1);

        const auto guid1 = Microsoft::Console::Utils::GuidFromString(L"{6239a42c-1111-49a3-80bd-e8fdd045185c}");
        const auto guid2 = Microsoft::Console::Utils::GuidFromString(L"{6239a42c-2222-49a3-80bd-e8fdd045185c}");
        const auto guid3 = Microsoft::Console::Utils::GuidFromString(L"{6239a42c-3333-49a3-80bd-e8fdd045185c}");

        // This is super wacky, but we can't just initialize the
        // com_ptr<impl::TerminalPage> in the lambda and assign it back out of
        // the lambda. We'll crash trying to get a weak_ref to the TerminalPage
        // during TerminalPage::Create() below.
        //
        // Instead, create the winrt object, then get a com_ptr to the
        // implementation _from_ the winrt object. This seems to work, even if
        // it's weird.
        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPage(page, settings0);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Duplicate the first tab");
        result = RunOnUIThread([&page]() {
            page->_DuplicateFocusedTab();
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(NoThrowString().Format(
            L"Change the settings of the TerminalPage so the first profile is "
            L"no longer in the list of profiles"));
        result = RunOnUIThread([&page, settings1]() {
            page->_settings = settings1;
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Duplicate the tab, and don't crash");
        result = RunOnUIThread([&page]() {
            page->_DuplicateFocusedTab();
            VERIFY_ARE_EQUAL(3u, page->_tabs.Size(), L"We should successfully duplicate a tab hosting a deleted profile.");
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::TryDuplicateBadPane()
    {
        // * Create a tab with a profile with GUID 1
        // * Reload the settings so that GUID 1 is no longer in the list of profiles
        // * Try calling _SplitPane(Duplicate) on tab 1
        // * No new pane should be created (and more importantly, the app should not crash)
        //
        // Created to test GH#2455

        static constexpr std::wstring_view settingsJson0{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                },
                {
                    "name" : "profile1",
                    "guid": "{6239a42c-2222-49a3-80bd-e8fdd045185c}",
                    "historySize": 2,
                    "closeOnExit": "never"
                }
            ]
        })" };

        static constexpr std::wstring_view settingsJson1{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile1",
                    "guid": "{6239a42c-2222-49a3-80bd-e8fdd045185c}",
                    "historySize": 2,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings0{ settingsJson0, {} };
        VERIFY_IS_NOT_NULL(settings0);

        CascadiaSettings settings1{ settingsJson1, {} };
        VERIFY_IS_NOT_NULL(settings1);

        const auto guid1 = Microsoft::Console::Utils::GuidFromString(L"{6239a42c-1111-49a3-80bd-e8fdd045185c}");
        const auto guid2 = Microsoft::Console::Utils::GuidFromString(L"{6239a42c-2222-49a3-80bd-e8fdd045185c}");
        const auto guid3 = Microsoft::Console::Utils::GuidFromString(L"{6239a42c-3333-49a3-80bd-e8fdd045185c}");

        // This is super wacky, but we can't just initialize the
        // com_ptr<impl::TerminalPage> in the lambda and assign it back out of
        // the lambda. We'll crash trying to get a weak_ref to the TerminalPage
        // during TerminalPage::Create() below.
        //
        // Instead, create the winrt object, then get a com_ptr to the
        // implementation _from_ the winrt object. This seems to work, even if
        // it's weird.
        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPage(page, settings0);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(1, tab->GetLeafPaneCount());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(NoThrowString().Format(L"Duplicate the first pane"));
        result = RunOnUIThread([&page]() {
            const auto workspaceId = page->_WorkspaceIdForTab(page->_GetFocusedTab());
            page->_SplitPane(nullptr,
                             SplitDirection::Automatic,
                             0.5f,
                             page->_MakePane(nullptr, page->_GetFocusedTab(), nullptr, true, workspaceId),
                             workspaceId);

            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(2, tab->GetLeafPaneCount());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(NoThrowString().Format(
            L"Change the settings of the TerminalPage so the first profile is "
            L"no longer in the list of profiles"));
        result = RunOnUIThread([&page, settings1]() {
            page->_settings = settings1;
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(NoThrowString().Format(L"Duplicate the pane, and don't crash"));
        result = RunOnUIThread([&page]() {
            const auto workspaceId = page->_WorkspaceIdForTab(page->_GetFocusedTab());
            page->_SplitPane(nullptr,
                             SplitDirection::Automatic,
                             0.5f,
                             page->_MakePane(nullptr, page->_GetFocusedTab(), nullptr, true, workspaceId),
                             workspaceId);

            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(3,
                             tab->GetLeafPaneCount(),
                             L"We should successfully duplicate a pane hosting a deleted profile.");
        });
        VERIFY_SUCCEEDED(result);

        auto cleanup = wil::scope_exit([] {
            auto result = RunOnUIThread([]() {
                // There's something causing us to crash north of
                // TSFInputControl::NotifyEnter, or LayoutRequested. It's very
                // unclear what that issue is. Since these tests don't run in
                // CI, simply log a message so that the dev running these tests
                // knows it's expected.
                Log::Comment(L"This test often crashes on cleanup, even when it succeeds. If it succeeded, then crashes, that's okay.");
            });
            VERIFY_SUCCEEDED(result);
        });
    }

    // Method Description:
    // - This is a helper method for setting up a TerminalPage with some common
    //   settings, and creating the first tab.
    // Arguments:
    // - <none>
    // Return Value:
    // - The initialized TerminalPage, ready to use.
    winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> TabTests::_commonSetup()
    {
        static constexpr std::wstring_view settingsJson0{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "showTabsInTitlebar": false,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "tabTitle" : "Profile 0",
                    "historySize": 1,
                    "closeOnExit": "never"
                },
                {
                    "name" : "profile1",
                    "guid": "{6239a42c-2222-49a3-80bd-e8fdd045185c}",
                    "tabTitle" : "Profile 1",
                    "historySize": 2,
                    "closeOnExit": "never"
                },
                {
                    "name" : "profile2",
                    "guid": "{6239a42c-3333-49a3-80bd-e8fdd045185c}",
                    "tabTitle" : "Profile 2",
                    "historySize": 3,
                    "closeOnExit": "never"
                },
                {
                    "name" : "profile3",
                    "guid": "{6239a42c-4444-49a3-80bd-e8fdd045185c}",
                    "tabTitle" : "Profile 3",
                    "historySize": 4,
                    "closeOnExit": "never"
                }
            ],
            "schemes":
            [
                {
                    "name": "Campbell",
                    "foreground": "#CCCCCC",
                    "background": "#0C0C0C",
                    "cursorColor": "#FFFFFF",
                    "black": "#0C0C0C",
                    "red": "#C50F1F",
                    "green": "#13A10E",
                    "yellow": "#C19C00",
                    "blue": "#0037DA",
                    "purple": "#881798",
                    "cyan": "#3A96DD",
                    "white": "#CCCCCC",
                    "brightBlack": "#767676",
                    "brightRed": "#E74856",
                    "brightGreen": "#16C60C",
                    "brightYellow": "#F9F1A5",
                    "brightBlue": "#3B78FF",
                    "brightPurple": "#B4009E",
                    "brightCyan": "#61D6D6",
                    "brightWhite": "#F2F2F2"
                },
                {
                    "name": "Vintage",
                    "foreground": "#C0C0C0",
                    "background": "#000000",
                    "cursorColor": "#FFFFFF",
                    "black": "#000000",
                    "red": "#800000",
                    "green": "#008000",
                    "yellow": "#808000",
                    "blue": "#000080",
                    "purple": "#800080",
                    "cyan": "#008080",
                    "white": "#C0C0C0",
                    "brightBlack": "#808080",
                    "brightRed": "#FF0000",
                    "brightGreen": "#00FF00",
                    "brightYellow": "#FFFF00",
                    "brightBlue": "#0000FF",
                    "brightPurple": "#FF00FF",
                    "brightCyan": "#00FFFF",
                    "brightWhite": "#FFFFFF"
                },
                {
                    "name": "One Half Light",
                    "foreground": "#383A42",
                    "background": "#FAFAFA",
                    "cursorColor": "#4F525D",
                    "black": "#383A42",
                    "red": "#E45649",
                    "green": "#50A14F",
                    "yellow": "#C18301",
                    "blue": "#0184BC",
                    "purple": "#A626A4",
                    "cyan": "#0997B3",
                    "white": "#FAFAFA",
                    "brightBlack": "#4F525D",
                    "brightRed": "#DF6C75",
                    "brightGreen": "#98C379",
                    "brightYellow": "#E4C07A",
                    "brightBlue": "#61AFEF",
                    "brightPurple": "#C577DD",
                    "brightCyan": "#56B5C1",
                    "brightWhite": "#FFFFFF"
                }
            ]
        })" };

        CascadiaSettings settings0{ settingsJson0, {} };
        VERIFY_IS_NOT_NULL(settings0);

        const auto guid1 = Microsoft::Console::Utils::GuidFromString(L"{6239a42c-1111-49a3-80bd-e8fdd045185c}");
        const auto guid2 = Microsoft::Console::Utils::GuidFromString(L"{6239a42c-2222-49a3-80bd-e8fdd045185c}");

        // This is super wacky, but we can't just initialize the
        // com_ptr<impl::TerminalPage> in the lambda and assign it back out of
        // the lambda. We'll crash trying to get a weak_ref to the TerminalPage
        // during TerminalPage::Create() below.
        //
        // Instead, create the winrt object, then get a com_ptr to the
        // implementation _from_ the winrt object. This seems to work, even if
        // it's weird.
        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPage(page, settings0);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
        });
        VERIFY_SUCCEEDED(result);

        return page;
    }

    void TabTests::TryZoomPane()
    {
        BEGIN_TEST_METHOD_PROPERTIES()
            TEST_METHOD_PROPERTY(L"IsolationLevel", L"Method")
        END_TEST_METHOD_PROPERTIES()

        auto page = _commonSetup();

        Log::Comment(L"Create a second pane");
        auto result = RunOnUIThread([&page]() {
            SplitPaneArgs args{ SplitType::Duplicate };
            ActionEventArgs eventArgs{ args };
            page->_HandleSplitPane(nullptr, eventArgs);
            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));

            VERIFY_ARE_EQUAL(2, firstTab->GetLeafPaneCount());
            VERIFY_IS_FALSE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Zoom in on the pane");
        result = RunOnUIThread([&page]() {
            ActionEventArgs eventArgs{};
            page->_HandleTogglePaneZoom(nullptr, eventArgs);
            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(2, firstTab->GetLeafPaneCount());
            VERIFY_IS_TRUE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Zoom out of the pane");
        result = RunOnUIThread([&page]() {
            ActionEventArgs eventArgs{};
            page->_HandleTogglePaneZoom(nullptr, eventArgs);
            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(2, firstTab->GetLeafPaneCount());
            VERIFY_IS_FALSE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::MoveFocusFromZoomedPane()
    {
        auto page = _commonSetup();

        Log::Comment(L"Create a second pane");
        auto result = RunOnUIThread([&page]() {
            // Set up action
            SplitPaneArgs args{ SplitType::Duplicate };
            ActionEventArgs eventArgs{ args };
            page->_HandleSplitPane(nullptr, eventArgs);
            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));

            VERIFY_ARE_EQUAL(2, firstTab->GetLeafPaneCount());
            VERIFY_IS_FALSE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Zoom in on the pane");
        result = RunOnUIThread([&page]() {
            // Set up action
            ActionEventArgs eventArgs{};

            page->_HandleTogglePaneZoom(nullptr, eventArgs);

            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(2, firstTab->GetLeafPaneCount());
            VERIFY_IS_TRUE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Move focus. We should still be zoomed.");
        result = RunOnUIThread([&page]() {
            // Set up action
            MoveFocusArgs args{ FocusDirection::Left };
            ActionEventArgs eventArgs{ args };

            page->_HandleMoveFocus(nullptr, eventArgs);

            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(2, firstTab->GetLeafPaneCount());
            VERIFY_IS_TRUE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::CloseZoomedPane()
    {
        auto page = _commonSetup();

        Log::Comment(L"Create a second pane");
        auto result = RunOnUIThread([&page]() {
            // Set up action
            SplitPaneArgs args{ SplitType::Duplicate };
            ActionEventArgs eventArgs{ args };
            page->_HandleSplitPane(nullptr, eventArgs);
            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));

            VERIFY_ARE_EQUAL(2, firstTab->GetLeafPaneCount());
            VERIFY_IS_FALSE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Zoom in on the pane");
        result = RunOnUIThread([&page]() {
            // Set up action
            ActionEventArgs eventArgs{};

            page->_HandleTogglePaneZoom(nullptr, eventArgs);

            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(2, firstTab->GetLeafPaneCount());
            VERIFY_IS_TRUE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Close Pane. This should cause us to un-zoom, and remove the second pane from the tree");
        result = RunOnUIThread([&page]() {
            // Set up action
            ActionEventArgs eventArgs{};

            page->_HandleClosePane(nullptr, eventArgs);

            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_FALSE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);

        // Introduce a slight delay to let the events finish propagating
        Sleep(250);

        Log::Comment(L"Check to ensure there's only one pane left.");

        result = RunOnUIThread([&page]() {
            auto firstTab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(1, firstTab->GetLeafPaneCount());
            VERIFY_IS_FALSE(firstTab->IsZoomed());
        });
        VERIFY_SUCCEEDED(result);
    }

    void TabTests::SwapPanes()
    {
        auto page = _commonSetup();

        Log::Comment(L"Setup 4 panes.");
        // Create the following layout
        // -------------------
        // |   1    |   2    |
        // |        |        |
        // -------------------
        // |   3    |   4    |
        // |        |        |
        // -------------------
        uint32_t firstId = 0, secondId = 0, thirdId = 0, fourthId = 0;
        TestOnUIThread([&]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            firstId = tab->_activePane->Id().value();
            // We start with 1 tab, split vertically to get
            // -------------------
            // |   1    |   2    |
            // |        |        |
            // -------------------
            const auto workspaceId = page->_WorkspaceIdForTab(page->_GetFocusedTab());
            page->_SplitPane(nullptr,
                             SplitDirection::Right,
                             0.5f,
                             page->_MakePane(nullptr, page->_GetFocusedTab(), nullptr, true, workspaceId),
                             workspaceId);
            secondId = tab->_activePane->Id().value();
        });
        Sleep(250);
        TestOnUIThread([&]() {
            // After this the `2` pane is focused, go back to `1` being focused
            page->_MoveFocus(FocusDirection::Left);
        });
        Sleep(250);
        TestOnUIThread([&]() {
            // Split again to make the 3rd tab
            // -------------------
            // |   1    |        |
            // |        |        |
            // ---------|   2    |
            // |   3    |        |
            // |        |        |
            // -------------------
            const auto workspaceId = page->_WorkspaceIdForTab(page->_GetFocusedTab());
            page->_SplitPane(nullptr,
                             SplitDirection::Down,
                             0.5f,
                             page->_MakePane(nullptr, page->_GetFocusedTab(), nullptr, true, workspaceId),
                             workspaceId);
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            // Split again to make the 3rd tab
            thirdId = tab->_activePane->Id().value();
        });
        Sleep(250);
        TestOnUIThread([&]() {
            // After this the `3` pane is focused, go back to `2` being focused
            page->_MoveFocus(FocusDirection::Right);
        });
        Sleep(250);
        TestOnUIThread([&]() {
            // Split to create the final pane
            // -------------------
            // |   1    |   2    |
            // |        |        |
            // -------------------
            // |   3    |   4    |
            // |        |        |
            // -------------------
            const auto workspaceId = page->_WorkspaceIdForTab(page->_GetFocusedTab());
            page->_SplitPane(nullptr,
                             SplitDirection::Down,
                             0.5f,
                             page->_MakePane(nullptr, page->_GetFocusedTab(), nullptr, true, workspaceId),
                             workspaceId);
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            fourthId = tab->_activePane->Id().value();
        });

        Sleep(250);
        TestOnUIThread([&]() {
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(4, tab->GetLeafPaneCount());
            // just to be complete, make sure we actually have 4 different ids
            VERIFY_ARE_NOT_EQUAL(firstId, fourthId);
            VERIFY_ARE_NOT_EQUAL(secondId, fourthId);
            VERIFY_ARE_NOT_EQUAL(thirdId, fourthId);
            VERIFY_ARE_NOT_EQUAL(firstId, thirdId);
            VERIFY_ARE_NOT_EQUAL(secondId, thirdId);
            VERIFY_ARE_NOT_EQUAL(firstId, secondId);
        });

        // Gratuitous use of sleep to make sure that the UI has updated properly
        // after each operation.
        Sleep(250);
        // Now try to move the pane through the tree
        Log::Comment(L"Move pane to the left. This should swap panes 3 and 4");
        // -------------------
        // |   1    |   2    |
        // |        |        |
        // -------------------
        // |   4    |   3    |
        // |        |        |
        // -------------------
        TestOnUIThread([&]() {
            // Set up action
            SwapPaneArgs args{ FocusDirection::Left };
            ActionEventArgs eventArgs{ args };

            page->_HandleSwapPane(nullptr, eventArgs);
        });

        Sleep(250);

        TestOnUIThread([&]() {
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(4, tab->GetLeafPaneCount());
            // Our currently focused pane should be `4`
            VERIFY_ARE_EQUAL(fourthId, tab->_activePane->Id().value());

            // Inspect the tree to make sure we swapped
            VERIFY_ARE_EQUAL(fourthId, tab->_rootPane->_firstChild->_secondChild->Id().value());
            VERIFY_ARE_EQUAL(thirdId, tab->_rootPane->_secondChild->_secondChild->Id().value());
        });

        Sleep(250);

        Log::Comment(L"Move pane to up. This should swap panes 1 and 4");
        // -------------------
        // |   4    |   2    |
        // |        |        |
        // -------------------
        // |   1    |   3    |
        // |        |        |
        // -------------------
        TestOnUIThread([&]() {
            // Set up action
            SwapPaneArgs args{ FocusDirection::Up };
            ActionEventArgs eventArgs{ args };

            page->_HandleSwapPane(nullptr, eventArgs);
        });

        Sleep(250);

        TestOnUIThread([&]() {
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(4, tab->GetLeafPaneCount());
            // Our currently focused pane should be `4`
            VERIFY_ARE_EQUAL(fourthId, tab->_activePane->Id().value());

            // Inspect the tree to make sure we swapped
            VERIFY_ARE_EQUAL(fourthId, tab->_rootPane->_firstChild->_firstChild->Id().value());
            VERIFY_ARE_EQUAL(firstId, tab->_rootPane->_firstChild->_secondChild->Id().value());
        });

        Sleep(250);

        Log::Comment(L"Move pane to the right. This should swap panes 2 and 4");
        // -------------------
        // |   2    |   4    |
        // |        |        |
        // -------------------
        // |   1    |   3    |
        // |        |        |
        // -------------------
        TestOnUIThread([&]() {
            // Set up action
            SwapPaneArgs args{ FocusDirection::Right };
            ActionEventArgs eventArgs{ args };

            page->_HandleSwapPane(nullptr, eventArgs);
        });

        Sleep(250);

        TestOnUIThread([&]() {
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(4, tab->GetLeafPaneCount());
            // Our currently focused pane should be `4`
            VERIFY_ARE_EQUAL(fourthId, tab->_activePane->Id().value());

            // Inspect the tree to make sure we swapped
            VERIFY_ARE_EQUAL(fourthId, tab->_rootPane->_secondChild->_firstChild->Id().value());
            VERIFY_ARE_EQUAL(secondId, tab->_rootPane->_firstChild->_firstChild->Id().value());
        });

        Sleep(250);

        Log::Comment(L"Move pane down. This should swap panes 3 and 4");
        // -------------------
        // |   2    |   3    |
        // |        |        |
        // -------------------
        // |   1    |   4    |
        // |        |        |
        // -------------------
        TestOnUIThread([&]() {
            // Set up action
            SwapPaneArgs args{ FocusDirection::Down };
            ActionEventArgs eventArgs{ args };

            page->_HandleSwapPane(nullptr, eventArgs);
        });

        Sleep(250);

        TestOnUIThread([&]() {
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_ARE_EQUAL(4, tab->GetLeafPaneCount());
            // Our currently focused pane should be `4`
            VERIFY_ARE_EQUAL(fourthId, tab->_activePane->Id().value());

            // Inspect the tree to make sure we swapped
            VERIFY_ARE_EQUAL(fourthId, tab->_rootPane->_secondChild->_secondChild->Id().value());
            VERIFY_ARE_EQUAL(thirdId, tab->_rootPane->_secondChild->_firstChild->Id().value());
        });
    }

    void TabTests::NextMRUTab()
    {
        // This is a test for GH#8025 - we want to make sure that MRU tab
        // ordering works correctly and that in-order/disabled switching works.
        //
        // Note: We test MRU ordering directly rather than going through the
        // command palette tab switcher, because the palette's anchor key
        // handling auto-dismisses when no modifier keys are held (which we
        // can't simulate in the test environment).

        auto page = _commonSetup();

        Log::Comment(L"Create Tab[1]");
        TestOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{ 1 };
            page->_OpenNewTab(newTerminalArgs);
        });
        VERIFY_ARE_EQUAL(2u, page->_tabs.Size());

        Log::Comment(L"Create Tab[2]");
        TestOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{ 2 };
            page->_OpenNewTab(newTerminalArgs);
        });
        VERIFY_ARE_EQUAL(3u, page->_tabs.Size());

        Log::Comment(L"Create Tab[3]");
        TestOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{ 3 };
            page->_OpenNewTab(newTerminalArgs);
        });
        VERIFY_ARE_EQUAL(4u, page->_tabs.Size());

        TestOnUIThread([&page]() {
            auto focusedIndex = page->_GetFocusedTabIndex().value_or(-1);
            VERIFY_ARE_EQUAL(3u, focusedIndex, L"Verify Tab[3] is focused");
        });

        Log::Comment(L"Select Tab[1]");
        TestOnUIThread([&page]() {
            page->_SelectTab(1);
        });

        TestOnUIThread([&page]() {
            auto focusedIndex = page->_GetFocusedTabIndex().value_or(-1);
            VERIFY_ARE_EQUAL(1u, focusedIndex, L"Verify Tab[1] is focused");
        });

        // MRU order should now be: Tab[1], Tab[3], Tab[2], Tab[0]
        // Verify the MRU list directly.
        Log::Comment(L"Verify MRU order: MRU[0]=Tab[1], MRU[1]=Tab[3]");
        TestOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(4u, page->_mruTabs.Size());
            uint32_t mruIdx;
            page->_tabs.IndexOf(page->_mruTabs.GetAt(0), mruIdx);
            VERIFY_ARE_EQUAL(1u, mruIdx, L"MRU[0] should be Tab[1] (most recent)");
            page->_tabs.IndexOf(page->_mruTabs.GetAt(1), mruIdx);
            VERIFY_ARE_EQUAL(3u, mruIdx, L"MRU[1] should be Tab[3] (last tab added)");
        });

        Log::Comment(L"Select MRU[1]=Tab[3] directly");
        TestOnUIThread([&page]() {
            // The next MRU tab after Tab[1] is Tab[3]
            uint32_t nextMruIdx;
            page->_tabs.IndexOf(page->_mruTabs.GetAt(1), nextMruIdx);
            page->_SelectTab(nextMruIdx);
        });

        TestOnUIThread([&page]() {
            auto focusedIndex = page->_GetFocusedTabIndex().value_or(-1);
            VERIFY_ARE_EQUAL(3u, focusedIndex, L"Verify Tab[3] is focused");
        });

        Log::Comment(L"Select MRU[1]=Tab[1] directly");
        TestOnUIThread([&page]() {
            uint32_t nextMruIdx;
            page->_tabs.IndexOf(page->_mruTabs.GetAt(1), nextMruIdx);
            page->_SelectTab(nextMruIdx);
        });

        TestOnUIThread([&page]() {
            auto focusedIndex = page->_GetFocusedTabIndex().value_or(-1);
            VERIFY_ARE_EQUAL(1u, focusedIndex, L"Verify Tab[1] is focused");
        });

        // The Disabled tab switcher mode uses direct index-based switching
        // without the command palette, so it works in the test environment.
        Log::Comment(L"Change the tab switch order to not use the tab switcher (which is in-order always)");
        page->_settings.WindowSettingsDefaults().TabSwitcherMode(TabSwitcherMode::Disabled);

        Log::Comment(L"Switch to the next in-order tab: Tab[2]");
        TestOnUIThread([&page]() {
            page->_SelectNextTab(true, nullptr);
        });
        TestOnUIThread([&page]() {
            auto focusedIndex = page->_GetFocusedTabIndex().value_or(-1);
            VERIFY_ARE_EQUAL(2u, focusedIndex, L"Verify Tab[2] is focused");
        });

        Log::Comment(L"Switch to the next in-order tab: Tab[3]");
        TestOnUIThread([&page]() {
            page->_SelectNextTab(true, nullptr);
        });
        TestOnUIThread([&page]() {
            auto focusedIndex = page->_GetFocusedTabIndex().value_or(-1);
            VERIFY_ARE_EQUAL(3u, focusedIndex, L"Verify Tab[3] is focused");
        });
    }

    void TabTests::VerifyCommandPaletteTabSwitcherOrder()
    {
        // This is a test for GH#8188 - we want to make sure that the MRU
        // ordering is correctly maintained as tabs are selected.
        //
        // Note: We verify MRU ordering directly rather than going through
        // the command palette tab switcher, because the palette's anchor key
        // handling auto-dismisses when no modifier keys are held (which we
        // can't simulate in the test environment).

        auto page = _commonSetup();

        Log::Comment(L"Create 3 additional tabs");
        RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{ 1 };
            page->_OpenNewTab(newTerminalArgs);
            page->_OpenNewTab(newTerminalArgs);
            page->_OpenNewTab(newTerminalArgs);
        });
        VERIFY_ARE_EQUAL(4u, page->_mruTabs.Size());

        Log::Comment(L"give alphabetical names to all tabs");
        TestOnUIThread([&page]() {
            page->_GetTabImpl(page->_tabs.GetAt(0))->Title(L"a");
        });
        TestOnUIThread([&page]() {
            page->_GetTabImpl(page->_tabs.GetAt(1))->Title(L"b");
        });
        TestOnUIThread([&page]() {
            page->_GetTabImpl(page->_tabs.GetAt(2))->Title(L"c");
        });
        TestOnUIThread([&page]() {
            page->_GetTabImpl(page->_tabs.GetAt(3))->Title(L"d");
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Sanity check the titles of our tabs are what we set them to.");

            VERIFY_ARE_EQUAL(L"a", page->_tabs.GetAt(0).Title());
            VERIFY_ARE_EQUAL(L"b", page->_tabs.GetAt(1).Title());
            VERIFY_ARE_EQUAL(L"c", page->_tabs.GetAt(2).Title());
            VERIFY_ARE_EQUAL(L"d", page->_tabs.GetAt(3).Title());

            // MRU order after creating Tab[0]-Tab[3]: MRU[0]=Tab[3], MRU[3]=Tab[0]
            VERIFY_ARE_EQUAL(L"d", page->_mruTabs.GetAt(0).Title());
            VERIFY_ARE_EQUAL(L"c", page->_mruTabs.GetAt(1).Title());
            VERIFY_ARE_EQUAL(L"b", page->_mruTabs.GetAt(2).Title());
            VERIFY_ARE_EQUAL(L"a", page->_mruTabs.GetAt(3).Title());
        });

        Log::Comment(L"Select Tab[0] through Tab[3] to establish MRU order");
        RunOnUIThread([&page]() {
            page->_UpdatedSelectedTab(page->_tabs.GetAt(0));
            page->_UpdatedSelectedTab(page->_tabs.GetAt(1));
            page->_UpdatedSelectedTab(page->_tabs.GetAt(2));
            page->_UpdatedSelectedTab(page->_tabs.GetAt(3));
        });

        Log::Comment(L"Verify MRU order: MRU[0]='d', MRU[1]='c', MRU[2]='b', MRU[3]='a'");
        VERIFY_ARE_EQUAL(4u, page->_mruTabs.Size());
        VERIFY_ARE_EQUAL(L"d", page->_mruTabs.GetAt(0).Title());
        VERIFY_ARE_EQUAL(L"c", page->_mruTabs.GetAt(1).Title());
        VERIFY_ARE_EQUAL(L"b", page->_mruTabs.GetAt(2).Title());
        VERIFY_ARE_EQUAL(L"a", page->_mruTabs.GetAt(3).Title());

        Log::Comment(L"Select Tab[2]='c' (MRU[1] after 'd')");
        TestOnUIThread([&page]() {
            page->_SelectTab(2);
        });

        Log::Comment(L"Verify MRU order updated: MRU[0]='c', MRU[1]='d', MRU[2]='b', MRU[3]='a'");
        TestOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(L"c", page->_mruTabs.GetAt(0).Title());
            VERIFY_ARE_EQUAL(L"d", page->_mruTabs.GetAt(1).Title());
            VERIFY_ARE_EQUAL(L"b", page->_mruTabs.GetAt(2).Title());
            VERIFY_ARE_EQUAL(L"a", page->_mruTabs.GetAt(3).Title());
        });
    }

    void TabTests::TestWindowRenameSuccessful()
    {
        BEGIN_TEST_METHOD_PROPERTIES()
            TEST_METHOD_PROPERTY(L"IsolationLevel", L"Method")
        END_TEST_METHOD_PROPERTIES()

        auto page = _commonSetup();
        page->RenameWindowRequested([&page, this](auto&&, const winrt::TerminalApp::RenameWindowRequestedArgs args) {
            // In the real terminal, this would bounce up to the monarch and
            // come back down. Instead, immediately call back and set the name.
            //
            // This replicates how TerminalWindow works
            _windowProperties->WindowName(args.ProposedName());
        });

        auto windowNameChanged = false;
        _windowProperties->PropertyChanged([&page, &windowNameChanged](auto&&, const winrt::WUX::Data::PropertyChangedEventArgs& args) mutable {
            if (args.PropertyName() == L"WindowNameForDisplay")
            {
                windowNameChanged = true;
            }
        });

        TestOnUIThread([&page]() {
            page->_RequestWindowRename(winrt::hstring{ L"Foo" });
        });
        TestOnUIThread([&]() {
            VERIFY_ARE_EQUAL(L"Foo", page->WindowProperties().WindowName());
            VERIFY_IS_TRUE(windowNameChanged,
                           L"The window name should have changed, and we should have raised a notification that WindowNameForDisplay changed");
        });
    }
    void TabTests::TestWindowRenameFailure()
    {
        BEGIN_TEST_METHOD_PROPERTIES()
            TEST_METHOD_PROPERTY(L"IsolationLevel", L"Method")
        END_TEST_METHOD_PROPERTIES()

        auto page = _commonSetup();
        auto windowNameChanged = false;

        page->PropertyChanged([&page, &windowNameChanged](auto&&, const winrt::WUX::Data::PropertyChangedEventArgs& args) mutable {
            if (args.PropertyName() == L"WindowNameForDisplay")
            {
                windowNameChanged = true;
            }
        });

        TestOnUIThread([&page]() {
            page->_RequestWindowRename(winrt::hstring{ L"Foo" });
        });
        TestOnUIThread([&]() {
            VERIFY_IS_FALSE(windowNameChanged,
                            L"The window name should not have changed, we should have rejected the change.");
        });
    }

    static til::color _getControlBackgroundColor(winrt::TerminalApp::implementation::ContentManager* contentManager,
                                                 const winrt::Microsoft::Terminal::Control::TermControl& c)
    {
        auto interactivity{ contentManager->TryLookupCore(c.ContentId()) };
        VERIFY_IS_NOT_NULL(interactivity);
        const auto core{ interactivity.Core() };
        return til::color{ core.BackgroundColor() };
    }

    void TabTests::TestPreviewCommitScheme()
    {
        Log::Comment(L"Preview a color scheme. Make sure it's applied, then committed accordingly");

        auto page = _commonSetup();
        VERIFY_IS_NOT_NULL(page);

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xff0c0c0c }, backgroundColor);
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Emulate previewing the SetColorScheme action");
            SetColorSchemeArgs args{ L"Vintage" };
            ActionAndArgs actionAndArgs{ ShortcutAction::SetColorScheme, args };
            page->_PreviewAction(actionAndArgs);
        });

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            Log::Comment(L"Color should be changed to the preview");
            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xff000000 }, backgroundColor);

            // And we should have stored a function to revert the change.
            VERIFY_ARE_EQUAL(1u, page->_restorePreviewFuncs.size());
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Emulate committing the SetColorScheme action");

            SetColorSchemeArgs args{ L"Vintage" };
            page->_EndPreview();
            page->_HandleSetColorScheme(nullptr, ActionEventArgs{ args });
        });

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            Log::Comment(L"Color should be changed");
            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xff000000 }, backgroundColor);

            // After preview there should be no more restore functions to execute.
            VERIFY_ARE_EQUAL(0u, page->_restorePreviewFuncs.size());
        });

        Log::Comment(L"Sleep to let events propagate");
        // If you don't do this, we will _sometimes_ crash as we're tearing down
        // the control from this test as we start the next one. We crash
        // somewhere in the CursorPositionChanged handler. It's annoying, but
        // this works.
        Sleep(250);
    }

    void TabTests::TestPreviewDismissScheme()
    {
        Log::Comment(L"Preview a color scheme. Make sure it's applied, then dismissed accordingly");

        auto page = _commonSetup();
        VERIFY_IS_NOT_NULL(page);

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xff0c0c0c }, backgroundColor);
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Emulate previewing the SetColorScheme action");
            SetColorSchemeArgs args{ L"Vintage" };
            ActionAndArgs actionAndArgs{ ShortcutAction::SetColorScheme, args };
            page->_PreviewAction(actionAndArgs);
        });

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            Log::Comment(L"Color should be changed to the preview");
            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xff000000 }, backgroundColor);
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Emulate dismissing the SetColorScheme action");
            page->_EndPreview();
        });

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            Log::Comment(L"Color should be the same as it originally was");
            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xff0c0c0c }, backgroundColor);
        });
        Log::Comment(L"Sleep to let events propagate");
        Sleep(250);
    }

    void TabTests::TestPreviewSchemeWhilePreviewing()
    {
        Log::Comment(L"Preview a color scheme, then preview another scheme. ");

        Log::Comment(L"Preview a color scheme. Make sure it's applied, then committed accordingly");

        auto page = _commonSetup();
        VERIFY_IS_NOT_NULL(page);

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xff0c0c0c }, backgroundColor);
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Emulate previewing the SetColorScheme action");
            SetColorSchemeArgs args{ L"Vintage" };
            page->_PreviewColorScheme(args);
        });

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            Log::Comment(L"Color should be changed to the preview");
            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xff000000 }, backgroundColor);
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Now, preview another scheme");
            SetColorSchemeArgs args{ L"One Half Light" };
            page->_PreviewColorScheme(args);
        });

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            Log::Comment(L"Color should be changed to the preview");
            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xffFAFAFA }, backgroundColor);
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Emulate committing the SetColorScheme action");

            SetColorSchemeArgs args{ L"One Half Light" };
            page->_EndPreview();
            page->_HandleSetColorScheme(nullptr, ActionEventArgs{ args });
        });

        TestOnUIThread([&page, this]() {
            const auto& activeControl{ page->_GetActiveControl() };
            VERIFY_IS_NOT_NULL(activeControl);

            Log::Comment(L"Color should be changed");
            const auto backgroundColor{ _getControlBackgroundColor(_contentManager.get(), activeControl) };
            VERIFY_ARE_EQUAL(til::color{ 0xffFAFAFA }, backgroundColor);
        });
        Log::Comment(L"Sleep to let events propagate");
        Sleep(250);
    }

    void TabTests::TestClampSwitchToTab()
    {
        Log::Comment(L"Test that switching to a tab index higher than the number of tabs just clamps to the last tab.");

        auto page = _commonSetup();
        VERIFY_IS_NOT_NULL(page);

        Log::Comment(L"Create a second tab");
        TestOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{ 1 };
            page->_OpenNewTab(newTerminalArgs);
        });
        VERIFY_ARE_EQUAL(2u, page->_tabs.Size());

        Log::Comment(L"Create a third tab");
        TestOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{ 2 };
            page->_OpenNewTab(newTerminalArgs);
        });
        VERIFY_ARE_EQUAL(3u, page->_tabs.Size());

        TestOnUIThread([&page]() {
            auto focusedTabIndexOpt{ page->_GetFocusedTabIndex() };
            VERIFY_IS_TRUE(focusedTabIndexOpt.has_value());
            VERIFY_ARE_EQUAL(2u, focusedTabIndexOpt.value());
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Switch to the first tab");
            page->_SelectTab(0);
        });

        TestOnUIThread([&page]() {
            auto focusedTabIndexOpt{ page->_GetFocusedTabIndex() };

            VERIFY_IS_TRUE(focusedTabIndexOpt.has_value());
            VERIFY_ARE_EQUAL(0u, focusedTabIndexOpt.value());
        });

        TestOnUIThread([&page]() {
            Log::Comment(L"Switch to the tab 6, which is greater than number of tabs. This should switch to the third tab");
            page->_SelectTab(6);
        });

        TestOnUIThread([&page]() {
            auto focusedTabIndexOpt{ page->_GetFocusedTabIndex() };
            VERIFY_IS_TRUE(focusedTabIndexOpt.has_value());
            VERIFY_ARE_EQUAL(2u, focusedTabIndexOpt.value());
        });
    }

}
