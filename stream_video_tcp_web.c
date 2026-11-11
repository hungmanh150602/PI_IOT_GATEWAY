/*
 * Program workflow overview:
 * 1. Connect to the ESP32-CAM over TCP and request the HTTP /stream endpoint.
 * 2. Validate the HTTP status code and multipart MJPEG content type.
 * 3. Start a local HTTP server that browsers can access through this gateway.
 * 4. Serve an HTML page at "/" and forward JPEG frames at "/stream".
 * 5. Frames are held in memory only while being forwarded; they are not saved to disk.
 */

/* Enable GNU extensions, including helpers such as strcasestr(). */
#define _GNU_SOURCE

/* Standard C functions for I/O, memory allocation, and string handling. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Buffer sizes and limits for data received over the network. */
#define BUFFER_SIZE 4096
#define LINE_SIZE 1024
#define REQUEST_SIZE 4096
#define MAX_FRAME_SIZE (10U * 1024U * 1024U)
#define DEFAULT_WEB_PORT "8080"

/*
 * Flag shared by the main loop and the signal handler.
 * sig_atomic_t allows simple reads and writes to be handled atomically
 * when accessed from a signal handler.
 */
static volatile sig_atomic_t running = 1;

/* Ask the loops to stop cleanly when Ctrl+C or SIGTERM is received. */
static void handle_sigint(int signo)
{
    (void)signo;
    running = 0;
}

/*
 * Send all length bytes because send() may transmit only part of the data.
 * Resume from the last transmitted byte after EINTR; return -1 if the socket
 * fails or closes, or if the program stops before the send is complete.
 */
static int send_all(int sockfd, const void *data, size_t length)
{
    const unsigned char *p = data;
    size_t total = 0;

    while (total < length && running)
    {
        ssize_t n = send(sockfd, p + total, length - total, MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR && running)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        total += (size_t)n;
    }

    return total == length ? 0 : -1;
}

/*
 * Read one protocol line, one byte at a time, until a newline is encountered.
 * Strip the CR preceding LF and always null-terminate the resulting string.
 * Return the line length, or -1 if the connection closes, an error occurs,
 * or the line exceeds the buffer capacity.
 */
static int read_line(int sockfd, char *buffer, size_t capacity)
{
    size_t pos = 0;
    char ch;

    if (capacity < 2)
        return -1;

    while (pos < capacity - 1 && running)
    {
        ssize_t n = recv(sockfd, &ch, 1, 0);

        if (n == 0)
            return -1;

        if (n < 0)
        {
            if (errno == EINTR && running)
                continue;
            return -1;
        }

        if (ch == '\n')
        {
            if (pos > 0 && buffer[pos - 1] == '\r')
                pos--;

            buffer[pos] = '\0';
            return (int)pos;
        }

        buffer[pos++] = ch;
    }

    buffer[pos] = '\0';
    return -1;
}

/*
 * Read exactly length bytes, typically for a JPEG payload whose size is
 * specified by its Content-Length header. A single recv() may not return
 * all requested data because TCP can deliver it in multiple segments.
 */
static int recv_exact(int sockfd, unsigned char *buffer, size_t length)
{
    size_t total = 0;

    while (total < length && running)
    {
        ssize_t n = recv(sockfd, buffer + total, length - total, 0);

        if (n == 0)
            return -1;

        if (n < 0)
        {
            if (errno == EINTR && running)
                continue;
            return -1;
        }

        total += (size_t)n;
    }

    return total == length ? 0 : -1;
}

/*
 * Parse the numeric value after the colon in a Content-Length header.
 * Reject empty, zero, malformed, or oversized values to avoid invalid or
 * excessive frame-memory allocations.
 */
static int parse_content_length(const char *line, size_t *length)
{
    const char *colon = strchr(line, ':');
    char *end = NULL;
    unsigned long value;

    if (colon == NULL)
        return -1;

    errno = 0;
    value = strtoul(colon + 1, &end, 10);

    if (errno != 0 || end == colon + 1)
        return -1;

    while (*end == ' ' || *end == '\t')
        end++;

    if (*end != '\0' || value == 0 || value > MAX_FRAME_SIZE)
        return -1;

    *length = (size_t)value;
    return 0;
}

/*
 * Resolve addresses for the host and port, then try each one in turn.
 * This supports both IPv4 and IPv6. Return the first successfully connected
 * socket, or -1 if none of the available addresses can be reached.
 */
static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    int sockfd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(host, port, &hints, &result);
    if (ret != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0)
            continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result);
    return sockfd;
}

/*
 * Connect to the ESP32-CAM, request GET /stream, and read the response headers.
 * Proceed only if the server returns HTTP 200 and a Content-Type identifying
 * the multipart/x-mixed-replace stream with the boundary=frame expected by
 * the frame reader below. On success, ownership of the socket passes to the caller.
 */
static int start_esp32_stream(const char *host, const char *port)
{
    int sockfd = connect_to_server(host, port);
    char request[1024];
    char line[LINE_SIZE];
    int status_code = 0;
    int have_mjpeg_type = 0;

    if (sockfd < 0)
    {
        fprintf(stderr, "Could not connect to ESP32-CAM\n");
        return -1;
    }

    /* Build the HTTP/1.1 request and ensure snprintf did not truncate it. */
    int request_len = snprintf(request, sizeof(request),
                               "GET /stream HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Accept: multipart/x-mixed-replace\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n",
                               host);

    if (request_len < 0 || (size_t)request_len >= sizeof(request) ||
        send_all(sockfd, request, (size_t)request_len) != 0)
    {
        fprintf(stderr, "Could not send HTTP request to ESP32-CAM\n");
        close(sockfd);
        return -1;
    }

    /*
     * Read header lines until the blank line separating headers from the body.
     * Validate the status line and Content-Type independently; ignore other
     * headers that are not needed by this gateway.
     */
    while (running)
    {
        int len = read_line(sockfd, line, sizeof(line));
        if (len < 0)
        {
            fprintf(stderr, "Failed to read ESP32-CAM HTTP headers\n");
            close(sockfd);
            return -1;
        }

        if (len == 0)
            break;

        if (strncmp(line, "HTTP/", 5) == 0)
        {
            if (sscanf(line, "%*s %d", &status_code) != 1)
            {
                fprintf(stderr, "Invalid HTTP status from ESP32-CAM\n");
                close(sockfd);
                return -1;
            }
        }
        else if (strncasecmp(line, "Content-Type:", 13) == 0 &&
                 strcasestr(line, "multipart/x-mixed-replace") != NULL &&
                 strcasestr(line, "boundary=frame") != NULL)
        {
            have_mjpeg_type = 1;
        }
    }

    /* Do not start the gateway unless the ESP32-CAM confirms the expected format. */
    if (status_code != 200 || !have_mjpeg_type)
    {
        fprintf(stderr,
                "Unexpected ESP32-CAM response (HTTP %d, MJPEG=%d)\n",
                status_code, have_mjpeg_type);
        close(sockfd);
        return -1;
    }

    printf("Connected to ESP32-CAM MJPEG stream\n");
    return sockfd;
}

/*
 * Create an HTTP listening socket on web_port. AI_PASSIVE with a NULL host
 * allows binding to suitable network interfaces; SO_REUSEADDR makes it easier
 * to restart the server shortly after a previous process has exited.
 */
static int create_http_server(const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    int listenfd = -1;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int ret = getaddrinfo(NULL, port, &hints, &result);
    if (ret != 0)
    {
        fprintf(stderr, "getaddrinfo (listen): %s\n", gai_strerror(ret));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        listenfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listenfd < 0)
            continue;

        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(listenfd, rp->ai_addr, rp->ai_addrlen) == 0 &&
            listen(listenfd, 5) == 0)
            break;

        close(listenfd);
        listenfd = -1;
    }

    freeaddrinfo(result);
    return listenfd;
}

/*
 * Read the browser's HTTP request through the end of its headers (CRLF CRLF).
 * The buffer is limited to REQUEST_SIZE, so oversized requests and connection
 * errors are rejected rather than being allowed to overflow memory.
 */
static int read_http_request(int clientfd, char *request, size_t capacity)
{
    size_t used = 0;

    while (used < capacity - 1 && running)
    {
        ssize_t n = recv(clientfd, request + used, capacity - 1 - used, 0);

        if (n == 0)
            return -1;

        if (n < 0)
        {
            if (errno == EINTR && running)
                continue;
            return -1;
        }

        used += (size_t)n;
        request[used] = '\0';

        if (strstr(request, "\r\n\r\n") != NULL)
            return (int)used;
    }

    return -1;
}

/*
 * Serve a static HTML page at the root path. Its img element loads /stream
 * separately, allowing the browser to display the JPEG parts of the MJPEG
 * response as video. Compute Content-Length from the page array, excluding
 * its terminating NUL byte.
 */
static void send_http_page(int clientfd)
{
    static const char page[] =
        "<!doctype html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <title>ESP32-CAM Live Stream</title>\n"
        "  <style>\n"
        "    body { margin:0; padding:24px; background:#171717; color:#eee;"
        " font:16px Arial,sans-serif; text-align:center; }\n"
        "    img { display:block; width:100%; max-width:640px; height:auto;"
        " margin:20px auto; background:#000; }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <h2>ESP32-CAM Live Stream</h2>\n"
        "  <img src=\"/stream\" alt=\"Waiting for ESP32-CAM stream...\">\n"
        "</body>\n"
        "</html>\n";

    char header[256];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n"
                     "\r\n",
                     sizeof(page) - 1);

    if (n > 0 && (size_t)n < sizeof(header))
    {
        (void)send_all(clientfd, header, (size_t)n);
        (void)send_all(clientfd, page, sizeof(page) - 1);
    }
}

/* Return a minimal 404 response for unsupported methods or paths. */
static void send_http_not_found(int clientfd)
{
    static const char response[] = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n";

    (void)send_all(clientfd, response, sizeof(response) - 1);
}

/*
 * Forward the MJPEG stream from the ESP32-CAM to one browser.
 * For each frame, read the multipart boundary and part headers, validate
 * Content-Length and Content-Type, receive the complete JPEG payload, and
 * send the part header and payload to the browser. Frames are held temporarily
 * on the heap and are never written to disk.
 * Clients are handled sequentially; when the browser disconnects, return so
 * the main loop can accept the next request.
 */
static int stream_to_browser(int espfd, int browserfd)
{
    /* Use a multipart response so the browser can receive a continuous sequence of images. */
    static const char response_header[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";

    char line[LINE_SIZE];
    unsigned int frame_count = 0;

    if (send_all(browserfd, response_header, sizeof(response_header) - 1) != 0)
        return -1;

    printf("Browser connected: streaming started\n");

    /*
     * Each frame starts with its own boundary. Accept the closing boundary
     * as the end of the stream; any other boundary indicates malformed data
     * and could otherwise desynchronize payload parsing.
     */
    while (running)
    {
        int len = read_line(espfd, line, sizeof(line));
        if (len < 0)
            return -1;

        if (strcmp(line, "--frame--") == 0)
            break;

        if (strcmp(line, "--frame") != 0)
        {
            fprintf(stderr, "Unexpected ESP32-CAM boundary: %s\n", line);
            return -1;
        }

        size_t content_length = 0;
        int have_length = 0;
        int valid_frame_type = 0;

        /*
         * Read a multipart part's headers through the blank line. Require both
         * a valid Content-Length and Content-Type image/jpeg before reading
         * the body.
         */
        while (running)
        {
            len = read_line(espfd, line, sizeof(line));
            if (len < 0)
                return -1;

            if (len == 0)
                break;

            if (strncasecmp(line, "Content-Length:", 15) == 0)
            {
                if (parse_content_length(line, &content_length) != 0)
                {
                    fprintf(stderr, "Invalid JPEG Content-Length\n");
                    return -1;
                }
                have_length = 1;
            }
            else if (strncasecmp(line, "Content-Type:", 13) == 0 &&
                     strcasestr(line, "image/jpeg") != NULL)
            {
                valid_frame_type = 1;
            }
        }

        if (!running)
            break;

        if (!have_length || !valid_frame_type)
        {
            fprintf(stderr, "Invalid JPEG frame headers\n");
            return -1;
        }

        /* Allocate the validated payload size; free the memory after sending. */
        unsigned char *jpeg = malloc(content_length);
        if (jpeg == NULL)
        {
            perror("malloc");
            return -1;
        }

        if (recv_exact(espfd, jpeg, content_length) != 0)
        {
            free(jpeg);
            fprintf(stderr, "Incomplete JPEG frame from ESP32-CAM\n");
            return -1;
        }

        /*
         * Send the multipart header and complete JPEG payload. All three send
         * operations must succeed for the frame to count as forwarded. Always
         * free the allocated memory, including when the browser disconnects.
         */
        char part_header[256];
        int header_len = snprintf(part_header, sizeof(part_header),
                                  "--frame\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "Content-Length: %zu\r\n"
                                  "\r\n",
                                  content_length);

        int send_result = -1;

        if (header_len > 0 && (size_t)header_len < sizeof(part_header) &&
            send_all(browserfd, part_header, (size_t)header_len) == 0 &&
            send_all(browserfd, jpeg, content_length) == 0 &&
            send_all(browserfd, "\r\n", 2) == 0)
        {
            send_result = 0;
        }

        free(jpeg);

        if (send_result != 0)
        {
            printf("Browser disconnected\n");
            return -1;
        }

        frame_count++;
        if (frame_count % 100 == 0)
            printf("Forwarded %u frames\n", frame_count);

        /*
         * The input format requires a CRLF after the JPEG payload and before
         * the next boundary. Consume this blank line to keep the reader
         * synchronized with the next frame.
         */
        len = read_line(espfd, line, sizeof(line));
        if (len != 0)
        {
            fprintf(stderr, "Missing CRLF after JPEG frame\n");
            return -1;
        }
    }

    printf("Browser stream ended (%u frames forwarded)\n", frame_count);
    return 0;
}

int main(int argc, char *argv[])
{
    /* The ESP32-CAM host and port are required; the web port is optional. */
    if (argc < 3 || argc > 4)
    {
        fprintf(stderr,
                "Usage: %s <esp32_host> <esp32_port> [web_port]\n"
                "Example: %s 192.168.1.100 81 8080\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    /* Read command-line configuration before setting up signal handlers and sockets. */
    const char *esp32_host = argv[1];
    const char *esp32_port = argv[2];
    const char *web_port = (argc == 4) ? argv[3] : DEFAULT_WEB_PORT;

    /*
     * SIGINT/SIGTERM request an orderly shutdown of the loops.
     * Ignore SIGPIPE so send() reports failure through its return value
     * instead of terminating the process when a client closes its socket.
     */
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    /* Connect to the MJPEG source first;
     * without it, the gateway cannot serve video.
     */
    int espfd = start_esp32_stream(esp32_host, esp32_port);
    if (espfd < 0)
        return EXIT_FAILURE;

    /* Open the web port only after the video source has been validated. */
    int listenfd = create_http_server(web_port);
    if (listenfd < 0)
    {
        perror("Could not create HTTP server");
        close(espfd);
        return EXIT_FAILURE;
    }

    printf("HTTP server listening on port %s\n", web_port);
    printf("Open in Chrome: http://<RASPBERRY_PI_IP>:%s/\n", web_port);
    printf("Press Ctrl+C to stop\n");

    /*
     * Handle clients sequentially: accept a socket, read one HTTP request,
     * route by method and path, then close the socket. A /stream request
     * occupies the loop until the stream ends or the client disconnects.
     */
    while (running)
    {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int clientfd = accept(listenfd,
                              (struct sockaddr *)&client_addr,
                              &addr_len);

        if (clientfd < 0)
        {
            if (errno == EINTR && running)
                continue;
            if (running)
                perror("accept");
            break;
        }

        char request[REQUEST_SIZE];
        int request_len = read_http_request(clientfd, request, sizeof(request));

        if (request_len < 0)
        {
            close(clientfd);
            continue;
        }

        char method[16] = {0};
        char path[256] = {0};

        /* Only GET is supported; other methods receive a 404 in this implementation. */
        if (sscanf(request, "%15s %255s", method, path) != 2 || strcmp(method, "GET") != 0)
        {
            send_http_not_found(clientfd);
        }
        else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
        {
            send_http_page(clientfd);
        }
        else if (strcmp(path, "/stream") == 0)
        {
            (void)stream_to_browser(espfd, clientfd);
        }
        else
        {
            send_http_not_found(clientfd);
        }

        close(clientfd);
    }

    /* Close the shared sockets before exiting to release system resources. */
    close(listenfd);
    close(espfd);

    printf("\nMJPEG gateway stopped\n");
    return EXIT_SUCCESS;
}
