# FreeCLI

**A keyboard-first, LLM-agnostic terminal chat UI written in C.**

FreeCLI is a fast, portable ncurses application for conversing with multiple LLM providers simultaneously. It runs on Linux, macOS, FreeBSD — anywhere a C toolchain and ncurses are available.

---

## Features

### Multi-provider support
Talk to any of the supported LLM backends from a single interface. Switch providers and models on the fly with `/provider` and `/model`.

| Provider | Models |
|---|---|
| **xAI (Grok)** | grok-3-fast, grok-3, grok-3-mini-fast, grok-3-mini |
| **Anthropic (Claude)** | claude-opus-4-5, claude-sonnet-4-5, claude-haiku-4-5, claude-3.5 family |
| **Google (Gemini)** | gemini-2.5-flash, gemini-2.5-pro, gemini-2.0-flash, gemini-1.5 family |
| **IBM (watsonx / Granite / Llama)** | granite-3-8b-instruct, llama-3.3-70b, and more |

### Three-pane TUI
```
┌─────────────────┬──────────────────────────────┬─────────────────────┐
│  Chat Sessions  │         Chat History          │  Background Workers │
│                 │                               │                     │
│  #1 API Design  │  You: How do I parse JSON in  │ ⣷ grok-3-fast       │
│  #2 Rust help   │       C cleanly?              │   → claude-sonnet   │
│  #3 ...         │  AI:  Use cJSON — it's a ...  │                     │
│                 │                               ├─────────────────────┤
│                 │                               │  [Backend output]   │
│                 │                               │                     │
└─────────────────┴──────────────────────────────┴─────────────────────┘
 xai/grok-3-fast | #1 API Design
```

- **Left pane** — session list with auto-generated topic names (e.g. `#1 API Design`) and marquee scroll for long names.
- **Centre pane** — scrollable chat history with word-wrapped messages.
- **Right pane (top)** — live progress meters for background workers, including indented sub-workers from LLM-to-LLM calls.
- **Right pane (bottom)** — raw backend output / debug log.

### LLM-to-LLM inter-communication
Supported models can autonomously call other LLMs via the built-in `ask_ai` tool. A Grok model can, for example, consult a Claude model mid-conversation. Sub-queries appear as indented workers in the UI with their own progress indicators.

### Persistent chat sessions
Sessions are saved to `~/.freecli/sessions.json`. Each session gets an automatically generated topic name after the first exchange. Sessions persist across restarts and can be renamed with `/rename`.

### Slash commands
| Command | Action |
|---|---|
| `/new` | Start a new chat session |
| `/provider` | Pop-up overlay to switch LLM provider |
| `/model` | Pop-up overlay to choose a model within the current provider |
| `/rename <name>` | Rename the current session |
| `/clear` | Clear current session history |
| `/help` | Show all available commands |

---

## Architecture

FreeCLI uses a **two-process model** with a TCP IPC channel:

```
freecli (TUI process)   ←──── TCP socket (localhost) ────→   freecli-backend (worker process)
     ncurses UI                                                   libcurl + LLM APIs
     session management                                           agentic loop
     input handling                                               provider dispatch
```

The TUI is deliberately free of libcurl — it sends requests over IPC and receives streamed back replies. The backend manages a lazy-initialized provider registry and supports concurrent requests. Spawned sub-workers (LLM-to-LLM calls) send `WORKER_START`/`WORKER_END` IPC events that the TUI renders in real time.

### Source layout

```
src/
├── ui/
│   ├── main.c          # TUI, window management, event loop, IPC polling
│   ├── commands.c/h    # Slash command dispatch + overlay UI
│   ├── session.c/h     # In-memory session model
│   ├── persist.c/h     # sessions.json read/write
│   ├── input.c/h       # Key event handling
│   └── overlay.c/h     # Generic overlay widget
├── backend/
│   └── backend.c       # Agentic request loop, provider dispatch, sub-queries, auto-naming
├── core/
│   ├── chat.c/h        # Message + ToolCall model
│   ├── ipc.c/h         # Wire protocol encode/decode
│   ├── provider.c/h    # Provider interface (ProviderRequest/Reply)
│   ├── provider_registry.h  # Shared provider metadata (names + model lists)
│   └── cJSON.c/h       # Embedded JSON parser (Dave Gamble, MIT)
└── providers/
    ├── registry.c                        # ProviderInfo[] array (both binaries)
    ├── xai/provider_xai.c                # xAI / Grok (+ ask_ai tool support)
    ├── anthropic/provider_anthropic.c    # Anthropic Claude
    ├── google/provider_google.c          # Google Gemini
    └── ibm/provider_ibm.c               # IBM watsonx (IAM token exchange)
```

---

## Building

### Dependencies

| Dependency | Purpose |
|---|---|
| `gcc` or `clang` | C compiler |
| `ncurses` (+ menu, form, panel) | TUI rendering |
| `libcurl` | HTTPS to LLM APIs (backend only) |
| `autoconf` / `automake` | Build system |

**Debian / Ubuntu:**
```bash
sudo apt install build-essential libncurses-dev libcurl4-openssl-dev autoconf automake
```

**macOS (Homebrew):**
```bash
brew install ncurses curl autoconf automake
```

**FreeBSD:**
```bash
pkg install ncurses curl autoconf automake
```

### Compile

```bash
./bootstrap        # generate configure script (first time only)
./configure
make -j4
```

Produces two binaries in `src/`:
- `freecli` — the TUI
- `freecli-backend` — the backend worker (auto-spawned by `freecli`)

### Install

```bash
sudo make install
```

Both binaries install to `$PREFIX/bin` (default `/usr/local/bin`). `freecli` finds `freecli-backend` relative to its own path at runtime.

---

## Configuration / API keys

FreeCLI reads API keys from environment variables or dotfiles:

| Provider | Environment variable | Fallback file |
|---|---|---|
| xAI | `XAI_API_KEY` | `~/.grok/user-settings.json` (key `"apiKey"`) |
| Anthropic | `ANTHROPIC_API_KEY` | `~/.anthropic/api_key` |
| Google | `GEMINI_API_KEY` or `GOOGLE_API_KEY` | `~/.google/gemini_api_key` |
| IBM watsonx | `WATSONX_API_KEY` + `WATSONX_PROJECT_ID` | `~/.ibm/watsonx.json` |

IBM also reads `WATSONX_REGION` (default: `us-south`). The `watsonx.json` format:
```json
{ "api_key": "...", "project_id": "...", "region": "us-south" }
```

---

## Keyboard shortcuts

| Key | Action |
|---|---|
| `Tab` | Switch focus between panes |
| `↑` / `↓` | Scroll history or navigate session list |
| `Enter` | Send message |
| `Ctrl-C` | Quit |

---

## License

FreeCLI is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License version 3** (or any later version) as published by the Free Software Foundation.

See [LICENSE](LICENSE) for the full text.

---

## Contributing

Pull requests welcome. The IPC wire format and provider interface are stable — adding a new provider means creating one `.c` file in `src/providers/<name>/` and registering it in `src/providers/registry.c` and `src/backend/backend.c`.
