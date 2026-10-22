/*
 * ESMTP client state machine (RFC 5321 + RFC 4954 AUTH PLAIN / LOGIN).
 * Purely event-driven and I/O-free: callers feed incoming bytes with
 * smtp_feed() and transmit outgoing data via ops.send().
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mail.h"

static void emit(struct smtp *sm, enum smtp_event_type type, const char *text)
{
    if (!sm->ops.on_event)
        return;
    struct smtp_event ev = { .type = type, .text = text };
    sm->ops.on_event(sm->ops.ctx, &ev);
}

static int send_raw(struct smtp *sm, const void *buf, size_t len)
{
    if (!sm->ops.send)
        return -1;
    return sm->ops.send(sm->ops.ctx, buf, len);
}

static int send_line(struct smtp *sm, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static int send_line(struct smtp *sm, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n > sizeof(buf) - 3)
        n = (int)(sizeof(buf) - 3);
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    return send_raw(sm, buf, (size_t)n);
}

void smtp_init(struct smtp *sm, const struct smtp_ops *ops)
{
    memset(sm, 0, sizeof(*sm));
    if (ops)
        sm->ops = *ops;
}

void smtp_reset(struct smtp *sm)
{
    struct smtp_ops ops = sm->ops;
    free(sm->msg_buf);
    smtp_init(sm, &ops);
}

int smtp_start(struct smtp *sm, const char *user, const char *pass, const char *from_addr,
               const char *to, const char *cc, const char *raw_msg, size_t msg_len)
{
    smtp_reset(sm);
    strlcpy(sm->user, user ? user : "", sizeof(sm->user));
    strlcpy(sm->pass, pass ? pass : "", sizeof(sm->pass));
    mime_parse_addr(from_addr, NULL, 0, sm->from, sizeof(sm->from));
    if (!sm->from[0] && user)
        strlcpy(sm->from, user, sizeof(sm->from));

    sm->nrcpts = mime_extract_addrs(to, sm->rcpts, MAIL_MAX_RCPTS);
    if (cc && *cc && sm->nrcpts < MAIL_MAX_RCPTS) {
        sm->nrcpts += mime_extract_addrs(cc, sm->rcpts + sm->nrcpts, MAIL_MAX_RCPTS - sm->nrcpts);
    }
    if (sm->nrcpts == 0) {
        emit(sm, SMTP_EV_ERROR, "Please enter a valid recipient email address.");
        return -1;
    }

    sm->msg_buf = malloc(msg_len + 1);
    if (!sm->msg_buf) {
        emit(sm, SMTP_EV_ERROR, "Out of memory.");
        return -1;
    }
    memcpy(sm->msg_buf, raw_msg, msg_len);
    sm->msg_buf[msg_len] = 0;
    sm->msg_len = msg_len;
    sm->phase = SMTP_GREETING;
    emit(sm, SMTP_EV_PROGRESS, "Connecting to mail server...");
    return 0;
}

/* Sends the RFC 5322 message with RFC 5321 leading-dot stuffing and CRLF.CRLF. */
static int send_dot_stuffed(struct smtp *sm)
{
    char chunk[1024];
    size_t o = 0;
    bool at_bol = true;
    const char *p = sm->msg_buf, *end = sm->msg_buf + sm->msg_len;

    while (p < end) {
        char c = *p++;
        if (c == '\r')
            continue;
        if (at_bol && c == '.') {
            if (o + 2 >= sizeof(chunk)) {
                if (send_raw(sm, chunk, o) < 0)
                    return -1;
                o = 0;
            }
            chunk[o++] = '.';
        }
        if (c == '\n') {
            if (o + 2 >= sizeof(chunk)) {
                if (send_raw(sm, chunk, o) < 0)
                    return -1;
                o = 0;
            }
            chunk[o++] = '\r';
            chunk[o++] = '\n';
            at_bol = true;
        } else {
            if (o + 1 >= sizeof(chunk)) {
                if (send_raw(sm, chunk, o) < 0)
                    return -1;
                o = 0;
            }
            chunk[o++] = c;
            at_bol = false;
        }
    }
    if (!at_bol) {
        if (o + 2 >= sizeof(chunk)) {
            if (send_raw(sm, chunk, o) < 0)
                return -1;
            o = 0;
        }
        chunk[o++] = '\r';
        chunk[o++] = '\n';
    }
    if (o > 0 && send_raw(sm, chunk, o) < 0)
        return -1;
    return send_raw(sm, ".\r\n", 3);
}

static void handle_reply(struct smtp *sm, int code, const char *text)
{
    if (sm->phase == SMTP_IDLE || sm->phase == SMTP_DONE)
        return;

    switch (sm->phase) {
    case SMTP_GREETING:
        if (code != 220) {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, "The SMTP server rejected the connection.");
            return;
        }
        sm->phase = SMTP_EHLO;
        send_line(sm, "EHLO glaze.local");
        break;

    case SMTP_EHLO:
        if (code != 250) {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, "SMTP handshake failed.");
            return;
        }
        if (sm->user[0] && sm->pass[0] && (sm->auth_plain_supported || !sm->auth_login_supported)) {
            char token[384];
            if (mime_sasl_plain(token, sizeof(token), sm->user, sm->pass) > 0) {
                sm->phase = SMTP_AUTH;
                emit(sm, SMTP_EV_PROGRESS, "Authenticating...");
                send_line(sm, "AUTH PLAIN %s", token);
                break;
            }
        }
        if (sm->user[0] && sm->pass[0] && sm->auth_login_supported) {
            sm->phase = SMTP_AUTH_USER;
            emit(sm, SMTP_EV_PROGRESS, "Authenticating...");
            send_line(sm, "AUTH LOGIN");
            break;
        }
        sm->phase = SMTP_MAIL_FROM;
        emit(sm, SMTP_EV_PROGRESS, "Sending message...");
        send_line(sm, "MAIL FROM:<%s>", sm->from);
        break;

    case SMTP_AUTH_USER:
        if (code == 334) {
            char b64[256];
            mime_base64_encode(b64, sizeof(b64), sm->user, strlen(sm->user));
            sm->phase = SMTP_AUTH_PASS;
            send_line(sm, "%s", b64);
        } else {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, "SMTP authentication failed.");
        }
        break;

    case SMTP_AUTH_PASS:
        if (code == 334) {
            char b64[256];
            mime_base64_encode(b64, sizeof(b64), sm->pass, strlen(sm->pass));
            sm->phase = SMTP_AUTH;
            send_line(sm, "%s", b64);
        } else {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, "SMTP authentication failed.");
        }
        break;

    case SMTP_AUTH:
        if (code != 235 && code != 250) {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, "SMTP sign-in failed. Check your App Password.");
            return;
        }
        sm->phase = SMTP_MAIL_FROM;
        emit(sm, SMTP_EV_PROGRESS, "Sending message...");
        send_line(sm, "MAIL FROM:<%s>", sm->from);
        break;

    case SMTP_MAIL_FROM:
        if (code != 250) {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, "Sender address was rejected by the server.");
            return;
        }
        sm->cur_rcpt = 0;
        sm->phase = SMTP_RCPT_TO;
        send_line(sm, "RCPT TO:<%s>", sm->rcpts[sm->cur_rcpt++]);
        break;

    case SMTP_RCPT_TO:
        if (code != 250 && code != 251) {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, "Recipient address was rejected by the server.");
            return;
        }
        if (sm->cur_rcpt < sm->nrcpts) {
            send_line(sm, "RCPT TO:<%s>", sm->rcpts[sm->cur_rcpt++]);
        } else {
            sm->phase = SMTP_DATA;
            send_line(sm, "DATA");
        }
        break;

    case SMTP_DATA:
        if (code != 354) {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, "Server refused message data.");
            return;
        }
        sm->phase = SMTP_BODY;
        send_dot_stuffed(sm);
        break;

    case SMTP_BODY:
        if (code != 250) {
            sm->phase = SMTP_DONE;
            emit(sm, SMTP_EV_ERROR, text && *text ? text : "Server rejected the message.");
            return;
        }
        sm->phase = SMTP_QUIT;
        send_line(sm, "QUIT");
        sm->phase = SMTP_DONE;
        emit(sm, SMTP_EV_SENT, NULL);
        break;

    case SMTP_QUIT:
    case SMTP_DONE:
    case SMTP_IDLE:
        break;
    }
}

static void handle_line(struct smtp *sm, const char *line)
{
    if (!isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) || !isdigit((unsigned char)line[2]))
        return;

    int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    char sep = line[3];
    const char *rest = line[3] ? line + 4 : "";

    if (sm->phase == SMTP_EHLO && !strncasecmp(rest, "AUTH", 4)) {
        if (strstr(rest, "PLAIN"))
            sm->auth_plain_supported = true;
        if (strstr(rest, "LOGIN"))
            sm->auth_login_supported = true;
    }

    if (sep == '-') {
        strlcpy(sm->multi_text, rest, sizeof(sm->multi_text));
        return;
    }
    handle_reply(sm, code, *rest ? rest : sm->multi_text);
    sm->multi_text[0] = 0;
}

void smtp_feed(struct smtp *sm, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            sm->line[sm->line_len] = 0;
            sm->line_len = 0;
            handle_line(sm, sm->line);
        } else if (sm->line_len + 1 < sizeof(sm->line)) {
            sm->line[sm->line_len++] = (char)c;
        }
    }
}
