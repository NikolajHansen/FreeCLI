#ifndef CHAT_H
#define CHAT_H

#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * ToolCall — a single function call emitted by an assistant message.
 * arguments is a JSON object string (opaque to the chat layer).
 */
typedef struct {
    char *id;           /* tool_call_id from provider */
    char *name;         /* function name, e.g. "ask_ai" */
    char *arguments;    /* JSON-encoded arguments string */
} ToolCall;

typedef struct {
    char     *text;
    bool      is_user;
    char     *reasoning;    /* xAI reasoning_content; NULL for user msgs */
    /* Tool-calling extensions (NULL / 0 unless part of an agentic loop) */
    ToolCall *tool_calls;   /* non-NULL => assistant requested these tool calls */
    int       n_tool_calls;
    char     *tool_call_id; /* non-NULL => this is a tool_result (role=tool) message */
} Message;

typedef struct {
    Message *msgs;
    int count;
    int capacity;
    int scroll;
} ChatBuffer;

void chat_init(ChatBuffer *cb);
void chat_add(ChatBuffer *cb, const char *text, bool is_user);
void chat_add_r(ChatBuffer *cb, const char *text, bool is_user, const char *reasoning);

/* Add an assistant message that carried tool_calls (takes ownership of tc array). */
void chat_add_tool_calls(ChatBuffer *cb, const char *content,
                         const char *reasoning,
                         ToolCall *tc, int n_tc);

/* Add a tool result message (role=tool). */
void chat_add_tool_result(ChatBuffer *cb, const char *tool_call_id,
                          const char *result);

void chat_render_decisions(ChatBuffer *cb, WINDOW *win);
void chat_scroll(ChatBuffer *cb, int delta);
void chat_render(ChatBuffer *cb, WINDOW *win);
void chat_remove_last(ChatBuffer *cb);
void chat_free(ChatBuffer *cb);

/* Free a ToolCall array (does NOT free the array pointer itself). */
void toolcall_array_free(ToolCall *tc, int n);

#endif
