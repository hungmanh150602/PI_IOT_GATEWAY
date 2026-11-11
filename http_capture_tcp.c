/*
 * Download an HTTP response body over TCP and save it to a local file.
 * The program resolves the server address, sends a GET request, separates
 * the response headers from the body, and writes only body bytes to disk.
 */

/* Standard C and POSIX headers for I/O, strings, sockets, and error handling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

#include <sys/socket.h>
#include <netdb.h>

/* Network read buffer and maximum accumulated response-header size. */
#define BUFFER_SIZE 4096
#define HEADER_SIZE 16384

/*
 * Metadata extracted from the HTTP response headers.
 * has_content_length distinguishes a missing length header from a valid
 * Content-Length value of zero; content_type is a bounded display buffer.
 */
struct http_response
{
    int status_code;
    size_t content_length;
    int has_content_length;
    char content_type[128];
};

/*
 * Send the complete request even when send() performs a partial write.
 * Retry when interrupted by a signal; report other socket errors and return
 * -1 if the peer closes the connection before all bytes are sent.
 */
static int send_all(int sockfd, const char *data, size_t len)
{
    size_t total = 0;

    while (total < len)
    {
        ssize_t sent = send(sockfd, data + total, len - total, MSG_NOSIGNAL);

        if (sent < 0)
        {
            if (errno == EINTR)
                continue;

            perror("send");
            return -1;
        }

        if (sent == 0)
            return -1;

        total += (size_t)sent;
    }

    return 0;
}

/*
 * Find the end of the HTTP header block, marked by CRLF CRLF.
 * Return the offset immediately after the delimiter, or -1 if it is not
 * present in the len bytes currently available.
 */
static int find_header_end(const char *buffer, size_t len)
{
    if (len < 4)
        return -1;

    for (size_t i = 0; i <= len - 4; i++)
    {
        /* Compare four bytes at this position with the header terminator. */
        if (memcmp(buffer + i, "\r\n\r\n", 4) == 0)
            return (int)(i + 4);
    }
    return -1;
}

/*
 * Parse the complete HTTP response header block and extract the status code,
 * optional Content-Length, and optional Content-Type. The input is modified
 * in place by strtok_r() as it splits the block into lines.
 */
static int parse_headers(char *header, struct http_response *response)
{
    /* Initialize every field so absent headers have well-defined defaults. */
    memset(response, 0, sizeof(*response));

    /*
     * Split the mutable header block into lines. The first line is the HTTP
     * status line (for example, "HTTP/1.1 200 OK"); saveptr preserves the
     * tokenizer state between calls.
     */
    char *saveptr = NULL;
    char *line = strtok_r(header, "\r\n", &saveptr);

    if (line == NULL || sscanf(line, "HTTP/%*u.%*u %d", &response->status_code) != 1)
    {
        fprintf(stderr, "Invalid HTTP status line\n");
        return -1;
    }

    /* Process each remaining header line until the block has been consumed. */
    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL)
    {
        /* HTTP field names are case-insensitive, so compare without case. */
        if (strncasecmp(line, "Content-Length:", 15) == 0)
        {
            char *value = line + 15;
            char *endptr = NULL;

            /* Ignore optional whitespace between the colon and the value. */
            while (*value == ' ' || *value == '\t')
                value++;

            errno = 0;
            /* Parse the decimal length while detecting conversion errors. */
            unsigned long long length = strtoull(value, &endptr, 10);

            /*
             * Reject conversion errors, empty values, trailing characters,
             * and values that cannot fit in size_t before storing the length.
             */
            if (errno != 0 || endptr == value || *endptr != '\0' ||
                length > SIZE_MAX)
            {
                fprintf(stderr, "Invalid Content-Length\n");
                return -1;
            }

            response->content_length = (size_t)length;
            response->has_content_length = 1;
        }

        /* Capture Content-Type for reporting; it is not used to decode the body. */
        else if (strncasecmp(line, "Content-Type:", 13) == 0)
        {
            const char *value = line + 13;

            /* Ignore optional whitespace after the field-name colon. */
            while (*value == ' ' || *value == '\t')
                value++;

            /* Bound the copy so an unusually long field cannot overflow the buffer. */
            snprintf(response->content_type, sizeof(response->content_type), "%s", value);
        }
    }
    return 0;
}

/*
 * Write len response-body bytes to the already-open output file and add the
 * successfully written count to total. A zero-length chunk is a no-op.
 */
static int save_body(FILE *file, const char *data, size_t len, size_t *total)
{
    if (len == 0)
        return 0;

    /* Write bytes verbatim; using one-byte elements preserves binary payloads. */
    size_t written = fwrite(data, 1, len, file);

    if (written != len)
    {
        perror("fwrite");
        return -1;
    }

    *total += written;
    return 0;
}

int main(int argc, char *argv[])
{
    /* Require the server host, TCP port, request path, and output file path. */
    if (argc != 5)
    {
        fprintf(stderr,
                "Usage: %s <host> <port> <path> <output_path>\n"
                "Example: ./build/http_capture_tcp 192.168.1.100 80 /capture images/capture.jpg\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    /* Keep command-line values as read-only strings for request construction. */
    const char *host = argv[1];
    const char *port = argv[2];
    const char *path = argv[3];
    const char *output = argv[4];

    int sockfd = -1;
    int ret = EXIT_FAILURE;

    /* Address-resolution structures used to try each candidate server address. */
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;

    FILE *file = NULL;

    /*
     * The buffers have distinct roles:
     * request holds the outgoing GET request,
     * buffer receives raw TCP data, and
     * header accumulates response bytes until the header terminator is found.
     */
    char request[1024];
    /* Temporary storage for each chunk received from the TCP socket. */
    char buffer[BUFFER_SIZE];
    /* Accumulated response headers, bounded by HEADER_SIZE. */
    char header[HEADER_SIZE];

    size_t header_len = 0;    /* Number of response-header bytes accumulated so far. */
    size_t body_received = 0; /* Number of response-body bytes written to the file. */

    int header_complete = 0; /* Set once the CRLF CRLF header terminator is found. */

    struct http_response response = {0};

    /* 1. Resolve the host and port into one or more usable network addresses. */
    memset(&hints, 0, sizeof(hints)); /* Clear optional addrinfo settings. */

    hints.ai_family = AF_UNSPEC;     /* Permit either IPv4 or IPv6 results. */
    hints.ai_socktype = SOCK_STREAM; /* Request a stream socket (TCP). */

    int status = getaddrinfo(host, port, &hints, &result);

    if (status != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        goto cleanup;
    }

    /* 2. Try each resolved address until a TCP connection succeeds. */
    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sockfd < 0)
            continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
        {
            /* Keep this connected socket and stop searching the address list. */
            break;
        }

        /* Discard the failed socket before trying the next resolved address. */
        close(sockfd);
        sockfd = -1;
    }

    if (sockfd < 0)
    {
        fprintf(stderr, "Cannot connect to server\n");
        goto cleanup;
    }

    printf("TCP connection established\n");

    /*
     * 3. Construct an HTTP/1.1 GET request. Connection: close asks the server
     * to close the connection after sending the response, which also allows
     * the client to detect completion when Content-Length is absent.
     */
    int request_len = snprintf(request, sizeof(request),
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Connection: close\r\n"
                               "\r\n",
                               path, host);

    if (request_len < 0 || (size_t)request_len >= sizeof(request))
    {
        fprintf(stderr, "HTTP request too long\n");
        goto cleanup;
    }

    /* 4. Send the entire request, handling partial writes in send_all(). */
    if (send_all(sockfd, request, (size_t)request_len) < 0)
        goto cleanup;

    printf("HTTP GET request sent\n");

    /*
     * 5. Open the destination in binary-write mode. This creates a new file
     * or truncates an existing one, and avoids text-mode transformations.
     */
    file = fopen(output, "wb");

    if (file == NULL)
    {
        perror("fopen");
        goto cleanup;
    }

    /* 6. Receive the response, separating its headers from its body. */
    while (1)
    {
        /* Read the next available chunk from the server. */
        ssize_t received = recv(sockfd, buffer, sizeof(buffer), 0);

        if (received < 0)
        {
            if (errno == EINTR)
                continue;

            perror("recv");
            goto cleanup;
        }

        if (received == 0)
            break;

        size_t offset = 0;
        size_t chunk_len = (size_t)received;

        /* 6.1. Accumulate header bytes until the CRLF CRLF delimiter is found. */
        if (!header_complete)
        {
            while (offset < chunk_len && !header_complete)
            {
                if (header_len >= HEADER_SIZE - 1)
                {
                    fprintf(stderr, "HTTP header too large\n");
                    goto cleanup;
                }

                header[header_len++] = buffer[offset++];

                /* Check whether the accumulated bytes now contain the delimiter. */
                int end = find_header_end(header, header_len);

                /* The complete header is available; parse it before body data. */
                if (end >= 0)
                {
                    header_complete = 1;

                    header_len = (size_t)end;
                    header[header_len] = '\0';

                    if (parse_headers(header, &response) < 0)
                        goto cleanup;

                    printf("\nHTTP Status: %d\n", response.status_code);

                    printf("Content-Type: %s\n",
                           response.content_type[0] ? response.content_type : "unknown");

                    if (response.has_content_length)
                    {
                        printf("Content-Length: %zu bytes\n", response.content_length);
                    }
                    else
                    {
                        printf("Content-Length: unknown\n");
                    }

                    printf("\nReceiving HTTP body...\n");
                }
            }
        }

        /*
         * 6.2. Save body bytes from the current TCP chunk.
         * offset points just past the header delimiter, so any remaining
         * bytes in this same chunk belong to the response body.
         */
        if (header_complete && offset < chunk_len)
        {
            size_t body_len = chunk_len - offset;

            /* If the server declared a body length, do not write beyond it. */
            if (response.has_content_length)
            {
                /* Calculate how many more bytes are expected. */
                size_t remaining = response.content_length - body_received;

                /* Ignore any bytes beyond the declared response-body length. */
                if (body_len > remaining)
                    body_len = remaining;
            }

            /* Write only the body portion; the HTTP headers are never saved. */
            if (save_body(file, buffer + offset, body_len, &body_received) < 0)
            {
                goto cleanup;
            }
        }

        /* Stop as soon as the declared number of body bytes has been received. */
        if (header_complete &&
            response.has_content_length &&
            body_received == response.content_length)
        {
            break;
        }
    }

    /* Verify that the response header and, when declared, the body are complete. */
    if (!header_complete)
    {
        fprintf(stderr, "Incomplete HTTP response header\n");
        goto cleanup;
    }

    if (response.has_content_length && body_received != response.content_length)
    {
        fprintf(stderr, "Incomplete body: expected %zu, received %zu\n",
                response.content_length,
                body_received);
        goto cleanup;
    }

    printf("\nHTTP response completed\n");
    printf("Body received: %zu bytes\n", body_received);
    printf("Saved to: %s\n", output);

    ret = response.status_code >= 200 && response.status_code < 300
              ? EXIT_SUCCESS
              : EXIT_FAILURE;

/* Release any resources acquired along the success or error paths. */
cleanup:

    if (file != NULL)
        fclose(file);

    if (sockfd >= 0)
        close(sockfd);

    if (result != NULL)
        freeaddrinfo(result);

    return ret;
}