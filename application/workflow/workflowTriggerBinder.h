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

namespace AIAssistant
{
    class WorkflowRegistry;
    class TriggerEngine;

    // -------------------------------------------------------------------------
    // WorkflowTriggerBinder
    // -------------------------------------------------------------------------
    // Responsibility:
    //  * Read WorkflowDefinition::m_Triggers from WorkflowRegistry
    //  * Interpret trigger params JSON (cron / file_watch)
    //  * Register all triggers into TriggerEngine
    //
    // This keeps JarvisAgent focused on wiring subsystems while the details of
    // JCWF trigger semantics live next to other workflow code.
    // -------------------------------------------------------------------------
    class WorkflowTriggerBinder
    {
    public:
        WorkflowTriggerBinder() = default;
        ~WorkflowTriggerBinder() = default;

        // Register all triggers for all workflows currently loaded in registry
        // into the given TriggerEngine instance.
        void RegisterAll(WorkflowRegistry const& workflowRegistry, TriggerEngine& triggerEngine) const;
    };

} // namespace AIAssistant
