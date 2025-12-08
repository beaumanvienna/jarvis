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

#include "workflowOrchestrator.h"

#include <algorithm>
#include <chrono>

#include "core.h"
#include "engine.h"
#include "workflowRegistry.h"
#include "dataflowResolver.h"
#include "taskExecutor.h"
#include "taskExecutorRegistry.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    WorkflowOrchestrator& WorkflowOrchestrator::Get()
    {
        static WorkflowOrchestrator instance;
        return instance;
    }

    void WorkflowOrchestrator::SetRegistry(WorkflowRegistry const* workflowRegistry)
    {
        m_WorkflowRegistry = workflowRegistry;
    }

    std::vector<std::string> WorkflowOrchestrator::GetWorkflowIds() const
    {
        std::vector<std::string> workflowIds;

        if (m_WorkflowRegistry == nullptr)
        {
            LOG_APP_WARN("WorkflowOrchestrator::GetWorkflowIds called without a registry");
            return workflowIds;
        }

        workflowIds = m_WorkflowRegistry->GetWorkflowIds();
        return workflowIds;
    }

    bool WorkflowOrchestrator::RunWorkflowOnce(std::string const& workflowId, std::string const& runId)
    {
        if (m_WorkflowRegistry == nullptr)
        {
            LOG_APP_ERROR("WorkflowOrchestrator::RunWorkflowOnce: No WorkflowRegistry attached");
            return false;
        }

        auto optionalDefinition = m_WorkflowRegistry->GetWorkflow(workflowId);
        if (!optionalDefinition.has_value())
        {
            LOG_APP_ERROR("WorkflowOrchestrator::RunWorkflowOnce: Unknown workflow id '{}'", workflowId);
            return false;
        }

        WorkflowDefinition const& workflowDefinition = optionalDefinition.value();

        WorkflowRun workflowRun;
        workflowRun.m_WorkflowId = workflowDefinition.m_Id;
        workflowRun.m_RunId = runId.empty() ? GenerateRunId(workflowDefinition) : runId;

        // Initialize task states
        for (auto const& taskPair : workflowDefinition.m_Tasks)
        {
            TaskInstanceState taskState;
            taskState.m_State = TaskInstanceStateKind::Pending;
            workflowRun.m_TaskStates[taskPair.first] = taskState;
        }

        bool const success = ExecuteWorkflow(workflowDefinition, workflowRun);

        // Store last completed run for inspection.
        m_LastRuns[workflowDefinition.m_Id] = workflowRun;

        return success;
    }

    bool WorkflowOrchestrator::TryGetLastRun(std::string const& workflowId, WorkflowRun& outRun) const
    {
        auto iterator = m_LastRuns.find(workflowId);
        if (iterator == m_LastRuns.end())
        {
            return false;
        }

        outRun = iterator->second;
        return true;
    }

    std::string WorkflowOrchestrator::GenerateRunId(WorkflowDefinition const& workflowDefinition) const
    {
        auto now = std::chrono::system_clock::now();
        auto nowTimeT = std::chrono::system_clock::to_time_t(now);

        std::string runId = workflowDefinition.m_Id;
        runId += "_";
        runId += std::to_string(static_cast<long long>(nowTimeT));

        return runId;
    }

    // ---------------------------------------------------------------------
    // Core execution loop (synchronous)
    // ---------------------------------------------------------------------

    bool WorkflowOrchestrator::ExecuteWorkflow(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun)
    {
        LOG_APP_INFO("WorkflowOrchestrator: Starting workflow '{}' (run id '{}')", workflowDefinition.m_Id,
                     workflowRun.m_RunId);

        bool overallSuccess = true;

        while (!workflowRun.m_IsCompleted)
        {
            bool madeProgress = false;

            if (!ExecuteOneReadyWave(workflowDefinition, workflowRun, madeProgress))
            {
                overallSuccess = false;
                break;
            }

            if (!madeProgress)
            {
                // No progress, but there might still be non-terminal tasks.
                bool hasActiveTasks = false;

                for (auto const& taskPair : workflowRun.m_TaskStates)
                {
                    TaskInstanceStateKind state = taskPair.second.m_State;
                    if (state == TaskInstanceStateKind::Pending || state == TaskInstanceStateKind::Ready ||
                        state == TaskInstanceStateKind::Running)
                    {
                        hasActiveTasks = true;
                        break;
                    }
                }

                if (hasActiveTasks)
                {
                    LOG_APP_CRITICAL("WorkflowOrchestrator: Deadlock or cycle detected in workflow '{}'",
                                     workflowDefinition.m_Id);
                    workflowRun.m_HasFailed = true;
                    overallSuccess = false;
                }

                workflowRun.m_IsCompleted = true;
            }
            else
            {
                bool allTerminal = true;

                for (auto const& taskPair : workflowRun.m_TaskStates)
                {
                    TaskInstanceStateKind state = taskPair.second.m_State;
                    if (state != TaskInstanceStateKind::Succeeded && state != TaskInstanceStateKind::Skipped &&
                        state != TaskInstanceStateKind::Failed)
                    {
                        allTerminal = false;
                        break;
                    }
                }

                if (allTerminal)
                {
                    workflowRun.m_IsCompleted = true;
                }
            }
        }

        if (workflowRun.m_HasFailed)
        {
            LOG_APP_ERROR("WorkflowOrchestrator: Workflow '{}' (run id '{}') finished with failure", workflowDefinition.m_Id,
                          workflowRun.m_RunId);
        }
        else
        {
            LOG_APP_INFO("WorkflowOrchestrator: Workflow '{}' (run id '{}') completed successfully", workflowDefinition.m_Id,
                         workflowRun.m_RunId);
        }

        return overallSuccess && !workflowRun.m_HasFailed;
    }

    bool WorkflowOrchestrator::ExecuteOneReadyWave(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                                   bool& outMadeProgress)
    {
        outMadeProgress = false;

        std::vector<std::pair<std::string, TaskInstanceState*>> readyTasks;

        // ---------------------------------------------------------
        // 1. Collect all ready tasks for this wave
        // ---------------------------------------------------------
        for (auto& taskPair : workflowRun.m_TaskStates)
        {
            std::string const& taskId = taskPair.first;
            TaskInstanceState* taskState = &taskPair.second;

            if (taskState->m_State != TaskInstanceStateKind::Pending && taskState->m_State != TaskInstanceStateKind::Ready)
            {
                continue;
            }

            auto defIt = workflowDefinition.m_Tasks.find(taskId);
            if (defIt == workflowDefinition.m_Tasks.end())
            {
                LOG_APP_ERROR("WorkflowOrchestrator: Task '{}' missing from workflow definition '{}'", taskId,
                              workflowDefinition.m_Id);
                taskState->m_State = TaskInstanceStateKind::Failed;
                workflowRun.m_HasFailed = true;
                outMadeProgress = true;
                continue;
            }

            TaskDef const& taskDefinition = defIt->second;

            // Structural readiness (dependency satisfaction)
            if (!IsTaskReady(workflowDefinition, workflowRun, taskDefinition))
            {
                continue;
            }

            // Up-to-date check
            if (IsTaskUpToDate(workflowDefinition, taskDefinition))
            {
                LOG_APP_INFO("WorkflowOrchestrator: Task '{}' is up to date → skipped", taskId);
                taskState->m_State = TaskInstanceStateKind::Skipped;
                outMadeProgress = true;
                continue;
            }

            // Task is ready to run in this wave
            readyTasks.emplace_back(taskId, taskState);
        }

        // ---------------------------------------------------------
        // 2. If no ready tasks exist, wave made no progress
        // ---------------------------------------------------------
        if (readyTasks.empty())
        {
            return true; // not an error, but no progress
        }

        outMadeProgress = true;

        // ---------------------------------------------------------
        // 3. Dispatch all ready tasks in parallel using ThreadPool
        // ---------------------------------------------------------
        ThreadPool& pool = Core::g_Core->GetThreadPool();

        struct TaskFuture
        {
            std::string taskId;
            TaskInstanceState* state;
            std::future<bool> future;
        };

        std::vector<TaskFuture> futures;
        futures.reserve(readyTasks.size());

        for (auto& [taskId, taskState] : readyTasks)
        {
            auto defIt = workflowDefinition.m_Tasks.find(taskId);
            TaskDef const& taskDefinition = defIt->second;

            taskState->m_State = TaskInstanceStateKind::Running;
            ++taskState->m_AttemptCount;

            futures.push_back(TaskFuture{taskId, taskState,
                                         pool.SubmitTask(
                                             [&, taskId, taskDefinition]() -> bool
                                             {
                                                 return TaskExecutorRegistry::Get().Execute(workflowDefinition, workflowRun,
                                                                                            taskDefinition, *taskState);
                                             })});
        }

        // ---------------------------------------------------------
        // 4. Wait for all tasks to finish
        // ---------------------------------------------------------
        for (auto& tf : futures)
        {
            bool success = false;

            try
            {
                success = tf.future.get(); // wait for completion
            }
            catch (std::exception const& e)
            {
                LOG_APP_ERROR("WorkflowOrchestrator: Task '{}' threw exception: {}", tf.taskId, e.what());
                success = false;
            }

            if (!success)
            {
                tf.state->m_State = TaskInstanceStateKind::Failed;
                workflowRun.m_HasFailed = true;
            }
            else
            {
                if (tf.state->m_State != TaskInstanceStateKind::Succeeded)
                {
                    tf.state->m_State = TaskInstanceStateKind::Succeeded;
                }
            }
        }

        return true;
    }

    bool WorkflowOrchestrator::IsTaskReady(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                           TaskDef const& taskDefinition) const
    {
        (void)workflowDefinition;

        // All dependencies must have succeeded or been skipped.
        for (std::string const& dependencyId : taskDefinition.m_DependsOn)
        {
            auto iterator = workflowRun.m_TaskStates.find(dependencyId);
            if (iterator == workflowRun.m_TaskStates.end())
            {
                LOG_APP_ERROR("WorkflowOrchestrator: Task '{}' depends on unknown task '{}'", taskDefinition.m_Id,
                              dependencyId);
                return false;
            }

            TaskInstanceStateKind dependencyState = iterator->second.m_State;
            if (dependencyState != TaskInstanceStateKind::Succeeded && dependencyState != TaskInstanceStateKind::Skipped)
            {
                return false;
            }
        }

        return true;
    }

    bool WorkflowOrchestrator::IsTaskUpToDate(WorkflowDefinition const& workflowDefinition,
                                              TaskDef const& taskDefinition) const
    {
        // If the task has no declared outputs, treat it as not provably up to date
        // and re-run it whenever its dependencies are satisfied.
        if (taskDefinition.m_FileOutputs.empty())
        {
            return false;
        }

        std::error_code errorCode;

        // ---------------------------------------------------------
        // 1) Collect timestamps for this task's own declared inputs
        // ---------------------------------------------------------
        std::vector<fs::file_time_type> inputTimes;

        for (std::string const& inputPath : taskDefinition.m_FileInputs)
        {
            fs::path path(inputPath);
            if (!fs::exists(path, errorCode))
            {
                // Missing input ⇒ not up to date.
                return false;
            }

            auto writeTime = fs::last_write_time(path, errorCode);
            if (errorCode)
            {
                LOG_APP_WARN("WorkflowOrchestrator::IsTaskUpToDate: Failed to get last_write_time for input '{}' : {}",
                             inputPath, errorCode.message());
                return false;
            }

            inputTimes.push_back(writeTime);
        }

        // ---------------------------------------------------------
        // 2) Collect timestamps for all upstream outputs (transitively)
        // ---------------------------------------------------------
        if (!taskDefinition.m_DependsOn.empty())
        {
            std::unordered_set<std::string> visitedTasks;
            std::vector<fs::file_time_type> upstreamTimes;

            for (std::string const& dependencyId : taskDefinition.m_DependsOn)
            {
                if (!CollectUpstreamOutputTimes(workflowDefinition, dependencyId, visitedTasks, upstreamTimes))
                {
                    // If upstream outputs are missing or unreadable, we err on the
                    // side of *not* considering this task up to date.
                    return false;
                }
            }

            inputTimes.insert(inputTimes.end(), upstreamTimes.begin(), upstreamTimes.end());
        }

        if (inputTimes.empty())
        {
            // No inputs and no upstream outputs => cannot prove freshness.
            return false;
        }

        fs::file_time_type latestInputTime = *std::max_element(inputTimes.begin(), inputTimes.end());

        // ---------------------------------------------------------
        // 3) Collect timestamps for this task's outputs
        // ---------------------------------------------------------
        std::vector<fs::file_time_type> outputTimes;
        outputTimes.reserve(taskDefinition.m_FileOutputs.size());

        for (std::string const& outputPath : taskDefinition.m_FileOutputs)
        {
            fs::path path(outputPath);
            if (!fs::exists(path, errorCode))
            {
                // An output is missing ⇒ not up to date.
                return false;
            }

            auto writeTime = fs::last_write_time(path, errorCode);
            if (errorCode)
            {
                LOG_APP_WARN("WorkflowOrchestrator::IsTaskUpToDate: Failed to get last_write_time for output '{}' : {}",
                             outputPath, errorCode.message());
                return false;
            }

            outputTimes.push_back(writeTime);
        }

        if (outputTimes.empty())
        {
            return false;
        }

        fs::file_time_type earliestOutputTime = *std::min_element(outputTimes.begin(), outputTimes.end());

        // Makefile-style rule extended with upstream outputs:
        // Task is up to date if all outputs exist and the oldest output
        // is >= the newest input or upstream output.
        bool const isUpToDate = (earliestOutputTime >= latestInputTime);
        return isUpToDate;
    }

    bool WorkflowOrchestrator::CollectUpstreamOutputTimes(WorkflowDefinition const& workflowDefinition,
                                                          std::string const& taskId,
                                                          std::unordered_set<std::string>& visitedTasks,
                                                          std::vector<fs::file_time_type>& outTimes) const
    {
        std::error_code errorCode;

        // Avoid infinite recursion in case validation was skipped.
        if (visitedTasks.contains(taskId))
        {
            return true;
        }
        visitedTasks.insert(taskId);

        auto definitionIterator = workflowDefinition.m_Tasks.find(taskId);
        if (definitionIterator == workflowDefinition.m_Tasks.end())
        {
            LOG_APP_ERROR("WorkflowOrchestrator::CollectUpstreamOutputTimes: Unknown task '{}'", taskId);
            return false;
        }

        TaskDef const& taskDefinition = definitionIterator->second;

        // Collect this task's own outputs if any.
        for (std::string const& outputPath : taskDefinition.m_FileOutputs)
        {
            fs::path path(outputPath);
            if (!fs::exists(path, errorCode))
            {
                // Upstream output is missing: downstream tasks cannot
                // reliably be up to date.
                return false;
            }

            auto writeTime = fs::last_write_time(path, errorCode);
            if (errorCode)
            {
                LOG_APP_WARN("WorkflowOrchestrator::CollectUpstreamOutputTimes: Failed to get last_write_time for '{}' : {}",
                             outputPath, errorCode.message());
                return false;
            }

            outTimes.push_back(writeTime);
        }

        // Recurse into dependencies (transitive closure).
        for (std::string const& dependencyId : taskDefinition.m_DependsOn)
        {
            if (!CollectUpstreamOutputTimes(workflowDefinition, dependencyId, visitedTasks, outTimes))
            {
                return false;
            }
        }

        return true;
    }

    bool WorkflowOrchestrator::ExecuteTaskInstance(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                                   TaskDef const& taskDefinition, std::string const& taskId,
                                                   TaskInstanceState& taskState)
    {
        // Mark as running and bump attempt counter.
        taskState.m_State = TaskInstanceStateKind::Running;
        ++taskState.m_AttemptCount;

        // ---------------------------------------------------------
        // Step 1: Resolve inputs via dataflow (correctly using taskId)
        // ---------------------------------------------------------
        DataflowResolver dataflowResolver;

        std::optional<TaskResolvedInputs> optionalResolvedInputs =
            dataflowResolver.ResolveInputsForTask(workflowDefinition, workflowRun, taskDefinition,
                                                  taskId); // <-- FIXED: was taskDefinition.m_Id

        if (!optionalResolvedInputs.has_value())
        {
            taskState.m_LastErrorMessage = "Failed to resolve task inputs via dataflow / context";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        TaskResolvedInputs const& resolvedInputs = optionalResolvedInputs.value();

        // Snapshot resolved inputs for UI debugging.
        taskState.m_InputValues = resolvedInputs.m_StringValues;

        {
            std::string summary;
            for (auto const& p : resolvedInputs.m_StringValues)
            {
                summary += p.first;
                summary += "=";
                summary += p.second;
                summary += ";";
            }
            taskState.m_InputsJson = summary;
        }

        // ---------------------------------------------------------
        // Step 2: Dispatch actual executor
        // ---------------------------------------------------------
        TaskExecutorRegistry& executorRegistry = TaskExecutorRegistry::Get();
        bool const executedOk = executorRegistry.Execute(workflowDefinition, workflowRun, taskDefinition, taskState);

        if (!executedOk)
        {
            if (taskState.m_State != TaskInstanceStateKind::Failed)
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
            }
            return false;
        }

        // ---------------------------------------------------------
        // Step 3: Snapshot outputs for UI
        // ---------------------------------------------------------
        {
            std::string summary;
            for (auto const& p : taskState.m_OutputValues)
            {
                summary += p.first;
                summary += "=";
                summary += p.second;
                summary += ";";
            }
            taskState.m_OutputsJson = summary;
        }

        // ---------------------------------------------------------
        // Step 4: Finalize state
        // ---------------------------------------------------------
        if (taskState.m_State != TaskInstanceStateKind::Failed && taskState.m_State != TaskInstanceStateKind::Skipped)
        {
            taskState.m_State = TaskInstanceStateKind::Succeeded;
        }

        return (taskState.m_State == TaskInstanceStateKind::Succeeded ||
                taskState.m_State == TaskInstanceStateKind::Skipped);
    }

} // namespace AIAssistant
