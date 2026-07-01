#ifndef FS_WORKER_H
#define FS_WORKER_H

/*
 * fs_worker.h — FileSystem BackendWorker for FreeCLI.
 *
 * All operations are scoped to a configurable sandbox root directory.
 * Read operations execute immediately; mutating operations require explicit
 * user approval via the bus approval flow before any filesystem change.
 *
 * Supported operations (passed as BusMsg.op):
 *
 *   "read_file"    args: {"path":"rel/path"}
 *                  result: {"content":"..."}
 *
 *   "list_dir"     args: {"path":"rel/path"}  (empty = sandbox root)
 *                  result: {"entries":[{"name":"...","type":"file"|"dir","size":N},...]}
 *
 *   "create_file"  args: {"path":"rel/path","content":"..."}  [approval required]
 *                  result: {"status":"ok","path":"rel/path"}
 *
 *   "modify_file"  args: {"path":"rel/path","content":"..."}  [approval required]
 *                  result: {"status":"ok","path":"rel/path"}
 *
 *   "delete_file"  args: {"path":"rel/path"}                  [approval required]
 *                  result: {"status":"ok","path":"rel/path"}
 */

#include "backend_worker.h"

/*
 * fs_worker_create — allocate and return a new FileSystem worker.
 *
 * root_dir: absolute path to the sandbox directory (created if absent).
 *           Pass NULL to use $FREECLI_SANDBOX, or ~/freecli-sandbox as default.
 *
 * The caller must call worker->init(worker) after creation and, on success,
 * pass the worker to bw_registry_add().
 */
BackendWorker *fs_worker_create(const char *root_dir);

#endif /* FS_WORKER_H */
