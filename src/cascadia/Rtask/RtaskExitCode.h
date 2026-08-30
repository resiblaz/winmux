// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace Microsoft::Terminal::Rtask
{
    enum class RtaskExitCode
    {
        Success = 0,
        Usage = 2,
        MissingContext = 3,
        InvalidInput = 4,
        ConnectionFailure = 5,
        CommandRejected = 6,
        InternalFailure = 7,
    };
}
