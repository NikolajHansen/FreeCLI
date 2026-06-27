#ifndef IPC_H
#define IPC_H

#include "net.h"
#include <stdint.h>

/*
 * IPC message framing between freecli (TUI) and freecli-backend.
 *
 * Wire format (all integers in host byte order — loopback only):
 *   [u32 frame_body_len][u32 type][u64 corr_id][payload...]
 *
 * frame_body_len = sizeof(type) + sizeof(corr_id) + sizeof(payload)
 *
 * Message flow:
 *   TUI  → backend : MSG_REQ_SEND        (chat history for one session)
 *   TUI  → backend : MSG_REQ_CANCEL      (cancel an in-flight corr_id)
 *   backend → TUI  : MSG_REP_FINAL       (complete reply + optional reasoning)
 *   backend → TUI  : MSG_REP_ERROR       (error string)
 *   backend → TUI  : MSG_WORKER_START    (sub-worker began; corr_id=sub, payload has parent+desc)
 *   backend → TUI  : MSG_WORKER_END      (sub-worker done; corr_id=sub, no payload)
 */

typedef enum {
    MSG_REQ_SEND    = 1,
    MSG_REQ_CANCEL  = 2,
    MSG_REP_FINAL   = 10,
    MSG_REP_ERROR   = 11,
    MSG_WORKER_START = 20,
    MSG_WORKER_END   = 21,
    MSG_SESSION_RENAME = 22,  /* backend→TUI: suggested session topic */
} MsgType;

typedef struct {
    uint32_t type;
    uint64_t corr_id;
    uint8_t *payload;     /* heap-allocated; caller must free */
    uint32_t payload_len;
} IpcMsg;

/* Send one message. payload may be NULL when payload_len == 0. */
int ipc_send(net_fd_t fd, uint32_t type, uint64_t corr_id,
             const void *payload, uint32_t payload_len);

/*
 * Poll for one message (non-blocking).
 * Returns  1 : message read into *msg (caller must free msg->payload)
 * Returns  0 : no data ready
 * Returns -1 : error / peer disconnected
 */
int ipc_recv(net_fd_t fd, IpcMsg *msg);

/* --- Payload helpers --- */

/* Encode MSG_REQ_SEND payload. model and provider may be NULL.
 * Returns heap buffer, sets *out_len. */
uint8_t *ipc_encode_req_send(uint32_t session_idx,
                              uint32_t msg_count,
                              const char * const *texts,
                              const uint8_t     *is_user,
                              const char        *model,
                              const char        *provider,
                              uint32_t          *out_len);

/* Decode MSG_REQ_SEND payload. Returns 0 on success.
 * Caller frees (*texts)[i], *texts, *is_user, *model, *provider (if non-NULL). */
int ipc_decode_req_send(const uint8_t *buf, uint32_t buf_len,
                         uint32_t *session_idx,
                         uint32_t *msg_count,
                         char    ***texts,
                         uint8_t  **is_user,
                         char     **model,
                         char     **provider);

/* Encode MSG_REP_FINAL payload. reasoning may be NULL. */
uint8_t *ipc_encode_rep_final(uint32_t    session_idx,
                               const char *content,
                               const char *reasoning,
                               uint32_t   *out_len);

/* Decode MSG_REP_FINAL payload. Returns 0 on success.
 * Caller frees *content and *reasoning (if non-NULL). */
int ipc_decode_rep_final(const uint8_t *buf, uint32_t buf_len,
                          uint32_t *session_idx,
                          char    **content,
                          char    **reasoning);

/* --- MSG_WORKER_START ---
 * Payload: [u64 parent_corr_id][u32 model_len][model][u32 desc_len][desc]
 * corr_id in IPC header = sub-worker's corr_id. */

/* Encode MSG_WORKER_START payload. */
uint8_t *ipc_encode_worker_start(uint64_t    parent_corr_id,
                                  const char *model,
                                  const char *description,
                                  uint32_t   *out_len);

/* Decode MSG_WORKER_START payload. Returns 0 on success.
 * Caller frees *model (if non-NULL) and *description. */
int ipc_decode_worker_start(const uint8_t *buf, uint32_t buf_len,
                             uint64_t    *parent_corr_id,
                             char       **model,
                             char       **description);

/* MSG_WORKER_END has no payload; corr_id in the IPC header identifies the worker. */

/* --- MSG_SESSION_RENAME ---
 * Payload: [u32 session_idx][u32 topic_len][topic]
 * topic is a short 2-4 word string. TUI formats as "#<id> <topic>".
 * Only applied if the session still has its default "Chat N" name. */

uint8_t *ipc_encode_session_rename(uint32_t session_idx,
                                    const char *topic,
                                    uint32_t   *out_len);

int ipc_decode_session_rename(const uint8_t *buf, uint32_t buf_len,
                               uint32_t *session_idx,
                               char    **topic);

#endif /* IPC_H */
