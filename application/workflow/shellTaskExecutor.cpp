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
#include "shellTaskExecutor.h"

#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <string>
#include <cctype>

#include "simdjson/simdjson.h"

namespace AIAssistant
{
    namespace
    {
        // ------------------------------------------------------------
        // Build a derived output-slot → value map for this task.
        //
        // Strategy:
        //   1) If outputs.size() == file_outputs.size(), zip them by index:
        //        outputSlotName[i] → file_outputs[i]
        //   2) For any remaining outputs, if an input with the same name exists
        //      in taskState.m_InputValues, use that.
        //
        // This provides a deterministic mapping for file-based workflows like
        // the make_example.jcwf test.
        // ------------------------------------------------------------
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

        // ------------------------------------------------------------
        // Join a list of file paths into a single space-separated string.
        // Example: ["a.cpp","b.cpp"] → "a.cpp b.cpp"
        // (This matches Makefile-style variable expansion semantics.)
        // ------------------------------------------------------------
        std::string JoinFileList(std::vector<std::string> const& files)
        {
            std::string joined;

            for (size_t index = 0; index < files.size(); ++index)
            {
                joined += files[index];

                if (index + 1 < files.size())
                {
                    joined += " ";
                }
            }

            return joined;
        }

        // ------------------------------------------------------------
        // Expand JCWF templates inside a single argument string.
        //
        // Supported patterns:
        //   * ${inputs}        → space-separated list of file_inputs
        //   * ${outputs}       → space-separated list of file_outputs
        //   * ${input[N]}      → N-th file_input (0-based)
        //   * ${output[N]}     → N-th file_output (0-based)
        //   * ${slot.NAME}     → value from taskState.m_InputValues["NAME"]
        //   * ${env.NAME}      → value from taskDefinition.m_Environment.m_Variables["NAME"]
        //                        (empty string if not found)
        //
        // Returns false on:
        //   * malformed pattern (missing closing '}')
        //   * invalid index
        //   * unknown slot.NAME
        //
        // This keeps misconfigurations explicit.
        // ------------------------------------------------------------
        bool ExpandTemplatesStrict(std::string const& raw, TaskDef const& taskDefinition, TaskInstanceState const& taskState,
                                   std::string& expandedOut)
        {
            expandedOut.clear();

            size_t currentIndex = 0;

            while (currentIndex < raw.size())
            {
                size_t startIndex = raw.find("${", currentIndex);
                if (startIndex == std::string::npos)
                {
                    expandedOut += raw.substr(currentIndex);
                    break;
                }

                // Copy literal prefix.
                expandedOut += raw.substr(currentIndex, startIndex - currentIndex);

                size_t closeBraceIndex = raw.find('}', startIndex + 2);
                if (closeBraceIndex == std::string::npos)
                {
                    // Malformed template.
                    return false;
                }

                std::string key = raw.substr(startIndex + 2, closeBraceIndex - (startIndex + 2));
                std::string replacement;

                // ${inputs}
                if (key == "inputs")
                {
                    replacement = JoinFileList(taskDefinition.m_FileInputs);
                }
                // ${outputs}
                else if (key == "outputs")
                {
                    replacement = JoinFileList(taskDefinition.m_FileOutputs);
                }
                // ${input[N]}
                else if (key.rfind("input[", 0) == 0 && key.back() == ']')
                {
                    std::string indexString = key.substr(6, key.size() - 7);
                    try
                    {
                        size_t index = static_cast<size_t>(std::stoul(indexString));
                        if (index >= taskDefinition.m_FileInputs.size())
                        {
                            return false;
                        }

                        replacement = taskDefinition.m_FileInputs[index];
                    }
                    catch (...)
                    {
                        return false;
                    }
                }
                // ${output[N]}
                else if (key.rfind("output[", 0) == 0 && key.back() == ']')
                {
                    std::string indexString = key.substr(7, key.size() - 8);
                    try
                    {
                        size_t index = static_cast<size_t>(std::stoul(indexString));
                        if (index >= taskDefinition.m_FileOutputs.size())
                        {
                            return false;
                        }

                        replacement = taskDefinition.m_FileOutputs[index];
                    }
                    catch (...)
                    {
                        return false;
                    }
                }
                // ${slot.NAME}
                else if (key.rfind("slot.", 0) == 0)
                {
                    std::string slotName = key.substr(5);
                    auto iterator = taskState.m_InputValues.find(slotName);
                    if (iterator == taskState.m_InputValues.end())
                    {
                        return false;
                    }

                    replacement = iterator->second;
                }
                // ${env.NAME}
                else if (key.rfind("env.", 0) == 0)
                {
                    std::string envName = key.substr(4);
                    auto iterator = taskDefinition.m_Environment.m_Variables.find(envName);
                    if (iterator != taskDefinition.m_Environment.m_Variables.end())
                    {
                        replacement = iterator->second;
                    }
                    else
                    {
                        // Missing env variable → expand as empty string.
                        replacement.clear();
                    }
                }
                else
                {
                    // Unknown pattern.
                    return false;
                }

                expandedOut += replacement;
                currentIndex = closeBraceIndex + 1;
            }

            return true;
        }

        // ------------------------------------------------------------
        // Build a command string for std::system() from argv-style vector.
        //
        // For now we assume arguments are already validated as "safe".
        // We simply join them with spaces.
        // ------------------------------------------------------------
        std::string JoinArgumentsForSystem(std::vector<std::string> const& arguments)
        {
            std::string command;

            for (size_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex)
            {
                command += arguments[argumentIndex];

                if (argumentIndex + 1 < arguments.size())
                {
                    command += " ";
                }
            }

            return command;
        }

        // ------------------------------------------------------------
        // Scan raw args for the presence of any input/output macros.
        //
        // Used to implement Option B:
        //   - If no input macro is present, inject "${inputs}" at the front.
        //   - If no output macro is present, append "${outputs}".
        // ------------------------------------------------------------
        void EnsureDefaultInputOutputArgs(std::vector<std::string>& rawArgs)
        {
            bool hasInputMacro = false;
            bool hasOutputMacro = false;

            for (std::string const& argument : rawArgs)
            {
                if (argument.find("${inputs}") != std::string::npos || argument.find("${input[") != std::string::npos)
                {
                    hasInputMacro = true;
                }

                if (argument.find("${outputs}") != std::string::npos || argument.find("${output[") != std::string::npos)
                {
                    hasOutputMacro = true;
                }
            }

            if (!hasInputMacro)
            {
                rawArgs.insert(rawArgs.begin(), std::string("${inputs}"));
            }

            if (!hasOutputMacro)
            {
                rawArgs.push_back(std::string("${outputs}"));
            }
        }
    } // anonymous namespace

    bool ShellTaskExecutor::ValidateScriptPath(std::string const& path) const
    {
        // Enforce "scripts/" prefix to avoid arbitrary command execution.
        return path.rfind("scripts/", 0) == 0;
    }

    bool ShellTaskExecutor::IsSafeArgument(std::string const& argument) const
    {
        // Conservative safety check:
        //  * Allow typical filename / flag characters and spaces.
        //  * Forbid characters commonly used in shell injection.
        //
        // This is not a perfect sandbox, but combined with ValidateScriptPath
        // it strongly nudges workflows toward simple, safe commands.
        for (char character : argument)
        {
            unsigned char const ch = static_cast<unsigned char>(character);

            if (std::iscntrl(ch) != 0)
            {
                return false;
            }

            switch (character)
            {
                case ';':
                case '&':
                case '|':
                case '>':
                case '<':
                case '\'':
                case '"':
                case '`':
                    return false;
                default:
                    break;
            }
        }

        return true;
    }

    bool ShellTaskExecutor::Execute(WorkflowDefinition const& workflowDefinition, WorkflowRun& workflowRun,
                                    TaskDef const& taskDefinition, TaskInstanceState& taskState)
    {
        (void)workflowDefinition;
        (void)workflowRun;

        LOG_APP_INFO("[shell] Executing shell task '{}'", taskDefinition.m_Id);

        // ------------------------------------------------------------
        // 1) Parse params JSON
        // ------------------------------------------------------------
        simdjson::dom::parser parser;
        simdjson::dom::element params;

        if (taskDefinition.m_ParamsJson.empty())
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Missing params JSON";
            return false;
        }

        if (parser.parse(taskDefinition.m_ParamsJson).get(params))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Invalid params JSON";
            return false;
        }

        // ------------------------------------------------------------
        // 2) Extract command path
        // ------------------------------------------------------------
        std::string commandPath;

        if (auto commandElement = params["command"])
        {
            auto view = commandElement.value().get_string();
            if (view.error())
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: Invalid 'command' field";
                return false;
            }

            commandPath = std::string(view.value());
        }
        else
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Missing 'command' field";
            return false;
        }

        if (!ValidateScriptPath(commandPath))
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Script path rejected (must start with 'scripts/')";
            return false;
        }

        // ------------------------------------------------------------
        // 3) Derive logical output values up front
        //
        // This ensures templates like ${output[0]} and dataflow outputs
        // are consistent with file_outputs.
        // ------------------------------------------------------------
        std::unordered_map<std::string, std::string> derivedOutputs;
        BuildOutputSlotMap(taskDefinition, taskState, derivedOutputs);

        // ------------------------------------------------------------
        // 4) Collect raw args from JCWF, then apply Option B defaults
        //    (auto-prepend ${inputs} / auto-append ${outputs} if absent).
        // ------------------------------------------------------------
        std::vector<std::string> rawArgs;

        if (auto argsElement = params["args"])
        {
            if (!argsElement.value().is_array())
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: 'args' must be an array if present";
                return false;
            }

            for (auto entry : argsElement.value().get_array())
            {
                auto view = entry.get_string();
                if (view.error())
                {
                    taskState.m_State = TaskInstanceStateKind::Failed;
                    taskState.m_LastErrorMessage = "ShellTaskExecutor: Non-string value in 'args' array";
                    return false;
                }

                rawArgs.emplace_back(std::string(view.value()));
            }
        }

        // Option B: inject default input/output macros if none are present.
        EnsureDefaultInputOutputArgs(rawArgs);

        // ------------------------------------------------------------
        // 5) Build argv-style list: [commandPath, expanded args...]
        // ------------------------------------------------------------
        std::vector<std::string> argumentList;
        argumentList.push_back(commandPath);

        for (std::string const& rawArgument : rawArgs)
        {
            std::string expandedArgument;
            if (!ExpandTemplatesStrict(rawArgument, taskDefinition, taskState, expandedArgument))
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage = "ShellTaskExecutor: Failed to expand argument template '" + rawArgument + "'";
                return false;
            }

            if (!IsSafeArgument(expandedArgument))
            {
                taskState.m_State = TaskInstanceStateKind::Failed;
                taskState.m_LastErrorMessage =
                    "ShellTaskExecutor: Argument contains unsupported characters (safety check failed)";
                return false;
            }

            if (!expandedArgument.empty())
            {
                argumentList.push_back(expandedArgument);
            }
        }

        // ------------------------------------------------------------
        // 6) Join into a single command string for std::system()
        // ------------------------------------------------------------
        std::string const fullCommand = JoinArgumentsForSystem(argumentList);

        LOG_APP_INFO("[shell] Command: {}", fullCommand);

        int const result = std::system(fullCommand.c_str());

        if (result != 0)
        {
            taskState.m_State = TaskInstanceStateKind::Failed;
            taskState.m_LastErrorMessage = "ShellTaskExecutor: Shell command returned non-zero exit status";
            return false;
        }

        // ------------------------------------------------------------
        // 7) Populate taskState.m_OutputValues for downstream dataflow
        // ------------------------------------------------------------
        for (auto const& outputPair : derivedOutputs)
        {
            taskState.m_OutputValues[outputPair.first] = outputPair.second;
        }

        taskState.m_State = TaskInstanceStateKind::Succeeded;
        return true;
    }

} // namespace AIAssistant
