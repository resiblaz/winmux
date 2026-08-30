// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RtaskStdin.h"

namespace Microsoft::Terminal::Rtask
{
    RtaskStdinResult ReadRtaskStdin(FILE* stream, const size_t maximumBytes)
    {
        RtaskStdinResult result;
        if (!stream || maximumBytes == 0)
        {
            result.Error = "rtask stdin configuration is invalid";
            return result;
        }
        try
        {
            std::array<char, 4096> buffer{};
            while (true)
            {
                const auto read = std::fread(
                    buffer.data(),
                    1,
                    buffer.size(),
                    stream);
                if (read != 0)
                {
                    if (read > maximumBytes -
                                   (std::min)(result.Text.size(), maximumBytes))
                    {
                        result.Text.clear();
                        result.Status = RtaskStdinStatus::Oversized;
                        result.Error = "rtask stdin exceeds the command limit";
                        return result;
                    }
                    result.Text.append(buffer.data(), read);
                }
                if (read < buffer.size())
                {
                    if (std::ferror(stream))
                    {
                        result.Text.clear();
                        result.Status = RtaskStdinStatus::ReadFailure;
                        result.Error = "rtask stdin read failed";
                        return result;
                    }
                    break;
                }
            }
            if (result.Text.empty())
            {
                result.Status = RtaskStdinStatus::Empty;
                result.Error = "rtask stdin is empty";
                return result;
            }
            result.Status = RtaskStdinStatus::Succeeded;
            return result;
        }
        catch (...)
        {
            result.Text.clear();
            result.Status = RtaskStdinStatus::ResourceFailure;
            result.Error = "rtask stdin exhausted resources";
            return result;
        }
    }
}
