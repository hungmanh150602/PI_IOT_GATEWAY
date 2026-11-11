/*
 * Raspberry Pi UDP MJPEG Gateway
 *
 * Functions:
 * 1. Send START_STREAM command to ESP32-CAM.
 * 2. Receive JPEG packets over UDP.
 * 3. Reassemble UDP packets into complete JPEG frames.
 * 4. Serve frames to browsers through HTTP MJPEG.
 *
 * UDP:
 *   Raspberry Pi local port : 5433
 *   ESP32-CAM control port  : 5432
 *
 * HTTP:
 *   Web server port         : 6868
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* =====================================================
 * Configuration
 * =====================================================
 */
#define DEFAULT_PI_UDP_PORT "5433"
#define DEFAULT_ESP32_UDP_PORT "5432"
#define DEFAULT_WEB_PORT "6868"

#define CONTROL_COMMAND "START_STREAM"
#define HEARTBEAT_INTERVAL_MS 1000U

#define UDP_DATAGRAM_SIZE 1200U
#define UDP_HEADER_SIZE 24U
#define UDP_PAYLOAD_SIZE (UDP_DATAGRAM_SIZE - UDP_HEADER_SIZE)

#define MAX_FRAME_SIZE (2U * 1024U * 1024U)
#define MAX_PACKETS \
    ((MAX_FRAME_SIZE + UDP_PAYLOAD_SIZE - 1U) / UDP_PAYLOAD_SIZE)

#define REQUEST_SIZE 4096U
#define PROTOCOL_VERSION 1U

/* =====================================================
 * Shared process state
 * =====================================================
 */
static volatile sig_atomic_t running = 1;

static struct sockaddr_in esp32_address;

static uint64_t last_heartbeat_ms = 0;

/* =====================================================
 * Signal handling
 * =====================================================
 * Set the shutdown flag so the main and streaming loops can exit cleanly.
 */
static void handle_signal(int signo)
{
    (void)signo;
    running = 0;
}

/* =====================================================
 * Monotonic time helper
 * =====================================================
 *
 * Return the current monotonic time in milliseconds. This clock is used for
 * heartbeat intervals because it is not affected by wall-clock adjustments.
 * Return zero if the clock query fails.
 */
static uint64_t get_monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* =====================================================
 * Big-endian integer decoding
 * =====================================================
 *
 * Decode two bytes in network byte order; p must point to at least two bytes.
 */
static uint16_t get_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Decode four bytes in network byte order; p must point to at least four bytes. */
static uint32_t get_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* =====================================================
 * ESP32 stream control
 * =====================================================
 *
 * Send START_STREAM to the configured ESP32-CAM destination. On success,
 * record the send time so the heartbeat interval can be measured. Return -1
 * if sendto() fails or does not report the complete command length.
 */
static int send_start_stream(int udpfd)
{
    const char command[] = CONTROL_COMMAND;

    ssize_t n = sendto(udpfd, command, sizeof(command) - 1U, 0,
                       (struct sockaddr *)&esp32_address,
                       sizeof(esp32_address));

    if (n < 0)
    {
        perror("sendto START_STREAM");
        return -1;
    }

    if ((size_t)n != sizeof(command) - 1U)
    {
        fprintf(stderr, "Incomplete START_STREAM command\n");
        return -1;
    }

    last_heartbeat_ms = get_monotonic_ms();

    return 0;
}

/*
 * Send START_STREAM immediately on the first call, then resend it whenever
 * HEARTBEAT_INTERVAL_MS has elapsed to keep the camera stream active.
 */
static void maintain_stream_connection(int udpfd)
{
    uint64_t now = get_monotonic_ms();

    if (last_heartbeat_ms == 0 || now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS)
    {
        (void)send_start_stream(udpfd);
    }
}

/* =====================================================
 * TCP response transmission
 * =====================================================
 *
 * Send all bytes of an HTTP response segment, handling partial writes and
 * interrupted system calls. Return -1 if sending fails or shutdown begins
 * before the entire buffer has been transmitted.
 */
static int send_all(int fd, const void *data, size_t length)
{
    const uint8_t *p = data;
    size_t sent = 0;

    while (sent < length && running)
    {
        ssize_t n = send(fd, p + sent, length - sent, MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR && running)
                continue;

            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return sent == length ? 0 : -1;
}

/* =====================================================
 * UDP receiver socket
 * =====================================================
 *
 * Create an IPv4 UDP socket, allow address reuse, and bind it to the specified
 * local port on all network interfaces. Return its descriptor or -1 if socket
 * creation or binding fails.
 */
static int create_udp_receiver(const char *port)
{
    struct sockaddr_in addr;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return -1;

    int reuse = 1;

    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)strtoul(port, NULL, 10));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

/* =====================================================
 * HTTP listener socket
 * =====================================================
 *
 * Create an IPv4 TCP listening socket on the specified local port and all
 * interfaces. The listen backlog limits pending connections awaiting accept().
 */
static int create_http_server(const char *port)
{
    struct sockaddr_in addr;

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    int reuse = 1;

    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)strtoul(port, NULL, 10));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

/* =====================================================
 * HTTP request input
 * =====================================================
 *
 * Read the browser request through the CRLF-CRLF header terminator. The
 * request is bounded by capacity and null-terminated after each receive.
 * Return the number of bytes read, or -1 if the client closes, an error
 * occurs, or the request does not fit in the provided buffer.
 */
static int read_http_request(int fd, char *buffer, size_t capacity)
{
    size_t used = 0;

    while (used < capacity - 1U && running)
    {
        ssize_t n = recv(fd, buffer + used, capacity - 1U - used, 0);

        if (n == 0)
            return -1;

        if (n < 0)
        {
            if (errno == EINTR && running)
                continue;

            return -1;
        }

        used += (size_t)n;
        buffer[used] = '\0';

        if (strstr(buffer, "\r\n\r\n") != NULL)
            return (int)used;
    }

    return -1;
}

/* =====================================================
 * HTTP page and error response
 * =====================================================
 *
 * Send a static HTML page whose image element loads /stream. The browser then
 * displays the multipart JPEG response as a live image. Content-Length counts
 * the HTML bytes but excludes the string's terminating NUL.
 */
static void send_http_page(int fd)
{
    static const char page[] =
        "<!doctype html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32-CAM UDP Live Stream</title>"
        "<style>"
        "body{margin:0;padding:24px;background:#171717;color:#eee;"
        "font:16px Arial,sans-serif;text-align:center}"
        "img{display:block;width:100%;max-width:640px;height:auto;"
        "margin:20px auto;background:#000}"
        "</style>"
        "</head>"
        "<body>"
        "<h2>ESP32-CAM Live Stream (UDP)</h2>"
        "<img src=\"/stream\" alt=\"Waiting for stream...\">"
        "</body>"
        "</html>";

    char header[256];

    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     sizeof(page) - 1U);

    if (n > 0 && (size_t)n < sizeof(header))
    {
        (void)send_all(fd, header, (size_t)n);
        (void)send_all(fd, page, sizeof(page) - 1U);
    }
}

/* Return a minimal 404 response for unsupported request methods or paths. */
static void send_not_found(int fd)
{
    static const char response[] = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n";

    (void)send_all(fd, response, sizeof(response) - 1U);
}

/* =====================================================
 * UDP frame assembly state
 * =====================================================
 *
 * The assembler holds one frame at a time. data stores JPEG bytes at offsets
 * derived from packet indexes; received records which indexes have arrived.
 * newest_frame_id helps reject delayed packets from previously seen frames.
 */
struct frame_assembler
{
    uint8_t *data;
    uint8_t *received;

    uint32_t frame_id;
    uint32_t frame_size;

    uint16_t packet_count;
    uint16_t received_count;

    uint32_t newest_frame_id;

    int have_newest_frame_id;
    int active;
};

/*
 * Reset the packet bitmap and metadata for a new frame while reusing existing
 * allocations. The data buffer is populated as that frame's packets arrive.
 */
static void reset_frame(struct frame_assembler *a,
                        uint32_t frame_id,
                        uint32_t frame_size,
                        uint16_t packet_count)
{
    memset(a->received, 0, MAX_PACKETS);

    a->frame_id = frame_id;
    a->frame_size = frame_size;
    a->packet_count = packet_count;

    a->received_count = 0;
    a->active = 1;
}

/* =====================================================
 * UDP packet validation and frame reassembly
 * =====================================================
 *
 * Receive one UDP datagram, validate its protocol header and frame metadata,
 * and copy its payload into the appropriate location in the current JPEG.
 * Return 1 when a complete frame is ready, 0 for an invalid/incomplete packet,
 * or -1 for a receive error. The caller owns and reuses the assembler buffers.
 */
static int receive_udp_packet(int udpfd,
                              struct frame_assembler *a)
{
    uint8_t packet[UDP_DATAGRAM_SIZE + 1U];

    ssize_t n;

    do
    {
        n = recv(udpfd, packet, sizeof(packet), 0);

    } while (n < 0 && errno == EINTR && running);

    if (n < 0)
        return running ? -1 : 0;

    /*
     * Validate the fixed packet header before interpreting metadata: total
     * datagram size, protocol magic, version, and declared header length.
     */
    if ((size_t)n < UDP_HEADER_SIZE || (size_t)n > UDP_DATAGRAM_SIZE ||
        memcmp(packet, "MJPG", 4) != 0 || packet[4] != PROTOCOL_VERSION ||
        get_u16_be(packet + 6) != UDP_HEADER_SIZE)
    {
        return 0;
    }

    /*
     * Decode the frame identity, packet position/count, total JPEG size, and
     * payload length. Multi-byte fields in this wire protocol use big-endian
     * (network byte) order.
     */
    uint32_t frame_id = get_u32_be(packet + 8);

    uint16_t packet_index = get_u16_be(packet + 12);
    uint16_t packet_count = get_u16_be(packet + 14);

    uint32_t frame_size = get_u32_be(packet + 16);

    uint16_t payload_size = get_u16_be(packet + 20);

    /*
     * Reject inconsistent or out-of-range values before using them to compute
     * a buffer offset or copy any packet payload.
     */
    if (frame_size == 0 || frame_size > MAX_FRAME_SIZE ||
        packet_count == 0 || packet_count > MAX_PACKETS ||
        packet_index >= packet_count || payload_size == 0 ||
        (size_t)n != UDP_HEADER_SIZE + (size_t)payload_size)
    {
        return 0;
    }

    /*
     * Each packet carries one UDP_PAYLOAD_SIZE chunk except the final packet.
     * Derive the expected packet count and tail size from frame_size so
     * malformed packet layouts cannot write outside the frame buffer.
     */
    uint32_t expected_count = (frame_size + UDP_PAYLOAD_SIZE - 1U) /
                              UDP_PAYLOAD_SIZE;

    size_t offset = (size_t)packet_index * UDP_PAYLOAD_SIZE;

    if (expected_count != packet_count || offset >= frame_size)
    {
        return 0;
    }

    size_t expected_payload = frame_size - offset;

    if (expected_payload > UDP_PAYLOAD_SIZE)
        expected_payload = UDP_PAYLOAD_SIZE;

    if (payload_size != expected_payload)
        return 0;

    /*
     * Start a fresh assembly when a newer frame arrives. Signed subtraction
     * tolerates uint32_t wraparound for nearby IDs; older or delayed frames
     * are ignored.
     */
    if (!a->active || a->frame_id != frame_id)
    {
        if (a->have_newest_frame_id && (int32_t)(frame_id - a->newest_frame_id) <= 0)
        {
            return 0;
        }

        a->newest_frame_id = frame_id;
        a->have_newest_frame_id = 1;

        reset_frame(a, frame_id, frame_size, packet_count);
    }
    else if (a->frame_size != frame_size || a->packet_count != packet_count)
    {
        /* A frame ID must keep the same declared size and packet count. */
        return 0;
    }

    /* Copy each packet once; duplicates must not increment the count twice. */
    if (a->received[packet_index] == 0)
    {
        memcpy(a->data + offset, packet + UDP_HEADER_SIZE, payload_size);

        a->received[packet_index] = 1;
        ++a->received_count;
    }

    /* A frame is complete only after every packet index has been received. */
    return a->received_count == a->packet_count ? 1 : 0;
}

/* =====================================================
 * HTTP MJPEG response headers
 * =====================================================
 *
 * Begin the browser response as an indefinite multipart MJPEG stream.
 * Individual JPEG frames are sent later as parts using boundary=frame.
 */
static int send_mjpeg_headers(int browserfd)
{
    static const char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";

    return send_all(browserfd, header, sizeof(header) - 1U);
}

/* =====================================================
 * Browser connection monitoring
 * =====================================================
 *
 * Check whether the browser closed its socket without consuming pending data.
 * MSG_PEEK preserves any client bytes; MSG_DONTWAIT prevents this check from
 * blocking the stream loop.
 */
static int browser_disconnected(int fd)
{
    uint8_t byte;

    ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);

    if (n == 0)
        return 1;

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
    {
        return 1;
    }

    return 0;
}

/* =====================================================
 * MJPEG forwarding to a browser
 * =====================================================
 *
 * Keep the camera stream alive and forward each fully assembled JPEG to one
 * browser as a multipart HTTP section. select() monitors UDP input and browser
 * closure so frame reception and client liveness are handled together.
 * Return 0 when streaming ends normally or the client disconnects, and -1
 * when socket I/O or frame forwarding fails.
 */
static int stream_to_browser(int udpfd,
                             int browserfd,
                             struct frame_assembler *assembler)
{
    unsigned long forwarded = 0;

    if (send_mjpeg_headers(browserfd) != 0)
        return -1;

    printf("Browser connected; forwarding UDP frames\n");

    while (running)
    {
        /* Periodically refresh the camera command while serving a browser. */
        maintain_stream_connection(udpfd);

        /*
         * Wait briefly for camera datagrams or browser activity. The timeout
         * lets the loop recheck shutdown and heartbeat state when idle.
         */
        fd_set readfds;

        FD_ZERO(&readfds);

        FD_SET(udpfd, &readfds);
        FD_SET(browserfd, &readfds);

        int maxfd = udpfd > browserfd ? udpfd : browserfd;

        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = 200000};

        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            perror("select stream");
            return -1;
        }

        if (ready == 0)
            continue;

        /* A readable browser socket may indicate that the peer has disconnected. */
        if (FD_ISSET(browserfd, &readfds) && browser_disconnected(browserfd))
        {
            printf("Browser disconnected\n");
            return 0;
        }

        /* Process one ready UDP packet and forward its frame when assembly completes. */
        if (FD_ISSET(udpfd, &readfds))
        {
            int result = receive_udp_packet(udpfd, assembler);

            if (result < 0)
            {
                perror("receive UDP");
                return -1;
            }

            if (result == 1)
            {
                /* Describe the completed JPEG as one part of the MJPEG response. */
                char part_header[192];

                int hlen = snprintf(part_header,
                                    sizeof(part_header),
                                    "--frame\r\n"
                                    "Content-Type: image/jpeg\r\n"
                                    "Content-Length: %u\r\n\r\n",
                                    assembler->frame_size);

                if (hlen < 0 || (size_t)hlen >= sizeof(part_header) ||
                    send_all(browserfd, part_header, (size_t)hlen) != 0 ||
                    send_all(browserfd, assembler->data, assembler->frame_size) != 0 ||
                    send_all(browserfd, "\r\n", 2) != 0)
                {
                    printf("Could not forward frame\n");
                    return -1;
                }

                ++forwarded;

                assembler->active = 0;

                if (forwarded % 100UL == 0UL)
                {
                    printf("Forwarded %lu frames\n", forwarded);
                }
            }
        }
    }

    return 0;
}

/* =====================================================
 * Program entry point and server lifecycle
 * =====================================================
 */
int main(int argc, char **argv)
{
    /*
     * Usage:
     * ./stream_video_udp_web <ESP32_IP> [ESP32_CONTROL_PORT] [WEB_PORT]
     * The Raspberry Pi UDP receive port is fixed by DEFAULT_PI_UDP_PORT.
     */
    if (argc < 2 || argc > 4)
    {
        fprintf(stderr,
                "Usage: %s <ESP32_IP> [ESP32_CONTROL_PORT] [WEB_PORT]\n"
                "Example: %s 192.168.1.100 5432 6868\n",
                argv[0], argv[0]);

        return EXIT_FAILURE;
    }

    /* Select optional control and web ports, falling back to configured defaults. */
    const char *esp32_ip = argv[1];
    const char *esp32_port = argc >= 3 ? argv[2] : DEFAULT_ESP32_UDP_PORT;
    const char *web_port = argc >= 4 ? argv[3] : DEFAULT_WEB_PORT;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    /*
     * Build the ESP32-CAM IPv4 destination used by sendto(). Incoming video
     * is received on the Raspberry Pi's UDP port; this structure configures
     * the destination for outgoing control commands.
     */
    memset(&esp32_address, 0, sizeof(esp32_address));

    esp32_address.sin_family = AF_INET;
    esp32_address.sin_port = htons((uint16_t)strtoul(esp32_port, NULL, 10));

    /* Reject invalid IPv4 text before attempting to open sockets. */
    if (inet_pton(AF_INET, esp32_ip, &esp32_address.sin_addr) != 1)
    {
        fprintf(stderr, "Invalid ESP32 IP address: %s\n", esp32_ip);
        return EXIT_FAILURE;
    }

    /*
     * Allocate reusable storage for the largest permitted JPEG and a bitmap
     * with one entry per possible packet. Both allocations live until shutdown.
     */
    struct frame_assembler assembler = {0};

    assembler.data = malloc(MAX_FRAME_SIZE);
    assembler.received = calloc(MAX_PACKETS, 1U);

    if (assembler.data == NULL || assembler.received == NULL)
    {
        perror("frame assembler allocation");

        free(assembler.data);
        free(assembler.received);

        return EXIT_FAILURE;
    }

    /* Bind the UDP receiver to the Raspberry Pi port configured for camera data. */
    int udpfd = create_udp_receiver(DEFAULT_PI_UDP_PORT);

    if (udpfd < 0)
    {
        perror("UDP bind");

        free(assembler.data);
        free(assembler.received);

        return EXIT_FAILURE;
    }

    /* Create the HTTP listener used by browsers to request the page and stream. */
    int listenfd = create_http_server(web_port);

    if (listenfd < 0)
    {
        perror("HTTP listen");

        close(udpfd);

        free(assembler.data);
        free(assembler.received);

        return EXIT_FAILURE;
    }

    /* Print the active endpoints and the URL users can open in a browser. */
    printf("\n");
    printf("====================================\n");
    printf(" Raspberry Pi UDP MJPEG Gateway\n");
    printf("====================================\n");
    printf("ESP32 destination : %s:%s\n", esp32_ip, esp32_port);
    printf("Pi UDP receiver   : %s\n", DEFAULT_PI_UDP_PORT);
    printf("HTTP server       : %s\n", web_port);
    printf("Open browser      : http://<PI_IP>:%s/\n", web_port);
    printf("Press Ctrl+C to stop\n\n");

    /* Request the camera stream immediately instead of waiting for a heartbeat. */
    (void)send_start_stream(udpfd);

    printf("Sent START_STREAM to ESP32-CAM\n");

    /*
     * Service camera traffic and browser connections until a termination
     * signal is received. UDP packets are still drained when no browser is
     * connected so the socket receive queue does not grow indefinitely.
     */
    while (running)
    {
        /* Maintain the camera stream independently of browser activity. */
        maintain_stream_connection(udpfd);

        /* Wait for incoming UDP data, a browser connection, or timeout polling. */
        fd_set readfds;

        FD_ZERO(&readfds);

        FD_SET(udpfd, &readfds);
        FD_SET(listenfd, &readfds);

        int maxfd = udpfd > listenfd ? udpfd : listenfd;

        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = 200000};

        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            perror("select");
            break;
        }

        if (ready == 0)
            continue;

        /*
         * Continue consuming camera datagrams while the main loop is available.
         * A completed frame is discarded because no browser is waiting for it.
         */
        if (FD_ISSET(udpfd, &readfds))
        {
            int result = receive_udp_packet(udpfd, &assembler);

            if (result < 0)
            {
                perror("receive UDP");
            }
            else if (result == 1)
            {
                /* Retain the newest ID but allow the next frame to start. */
                assembler.active = 0;
            }
        }

        /* Accept one browser connection and route its HTTP request. */
        if (FD_ISSET(listenfd, &readfds))
        {
            int clientfd = accept(listenfd, NULL, NULL);

            if (clientfd < 0)
            {
                if (errno != EINTR && running)
                    perror("accept");

                continue;
            }

            /* Read the complete request headers before extracting method and path. */
            char request[REQUEST_SIZE];

            int n = read_http_request(clientfd, request, sizeof(request));

            char method[16] = {0};
            char path[256] = {0};

            /* Serve the page, begin streaming, or return 404 for other requests. */
            if (n > 0 &&
                sscanf(request, "%15s %255s", method, path) == 2 &&
                strcmp(method, "GET") == 0 &&
                (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0))
            {
                send_http_page(clientfd);
            }
            else if (n > 0 &&
                     sscanf(request, "%15s %255s", method, path) == 2 &&
                     strcmp(method, "GET") == 0 && strcmp(path, "/stream") == 0)
            {
                (void)stream_to_browser(udpfd, clientfd, &assembler);
            }
            else
            {
                send_not_found(clientfd);
            }

            close(clientfd);
        }
    }

    close(listenfd);
    close(udpfd);

    free(assembler.data);
    free(assembler.received);

    printf("\nUDP MJPEG gateway stopped\n");

    return EXIT_SUCCESS;
}
