#include "ipc.h"
#include <string.h>
#include <stdlib.h>

int ipc_send(net_fd_t fd, uint32_t type, uint64_t corr_id,
             const void *payload, uint32_t payload_len) {
    uint32_t body_len = sizeof(uint32_t) + sizeof(uint64_t) + payload_len;
    uint8_t *frame    = malloc(sizeof(uint32_t) + body_len);
    if (!frame) return -1;

    uint8_t *p = frame;
    memcpy(p, &body_len,    sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(p, &type,        sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(p, &corr_id,     sizeof(uint64_t)); p += sizeof(uint64_t);
    if (payload_len && payload) memcpy(p, payload, payload_len);

    int r = net_send_all(fd, frame, sizeof(uint32_t) + body_len);
    free(frame);
    return r;
}

int ipc_recv(net_fd_t fd, IpcMsg *msg) {
    if (!net_poll_read(fd, 0)) return 0;

    uint32_t body_len;
    if (net_recv_all(fd, &body_len, sizeof(body_len)) != 0) return -1;
    if (body_len < sizeof(uint32_t) + sizeof(uint64_t))     return -1;

    uint8_t *buf = malloc(body_len);
    if (!buf) return -1;
    if (net_recv_all(fd, buf, body_len) != 0) { free(buf); return -1; }

    uint8_t *p = buf;
    memcpy(&msg->type,    p, sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(&msg->corr_id, p, sizeof(uint64_t)); p += sizeof(uint64_t);

    uint32_t plen = body_len - sizeof(uint32_t) - sizeof(uint64_t);
    if (plen > 0) {
        msg->payload = malloc(plen);
        if (!msg->payload) { free(buf); return -1; }
        memcpy(msg->payload, p, plen);
    } else {
        msg->payload = NULL;
    }
    msg->payload_len = plen;

    free(buf);
    return 1;
}

/* --- MSG_REQ_SEND --- */

uint8_t *ipc_encode_req_send(uint32_t session_idx,
                              uint32_t msg_count,
                              const char * const *texts,
                              const uint8_t     *is_user,
                              const char        *model,
                              const char        *provider,
                              uint32_t          *out_len) {
    uint32_t mlen = model    ? (uint32_t)strlen(model)    : 0;
    uint32_t plen = provider ? (uint32_t)strlen(provider) : 0;
    uint32_t sz = sizeof(uint32_t) * 2;
    for (uint32_t i = 0; i < msg_count; i++)
        sz += 1 + sizeof(uint32_t) + (uint32_t)strlen(texts[i]);
    sz += sizeof(uint32_t) + mlen;
    sz += sizeof(uint32_t) + plen;

    uint8_t *buf = malloc(sz);
    if (!buf) return NULL;
    uint8_t *p = buf;

    memcpy(p, &session_idx, sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(p, &msg_count,   sizeof(uint32_t)); p += sizeof(uint32_t);
    for (uint32_t i = 0; i < msg_count; i++) {
        *p++ = is_user[i];
        uint32_t tlen = (uint32_t)strlen(texts[i]);
        memcpy(p, &tlen, sizeof(uint32_t)); p += sizeof(uint32_t);
        memcpy(p, texts[i], tlen);          p += tlen;
    }
    memcpy(p, &mlen, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (mlen) { memcpy(p, model, mlen); p += mlen; }
    memcpy(p, &plen, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (plen) { memcpy(p, provider, plen); p += plen; }

    *out_len = sz;
    return buf;
}

int ipc_decode_req_send(const uint8_t *buf, uint32_t buf_len,
                         uint32_t *session_idx,
                         uint32_t *msg_count,
                         char    ***texts,
                         uint8_t  **is_user,
                         char     **model,
                         char     **provider) {
    if (buf_len < 8) return -1;
    const uint8_t *p   = buf;
    const uint8_t *end = buf + buf_len;

    memcpy(session_idx, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(msg_count,   p, sizeof(uint32_t)); p += sizeof(uint32_t);

    *texts    = calloc(*msg_count, sizeof(char *));
    *is_user  = calloc(*msg_count, 1);
    *model    = NULL;
    *provider = NULL;
    if (!*texts || !*is_user) return -1;

    for (uint32_t i = 0; i < *msg_count; i++) {
        if (p + 5 > end) return -1;
        (*is_user)[i] = *p++;
        uint32_t tlen;
        memcpy(&tlen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
        if (p + tlen > end) return -1;
        (*texts)[i] = malloc(tlen + 1);
        if (!(*texts)[i]) return -1;
        memcpy((*texts)[i], p, tlen);
        (*texts)[i][tlen] = '\0';
        p += tlen;
    }

    /* Optional model tail */
    if (p + sizeof(uint32_t) <= end) {
        uint32_t mlen;
        memcpy(&mlen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
        if (mlen > 0 && p + mlen <= end) {
            *model = malloc(mlen + 1);
            if (*model) { memcpy(*model, p, mlen); (*model)[mlen] = '\0'; }
            p += mlen;
        }
    }

    /* Optional provider tail */
    if (p + sizeof(uint32_t) <= end) {
        uint32_t plen;
        memcpy(&plen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
        if (plen > 0 && p + plen <= end) {
            *provider = malloc(plen + 1);
            if (*provider) { memcpy(*provider, p, plen); (*provider)[plen] = '\0'; }
        }
    }
    return 0;
}

/* --- MSG_REP_FINAL --- */

uint8_t *ipc_encode_rep_final(uint32_t    session_idx,
                               const char *content,
                               const char *reasoning,
                               uint32_t   *out_len) {
    uint32_t clen = content   ? (uint32_t)strlen(content)   : 0;
    uint32_t rlen = reasoning ? (uint32_t)strlen(reasoning) : 0;
    uint32_t sz   = sizeof(uint32_t) * 3 + clen + rlen;

    uint8_t *buf = malloc(sz);
    if (!buf) return NULL;
    uint8_t *p = buf;

    memcpy(p, &session_idx, sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(p, &clen,        sizeof(uint32_t)); p += sizeof(uint32_t);
    if (clen) { memcpy(p, content, clen);     p += clen; }
    memcpy(p, &rlen,        sizeof(uint32_t)); p += sizeof(uint32_t);
    if (rlen) { memcpy(p, reasoning, rlen);   p += rlen; }

    *out_len = sz;
    return buf;
}

int ipc_decode_rep_final(const uint8_t *buf, uint32_t buf_len,
                          uint32_t *session_idx,
                          char    **content,
                          char    **reasoning) {
    if (buf_len < 12) return -1;
    const uint8_t *p   = buf;
    const uint8_t *end = buf + buf_len;

    memcpy(session_idx, p, sizeof(uint32_t)); p += sizeof(uint32_t);

    uint32_t clen;
    memcpy(&clen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (p + clen > end) return -1;
    *content = malloc(clen + 1);
    if (!*content) return -1;
    memcpy(*content, p, clen); (*content)[clen] = '\0'; p += clen;

    uint32_t rlen;
    if (p + sizeof(uint32_t) > end) { *reasoning = NULL; return 0; }
    memcpy(&rlen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (rlen > 0 && p + rlen <= end) {
        *reasoning = malloc(rlen + 1);
        if (*reasoning) {
            memcpy(*reasoning, p, rlen);
            (*reasoning)[rlen] = '\0';
        }
    } else {
        *reasoning = NULL;
    }
    return 0;
}

/* --- MSG_WORKER_START --- */

uint8_t *ipc_encode_worker_start(uint64_t    parent_corr_id,
                                  const char *model,
                                  const char *description,
                                  uint32_t   *out_len) {
    uint32_t mlen = model       ? (uint32_t)strlen(model)       : 0;
    uint32_t dlen = description ? (uint32_t)strlen(description) : 0;
    uint32_t sz   = sizeof(uint64_t) + sizeof(uint32_t) + mlen
                  + sizeof(uint32_t) + dlen;

    uint8_t *buf = malloc(sz);
    if (!buf) return NULL;
    uint8_t *p = buf;

    memcpy(p, &parent_corr_id, sizeof(uint64_t)); p += sizeof(uint64_t);
    memcpy(p, &mlen,           sizeof(uint32_t)); p += sizeof(uint32_t);
    if (mlen) { memcpy(p, model, mlen); p += mlen; }
    memcpy(p, &dlen,           sizeof(uint32_t)); p += sizeof(uint32_t);
    if (dlen) memcpy(p, description, dlen);

    *out_len = sz;
    return buf;
}

int ipc_decode_worker_start(const uint8_t *buf, uint32_t buf_len,
                             uint64_t    *parent_corr_id,
                             char       **model,
                             char       **description) {
    const uint8_t *p   = buf;
    const uint8_t *end = buf + buf_len;
    *model = *description = NULL;

    if (p + sizeof(uint64_t) > end) return -1;
    memcpy(parent_corr_id, p, sizeof(uint64_t)); p += sizeof(uint64_t);

    if (p + sizeof(uint32_t) > end) return -1;
    uint32_t mlen;
    memcpy(&mlen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (mlen > 0) {
        if (p + mlen > end) return -1;
        *model = malloc(mlen + 1);
        if (*model) { memcpy(*model, p, mlen); (*model)[mlen] = '\0'; }
        p += mlen;
    }

    if (p + sizeof(uint32_t) > end) return 0; /* description is optional */
    uint32_t dlen;
    memcpy(&dlen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (dlen > 0 && p + dlen <= end) {
        *description = malloc(dlen + 1);
        if (*description) { memcpy(*description, p, dlen); (*description)[dlen] = '\0'; }
    }
    return 0;
}

/* --- MSG_SESSION_RENAME --- */

uint8_t *ipc_encode_session_rename(uint32_t session_idx,
                                    const char *topic,
                                    uint32_t   *out_len) {
    uint32_t tlen = topic ? (uint32_t)strlen(topic) : 0;
    uint32_t sz   = sizeof(uint32_t) * 2 + tlen;
    uint8_t *buf  = malloc(sz);
    if (!buf) return NULL;
    uint8_t *p = buf;
    memcpy(p, &session_idx, sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(p, &tlen,        sizeof(uint32_t)); p += sizeof(uint32_t);
    if (tlen) memcpy(p, topic, tlen);
    *out_len = sz;
    return buf;
}

int ipc_decode_session_rename(const uint8_t *buf, uint32_t buf_len,
                               uint32_t *session_idx,
                               char    **topic) {
    *topic = NULL;
    if (buf_len < 8) return -1;
    const uint8_t *p = buf;
    memcpy(session_idx, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    uint32_t tlen;
    memcpy(&tlen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (tlen > 0 && p + tlen <= buf + buf_len) {
        *topic = malloc(tlen + 1);
        if (*topic) { memcpy(*topic, p, tlen); (*topic)[tlen] = '\0'; }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * MSG_WORKER_REQ
 * Payload: [u32 worker_type][u32 op_len][op][u32 args_len][args_json]
 * ------------------------------------------------------------------------- */

uint8_t *ipc_encode_worker_req(uint32_t    worker_type,
                                const char *op,
                                const char *args_json,
                                uint32_t   *out_len) {
    uint32_t op_len   = op        ? (uint32_t)strlen(op)        : 0;
    uint32_t args_len = args_json ? (uint32_t)strlen(args_json) : 0;
    uint32_t sz = sizeof(uint32_t) * 3 + op_len + args_len;
    uint8_t *buf = malloc(sz);
    if (!buf) return NULL;
    uint8_t *p = buf;
    memcpy(p, &worker_type, sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(p, &op_len,      sizeof(uint32_t)); p += sizeof(uint32_t);
    if (op_len) { memcpy(p, op, op_len); p += op_len; }
    memcpy(p, &args_len,    sizeof(uint32_t)); p += sizeof(uint32_t);
    if (args_len) memcpy(p, args_json, args_len);
    *out_len = sz;
    return buf;
}

int ipc_decode_worker_req(const uint8_t *buf, uint32_t buf_len,
                           uint32_t *worker_type,
                           char    **op,
                           char    **args_json) {
    *op = *args_json = NULL;
    if (buf_len < 12) return -1;
    const uint8_t *p   = buf;
    const uint8_t *end = buf + buf_len;

    memcpy(worker_type, p, sizeof(uint32_t)); p += sizeof(uint32_t);

    uint32_t op_len;
    memcpy(&op_len, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (p + op_len > end) return -1;
    *op = malloc(op_len + 1);
    if (!*op) return -1;
    memcpy(*op, p, op_len); (*op)[op_len] = '\0'; p += op_len;

    if (p + sizeof(uint32_t) > end) return -1;
    uint32_t args_len;
    memcpy(&args_len, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (args_len > 0) {
        if (p + args_len > end) { free(*op); *op = NULL; return -1; }
        *args_json = malloc(args_len + 1);
        if (!*args_json) { free(*op); *op = NULL; return -1; }
        memcpy(*args_json, p, args_len); (*args_json)[args_len] = '\0';
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * MSG_WORKER_REP
 * Payload: [u8 success][u32 result_len][result]
 * ------------------------------------------------------------------------- */

uint8_t *ipc_encode_worker_rep(int success, const char *result,
                                uint32_t *out_len) {
    uint32_t rlen = result ? (uint32_t)strlen(result) : 0;
    uint32_t sz   = 1 + sizeof(uint32_t) + rlen;
    uint8_t *buf  = malloc(sz);
    if (!buf) return NULL;
    uint8_t *p = buf;
    *p++ = (uint8_t)(success ? 1 : 0);
    memcpy(p, &rlen, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (rlen) memcpy(p, result, rlen);
    *out_len = sz;
    return buf;
}

int ipc_decode_worker_rep(const uint8_t *buf, uint32_t buf_len,
                           int *success, char **result) {
    *result = NULL;
    if (buf_len < 5) return -1;
    const uint8_t *p   = buf;
    const uint8_t *end = buf + buf_len;
    *success = (*p++ != 0) ? 1 : 0;
    uint32_t rlen;
    memcpy(&rlen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (rlen > 0 && p + rlen <= end) {
        *result = malloc(rlen + 1);
        if (*result) { memcpy(*result, p, rlen); (*result)[rlen] = '\0'; }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * MSG_APPROVAL_REQ
 * Payload: [u64 req_corr_id][u32 desc_len][description]
 * ------------------------------------------------------------------------- */

uint8_t *ipc_encode_approval_req(uint64_t req_corr_id, const char *description,
                                  uint32_t *out_len) {
    uint32_t dlen = description ? (uint32_t)strlen(description) : 0;
    uint32_t sz   = sizeof(uint64_t) + sizeof(uint32_t) + dlen;
    uint8_t *buf  = malloc(sz);
    if (!buf) return NULL;
    uint8_t *p = buf;
    memcpy(p, &req_corr_id, sizeof(uint64_t)); p += sizeof(uint64_t);
    memcpy(p, &dlen,        sizeof(uint32_t)); p += sizeof(uint32_t);
    if (dlen) memcpy(p, description, dlen);
    *out_len = sz;
    return buf;
}

int ipc_decode_approval_req(const uint8_t *buf, uint32_t buf_len,
                             uint64_t *req_corr_id, char **description) {
    *description = NULL;
    if (buf_len < 12) return -1;
    const uint8_t *p   = buf;
    const uint8_t *end = buf + buf_len;
    memcpy(req_corr_id, p, sizeof(uint64_t)); p += sizeof(uint64_t);
    uint32_t dlen;
    memcpy(&dlen, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    if (dlen > 0 && p + dlen <= end) {
        *description = malloc(dlen + 1);
        if (*description) { memcpy(*description, p, dlen); (*description)[dlen] = '\0'; }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * MSG_APPROVAL_REP
 * Payload: [u64 req_corr_id][u8 approved]
 * ------------------------------------------------------------------------- */

uint8_t *ipc_encode_approval_rep(uint64_t req_corr_id, int approved,
                                  uint32_t *out_len) {
    uint32_t sz  = sizeof(uint64_t) + 1;
    uint8_t *buf = malloc(sz);
    if (!buf) return NULL;
    memcpy(buf, &req_corr_id, sizeof(uint64_t));
    buf[sizeof(uint64_t)] = (uint8_t)(approved ? 1 : 0);
    *out_len = sz;
    return buf;
}

int ipc_decode_approval_rep(const uint8_t *buf, uint32_t buf_len,
                             uint64_t *req_corr_id, int *approved) {
    if (buf_len < sizeof(uint64_t) + 1) return -1;
    memcpy(req_corr_id, buf, sizeof(uint64_t));
    *approved = (buf[sizeof(uint64_t)] != 0) ? 1 : 0;
    return 0;
}

