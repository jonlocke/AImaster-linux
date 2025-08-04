# Changelog

## [v1.1.0] - 2025-08-04

### ✨ New Features
- **Interactive mode (`INT`)**  
  - Added new `INT` command for conversational mode with the LLM.  
  - When `INT` is entered at the main prompt, the prompt changes to `->`.  
  - Each message is sent immediately and streamed back.  
  - Conversation continues until `/bye` is entered.  
  - Chat history is maintained between turns.

### 🛠 Improvements
- **Code block saving**  
  - Updated `saveCodeBlocks()` so that the first line of any code block saved from LLM output is prefixed with `# `.  
  - Makes saved files clearly marked for review.  
- **Status prompt change**  
  - Replaced `[Waiting for response from Ollama...]` with `[Thinking..]` across all modes (`ASK`, `INT`, `READ`) for a cleaner UX.  
- **HELP command**  
  - Added `INT` command to help output with description.  
- **Persistent command history**  
  - Command history now persists between runs in `~/.ollama_cli_history`.  
- **Tab completion**  
  - Added filename tab completion for `READ` command arguments.  
- **Interactive file picker**  
  - When `READ` is entered without arguments, a menu lets the user choose from available files.

### 🔨 Breaking Changes
- Removed legacy `config.h` and replaced with `config_loader.h`.  
- All components now include `config_loader.h` for `AppConfig` definition.  
- `serial_handler.h` updated to take `std::string` port and `int` baudrate instead of `AppConfig` struct for `initSerial()`.  
- Makefile updated to use `config_loader.cpp` instead of `config.cpp`.  
- Added `src/utils.cpp` and `include/utils.h` for file picker logic.  
- Removed `src/config.cpp` and `include/config.h`.

### 📂 Files Changed
- `include/config_loader.h` (new) — Defines `AppConfig`, `loadConfig()`, `loadCommands()`.
- `src/config_loader.cpp` (new) — Loads `config.txt` and `cmds.txt`.
- `include/serial_handler.h` — Removed `config.h` include, updated `initSerial()` signature.
- `src/serial_handler.cpp` — Updated for new `initSerial()` signature.
- `src/main.cpp` — Switched to `config_loader.h`, added readline history, tab completion.
- `include/utils.h` (new) — File picker function declaration.
- `src/utils.cpp` (new) — File picker implementation.
- `src/ollama_client.cpp` — Added `INT` mode, updated code block saving, changed status messages.
- `Makefile` — Replaced `config.o` with `config_loader.o`, added `utils.o`.
- Removed: `include/config.h`, `src/config.cpp`.
