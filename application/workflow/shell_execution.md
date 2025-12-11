# Shell Task Execution in JarvisAgent (make-example Workflow)

This document describes how JarvisAgent executes **shell-based tasks** for the `make-example` workflow (the small “makefile-style” compilation demo), which builds:

- `lib1.o`, `lib2.o`, `main.o`, `app.o`
- `libmylib.a` (static library)
- `myapp` (final executable)

It focuses on **what happens**, in **which order**, and **which major components are involved**, based on the current code and the log output you captured.

---

## 1. High-level Data Flow

At a high level, a run of the `make-example` workflow goes through these stages:

1. **Engine + App startup**
2. **Workflows loaded and registered**
3. **Trigger registered and fired**
4. **Workflow orchestrated as a DAG of tasks**
5. **Shell tasks executed in dependency order**
6. **Workflow marked as complete**

In log form, the critical lines are (abridged):

```text
[Application] [info] WorkflowRegistry::LoadDirectory scanning ../workflows
[Application] [info] Loading workflow file ../workflows/make_example.jcwf
[Application] [info] Registered workflow make-example
[Application] [info] Validating workflow make-example
[Application] [info] TriggerEngine::AddAutoTrigger: registered auto trigger 'auto' for workflow 'make-example'
[Application] [info] TriggerEngine::FireTrigger: firing trigger 'auto' for workflow 'make-example'
[Application] [info] JarvisAgent: Trigger fired for workflow 'make-example' (trigger id 'auto')
[Application] [info] WorkflowOrchestrator: Starting workflow 'make-example' (run id 'make-example_...')
```

Then the shell tasks:

```text
[Application] [info] [shell] Executing shell task 'compile_lib1'
[Application] [info] [shell] Command: scripts/compile.sh ../workflows/lib1.cpp ../workflows/lib1.o
...
[Application] [info] [shell] Executing shell task 'make_static_lib'
[Application] [info] [shell] Command: scripts/archive.sh ../workflows/lib1.o ../workflows/lib2.o ../workflows/libmylib.a
...
[Application] [info] [shell] Executing shell task 'make_executable'
[Application] [info] [shell] Command: scripts/link.sh ../workflows/main.o ../workflows/app.o ../workflows/libmylib.a ../workflows/myapp
[Application] [info] WorkflowOrchestrator: Workflow 'make-example' (...) completed successfully
```

---

## 2. Components / Classes Involved

From the logs and the current architecture, these are the main components that participate in running shell tasks for JCWF workflows:

1. **Engine / Core**
   - Reads the **engine config** (format, folders, API settings, etc.).  
   - Starts the main run loop, logging, thread pool, and file watcher.
   - Hands control to the **JarvisAgent application**.

2. **JarvisAgent Application**
   - Initializes subsystems (web server, Python engine, workflow system).
   - Calls **WorkflowRegistry::LoadDirectory** to load `.jcwf` files:
     - `../workflows/make_example.jcwf` in your case.
   - Calls **WorkflowRegistry::LoadFile** for each workflow file and registers it.

3. **WorkflowRegistry**
   - Parses the JCWF JSON (`make_example.jcwf`).
   - Registers:
     - Workflow metadata (id, label, etc.).
     - Tasks (nodes in the DAG): `compile_lib1`, `compile_lib2`, `make_static_lib`, `compile_main`, `compile_app`, `make_executable`.
     - Dataflow edges between tasks (the `dataflow` array in the JCWF).

4. **TriggerEngine**
   - After the workflow is registered and validated, a default **auto trigger** is registered:
     - `TriggerEngine::AddAutoTrigger: ... 'auto' for workflow 'make-example'`.
   - Immediately fires that trigger at startup:
     - `TriggerEngine::FireTrigger: firing trigger 'auto' for workflow 'make-example'`.
   - The fired trigger tells the orchestrator to start a run of `make-example`.

5. **WorkflowOrchestrator**
   - Receives: “start workflow `make-example` with run id `make-example_...`”.
   - Resolves the **task graph** (tasks + `depends_on` + `dataflow`):
     - Finds tasks with no unmet dependencies and schedules them.
     - Tracks which tasks have completed.
     - When all dependencies of a task are done, it schedules the next dependent task.
   - For **shell tasks**, it uses the **shell-execution helper** (the code that emits `[shell]` log lines) to:
     - Build the command line.
     - Launch the external process.
     - Wait for exit and record success/failure.

6. **Shell Execution Helper (shell task runner)**
   - It is responsible for these log lines:
     - `[shell] Executing shell task 'compile_lib1'`
     - `[shell] Command: scripts/compile.sh ../workflows/lib1.cpp ../workflows/lib1.o`
   - Performs the following steps:
     1. Resolves **file inputs** and **file outputs** from the JCWF:
        - For example, for `compile_lib1`:
          - `file_inputs`: `["lib1.cpp"]` → resolved as `../workflows/lib1.cpp`
          - `file_outputs`: `["lib1.o"]`   → resolved as `../workflows/lib1.o`
     2. Resolves any **dataflow inputs/outputs** (like `${input[0]}`, `${output[0]}` in `params.args`).
     3. Constructs the **final command line** from the JCWF `params`:
        - `command`: `scripts/compile.sh`
        - `args`: `["${input[0]}", "${output[0]}"]` → becomes `../workflows/lib1.cpp` / `../workflows/lib1.o`.
     4. Spawns a **child process** to run the command.
     5. Collects the exit code and reports success/failure back to the orchestrator.
   - The exact low-level API (e.g., `std::system`, `popen`, `fork/exec`) is an implementation detail; logically, it just **runs an external program** with the constructed arguments.

7. **External Scripts + Toolchain**
   - These are the actual shell scripts that do the work:

   ```bash
   # scripts/compile.sh
   g++ -Wall -Wextra -std=c++20 -c "$SOURCE" -o "$OUTPUT"

   # scripts/archive.sh
   ar rcs "$ARCHIVE" "$OBJ1" "$OBJ2"

   # scripts/link.sh
   g++ -Wall -Wextra "$MAIN_OBJ" "$APP_OBJ" "$ARCHIVE" -o "$OUTPUT"
   ```

   - JCWF + JarvisAgent decides **what to run, and in which order**.  
   - The shell scripts + `g++`/`ar`/linker actually do the compilation and linking.

---

## 3. Task Order and Dependencies

From `make_example.jcwf`, the task graph is:

1. **Leaf tasks (no depends_on):**
   - `compile_lib1`
   - `compile_lib2`
   - `compile_main`
   - `compile_app`

2. **Intermediate task:**
   - `make_static_lib`
     - `depends_on`: `["compile_lib1", "compile_lib2"]`
     - `file_inputs`: `["lib1.o", "lib2.o"]`
     - Produces: `libmylib.a`

3. **Final task:**
   - `make_executable`
     - `depends_on`: `["compile_main", "compile_app", "make_static_lib"]`
     - `file_inputs`: `["main.o", "app.o", "libmylib.a"]`
     - Produces: `myapp`

So the orchestrator runs them in this DAG order:

```text
Step 1: compile_lib1, compile_lib2, compile_main, compile_app  (can run in parallel)
Step 2: make_static_lib                                       (requires lib1.o + lib2.o)
Step 3: make_executable                                       (requires main.o + app.o + libmylib.a)
```

This matches the log sequence you saw:

```text
[shell] Executing shell task 'compile_lib1'
[shell] Executing shell task 'compile_lib2'
[shell] Executing shell task 'compile_main'
[shell] Executing shell task 'compile_app'
...
[shell] Executing shell task 'make_static_lib'
...
[shell] Executing shell task 'make_executable'
```

---

## 4. Detailed Shell Task Example

Let’s look at `compile_lib1` as a concrete example, step by step.

### 4.1. JCWF fragment

```jsonc
"compile_lib1": {
  "id": "compile_lib1",
  "type": "shell",
  "label": "Compile lib1.o",
  "file_inputs": ["lib1.cpp"],
  "file_outputs": ["lib1.o"],
  "inputs": {
    "source": { "type": "string", "required": true }
  },
  "outputs": {
    "object": { "type": "string" }
  },
  "params": {
    "command": "scripts/compile.sh",
    "args": ["${input[0]}", "${output[0]}"]
  }
}
```

### 4.2. How it is executed

1. **WorkflowOrchestrator** picks `compile_lib1` as ready to run (no dependencies).  
2. It looks at `type: "shell"` and calls into the **shell task executor**.
3. The executor:
   - Resolves `file_inputs[0] = "lib1.cpp"` → `../workflows/lib1.cpp`.
   - Resolves `file_outputs[0] = "lib1.o"`  → `../workflows/lib1.o`.
   - Maps `${input[0]}` → the resolved input path.
   - Maps `${output[0]}` → the resolved output path.
4. It constructs the command:

   ```text
   scripts/compile.sh ../workflows/lib1.cpp ../workflows/lib1.o
   ```

5. It logs:

   ```text
   [shell] Executing shell task 'compile_lib1'
   [shell] Command: scripts/compile.sh ../workflows/lib1.cpp ../workflows/lib1.o
   ```

6. It spawns the child process, where `compile.sh` runs:

   ```bash
   #!/usr/bin/env bash
   set -euo pipefail

   SOURCE="$1"
   OUTPUT="$2"

   echo "[compile] $SOURCE -> $OUTPUT"

   g++ -Wall -Wextra -std=c++20 -c "$SOURCE" -o "$OUTPUT"
   ```

7. On success (exit code 0), the orchestrator marks `compile_lib1` as **completed**, records that `lib1.o` exists, and this unblocks `make_static_lib` once `compile_lib2` is also done.

All other shell tasks follow the same pattern, just with different `file_inputs`, `file_outputs`, and scripts/arguments.

---

## 5. What Happens If We Run the Workflow Again?

Short version:

> **Right now, the `make-example` workflow will re-run all its shell tasks every time it is triggered.**  
> There is **no “up-to-date” / incremental detection** for these generic shell tasks yet.

More detail:

- The **up-to-date / “skipping” logic** you see in the logs (e.g. `Skipping ...: output is up-to-date`) belongs to the **requirements / environment processing** subsystem (the part that works on REQ\_*.txt, ICE chunks, etc.).
  - That subsystem tracks which inputs/outputs have already been processed and skips re-processing if nothing changed.
- For **JCWF shell workflows** like `make-example`:
  - When the trigger fires again (auto or manual), the **WorkflowOrchestrator** currently treats this as a **fresh run**.
  - It will schedule:
    - `compile_lib1`, `compile_lib2`, `compile_main`, `compile_app` again,
    - then `make_static_lib`,
    - then `make_executable`.
  - The external scripts (`compile.sh`, `archive.sh`, `link.sh`) do **not** perform any timestamp or content-based checks—they just overwrite the outputs if they already exist.

So if you restart JarvisAgent or otherwise fire the `make-example` workflow again, you should see the same `[shell]` lines and the .o/.a/exe files will simply be rebuilt in place.

If you’d like true “make-style” incremental behavior (skip compile/link if outputs are newer than inputs), that would require **extra logic** either:

- inside JarvisAgent’s JCWF engine (track timestamps or hashes), or  
- inside the shell scripts themselves (check input/output mtimes and early-exit when up-to-date).

As of the current state you ran, that logic is **not** implemented for `make-example`, which is why every trigger re-runs the whole pipeline.

---

## 6. Summary

- **Classes / components involved (high level)**:
  - Engine/Core (config + run loop)
  - JarvisAgent Application (initialization + subsystem wiring)
  - WorkflowRegistry (parse & register JCWF workflows)
  - TriggerEngine (auto + other triggers)
  - WorkflowOrchestrator (task DAG scheduling)
  - Shell task executor helper (constructs commands & spawns child processes)
  - External shell scripts (`compile.sh`, `archive.sh`, `link.sh`) and toolchain (`g++`, `ar`, linker)

- **Order of operations for `make-example`**:
  1. Engine + JarvisAgent start.
  2. Workflows loaded from `../workflows`.
  3. `make-example` registered and validated.
  4. Auto trigger `auto` registered and fired.
  5. WorkflowOrchestrator runs tasks in DAG order:
     - `compile_lib1`, `compile_lib2`, `compile_main`, `compile_app`
     - `make_static_lib`
     - `make_executable`
  6. Shell executor runs each task’s `command + args` as an external process.
  7. On success of all tasks, run is marked as complete.

- **Re-running** the workflow:
  - Currently **rebuilds everything**, with no up-to-date detection for these shell tasks.
