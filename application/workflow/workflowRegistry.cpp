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

#include "engine.h"

#include "workflowRegistry.h"
#include "workflowJsonParser.h"

#include <unordered_set>
#include <functional>
#include <fstream>

namespace AIAssistant
{

    // ------------------------------------------------------------
    // Load all workflow files (.jcwf) from directory
    // ------------------------------------------------------------
    bool WorkflowRegistry::LoadDirectory(std::filesystem::path const& dirPath)
    {
        LOG_APP_INFO("WorkflowRegistry::LoadDirectory scanning {}", dirPath.string());

        if (!std::filesystem::exists(dirPath))
        {
            LOG_APP_WARN("Directory does not exist: {}", dirPath.string());
            return false;
        }

        for (auto const& entry : std::filesystem::directory_iterator(dirPath))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            auto const& path = entry.path();
            if (path.extension() == ".jcwf")
            {
                LOG_APP_INFO("Loading workflow file {}", path.string());

                if (!LoadFile(path))
                {
                    LOG_APP_ERROR("Failed to load workflow {}", path.string());
                    return false;
                }
            }
        }

        return true;
    }

    // ------------------------------------------------------------
    // Load or reload a single JCWF file
    // ------------------------------------------------------------
    bool WorkflowRegistry::LoadFile(std::filesystem::path const& filePath)
    {
        LOG_APP_INFO("WorkflowRegistry::LoadFile {}", filePath.string());

        std::ifstream file(filePath);
        if (!file.is_open())
        {
            LOG_APP_ERROR("Failed to open {}", filePath.string());
            return false;
        }

        std::string jsonContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        WorkflowDefinition definition;
        std::string errorMessage;

        WorkflowJsonParser parser;
        if (!parser.ParseWorkflowJson(jsonContent, definition, errorMessage))
        {
            LOG_APP_ERROR("WorkflowJsonParser failed for {}: {}", filePath.string(), errorMessage);
            return false;
        }

        // enforce version
        if (definition.m_Version != "1.0")
        {
            LOG_APP_ERROR("Workflow {} has unsupported version '{}'. Only version 1.0 is allowed.", definition.m_Id,
                          definition.m_Version);
            return false;
        }

        // reload warning or normal load
        if (m_Workflows.contains(definition.m_Id))
        {
            LOG_APP_WARN("Workflow {} already exists; reloading.", definition.m_Id);
        }

        auto id = definition.m_Id;

        // Use the workflow id as the map key; do not look up by filename stem.
        LOG_APP_INFO("Registered workflow {}", id);

        m_Workflows[definition.m_Id] = std::move(definition);

        return true;
    }

    // ------------------------------------------------------------
    // Lookup functions
    // ------------------------------------------------------------
    bool WorkflowRegistry::HasWorkflow(std::string const& workflowId) const { return m_Workflows.contains(workflowId); }

    std::optional<WorkflowDefinition> WorkflowRegistry::GetWorkflow(std::string const& workflowId) const
    {
        auto it = m_Workflows.find(workflowId);
        if (it == m_Workflows.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<std::string> WorkflowRegistry::GetWorkflowIds() const
    {
        std::vector<std::string> ids;
        ids.reserve(m_Workflows.size());

        for (auto const& [key, _] : m_Workflows)
        {
            ids.push_back(key);
        }
        return ids;
    }

    // ------------------------------------------------------------
    // Validate all workflows
    // ------------------------------------------------------------
    bool WorkflowRegistry::ValidateAll() const
    {
        bool allOk = true;

        for (auto const& [id, wf] : m_Workflows)
        {
            LOG_APP_INFO("Validating workflow {}", id);

            if (!ValidateWorkflow(wf))
            {
                allOk = false;
            }
        }

        return allOk;
    }

    // ------------------------------------------------------------
    // Validate a single workflow
    // ------------------------------------------------------------
    bool WorkflowRegistry::ValidateWorkflow(WorkflowDefinition const& wf) const
    {
        bool ok = true;

        if (!ValidateTriggers(wf))
        {
            ok = false;
        }

        if (!ValidateTasks(wf))
        {
            ok = false;
        }

        if (!ValidateDataflow(wf))
        {
            ok = false;
        }

        if (!ValidateNoCycles(wf))
        {
            ok = false;
        }

        return ok;
    }

    // ------------------------------------------------------------
    // Validate triggers
    // ------------------------------------------------------------
    bool WorkflowRegistry::ValidateTriggers(WorkflowDefinition const& wf) const
    {
        std::unordered_set<std::string> seenIds;

        for (auto const& trig : wf.m_Triggers)
        {
            if (seenIds.contains(trig.m_Id))
            {
                LOG_APP_ERROR("Workflow {} trigger '{}' is duplicated", wf.m_Id, trig.m_Id);
                return false;
            }
            seenIds.insert(trig.m_Id);

            if (trig.m_Type == WorkflowTriggerType::Unknown)
            {
                LOG_APP_ERROR("Workflow {} trigger '{}' has unknown type", wf.m_Id, trig.m_Id);
                return false;
            }

            // Validate minimal required fields for each type
            // (real logic is performed later by TriggerEngine)
            if (trig.m_Type == WorkflowTriggerType::Cron)
            {
                if (trig.m_ParamsJson.empty())
                {
                    LOG_APP_ERROR("Workflow {} trigger '{}' missing cron parameters", wf.m_Id, trig.m_Id);
                    return false;
                }
            }
        }

        return true;
    }

    // ------------------------------------------------------------
    // Validate tasks
    // ------------------------------------------------------------
    bool WorkflowRegistry::ValidateTasks(WorkflowDefinition const& wf) const
    {
        for (auto const& [taskId, task] : wf.m_Tasks)
        {
            // Validate depends_on references
            for (std::string const& dep : task.m_DependsOn)
            {
                if (!wf.m_Tasks.contains(dep))
                {
                    LOG_APP_ERROR("Workflow {}: task '{}' depends on unknown task '{}'", wf.m_Id, taskId, dep);
                    return false;
                }
            }

            // Validate I/O slots
            if (!ValidateTaskIO(task, wf))
            {
                return false;
            }

            // Validate environment
            if (task.m_Type == TaskType::AiCall)
            {
                // assistant mode requires environment or provider inside params
                // (we cannot inspect params JSON here)
            }
        }

        return true;
    }

    // ------------------------------------------------------------
    // Validate task I/O slots
    // ------------------------------------------------------------
    bool WorkflowRegistry::ValidateTaskIO(TaskDef const& task, WorkflowDefinition const& wf) const
    {
        // Required inputs must have a type string
        for (auto const& [inputName, field] : task.m_Inputs)
        {
            if (field.m_IsRequired && field.m_Type.empty())
            {
                LOG_APP_ERROR("Workflow {} task '{}' input '{}' is required but has no type", wf.m_Id, task.m_Id, inputName);
                return false;
            }
        }

        // Outputs must have type to help the orchestrator
        for (auto const& [outputName, field] : task.m_Outputs)
        {
            if (field.m_Type.empty())
            {
                LOG_APP_ERROR("Workflow {} task '{}' output '{}' missing type", wf.m_Id, task.m_Id, outputName);
                return false;
            }
        }

        return true;
    }

    // ------------------------------------------------------------
    // Validate dataflow edges
    // ------------------------------------------------------------
    bool WorkflowRegistry::ValidateDataflow(WorkflowDefinition const& wf) const
    {
        for (auto const& df : wf.m_Dataflows)
        {
            if (!wf.m_Tasks.contains(df.m_FromTask))
            {
                LOG_APP_ERROR("Workflow {} dataflow references unknown from_task '{}'", wf.m_Id, df.m_FromTask);
                return false;
            }

            if (!wf.m_Tasks.contains(df.m_ToTask))
            {
                LOG_APP_ERROR("Workflow {} dataflow references unknown to_task '{}'", wf.m_Id, df.m_ToTask);
                return false;
            }

            // Validate output slot existence
            if (!df.m_FromOutput.empty())
            {
                auto const& taskFrom = wf.m_Tasks.at(df.m_FromTask);
                if (!taskFrom.m_Outputs.contains(df.m_FromOutput))
                {
                    LOG_APP_ERROR("Workflow {} dataflow: from_task '{}' has no output slot '{}'", wf.m_Id, df.m_FromTask,
                                  df.m_FromOutput);
                    return false;
                }
            }

            // Validate input slot existence
            if (!df.m_ToInput.empty())
            {
                auto const& taskTo = wf.m_Tasks.at(df.m_ToTask);
                if (!taskTo.m_Inputs.contains(df.m_ToInput))
                {
                    LOG_APP_ERROR("Workflow {} dataflow: to_task '{}' has no input slot '{}'", wf.m_Id, df.m_ToTask,
                                  df.m_ToInput);
                    return false;
                }
            }
        }

        return true;
    }

    // ------------------------------------------------------------
    // Validate no circular dependencies in task DAG
    // ------------------------------------------------------------
    bool WorkflowRegistry::ValidateNoCycles(WorkflowDefinition const& wf) const
    {
        std::unordered_set<std::string> visiting;
        std::unordered_set<std::string> visited;

        std::function<bool(std::string const&)> dfs = [&](std::string const& taskId) -> bool
        {
            if (visiting.contains(taskId))
            {
                LOG_APP_ERROR("Workflow {} cycle detected at task '{}'", wf.m_Id, taskId);
                return true;
            }

            if (visited.contains(taskId))
            {
                return false;
            }

            visiting.insert(taskId);

            auto const& task = wf.m_Tasks.at(taskId);
            for (auto const& dep : task.m_DependsOn)
            {
                if (dfs(dep))
                {
                    return true;
                }
            }

            visiting.erase(taskId);
            visited.insert(taskId);
            return false;
        };

        bool ok = true;

        for (auto const& [taskId, _] : wf.m_Tasks)
        {
            if (dfs(taskId))
            {
                ok = false;
            }
        }

        return ok;
    }

} // namespace AIAssistant