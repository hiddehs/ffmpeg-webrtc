/*
 * WebRTC-HTTP ingestion protocol (WHIP) muxer
 * Copyright (c) 2023 The FFmpeg Project
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "libavcodec/avcodec.h"
#include "libavutil/base64.h"
#include "libavutil/bprint.h"
#include "libavutil/crc.h"
#include "libavutil/hmac.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/random_seed.h"
#include "libavutil/time.h"
#include "avc.h"
#include "avio_internal.h"
#include "http.h"
#include "internal.h"
#include "mux.h"
#include "network.h"
#include "srtp.h"

/**
 * Maximum size limit of a Session Description Protocol (SDP),
 * be it an offer or answer.
 */
#define MAX_SDP_SIZE 8192
/**
 * Maximum size of the buffer for sending and receiving UDP packets.
 * Please note that this size does not limit the size of the UDP packet that can be sent.
 * To set the limit for packet size, modify the `pkt_size` parameter.
 * For instance, it is possible to set the UDP buffer to 4096 to send or receive packets,
 * but please keep in mind that the `pkt_size` option limits the packet size to 1400.
 */
#define MAX_UDP_BUFFER_SIZE 4096

/**
 * The size of the Secure Real-time Transport Protocol (SRTP) master key material
 * that is exported by Secure Sockets Layer (SSL) after a successful Datagram
 * Transport Layer Security (DTLS) handshake. This material consists of a key
 * of 16 bytes and a salt of 14 bytes.
 */
#define DTLS_SRTP_KEY_LEN 16
#define DTLS_SRTP_SALT_LEN 14
/**
 * The maximum size of the Secure Real-time Transport Protocol (SRTP) HMAC checksum
 * and padding that is appended to the end of the packet. To calculate the maximum
 * size of the User Datagram Protocol (UDP) packet that can be sent out, subtract
 * this size from the `pkt_size`.
 */
#define DTLS_SRTP_CHECKSUM_LEN 16
/**
 * STAP-A stands for Single-Time Aggregation Packet.
 * The NALU type for STAP-A is 24 (0x18).
 */
#define NALU_TYPE_STAP_A 24

/**
 * When sending ICE or DTLS messages, responses are received via UDP. However, the peer
 * may not be ready and return EAGAIN, in which case we should wait for a short duration
 * and retry reading.
 * For instance, if we try to read from UDP and get EAGAIN, we sleep for 5ms and retry.
 * This macro is used to limit the total duration in milliseconds (e.g., 50ms), so we
 * will try at most 5 times.
 * Keep in mind that this macro should have a minimum duration of 5 ms.
 */
#define ICE_DTLS_READ_INTERVAL 50

/* The magic cookie for Session Traversal Utilities for NAT (STUN) messages. */
#define STUN_MAGIC_COOKIE 0x2112A442

/* Calculate the elapsed time from starttime to endtime in milliseconds. */
#define ELAPSED(starttime, endtime) ((int)(endtime - starttime) / 1000)

/* STUN Attribute, comprehension-required range (0x0000-0x7FFF) */
enum STUNAttr {
    STUN_ATTR_USERNAME                  = 0x0006, /// shared secret response/bind request
    STUN_ATTR_USE_CANDIDATE             = 0x0025, /// bind request
    STUN_ATTR_MESSAGE_INTEGRITY         = 0x0008, /// bind request/response
    STUN_ATTR_FINGERPRINT               = 0x8028, /// rfc5389
};

enum DTLSState {
    DTLS_STATE_NONE,

    /* Whether DTLS handshake is finished. */
    DTLS_STATE_FINISHED,
    /* Whether DTLS session is closed. */
    DTLS_STATE_CLOSED,
    /* Whether DTLS handshake is failed. */
    DTLS_STATE_FAILED,
};

typedef struct DTLSContext DTLSContext;
typedef int (*DTLSContext_on_state_fn)(DTLSContext *ctx, enum DTLSState state, const char* type, const char* desc);
typedef int (*DTLSContext_on_write_fn)(DTLSContext *ctx, char* data, int size);

typedef struct DTLSContext {
    AVClass *av_class;

    /* For callback. */
    DTLSContext_on_state_fn on_state;
    DTLSContext_on_write_fn on_write;
    void* opaque;

    /* For logging. */
    AVClass *log_avcl;

    /* The DTLS context. */
    SSL_CTX *dtls_ctx;
    SSL *dtls;
    /* The DTLS BIOs. */
    BIO *bio_in;

    /* The private key for DTLS handshake. */
    EVP_PKEY *dtls_pkey;
    /* The EC key for DTLS handshake. */
    EC_KEY* dtls_eckey;
    /* The SSL certificate used for fingerprint in SDP and DTLS handshake. */
    X509 *dtls_cert;
    /* The fingerprint of certificate, used in SDP offer. */
    char *dtls_fingerprint;

    /**
     * This represents the material used to build the SRTP master key. It is
     * generated by DTLS and has the following layout:
     *          16B         16B         14B             14B
     *      client_key | server_key | client_salt | server_salt
     */
    uint8_t dtls_srtp_materials[(DTLS_SRTP_KEY_LEN + DTLS_SRTP_SALT_LEN) * 2];

    /* Whether the DTLS is done at least for us. */
    int dtls_done_for_us;
    /* Whether the SRTP key is exported. */
    int dtls_srtp_key_exported;
    /* The number of packets retransmitted for DTLS. */
    int dtls_arq_packets;
    /**
     * This is the last DTLS content type and handshake type that is used to detect
     * the ARQ packet.
     */
    uint8_t dtls_last_content_type;
    uint8_t dtls_last_handshake_type;

    /* These variables represent timestamps used for calculating and tracking the cost. */
    int64_t dtls_init_starttime;
    int64_t dtls_init_endtime;
    int64_t dtls_handshake_starttime;
    int64_t dtls_handshake_endtime;

    /* Helper for get error code and message. */
    int error_code;
    char error_message[256];

    /**
     * The size of RTP packet, should generally be set to MTU.
     * Note that pion requires a smaller value, for example, 1200.
     */
    int mtu;
} DTLSContext;

static int is_dtls_packet(char *buf, int buf_size) {
    return buf_size > 13 && buf[0] > 19 && buf[0] < 64;
}

/**
 * Retrieves the error message for the latest OpenSSL error.
 *
 * This function retrieves the error code from the thread's error queue, converts it
 * to a human-readable string, and stores it in the DTLSContext's error_message field.
 * The error queue is then cleared using ERR_clear_error().
 */
static const char* openssl_get_error(DTLSContext *ctx)
{
    int r2 = ERR_get_error();
    if (r2)
        ERR_error_string_n(r2, ctx->error_message, sizeof(ctx->error_message));
    else
        ctx->error_message[0] = '\0';

    ERR_clear_error();
    return ctx->error_message;
}

/**
 * Get the error code for the given SSL operation result.
 *
 * This function retrieves the error code for the given SSL operation result
 * and stores the error message in the DTLS context if an error occurred.
 * It also clears the error queue.
 */
static int openssl_ssl_get_error(DTLSContext *ctx, int ret)
{
    SSL *dtls = ctx->dtls;
    int r1 = SSL_ERROR_NONE;

    if (ret <= 0)
        r1 = SSL_get_error(dtls, ret);

    openssl_get_error(ctx);
    return r1;
}

/**
 * Callback function to print the OpenSSL SSL status.
 */
static void openssl_dtls_on_info(const SSL *dtls, int where, int r0)
{
    int w, r1, is_fatal, is_warning, is_close_notify;
    const char *method = "undefined", *alert_type, *alert_desc;
    enum DTLSState state;
    DTLSContext *ctx = (DTLSContext*)SSL_get_ex_data(dtls, 0);

    w = where & ~SSL_ST_MASK;
    if (w & SSL_ST_CONNECT)
        method = "SSL_connect";
    else if (w & SSL_ST_ACCEPT)
        method = "SSL_accept";

    r1 = openssl_ssl_get_error(ctx, r0);
    if (where & SSL_CB_LOOP) {
        av_log(ctx, AV_LOG_VERBOSE, "DTLS: Info method=%s state=%s(%s), where=%d, ret=%d, r1=%d\n",
            method, SSL_state_string(dtls), SSL_state_string_long(dtls), where, r0, r1);
    } else if (where & SSL_CB_ALERT) {
        method = (where & SSL_CB_READ) ? "read":"write";

        alert_type = SSL_alert_type_string_long(r0);
        alert_desc = SSL_alert_desc_string(r0);

        if (!av_strcasecmp(alert_type, "warning") && !av_strcasecmp(alert_desc, "CN"))
            av_log(ctx, AV_LOG_WARNING, "DTLS: SSL3 alert method=%s type=%s, desc=%s(%s), where=%d, ret=%d, r1=%d\n",
                method, alert_type, alert_desc, SSL_alert_desc_string_long(r0), where, r0, r1);
        else
            av_log(ctx, AV_LOG_ERROR, "DTLS: SSL3 alert method=%s type=%s, desc=%s(%s), where=%d, ret=%d, r1=%d %s\n",
                method, alert_type, alert_desc, SSL_alert_desc_string_long(r0), where, r0, r1, ctx->error_message);

        /**
         * Notify the DTLS to handle the ALERT message, which maybe means media connection disconnect.
         * CN(Close Notify) is sent when peer close the PeerConnection. fatal, IP(Illegal Parameter)
         * is sent when DTLS failed.
         */
        is_fatal = !av_strncasecmp(alert_type, "fatal", 5);
        is_warning = !av_strncasecmp(alert_type, "warning", 7);
        is_close_notify = !av_strncasecmp(alert_desc, "CN", 2);
        state = is_fatal ? DTLS_STATE_FAILED : (is_warning && is_close_notify ? DTLS_STATE_CLOSED : DTLS_STATE_NONE);
        if (state != DTLS_STATE_NONE && ctx->on_state) {
            av_log(ctx, AV_LOG_INFO, "DTLS: Notify ctx=%p, state=%d, fatal=%d, warning=%d, cn=%d\n",
                ctx, state, is_fatal, is_warning, is_close_notify);
            ctx->on_state(ctx, state, alert_type, alert_desc);
        }
    } else if (where & SSL_CB_EXIT) {
        if (!r0)
            av_log(ctx, AV_LOG_WARNING, "DTLS: Fail method=%s state=%s(%s), where=%d, ret=%d, r1=%d\n",
                method, SSL_state_string(dtls), SSL_state_string_long(dtls), where, r0, r1);
        else if (r0 < 0)
            if (r1 != SSL_ERROR_NONE && r1 != SSL_ERROR_WANT_READ && r1 != SSL_ERROR_WANT_WRITE)
                av_log(ctx, AV_LOG_ERROR, "DTLS: Error method=%s state=%s(%s), where=%d, ret=%d, r1=%d %s\n",
                    method, SSL_state_string(dtls), SSL_state_string_long(dtls), where, r0, r1, ctx->error_message);
            else
                av_log(ctx, AV_LOG_VERBOSE, "DTLS: Info method=%s state=%s(%s), where=%d, ret=%d, r1=%d\n",
                    method, SSL_state_string(dtls), SSL_state_string_long(dtls), where, r0, r1);
    }
}

static void openssl_dtls_state_trace(DTLSContext *ctx, uint8_t *data, int length, int incoming)
{
    uint8_t content_type = 0;
    uint16_t size = 0;
    uint8_t handshake_type = 0;

    /* Change_cipher_spec(20), alert(21), handshake(22), application_data(23) */
    if (length >= 1)
        content_type = AV_RB8(&data[0]);
    if (length >= 13)
        size = AV_RB16(&data[11]);
    if (length >= 14)
        handshake_type = AV_RB8(&data[13]);

    av_log(ctx, AV_LOG_VERBOSE, "DTLS: Trace %s, done=%u, arq=%u, len=%u, cnt=%u, size=%u, hs=%u\n",
        (incoming? "RECV":"SEND"), ctx->dtls_done_for_us, ctx->dtls_arq_packets, length,
        content_type, size, handshake_type);
}

/**
 * Always return 1 to accept any certificate. This is because we allow the peer to
 * use a temporary self-signed certificate for DTLS.
 */
static int openssl_dtls_verify_callback(int preverify_ok, X509_STORE_CTX *ctx)
{
    return 1;
}

/**
 * DTLS BIO read callback.
 */
#if OPENSSL_VERSION_NUMBER < 0x30000000L // v3.0.x
static long openssl_dtls_bio_out_callback(BIO* b, int oper, const char* argp, int argi, long argl, long retvalue)
#else
static long openssl_dtls_bio_out_callback_ex(BIO *b, int oper, const char *argp, size_t len, int argi, long argl, int retvalue, size_t *processed)
#endif
{
    int ret, req_size = argi, is_arq = 0;
    uint8_t content_type, handshake_type;
    uint8_t *data = (uint8_t*)argp;
    DTLSContext* ctx = b ? (DTLSContext*)BIO_get_callback_arg(b) : NULL;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L // v3.0.x
    req_size = len;
    av_log(ctx, AV_LOG_DEBUG, "DTLS: BIO callback b=%p, oper=%d, argp=%p, len=%ld, argi=%d, argl=%ld, retvalue=%d, processed=%p, req_size=%d\n",
        b, oper, argp, len, argi, argl, retvalue, processed, req_size);
#else
    av_log(ctx, AV_LOG_DEBUG, "DTLS: BIO callback b=%p, oper=%d, argp=%p, argi=%d, argl=%ld, retvalue=%ld, req_size=%d\n",
        b, oper, argp, argi, argl, retvalue, req_size);
#endif

    if (oper != BIO_CB_WRITE || !argp || req_size <= 0)
        return retvalue;

    openssl_dtls_state_trace(ctx, data, req_size, 0);
    ret = ctx->on_write ? ctx->on_write(ctx, data, req_size) : 0;
    content_type = req_size > 0 ? AV_RB8(&data[0]) : 0;
    handshake_type = req_size > 13 ? AV_RB8(&data[13]) : 0;

    is_arq = ctx->dtls_last_content_type == content_type && ctx->dtls_last_handshake_type == handshake_type;
    ctx->dtls_arq_packets += is_arq;
    ctx->dtls_last_content_type = content_type;
    ctx->dtls_last_handshake_type = handshake_type;

    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Send request failed, oper=%d, content=%d, handshake=%d, size=%d, is_arq=%d\n",
            oper, content_type, handshake_type, req_size, is_arq);
        return ret;
    }

    return retvalue;
}

static int openssl_dtls_gen_private_key(DTLSContext *ctx)
{
    int ret = 0;

    /**
     * Note that secp256r1 in openssl is called NID_X9_62_prime256v1 or prime256v1 in string,
     * not NID_secp256k1 or secp256k1 in string.
     *
     * TODO: Should choose the curves in ClientHello.supported_groups, for example:
     *      Supported Group: x25519 (0x001d)
     *      Supported Group: secp256r1 (0x0017)
     *      Supported Group: secp384r1 (0x0018)
     */
#if OPENSSL_VERSION_NUMBER < 0x30000000L /* OpenSSL 3.0 */
    EC_GROUP *ecgroup = NULL;
    int curve = NID_X9_62_prime256v1;
#else
    const char *curve = SN_X9_62_prime256v1;
#endif

#if OPENSSL_VERSION_NUMBER < 0x30000000L /* OpenSSL 3.0 */
    ctx->dtls_pkey = EVP_PKEY_new();
    ctx->dtls_eckey = EC_KEY_new();
    ecgroup = EC_GROUP_new_by_curve_name(curve);
    if (!ecgroup) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Create EC group by curve=%d failed, %s", curve, openssl_get_error(ctx));
        goto einval_end;
    }

#if OPENSSL_VERSION_NUMBER < 0x10100000L // v1.1.x
    /* For openssl 1.0, we must set the group parameters, so that cert is ok. */
    EC_GROUP_set_asn1_flag(ecgroup, OPENSSL_EC_NAMED_CURVE);
#endif

    if (EC_KEY_set_group(ctx->dtls_eckey, ecgroup) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Generate private key, EC_KEY_set_group failed, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    if (EC_KEY_generate_key(ctx->dtls_eckey) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Generate private key, EC_KEY_generate_key failed, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    if (EVP_PKEY_set1_EC_KEY(ctx->dtls_pkey, ctx->dtls_eckey) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Generate private key, EVP_PKEY_set1_EC_KEY failed, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }
#else
    ctx->dtls_pkey = EVP_EC_gen(curve);
    if (!ctx->dtls_pkey) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Generate private key, EVP_EC_gen curve=%s failed, %s\n", curve, openssl_get_error(ctx));
        goto einval_end;
    }
#endif
    goto end;

einval_end:
    ret = AVERROR(EINVAL);
end:
#if OPENSSL_VERSION_NUMBER < 0x30000000L /* OpenSSL 3.0 */
    EC_GROUP_free(ecgroup);
#endif
    return ret;
}

static int openssl_dtls_gen_certificate(DTLSContext *ctx)
{
    int ret = 0, serial, expire_day, i, n = 0;
    AVBPrint fingerprint;
    unsigned char md[EVP_MAX_MD_SIZE];
    const char *aor = "ffmpeg.org";
    X509_NAME* subject = NULL;
    X509 *dtls_cert = NULL;

    /* To prevent a crash during cleanup, always initialize it. */
    av_bprint_init(&fingerprint, 1, MAX_URL_SIZE);

    dtls_cert = ctx->dtls_cert = X509_new();
    if (!dtls_cert) {
        goto enomem_end;
    }

    // TODO: Support non-self-signed certificate, for example, load from a file.
    subject = X509_NAME_new();
    if (!subject) {
        goto enomem_end;
    }

    serial = (int)av_get_random_seed();
    if (ASN1_INTEGER_set(X509_get_serialNumber(dtls_cert), serial) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to set serial, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, aor, strlen(aor), -1, 0) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to set CN, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    if (X509_set_issuer_name(dtls_cert, subject) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to set issuer, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }
    if (X509_set_subject_name(dtls_cert, subject) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to set subject name, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    expire_day = 365;
    if (!X509_gmtime_adj(X509_get_notBefore(dtls_cert), 0)) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to set notBefore, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }
    if (!X509_gmtime_adj(X509_get_notAfter(dtls_cert), 60*60*24*expire_day)) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to set notAfter, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    if (X509_set_version(dtls_cert, 2) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to set version, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    if (X509_set_pubkey(dtls_cert, ctx->dtls_pkey) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to set public key, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    if (!X509_sign(dtls_cert, ctx->dtls_pkey, EVP_sha1())) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to sign certificate, %s\n", openssl_get_error(ctx));
        goto einval_end;
    }

    /* Generate the fingerpint of certficate. */
    if (X509_digest(dtls_cert, EVP_sha256(), md, &n) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to generate fingerprint, %s\n", openssl_get_error(ctx));
        goto eio_end;
    }
    for (i = 0; i < n; i++) {
        av_bprintf(&fingerprint, "%02X", md[i]);
        if (i < n - 1)
            av_bprintf(&fingerprint, ":");
    }
    if (!fingerprint.str || !strlen(fingerprint.str)) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Fingerprint is empty\n");
        goto einval_end;
    }

    ctx->dtls_fingerprint = av_strdup(fingerprint.str);
    if (!ctx->dtls_fingerprint) {
        goto enomem_end;
    }

    goto end;
enomem_end:
    ret = AVERROR(ENOMEM);
    goto end;
eio_end:
    ret = AVERROR(EIO);
    goto end;
einval_end:
    ret = AVERROR(EINVAL);
end:
    X509_NAME_free(subject);
    av_bprint_finalize(&fingerprint, NULL);
    return ret;
}

/**
 * Initializes DTLS context using ECDHE.
 */
static av_cold int openssl_dtls_init_context(DTLSContext *ctx)
{
    int ret = 0;
    EVP_PKEY *dtls_pkey = ctx->dtls_pkey;
    X509 *dtls_cert = ctx->dtls_cert;
    SSL_CTX *dtls_ctx = NULL;
    SSL *dtls = NULL;
    BIO *bio_in = NULL, *bio_out = NULL;
    const char* ciphers = "ALL";
    const char* profiles = "SRTP_AES128_CM_SHA1_80";

    /* Refer to the test cases regarding these curves in the WebRTC code. */
#if OPENSSL_VERSION_NUMBER >= 0x10100000L /* OpenSSL 1.1.0 */
    const char* curves = "X25519:P-256:P-384:P-521";
#elif OPENSSL_VERSION_NUMBER >= 0x10002000L /* OpenSSL 1.0.2 */
    const char* curves = "P-256:P-384:P-521";
#endif

#if OPENSSL_VERSION_NUMBER < 0x10002000L /* OpenSSL v1.0.2 */
    dtls_ctx = ctx->dtls_ctx = SSL_CTX_new(DTLSv1_method());
#else
    dtls_ctx = ctx->dtls_ctx = SSL_CTX_new(DTLS_method());
#endif
    if (!dtls_ctx) {
        return AVERROR(ENOMEM);
    }

#if OPENSSL_VERSION_NUMBER >= 0x10002000L /* OpenSSL 1.0.2 */
    /* For ECDSA, we could set the curves list. */
    if (SSL_CTX_set1_curves_list(dtls_ctx, curves) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Init SSL_CTX_set1_curves_list failed, curves=%s, %s\n",
            curves, openssl_get_error(ctx));
        return AVERROR(EINVAL);
    }
#endif

#if OPENSSL_VERSION_NUMBER < 0x10100000L // v1.1.x
#if OPENSSL_VERSION_NUMBER < 0x10002000L // v1.0.2
    SSL_CTX_set_tmp_ecdh(dtls_ctx, ctx->dtls_eckey);
#else
    SSL_CTX_set_ecdh_auto(dtls_ctx, 1);
#endif
#endif

    /**
     * We activate "ALL" cipher suites to align with the peer's capabilities,
     * ensuring maximum compatibility.
     */
    if (SSL_CTX_set_cipher_list(dtls_ctx, ciphers) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Init SSL_CTX_set_cipher_list failed, ciphers=%s, %s\n",
            ciphers, openssl_get_error(ctx));
        return AVERROR(EINVAL);
    }
    /* Setup the certificate. */
    if (SSL_CTX_use_certificate(dtls_ctx, dtls_cert) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Init SSL_CTX_use_certificate failed, %s\n", openssl_get_error(ctx));
        return AVERROR(EINVAL);
    }
    if (SSL_CTX_use_PrivateKey(dtls_ctx, dtls_pkey) != 1) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Init SSL_CTX_use_PrivateKey failed, %s\n", openssl_get_error(ctx));
        return AVERROR(EINVAL);
    }

    /* Server will send Certificate Request. */
    SSL_CTX_set_verify(dtls_ctx, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE, openssl_dtls_verify_callback);
    /* The depth count is "level 0:peer certificate", "level 1: CA certificate",
     * "level 2: higher level CA certificate", and so on. */
    SSL_CTX_set_verify_depth(dtls_ctx, 4);
    /* Whether we should read as many input bytes as possible (for non-blocking reads) or not. */
    SSL_CTX_set_read_ahead(dtls_ctx, 1);
    /* Only support SRTP_AES128_CM_SHA1_80, please read ssl/d1_srtp.c */
    if (SSL_CTX_set_tlsext_use_srtp(dtls_ctx, profiles)) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Init SSL_CTX_set_tlsext_use_srtp failed, profiles=%s, %s\n",
            profiles, openssl_get_error(ctx));
        return AVERROR(EINVAL);
    }

    /* The dtls should not be created unless the dtls_ctx has been initialized. */
    dtls = ctx->dtls = SSL_new(dtls_ctx);
    if (!dtls) {
        return AVERROR(ENOMEM);
    }

    /* Setup the callback for logging. */
    SSL_set_ex_data(dtls, 0, ctx);
    SSL_set_info_callback(dtls, openssl_dtls_on_info);

    /**
     * We have set the MTU to fragment the DTLS packet. It is important to note that the
     * packet is split to ensure that each handshake packet is smaller than the MTU.
     */
    SSL_set_options(dtls, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(dtls, ctx->mtu);
#if OPENSSL_VERSION_NUMBER >= 0x100010b0L /* OpenSSL 1.0.1k */
    DTLS_set_link_mtu(dtls, ctx->mtu);
#endif

    bio_in = ctx->bio_in = BIO_new(BIO_s_mem());
    if (!bio_in) {
        return AVERROR(ENOMEM);
    }

    bio_out = BIO_new(BIO_s_mem());
    if (!bio_out) {
        return AVERROR(ENOMEM);
    }

    /**
     * Please be aware that it is necessary to use a callback to obtain the packet to be written out. It is
     * imperative that BIO_get_mem_data is not used to retrieve the packet, as it returns all the bytes that
     * need to be sent out.
     * For example, if MTU is set to 1200, and we got two DTLS packets to sendout:
     *      ServerHello, 95bytes.
     *      Certificate, 1105+143=1248bytes.
     * If use BIO_get_mem_data, it will return 95+1248=1343bytes, which is larger than MTU 1200.
     * If use callback, it will return two UDP packets:
     *      ServerHello+Certificate(Frament) = 95+1105=1200bytes.
     *      Certificate(Fragment) = 143bytes.
     * Note that there should be more packets in real world, like ServerKeyExchange, CertificateRequest,
     * and ServerHelloDone. Here we just use two packets for example.
     */
#if OPENSSL_VERSION_NUMBER < 0x30000000L // v3.0.x
    BIO_set_callback(bio_out, openssl_dtls_bio_out_callback);
#else
    BIO_set_callback_ex(bio_out, openssl_dtls_bio_out_callback_ex);
#endif
    BIO_set_callback_arg(bio_out, (char*)ctx);

    SSL_set_bio(dtls, bio_in, bio_out);

    return ret;
}

/**
 * Generate a self-signed certificate and private key for DTLS. Please note that the
 * ff_openssl_init in tls_openssl.c has already called SSL_library_init(), and therefore,
 * there is no need to call it again.
 */
static av_cold int dtls_context_init(DTLSContext *ctx)
{
    int ret = 0;

    ctx->dtls_init_starttime = av_gettime();

    /* Generate a private key to ctx->dtls_pkey. */
    if ((ret = openssl_dtls_gen_private_key(ctx)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to generate DTLS private key\n");
        return ret;
    }

    /* Generate a self-signed certificate. */
    if ((ret = openssl_dtls_gen_certificate(ctx)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to generate DTLS certificate\n");
        return ret;
    }

    if ((ret = openssl_dtls_init_context(ctx)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to initialize DTLS context\n");
        return ret;
    }

    ctx->dtls_init_endtime = av_gettime();
    av_log(ctx, AV_LOG_INFO, "DTLS: Setup ok, MTU=%d, cost=%dms, fingerprint %s\n",
        ctx->mtu, ELAPSED(ctx->dtls_init_starttime, av_gettime()), ctx->dtls_fingerprint);

    return ret;
}

/**
 * Once the DTLS role has been negotiated - active for the DTLS client or passive for the
 * DTLS server - we proceed to set up the DTLS state and initiate the handshake.
 */
static int dtls_context_start(DTLSContext *ctx)
{
    int ret = 0, r0, r1;
    SSL *dtls = ctx->dtls;

    ctx->dtls_handshake_starttime = av_gettime();

    /* Setup DTLS as passive, which is server role. */
    SSL_set_accept_state(dtls);

    /**
     * During initialization, we only need to call SSL_do_handshake once because SSL_read consumes
     * the handshake message if the handshake is incomplete.
     * To simplify maintenance, we initiate the handshake for both the DTLS server and client after
     * sending out the ICE response in the start_active_handshake function. It's worth noting that
     * although the DTLS server may receive the ClientHello immediately after sending out the ICE
     * response, this shouldn't be an issue as the handshake function is called before any DTLS
     * packets are received.
     */
    r0 = SSL_do_handshake(dtls);
    r1 = openssl_ssl_get_error(ctx, r0);
    // Fatal SSL error, for example, no available suite when peer is DTLS 1.0 while we are DTLS 1.2.
    if (r0 < 0 && (r1 != SSL_ERROR_NONE && r1 != SSL_ERROR_WANT_READ && r1 != SSL_ERROR_WANT_WRITE)) {
        av_log(ctx, AV_LOG_ERROR, "DTLS: Failed to drive SSL context, r0=%d, r1=%d %s\n", r0, r1, ctx->error_message);
        return AVERROR(EIO);
    }

    return ret;
}

/**
 * DTLS handshake with server, as a server in passive mode, using openssl.
 *
 * This function initializes the SSL context as the client role using OpenSSL and
 * then performs the DTLS handshake until success. Upon successful completion, it
 * exports the SRTP material key.
 *
 * @return 0 if OK, AVERROR_xxx on error
 */
static int dtls_context_write(DTLSContext *ctx, char* buf, int size)
{
    int ret = 0, res_ct, res_ht, r0, r1, do_callback;
    SSL *dtls = ctx->dtls;
    const char* dst = "EXTRACTOR-dtls_srtp";
    BIO *bio_in = ctx->bio_in;

    /* Got DTLS response successfully. */
    openssl_dtls_state_trace(ctx, buf, size, 1);
    if ((r0 = BIO_write(bio_in, buf, size)) <= 0) {
        res_ct = size > 0 ? buf[0]: 0;
        res_ht = size > 13 ? buf[13] : 0;
        av_log(ctx, AV_LOG_ERROR, "DTLS: Feed response failed, content=%d, handshake=%d, size=%d, r0=%d\n",
            res_ct, res_ht, size, r0);
        ret = AVERROR(EIO);
        goto end;
    }

    /**
     * If there is data available in bio_in, use SSL_read to allow SSL to process it.
     * We limit the MTU to 1200 for DTLS handshake, which ensures that the buffer is large enough for reading.
     */
    r0 = SSL_read(dtls, buf, sizeof(buf));
    r1 = openssl_ssl_get_error(ctx, r0);
    if (r0 <= 0) {
        if (r1 != SSL_ERROR_WANT_READ && r1 != SSL_ERROR_WANT_WRITE && r1 != SSL_ERROR_ZERO_RETURN) {
            av_log(ctx, AV_LOG_ERROR, "DTLS: Read failed, r0=%d, r1=%d %s\n", r0, r1, ctx->error_message);
            ret = AVERROR(EIO);
            goto end;
        }
    } else {
        av_log(ctx, AV_LOG_TRACE, "DTLS: Read %d bytes, r0=%d, r1=%d\n", r0, r0, r1);
    }

    /* Check whether the DTLS is completed. */
    if (SSL_is_init_finished(dtls) != 1)
        goto end;

    do_callback = ctx->on_state && !ctx->dtls_done_for_us;
    ctx->dtls_done_for_us = 1;
    ctx->dtls_handshake_endtime = av_gettime();

    /* Export SRTP master key after DTLS done */
    if (!ctx->dtls_srtp_key_exported) {
        ret = SSL_export_keying_material(dtls, ctx->dtls_srtp_materials, sizeof(ctx->dtls_srtp_materials),
            dst, strlen(dst), NULL, 0, 0);
        r1 = openssl_ssl_get_error(ctx, r0);
        if (!ret) {
            av_log(ctx, AV_LOG_ERROR, "DTLS: SSL export key ret=%d, r1=%d %s\n", ret, r1, ctx->error_message);
            ret = AVERROR(EIO);
            goto end;
        }

        ctx->dtls_srtp_key_exported = 1;
    }

    if (do_callback && (ret = ctx->on_state(ctx, DTLS_STATE_FINISHED, NULL, NULL)) < 0)
        goto end;

end:
    return ret;
}

/**
 * Cleanup the DTLS context.
 */
static av_cold void dtls_context_deinit(DTLSContext *ctx)
{
    SSL_free(ctx->dtls);
    SSL_CTX_free(ctx->dtls_ctx);
    X509_free(ctx->dtls_cert);
    EVP_PKEY_free(ctx->dtls_pkey);
    av_freep(&ctx->dtls_fingerprint);
#if OPENSSL_VERSION_NUMBER < 0x30000000L /* OpenSSL 3.0 */
    EC_KEY_free(ctx->dtls_eckey);
#endif
}

enum RTCState {
    RTC_STATE_NONE,

    /* The initial state. */
    RTC_STATE_INIT,
    /* The muxer has sent the offer to the peer. */
    RTC_STATE_OFFER,
    /* The muxer has received the answer from the peer. */
    RTC_STATE_ANSWER,
    /**
     * After parsing the answer received from the peer, the muxer negotiates the abilities
     * in the offer that it generated.
     */
    RTC_STATE_NEGOTIATED,
    /* The muxer has connected to the peer via UDP. */
    RTC_STATE_UDP_CONNECTED,
    /* The muxer has sent the ICE request to the peer. */
    RTC_STATE_ICE_CONNECTING,
    /* The muxer has received the ICE response from the peer. */
    RTC_STATE_ICE_CONNECTED,
    /* The muxer has finished the DTLS handshake with the peer. */
    RTC_STATE_DTLS_FINISHED,
    /* The muxer has finished the SRTP setup. */
    RTC_STATE_SRTP_FINISHED,
    /* The muxer is ready to send/receive media frames. */
    RTC_STATE_READY,
    /* The muxer is failed. */
    RTC_STATE_FAILED,
};

typedef struct RTCContext {
    AVClass *av_class;

    /* The state of the RTC connection. */
    enum RTCState state;
    /* The callback return value for DTLS. */
    int dtls_ret;
    int dtls_closed;

    /* Parameters for the input audio and video codecs. */
    AVCodecParameters *audio_par;
    AVCodecParameters *video_par;

    /* The SPS/PPS of AVC video */
    uint8_t *avc_sps;
    int avc_sps_size;
    uint8_t *avc_pps;
    int avc_pps_size;
    /* The size of NALU in ISOM format. */
    int avc_nal_length_size;

    /* The ICE username and pwd fragment generated by the muxer. */
    char ice_ufrag_local[9];
    char ice_pwd_local[33];
    /* The SSRC of the audio and video stream, generated by the muxer. */
    uint32_t audio_ssrc;
    uint32_t video_ssrc;
    /* The PT(Payload Type) of stream, generated by the muxer. */
    uint8_t audio_payload_type;
    uint8_t video_payload_type;
    /**
     * This is the SDP offer generated by the muxer based on the codec parameters,
     * DTLS, and ICE information.
     */
    char *sdp_offer;

    /* The ICE username and pwd from remote server. */
    char *ice_ufrag_remote;
    char *ice_pwd_remote;
    /**
     * This represents the ICE candidate protocol, priority, host and port.
     * Currently, we only support one candidate and choose the first UDP candidate.
     * However, we plan to support multiple candidates in the future.
     */
    char *ice_protocol;
    char *ice_host;
    int ice_port;

    /* The SDP answer received from the WebRTC server. */
    char *sdp_answer;
    /* The resource URL returned in the Location header of WHIP HTTP response. */
    char *whip_resource_url;

    /* These variables represent timestamps used for calculating and tracking the cost. */
    int64_t rtc_starttime;
    int64_t rtc_init_time;
    int64_t rtc_offer_time;
    int64_t rtc_answer_time;
    int64_t rtc_udp_time;
    int64_t rtc_ice_time;
    int64_t rtc_dtls_time;
    int64_t rtc_srtp_time;
    int64_t rtc_ready_time;

    /* The DTLS context. */
    DTLSContext dtls_ctx;

    /* The SRTP send context, to encrypt outgoing packets. */
    struct SRTPContext srtp_audio_send;
    struct SRTPContext srtp_video_send;
    struct SRTPContext srtp_rtcp_send;
    /* The SRTP receive context, to decrypt incoming packets. */
    struct SRTPContext srtp_recv;

    /* The time jitter base for audio OPUS stream. */
    int64_t audio_jitter_base;

    /* The UDP transport is used for delivering ICE, DTLS and SRTP packets. */
    URLContext *udp_uc;
    /* The buffer for UDP transmission. */
    char buf[MAX_UDP_BUFFER_SIZE];

    /* The timeout in milliseconds for ICE and DTLS handshake. */
    int handshake_timeout;
    /**
     * The size of RTP packet, should generally be set to MTU.
     * Note that pion requires a smaller value, for example, 1200.
     */
    int pkt_size;
    /**
     * The optional Bearer token for WHIP Authorization.
     * See https://www.ietf.org/archive/id/draft-ietf-wish-whip-08.html#name-authentication-and-authoriz
     */
    char* authorization;
} RTCContext;

/**
 * When DTLS state change.
 */
static int dtls_context_on_state(DTLSContext *ctx, enum DTLSState state, const char* type, const char* desc)
{
    int ret = 0;
    AVFormatContext *s = ctx->opaque;
    RTCContext *rtc = s->priv_data;

    if (state == DTLS_STATE_CLOSED) {
        rtc->dtls_closed = 1;
        av_log(rtc, AV_LOG_INFO, "WHIP: DTLS session closed, type=%s, desc=%s, elapsed=%dms\n",
            type ? type : "", desc ? desc : "", ELAPSED(rtc->rtc_starttime, av_gettime()));
        return ret;
    }

    if (state == DTLS_STATE_FAILED) {
        rtc->state = RTC_STATE_FAILED;
        av_log(rtc, AV_LOG_ERROR, "WHIP: DTLS session failed, type=%s, desc=%s\n",
            type ? type : "", desc ? desc : "");
        rtc->dtls_ret = AVERROR(EIO);
        return ret;
    }

    if (state == DTLS_STATE_FINISHED && rtc->state < RTC_STATE_DTLS_FINISHED) {
        rtc->state = RTC_STATE_DTLS_FINISHED;
        rtc->rtc_dtls_time = av_gettime();
        av_log(rtc, AV_LOG_INFO, "WHIP: DTLS handshake, done=%d, exported=%d, arq=%d, srtp_material=%luB, cost=%dms, elapsed=%dms\n",
            ctx->dtls_done_for_us, ctx->dtls_srtp_key_exported, ctx->dtls_arq_packets, sizeof(ctx->dtls_srtp_materials),
            ELAPSED(ctx->dtls_handshake_starttime, ctx->dtls_handshake_endtime),
            ELAPSED(rtc->rtc_starttime, av_gettime()));
        return ret;
    }

    return ret;
}

/**
 * When DTLS write data.
 */
static int dtls_context_on_write(DTLSContext *ctx, char* data, int size)
{
    AVFormatContext *s = ctx->opaque;
    RTCContext *rtc = s->priv_data;

    if (!rtc->udp_uc) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: DTLS write data, but udp_uc is NULL\n");
        return AVERROR(EIO);
    }

    return ffurl_write(rtc->udp_uc, data, size);
}

/**
 * Initialize and check the options for the WebRTC muxer.
 */
static av_cold int whip_init(AVFormatContext *s)
{
    int ret, ideal_pkt_size = 532;
    RTCContext *rtc = s->priv_data;

    rtc->rtc_starttime = av_gettime();

    /* Use the same logging context as AV format. */
    rtc->dtls_ctx.av_class = rtc->av_class;
    rtc->dtls_ctx.mtu = rtc->pkt_size;
    rtc->dtls_ctx.opaque = s;
    rtc->dtls_ctx.on_state = dtls_context_on_state;
    rtc->dtls_ctx.on_write = dtls_context_on_write;

    if ((ret = dtls_context_init(&rtc->dtls_ctx)) < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to init DTLS context\n");
        return ret;
    }

    if (rtc->pkt_size < ideal_pkt_size)
        av_log(rtc, AV_LOG_WARNING, "WHIP: pkt_size=%d(<%d) is too small, may cause packet loss\n",
            rtc->pkt_size, ideal_pkt_size);

    if (rtc->state < RTC_STATE_INIT)
        rtc->state = RTC_STATE_INIT;
    rtc->rtc_init_time = av_gettime();
    av_log(rtc, AV_LOG_INFO, "WHIP: Init state=%d, handshake_timeout=%dms, pkt_size=%d, elapsed=%dms\n",
        rtc->state, rtc->handshake_timeout, rtc->pkt_size, ELAPSED(rtc->rtc_starttime, av_gettime()));

    return 0;
}

/**
 * Parses the ISOM AVCC format of extradata and extracts SPS/PPS.
 *
 * This function is used to parse SPS/PPS from the extradata in ISOM AVCC format.
 * It can handle both ISOM and annexb formats but only parses data in ISOM format.
 * If the extradata is in annexb format, this function ignores it, and uses the entire
 * extradata as a sequence header with SPS/PPS. Refer to ff_isom_write_avcc.
 *
 * @param s                Pointer to the AVFormatContext
 * @param extradata        Pointer to the extradata
 * @param extradata_size   Size of the extradata
 * @returns                Returns 0 if successful or AVERROR_xxx in case of an error.
 */
static int isom_read_avcc(AVFormatContext *s, uint8_t *extradata, int  extradata_size)
{
    int ret = 0;
    uint8_t version, nal_length_size, nb_sps, nb_pps;
    AVIOContext *pb;
    RTCContext *rtc = s->priv_data;

    if (!extradata || !extradata_size)
        return 0;

    /* Not H.264 ISOM format, may be annexb etc. */
    if (extradata_size < 4 || extradata[0] != 1) {
        if (!ff_avc_find_startcode(extradata, extradata + extradata_size)) {
            av_log(rtc, AV_LOG_ERROR, "Format must be ISOM or annexb\n");
            return AVERROR_INVALIDDATA;
        }
        return 0;
    }

    /* Parse the SPS/PPS in ISOM format in extradata. */
    pb = avio_alloc_context(extradata, extradata_size, 0, NULL, NULL, NULL, NULL);
    if (!pb)
        return AVERROR(ENOMEM);

    version = avio_r8(pb); /* version */
    avio_r8(pb); /* avc profile */
    avio_r8(pb); /* avc profile compat */
    avio_r8(pb); /* avc level */
    nal_length_size = avio_r8(pb); /* 6 bits reserved (111111) + 2 bits nal size length - 1 (11) */
    nb_sps = avio_r8(pb); /* 3 bits reserved (111) + 5 bits number of sps */

    if (version != 1) {
        av_log(rtc, AV_LOG_ERROR, "ISOM invalid version=%d\n", version);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    rtc->avc_nal_length_size = (nal_length_size & 0x03) + 1;
    if (rtc->avc_nal_length_size == 3) {
        av_log(rtc, AV_LOG_ERROR, "ISOM invalid nal length size=%d\n", rtc->avc_nal_length_size);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    /* Read SPS */
    nb_sps &= 0x1f;
    if (nb_sps != 1 || avio_feof(pb)) {
        av_log(rtc, AV_LOG_ERROR, "ISOM invalid number of sps=%d, eof=%d\n", nb_sps, avio_feof(pb));
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    rtc->avc_sps_size = avio_rb16(pb); /* sps size */
    if (rtc->avc_sps_size <= 0 || avio_feof(pb)) {
        av_log(rtc, AV_LOG_ERROR, "ISOM invalid sps size=%d, eof=%d\n", rtc->avc_sps_size, avio_feof(pb));
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    rtc->avc_sps = av_malloc(rtc->avc_sps_size);
    if (!rtc->avc_sps) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avio_read(pb, rtc->avc_sps, rtc->avc_sps_size); /* sps */
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "Failed to read sps, size=%d\n", rtc->avc_sps_size);
        goto end;
    }

    /* Read PPS */
    nb_pps = avio_r8(pb); /* number of pps */
    if (nb_pps != 1 || avio_feof(pb)) {
        av_log(rtc, AV_LOG_ERROR, "ISOM invalid number of pps=%d, eof=%d\n", nb_pps, avio_feof(pb));
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    rtc->avc_pps_size = avio_rb16(pb); /* pps size */
    if (rtc->avc_pps_size <= 0 || avio_feof(pb)) {
        av_log(rtc, AV_LOG_ERROR, "ISOM invalid pps size=%d, eof=%d\n", rtc->avc_pps_size, avio_feof(pb));
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    rtc->avc_pps = av_malloc(rtc->avc_pps_size);
    if (!rtc->avc_pps) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avio_read(pb, rtc->avc_pps, rtc->avc_pps_size); /* pps */
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "ISOM failed to read pps, size=%d\n", rtc->avc_pps_size);
        goto end;
    }

end:
    avio_context_free(&pb);
    return ret;
}

/**
 * Parses video SPS/PPS from the extradata of codecpar and checks the codec.
 * Currently only supports video(h264) and audio(opus). Note that only baseline
 * and constrained baseline profiles of h264 are supported.
 *
 * If the profile is less than 0, the function considers the profile as baseline.
 * It may need to parse the profile from SPS/PPS. This situation occurs when ingesting
 * desktop and transcoding.
 *
 * @param s Pointer to the AVFormatContext
 * @returns Returns 0 if successful or AVERROR_xxx in case of an error.
 */
static int parse_codec(AVFormatContext *s)
{
    int i, ret;
    RTCContext *rtc = s->priv_data;

    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
        switch (par->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (rtc->video_par) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Only one video stream is supported by RTC\n");
                return AVERROR(EINVAL);
            }
            rtc->video_par = par;

            if (par->codec_id != AV_CODEC_ID_H264) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Unsupported video codec %s by RTC, choose h264\n",
                       desc ? desc->name : "unknown");
                return AVERROR_PATCHWELCOME;
            }

            if (par->video_delay > 0) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Unsupported B frames by RTC\n");
                return AVERROR_PATCHWELCOME;
            }

            ret = isom_read_avcc(s, par->extradata, par->extradata_size);
            if (ret < 0) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to parse SPS/PPS from extradata\n");
                return ret;
            }
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (rtc->audio_par) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Only one audio stream is supported by RTC\n");
                return AVERROR(EINVAL);
            }
            rtc->audio_par = par;

            if (par->codec_id != AV_CODEC_ID_OPUS) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Unsupported audio codec %s by RTC, choose opus\n",
                    desc ? desc->name : "unknown");
                return AVERROR_PATCHWELCOME;
            }

            if (par->ch_layout.nb_channels != 2) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Unsupported audio channels %d by RTC, choose stereo\n",
                    par->ch_layout.nb_channels);
                return AVERROR_PATCHWELCOME;
            }

            if (par->sample_rate != 48000) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Unsupported audio sample rate %d by RTC, choose 48000\n", par->sample_rate);
                return AVERROR_PATCHWELCOME;
            }
            break;
        default:
            av_log(rtc, AV_LOG_ERROR, "WHIP: Codec type '%s' for stream %d is not supported by RTC\n",
                   av_get_media_type_string(par->codec_type), i);
            return AVERROR_PATCHWELCOME;
        }
    }

    return 0;
}

/**
 * Generate SDP offer according to the codec parameters, DTLS and ICE information.
 *
 * Note that we don't use av_sdp_create to generate SDP offer because it doesn't
 * support DTLS and ICE information.
 *
 * @return 0 if OK, AVERROR_xxx on error
 */
static int generate_sdp_offer(AVFormatContext *s)
{
    int ret = 0, profile, level, profile_iop;
    AVBPrint bp;
    RTCContext *rtc = s->priv_data;

    /* To prevent a crash during cleanup, always initialize it. */
    av_bprint_init(&bp, 1, MAX_SDP_SIZE);

    if (rtc->sdp_offer) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: SDP offer is already set\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    snprintf(rtc->ice_ufrag_local, sizeof(rtc->ice_ufrag_local), "%08x",
             av_get_random_seed());
    snprintf(rtc->ice_pwd_local, sizeof(rtc->ice_pwd_local), "%08x%08x%08x%08x",
             av_get_random_seed(), av_get_random_seed(), av_get_random_seed(),
             av_get_random_seed());

    rtc->audio_ssrc = av_get_random_seed();
    rtc->video_ssrc = av_get_random_seed();

    rtc->audio_payload_type = 111;
    rtc->video_payload_type = 106;

    av_bprintf(&bp, ""
        "v=0\r\n"
        "o=FFmpeg 4489045141692799359 2 IN IP4 127.0.0.1\r\n"
        "s=FFmpegPublishSession\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "a=extmap-allow-mixed\r\n"
        "a=msid-semantic: WMS\r\n");

    if (rtc->audio_par) {
        av_bprintf(&bp, ""
            "m=audio 9 UDP/TLS/RTP/SAVPF %u\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=ice-ufrag:%s\r\n"
            "a=ice-pwd:%s\r\n"
            "a=fingerprint:sha-256 %s\r\n"
            "a=setup:passive\r\n"
            "a=mid:0\r\n"
            "a=sendonly\r\n"
            "a=msid:FFmpeg audio\r\n"
            "a=rtcp-mux\r\n"
            "a=rtpmap:%u opus/%d/%d\r\n"
            "a=ssrc:%u cname:FFmpeg\r\n"
            "a=ssrc:%u msid:FFmpeg audio\r\n",
            rtc->audio_payload_type,
            rtc->ice_ufrag_local,
            rtc->ice_pwd_local,
            rtc->dtls_ctx.dtls_fingerprint,
            rtc->audio_payload_type,
            rtc->audio_par->sample_rate,
            rtc->audio_par->ch_layout.nb_channels,
            rtc->audio_ssrc,
            rtc->audio_ssrc);
    }

    if (rtc->video_par) {
        profile = rtc->video_par->profile < 0 ? 0x42 : rtc->video_par->profile;
        level = rtc->video_par->level < 0 ? 30 : rtc->video_par->level;
        profile_iop = profile & FF_PROFILE_H264_CONSTRAINED;
        av_bprintf(&bp, ""
            "m=video 9 UDP/TLS/RTP/SAVPF %u\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=ice-ufrag:%s\r\n"
            "a=ice-pwd:%s\r\n"
            "a=fingerprint:sha-256 %s\r\n"
            "a=setup:passive\r\n"
            "a=mid:1\r\n"
            "a=sendonly\r\n"
            "a=msid:FFmpeg video\r\n"
            "a=rtcp-mux\r\n"
            "a=rtcp-rsize\r\n"
            "a=rtpmap:%u H264/90000\r\n"
            "a=fmtp:%u level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%02x%02x%02x\r\n"
            "a=ssrc:%u cname:FFmpeg\r\n"
            "a=ssrc:%u msid:FFmpeg video\r\n",
            rtc->video_payload_type,
            rtc->ice_ufrag_local,
            rtc->ice_pwd_local,
            rtc->dtls_ctx.dtls_fingerprint,
            rtc->video_payload_type,
            rtc->video_payload_type,
            profile & (~FF_PROFILE_H264_CONSTRAINED),
            profile_iop,
            level,
            rtc->video_ssrc,
            rtc->video_ssrc);
    }

    if (!av_bprint_is_complete(&bp)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Offer exceed max %d, %s\n", MAX_SDP_SIZE, bp.str);
        ret = AVERROR(EIO);
        goto end;
    }

    rtc->sdp_offer = av_strdup(bp.str);
    if (!rtc->sdp_offer) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (rtc->state < RTC_STATE_OFFER)
        rtc->state = RTC_STATE_OFFER;
    rtc->rtc_offer_time = av_gettime();
    av_log(rtc, AV_LOG_VERBOSE, "WHIP: Generated state=%d, offer: %s\n", rtc->state, rtc->sdp_offer);

end:
    av_bprint_finalize(&bp, NULL);
    return ret;
}

/**
 * Exchange SDP offer with WebRTC peer to get the answer.
 *
 * @return 0 if OK, AVERROR_xxx on error
 */
static int exchange_sdp(AVFormatContext *s)
{
    int ret;
    char buf[MAX_URL_SIZE];
    AVBPrint bp;
    RTCContext *rtc = s->priv_data;
    /* The URL context is an HTTP transport layer for the WHIP protocol. */
    URLContext *whip_uc = NULL;

    /* To prevent a crash during cleanup, always initialize it. */
    av_bprint_init(&bp, 1, MAX_SDP_SIZE);

    ret = ffurl_alloc(&whip_uc, s->url, AVIO_FLAG_READ_WRITE, &s->interrupt_callback);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to alloc HTTP context: %s\n", s->url);
        goto end;
    }

    if (!rtc->sdp_offer || !strlen(rtc->sdp_offer)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: No offer to exchange\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    ret = snprintf(buf, sizeof(buf),
             "Cache-Control: no-cache\r\n"
             "Content-Type: application/sdp\r\n");
    if (rtc->authorization)
        ret += snprintf(buf + ret, sizeof(buf) - ret, "Authorization: Bearer %s\r\n", rtc->authorization);
    if (ret <= 0 || ret >= sizeof(buf)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to generate headers, size=%d, %s\n", ret, buf);
        ret = AVERROR(EINVAL);
        goto end;
    }

    av_opt_set(whip_uc->priv_data, "headers", buf, 0);
    av_opt_set(whip_uc->priv_data, "chunked_post", "0", 0);
    av_opt_set_bin(whip_uc->priv_data, "post_data", rtc->sdp_offer, (int)strlen(rtc->sdp_offer), 0);

    ret = ffurl_connect(whip_uc, NULL);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to request url=%s, offer: %s\n", s->url, rtc->sdp_offer);
        goto end;
    }

    if (ff_http_get_new_location(whip_uc)) {
        rtc->whip_resource_url = av_strdup(ff_http_get_new_location(whip_uc));
        if (!rtc->whip_resource_url) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    while (1) {
        ret = ffurl_read(whip_uc, buf, sizeof(buf));
        if (ret == AVERROR_EOF) {
            /* Reset the error because we read all response as answer util EOF. */
            ret = 0;
            break;
        }
        if (ret <= 0) {
            av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to read response from url=%s, offer is %s, answer is %s\n",
                s->url, rtc->sdp_offer, rtc->sdp_answer);
            goto end;
        }

        av_bprintf(&bp, "%.*s", ret, buf);
        if (!av_bprint_is_complete(&bp)) {
            av_log(rtc, AV_LOG_ERROR, "WHIP: Answer exceed max size %d, %.*s, %s\n", MAX_SDP_SIZE, ret, buf, bp.str);
            ret = AVERROR(EIO);
            goto end;
        }
    }

    if (!av_strstart(bp.str, "v=", NULL)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Invalid answer: %s\n", bp.str);
        ret = AVERROR(EINVAL);
        goto end;
    }

    rtc->sdp_answer = av_strdup(bp.str);
    if (!rtc->sdp_answer) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (rtc->state < RTC_STATE_ANSWER)
        rtc->state = RTC_STATE_ANSWER;
    av_log(rtc, AV_LOG_VERBOSE, "WHIP: Got state=%d, answer: %s\n", rtc->state, rtc->sdp_answer);

end:
    ffurl_closep(&whip_uc);
    av_bprint_finalize(&bp, NULL);
    return ret;
}

/**
 * Parses the ICE ufrag, pwd, and candidates from the SDP answer.
 *
 * This function is used to extract the ICE ufrag, pwd, and candidates from the SDP answer.
 * It returns an error if any of these fields is NULL. The function only uses the first
 * candidate if there are multiple candidates. However, support for multiple candidates
 * will be added in the future.
 *
 * @param s Pointer to the AVFormatContext
 * @returns Returns 0 if successful or AVERROR_xxx if an error occurs.
 */
static int parse_answer(AVFormatContext *s)
{
    int ret = 0;
    AVIOContext *pb;
    char line[MAX_URL_SIZE];
    const char *ptr;
    int i;
    RTCContext *rtc = s->priv_data;

    if (!rtc->sdp_answer || !strlen(rtc->sdp_answer)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: No answer to parse\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    pb = avio_alloc_context(rtc->sdp_answer, strlen(rtc->sdp_answer), 0, NULL, NULL, NULL, NULL);
    if (!pb)
        return AVERROR(ENOMEM);

    for (i = 0; !avio_feof(pb); i++) {
        ff_get_chomp_line(pb, line, sizeof(line));
        if (av_strstart(line, "a=ice-ufrag:", &ptr) && !rtc->ice_ufrag_remote) {
            rtc->ice_ufrag_remote = av_strdup(ptr);
            if (!rtc->ice_ufrag_remote) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
        } else if (av_strstart(line, "a=ice-pwd:", &ptr) && !rtc->ice_pwd_remote) {
            rtc->ice_pwd_remote = av_strdup(ptr);
            if (!rtc->ice_pwd_remote) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
        } else if (av_strstart(line, "a=candidate:", &ptr) && !rtc->ice_protocol) {
            ptr = av_stristr(ptr, "udp");
            if (ptr && av_stristr(ptr, "host")) {
                char protocol[17], host[129];
                int priority, port;
                ret = sscanf(ptr, "%16s %d %128s %d typ host", protocol, &priority, host, &port);
                if (ret != 4) {
                    av_log(rtc, AV_LOG_ERROR, "WHIP: Failed %d to parse line %d %s from %s\n",
                        ret, i, line, rtc->sdp_answer);
                    ret = AVERROR(EIO);
                    goto end;
                }

                if (av_strcasecmp(protocol, "udp")) {
                    av_log(rtc, AV_LOG_ERROR, "WHIP: Protocol %s is not supported by RTC, choose udp, line %d %s of %s\n",
                        protocol, i, line, rtc->sdp_answer);
                    ret = AVERROR(EIO);
                    goto end;
                }

                rtc->ice_protocol = av_strdup(protocol);
                rtc->ice_host = av_strdup(host);
                rtc->ice_port = port;
                if (!rtc->ice_protocol || !rtc->ice_host) {
                    ret = AVERROR(ENOMEM);
                    goto end;
                }
            }
        }
    }

    if (!rtc->ice_pwd_remote || !strlen(rtc->ice_pwd_remote)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: No remote ice pwd parsed from %s\n", rtc->sdp_answer);
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (!rtc->ice_ufrag_remote || !strlen(rtc->ice_ufrag_remote)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: No remote ice ufrag parsed from %s\n", rtc->sdp_answer);
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (!rtc->ice_protocol || !rtc->ice_host || !rtc->ice_port) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: No ice candidate parsed from %s\n", rtc->sdp_answer);
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (rtc->state < RTC_STATE_NEGOTIATED)
        rtc->state = RTC_STATE_NEGOTIATED;
    rtc->rtc_answer_time = av_gettime();
    av_log(rtc, AV_LOG_INFO, "WHIP: SDP state=%d, offer=%luB, answer=%luB, ufrag=%s, pwd=%luB, transport=%s://%s:%d, elapsed=%dms\n",
        rtc->state, strlen(rtc->sdp_offer), strlen(rtc->sdp_answer), rtc->ice_ufrag_remote, strlen(rtc->ice_pwd_remote),
        rtc->ice_protocol, rtc->ice_host, rtc->ice_port, ELAPSED(rtc->rtc_starttime, av_gettime()));

end:
    avio_context_free(&pb);
    return ret;
}

/**
 * Creates and marshals an ICE binding request packet.
 *
 * This function creates and marshals an ICE binding request packet. The function only
 * generates the username attribute and does not include goog-network-info, ice-controlling,
 * use-candidate, and priority. However, some of these attributes may be added in the future.
 *
 * @param s Pointer to the AVFormatContext
 * @param buf Pointer to memory buffer to store the request packet
 * @param buf_size Size of the memory buffer
 * @param request_size Pointer to an integer that receives the size of the request packet
 * @return Returns 0 if successful or AVERROR_xxx if an error occurs.
 */
static int ice_create_request(AVFormatContext *s, uint8_t *buf, int buf_size, int *request_size)
{
    int ret, size, crc32;
    char username[128];
    AVIOContext *pb = NULL;
    AVHMAC *hmac = NULL;
    RTCContext *rtc = s->priv_data;

    pb = avio_alloc_context(buf, buf_size, 1, NULL, NULL, NULL, NULL);
    if (!pb)
        return AVERROR(ENOMEM);

    hmac = av_hmac_alloc(AV_HMAC_SHA1);
    if (!hmac) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* Write 20 bytes header */
    avio_wb16(pb, 0x0001); /* STUN binding request */
    avio_wb16(pb, 0);      /* length */
    avio_wb32(pb, STUN_MAGIC_COOKIE); /* magic cookie */
    avio_wb32(pb, av_get_random_seed()); /* transaction ID */
    avio_wb32(pb, av_get_random_seed()); /* transaction ID */
    avio_wb32(pb, av_get_random_seed()); /* transaction ID */

    /* The username is the concatenation of the two ICE ufrag */
    ret = snprintf(username, sizeof(username), "%s:%s", rtc->ice_ufrag_remote, rtc->ice_ufrag_local);
    if (ret <= 0 || ret >= sizeof(username)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to build username %s:%s, max=%lu, ret=%d\n",
            rtc->ice_ufrag_remote, rtc->ice_ufrag_local, sizeof(username), ret);
        ret = AVERROR(EIO);
        goto end;
    }

    /* Write the username attribute */
    avio_wb16(pb, STUN_ATTR_USERNAME); /* attribute type username */
    avio_wb16(pb, ret); /* size of username */
    avio_write(pb, username, ret); /* bytes of username */
    ffio_fill(pb, 0, (4 - (ret % 4)) % 4); /* padding */

    /* Write the use-candidate attribute */
    avio_wb16(pb, STUN_ATTR_USE_CANDIDATE); /* attribute type use-candidate */
    avio_wb16(pb, 0); /* size of use-candidate */

    /* Build and update message integrity */
    avio_wb16(pb, STUN_ATTR_MESSAGE_INTEGRITY); /* attribute type message integrity */
    avio_wb16(pb, 20); /* size of message integrity */
    ffio_fill(pb, 0, 20); /* fill with zero to directly write and skip it */
    size = avio_tell(pb);
    buf[2] = (size - 20) >> 8;
    buf[3] = (size - 20) & 0xFF;
    av_hmac_init(hmac, rtc->ice_pwd_remote, strlen(rtc->ice_pwd_remote));
    av_hmac_update(hmac, buf, size - 24);
    av_hmac_final(hmac, buf + size - 20, 20);

    /* Write the fingerprint attribute */
    avio_wb16(pb, STUN_ATTR_FINGERPRINT); /* attribute type fingerprint */
    avio_wb16(pb, 4); /* size of fingerprint */
    ffio_fill(pb, 0, 4); /* fill with zero to directly write and skip it */
    size = avio_tell(pb);
    buf[2] = (size - 20) >> 8;
    buf[3] = (size - 20) & 0xFF;
    /* Refer to the av_hash_alloc("CRC32"), av_hash_init and av_hash_final */
    crc32 = av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), 0xFFFFFFFF, buf, size - 8) ^ 0xFFFFFFFF;
    avio_skip(pb, -4);
    avio_wb32(pb, crc32 ^ 0x5354554E); /* xor with "STUN" */

    *request_size = size;

end:
    avio_context_free(&pb);
    av_hmac_free(hmac);
    return ret;
}

/**
 * Create an ICE binding response.
 *
 * This function generates an ICE binding response and writes it to the provided
 * buffer. The response is signed using the local password for message integrity.
 *
 * @param s Pointer to the AVFormatContext structure.
 * @param tid Pointer to the transaction ID of the binding request. The tid_size should be 12.
 * @param tid_size The size of the transaction ID, should be 12.
 * @param buf Pointer to the buffer where the response will be written.
 * @param buf_size The size of the buffer provided for the response.
 * @param response_size Pointer to an integer that will store the size of the generated response.
 * @return Returns 0 if successful or AVERROR_xxx if an error occurs.
 */
static int ice_create_response(AVFormatContext *s, char *tid, int tid_size, uint8_t *buf, int buf_size, int *response_size) {
    int ret = 0, size, crc32;
    AVIOContext *pb = NULL;
    AVHMAC *hmac = NULL;
    RTCContext *rtc = s->priv_data;

    if (tid_size != 12) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Invalid transaction ID size. Expected 12, got %d\n", tid_size);
        return AVERROR(EINVAL);
    }

    pb = avio_alloc_context(buf, buf_size, 1, NULL, NULL, NULL, NULL);
    if (!pb)
        return AVERROR(ENOMEM);

    hmac = av_hmac_alloc(AV_HMAC_SHA1);
    if (!hmac) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* Write 20 bytes header */
    avio_wb16(pb, 0x0101); /* STUN binding response */
    avio_wb16(pb, 0);      /* length */
    avio_wb32(pb, STUN_MAGIC_COOKIE); /* magic cookie */
    avio_write(pb, tid, tid_size); /* transaction ID */

    /* Build and update message integrity */
    avio_wb16(pb, STUN_ATTR_MESSAGE_INTEGRITY); /* attribute type message integrity */
    avio_wb16(pb, 20); /* size of message integrity */
    ffio_fill(pb, 0, 20); /* fill with zero to directly write and skip it */
    size = avio_tell(pb);
    buf[2] = (size - 20) >> 8;
    buf[3] = (size - 20) & 0xFF;
    av_hmac_init(hmac, rtc->ice_pwd_local, strlen(rtc->ice_pwd_local));
    av_hmac_update(hmac, buf, size - 24);
    av_hmac_final(hmac, buf + size - 20, 20);

    /* Write the fingerprint attribute */
    avio_wb16(pb, STUN_ATTR_FINGERPRINT); /* attribute type fingerprint */
    avio_wb16(pb, 4); /* size of fingerprint */
    ffio_fill(pb, 0, 4); /* fill with zero to directly write and skip it */
    size = avio_tell(pb);
    buf[2] = (size - 20) >> 8;
    buf[3] = (size - 20) & 0xFF;
    /* Refer to the av_hash_alloc("CRC32"), av_hash_init and av_hash_final */
    crc32 = av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), 0xFFFFFFFF, buf, size - 8) ^ 0xFFFFFFFF;
    avio_skip(pb, -4);
    avio_wb32(pb, crc32 ^ 0x5354554E); /* xor with "STUN" */

    *response_size = size;

end:
    avio_context_free(&pb);
    av_hmac_free(hmac);
    return ret;
}

static int ice_is_binding_request(char *buf, int buf_size) {
    return buf_size > 1 && buf[0] == 0x00 && buf[1] == 0x01;
}

static int ice_is_binding_response(char *buf, int buf_size) {
    return buf_size > 1 && buf[0] == 0x01 && buf[1] == 0x01;
}

/**
 * This function handles incoming binding request messages by responding to them.
 * If the message is not a binding request, it will be ignored.
 */
static int ice_handle_binding_request(AVFormatContext *s, char *buf, int buf_size) {
    int ret = 0, size;
    char tid[12];
    RTCContext *rtc = s->priv_data;

    /* Ignore if not a binding request. */
    if (!ice_is_binding_request(buf, buf_size))
        return ret;

    if (buf_size < 20) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Invalid STUN message size. Expected at least 20, got %d\n", buf_size);
        return AVERROR(EINVAL);
    }

    /* Parse transaction id from binding request in buf. */
    memcpy(tid, buf + 8, 12);

    /* Build the STUN binding response. */
    ret = ice_create_response(s, tid, sizeof(tid), rtc->buf, sizeof(rtc->buf), &size);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to create STUN binding response, size=%d\n", size);
        return ret;
    }

    ret = ffurl_write(rtc->udp_uc, rtc->buf, size);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to send STUN binding response, size=%d\n", size);
        return ret;
    }

    return 0;
}

/**
 * To establish a connection with the UDP server, we utilize ICE-LITE in a Client-Server
 * mode. In this setup, FFmpeg acts as the UDP client, while the peer functions as the
 * UDP server.
 */
static int udp_connect(AVFormatContext *s)
{
    int ret = 0;
    char url[256], tmp[16];
    RTCContext *rtc = s->priv_data;

    /* Build UDP URL and create the UDP context as transport. */
    ff_url_join(url, sizeof(url), "udp", NULL, rtc->ice_host, rtc->ice_port, NULL);
    ret = ffurl_alloc(&rtc->udp_uc, url, AVIO_FLAG_WRITE, &s->interrupt_callback);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to open udp://%s:%d\n", rtc->ice_host, rtc->ice_port);
        return ret;
    }

    av_opt_set(rtc->udp_uc->priv_data, "connect", "1", 0);
    av_opt_set(rtc->udp_uc->priv_data, "fifo_size", "0", 0);
    /* Set the max packet size to the buffer size. */
    snprintf(tmp, sizeof(tmp), "%d", rtc->pkt_size);
    av_opt_set(rtc->udp_uc->priv_data, "pkt_size", tmp, 0);

    ret = ffurl_connect(rtc->udp_uc, NULL);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to connect udp://%s:%d\n", rtc->ice_host, rtc->ice_port);
        return ret;
    }

    /* Make the socket non-blocking, set to READ and WRITE mode after connected */
    ff_socket_nonblock(ffurl_get_file_handle(rtc->udp_uc), 1);
    rtc->udp_uc->flags |= AVIO_FLAG_READ | AVIO_FLAG_NONBLOCK;

    if (rtc->state < RTC_STATE_UDP_CONNECTED)
        rtc->state = RTC_STATE_UDP_CONNECTED;
    rtc->rtc_udp_time = av_gettime();
    av_log(rtc, AV_LOG_VERBOSE, "WHIP: UDP state=%d, elapsed=%dms, connected to udp://%s:%d\n",
        rtc->state, ELAPSED(rtc->rtc_starttime, av_gettime()), rtc->ice_host, rtc->ice_port);

    return ret;
}

static int ice_dtls_handshake(AVFormatContext *s)
{
    int ret = 0, size, i;
    int64_t starttime = av_gettime(), now;
    RTCContext *rtc = s->priv_data;

    if (rtc->state < RTC_STATE_UDP_CONNECTED || !rtc->udp_uc) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: UDP not connected, state=%d, udp_uc=%p\n", rtc->state, rtc->udp_uc);
        return AVERROR(EINVAL);
    }

    while (1) {
        if (rtc->state <= RTC_STATE_ICE_CONNECTING) {
            /* Build the STUN binding request. */
            ret = ice_create_request(s, rtc->buf, sizeof(rtc->buf), &size);
            if (ret < 0) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to create STUN binding request, size=%d\n", size);
                goto end;
            }

            ret = ffurl_write(rtc->udp_uc, rtc->buf, size);
            if (ret < 0) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to send STUN binding request, size=%d\n", size);
                goto end;
            }

            if (rtc->state < RTC_STATE_ICE_CONNECTING)
                rtc->state = RTC_STATE_ICE_CONNECTING;
        }

next_packet:
        if (rtc->state >= RTC_STATE_DTLS_FINISHED)
            /* DTLS handshake is done, exit the loop. */
            break;

        now = av_gettime();
        if (now - starttime >= rtc->handshake_timeout * 1000) {
            av_log(rtc, AV_LOG_ERROR, "WHIP: DTLS handshake timeout=%dms, cost=%dms, elapsed=%dms, state=%d\n",
                rtc->handshake_timeout, ELAPSED(starttime, now), ELAPSED(rtc->rtc_starttime, now), rtc->state);
            ret = AVERROR(ETIMEDOUT);
            goto end;
        }

        /* Read the STUN or DTLS messages from peer. */
        for (i = 0; i < ICE_DTLS_READ_INTERVAL / 5; i++) {
            ret = ffurl_read(rtc->udp_uc, rtc->buf, sizeof(rtc->buf));
            if (ret > 0)
                break;
            if (ret == AVERROR(EAGAIN)) {
                av_usleep(5 * 1000);
                continue;
            }
            av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to read message\n");
            goto end;
        }

        /* Got nothing, continue to process handshake. */
        if (ret <= 0)
            continue;

        /* Handle the ICE binding response. */
        if (ice_is_binding_response(rtc->buf, ret)) {
            if (rtc->state < RTC_STATE_ICE_CONNECTED) {
                rtc->state = RTC_STATE_ICE_CONNECTED;
                rtc->rtc_ice_time = av_gettime();
                av_log(rtc, AV_LOG_INFO, "WHIP: ICE STUN ok, state=%d, url=udp://%s:%d, location=%s, username=%s:%s, res=%dB, elapsed=%dms\n",
                    rtc->state, rtc->ice_host, rtc->ice_port, rtc->whip_resource_url ? rtc->whip_resource_url : "",
                    rtc->ice_ufrag_remote, rtc->ice_ufrag_local, ret, ELAPSED(rtc->rtc_starttime, av_gettime()));

                /* If got the first binding response, start DTLS handshake. */
                if ((ret = dtls_context_start(&rtc->dtls_ctx)) < 0)
                    goto end;
            }
            goto next_packet;
        }

        /* When a binding request is received, it is necessary to respond immediately. */
        if (ice_is_binding_request(rtc->buf, ret)) {
            if ((ret = ice_handle_binding_request(s, rtc->buf, ret)) < 0)
                goto end;
            goto next_packet;
        }

        /* If got any DTLS messages, handle it. */
        if (is_dtls_packet(rtc->buf, ret) && rtc->state >= RTC_STATE_ICE_CONNECTED) {
            if ((ret = dtls_context_write(&rtc->dtls_ctx, rtc->buf, ret)) < 0)
                goto end;
            goto next_packet;
        }
    }

end:
    return ret;
}

/**
 * Establish the SRTP context using the keying material exported from DTLS.
 *
 * Create separate SRTP contexts for sending video and audio, as their sequences differ
 * and should not share a single context. Generate a single SRTP context for receiving
 * RTCP only.
 *
 * @return 0 if OK, AVERROR_xxx on error
 */
static int setup_srtp(AVFormatContext *s)
{
    int ret;
    char recv_key[DTLS_SRTP_KEY_LEN + DTLS_SRTP_SALT_LEN];
    char send_key[DTLS_SRTP_KEY_LEN + DTLS_SRTP_SALT_LEN];
    char buf[AV_BASE64_SIZE(DTLS_SRTP_KEY_LEN + DTLS_SRTP_SALT_LEN)];
    const char* suite = "AES_CM_128_HMAC_SHA1_80";
    RTCContext *rtc = s->priv_data;

    /**
     * This represents the material used to build the SRTP master key. It is
     * generated by DTLS and has the following layout:
     *          16B         16B         14B             14B
     *      client_key | server_key | client_salt | server_salt
     */
    char *client_key = rtc->dtls_ctx.dtls_srtp_materials;
    char *server_key = rtc->dtls_ctx.dtls_srtp_materials + DTLS_SRTP_KEY_LEN;
    char *client_salt = server_key + DTLS_SRTP_KEY_LEN;
    char *server_salt = client_salt + DTLS_SRTP_SALT_LEN;

    /* As DTLS server, the recv key is client master key plus salt. */
    memcpy(recv_key, client_key, DTLS_SRTP_KEY_LEN);
    memcpy(recv_key + DTLS_SRTP_KEY_LEN, client_salt, DTLS_SRTP_SALT_LEN);

    /* As DTLS server, the send key is server master key plus salt. */
    memcpy(send_key, server_key, DTLS_SRTP_KEY_LEN);
    memcpy(send_key + DTLS_SRTP_KEY_LEN, server_salt, DTLS_SRTP_SALT_LEN);

    /* Setup SRTP context for outgoing packets */
    if (!av_base64_encode(buf, sizeof(buf), send_key, sizeof(send_key))) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to encode send key\n");
        ret = AVERROR(EIO);
        goto end;
    }

    ret = ff_srtp_set_crypto(&rtc->srtp_audio_send, suite, buf);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to set crypto for audio send\n");
        goto end;
    }

    ret = ff_srtp_set_crypto(&rtc->srtp_video_send, suite, buf);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to set crypto for video send\n");
        goto end;
    }

    ret = ff_srtp_set_crypto(&rtc->srtp_rtcp_send, suite, buf);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "Failed to set crypto for rtcp send\n");
        goto end;
    }

    /* Setup SRTP context for incoming packets */
    if (!av_base64_encode(buf, sizeof(buf), recv_key, sizeof(recv_key))) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to encode recv key\n");
        ret = AVERROR(EIO);
        goto end;
    }

    ret = ff_srtp_set_crypto(&rtc->srtp_recv, suite, buf);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to set crypto for recv\n");
        goto end;
    }

    if (rtc->state < RTC_STATE_SRTP_FINISHED)
        rtc->state = RTC_STATE_SRTP_FINISHED;
    rtc->rtc_srtp_time = av_gettime();
    av_log(rtc, AV_LOG_INFO, "WHIP: SRTP setup done, state=%d, suite=%s, key=%luB, elapsed=%dms\n",
        rtc->state, suite, sizeof(send_key), ELAPSED(rtc->rtc_starttime, av_gettime()));

end:
    return ret;
}

static int on_rtp_write_packet(void *opaque, uint8_t *buf, int buf_size);

/**
 * Creates dedicated RTP muxers for each stream in the AVFormatContext to build RTP
 * packets from the encoded frames.
 *
 * The corresponding SRTP context is utilized to encrypt each stream's RTP packets. For
 * example, a video SRTP context is used for the video stream. Additionally, the
 * "on_rtp_write_packet" callback function is set as the write function for each RTP
 * muxer to send out encrypted RTP packets.
 *
 * @return 0 if OK, AVERROR_xxx on error
 */
static int create_rtp_muxer(AVFormatContext *s)
{
    int ret, i, is_video, buffer_size, max_packet_size;
    AVFormatContext *rtp_ctx = NULL;
    AVDictionary *opts = NULL;
    uint8_t *buffer = NULL;
    char buf[64];
    RTCContext *rtc = s->priv_data;

    const AVOutputFormat *rtp_format = av_guess_format("rtp", NULL, NULL);
    if (!rtp_format) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to guess rtp muxer\n");
        ret = AVERROR(ENOSYS);
        goto end;
    }

    /* The UDP buffer size, may greater than MTU. */
    buffer_size = MAX_UDP_BUFFER_SIZE;
    /* The RTP payload max size. Reserved some bytes for SRTP checksum and padding. */
    max_packet_size = rtc->pkt_size - DTLS_SRTP_CHECKSUM_LEN;

    for (i = 0; i < s->nb_streams; i++) {
        rtp_ctx = avformat_alloc_context();
        if (!rtp_ctx) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        rtp_ctx->oformat = rtp_format;
        if (!avformat_new_stream(rtp_ctx, NULL)) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        /* Pass the interrupt callback on */
        rtp_ctx->interrupt_callback = s->interrupt_callback;
        /* Copy the max delay setting; the rtp muxer reads this. */
        rtp_ctx->max_delay = s->max_delay;
        /* Copy other stream parameters. */
        rtp_ctx->streams[0]->sample_aspect_ratio = s->streams[i]->sample_aspect_ratio;
        rtp_ctx->flags |= s->flags & AVFMT_FLAG_BITEXACT;
        rtp_ctx->strict_std_compliance = s->strict_std_compliance;

        /* Set the synchronized start time. */
        rtp_ctx->start_time_realtime = s->start_time_realtime;

        avcodec_parameters_copy(rtp_ctx->streams[0]->codecpar, s->streams[i]->codecpar);
        rtp_ctx->streams[0]->time_base = s->streams[i]->time_base;

        buffer = av_malloc(buffer_size);
        if (!buffer) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        rtp_ctx->pb = avio_alloc_context(buffer, buffer_size, 1, s, NULL, on_rtp_write_packet, NULL);
        if (!rtp_ctx->pb) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        rtp_ctx->pb->max_packet_size = max_packet_size;
        rtp_ctx->pb->av_class = &ff_avio_class;

        is_video = s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        snprintf(buf, sizeof(buf), "%d", is_video? rtc->video_payload_type : rtc->audio_payload_type);
        av_dict_set(&opts, "payload_type", buf, 0);
        snprintf(buf, sizeof(buf), "%d", is_video? rtc->video_ssrc : rtc->audio_ssrc);
        av_dict_set(&opts, "ssrc", buf, 0);

        ret = avformat_write_header(rtp_ctx, &opts);
        if (ret < 0) {
            av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to write rtp header\n");
            goto end;
        }

        ff_format_set_url(rtp_ctx, av_strdup(s->url));
        s->streams[i]->time_base = rtp_ctx->streams[0]->time_base;
        s->streams[i]->priv_data = rtp_ctx;
        rtp_ctx = NULL;
    }

    if (rtc->state < RTC_STATE_READY)
        rtc->state = RTC_STATE_READY;
    rtc->rtc_ready_time = av_gettime();
    av_log(rtc, AV_LOG_INFO, "WHIP: Muxer is ready, state=%d, buffer_size=%d, max_packet_size=%d, "
                           "elapsed=%dms(init:%d,offer:%d,answer:%d,udp:%d,ice:%d,dtls:%d,srtp:%d,ready:%d)\n",
        rtc->state, buffer_size, max_packet_size, ELAPSED(rtc->rtc_starttime, av_gettime()),
        ELAPSED(rtc->rtc_starttime, rtc->rtc_init_time),
        ELAPSED(rtc->rtc_init_time, rtc->rtc_offer_time),
        ELAPSED(rtc->rtc_offer_time, rtc->rtc_answer_time),
        ELAPSED(rtc->rtc_answer_time, rtc->rtc_udp_time),
        ELAPSED(rtc->rtc_udp_time, rtc->rtc_ice_time),
        ELAPSED(rtc->rtc_ice_time, rtc->rtc_dtls_time),
        ELAPSED(rtc->rtc_dtls_time, rtc->rtc_srtp_time),
        ELAPSED(rtc->rtc_srtp_time, rtc->rtc_ready_time));

end:
    if (rtp_ctx)
        avio_context_free(&rtp_ctx->pb);
    avformat_free_context(rtp_ctx);
    av_dict_free(&opts);
    return ret;
}

/**
 * Callback triggered by the RTP muxer when it creates and sends out an RTP packet.
 *
 * This function modifies the video STAP packet, removing the markers, and updating the
 * NRI of the first NALU. Additionally, it uses the corresponding SRTP context to encrypt
 * the RTP packet, where the video packet is handled by the video SRTP context.
 */
static int on_rtp_write_packet(void *opaque, uint8_t *buf, int buf_size)
{
    int ret, cipher_size, is_rtcp, is_video;
    uint8_t payload_type, nalu_header;
    AVFormatContext *s = opaque;
    RTCContext *rtc = s->priv_data;
    struct SRTPContext *srtp;

    /* Ignore if not RTP or RTCP packet. */
    if (buf_size < 12 || (buf[0] & 0xC0) != 0x80)
        return 0;

    /* Only support audio, video and rtcp. */
    is_rtcp = buf[1] >= 192 && buf[1] <= 223;
    payload_type = buf[1] & 0x7f;
    is_video = payload_type == rtc->video_payload_type;
    if (!is_rtcp && payload_type != rtc->video_payload_type && payload_type != rtc->audio_payload_type)
        return 0;

    /**
     * For video, the STAP-A with SPS/PPS should:
     * 1. The marker bit should be 0, never be 1.
     * 2. The NRI should equal to the first NALU's.
     * TODO: FIXME: Should fix it in rtpenc.c
     */
    if (is_video && buf_size > 12) {
        nalu_header = buf[12] & 0x1f;
        if (nalu_header == NALU_TYPE_STAP_A) {
            /* Reset the marker bit to 0. */
            if (buf[1] & 0x80)
                buf[1] &= 0x7f;

            /* Reset the NRI to the first NALU's NRI. */
            if (buf_size > 15 && (buf[15]&0x60) != (buf[12]&0x60))
                buf[12] = (buf[12]&0x80) | (buf[15]&0x60) | (buf[12]&0x1f);
        }
    }

    /* Get the corresponding SRTP context. */
    srtp = is_rtcp ? &rtc->srtp_rtcp_send : (is_video? &rtc->srtp_video_send : &rtc->srtp_audio_send);

    /* Encrypt by SRTP and send out. */
    cipher_size = ff_srtp_encrypt(srtp, buf, buf_size, rtc->buf, sizeof(rtc->buf));
    if (cipher_size <= 0 || cipher_size < buf_size) {
        av_log(rtc, AV_LOG_WARNING, "WHIP: Failed to encrypt packet=%dB, cipher=%dB\n", buf_size, cipher_size);
        return 0;
    }

    ret = ffurl_write(rtc->udp_uc, rtc->buf, cipher_size);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to write packet=%dB, ret=%d\n", cipher_size, ret);
        return ret;
    }

    return ret;
}

/**
 * Inserts the SPS/PPS data before each IDR (Instantaneous Decoder Refresh) frame.
 *
 * The SPS/PPS is parsed from the extradata. If it's in ISOM format, the SPS/PPS is
 * multiplexed to the data field of the packet. If it's in annexb format, then the entire
 * extradata is set to the data field of the packet.
 */
static int insert_sps_pps_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret, is_idr, size, i;
    uint8_t *p;
    AVPacket* extra = NULL;
    AVStream *st = s->streams[pkt->stream_index];
    AVFormatContext *rtp_ctx = st->priv_data;
    RTCContext *rtc = s->priv_data;

    is_idr = (pkt->flags & AV_PKT_FLAG_KEY) && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    if (!is_idr || !st->codecpar->extradata)
        return 0;

    extra = av_packet_alloc();
    if (!extra)
        return AVERROR(ENOMEM);

    size = !rtc->avc_nal_length_size ? st->codecpar->extradata_size :
            rtc->avc_nal_length_size * 2 + rtc->avc_sps_size + rtc->avc_pps_size;
    ret = av_new_packet(extra, size);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to allocate extra packet\n");
        goto end;
    }

    /* Encode SPS/PPS in annexb format. */
    if (!rtc->avc_nal_length_size) {
        memcpy(extra->data, st->codecpar->extradata, size);
    } else {
        /* Encode SPS/PPS in ISOM format. */
        p = extra->data;
        for (i = 0; i < rtc->avc_nal_length_size; i++) {
            *p++ = rtc->avc_sps_size >> (8 * (rtc->avc_nal_length_size - i - 1));
        }
        memcpy(p, rtc->avc_sps, rtc->avc_sps_size);
        p += rtc->avc_sps_size;

        /* Encode PPS in ISOM format. */
        for (i = 0; i < rtc->avc_nal_length_size; i++) {
            *p++ = rtc->avc_pps_size >> (8 * (rtc->avc_nal_length_size - i - 1));
        }
        memcpy(p, rtc->avc_pps, rtc->avc_pps_size);
        p += rtc->avc_pps_size;
    }

    /* Setup packet and feed it to chain. */
    extra->pts = pkt->pts;
    extra->dts = pkt->dts;
    extra->stream_index = pkt->stream_index;
    extra->time_base = pkt->time_base;

    ret = ff_write_chained(rtp_ctx, 0, extra, s, 0);
    if (ret < 0)
        goto end;

end:
    av_packet_free(&extra);
    return ret;
}

/**
 * RTC is connectionless, for it's based on UDP, so it check whether sesison is
 * timeout. In such case, publishers can't republish the stream util the session
 * is timeout.
 * This function is called to notify the server that the stream is ended, server
 * should expire and close the session immediately, so that publishers can republish
 * the stream quickly.
 */
static int whip_dispose(AVFormatContext *s)
{
    int ret;
    char buf[MAX_URL_SIZE];
    URLContext *whip_uc = NULL;
    RTCContext *rtc = s->priv_data;

    if (!rtc->whip_resource_url)
        return 0;

    ret = ffurl_alloc(&whip_uc, rtc->whip_resource_url, AVIO_FLAG_READ_WRITE, &s->interrupt_callback);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to alloc WHIP delete context: %s\n", s->url);
        goto end;
    }

    av_opt_set(whip_uc->priv_data, "chunked_post", "0", 0);
    av_opt_set(whip_uc->priv_data, "method", "DELETE", 0);
    ret = ffurl_connect(whip_uc, NULL);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to DELETE url=%s\n", rtc->whip_resource_url);
        goto end;
    }

    while (1) {
        ret = ffurl_read(whip_uc, buf, sizeof(buf));
        if (ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) {
            av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to read response from DELETE url=%s\n", rtc->whip_resource_url);
            goto end;
        }
    }

    av_log(rtc, AV_LOG_INFO, "WHIP: Dispose resource %s ok\n", rtc->whip_resource_url);

end:
    ffurl_closep(&whip_uc);
    return ret;
}

static av_cold int rtc_init(AVFormatContext *s)
{
    int ret;
    RTCContext *rtc = s->priv_data;

    if ((ret = whip_init(s)) < 0)
        goto end;

    if ((ret = parse_codec(s)) < 0)
        goto end;

    if ((ret = generate_sdp_offer(s)) < 0)
        goto end;

    if ((ret = exchange_sdp(s)) < 0)
        goto end;

    if ((ret = parse_answer(s)) < 0)
        goto end;

    if ((ret = udp_connect(s)) < 0)
        goto end;

    if ((ret = ice_dtls_handshake(s)) < 0)
        goto end;

    if ((ret = setup_srtp(s)) < 0)
        goto end;

    if ((ret = create_rtp_muxer(s)) < 0)
        goto end;

end:
    if (ret < 0 && rtc->state < RTC_STATE_FAILED)
        rtc->state = RTC_STATE_FAILED;
    if (ret >= 0 && rtc->state >= RTC_STATE_FAILED && rtc->dtls_ret < 0)
        ret = rtc->dtls_ret;
    return ret;
}

static int rtc_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret;
    RTCContext *rtc = s->priv_data;
    AVStream *st = s->streams[pkt->stream_index];
    AVFormatContext *rtp_ctx = st->priv_data;

    /* TODO: Send binding request every 1s as WebRTC heartbeat. */

    /**
     * Receive packets from the server such as ICE binding requests, DTLS messages,
     * and RTCP like PLI requests, then respond to them.
     */
    ret = ffurl_read(rtc->udp_uc, rtc->buf, sizeof(rtc->buf));
    if (ret > 0) {
        if (is_dtls_packet(rtc->buf, ret)) {
            if ((ret = dtls_context_write(&rtc->dtls_ctx, rtc->buf, ret)) < 0) {
                av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to handle DTLS message\n");
                goto end;
            }
        }
    } else if (ret != AVERROR(EAGAIN)) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to read from UDP socket\n");
        goto end;
    }

    /* For audio OPUS stream, correct the timestamp. */
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        pkt->dts = pkt->pts = rtc->audio_jitter_base;
        // TODO: FIXME: For opus 48khz, each frame is 20ms which is 48000*20/1000 = 960. It appears that there is a
        //  bug introduced by libopus regarding the timestamp. Instead of being exactly 960, there is a slight
        //  deviation, such as 956 or 970. This deviation can cause Chrome to play the audio stream with noise.
        //  Although we are unsure of the root cause, we can simply correct the timestamp by using the timebase of
        //  Opus. We need to conduct further research and remove this line.
        rtc->audio_jitter_base += 960;
    }

    ret = insert_sps_pps_packet(s, pkt);
    if (ret < 0) {
        av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to insert SPS/PPS packet\n");
        goto end;
    }

    ret = ff_write_chained(rtp_ctx, 0, pkt, s, 0);
    if (ret < 0) {
        if (ret == AVERROR(EINVAL)) {
            av_log(rtc, AV_LOG_WARNING, "WHIP: Ignore failed to write packet=%dB, ret=%d\n", pkt->size, ret);
            ret = 0;
        } else
            av_log(rtc, AV_LOG_ERROR, "WHIP: Failed to write packet, size=%d\n", pkt->size);
        goto end;
    }

end:
    if (ret < 0 && rtc->state < RTC_STATE_FAILED)
        rtc->state = RTC_STATE_FAILED;
    if (ret >= 0 && rtc->state >= RTC_STATE_FAILED && rtc->dtls_ret < 0)
        ret = rtc->dtls_ret;
    if (ret >= 0 && rtc->dtls_closed)
        ret = AVERROR(EIO);
    return ret;
}

static av_cold void rtc_deinit(AVFormatContext *s)
{
    int i, ret;
    RTCContext *rtc = s->priv_data;

    ret = whip_dispose(s);
    if (ret < 0)
        av_log(rtc, AV_LOG_WARNING, "WHIP: Failed to dispose resource, ret=%d\n", ret);

    for (i = 0; i < s->nb_streams; i++) {
        AVFormatContext* rtp_ctx = s->streams[i]->priv_data;
        if (!rtp_ctx)
            continue;

        av_write_trailer(rtp_ctx);
        avio_context_free(&rtp_ctx->pb);
        avformat_free_context(rtp_ctx);
        s->streams[i]->priv_data = NULL;
    }

    av_freep(&rtc->avc_sps);
    av_freep(&rtc->avc_pps);
    av_freep(&rtc->sdp_offer);
    av_freep(&rtc->sdp_answer);
    av_freep(&rtc->whip_resource_url);
    av_freep(&rtc->ice_ufrag_remote);
    av_freep(&rtc->ice_pwd_remote);
    av_freep(&rtc->ice_protocol);
    av_freep(&rtc->ice_host);
    av_freep(&rtc->authorization);
    ffurl_closep(&rtc->udp_uc);
    ff_srtp_free(&rtc->srtp_audio_send);
    ff_srtp_free(&rtc->srtp_video_send);
    ff_srtp_free(&rtc->srtp_rtcp_send);
    ff_srtp_free(&rtc->srtp_recv);
    dtls_context_deinit(&rtc->dtls_ctx);
}

#define OFFSET(x) offsetof(RTCContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "handshake_timeout",  "Timeout in milliseconds for ICE and DTLS handshake.",              OFFSET(handshake_timeout),  AV_OPT_TYPE_INT,    { .i64 = 5000 },    -1, INT_MAX, DEC },
    { "pkt_size",           "The maximum size, in bytes, of RTP packets that send out",         OFFSET(pkt_size),           AV_OPT_TYPE_INT,    { .i64 = 1200 },    -1, INT_MAX, DEC },
    { "authorization",      "The optional Bearer token for WHIP Authorization",                 OFFSET(authorization),      AV_OPT_TYPE_STRING, { .str = NULL },     0,       0, DEC },
    { NULL },
};

static const AVClass rtc_muxer_class = {
    .class_name = "WebRTC muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_rtc_muxer = {
    .p.name             = "rtc",
    .p.long_name        = NULL_IF_CONFIG_SMALL("WHIP WebRTC muxer"),
    .p.audio_codec      = AV_CODEC_ID_OPUS,
    .p.video_codec      = AV_CODEC_ID_H264,
    .p.flags            = AVFMT_GLOBALHEADER | AVFMT_NOFILE,
    .p.priv_class       = &rtc_muxer_class,
    .priv_data_size     = sizeof(RTCContext),
    .init               = rtc_init,
    .write_packet       = rtc_write_packet,
    .deinit             = rtc_deinit,
};
