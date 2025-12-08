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
#include <vector>
#include <filesystem>
#include <optional>

#include "workflowTypes.h"

namespace AIAssistant
{

    class WorkflowRegistry
    {
    public:
        WorkflowRegistry() = default;
        ~WorkflowRegistry() = default;

        bool LoadDirectory(std::filesystem::path const& dirPath);
        bool LoadFile(std::filesystem::path const& filePath);

        bool HasWorkflow(std::string const& workflowId) const;

        std::optional<WorkflowDefinition> GetWorkflow(std::string const& workflowId) const;

        std::vector<std::string> GetWorkflowIds() const;

        bool ValidateAll() const;

    private:
        bool ValidateWorkflow(WorkflowDefinition const& wf) const;
        bool ValidateTasks(WorkflowDefinition const& wf) const;
        bool ValidateTaskIO(TaskDef const& task, WorkflowDefinition const& wf) const;
        bool ValidateDataflow(WorkflowDefinition const& wf) const;
        bool ValidateTriggers(WorkflowDefinition const& wf) const;
        bool ValidateNoCycles(WorkflowDefinition const& wf) const;

    private:
        std::unordered_map<std::string, WorkflowDefinition> m_Workflows;
    };

} // namespace AIAssistant
