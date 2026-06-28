# FreeCLI Roadmap

## Vision
Evolve FreeCLI into a powerful, keyboard-first local AI agent terminal with a central comm bus, full MCP support, safe system operations, and extensible backends (LLM, MCP, Code Interpreter, Database, etc.).

## Phase 1: Comm Bus Foundation (Current Priority)
- Refactor current TCP IPC to a cleaner bus (Unix Domain Sockets first, then ZeroMQ/nng with ipc://).
- Introduce common BackendWorker interface (generalization).
- Implement Shell worker (restricted code interpreter).
- Basic safety framework (allow-lists, approvals).
- Update documentation (ARCHITECTURE.md, this file).

## Phase 2: Database & Early MCP
- SQLite worker (embedded DB connector with safety).
- Basic MCP client (tool discovery + execution via stdio).
- Integrate MCP tools into LLM calls.
- Enhanced safety & approval UI in TUI.

## Phase 3: Full Agent Capabilities
- Full tool calling across all providers.
- More code interpreters (Python, etc.).
- Additional databases (DuckDB, PostgreSQL, etc.).
- Better sandboxing across platforms (FreeBSD jails, macOS sandbox-exec, Windows AppContainer).
- Persistent MCP connections.

## Phase 4: Polish & Extensibility
- Advanced UI for tool management and approvals.
- Export/import workflows.
- More MCP server examples and integrations.
- Performance tuning on older hardware.
- Packaging / releases for FreeBSD, Linux, macOS.

## Long-term Ideas
- Local code execution sandbox (e.g. Firejail / bubblewrap integration).
- Multi-agent orchestration.
- Plugin system for new backend types.
- Self-hosted model support via Ollama/LocalAI.

## Status
- Architecture documented.
- BackendWorker generalization planned.
- SQLite + Shell as initial specialized backends.

This roadmap will be updated as we progress.
