#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "native.h"

#define BUF_LEN 2048

#define DEBUG(X, ...)                                                          \
  printf(X "\n", ##__VA_ARGS__);                                               \
  fflush(stdout);

static void *handshake_function(void *user_data);

UNIFEX_TERM init(UnifexEnv *env, int client_mode) {
  State *state = unifex_alloc_state(env);
  state->env = env;

  state->ssl_ctx = create_ctx();
  if (state->ssl_ctx == NULL) {
    return unifex_raise(env, "Cannot create ssl_ctx");
  }

  state->pkey = gen_key();
  if (state->pkey == NULL) {
    return unifex_raise(env, "Cannot generate key pair");
  }

  if (SSL_CTX_use_PrivateKey(state->ssl_ctx, state->pkey) != 1) {
    return unifex_raise(env, "Cannot set private key");
  }

  state->x509 = gen_cert(state->pkey);
  if (state->x509 == NULL) {
    return unifex_raise(env, "Cannot generate cert");
  }

  if (SSL_CTX_use_certificate(state->ssl_ctx, state->x509) != 1) {
    return unifex_raise(env, "Cannot set cert");
  }

  state->ssl = create_ssl(state->ssl_ctx, client_mode);
  if (state->ssl == NULL) {
    return unifex_raise(env, "Cannot create ssl");
  }

  state->client_mode = client_mode;
  state->SSL_error = SSL_ERROR_NONE;

  return init_result_ok(env, state);
}

UNIFEX_TERM get_cert_fingerprint(UnifexEnv *env, State *state) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int size;
  if(X509_digest(state->x509, EVP_sha256(), md, &size) != 1) {
    get_cert_fingerprint_result_error_failed_to_get_fingerprint(env);
  }
  return get_cert_fingerprint_result_ok(env, state, (char *)md);
}

UNIFEX_TERM do_handshake(UnifexEnv *env, State *state) {
  if (pthread_create(&state->handshake_fun_tid, NULL, handshake_function,
                     (void *)state)) {
    return unifex_raise(env, "Cannot create handshake function thread");
  }
  return do_handshake_result_ok(env, state);
}

static void *handshake_function(void *user_data) {
  State *state = (State *)user_data;
  for (;;) {
    int res = SSL_do_handshake(state->ssl);

    size_t pending_data_len = BIO_ctrl_pending(SSL_get_wbio(state->ssl));
    if (pending_data_len > 0) {
      DEBUG("WBIO: pending data: %ld bytes", pending_data_len);

      char *data = (char *)malloc(pending_data_len * sizeof(char));
      memset(data, 0, pending_data_len);
      BIO *wbio = SSL_get_wbio(state->ssl);
      int read_bytes = BIO_read(wbio, data, pending_data_len);
      if (read_bytes <= 0) {
        DEBUG("WBIO: read error");
        send_handshake_failed_wbio_error(state->env, *state->env->reply_to, 0);
        exit(EXIT_FAILURE);
      }

      DEBUG("WBIO: read: %d bytes", read_bytes);

      UnifexPayload *payload = (UnifexPayload *)unifex_alloc(sizeof(UnifexPayload));
      payload->data = (unsigned char*)data;
      payload->size = (unsigned int)read_bytes;
      payload->type = UNIFEX_PAYLOAD_BINARY,
      payload->owned = 0;
      send_packets(state->env, *state->env->reply_to, 0, payload);

      DEBUG("Sent %d bytes", payload->size);

      free(data);
    }

    res = SSL_get_error(state->ssl, res);
    switch (res) {
    case SSL_ERROR_WANT_READ:
      DEBUG("SSL WANT READ");
      state->SSL_error = SSL_ERROR_WANT_READ;
      // break and wait for data from remote host. It will come in feed() function.
      return NULL;
    case SSL_ERROR_WANT_WRITE:
      DEBUG("SSL WANT WRITE");
      break;
    case SSL_ERROR_NONE:
      /*
        This will be reached only when handshake succeeds because SSL_ERROR_NONE
        is returned only when res (passed to SSL_get_error) is greater than 0
        and res greater than zero means handshake succeeded.
      */
      DEBUG("Handshake finished successfully");

      unsigned char *material = export_keying_material(state->ssl);
      if (material == NULL) {
        DEBUG("Cannot export keying material");
        exit(EXIT_FAILURE);
      }

      DEBUG("Keying material %s", material);

      send_handshake_finished(state->env, *state->env->reply_to, 0,
                              (char *)material);

      exit(EXIT_SUCCESS);
    default:
      DEBUG("SSL ERROR: %d", res);
      send_handshake_failed_ssl_error(state->env, *state->env->reply_to, 0,
                                      res);
      exit(EXIT_FAILURE);
    }
  }
  return NULL;
}

UNIFEX_TERM feed(UnifexEnv *env, State *state, UnifexPayload *payload) {

  DEBUG("Feeding: %d", payload->size);

  if (payload->size == 0) {
    DEBUG("Peer socket shutdown, handshake failed");
    send_handshake_failed_peer_shutdown(state->env, *state->env->reply_to,
                                        0);
    exit(EXIT_FAILURE);
  }

  int bytes = BIO_write(SSL_get_rbio(state->ssl), payload->data, payload->size);
  if (bytes <= 0) {
    DEBUG("RBIO: write error");
    send_handshake_failed_rbio_error(state->env, *state->env->reply_to, 0);
    exit(EXIT_FAILURE);
  }

  DEBUG("RBIO: wrote %d", bytes);

  do_handshake(env, state);

  return feed_result_ok(env, state);
}

void handle_destroy_state(UnifexEnv *env, State *state) {
  UNIFEX_UNUSED(env);
  DEBUG("Destroying state");

  if (state->ssl_ctx) {
    SSL_CTX_free(state->ssl_ctx);
  }

  if (state->ssl) {
    SSL_free(state->ssl);
  }

  if (state->pkey) {
    EVP_PKEY_free(state->pkey);
  }

  if (state->x509) {
    X509_free(state->x509);
  }
}
