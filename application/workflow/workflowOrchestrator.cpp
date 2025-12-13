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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#include "workflowOrchestrator.h"

#include <algorithm>
#include <chrono>
#include <future>

#include "core.h"
#include "engine.h"
#include "workflowRegistry.h"
#include "dataflowResolver.h"
#include "taskExecutor.h"
#include "taskExecutorRegistry.h"
#include "taskFreshnessChecker.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    namespace
    {
        bool ResolveTemplateString(std::string const& value, std::unordered_map<std::string, std::string> const& inputValues,
                                   std::unordered_map<std::string, std::string> const& outputValues,
                                   std::string& outResolved)
        {
            outResolved.clear();
            outResolved.reserve(value.size());

            size_t pos = 0;

            while (pos < value.size())
            {
                size_t const dollar = value.find("${", pos);
                if (dollar == std::string::npos)
                {
                    outResolved.append(value.substr(pos));
                    break;
                }

                outResolved.append(value.substr(pos, dollar - pos));

                size_t const close = value.find('}', dollar + 2);
                if (close == std::string::npos)
                {
                    return false;
                }

                std::string const token = value.substr(dollar + 2, close - (dollar + 2));

                // Supported forms:
                //  - inputs.<name>
                //  - outputs.<name>
                if (token.rfind("inputs.", 0) == 0)
                {
                    std::string const key = token.substr(7);
                    auto iterator = inputValues.find(key);
                    if (iterator == inputValues.end())
                    {
                        return false;
                    }
                    outResolved.append(iterator->second);
                }
                else if (token.rfind("outputs.", 0) == 0)
                {
                    std::string const key = token.substr(8);
                    auto iterator = outputValues.find(key);
                    if (iterator == outputValues.end())
                    {
                        return false;
                    }
                    outResolved.append(iterator->second);
                }
                else
                {
                    return false;
                }

                pos = close + 1;
            }

            // If unresolved templates remain, treat as not resolved.
            if (outResolved.find("${") != std::string::npos)
            {
                return false;
            }

            return true;
        }

        bool ResolveTemplatePathList(std::vector<std::string> const& templates,
                                     std::unordered_map<std::string, std::string> const& inputValues,
                                     std::unordered_map<std::string, std::string> const& outputValues,
                                     std::vector<fs::path>& outPaths)
        {
            outPaths.clear();
            outPaths.reserve(templates.size());

            for (std::string const& t : templates)
            {
                std::string resolved;
                if (!ResolveTemplateString(t, inputValues, outputValues, resolved))
                {
                    // If there are no templates at all, accept as literal path.
                    if (t.find("${") == std::string::npos)
                    {
                        outPaths.emplace_back(t);
                        continue;
                    }
                    return false;
                }

                if (resolved.empty())
                {
                    return false;
                }

                outPaths.emplace_back(resolved);
            }

            return true;
        }

        bool TryResolveTaskInputsForFreshness(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                              TaskDef const& taskDefinition, std::string const& taskId,
                                              std::unordered_map<std::string, std::string>& outInputValues)
        {
            DataflowResolver dataflowResolver;

            std::optional<TaskResolvedInputs> optionalResolvedInputs =
                dataflowResolver.ResolveInputsForTask(workflowDefinition, workflowRun, taskDefinition, taskId);

            if (!optionalResolvedInputs.has_value())
            {
                return false;
            }

            outInputValues = optionalResolvedInputs.value().m_StringValues;
            return true;
        }

        bool ResolveFreshnessPathsForTask(WorkflowDefinition const& workflowDefinition, WorkflowRun const& workflowRun,
                                          TaskDef const& taskDefinition, std::string const& taskId,
                                          std::vector<fs::path>& outInputPaths, std::vector<fs::path>& outOutputPaths)
        {
            auto hasTemplatePrefix = [](std::vector<std::string> const& values, std::string const& prefix) -> bool
            {
                for (std::string const& value : values)
                {
                    if (value.find(prefix) != std::string::npos)
                    {
                        return true;
                    }
                }
                return false;
            };

            // Only resolve task "inputs" if file_inputs/file_outputs actually reference ${inputs.*}.
            // This prevents DataflowResolver from failing (and spamming logs) for tasks whose declared
            // inputs are required for execution but irrelevant for freshness checks when paths are literal.
            bool const needsInputResolution = hasTemplatePrefix(taskDefinition.m_FileInputs, "${inputs.") ||
                                              hasTemplatePrefix(taskDefinition.m_FileOutputs, "${inputs.");

            std::unordered_map<std::string, std::string> inputValues;
            if (needsInputResolution)
            {
                if (!TryResolveTaskInputsForFreshness(workflowDefinition, workflowRun, taskDefinition, taskId, inputValues))
                {
                    return false;
                }
            }

            // For freshness checks we can only reliably substitute outputs if they are already known.
            // (For skipped tasks m_OutputValues is typically empty; in that case output template resolution
            // may fail and we conservatively treat the task as not up to date.)
            std::unordered_map<std::string, std::string> outputValues;

            auto stateIterator = workflowRun.m_TaskStates.find(taskId);
            if (stateIterator != workflowRun.m_TaskStates.end())
            {
                outputValues = stateIterator->second.m_OutputValues;
            }

            if (!ResolveTemplatePathList(taskDefinition.m_FileInputs, inputValues, outputValues, outInputPaths))
            {
                return false;
            }

            if (!ResolveTemplatePathList(taskDefinition.m_FileOutputs, inputValues, outputValues, outOutputPaths))
            {
                return false;
            }

            return true;
        }

        // ---------------------------------------------------------
        // When a task is skipped due to freshness, we still want its
        // "logical" outputs to be available for downstream dataflow.
        //
        // Otherwise you get logs like:
        //   DataflowResolver: output 'object' not found in task 'compile_lib2' ...
        //
        // Convention (deterministic):
        //   * We derive a mapping from declared output slots -> resolved file_outputs.
        //   * If slot_count == path_count: zip by sorted slot name order.
        //   * Else if path_count == 1: map all slots to that one path.
        //   * Else if slot_count == 1: map that slot to the first path.
        //
        // If we cannot resolve file_outputs, we leave outputs empty and
        // downstream dataflow may still fail (as it should).
        // ---------------------------------------------------------
        void PopulateSkippedTaskOutputsIfPossible(WorkflowDefinition const& workflowDefinition,
                                                  WorkflowRun const& workflowRun, TaskDef const& taskDefinition,
                                                  std::string const& taskId, TaskInstanceState& taskState)
        {
            std::vector<fs::path> unusedInputPaths;
            std::vector<fs::path> resolvedOutputPaths;

            if (!ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId, unusedInputPaths,
                                              resolvedOutputPaths))
            {
                return;
            }

            if (taskDefinition.m_Outputs.empty() || resolvedOutputPaths.empty())
            {
                return;
            }

            std::vector<std::string> outputSlotNames;
            outputSlotNames.reserve(taskDefinition.m_Outputs.size());

            for (auto const& outputPair : taskDefinition.m_Outputs)
            {
                outputSlotNames.push_back(outputPair.first);
            }

            std::sort(outputSlotNames.begin(), outputSlotNames.end());

            if (outputSlotNames.size() == resolvedOutputPaths.size())
            {
                for (size_t index = 0; index < outputSlotNames.size(); ++index)
                {
                    taskState.m_OutputValues[outputSlotNames[index]] = resolvedOutputPaths[index].string();
                }
            }
            else if (resolvedOutputPaths.size() == 1)
            {
                std::string const onlyPath = resolvedOutputPaths[0].string();
                for (std::string const& slotName : outputSlotNames)
                {
                    taskState.m_OutputValues[slotName] = onlyPath;
                }
            }
            else if (outputSlotNames.size() == 1)
            {
                taskState.m_OutputValues[outputSlotNames[0]] = resolvedOutputPaths[0].string();
            }
            else
            {
                // Ambiguous mapping; do not guess.
                return;
            }

            // Keep UI summary fields consistent with executed tasks.
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
        }

    } // namespace

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

            // Up-to-date check (Makefile semantics, with template resolution)
            {
                TaskFreshnessChecker freshnessChecker;
                TaskFreshnessChecker::ResolvedPaths resolvedPaths;

                if (ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                 resolvedPaths.m_InputPaths, resolvedPaths.m_OutputPaths))
                {
                    auto resolveUpstreamOutputs = [&](std::string const& upstreamTaskId,
                                                      std::vector<fs::path>& outPaths) -> bool
                    {
                        auto upstreamIt = workflowDefinition.m_Tasks.find(upstreamTaskId);
                        if (upstreamIt == workflowDefinition.m_Tasks.end())
                        {
                            return false;
                        }

                        std::vector<fs::path> unusedInputs;
                        std::vector<fs::path> outputPaths;

                        if (!ResolveFreshnessPathsForTask(workflowDefinition, workflowRun, upstreamIt->second,
                                                          upstreamTaskId, unusedInputs, outputPaths))
                        {
                            return false;
                        }

                        outPaths = outputPaths;
                        return true;
                    };

                    if (freshnessChecker.IsTaskUpToDate(workflowDefinition, taskId, resolvedPaths, resolveUpstreamOutputs))
                    {
                        LOG_APP_INFO("WorkflowOrchestrator: Task '{}' is up to date → skipped", taskId);

                        // NEW: populate outputs for skipped tasks so downstream dataflow can resolve.
                        PopulateSkippedTaskOutputsIfPossible(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                             *taskState);

                        taskState->m_State = TaskInstanceStateKind::Skipped;
                        outMadeProgress = true;
                        continue;
                    }
                }
                else
                {
                    // If we cannot resolve file templates for freshness checks,
                    // we conservatively treat the task as not up to date.
                }
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

            // Mark as running before dispatch (attempt count is incremented inside ExecuteTaskInstance()).
            taskState->m_State = TaskInstanceStateKind::Running;

            TaskInstanceState* const capturedTaskState = taskState;

            futures.push_back(
                TaskFuture{taskId, capturedTaskState,
                           pool.SubmitTask(
                               [this, &workflowDefinition, &workflowRun, taskId, taskDefinition, capturedTaskState]() -> bool
                               {
                                   return ExecuteTaskInstance(workflowDefinition, workflowRun, taskDefinition, taskId,
                                                              *capturedTaskState);
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
                // Do not override Skipped.
                if (tf.state->m_State != TaskInstanceStateKind::Succeeded &&
                    tf.state->m_State != TaskInstanceStateKind::Skipped)
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
