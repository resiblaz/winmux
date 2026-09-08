// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "ProjectNavigationState.h"
#include "WorkspaceRpcJson.h"
#include <unordered_set>

namespace winrt::TerminalApp::implementation::projects
{
    std::string ProjectNavigationState::SerializeLayout() const
    {
        Json::Value root{ Json::objectValue }; root["version"] = 1; root["active"] = ActiveProjectId;
        root["open"] = Json::Value{ Json::arrayValue };
        for (const auto& id : OpenProjects)
        {
            Json::Value item{ Json::objectValue }; item["id"] = id;
            const auto view = Views.find(id); item["view"] = view == Views.end() ? 0 : static_cast<uint32_t>(view->second);
            root["open"].append(std::move(item));
        }
        Json::StreamWriterBuilder writer; writer["indentation"] = "";
        return Json::writeString(writer, root);
    }
    bool ProjectNavigationState::RestoreLayout(std::string_view json)
    {
        struct Layout { std::vector<std::string> Open; std::string Active; std::map<std::string, ProjectView> Views; };
        using namespace protocol::workspace_json;
        if (json.empty() || json.size() > 65536) return false;
        const auto result = Decode<Layout>(json, [](const Json::Value& root, Layout& value) {
            uint64_t version{};
            if (!Exact(root, { "version", "active", "open" }) || !UInt(root["version"], version, 1) || version != 1 ||
                !Id(root["active"], value.Active, true) || !root["open"].isArray() || root["open"].size() > 256) return false;
            for (const auto& item : root["open"])
            {
                std::string id; ProjectView view{};
                if (!Exact(item, { "id", "view" }) || !Id(item["id"], id) || !Enum(item["view"], view, 3) || value.Views.contains(id)) return false;
                value.Open.push_back(id); value.Views.emplace(std::move(id), view);
            }
            return value.Active.empty() || value.Views.contains(value.Active);
        });
        if (!result.Succeeded()) return false;
        OpenProjects = result.Value.Open; ActiveProjectId = result.Value.Active; Views = result.Value.Views; return true;
    }
}
