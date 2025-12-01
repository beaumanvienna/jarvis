# JarvisAgent Engine JSON Utilities

This document describes the JSON-related engine components:

- `ConfigParser`
- `ConfigChecker`
- `JsonHelper`

All code lives under `engine/json`.

---

## 1. ConfigParser

**Header:** `json/configParser.h`  
**Source:** `json/configParser.cpp`  
**Namespace:** `AIAssistant`

`ConfigParser` loads and validates the raw JSON configuration file (e.g. `config.json`) and fills an `EngineConfig` struct.

### 1.1 EngineConfig

```cpp
struct EngineConfig
{
    enum InterfaceType
    {
        API1 = 0,
        API2,
        NumAPIs,
        InvalidAPI
    };

    struct ApiInterface
    {
        std::string m_Url;
        std::string m_Model;
        InterfaceType m_InterfaceType{InterfaceType::InvalidAPI};
    };

    uint m_MaxThreads{0};
    std::chrono::milliseconds m_SleepDuration{0};
    std::string m_QueueFolderFilepath;
    bool m_Verbose{false};
    size_t m_ApiIndex{0};
    std::vector<ApiInterface> m_ApiInterfaces;
    size_t m_MaxFileSizekB{20};
    bool m_ConfigValid{false};

    bool IsValid() const { return m_ConfigValid; }
};
```

**Fields:**

- `m_MaxThreads`  
  Maximum number of worker threads used by the engine thread pool.

- `m_SleepDuration`  
  Sleep interval of the main run loop (`Core::Run`).

- `m_QueueFolderFilepath`  
  Path to the queue directory that JarvisAgent monitors for work items.

- `m_Verbose`  
  Enables verbose logging in the engine.

- `m_ApiIndex`  
  Index into `m_ApiInterfaces` selecting the active API configuration.

- `m_ApiInterfaces`  
  List of configured API endpoints:
  - `m_Url` – Full HTTPS URL of the API endpoint.
  - `m_Model` – Model name (e.g. an LLM identifier).
  - `m_InterfaceType` – One of `API1`, `API2`, or `InvalidAPI`.

- `m_MaxFileSizekB`  
  Maximum allowed file size (kB) for items processed from the queue.

- `m_ConfigValid` / `IsValid()`  
  Set by `ConfigChecker` to indicate whether the configuration is valid.

### 1.2 Parser State

```cpp
enum State
{
    Undefined = 0,
    ConfigOk,
    ParseFailure,
    FileNotFound,
    FileFormatFailure
};
```

Used internally for reporting parser status.

### 1.3 Construction and State

```cpp
ConfigParser(std::string const& filepathAndFilename);
~ConfigParser();

State GetState() const;
bool ConfigParsed() const;
State Parse(EngineConfig& engineConfig);
```

- **Constructor**  
  Stores the config file path and initializes state to `Undefined`.

- **GetState()**  
  Returns last parser state.

- **ConfigParsed()**  
  Convenience: returns `true` if `GetState() == ConfigOk`.

- **Parse(EngineConfig& engineConfig)**  
  Main entry point. Responsibilities:
  1. Reset `engineConfig` to default-initialized state.
  2. Verify the file exists and is not a directory:
     - On failure → logs error, sets `FileNotFound`, returns.
  3. Use `simdjson::ondemand` to parse the JSON.
     - On parse error → logs error, sets `ParseFailure`, returns.
  4. Iterate top-level fields and fill:
     - `"description"` → logs only.
     - `"author"` → logs only.
     - `"queue folder"` → sets `m_QueueFolderFilepath`.
     - `"max threads"` → sets `m_MaxThreads`.
     - `"engine sleep time in run loop in ms"` → sets `m_SleepDuration`.
     - `"max file size in kB"` → sets `m_MaxFileSizekB`.
     - `"verbose"` → sets `m_Verbose`.
     - `"API interfaces"` → handled by `ParseInterfaces`.
     - `"API index"` → sets `m_ApiIndex`.
     - Unknown fields → best-effort stringification and logging.
  5. Maintain `fieldOccurances[]` to track which expected fields appeared.
  6. Final state:
     - If `"queue folder"` and at least one `"url"` were found → `ConfigOk`.
     - Otherwise → `FileFormatFailure`.
  7. Logs a small “format info” summary of field occurrences.

The engine (`engine.cpp`) uses:

```cpp
ConfigParser configParser("./config.json");
ConfigParser::EngineConfig engineConfig{};
configParser.Parse(engineConfig);
if (!configParser.ConfigParsed())
{
    return EXIT_FAILURE;
}
```

### 1.4 ParseInterfaces()

```cpp
void ParseInterfaces(simdjson::ondemand::array jsonArray,
                     EngineConfig& engineConfig,
                     FieldOccurances& fieldOccurances);
```

- Iterates the `"API interfaces"` array.
- For each element:
  - `"url"` → `ApiInterface::m_Url`
  - `"model"` → `ApiInterface::m_Model`
  - `"API"` → maps `"API1"` / `"API2"` to `InterfaceType::API1` / `API2`, otherwise calls `CORE_HARD_STOP`.
- Appends each `ApiInterface` to `engineConfig.m_ApiInterfaces`.

---

## 2. ConfigChecker

**Header:** `json/configChecker.h`  
**Source:** `json/configChecker.cpp`  
**Namespace:** `AIAssistant`

`ConfigChecker` performs semantic validation and applies defaults on a previously parsed `EngineConfig`.

### 2.1 Public API

```cpp
class ConfigChecker
{
public:
    ConfigChecker() = default;
    ~ConfigChecker() = default;

    bool Check(ConfigParser::EngineConfig& engineConfig);
    bool ConfigIsOk() const;

private:
    bool m_ConfigIsOk{false};
};
```

### 2.2 Check()

```cpp
bool Check(ConfigParser::EngineConfig& engineConfig);
```

Responsibilities:

1. **Queue folder validity**
   - Uses `EngineCore::IsDirectory()` to verify that
     `engineConfig.m_QueueFolderFilepath` is an existing directory.
   - Logs a `CORE_ASSERT` if it is not a directory.

2. **API selection validity**
   - Ensures at least one `ApiInterface` exists.
   - Ensures `m_ApiIndex` is in range for `m_ApiInterfaces`.
   - For the selected interface:
     - URL is non-empty and contains `"https://"`.
     - Model string is non-empty.
     - `m_InterfaceType` is not `InvalidAPI`.

3. **Global result**
   - `m_ConfigIsOk` is set to the combined result of the checks above.
   - On failure, logs an error indicating the queue folder issue.

4. **Fixing defaults (only if base checks passed)**
   - `m_MaxThreads`  
     If `<= 0` or `> 256` → logs an app error and sets to `16`.
   - `m_SleepDuration`  
     If `<= 0ms` or `> 256ms` → logs an app error and sets to `10ms`.
   - `m_MaxFileSizekB`  
     If `<= 0` or `> 256` → logs an app error and sets to `20`.

5. **Finalizing EngineConfig**
   - Writes `engineConfig.m_ConfigValid = m_ConfigIsOk`.

The engine then checks:

```cpp
ConfigChecker().Check(engineConfig);
if (!engineConfig.IsValid())
{
    return EXIT_FAILURE;
}
```

### 2.3 ConfigIsOk()

Returns the last result of `Check()`.

---

## 3. JsonHelper

**Header:** `json/jsonHelper.h`  
**Source:** `json/jsonHelper.cpp`  
**Namespace:** `AIAssistant`

`JsonHelper` provides small JSON utility helpers.  
Currently only one function is implemented.

### 3.1 SanitizeForJson()

```cpp
class JsonHelper
{
public:
    std::string SanitizeForJson(std::string const& input);
};
```

**Behavior:**

- Produces a copy of the input where:
  - Double quotes (`"`) → `\"`
  - Backslashes (`\`) → `\\`
  - Newline (`\n`) → `\n`
  - Carriage return (`\r`) → `\r`
  - Tab (`\t`) → `\t`
  - Single quotes and other characters are left as-is, except for the specific handling in the `switch` statement.
- Pre-allocates output capacity to `input.size()` for efficiency.

This is used wherever text needs to be safely embedded inside JSON string values (e.g. when constructing request payloads for APIs).

---

## 4. Summary

- **ConfigParser**: parses `config.json` using `simdjson`, fills `EngineConfig`, and tracks basic field presence.
- **ConfigChecker**: verifies directories, API configuration, and sensible ranges; sets defaults where needed and marks the config as valid.
- **JsonHelper**: sanitizes arbitrary strings so they are safe to embed in JSON.

Together these components provide JarvisAgent’s engine-level JSON parsing, validation, and string-sanitization infrastructure.
