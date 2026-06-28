# FreeCLI Architecture & Future Vision

## Current Architecture (as of late June 2026)

FreeCLI uses a clean two-process model:
- TUI process (freecli): ncurses-based keyboard-first interface.
- Backend process (freecli-backend): LLM API calls, agentic loops, providers.

Communication: custom binary IPC over localhost TCP.

Strengths: multi-provider, persistent sessions, live worker visualization.

## Target Vision: Comm Bus + Extensible Backend Workers

Introduce a central Comm Bus that orchestrates the TUI, LLM providers, MCP servers, and additional specialized backends.

Key principle: Generalization + Specialization
- Common BackendWorker interface for lifecycle, message ingestion, progress reporting, and queuing.
- Specialization per backend type for implementation details.

### Core Abstraction: BackendWorker

typedef enum {
    BACKEND_TYPE_LLM,
    BACKEND_TYPE_MCP,
    BACKEND_TYPE_CODE_INTERPRETER,  // Shell, Python, etc.
    BACKEND_TYPE_DATABASE,          // SQLite, DuckDB, etc.
    BACKEND_TYPE_FILESYSTEM,        // Native file ops
    BACKEND_TYPE_OTHER,
} BackendType;

typedef struct BackendWorker {
    BackendType type;
    char       *name;
    uint64_t    corr_id;

    int  (*init)(struct BackendWorker *self);
    int  (*start)(struct BackendWorker *self, const char *input, void *context);
    int  (*cancel)(struct BackendWorker *self);
    void (*report_progress)(struct BackendWorker *self, const char *msg);

    void *private_data;   // specialization
    void *bus;            // shared comm bus reference
} BackendWorker;

Shared bus logic handles:
- Message queuing/ingestion
- Worker registration & tracking
- Progress to TUI (right pane)
- Cancellation, logging, safety gates

### Specialized Backends (Starting Point)

1. Code Interpreter (starting with Shell)
   - Restricted shell execution.
   - Safety: allow-lists, working directory sandbox, user approval for writes/exec.
   - Future: Python, Java, etc.

2. Database Connector (starting with SQLite)
   - Embedded single-file database (libsqlite3).
   - Safety: read-only default, parameterized queries, file/database allow-lists.
   - Future: DuckDB, LMDB, PostgreSQL (via libpq), etc.

3. FileSystem Worker
   - Native file create/modify/read operations.
   - Safety: scoped directories, explicit approval for writes, diff preview where possible.

4. MCP Client — Connects to external MCP servers for tool/context (note: many MCP servers require Node.js/npm).

5. LLM Providers — Existing ones, extended with full tool calling.

### Safety & Restrictions (Core Principle)

Unlike more autonomous tools (Claude Code, Copilot CLI), FreeCLI defaults to conservative:
- Explicit user approval for all mutating operations.
- Read-only / safe mode by default.
- Allow-lists and sandboxing (jails on FreeBSD, sandbox-exec on macOS, AppContainer on Windows).
- Full audit logging.

### Cross-Platform Notes

- FreeBSD (primary): Strong sandboxing with jails/capsicum.
- macOS: sandbox-exec / Seatbelt.
- Windows: AppContainer or restricted PowerShell.
- SQLite works natively everywhere (no Java required).
- Abstract process spawning and paths for portability.

### Comm Bus Role

- Central message routing using ZeroMQ (or nng) with ipc:// transport.
- Aggregates tools from MCP + internal backends.
- Feeds tools to LLMs.
- Manages worker queue and notifications.

### Phased Implementation

Phase 1: Bus foundation + common BackendWorker + Shell worker (restricted).  
Phase 2: SQLite worker + FileSystem worker + basic safety controls.  
Phase 3: MCP integration.  
Phase 4: More languages/DBs + advanced sandboxing.

This design gives powerful sandboxed AI capabilities while maintaining control.
