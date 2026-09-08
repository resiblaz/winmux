// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "TaskBoard.h"
#include "TaskStoreCommandSqlite.h"
#include "TaskStoreMessageSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    struct BoardTaskState
    {
        StoredTaskState Task;
        WorkspaceId Workspace;
        ExecutionContextId Context;
        bool ArchivedProject{};
        bool PendingBoardOperation{};
        std::optional<StoredActiveTaskRun> ActiveRun;
        std::optional<TaskIntentRecord> Intent;
    };
    [[nodiscard]] bool LoadBoardTask(sqlite3* db, const TaskId& id, std::optional<BoardTaskState>& task, std::string& error);
    [[nodiscard]] bool ReadBoardRevision(sqlite3* db, const WorkspaceId& workspace, TaskStatus status, uint64_t& revision, std::string& error);
    [[nodiscard]] bool PlaceBoardTask(sqlite3* db, const TaskBoardCommand& command, TaskStatus status, std::string& error);
    [[nodiscard]] bool StartBoardRun(sqlite3* db, const TaskBoardCommand& command, const BoardTaskState& task, std::string& error);
    [[nodiscard]] bool BeginBoardIntent(sqlite3* db, const TaskBoardCommand& command, TaskIntentKind kind, bool done, std::string& error);
    [[nodiscard]] bool UpdateBoardTask(sqlite3* db, const TaskId& task, const StoredTaskState& current, TaskStatus status, bool hold, int64_t timestamp, std::string& error);
    [[nodiscard]] bool ReadBoardReceipt(sqlite3* db, const TaskBoardCommand& command, std::optional<TaskBoardCommandResult>& result, std::string& error);
    [[nodiscard]] bool WriteBoardReceipt(sqlite3* db, const TaskBoardCommand& command, const TaskBoardCommandResult& result, std::string& error);
    struct BoardUndoSnapshot
    {
        std::optional<std::string> NextTask;
        std::optional<std::string> Note;
        int64_t NoteKind{};
        int64_t NoteCreatedAt{};
    };
    [[nodiscard]] bool CaptureBoardUndo(sqlite3* db, const TaskId& task, BoardUndoSnapshot& snapshot, std::string& error);
    [[nodiscard]] bool SaveBoardUndo(sqlite3* db, const TaskBoardCommand& command, const BoardTaskState& before,
                                     const BoardUndoSnapshot& snapshot, const TaskBoardCommandResult& after, std::string& error);
    [[nodiscard]] TaskBoardCommandResult ApplyBoardUndo(sqlite3* db, const TaskBoardCommand& command, const BoardTaskState& current);
}
