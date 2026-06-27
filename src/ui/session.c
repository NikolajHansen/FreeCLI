#include "session.h"
#include <string.h>
#include <stdio.h>

void sm_init(SessionManager *sm) {
    memset(sm, 0, sizeof(*sm));
    sm->active  = -1;
    sm->next_id =  1;
}

int sm_new_session(SessionManager *sm, const char *name) {
    if (sm->count >= MAX_SESSIONS) return -1;
    int idx = sm->count;
    sm->sessions[idx].id = sm->next_id++;
    strncpy(sm->sessions[idx].name, name, 63);
    sm->sessions[idx].name[63] = '\0';
    chat_init(&sm->sessions[idx].chat);
    sm->count++;
    if (sm->active == -1) sm->active = idx;
    return idx;
}

ChatBuffer *sm_active_chat(SessionManager *sm) {
    if (sm->active < 0 || sm->active >= sm->count) return NULL;
    return &sm->sessions[sm->active].chat;
}

Session *sm_active_session(SessionManager *sm) {
    if (sm->active < 0 || sm->active >= sm->count) return NULL;
    return &sm->sessions[sm->active];
}

void sm_select(SessionManager *sm, int index) {
    if (index >= 0 && index < sm->count) sm->active = index;
}

int sm_count(SessionManager *sm) { return sm->count; }

Session *sm_get(SessionManager *sm, int index) {
    if (index < 0 || index >= sm->count) return NULL;
    return &sm->sessions[index];
}
