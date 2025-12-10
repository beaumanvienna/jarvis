# JC Workflow Runtime – Software Design (application/workflow)

This document describes the **implementation** of the JC Workflow File (JCWF) runtime in the `application/workflow` folder.  
It is strictly based on the current C++ source code and the JCWF specification v1.0.

---

## 1. Scope

The workflow runtime in `application/workflow` is responsible for:

- Representing JCWF workflows in memory.
- Parsing JCWF JSON into these in‑memory structures.
- Validating workflows (structure, references, DAG properties).
- Registering workflows and giving read‑only access to them.
- Resolving dataflow wiring between tasks.
- Managing triggers (auto, cron, file‑watch, manual) and invoking callbacks.
- Orchestrating one workflow run at a time:
  - choosing ready tasks,
  - checking “up‑to‑date” status using file timestamps and dependencies,
  - resolving task inputs (including dataflow),
  - dispatching task execution via task executors,
  - tracking per‑task and per‑run state.

Execution of individual tasks is delegated to `ITaskExecutor` implementations, which bridge to Python, shell, or internal C++ logic.

All code lives in the **`AIAssistant`** namespace.

---

## 2. File Overview

All files listed here live under `application/workflow/` in the repository.

### 2.1 Data Model and Utilities

- **`workflowTypes.h`**  
  Defines the core workflow data model used across the module:
  - Enumerations for task type/mode, trigger type, and task instance state.
  - Value types such as retry policy and task environment.
  - `TaskDef`, `WorkflowTrigger`, `DataflowDef`, `WorkflowRunState`, `TaskInstanceState`, and `WorkflowDefinition`.
  These types are the central in‑memory representation of a JCWF workflow and its runs.

- **`workflowDataflow.h`**  
  Small helper header providing type aliases for dataflow structures (based on `DataflowDef`).

- **`dataflowResolver.h/.cpp`**  
  Implements `DataflowResolver`, a helper that:
  - Examines a `WorkflowDefinition`, `WorkflowRunState`, and a `TaskDef`.
  - Computes a set of **resolved input values** for a task instance based on
    - explicit `dataflow` links from other tasks’ outputs, and
    - (future) workflow context (see TODOs in the code).
  - Produces a `ResolvedInputs` map and a list of unresolved required inputs.

---

### 2.2 Parsing and Validation

- **`workflowJsonParser.h/.cpp`**  
  Provides the public parsing entry point:

  ```cpp
  class WorkflowJsonParser
  {
  public:
      struct ParseResult
      {
          bool                         success;
          std::string                  errorMessage;
          std::optional<WorkflowDefinition> workflow;
      };

      static ParseResult ParseWorkflowJson(std::string const& jsonText,
                                           std::string const& sourcePath);
  };
  ```

  Responsibilities:
  - Uses **simdjson ondemand** to parse JCWF JSON text.
  - Applies a **two‑phase** workflow:
    1. Checks the top‑level object and required fields.
    2. Delegates detailed parsing of `tasks`, `triggers`, and `dataflow` to helpers in `workflowJsonParserDetails.cpp`.
  - On success, returns a fully populated `WorkflowDefinition`.
  - On failure, returns `success == false` and an explanatory `errorMessage` (including context such as `sourcePath`).

- **`workflowJsonParserDetails.cpp`**  
  Internal helpers (no public interface) that parse structured sections of JCWF into `TaskDef`, `WorkflowTrigger`, and `DataflowDef`:

  - `ParseTasksObject(...)`
  - `ParseTaskObject(...)`
  - `ParseTriggersArray(...)`
  - `ParseDataflowArray(...)`
  - Small helpers to parse enums (`TaskType`, `TaskMode`, `WorkflowTriggerType`), retry policies, file input/output arrays, and environment / queue binding sub‑objects.

  The code directly maps JCWF fields to C++ fields, for example:
  - `tasks` → `WorkflowDefinition::m_Tasks` (`std::unordered_map<std::string, TaskDef>`).
  - `triggers` → `WorkflowDefinition::m_Triggers` (`std::vector<WorkflowTrigger>`).
  - `dataflow` → `WorkflowDefinition::m_Dataflow` (`std::vector<DataflowDef>`).
  - `defaults` → `WorkflowDefinition::m_DefaultsJson` (stored as JSON text/string for now).

  Validation performed during parsing includes:
  - Checking required fields (`version`, `id`, `tasks`).
  - Ensuring task IDs and trigger IDs are unique.
  - Ensuring referenced tasks in `depends_on` and dataflow links exist.
  - Enforcing JCWF enums (invalid strings → parse error).

  - If the JCWF root object does **not** contain a `triggers` field, the parser
    synthesizes a default trigger:
    - `WorkflowTriggerType::Auto`
    - id `"auto"`
    - `enabled == true`
    - empty `params` (`{}`)
    This matches the JCWF spec rule “if no trigger is provided, `auto` is
    assumed as the default trigger”.

---

### 2.3 Workflow Registry

- **`workflowRegistry.h/.cpp`**  

  The `WorkflowRegistry` owns all loaded workflows and provides read‑only access to them.

  Public interface (high‑level):

  - `LoadDirectory(std::filesystem::path const& directoryPath)`  
    Scans a directory for `.jcwf` files, parses each using `WorkflowJsonParser`, and registers successful workflows.

  - `LoadFile(std::filesystem::path const& filePath)`  
    Loads a single workflow file.

  - `HasWorkflow(std::string const& id) const`  
    Checks if a workflow with the given ID is registered.

  - `GetWorkflow(std::string const& id) const`  
    Returns a `WorkflowDefinition const*` for a registered workflow (or `nullptr`).

  - `GetWorkflowIds() const`  
    Returns a list of all registered workflow IDs (used by the UI layer or higher‑level engine).

  - `ValidateAll() const`  
    Runs additional cross‑workflow checks, such as:
    - Ensuring task dependency DAGs have **no cycles**.
    - Ensuring dataflow links refer to existing tasks and slots.
    - Checking consistency between triggers, tasks, and dataflow.

  Internally, workflows are stored in a map keyed by `WorkflowDefinition::m_Id`.  
  The registry is the **single source of truth** for available workflows in the process.

---

### 2.4 Triggers

- **`triggerEngine.h/.cpp`**  
  `TriggerEngine` is a small component that turns **already-parsed JCWF trigger definitions** into runtime trigger instances and exposes a callback when a trigger fires.

  In the current design, the JSON `params` for each trigger are interpreted by
  `WorkflowTriggerBinder` (see below), which then calls `TriggerEngine` with
  concrete values such as cron expressions or file paths.

  Key public pieces:

  - `struct TriggerFiredEvent`
    - `std::string m_WorkflowId;`
    - `std::string m_TriggerId;`

  - `using TriggerCallback = std::function<void(TriggerFiredEvent const&)>;`  
    The callback is supplied by the outer application (JarvisAgent). When a
    trigger condition is satisfied, `TriggerEngine` calls this callback with
    the workflow id and trigger id.

  - `explicit TriggerEngine(TriggerCallback const& callback)`  
    Stores the application-provided callback that will be invoked when a
    trigger fires.

  - `void AddAutoTrigger(std::string const& workflowId,
                         std::string const& triggerId,
                         bool               isEnabled);`  
    Registers an **auto trigger**. If `isEnabled` is true, the trigger fires
    immediately upon registration. This is what implements the JCWF behavior
    “if no triggers are defined, a default `auto` trigger is assumed”.

  - `void AddCronTrigger(std::string const& workflowId,
                         std::string const& triggerId,
                         std::string const& expression,
                         bool               isEnabled);`  
    Registers a cron trigger. The `expression` is the raw cron expression
    string (5 fields) taken from JCWF `params.expression`. `TriggerEngine`
    parses and validates it internally; if parsing fails, the trigger is
    stored as disabled and never fires.

  - `void AddFileWatchTrigger(std::string const& workflowId,
                              std::string const& triggerId,
                              std::string const& path,
                              std::vector<FileEventType> const& events,
                              uint32_t          debounceMilliseconds,
                              bool              isEnabled);`  
    Registers a file-watch trigger:
    - `path` is the watched file path (as parsed from JCWF).
    - `events` is a list of allowed file events (Created / Modified / Deleted).
    - `debounceMilliseconds` controls minimum time between firings.
    - `isEnabled` controls whether the trigger may fire.

  - `void AddManualTrigger(std::string const& workflowId,
                           std::string const& triggerId,
                           bool               isEnabled);`  
    Registers a manual trigger, which can later be activated via
    `FireManualTrigger`.

  - `void ClearWorkflowTriggers(std::string const& workflowId);`  
    Removes all triggers (auto, cron, file-watch, manual) associated with a
    given workflow id. Also rebuilds the internal file-path → trigger index
    map used for fast file event matching.

  - `void Tick(std::chrono::system_clock::time_point const& now);`  
    Called periodically by the application. Checks all registered cron triggers
    and fires those whose next fire time is ≤ `now`, then updates their
    next-fire timestamp.

  - `void NotifyFileEvent(std::string const& path,
                          FileEventType      fileEventType,
                          std::chrono::system_clock::time_point const& now);`  
    Called by JarvisAgent when a file event is observed (from `FileWatcher`).
    Matches the event against registered file-watch triggers and fires them,
    respecting `events` and `debounce_ms`.

  - `void FireManualTrigger(std::string const& workflowId,
                            std::string const& triggerId);`  
    Looks up a previously registered manual trigger and fires it if enabled,
    otherwise logs a warning.

  Data structures:

  - `enum class FileEventType`  
    Represents the file events understood by file-watch triggers
    (`Created`, `Modified`, `Deleted`).

  - `struct CronTriggerInstance`  
    Stores:
    - `workflowId`
    - `triggerId`
    - parsed cron expression (`CronExpression`)
    - `m_IsEnabled`
    - next fire time (`m_NextFireTime`).

  - `struct FileWatchTriggerInstance`  
    Stores:
    - `workflowId`
    - `triggerId`
    - `m_WatchedPath`
    - `m_Events` (allowed `FileEventType`s)
    - `m_DebounceInterval`
    - `m_LastFireTime`
    - `m_HasFiredOnce`
    - `m_IsEnabled`.

  - `struct ManualTriggerInstance`  
    Stores:
    - `workflowId`
    - `triggerId`
    - `m_IsEnabled`.

  The `TriggerEngine` itself does **not** run workflows; it only notifies the
  outer application via the callback when a trigger condition is satisfied.

- **`workflowTriggerBinder.h/.cpp`**  

  `WorkflowTriggerBinder` is a small helper that connects the **parsed JCWF
  triggers** (`WorkflowDefinition::m_Triggers`) to `TriggerEngine`:

  - `void RegisterAll(WorkflowRegistry const& workflowRegistry,
                      TriggerEngine&          triggerEngine) const;`  
    For each registered workflow, iterates over its `WorkflowTrigger` entries
    and:

    - For `WorkflowTriggerType::Auto`, calls `AddAutoTrigger(...)`.
    - For `Cron`, parses `params.expression` and calls `AddCronTrigger(...)`.
    - For `FileWatch`, parses `params.path`, `params.events`, `params.debounce_ms`
      and calls `AddFileWatchTrigger(...)`.
    - For `Manual`, calls `AddManualTrigger(...)`.
    - For `Structure`, logs that the trigger is used only for per-item expansion
      and does not register a runtime trigger.

  This keeps JCWF JSON interpretation close to the workflow code and leaves
  `TriggerEngine` responsible only for runtime trigger scheduling and firing.

---

### 2.5 Task Execution

- **`taskExecutor.h`**  

  Defines the interface for all task executors:

  ```cpp
  class ITaskExecutor
  {
  public:
      virtual ~ITaskExecutor() = default;

      virtual bool Execute(WorkflowDefinition const& workflowDefinition,
                           WorkflowRun&            workflowRun,
                           TaskDef const&          taskDefinition,
                           TaskInstanceState&      taskState) = 0;
  };
  ```

  The orchestrator passes:
  - the immutable `WorkflowDefinition`,
  - the mutable `WorkflowRun` structure (shared across tasks),
  - the `TaskDef` for this task,
  - and the `TaskInstanceState` to update (`Succeeded` / `Failed`, outputs, error message).

- **`aiCallTaskExecutor.h/.cpp`**  

  Implements `ITaskExecutor` for JCWF tasks with `TaskType::AiCall`.  
  Responsibilities:
  - Uses the AI call parameters (provider, model, mode, prompt template, environment/queue binding) stored in `TaskDef::m_ParamsJson` and related fields.
  - Delegates the actual HTTP/SDK call to the AI backend through existing JarvisAgent abstractions (Python or C++), as wired in the implementation.
  - Writes task result and status back into `TaskInstanceState`.

- **`pythonTaskExecutor.h/.cpp`**  

  Implements `ITaskExecutor` for `TaskType::Python`.  
  Responsibilities:
  - Invokes the configured Python module + function (as parsed from JCWF) via `PythonEngine`.
  - Passes resolved inputs and workflow run context.
  - Updates `TaskInstanceState` with success/failure and any returned outputs.

- **`shellTaskExecutor.h/.cpp`**  

  Implements `ITaskExecutor` for `TaskType::Shell`.  
  Responsibilities:
  - Builds a command line from:
    - the `command` and `args` in the task parameters,
    - optional environment variables defined in `TaskEnvironment`.
  - Executes the command (subject to security restrictions implemented in code).
  - Marks the task as succeeded or failed based on process exit status.

- **`internalTaskExecutor.h/.cpp`**  

  Implements `ITaskExecutor` for `TaskType::Internal`.  
  Responsibilities:
  - Handles built‑in engine functions that are modeled as tasks (for example, updating status, writing summary files, etc.).  
  - Actual behavior is defined by the switch logic in `internalTaskExecutor.cpp`.

- **`taskExecutorRegistry.h/.cpp`**  

  Acts as a **factory/registry** that maps `TaskType` to concrete executors:

  - `TaskExecutorRegistry::TaskExecutorRegistry(...)`  
    Constructor wires dependencies needed by executors (for example, `PythonEngine` and AI client interfaces).

  - `RegisterDefaultExecutors()`  
    Creates one instance of each executor type and registers them:
    - `TaskType::Python` → `PythonTaskExecutor`
    - `TaskType::Shell` → `ShellTaskExecutor`
    - `TaskType::AiCall` → `AiCallTaskExecutor`
    - `TaskType::Internal` → `InternalTaskExecutor`

  - `ITaskExecutor* GetExecutor(TaskType type)`  
    Returns the executor instance associated with the given `TaskType` (or `nullptr` if none is registered).

---

### 2.6 Orchestration

- **`workflowOrchestrator.h/.cpp`**  

  `WorkflowOrchestrator` owns the core logic for **running** workflows.

  High‑level responsibilities:

  - **Integration points**
    - Holds a reference to `WorkflowRegistry` (to look up workflow definitions by ID).
    - Holds a reference to `TaskExecutorRegistry` (to dispatch tasks).
    - Uses `DataflowResolver` to derive task inputs from prior task outputs and dataflow definitions.

  - **Running a workflow once**

    ```cpp
    class WorkflowOrchestrator
    {
    public:
        explicit WorkflowOrchestrator(WorkflowRegistry&      workflowRegistry,
                                      TaskExecutorRegistry&  taskExecutorRegistry);

        // Run a workflow by ID, return a completed WorkflowRunState
        std::optional<WorkflowRunState> RunWorkflowOnce(std::string const& workflowId);
    };
    ```

    `RunWorkflowOnce`:
    1. Looks up the `WorkflowDefinition` from `WorkflowRegistry`.
    2. Constructs an initial `WorkflowRunState` for this run (run id, timestamps, initial task states).
    3. Delegates to `ExecuteWorkflow(...)` to perform scheduling and execution.
    4. Returns the final `WorkflowRunState` (or `std::nullopt` if the workflow does not exist).

  - **Scheduling and execution**

    Internal methods (as seen in `workflowOrchestrator.cpp`) include:

    - `ExecuteWorkflow(WorkflowDefinition const& workflow,
                       WorkflowRunState&         runState)`  
      Main loop that repeatedly:
      - identifies ready tasks,
      - checks up‑to‑date status,
      - calls executors for tasks that must run,
      - updates per‑task and run‑level status,
      - terminates when no further tasks can be run.

    - `ExecuteOneReadyWave(...)`  
      Executes a “wave” of ready tasks that can run in this scheduling step.  
      This is a natural point for parallelization (the current implementation chooses a strategy explicitly in the code).

    - `ComputeReadyTasks(...)`  
      Computes which tasks are **ready** by:
      - considering `depends_on`,
      - checking that all prerequisites have succeeded,
      - ensuring they have not already run or skipped.

    - `IsTaskUpToDate(...)`  
      Implements JCWF’s freshness semantics using `file_inputs` and `file_outputs`:
      - If any declared output is missing → not up‑to‑date.
      - If any declared input or upstream output is newer than this task’s outputs → not up‑to‑date.
      - Otherwise, task is considered **up‑to‑date** and may be skipped.

    - Dataflow integration:
      - Before executing a task, the orchestrator calls `DataflowResolver` to build the input map.
      - If required inputs cannot be resolved, the task fails fast with a validation error.

  - **Run tracking**

    The orchestrator updates the `WorkflowRunState` for:
    - start / end timestamps of the run,
    - per‑task `TaskInstanceStateKind` (Pending, Skipped, Running, Succeeded, Failed),
    - error messages (stored on `TaskInstanceState`).

    A small `m_LastRuns` map keeps the last run for each workflow ID for inspection by the caller (e.g., web UI or higher‑level engine).

---

## 3. Core Data Structures (`workflowTypes.h`)

This section summarizes the key types defined in `workflowTypes.h` and how they relate to the JCWF spec. Only the fields observable in the header are described.

### 3.1 Enums

- **`enum class TaskMode`**  
  - Represents JCWF `"mode"` per task.
  - Values include `Single` and `PerItem`, matching `"single"` / `"per_item"`.

- **`enum class TaskType`**  
  - Represents JCWF `"type"` per task.
  - Values include `Python`, `Shell`, `AiCall`, `Internal`.

- **`enum class WorkflowTriggerType`**  
  - Represents JCWF trigger `"type"`.
  - Values:
    - `Auto`       – fire automatically when the workflow is registered (used
                    both for explicit `"type": "auto"` triggers and for the
                    **implicit default trigger** when no `triggers` array is
                    provided in the JCWF file).
    - `Cron`       – time-based trigger using a cron expression.
    - `FileWatch`  – file-based trigger driven by `FileWatcher` events.
    - `Structure`  – structure-based trigger used to define per-item expansion
                    (for `mode: "per_item"` tasks); does not itself schedule a
                    run.
    - `Manual`     – manually invoked trigger from CLI / web UI.

- **`enum class TaskInstanceStateKind`**  
  - Represents the runtime state of a task instance within a run:
    - `Pending`, `Ready`, `Running`, `Skipped`, `Succeeded`, `Failed`.

### 3.2 Supporting Types

- **`struct RetryPolicy`**  
  - `int m_MaxAttempts;`
  - `int m_BackoffMs;`  
  Represents JCWF `"retries"` object.

- **`struct TaskEnvironment`**  
  - `std::string m_Name;`
  - `std::string m_AssistantId;`
  - `std::unordered_map<std::string, std::string> m_Variables;`  
  Represents JCWF `"environment"` section (assistant id and environment variables).

- **`struct TaskIOField`**  
  - `std::string m_Type;`
  - `bool        m_Required;`  
  Stores JCWF `"inputs"` / `"outputs"` slot metadata.

### 3.3 Task and Trigger Definitions

- **`struct TaskDef`**  
  Represents a **single JCWF task definition**. Key fields (names as in code):

  - `std::string              m_Id;`
  - `TaskType                 m_Type;`
  - `TaskMode                 m_Mode;`
  - `std::string              m_Label;`
  - `std::string              m_Doc;`
  - `std::vector<std::string> m_DependsOn;`
  - `std::vector<std::string> m_FileInputs;`
  - `std::vector<std::string> m_FileOutputs;`
  - `TaskEnvironment          m_Environment;`
  - `RetryPolicy              m_RetryPolicy;`
  - `std::optional<int>       m_TimeoutMs;`
  - `std::unordered_map<std::string, TaskIOField> m_Inputs;`
  - `std::unordered_map<std::string, TaskIOField> m_Outputs;`
  - `std::string              m_ParamsJson;` (raw JSON of `"params"`).

  This struct is the main bridge between JCWF task JSON and runtime behavior.

- **`struct WorkflowTrigger`**  
  Represents a **single JCWF trigger**:

  - `std::string          m_Id;`
  - `WorkflowTriggerType  m_Type;`
  - `bool                 m_Enabled;`
  - `std::string          m_ParamsJson;` (raw JSON of `"params"`).  

  The raw `params` JSON is interpreted by `WorkflowTriggerBinder` (and, for
  some low-level details such as cron parsing, by `TriggerEngine`) and then
  turned into concrete trigger configuration.

- **`struct DataflowDef`**  
  Represents a **single JCWF dataflow entry**:

  - `std::string m_FromTask;`
  - `std::string m_FromOutput;`
  - `std::string m_ToTask;`
  - `std::string m_ToInput;`
  - `std::string m_MappingJson;` (raw JSON of `"mapping"`).

  These entries are evaluated by `DataflowResolver`.

### 3.4 Workflow and Run State

- **`struct TaskInstanceState`**  
  Represents the runtime state of one task instance in a workflow run:

  - `TaskInstanceStateKind m_State;`
  - `int                   m_Attempt;`
  - `std::string           m_ErrorMessage;`
  - `std::filesystem::file_time_type m_LastExecutionTime;`
  - Any fields related to resolved inputs/outputs as present in the header.

- **`struct WorkflowRunState`**  
  Represents one **run** of a workflow:

  - `std::string m_RunId;`
  - `std::string m_WorkflowId;`
  - `std::chrono::system_clock::time_point m_StartTime;`
  - `std::optional<std::chrono::system_clock::time_point> m_EndTime;`
  - `std::unordered_map<std::string, TaskInstanceState> m_TaskStates;`  
    (per task ID).

  This struct is mutated by `WorkflowOrchestrator` as the run progresses and is returned to callers (e.g., UI code).

- **`struct WorkflowDefinition`**  
  Represents an entire JCWF workflow:

  - `std::string                         m_Version;`
  - `std::string                         m_Id;`
  - `std::string                         m_Label;`
  - `std::string                         m_Doc;`
  - `std::unordered_map<std::string, TaskDef> m_Tasks;`
  - `std::vector<WorkflowTrigger>        m_Triggers;`
  - `std::vector<DataflowDef>            m_Dataflow;`
  - `std::string                         m_DefaultsJson;` (raw `"defaults"` object).  

  It is a **pure data structure**; all behavior is implemented in the surrounding modules.

---

## 4. Relation to JCWF Specification

The implementation in `application/workflow` is a direct mapping of the JCWF spec v1.0 into C++:

- JCWF root fields (`version`, `id`, `label`, `doc`, `triggers`, `tasks`, `dataflow`, `defaults`)  
  → represented by `WorkflowDefinition` and parsed by `WorkflowJsonParser`.

- JCWF task fields (`type`, `mode`, `depends_on`, `file_inputs`, `file_outputs`, `environment`, `inputs`, `outputs`, `params`, `timeout_ms`, `retries`)  
  → represented by `TaskDef`, `TaskEnvironment`, `TaskIOField`, and `RetryPolicy`.

- JCWF triggers (`type`, `id`, `enabled`, `params`)  
  → represented by `WorkflowTrigger` and interpreted by `WorkflowTriggerBinder` / `TriggerEngine`.

- JCWF dataflow entries (`from_task`, `from_output`, `to_task`, `to_input`, `mapping`)  
  → represented by `DataflowDef` and interpreted by `DataflowResolver`.

- JCWF execution semantics (dependency DAG + freshness checks)  
  → implemented in `WorkflowOrchestrator` using:
  - `TaskDef::m_DependsOn`,
  - `TaskDef::m_FileInputs` / `m_FileOutputs`,
  - `TaskInstanceState` and `WorkflowRunState`.

This document only describes behavior that is present in the current source code; it does not introduce any additional features beyond what is implemented.
