/* server-tls-pkcallback.c
 *
 * Copyright (C) 2006-2022 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* This example shows how to register a private key callback and optionally
 * use the asynchronous version of the code to return a WC_PENDING_E while the
 * hardware is processing */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

/* wolfSSL */
#ifndef WOLFSSL_USER_SETTINGS
    #include <wolfssl/options.h>
#endif
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/cryptocb.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#define DEFAULT_PORT 11111

#define USE_ECDHE_ECDSA
#define USE_TLSV13

#ifdef USE_ECDHE_ECDSA
#define CERT_FILE "../certs/server-ecc.pem"
#define KEY_FILE  "../certs/ecc-key.pem"
#define CA_FILE   "../certs/client-ecc-cert.pem"
#else
#define CERT_FILE "../certs/server-cert.pem"
#define KEY_FILE  "../certs/server-key.pem"
#define CA_FILE   "../certs/client-cert.pem"
#endif

typedef struct {
    const char* keyFile;
    ecc_key     key;
    int         state;
} PkCbInfo;

#ifdef HAVE_PK_CALLBACKS
/* reads file size, allocates buffer, reads into buffer, returns buffer */
static int load_file(const char* fname, byte** buf, size_t* bufLen)
{
    int ret;
    long int fileSz;
    XFILE lFile;

    if (fname == NULL || buf == NULL || bufLen == NULL)
        return BAD_FUNC_ARG;

    /* set defaults */
    *buf = NULL;
    *bufLen = 0;

    /* open file (read-only binary) */
    lFile = XFOPEN(fname, "rb");
    if (!lFile) {
        printf("Error loading %s\n", fname);
        return BAD_PATH_ERROR;
    }

    fseek(lFile, 0, SEEK_END);
    fileSz = (int)ftell(lFile);
    rewind(lFile);
    if (fileSz  > 0) {
        *bufLen = (size_t)fileSz;
        *buf = (byte*)malloc(*bufLen);
        if (*buf == NULL) {
            ret = MEMORY_E;
            printf("Error allocating %lu bytes\n", (unsigned long)*bufLen);
        }
        else {
            size_t readLen = fread(*buf, *bufLen, 1, lFile);

            /* check response code */
            ret = (readLen > 0) ? 0 : -1;
        }
    }
    else {
        ret = BUFFER_E;
    }
    fclose(lFile);

    return ret;
}

static int load_key_file(const char* fname, byte** derBuf, word32* derLen)
{
    int ret;
    byte* buf = NULL;
    size_t bufLen;

    ret = load_file(fname, &buf, &bufLen);
    if (ret != 0)
        return ret;

    *derBuf = (byte*)malloc(bufLen);
    if (*derBuf == NULL) {
        free(buf);
        return MEMORY_E;
    }

    ret = wc_KeyPemToDer(buf, (word32)bufLen, *derBuf, (word32)bufLen, NULL);
    if (ret < 0) {
        free(buf);
        free(*derBuf);
        return ret;
    }
    *derLen = ret;
    free(buf);

    return 0;
}

/* This function is performing a sign using a private key for testing. In a
 * real-world use case this would be sent to HSM / TPM hardware for processing
 * and return WC_PENDING_E to give this thread time to do other work */
static int myEccSign(WOLFSSL* ssl, const byte* in, word32 inSz,
        byte* out, word32* outSz, const byte* key, word32 keySz, void* ctx)
{
    int ret;
    byte* keyBuf = (byte*)key;
    PkCbInfo* cbInfo = (PkCbInfo*)ctx;

    printf("PK ECC Sign: inSz %u, keySz %u\n", inSz, keySz);

#ifdef WOLFSSL_ASYNC_CRYPT
    if (cbInfo->state == 0) {
        cbInfo->state++;
        printf("PK ECC Sign: Async Simulate\n");
        return WC_PENDING_E;
    }
#endif

    ret = load_key_file(cbInfo->keyFile, &keyBuf, &keySz);
    if (ret == 0) {
        ret = wc_ecc_init(&cbInfo->key);
        if (ret == 0) {
            word32 idx = 0;
            ret = wc_EccPrivateKeyDecode(keyBuf, &idx, &cbInfo->key, keySz);
            if (ret == 0) {
                WC_RNG *rng = wolfSSL_GetRNG(ssl);

                printf("PK ECC Sign: Curve ID %d\n", cbInfo->key.dp->id);
                ret = wc_ecc_sign_hash(in, inSz, out, outSz, rng, &cbInfo->key);
            }
            wc_ecc_free(&cbInfo->key);
        }
    }
    free(keyBuf);
#ifdef WOLFSSL_ASYNC_CRYPT
    cbInfo->state = 0;
#endif

    printf("PK ECC Sign: ret %d outSz %u\n", ret, *outSz);

    return ret;
}
#endif /* HAVE_PK_CALLBACKS */

int main(int argc, char** argv)
{
    int                ret, err;
    int                sockfd = SOCKET_INVALID;
    int                connd  = SOCKET_INVALID;
    struct sockaddr_in servAddr;
    struct sockaddr_in clientAddr;
    socklen_t          size = sizeof(clientAddr);
    char               buff[256];
    size_t             len;
    int                shutdown = 0;
    const char*        reply = "I hear ya fa shizzle!\n";
    int                on;

    /* PK callback context */
    PkCbInfo myCtx;
    memset(&myCtx, 0, sizeof(myCtx));
    myCtx.keyFile = KEY_FILE;

    /* declare wolfSSL objects */
    WOLFSSL_CTX* ctx = NULL;
    WOLFSSL*     ssl = NULL;

    /* Initialize the server address struct with zeros */
    memset(&servAddr, 0, sizeof(servAddr));

    /* Fill in the server address */
    servAddr.sin_family      = AF_INET;             /* using IPv4      */
    servAddr.sin_port        = htons(DEFAULT_PORT); /* on DEFAULT_PORT */
    servAddr.sin_addr.s_addr = INADDR_ANY;          /* from anywhere   */

    /* Create a socket that uses an internet IPv4 address,
     * Sets the socket to be stream based (TCP),
     * 0 means choose the default protocol. */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "ERROR: failed to create the socket\n");
        ret = -1;
        goto exit;
    }

    /* make sure server is setup for reuse addr/port */
    on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
            (char*)&on, (socklen_t)sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT,
               (char*)&on, (socklen_t)sizeof(on));
#endif


    /* Bind the server socket to our port */
    if (bind(sockfd, (struct sockaddr*)&servAddr, sizeof(servAddr)) == -1) {
        fprintf(stderr, "ERROR: failed to bind\n");
        ret = -1;
        goto exit;
    }

    /* Listen for a new connection, allow 5 pending connections */
    if (listen(sockfd, 5) == -1) {
        fprintf(stderr, "ERROR: failed to listen\n");
        ret = -1;
        goto exit;
    }



    /*---------------------------------*/
    /* Start of wolfSSL initialization and configuration */
    /*---------------------------------*/
#if 0
    wolfSSL_Debugging_ON();
#endif

    /* Initialize wolfSSL */
    wolfSSL_Init();

    /* Create and initialize WOLFSSL_CTX */
#ifdef USE_TLSV13
    ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
#else
    ctx = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
#endif
    if (ctx == NULL) {
        fprintf(stderr, "ERROR: failed to create WOLFSSL_CTX\n");
        ret = -1;
        goto exit;
    }

    /* Load server certificates into WOLFSSL_CTX */
    if ((ret = wolfSSL_CTX_use_certificate_file(ctx, CERT_FILE, SSL_FILETYPE_PEM))
        != WOLFSSL_SUCCESS) {
        fprintf(stderr, "ERROR: failed to load %s, please check the file.\n",
                CERT_FILE);
        goto exit;
    }

    /* Load server key into WOLFSSL_CTX */
    if ((ret = wolfSSL_CTX_use_PrivateKey_file(ctx, KEY_FILE, SSL_FILETYPE_PEM))
        != WOLFSSL_SUCCESS) {
        fprintf(stderr, "ERROR: failed to load %s, please check the file.\n",
                KEY_FILE);
        goto exit;
    }

    /* Load CA certificate into WOLFSSL_CTX for validating peer */
    if ((ret = wolfSSL_CTX_load_verify_locations(ctx, CA_FILE, NULL))
         != WOLFSSL_SUCCESS) {
        fprintf(stderr, "ERROR: failed to load %s, please check the file.\n",
                CA_FILE);
        goto exit;
    }

    /* enable mutual authentication */
    wolfSSL_CTX_set_verify(ctx,
        WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

#ifdef HAVE_PK_CALLBACKS
    /* register an ECC sign callback for the long term key */
    wolfSSL_CTX_SetEccSignCb(ctx, myEccSign);
#else
    printf("Warning: PK not compiled in! Please configure wolfSSL with "
           " --enable-pkcallbacks and try again\n");
#endif


    /* Continue to accept clients until shutdown is issued */
    while (!shutdown) {
        printf("Waiting for a connection...\n");

        /* Accept client connections */
        if ((connd = accept(sockfd, (struct sockaddr*)&clientAddr, &size))
            == -1) {
            fprintf(stderr, "ERROR: failed to accept the connection\n\n");
            ret = -1;
            goto exit;
        }

        /* Create a WOLFSSL object */
        if ((ssl = wolfSSL_new(ctx)) == NULL) {
            fprintf(stderr, "ERROR: failed to create WOLFSSL object\n");
            ret = -1;
            goto exit;
        }

        /* Attach wolfSSL to the socket */
        wolfSSL_set_fd(ssl, connd);

    #ifdef HAVE_PK_CALLBACKS
        /* setup the PK context */
        wolfSSL_SetEccSignCtx(ssl, &myCtx);
    #else
        (void)myCtx; /* not used */
    #endif

        /* Establish TLS connection */
        do {
            ret = wolfSSL_accept(ssl);
            err = wolfSSL_get_error(ssl, ret);
        } while (err == WC_PENDING_E);
        if (ret != WOLFSSL_SUCCESS) {
            fprintf(stderr, "wolfSSL_accept error = %d\n", err);
            goto exit;
        }

        printf("Client connected successfully\n");


        /* Read the client data into our buff array */
        memset(buff, 0, sizeof(buff));
        if ((ret = wolfSSL_read(ssl, buff, sizeof(buff)-1)) == -1) {
            fprintf(stderr, "ERROR: failed to read\n");
            goto exit;
        }

        /* Print to stdout any data the client sends */
        printf("Client: %s\n", buff);

        /* Check for server shutdown command */
        if (strncmp(buff, "shutdown", 8) == 0) {
            printf("Shutdown command issued!\n");
            shutdown = 1;
        }


        /* Write our reply into buff */
        memset(buff, 0, sizeof(buff));
        memcpy(buff, reply, strlen(reply));
        len = strnlen(buff, sizeof(buff));

        /* Reply back to the client */
        if ((ret = wolfSSL_write(ssl, buff, len)) != len) {
            fprintf(stderr, "ERROR: failed to write\n");
            goto exit;
        }

        /* Notify the client that the connection is ending */
        wolfSSL_shutdown(ssl);
        printf("Shutdown complete\n");

        /* Cleanup after this connection */
        wolfSSL_free(ssl);      /* Free the wolfSSL object              */
        close(connd);           /* Close the connection to the client   */
        connd = SOCKET_INVALID;
    }

    ret = 0; /* success */

exit:

    /* Cleanup and return */
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (connd != SOCKET_INVALID)
        close(connd);
    if (sockfd != SOCKET_INVALID)
        close(sockfd);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();

    return ret;
}
