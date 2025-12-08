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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
   OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "engine.h"
#include "jarvisAgent.h"
#include "pythonTaskExecutor.h"
#include "python/pythonEngine.h"

#include <unordered_map>

namespace AIAssistant
{
    namespace
    {
        // Shared helper: see ShellTaskExecutor explanation.
        void BuildOutputSlotMap(TaskDef const& taskDefinition, TaskInstanceState const& taskState,
                                std::unordered_map<std::string, std::string>& outputSlotMapOut)
        {
            outputSlotMapOut.clear();

            // 1) Zip outputs with file_outputs when sizes match
            if (!taskDefinition.m_FileOutputs.empty() &&
                taskDefinition.m_FileOutputs.size() == taskDefinition.m_Outputs.size())
            {
                size_t fileIndex = 0;
                for (auto const& outputPair : taskDefinition.m_Outputs)
                {
                    if (fileIndex < taskDefinition.m_FileOutputs.size())
                    {
                        outputSlotMapOut[outputPair.first] = taskDefinition.m_FileOutputs[fileIndex];
                    }

                    ++fileIndex;
                }
            }

            // 2) Fallback: use input with the same name
            for (auto const& outputPair : taskDefinition.m_Outputs)
            {
                std::string const& outputName = outputPair.first;

                if (outputSlotMapOut.contains(outputName))
                {
                    continue;
                }

                auto inputIterator = taskState.m_InputValues.find(outputName);
                if (inputIterator != taskState.m_InputValues.end())
                {
                    outputSlotMapOut[outputName] = inputIterator->second;
                }
            }
        }
    } // anonymous namespace

    bool PythonTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                     TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        (void)workflowDefinition;
        (void)workflowRun;

        LOG_APP_INFO("[python] Executing Python task '{}'", taskDefinition.m_Id);

        PythonEngine* pythonEngine = App::g_App->GetPythonEngine();

        if (pythonEngine == nullptr)
        {
            taskState.m_LastErrorMessage = "PythonTaskExecutor: PythonEngine not initialized";
            taskState.m_State = TaskInstanceStateKind::Failed;
            return false;
        }

        std::string errorMessage;
        bool const ok = pythonEngine->ExecuteWorkflowTask(taskDefinition, errorMessage);

        if (!ok)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = errorMessage;
            return false;
        }

        // Derive outputs from the task definition and resolved inputs.
        std::unordered_map<std::string, std::string> derivedOutputs;
        BuildOutputSlotMap(taskDefinition, taskState, derivedOutputs);

        for (auto const& outputPair : derivedOutputs)
        {
            taskState.m_OutputValues[outputPair.first] = outputPair.second;
        }

        taskState.m_State = TaskInstanceStateKind::Succeeded;
        return true;
    }

} // namespace AIAssistant
