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
    MSG_REQ_SEND     = 1,
    MSG_REQ_CANCEL   = 2,
    MSG_REP_FINAL    = 10,
    MSG_REP_ERROR    = 11,
    MSG_WORKER_START = 20,
    MSG_WORKER_END   = 21,
    MSG_SESSION_RENAME = 22,
    /* --- Backend worker operations --- */
    MSG_WORKER_REQ   = 30,  /* TUI → backend : invoke a worker operation  */
    MSG_WORKER_REP   = 31,  /* backend → TUI : worker operation result     */
    MSG_APPROVAL_REQ = 40,  /* backend → TUI : request explicit approval   */
    MSG_APPROVAL_REP = 41,  /* TUI → backend : user's approval decision    */
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
 * Payload: [u32 session_idx][u32 topic_len][topic] */

uint8_t *ipc_encode_session_rename(uint32_t session_idx,
                                    const char *topic,
                                    uint32_t   *out_len);

int ipc_decode_session_rename(const uint8_t *buf, uint32_t buf_len,
                               uint32_t *session_idx,
                               char    **topic);

/* --- MSG_WORKER_REQ ---
 * Payload: [u32 worker_type][u32 op_len][op][u32 args_len][args_json]
 * corr_id in IPC header identifies this request throughout its lifetime. */

uint8_t *ipc_encode_worker_req(uint32_t    worker_type,
                                const char *op,
                                const char *args_json,   /* may be NULL */
                                uint32_t   *out_len);

int ipc_decode_worker_req(const uint8_t *buf, uint32_t buf_len,
                           uint32_t *worker_type,
                           char    **op,
                           char    **args_json);  /* caller frees both */

/* --- MSG_WORKER_REP ---
 * Payload: [u8 success][u32 result_len][result_json_or_error_string] */

uint8_t *ipc_encode_worker_rep(int         success,
                                const char *result,
                                uint32_t   *out_len);

int ipc_decode_worker_rep(const uint8_t *buf, uint32_t buf_len,
                           int  *success,
                           char **result);  /* caller frees */

/* --- MSG_APPROVAL_REQ ---
 * Payload: [u64 req_corr_id][u32 desc_len][description]
 * req_corr_id echoes the corr_id of the blocked worker request so the TUI
 * can match it when the user responds. */

uint8_t *ipc_encode_approval_req(uint64_t    req_corr_id,
                                  const char *description,
                                  uint32_t   *out_len);

int ipc_decode_approval_req(const uint8_t *buf, uint32_t buf_len,
                             uint64_t *req_corr_id,
                             char    **description);  /* caller frees */

/* --- MSG_APPROVAL_REP ---
 * Payload: [u64 req_corr_id][u8 approved]  (approved: 1=yes, 0=no) */

uint8_t *ipc_encode_approval_rep(uint64_t req_corr_id, int approved,
                                  uint32_t *out_len);

int ipc_decode_approval_rep(const uint8_t *buf, uint32_t buf_len,
                             uint64_t *req_corr_id, int *approved);

#endif /* IPC_H */
