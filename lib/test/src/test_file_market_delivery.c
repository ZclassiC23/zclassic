/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * zfileget.v1 session binding and authorize-before-read delivery proofs. */

#include "test/test_core.h"

#include "chain/chainparams.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"
#include "net/file_market_delivery.h"
#include "net/file_service.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <pthread.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DELIVERY_CHECK(label, condition) do {                        \
    printf("file_market delivery: %s... ", (label));                \
    if (condition) printf("OK\n");                                  \
    else { printf("FAIL\n"); failures++; }                         \
} while (0)

struct delivery_fixture {
    enum file_market_delivery_authorization authorization;
    int authorize_calls;
    int load_calls;
    bool load_ok;
    bool corrupt_hash;
};

static enum file_market_delivery_authorization delivery_authorize(
    const uint8_t offer_id[32], const uint8_t buyer_pubkey[32],
    uint32_t chunk_index, void *ctx)
{
    struct delivery_fixture *fixture = ctx;
    fixture->authorize_calls++;
    if (!offer_id || !buyer_pubkey || chunk_index != 7)
        return FILE_MARKET_DELIVERY_REJECTED;
    return fixture->authorization;
}

static bool delivery_load(
    const uint8_t offer_id[32], uint32_t chunk_index,
    struct file_market_delivery_chunk *out, void *ctx)
{
    struct delivery_fixture *fixture = ctx;
    static const uint8_t payload[] = "paid-chunk-proof";
    fixture->load_calls++;
    if (!fixture->load_ok || !offer_id || chunk_index != 7 || !out)
        return false;
    out->data = zcl_malloc(sizeof(payload), "delivery_test_chunk");
    if (!out->data)
        return false;
    memcpy(out->data, payload, sizeof(payload));
    out->size = sizeof(payload);
    sha3_256(out->data, out->size, out->sha3);
    if (fixture->corrupt_hash)
        out->sha3[0] ^= 1;
    return true;
}

static bool delivery_request_fixture(
    struct fs_session *server,
    struct file_market_delivery_request *request,
    uint8_t wire[FILE_MARKET_DELIVERY_WIRE_BYTES], uint8_t buyer_seed[32])
{
    const struct chain_params *params = chain_params_get();
    uint8_t secret[32];
    if (!params)
        return false;
    memset(server, 0, sizeof(*server));
    memset(server->peer_nonce, 0x31, sizeof(server->peer_nonce));
    memset(server->our_nonce, 0x42, sizeof(server->our_nonce));
    memset(request, 0, sizeof(*request));
    memset(buyer_seed, 0x53, 32);
    request->version = FILE_MARKET_DELIVERY_VERSION;
    memcpy(request->network_genesis,
           params->consensus.hashGenesisBlock.data, 32);
    memset(request->offer_id, 0x64, sizeof(request->offer_id));
    request->chunk_index = 7;
    ed25519_keypair(request->buyer_pubkey, secret, buyer_seed);
    file_market_delivery_session_id(
        request->network_genesis, server->peer_nonce, server->our_nonce,
        request->session_id);
    return file_market_delivery_request_seal(request, buyer_seed) ==
               FILE_MARKET_DELIVERY_OK &&
           file_market_delivery_request_encode(request, wire) ==
               FILE_MARKET_DELIVERY_OK;
}

static bool delivery_recv_exact(int fd, uint8_t *out, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, out + got, len - got, 0);
        if (n <= 0)
            return false;
        got += (size_t)n;
    }
    return true;
}

struct delivery_server_call {
    struct fs_session *session;
    bool served;
};

static void *delivery_server_call_main(void *opaque)
{
    struct delivery_server_call *call = opaque;
    uint8_t type = 0;
    const uint8_t *payload = NULL;
    uint32_t payload_len = 0;
    uint8_t client_ip[16] = {0};
    call->served = fs_recv_frame(call->session, &type, &payload,
                                 &payload_len) &&
        type == FS_REQUEST && file_market_delivery_serve(
            call->session, client_ip, payload, payload_len);
    return NULL;
}

struct delivery_endpoint_server {
    int listen_fd;
    bool served;
};

static void *delivery_endpoint_server_main(void *opaque)
{
    struct delivery_endpoint_server *server = opaque;
    int fd = accept(server->listen_fd, NULL, NULL);
    if (fd < 0)
        return NULL;
    struct fs_session session;
    fs_session_init(&session, fd);
    uint8_t transport_root[32] = {0};
    uint8_t type = 0;
    const uint8_t *payload = NULL;
    uint32_t payload_len = 0;
    uint8_t client_ip[16] = {0};
    server->served = fs_handshake(&session, transport_root, false) &&
        fs_recv_frame(&session, &type, &payload, &payload_len) &&
        type == FS_REQUEST && file_market_delivery_serve(
            &session, client_ip, payload, payload_len);
    close(fd);
    return NULL;
}

int file_market_delivery_tests(void)
{
    int failures = 0;
    struct fs_session server;
    struct file_market_delivery_request request, decoded;
    uint8_t wire[FILE_MARKET_DELIVERY_WIRE_BYTES], buyer_seed[32];
    bool made = delivery_request_fixture(&server, &request, wire, buyer_seed);
    DELIVERY_CHECK("session-bound signed request fixture", made);
    if (!made)
        return failures;

    uint8_t expected_session[32];
    file_market_delivery_session_id(
        request.network_genesis, server.peer_nonce, server.our_nonce,
        expected_session);
    bool codec = file_market_delivery_request_decode(
                     wire, sizeof(wire), &decoded) ==
                     FILE_MARKET_DELIVERY_OK &&
                 file_market_delivery_request_verify(
                     &decoded, request.network_genesis, expected_session) ==
                     FILE_MARKET_DELIVERY_OK;
    DELIVERY_CHECK("fixed request codec and buyer signature", codec);

    uint8_t other_session[32];
    memcpy(other_session, expected_session, 32);
    other_session[0] ^= 1;
    DELIVERY_CHECK("request cannot move to another encrypted session",
        file_market_delivery_request_verify(
            &decoded, request.network_genesis, other_session) ==
        FILE_MARKET_DELIVERY_ERR_SESSION);
    struct file_market_delivery_request tampered = decoded;
    tampered.chunk_index++;
    DELIVERY_CHECK("changed chunk fails buyer authentication",
        file_market_delivery_request_verify(
            &tampered, request.network_genesis, expected_session) ==
        FILE_MARKET_DELIVERY_ERR_SIGNATURE);

    struct delivery_fixture fixture = {
        .authorization = FILE_MARKET_DELIVERY_PENDING,
        .load_ok = true,
    };
    file_market_delivery_set_handlers(
        request.network_genesis, delivery_authorize, delivery_load, &fixture);
    struct file_market_delivery_reply reply, reply_roundtrip;
    struct file_market_delivery_chunk chunk;
    enum file_market_delivery_status status = file_market_delivery_prepare(
        &server, wire, sizeof(wire), &reply, &chunk);
    DELIVERY_CHECK("pending payment never invokes content reader",
        status == FILE_MARKET_DELIVERY_PAYMENT_PENDING &&
        fixture.authorize_calls == 1 && fixture.load_calls == 0 &&
        chunk.data == NULL);

    fixture.authorization = FILE_MARKET_DELIVERY_UNKNOWN;
    status = file_market_delivery_prepare(
        &server, wire, sizeof(wire), &reply, &chunk);
    DELIVERY_CHECK("unknown payment never invokes content reader",
        status == FILE_MARKET_DELIVERY_PAYMENT_UNKNOWN &&
        fixture.load_calls == 0 && chunk.data == NULL);
    fixture.authorization = FILE_MARKET_DELIVERY_CONFLICTED;
    status = file_market_delivery_prepare(
        &server, wire, sizeof(wire), &reply, &chunk);
    DELIVERY_CHECK("conflicted payment never invokes content reader",
        status == FILE_MARKET_DELIVERY_PAYMENT_CONFLICTED &&
        fixture.load_calls == 0 && chunk.data == NULL);

    uint8_t tampered_wire[FILE_MARKET_DELIVERY_WIRE_BYTES];
    memcpy(tampered_wire, wire, sizeof(tampered_wire));
    tampered_wire[70] ^= 1;
    int auth_before = fixture.authorize_calls;
    status = file_market_delivery_prepare(
        &server, tampered_wire, sizeof(tampered_wire), &reply, &chunk);
    DELIVERY_CHECK("unauthenticated request reaches neither app callback",
        status == FILE_MARKET_DELIVERY_UNAUTHENTICATED &&
        fixture.authorize_calls == auth_before && fixture.load_calls == 0);

    fixture.authorization = FILE_MARKET_DELIVERY_AUTHORIZED;
    status = file_market_delivery_prepare(
        &server, wire, sizeof(wire), &reply, &chunk);
    bool ready = status == FILE_MARKET_DELIVERY_READY && chunk.data &&
                 fixture.load_calls == 1 && reply.size == chunk.size &&
                 memcmp(reply.sha3, chunk.sha3, 32) == 0 &&
                 file_market_delivery_reply_encode(&reply, tampered_wire) &&
                 file_market_delivery_reply_decode(
                     tampered_wire, FILE_MARKET_DELIVERY_REPLY_BYTES,
                     &reply_roundtrip) &&
                 reply_roundtrip.status == FILE_MARKET_DELIVERY_READY;
    DELIVERY_CHECK("confirmed payment loads one exact typed chunk", ready);
    free(chunk.data);

    fixture.corrupt_hash = true;
    status = file_market_delivery_prepare(
        &server, wire, sizeof(wire), &reply, &chunk);
    DELIVERY_CHECK("content hash mismatch fails closed before send",
        status == FILE_MARKET_DELIVERY_CONTENT_UNAVAILABLE &&
        chunk.data == NULL);
    fixture.corrupt_hash = false;

    int sockets[2] = {-1, -1};
    bool socket_ready = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0;
    struct fs_session client;
    if (socket_ready) {
        fs_session_init(&server, sockets[0]);
        fs_session_init(&client, sockets[1]);
        memset(server.key, 0x75, sizeof(server.key));
        memcpy(client.key, server.key, sizeof(client.key));
        server.key_established = client.key_established = true;
        memset(server.peer_nonce, 0x31, sizeof(server.peer_nonce));
        memset(server.our_nonce, 0x42, sizeof(server.our_nonce));
        fixture.authorization = FILE_MARKET_DELIVERY_AUTHORIZED;
        fixture.load_ok = true;
        fs_pow_reset_state();
    }
    uint8_t client_ip[16] = {0};
    bool served = socket_ready && file_market_delivery_serve(
        &server, client_ip, wire, sizeof(wire));
    uint8_t type = 0;
    const uint8_t *reply_payload = NULL;
    uint32_t reply_len = 0;
    served = served && fs_recv_frame(&client, &type, &reply_payload,
                                     &reply_len) &&
             type == FS_MARKET_REPLY &&
             file_market_delivery_reply_decode(
                 reply_payload, reply_len, &reply_roundtrip) &&
             reply_roundtrip.status == FILE_MARKET_DELIVERY_READY;
    uint8_t size_wire[4], body[32], mac[32];
    uint32_t served_size = 0;
    if (served) {
        served = delivery_recv_exact(sockets[1], size_wire, 4);
        served_size = (uint32_t)size_wire[0] |
            ((uint32_t)size_wire[1] << 8) |
            ((uint32_t)size_wire[2] << 16) |
            ((uint32_t)size_wire[3] << 24);
        served = served && served_size <= sizeof(body) &&
            delivery_recv_exact(sockets[1], body, served_size) &&
            delivery_recv_exact(sockets[1], mac, sizeof(mac));
    }
    DELIVERY_CHECK("encrypted server dispatch sends reply before paid bytes",
        served && served_size == reply_roundtrip.size);
    if (sockets[0] >= 0) close(sockets[0]);
    if (sockets[1] >= 0) close(sockets[1]);

    int buyer_sockets[2] = {-1, -1};
    bool buyer_ready = socketpair(AF_UNIX, SOCK_STREAM, 0, buyer_sockets) == 0;
    struct fs_session buyer_server, buyer_client;
    pthread_t server_thread;
    bool server_started = false;
    struct delivery_server_call server_call = {0};
    if (buyer_ready) {
        fs_session_init(&buyer_server, buyer_sockets[0]);
        fs_session_init(&buyer_client, buyer_sockets[1]);
        memset(buyer_server.key, 0x86, sizeof(buyer_server.key));
        memcpy(buyer_client.key, buyer_server.key, sizeof(buyer_client.key));
        buyer_server.key_established = buyer_client.key_established = true;
        memset(buyer_server.peer_nonce, 0x91,
               sizeof(buyer_server.peer_nonce));
        memset(buyer_server.our_nonce, 0x92,
               sizeof(buyer_server.our_nonce));
        memcpy(buyer_client.our_nonce, buyer_server.peer_nonce, 32);
        memcpy(buyer_client.peer_nonce, buyer_server.our_nonce, 32);
        fixture.authorization = FILE_MARKET_DELIVERY_AUTHORIZED;
        fixture.load_ok = true;
        fixture.corrupt_hash = false;
        server_call.session = &buyer_server;
        server_started = pthread_create(&server_thread, NULL,
                                        delivery_server_call_main,
                                        &server_call) == 0;
    }
    uint8_t buyer_public[32], buyer_secret[32];
    ed25519_keypair(buyer_public, buyer_secret, buyer_seed);
    struct file_market_delivery_chunk fetched = {0};
    enum file_market_delivery_status fetched_status =
        buyer_ready && server_started
            ? file_market_delivery_fetch_session(
                &buyer_client, request.network_genesis, request.offer_id, 7,
                buyer_public, buyer_seed, &fetched)
            : FILE_MARKET_DELIVERY_PAYMENT_UNKNOWN;
    if (server_started)
        pthread_join(server_thread, NULL);
    DELIVERY_CHECK("buyer client sends session-bound request and verifies chunk",
        server_call.served && fetched_status == FILE_MARKET_DELIVERY_READY &&
        fetched.data && fetched.size == sizeof("paid-chunk-proof") &&
        memcmp(fetched.data, "paid-chunk-proof",
               sizeof("paid-chunk-proof")) == 0);
    free(fetched.data);
    if (buyer_sockets[0] >= 0) close(buyer_sockets[0]);
    if (buyer_sockets[1] >= 0) close(buyer_sockets[1]);

    int listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 endpoint_addr;
    memset(&endpoint_addr, 0, sizeof(endpoint_addr));
    endpoint_addr.sin6_family = AF_INET6;
    endpoint_addr.sin6_addr = in6addr_loopback;
    endpoint_addr.sin6_port = 0;
    socklen_t endpoint_len = sizeof(endpoint_addr);
    bool endpoint_ready = listen_fd >= 0 &&
        bind(listen_fd, (struct sockaddr *)&endpoint_addr,
             sizeof(endpoint_addr)) == 0 &&
        listen(listen_fd, 1) == 0 &&
        getsockname(listen_fd, (struct sockaddr *)&endpoint_addr,
                    &endpoint_len) == 0;
    struct delivery_endpoint_server endpoint_server = {
        .listen_fd = listen_fd,
    };
    pthread_t endpoint_thread;
    bool endpoint_started = endpoint_ready && pthread_create(
        &endpoint_thread, NULL, delivery_endpoint_server_main,
        &endpoint_server) == 0;
    uint8_t loopback_ip[16];
    memcpy(loopback_ip, &in6addr_loopback, 16);
    struct file_market_delivery_chunk endpoint_chunk = {0};
    enum file_market_delivery_status endpoint_status = endpoint_started
        ? file_market_delivery_fetch_endpoint(
            loopback_ip, ntohs(endpoint_addr.sin6_port),
            request.network_genesis, request.offer_id, 7,
            buyer_public, buyer_seed, &endpoint_chunk)
        : FILE_MARKET_DELIVERY_PAYMENT_UNKNOWN;
    if (endpoint_started)
        pthread_join(endpoint_thread, NULL);
    DELIVERY_CHECK("signed offer endpoint completes real encrypted loopback fetch",
        endpoint_server.served &&
        endpoint_status == FILE_MARKET_DELIVERY_READY &&
        endpoint_chunk.data &&
        endpoint_chunk.size == sizeof("paid-chunk-proof") &&
        memcmp(endpoint_chunk.data, "paid-chunk-proof",
               sizeof("paid-chunk-proof")) == 0);
    free(endpoint_chunk.data);
    if (listen_fd >= 0) close(listen_fd);

    file_market_delivery_reset_handlers();
    return failures;
}
