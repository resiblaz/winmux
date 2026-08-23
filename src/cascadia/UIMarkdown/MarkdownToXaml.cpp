// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CodeBlock.h"
#include "MarkdownToXaml.h"

#include <cmark-gfm.h>
#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm-extension_api.h>

#include <mutex>

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}
using namespace winrt;

// Bullet points used for unordered lists.
static constexpr std::wstring_view bullets[]{
    L"• ",
    L"◦ ",
    L"▪ " // After this level, we'll keep using this one.
};
static constexpr int WidthOfBulletPoint{ 9 };
static constexpr int IndentWidth{ 3 * WidthOfBulletPoint };
static constexpr int H1FontSize{ 36 };
static constexpr int HeaderMinFontSize{ 16 };
static constexpr int CompactHeaderFontSizes[]{ 16, 15, 14, 13, 13, 13 };
static constexpr double CompactBodyFontSize{ 12.0 };

static constexpr std::wstring_view CodeFontFamily{ L"Cascadia Mono, Consolas" };

template<typename T>
static std::string_view textFromCmarkString(const T& s) noexcept
{
    return std::string_view{ (char*)s.data, (size_t)s.len };
}
static std::string_view textFromLiteral(cmark_node* node) noexcept
{
    return cmark_node_get_literal(node);
}
static std::string_view textFromUrl(cmark_node* node) noexcept
{
    return cmark_node_get_url(node);
}
static std::string_view typeFromNode(cmark_node* node) noexcept
{
    const auto type = cmark_node_get_type_string(node);
    return type == nullptr ? std::string_view{} : std::string_view{ type };
}

typedef wil::unique_any<cmark_node*, decltype(&cmark_node_free), cmark_node_free> unique_node;
typedef wil::unique_any<cmark_iter*, decltype(&cmark_iter_free), cmark_iter_free> unique_iter;
typedef wil::unique_any<cmark_parser*, decltype(&cmark_parser_free), cmark_parser_free> unique_parser;

// Function Description:
// - Entrypoint to convert a string of markdown into a XAML RichTextBlock.
// Arguments:
// - markdownText: the markdown content to render
// - baseUrl: the current URI of the content. This will allow for relative links
//   to be appropriately resolved.
// Return Value:
// - a RichTextBlock with the rendered markdown in it.
WUX::Controls::RichTextBlock MarkdownToXaml::Convert(std::string_view markdownText,
                                                     const winrt::hstring& baseUrl,
                                                     const MarkdownRenderMode renderMode)
{
    MarkdownToXaml data{ baseUrl, renderMode };

    static std::once_flag extensionsRegistration;
    std::call_once(extensionsRegistration, []() {
        cmark_gfm_core_extensions_ensure_registered();
    });

    unique_parser parser{ cmark_parser_new(CMARK_OPT_DEFAULT) };
    THROW_HR_IF_NULL(E_FAIL, parser.get());

    if (renderMode == MarkdownRenderMode::CompactSafe)
    {
        for (const auto extensionName : { "table", "strikethrough", "autolink", "tasklist" })
        {
            const auto extension = cmark_find_syntax_extension(extensionName);
            THROW_HR_IF(E_FAIL, extension == nullptr || !cmark_parser_attach_syntax_extension(parser.get(), extension));
        }
    }

    cmark_parser_feed(parser.get(), markdownText.data(), markdownText.size());
    unique_node doc{ cmark_parser_finish(parser.get()) };
    THROW_HR_IF_NULL(E_FAIL, doc.get());
    unique_iter iter{ cmark_iter_new(doc.get()) };
    THROW_HR_IF_NULL(E_FAIL, iter.get());
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter.get())) != CMARK_EVENT_DONE)
    {
        data._RenderNode(cmark_iter_get_node(iter.get()), ev_type);
    }

    return data._root;
}

MarkdownToXaml::MarkdownToXaml(const winrt::hstring& baseUrl, const MarkdownRenderMode renderMode) :
    _baseUri{ baseUrl },
    _renderMode{ renderMode }
{
    _root.ContextFlyout(winrt::Microsoft::Terminal::UI::TextMenuFlyout{});
    _root.IsTextSelectionEnabled(true);
    _root.TextWrapping(WUX::TextWrapping::WrapWholeWords);
    if (_isCompactSafeMode())
    {
        _root.FontSize(CompactBodyFontSize);
    }
}

bool MarkdownToXaml::_isCompactSafeMode() const noexcept
{
    return _renderMode == MarkdownRenderMode::CompactSafe;
}

bool MarkdownToXaml::_isAllowedCompactLink(const winrt::Windows::Foundation::Uri& uri) const noexcept
{
    const auto scheme = uri.SchemeName();
    return til::equals_insensitive_ascii(scheme, L"https") ||
           til::equals_insensitive_ascii(scheme, L"http") ||
           til::equals_insensitive_ascii(scheme, L"mailto");
}

WUX::Documents::Paragraph MarkdownToXaml::_CurrentParagraph()
{
    if (_lastParagraph == nullptr)
    {
        _lastParagraph = WUX::Documents::Paragraph{};
        if (_indent > 0)
        {
            // If we're in a list, we will start this paragraph with a bullet
            // point. That bullet point will be added as part of the actual text
            // of the paragraph, but we want the real text of the paragraph all
            // aligned. So we will _de-indent_ the first line, to give us space
            // for the bullet.
            if (_indent - _blockQuoteDepth > 0)
            {
                _lastParagraph.TextIndent(-WidthOfBulletPoint);
            }
            _lastParagraph.Margin(WUX::ThicknessHelper::FromLengths(static_cast<double>(IndentWidth) * _indent, 0, 0, 0));
        }
        _root.Blocks().Append(_lastParagraph);
    }
    return _lastParagraph;
}
WUX::Documents::Run MarkdownToXaml::_CurrentRun()
{
    if (_lastRun == nullptr)
    {
        _lastRun = WUX::Documents::Run{};
        _CurrentSpan().Inlines().Append(_lastRun);
    }
    return _lastRun;
}
WUX::Documents::Span MarkdownToXaml::_CurrentSpan()
{
    if (_lastSpan == nullptr)
    {
        _lastSpan = WUX::Documents::Span{};
        _CurrentParagraph().Inlines().Append(_lastSpan);
    }
    return _lastSpan;
}
WUX::Documents::Run MarkdownToXaml::_NewRun()
{
    if (_lastRun == nullptr)
    {
        return _CurrentRun();
    }
    else
    {
        auto old{ _lastRun };

        WUX::Documents::Run newRun{};

        newRun.FontFamily(old.FontFamily());
        newRun.FontWeight(old.FontWeight());
        newRun.FontStyle(old.FontStyle());
        newRun.TextDecorations(old.TextDecorations());

        _lastRun = newRun;
        _CurrentSpan().Inlines().Append(_lastRun);
    }
    return _lastRun;
}
void MarkdownToXaml::_EndRun() noexcept
{
    _lastRun = nullptr;
}
void MarkdownToXaml::_EndSpan() noexcept
{
    _EndRun();
    _lastSpan = nullptr;
}
void MarkdownToXaml::_EndParagraph() noexcept
{
    _EndSpan();
    _lastParagraph = nullptr;
}

WUX::Controls::TextBlock MarkdownToXaml::_makeDefaultTextBlock()
{
    WUX::Controls::TextBlock b{};
    b.ContextFlyout(winrt::Microsoft::Terminal::UI::TextMenuFlyout{});
    b.IsTextSelectionEnabled(true);
    b.TextWrapping(WUX::TextWrapping::WrapWholeWords);
    return b;
}

bool MarkdownToXaml::_RenderExtensionNode(cmark_node* node, const cmark_event_type ev_type)
{
    const bool entering = ev_type == CMARK_EVENT_ENTER;
    const auto type = typeFromNode(node);

    if (type == "table")
    {
        _EndParagraph();
        return true;
    }

    if (type == "table_header" || type == "table_row")
    {
        _EndParagraph();
        if (entering && type == "table_header")
        {
            _CurrentParagraph().FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        }
        return true;
    }

    if (type == "table_cell")
    {
        if (entering && cmark_node_previous(node) != nullptr)
        {
            _NewRun().Text(L"  │  ");
        }
        return true;
    }

    if (type == "strikethrough")
    {
        _NewRun().TextDecorations(entering ?
                                      winrt::Windows::UI::Text::TextDecorations::Strikethrough :
                                      winrt::Windows::UI::Text::TextDecorations::None);
        return true;
    }

    return false;
}

void MarkdownToXaml::_RenderNode(cmark_node* node, cmark_event_type ev_type)
{
    const bool entering = (ev_type == CMARK_EVENT_ENTER);

    if (_RenderExtensionNode(node, ev_type))
    {
        return;
    }

    switch (cmark_node_get_type(node))
    {
    case CMARK_NODE_DOCUMENT:
        break;

    case CMARK_NODE_BLOCK_QUOTE:

        // It's non-trivial to deal with the right-side vertical lines that
        // we're accustomed to seeing for block quotes in markdown content.
        // RichTextBlock doesn't have a good way of adding a border to a
        // paragraph, it would seem.
        //
        // We could add a InlineUIContainer, with a Border in there, then
        // put a new RichTextBlock in there, but I believe text selection
        // wouldn't transit across the border.

        // Instead, we're just going to add a new layer of indenting.

        if (entering)
        {
            _EndParagraph();
            _indent++;
            _blockQuoteDepth++;
        }
        else
        {
            _EndParagraph();
            _indent = std::max(0, _indent - 1);
            _blockQuoteDepth = std::max(0, _blockQuoteDepth - 1);
        }

        break;

    case CMARK_NODE_LIST:
    {
        // Track the list type and starting number so nested ordered and
        // unordered lists can each render their own marker correctly.
        if (entering)
        {
            _EndParagraph();
            _indent++;
            _listStack.emplace_back(ListContext{
                .Ordered = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST,
                .NextNumber = cmark_node_get_list_start(node),
            });
        }
        else
        {
            _EndParagraph();
            _indent = std::max(0, _indent - 1);
            if (!_listStack.empty())
            {
                _listStack.pop_back();
            }
        }
        break;
    }

    case CMARK_NODE_ITEM:
        // A list item, either for an ordered list or an unordered one.
        if (entering)
        {
            _EndParagraph();
            if (typeFromNode(node) == "tasklist")
            {
                const auto taskMarker = cmark_gfm_extensions_get_tasklist_item_checked(node) ? L"☑" : L"☐";
                if (!_listStack.empty() && _listStack.back().Ordered)
                {
                    auto& context = _listStack.back();
                    _NewRun().Text(winrt::hstring{ fmt::format(FMT_COMPILE(L"{}. {} "), context.NextNumber++, taskMarker) });
                }
                else
                {
                    _NewRun().Text(winrt::hstring{ fmt::format(FMT_COMPILE(L"{} "), taskMarker) });
                }
            }
            else if (!_listStack.empty() && _listStack.back().Ordered)
            {
                auto& context = _listStack.back();
                _NewRun().Text(winrt::hstring{ fmt::format(FMT_COMPILE(L"{}. "), context.NextNumber++) });
            }
            else
            {
                _NewRun().Text(gsl::at(bullets, std::clamp(_indent - _blockQuoteDepth - 1, 0, 2)));
            }
        }
        break;

    case CMARK_NODE_HEADING:
    {
        _EndParagraph();

        // At the start of a header, change the font size to match the new
        // level of header we're at. The text will come later, in a
        // CMARK_NODE_TEXT
        if (entering)
        {
            const auto level = cmark_node_get_heading_level(node);
            if (_isCompactSafeMode())
            {
                const auto index = std::clamp(level - 1, 0, gsl::narrow_cast<int>(std::size(CompactHeaderFontSizes) - 1));
                _CurrentParagraph().FontSize(gsl::at(CompactHeaderFontSizes, index));
                _CurrentParagraph().FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            else
            {
                // Insert a blank line, just to help break up the walls of text.
                // This better reflects the way MD is rendered to HTML.
                _root.Blocks().Append(WUX::Documents::Paragraph{});
                _CurrentParagraph().FontSize(std::max(HeaderMinFontSize, H1FontSize - level * 6));
            }
        }
        break;
    }

    case CMARK_NODE_CODE_BLOCK:
    {
        _EndParagraph();

        const auto codeHstring{ winrt::to_hstring(cmark_node_get_literal(node)) };
        // The literal for a code node always includes the trailing newline.
        // Trim that off.
        const std::wstring_view codeView{ codeHstring.c_str(), codeHstring.size() - 1 };

        auto codeBlock = winrt::make<winrt::Microsoft::Terminal::UI::Markdown::implementation::CodeBlock>(winrt::hstring{ codeView });
        if (_isCompactSafeMode())
        {
            codeBlock.PlayButtonVisibility(WUX::Visibility::Collapsed);
        }
        WUX::Documents::InlineUIContainer codeContainer{};
        codeContainer.Child(codeBlock);
        _CurrentParagraph().Inlines().Append(codeContainer);

        _EndParagraph();
    }
    break;

    case CMARK_NODE_HTML_BLOCK:
        // Raw HTML comes to us in the literal
        //    node->as.literal.data, node->as.literal.len
        // But we don't support raw HTML, so we'll do nothing.
        break;

    case CMARK_NODE_CUSTOM_BLOCK:
        // Not even entirely sure what this is.
        break;

    case CMARK_NODE_THEMATIC_BREAK:
        if (entering)
        {
            _EndParagraph();
            const auto& divider = _NewRun();
            divider.Text(L"────────");
            _EndParagraph();
        }
        break;

    case CMARK_NODE_PARAGRAPH:
    {
        bool tight;
        cmark_node* parent = cmark_node_parent(node);
        cmark_node* grandparent = cmark_node_parent(parent);

        if (grandparent != nullptr && cmark_node_get_type(grandparent))
        {
            tight = cmark_node_get_list_tight(grandparent);
        }
        else
        {
            tight = false;
        }

        // If we aren't in a list, then end the current paragraph and
        // start a new one.
        if (!tight)
        {
            _EndParagraph();
        }

        // Start a new paragraph if we don't have one
        break;
    }
    case CMARK_NODE_TEXT:
    {
        const auto text{ winrt::to_hstring(textFromLiteral(node)) };

        if (_lastImage)
        {
            // The tooltip for an image comes in as a CMARK_NODE_TEXT, so set that here.
            WUX::Controls::ToolTipService::SetToolTip(_lastImage, box_value(text));
        }
        else
        {
            // Otherwise, just add the text to the current paragraph
            _NewRun().Text(text);
        }
    }

    break;

    case CMARK_NODE_LINEBREAK:
        _EndSpan();
        _CurrentParagraph().Inlines().Append(WUX::Documents::LineBreak());
        break;

    case CMARK_NODE_SOFTBREAK:
        // I'm fairly confident this is what happens when you've just got
        // two lines only separated by a single \r\n in a MD doc. E.g. when
        // you want a paragraph to wrap at 80 columns in code, but wrap in
        // the rendered document.
        //
        // In the HTML implementation, what happens here depends on the options:
        // * CMARK_OPT_HARDBREAKS: add a full line break
        // * CMARK_OPT_NOBREAKS: Just add a space
        // * otherwise, just add a '\n'
        //
        // We're not really messing with options here, so lets just add a
        // space. That seems to keep the current line going, but allow for
        // word breaking.

        _NewRun().Text(L" ");
        break;

    case CMARK_NODE_CODE:
    {
        const auto text{ winrt::to_hstring(textFromLiteral(node)) };
        const auto& codeRun{ _NewRun() };

        codeRun.FontFamily(WUX::Media::FontFamily{ CodeFontFamily });
        // A Span can't have a border or a background, so we can't give
        // it the whole treatment that a <code> span gets in HTML.
        codeRun.Text(text);

        _NewRun().FontFamily(_root.FontFamily());
    }
    break;

    case CMARK_NODE_HTML_INLINE:
        // Same as above - no raw HTML support here.
        break;

    case CMARK_NODE_CUSTOM_INLINE:
        // Same as above - not even entirely sure what this is.
        break;

    case CMARK_NODE_STRONG:
        _NewRun().FontWeight(entering ?
                                 winrt::Windows::UI::Text::FontWeights::Bold() :
                                 winrt::Windows::UI::Text::FontWeights::Normal());
        break;

    case CMARK_NODE_EMPH:
        _NewRun().FontStyle(entering ?
                                winrt::Windows::UI::Text::FontStyle::Italic :
                                winrt::Windows::UI::Text::FontStyle::Normal);
        break;

    case CMARK_NODE_LINK:

        if (entering)
        {
            const auto urlHstring{ to_hstring(textFromUrl(node)) };
            bool linkCreated = false;

            // Set the tooltip to display the URL
            try
            {
                // This block is from TermControl.cpp, where we sanitize the
                // tooltips for URLs. That has a much more comprehensive
                // comment.

                const auto uri = _isCompactSafeMode() ?
                                     winrt::Windows::Foundation::Uri{ urlHstring } :
                                     winrt::Windows::Foundation::Uri{ _baseUri, urlHstring };
                if (_isCompactSafeMode() && !_isAllowedCompactLink(uri))
                {
                    _linkStack.emplace_back(false);
                    break;
                }

                WUX::Documents::Hyperlink a{};
                a.NavigateUri(uri);

                auto tooltipText = urlHstring;
                const auto unicode = uri.AbsoluteUri();
                const auto punycode = uri.AbsoluteCanonicalUri();
                if (punycode != unicode)
                {
                    tooltipText = winrt::hstring{ punycode + L"\n" + unicode };
                }
                WUX::Controls::ToolTipService::SetToolTip(a, box_value(tooltipText));

                _CurrentParagraph().Inlines().Append(a);
                _lastSpan = a;
                linkCreated = true;
            }
            catch (...)
            {
            }

            _linkStack.emplace_back(linkCreated);

            // Similar to the header element, the actual text of the link
            // will later come through as a CMARK_NODE_TEXT
        }
        else
        {
            if (!_linkStack.empty())
            {
                if (_linkStack.back())
                {
                    _EndSpan();
                }
                _linkStack.pop_back();
            }
        }
        break;

    case CMARK_NODE_IMAGE:
        if (_isCompactSafeMode())
        {
            // Conclusions intentionally do not load images. The image alt text
            // still arrives as CMARK_NODE_TEXT and remains selectable.
            break;
        }
        if (entering)
        {
            const auto urlHstring{ to_hstring(textFromUrl(node)) };

            try
            {
                winrt::Windows::Foundation::Uri uri{ _baseUri, urlHstring };

                WUX::Media::Imaging::BitmapImage bitmapImage;
                bitmapImage.UriSource(uri);

                WUX::Controls::Image img{};
                img.Source(bitmapImage);

                WUX::Documents::InlineUIContainer imageBlock{};
                imageBlock.Child(img);

                _CurrentParagraph().Inlines().Append(imageBlock);
                _lastImage = img;
            }
            catch (...)
            {
            }
        }
        else
        {
            _EndSpan();
            _lastImage = nullptr;
        }
        break;

    default:
        assert(false);
        break;
    }
}
