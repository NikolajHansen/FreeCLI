#ifndef SESSION_H
#define SESSION_H

#include "chat.h"

#define MAX_SESSIONS 32

typedef struct {
    int        id;       /* persistent ID, assigned at creation */
    char       name[64];
    ChatBuffer chat;
} Session;

typedef struct {
    Session sessions[MAX_SESSIONS];
    int count;
    int active;
    int next_id;  /* counter for assigning new session IDs */
} SessionManager;

void     sm_init(SessionManager *sm);
int      sm_new_session(SessionManager *sm, const char *name);
int      sm_remove(SessionManager *sm, int index);   /* returns new active index */
ChatBuffer *sm_active_chat(SessionManager *sm);
Session *sm_active_session(SessionManager *sm);
void     sm_select(SessionManager *sm, int index);
int      sm_count(SessionManager *sm);
Session *sm_get(SessionManager *sm, int index);

#endif
