#ifndef PERSIST_H
#define PERSIST_H

#include "session.h"

/*
 * Persistence layer — saves/loads sessions to/from JSON files.
 *
 * Storage location:
 *   Linux/FreeBSD/macOS : $XDG_DATA_HOME/freecli/   (default ~/.local/share/freecli/)
 *   Windows             : %APPDATA%\FreeCLI\
 *
 * Layout:
 *   <dir>/sessions.json          — index: ordered list of sessions + next_id
 *   <dir>/sessions/<id>.json     — messages for one session
 */

/* Load saved sessions into sm. Returns number loaded (0 on first run).
 * Must be called before sm_new_session so next_id is restored correctly. */
int  persist_load(SessionManager *sm);

/* Save the index file + all session message files. */
void persist_save_all(const SessionManager *sm);

/* Save the index file + one session's message file.
 * Call this after every assistant reply or session rename. */
void persist_save_session(const SessionManager *sm, int idx);

/* Remove a session's message file from disk (call after /clear or delete). */
void persist_delete_session(int session_id);

#endif /* PERSIST_H */
