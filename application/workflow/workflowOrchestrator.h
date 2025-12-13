/* Copyright (c) 2025 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>

#include "workflowTypes.h"

namespace AIAssistant
{
    class WorkflowRegistry;

    // -------------------------------------------------------------------------
    // WorkflowOrchestrator
    // -------------------------------------------------------------------------
    // Responsibilities (v1, synchronous, single-threaded orchestrator):
    //  * Use WorkflowRegistry to look up WorkflowDefinition by id
    //  * Create WorkflowRun instances (ephemeral run state)
    //  * Perform dependency readiness checks based on depends_on
    //  * Perform Makefile-style freshness checks based on file_inputs / file_outputs
    //  * Execute tasks (currently as synchronous stubs with clear hook points)
    //  * Track last completed run per workflow for inspection (UI, tests)
    //
    // Notes:
    //  * This orchestrator is intentionally single-threaded. It runs in the
    //    caller's thread (for example, CLI command or a dedicated worker thread).
    //  * Parallel / asynchronous dispatch can be added later by using the
    //    Core::GetThreadPool() API, but that requires careful synchronization
    //    around WorkflowRun mutation to avoid data races.
    // -------------------------------------------------------------------------
    class WorkflowOrchestrator
    {
    public:
        static WorkflowOrchestrator& Get();

        // Attach a registry that owns the loaded workflows.
        // The pointer is not owned; the caller is responsible for ensuring
        // that the registry outlives the orchestrator.
        void SetRegistry(WorkflowRegistry const* workflowRegistry);

        // Returns a list of known workflow ids (as seen through the registry).
        std::vector<std::string> GetWorkflowIds() const;

        // Start and run a workflow to completion (synchronously).
        // Returns true on success (all tasks either succeeded or were skipped).
        // Returns false if the workflow is unknown or if any task fails.
        bool RunWorkflowOnce(std::string const& workflowId, std::string const& runId = std::string());

        // Access the last completed run for inspection (UI, tests).
        bool TryGetLastRun(std::string const& workflowId, WorkflowRun& outRun) const;

    private:
        WorkflowOrchestrator() = default;

        bool ExecuteWorkflow(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun);

        bool ExecuteOneReadyWave(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                 bool& outMadeProgress);

        bool IsTaskReady(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                         TaskDef const& taskDefinition) const;

        bool ExecuteTaskInstance(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                 TaskDef const& taskDefinition, std::string const& taskId, TaskInstanceState& taskState);

        std::string GenerateRunId(WorkflowDefinition const& workflowDefinition) const;

    private:
        WorkflowRegistry const* m_WorkflowRegistry{nullptr};

        // Map: workflow id -> last completed run for that workflow.
        std::unordered_map<std::string, WorkflowRun> m_LastRuns;
    };

} // namespace AIAssistant
