# JC Workflow File Format (extension jcwf)

Copyright (c) 2025 JC Technolabs<br>
License: GPL-3.0

---

## Abstract

This document specifies the **JC Workflow File format (JCWF)** and the corresponding execution model for **JarvisAgent**.  
JCWF is a **JSON-based** workflow description language that allows JarvisAgent to:

- Define workflows as graphs of **tasks** (a workflow *pipeline*).
- Express **dependencies** between tasks (Makefile-style, with file freshness checks).
- Attach **triggers** (cron-like, file-based, structure-based).
- Execute tasks using **C++ modules** and **Python scripts**, including AI assistant calls.
- Monitor execution and propagate **data** from one task to another.
- Integrate with the **JarvisAgent web UI** for visualization and control.

This specification focuses on:

1. Defining Workflows (JSON schema and semantics)  
2. Managing Dependencies (task- and file-based)  
3. Handling Triggers  
4. Executing Tasks (C++ and Python responsibilities)  
5. Monitoring & Reporting  
6. Data Flow between tasks  
7. Integration with JarvisAgent’s architecture (C++, Python, Web UI)  

---

## 1. Introduction

JarvisAgent orchestrates a variety of automation tasks: PDF-to-markdown conversion, AI assistant queries, file watching, and more.  
The **JC Workflow File (JCWF)** provides a declarative way to describe these tasks and their relationships, so that JarvisAgent can:

- Load workflows at startup or on demand.  
- Run them reactively (on triggers) or explicitly (manual trigger from CLI / web UI).  
- Parallelize non-dependent tasks.  
- Track progress and expose status to the web dashboard.

You can think of each workflow as a **pipeline**: a sequence/DAG of stages, similar in spirit to a Vulkan rendering pipeline, but for automation tasks rather than shaders and fixed-function stages.

This document defines:

- The JSON structure of a `.jcwf` file.  
- The execution semantics inside JarvisAgent.  
- The interactions between C++ core, Python scripting engines, and the web UI.

### 1.1 Requirements Language

The key words **"MUST"**, **"MUST NOT"**, **"REQUIRED"**, **"SHALL"**, **"SHALL NOT"**, **"SHOULD"**, **"SHOULD NOT"**, **"RECOMMENDED"**, **"MAY"**, and **"OPTIONAL"** in this document are to be interpreted as described in [ RFC 2119 ](https://datatracker.ietf.org/doc/html/rfc2119).

---

## 2. Terminology

- **Workflow**: A named collection of tasks, triggers, and global configuration.  
- **Task**: A unit of work (e.g., convert a PDF, call an AI assistant, run a Python function, invoke a shell command).  
- **Dependency**: A requirement that one or more tasks **MUST** complete successfully (and be up to date) before another task starts.  
- **Trigger**: A condition that **starts** a workflow or a task:  
  - Time-based (cron-like)  
  - File-based (XLS/CSV/Markdown changes)  
  - Structure-based (e.g., “for each subsection in chapter 3”)  
  - Manual (explicit command or UI action)  
- **Data Slot**: A named input or output field associated with a task.  
  - Example: task `convert_pdf` with input slot `pdf_path` and output slot `markdown_path`.  
- **Context / State**: A key-value store that persists data across tasks within a workflow run.  
  - Example: `context["today"] = "2025-12-01"` or `context["report_url"] = "https://..."`.  
- **Run**: A single execution instance of a workflow with its own state and logs.  
- **JCWF Runtime**: The JarvisAgent orchestration layer that loads, validates, and runs JCWF workflows.

---

## 3. JCWF JSON Specification

JCWF files **MUST** be valid JSON documents.  
The file extension **SHOULD** be `.jcwf`.

### 3.1 Root Object

The root object has the following top-level fields:

```jsonc
{
  "version": "1.0",
  "id": "daily-report",
  "label": "Daily Reporting Workflow",
  "doc": "Generates a daily report from XLS and sends it to an AI assistant for summarization.",
  "triggers": [ /* see 3.2 */ ],
  "tasks": { /* see 3.3 */ },
  "dataflow": [ /* see 3.5 */ ],
  "defaults": { /* see 3.6 */ }
}
```

#### 3.1.1 Fields

- `version` (REQUIRED, string)  
  - The JCWF spec version. For this document, `"1.0"` is assumed.
  - Implementations **MUST** reject unknown major versions.

- `id` (REQUIRED, string)  
  - Unique identifier for this workflow within the JarvisAgent environment.  
  - When workflows are created externally (e.g., from the web UI), the UI **SHOULD** generate an ID that is unique and stable (e.g., a UUID or a slug).  
  - JarvisAgent **MUST** reject a new workflow whose `id` collides with an already loaded workflow.

- `label` (OPTIONAL, string)  
  - Human-friendly name for UI display.

- `doc` (OPTIONAL, string or array of strings)  
  - Documentation or comments about the workflow.

- `triggers` (OPTIONAL, array of trigger objects)  
  - Defines when and how the workflow starts. See **3.2**.

- `tasks` (REQUIRED, object)  
  - Map from **taskId** to a task specification. See **3.3**.

- `dataflow` (OPTIONAL, array)  
  - Explicit data wiring between task outputs and inputs. See **3.5**.

- `defaults` (OPTIONAL, object)  
  - Default settings for tasks, retries, timeouts, etc. See **3.6**.

---

### 3.2 Triggers

A workflow MAY be started by one or more triggers. Manual start is always allowed unless explicitly disabled.

Each trigger has:

```jsonc
{
  "type": "cron | file_watch | structure | manual",
  "id": "morning-run",
  "enabled": true,
  "params": { /* type dependent */ }
}
```

#### 3.2.1 Cron Triggers

```jsonc
{
  "type": "cron",
  "id": "every-morning",
  "enabled": true,
  "params": {
    "expression": "0 8 * * *",
    "timezone": "America/Los_Angeles"
  }
}
```

- `expression` (REQUIRED) is a standard 5-field cron expression.  
- `timezone` (OPTIONAL) defaults to system time.

#### 3.2.2 File-Watch Triggers

```jsonc
{
  "type": "file_watch",
  "id": "on-xls-change",
  "enabled": true,
  "params": {
    "path": "data/report.xlsx",
    "events": ["modified", "created"],
    "debounce_ms": 2000
  }
}
```

- Tied to JarvisAgent’s existing `FileWatcher`.  
- **MUST** map events to JarvisAgent event types.

#### 3.2.3 Structure-Based Triggers

Used for “for each row / section” style operations. These **do not** schedule time; rather, they define **how** to expand tasks when the workflow is triggered.

```jsonc
{
  "type": "structure",
  "id": "each-row-in-xls",
  "enabled": true,
  "params": {
    "source": {
      "kind": "xls",
      "path": "data/tasks.xlsx",
      "sheet": "Sheet1"
    },
    "iterator": {
      "mode": "rows",
      "range": "A2:D100",
      "binding": "row" 
    }
  }
}
```

At runtime, the engine will expand designated tasks from this **iterator** (see tasks with `"mode": "per_item"` in 3.3.2).

#### 3.2.4 Manual Triggers

```jsonc
{
  "type": "manual",
  "id": "manual-run",
  "enabled": true,
  "params": {
    "exposed_in_ui": true
  }
}
```

Manual triggers are exposed in the web UI and/or CLI.

---

### 3.3 Tasks

The `tasks` field is a mapping from **taskId** to a task object:

```jsonc
"tasks": {
  "load_xls": { ... },
  "summarize": { ... },
  "notify": { ... }
}
```

Each task has:

```jsonc
{
  "id": "summarize",
  "type": "python | shell | ai_call | internal",
  "label": "Summarize report with AI",
  "doc": "Sends the prepared text to an AI assistant and stores the answer.",
  "mode": "single | per_item",
  "depends_on": ["load_xls"],
  "file_inputs": ["data/report.xlsx"],
  "file_outputs": ["output/report.summary.txt"],
  "params": { /* type-specific */ },
  "inputs": { /* named inputs */ },
  "outputs": { /* named outputs */ },
  "timeout_ms": 600000,
  "retries": {
    "max_attempts": 3,
    "backoff_ms": 1000
  }
}
```

#### 3.3.1 Task Types

- `python`  
  - Executes a function or script via the PythonEngine.

  ```jsonc
  {
    "id": "convert_pdf",
    "type": "python",
    "params": {
      "module": "workflows.pdf_tasks",
      "function": "convert_pdf_to_markdown"
    },
    "inputs": {
      "pdf_path": {"type": "string"},
      "output_dir": {"type": "string"}
    },
    "outputs": {
      "markdown_path": {"type": "string"}
    },
    "file_inputs": ["${inputs.pdf_path}"],
    "file_outputs": ["${outputs.markdown_path}"]
  }
  ```

- `shell`  
  - Executes a command on the host (JarvisAgent SHOULD restrict/whitelist this).  
  - **Security rule**: shell commands **MUST** start with `scripts/` (relative to the JarvisAgent working directory).  
    - Example: `"command": "scripts/clean_artifacts.sh"`

  ```jsonc
  {
    "id": "run_script",
    "type": "shell",
    "params": {
      "command": "scripts/run_something.sh",
      "args": ["--flag", "value"]
    }
  }
  ```

- `ai_call`  
  - A high-level “call AI assistant” task that routes via Python or C++ backend.

  ```jsonc
  {
    "id": "ask_ai",
    "type": "ai_call",
    "params": {
      "provider": "openai",
      "model": "gpt-4.1-mini",
      "prompt_template": "Summarize the following report:\n{{report_text}}"
    },
    "inputs": {
      "report_text": {"type": "string"}
    },
    "outputs": {
      "summary": {"type": "string"}
    }
  }
  ```

- `internal`  
  - Built-in C++ actions, like updating status, writing to ChatMessagePool, creating STNG_/TASK_/CNXT files, etc.

#### 3.3.2 Mode: `single` vs `per_item`

- `mode: "single"` (default)  
  - Task executes once per workflow run.

- `mode: "per_item"`  
  - Task is **expanded** per iterator item (e.g., each row in XLS, each subsection in a document).  
  - The expansion is driven by a **structure trigger** (3.2.3) or by explicit dataflow list sources.

Example:

```jsonc
{
  "id": "summarize_section",
  "type": "ai_call",
  "mode": "per_item",
  "params": { "provider": "openai", "model": "gpt-4.1-mini" },
  "inputs": {
    "section_text": {"type": "string"},
    "section_title": {"type": "string"}
  },
  "outputs": {
    "section_summary": {"type": "string"}
  },
  "file_inputs": ["${inputs.section_text}"],
  "file_outputs": ["output/sections/${inputs.section_title}.summary.txt"]
}
```

At runtime, the **iterator binding** (e.g., `"row"`, `"subsection"`) will be injected into the inputs, and thus into the `file_inputs` / `file_outputs` templates.

#### 3.3.3 Timeouts and Retries

- `timeout_ms` (OPTIONAL, integer)  
  - Maximum execution time per task instance.  
  - If exceeded, task is considered failed.

- `retries` (OPTIONAL, object)  
  - `max_attempts` (integer)  
  - `backoff_ms` (integer) linear backoff between retries.  
  - Implementation **MAY** extend with exponential strategy later.

#### 3.3.4 Inputs & Outputs (Data Slots)

Inputs and outputs are declared to aid validation and UI:

```jsonc
"inputs": {
  "pdf_path": { "type": "string", "required": true },
  "config": { "type": "object", "required": false }
},
"outputs": {
  "markdown_path": { "type": "string" }
}
```

- Each key is a **data slot** name.  
- Types are advisory but useful for sanity checks and editor tooling.

#### 3.3.5 Clean Tasks

A workflow **MAY** define a dedicated `clean` task that removes generated artifacts. For example:

```jsonc
"tasks": {
  "clean": {
    "id": "clean",
    "type": "shell",
    "label": "Clean artifacts",
    "params": {
      "command": "scripts/clean_artifacts.sh"
    }
  }
}
```

The orchestrator or UI **MAY** expose a “clean” action that simply runs this task (ignoring usual dependency checks).

---

### 3.4 Dependency Semantics and Makefile-Like Checks

JCWF models dependencies **per task**, similar to Makefile targets and prerequisites.

Each task can declare:

- `depends_on` (OPTIONAL, array of task IDs)  
  - Other tasks that **must succeed** before this task is considered **ready**.  
- `file_inputs` (OPTIONAL, array of strings)  
  - Files or patterns this task reads from.  
- `file_outputs` (OPTIONAL, array of strings)  
  - Files or patterns this task produces/updates.

Example (Makefile-like) for “chunk an MD file if the output is missing or stale”:

```jsonc
"tasks": {
  "chunk_book": {
    "id": "chunk_book",
    "type": "python",
    "label": "Chunk MD file book.md",
    "params": {
      "module": "workflows.chunking",
      "function": "chunk_markdown_file"
    },
    "inputs": {
      "input_path": { "type": "string", "required": true },
      "output_path": { "type": "string", "required": true }
    },
    "file_inputs": ["book.md"],
    "file_outputs": ["book.output.md"]
  }
}
```

**Rules (Makefile-style):**

1. **Task graph**  
   - `depends_on` defines a **task-level DAG**.  
   - If a task has no `depends_on`, it is considered a **root** task (subject to triggers).  
   - The workflow **MUST NOT** contain cycles in `depends_on`. Cycles SHOULD be detected and rejected at load time.

2. **Up-to-date check**  
   - A task **MAY** be skipped as “up to date” if all of the following are true:  
     - All `file_outputs` exist, and  
     - Each `file_output` has a modification time **newer than or equal to** every `file_input` and all upstream outputs from `depends_on` tasks.  
   - If any `file_output` is missing, or any `file_input`/upstream output is newer, the task is considered **stale** and **MUST** run.  
   - If `file_inputs` or `file_outputs` are omitted, the engine **MUST** assume the task is **not** up to date and SHOULD run it whenever its dependencies are satisfied.

3. **Per-item mode**  
   - For `mode: "per_item"` tasks, the same freshness rules apply **per item**.  
   - `file_inputs` / `file_outputs` may use templates (e.g., `"output/${inputs.section_title}.summary.txt"`). The runtime evaluates templates per item before checking timestamps.

4. **Interaction with triggers**  
   - A trigger (cron, file, structure, manual) creates a **new workflow run**.  
   - Within that run, each task is examined for readiness and freshness as above.  
   - It is valid to model a “no-op” run where all tasks are up to date and thus skipped.

---

### 3.5 Data Flow

Optional explicit wiring of outputs to inputs.

```jsonc
"dataflow": [
  {
    "from_task": "load_xls",
    "from_output": "rows",
    "to_task": "summarize_section",
    "to_input": "section_text",
    "mapping": {
      "use_field": "A"  // e.g., from XLS column A
    }
  },
  {
    "from_task": "summarize_section",
    "from_output": "section_summary",
    "to_task": "notify",
    "to_input": "body"
  }
]
```

Semantics:

- The runtime **MUST** ensure that the source task has completed and produced the referenced output before starting the target task.  
- For `per_item` tasks, dataflow can create **fan-out**: one `load_xls` task -> many `summarize_section` tasks.

If `dataflow` is omitted, tasks may rely purely on the workflow **state** (context) or external files.

---

### 3.6 Defaults

`defaults` supplies common configuration inherited by tasks unless overridden.

```jsonc
"defaults": {
  "timeout_ms": 600000,
  "retries": {
    "max_attempts": 2,
    "backoff_ms": 1000
  },
  "ai": {
    "provider": "openai",
    "model": "gpt-4.1-mini"
  }
}
```

- Task-specific fields override defaults at the same key path.

---

## 4. Execution Model

This section describes how JarvisAgent should execute JCWF workflows across **C++ core**, **Python engines**, and the **web UI**.

### 4.1 High-Level Flow

1. **Load** `.jcwf` files (from a configured directory).  
2. **Validate** JSON structure, triggers, and `depends_on` DAG (no cycles).  
3. **Register** workflows and triggers with JarvisAgent core.  
4. On trigger activation (cron / file / structure / manual):  
   - Create a **workflow run instance** with its own ID and context.  
   - Resolve **ready tasks**:  
     - All `depends_on` tasks succeeded, and  
     - Inputs are resolvable, and  
     - Task is not up to date (or up-to-date checking is disabled).  
   - Schedule tasks on worker pools and/or Python engines.  
5. **Monitor** task states, store outputs in a run-local state store (context).  
6. **Propagate data** as specified by `dataflow`.  
7. **Update web UI** with real-time status (pending, running, skipped, success, failed).  
8. Mark workflow run **completed** when no further tasks can run.

---

### 4.2 C++ Side: Core Orchestrator

Responsibilities:

- Parse and hold in-memory representation of workflows:  
  - `WorkflowDefinition` (id, label, triggers, tasks, dataflow).  
- Listen to cron/file events and map them to workflow triggers.  
- Maintain a `WorkflowRun` object per workflow execution.  
- Perform dependency resolution and ready-task scheduling using `depends_on` and file freshness.  
- Assign tasks to:  
  - PythonEngine instances (for `python` and `ai_call` tasks using Python).  
  - Internal handlers (for `internal` tasks).  
  - Shell executor (for `shell` tasks, if allowed).  
- Track task status (`Pending`, `Ready`, `Running`, `Skipped`, `Succeeded`, `Failed`).  
- Emit events for UI and logging (e.g., `WorkflowRunStartedEvent`, `TaskStatusChangedEvent`).

Recommended data structures:

- `WorkflowDefinition`  
  - `std::string id;`  
  - `std::unordered_map<std::string, TaskDef> tasks;`  
  - `std::vector<DataflowDef> dataflows;`  
  - `std::vector<TriggerDef> triggers;`

- `TaskDef` (conceptual)  
  - `std::string id;`  
  - `std::vector<std::string> dependsOn;`  
  - `std::vector<std::string> fileInputs;`  
  - `std::vector<std::string> fileOutputs;`  
  - `TaskType type;`  
  - `TaskMode mode;`  
  - `JsonLike params;`  
  - `TaskIO inputs;`  
  - `TaskIO outputs;`

- `WorkflowRun`  
  - `std::string runId;`  
  - `std::string workflowId;`  
  - `RunState state;`  
  - `std::unordered_map<std::string, TaskInstanceState> taskStates;`  
  - `JsonLikeState context; // e.g. nlohmann::json or similar`

The core orchestrator **SHOULD** be deterministic and thread-safe. It **MAY** use a task queue and worker threads, but heavy work (PDF conversion, AI calls) is delegated to Python or external processes.

Internally, JarvisAgent may map task execution to creation of STNG_, TASK_, and CNXT files in the queue directories; the specifics are an implementation detail, but JCWF gives the orchestrator a higher-level, structured view of what must happen.

---

### 4.3 Python Side: Task Executors

Python is responsible for:

- Implementing `python` tasks’ logic.  
- Acting as a **bridge** for `ai_call` tasks (HTTP requests to AI providers).  
- Optional helpers for reading XLS, documents, etc., for structure-based iteration.

Python interface expectations:

- For `python` tasks:

  ```python
  # Example module: workflows/pdf_tasks.py

  def convert_pdf_to_markdown(pdf_path: str, output_dir: str, context: dict) -> dict:
      # context: workflow run state (read-only or partial write)
      # return: dict of outputs, e.g. {"markdown_path": "..."}
      ...
  ```

- For `ai_call` tasks, either:  
  - a generic function that takes provider/model/prompt, or  
  - a specific module for workflow-specific logic.

Python tasks **SHOULD** avoid blocking the GIL unnecessarily (e.g., use I/O-bound calls, async HTTP, etc.). Long-running CPU tasks **MAY** be done via separate processes if needed.

---

### 4.4 Web UI: Monitoring & Control

The web interface **SHOULD**:

- List registered workflows (id, label, doc, triggers).  
- Allow manual trigger of workflows and manual **cancel** of runs.  
- Visualize each `WorkflowRun` as:  
  - A DAG of tasks (nodes) with statuses.  
  - A table or card view of all task instances.  
- Show logs and outputs for each task (where practical).  
- Permit per-task or per-workflow debugging (e.g., view context, last errors).

Data exchange between C++ and Web UI is via:

- Status snapshots broadcasted by `WebServer` (existing mechanism).  
- Possibly new routes such as `/workflows`, `/workflow/:id/runs`, `/workflow/:id/run/:runId/tasks`.

---

## 5. Managing Dependencies

### 5.1 Readiness Rule

A task instance is **ready** to execute when:

1. All tasks in its `depends_on` list have **Succeeded**.  
2. All its declared required inputs are resolvable from:
   - Dataflow links,  
   - Workflow context, or  
   - Static literals / defaults.  
3. The up-to-date check (3.4) either:  
   - Determines the task is stale, **or**  
   - Is explicitly disabled (implementation option).

Tasks with no `depends_on` and no missing required inputs **MAY** start immediately after the workflow is triggered.

### 5.2 Parallel Execution

If multiple tasks become **ready** at the same time and do not depend on each other (no path between them in the `depends_on` DAG), the orchestrator **SHOULD** run them in parallel, subject to resource limits (thread pool size, number of Python engines).

### 5.3 Failure Propagation

If a task fails (after all retries):

- Tasks that depend on it **MUST NOT** run, unless a future version introduces an explicit `"allow_failed_prereqs": true` override.  
- The workflow run status **SHOULD** be marked as `Failed`, unless the failure is confined to optional branches (implementation-defined policy).

---

## 6. Handling Triggers

### 6.1 Time-Based (Cron)

- C++ side integrates with a scheduler (e.g., one per second/minute) to check cron expressions.  
- When a cron fires, it creates a new **WorkflowRun**.

### 6.2 File-Based

- Integrated with the existing `FileWatcher`.  
- When a relevant event is received (path + type), the orchestrator checks matching triggers and starts a run.

### 6.3 Structure-Based

- The runtime uses the structure trigger params to **preprocess** input sources (XLS, documents, etc.).  
- It builds an iterator collection (e.g., list of rows / sections).  
- For `per_item` tasks, the orchestrator creates **one task instance per item**, injecting the k-th item as inputs.

### 6.4 Manual

- Web UI or CLI sends a “start workflow” command to the core, referencing workflow `id` and optional initial context.

---

## 7. Monitoring & Reporting

JarvisAgent **SHOULD** maintain for each workflow run:

- Start time, end time, duration.  
- Task instance statuses and timestamps.  
- Error messages, if any.  
- Key outputs (as configured).

The web UI **SHOULD** offer:

- A summary of recent runs (success/failure).  
- Drill-down views per run.  
- Log panels for debugging tasks.  

C++ **MAY** expose a compact JSON representation of workflows and runs over an HTTP endpoint for external monitoring.

---

## 8. Data Flow

Data flow is a combination of:

1. Explicit `dataflow` wiring.  
2. Shared run-level `context` (key-value).  
3. External files and side effects (e.g., generated markdown, XLS).

### 8.1 Task Input Resolution

When a task starts, its inputs are resolved from:

1. Dataflow links (`from_task` → `to_task`).  
2. Context fields (e.g., `context["config"]`).  
3. Static literals provided in `params` or `defaults` (like `provider`, `model`).

If a **required** input cannot be resolved, the task **MUST** fail fast with a validation error.

### 8.2 Outputs and Context

Task outputs **MAY** be written to:

- Local task outputs (for dataflow).  
- Shared context, if configured (e.g., `"write_to_context": true`).

The exact policy is left to implementation, but JCWF **SHOULD** allow a future extension to specify context writes explicitly.

---

## 9. JSON Schema (Draft)

Below is a **simplified** JSON Schema for JCWF v1.0. It is not exhaustive but is suitable for validation and editor tooling.

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "JC Workflow File (JCWF) v1.0",
  "type": "object",
  "required": ["version", "id", "tasks"],
  "properties": {
    "version": {
      "type": "string",
      "pattern": "^1\.0$"
    },
    "id": { "type": "string" },
    "label": { "type": "string" },
    "doc": {
      "anyOf": [
        { "type": "string" },
        { "type": "array", "items": { "type": "string" } }
      ]
    },
    "triggers": {
      "type": "array",
      "items": { "$ref": "#/$defs/trigger" }
    },
    "tasks": {
      "type": "object",
      "additionalProperties": { "$ref": "#/$defs/task" }
    },
    "dataflow": {
      "type": "array",
      "items": { "$ref": "#/$defs/dataflow" }
    },
    "defaults": {
      "type": "object",
      "additionalProperties": true
    }
  },
  "$defs": {
    "trigger": {
      "type": "object",
      "required": ["type", "id"],
      "properties": {
        "type": {
          "type": "string",
          "enum": ["cron", "file_watch", "structure", "manual"]
        },
        "id": { "type": "string" },
        "enabled": { "type": "boolean" },
        "params": { "type": "object" }
      }
    },
    "task": {
      "type": "object",
      "required": ["id", "type"],
      "properties": {
        "id": { "type": "string" },
        "type": {
          "type": "string",
          "enum": ["python", "shell", "ai_call", "internal"]
        },
        "label": { "type": "string" },
        "doc": { "type": "string" },
        "mode": {
          "type": "string",
          "enum": ["single", "per_item"],
          "default": "single"
        },
        "depends_on": {
          "type": "array",
          "items": { "type": "string" }
        },
        "file_inputs": {
          "type": "array",
          "items": { "type": "string" }
        },
        "file_outputs": {
          "type": "array",
          "items": { "type": "string" }
        },
        "params": { "type": "object" },
        "inputs": {
          "type": "object",
          "additionalProperties": {
            "type": "object",
            "properties": {
              "type": { "type": "string" },
              "required": { "type": "boolean" }
            }
          }
        },
        "outputs": {
          "type": "object",
          "additionalProperties": {
            "type": "object",
            "properties": {
              "type": { "type": "string" }
            }
          }
        },
        "timeout_ms": { "type": "integer" },
        "retries": {
          "type": "object",
          "properties": {
            "max_attempts": { "type": "integer" },
            "backoff_ms": { "type": "integer" }
          }
        }
      }
    },
    "dataflow": {
      "type": "object",
      "required": ["from_task", "to_task"],
      "properties": {
        "from_task": { "type": "string" },
        "from_output": { "type": "string" },
        "to_task": { "type": "string" },
        "to_input": { "type": "string" },
        "mapping": { "type": "object" }
      }
    }
  }
}
```

---

## 10. Security Considerations

- `shell` tasks can be dangerous; JarvisAgent **SHOULD** provide configuration flags to disable or restrict them and **MUST** enforce the `scripts/` prefix rule.  
- `ai_call` tasks send data to external services; sensitive data **MUST** be handled carefully.  
- JCWF files **SHOULD** be sourced from trusted locations; tampering can change automation behavior.  
- Structure-based iteration over external documents/XLS files **SHOULD** validate inputs to avoid unexpected expansion or injection.

---

## 11. Versioning

- Minor versions (`1.x`) **SHOULD** remain backward compatible with `1.0`.  
- Major versions (`2.0`, etc.) may introduce breaking changes; implementations **MUST** check `version` before execution.

---

*End of JC Workflow File (JCWF) Specification v1.0*
