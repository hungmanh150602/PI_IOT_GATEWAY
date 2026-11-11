/*
 * Receive JPEG frames over a custom UDP packet format, reassemble each frame,
 * and expose the completed images to browsers as an HTTP MJPEG stream.
 * The root URL serves a small HTML page; /stream serves the multipart feed.
 */
#define _POSIX_C_SOURCE 200809L

/* POSIX networking, signal, I/O, and multiplexing interfaces used below. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Default listening ports and packet-format limits.
 * Each UDP packet has a fixed-size header followed by one JPEG payload chunk;
 * frame and packet limits bound memory use and validate incoming datagrams.
 */
#define DEFAULT_UDP_PORT "5000"
#define DEFAULT_WEB_PORT "8080"
#define UDP_DATAGRAM_SIZE 1200U
#define UDP_HEADER_SIZE 24U
#define UDP_PAYLOAD_SIZE (UDP_DATAGRAM_SIZE - UDP_HEADER_SIZE)
#define MAX_FRAME_SIZE (2U * 1024U * 1024U)
#define MAX_PACKETS ((MAX_FRAME_SIZE + UDP_PAYLOAD_SIZE - 1U) / UDP_PAYLOAD_SIZE)
#define REQUEST_SIZE 4096U
#define LINE_SIZE 1024U
#define PROTOCOL_VERSION 1U

/* Signal-safe flag used by the server loops to request an orderly shutdown. */
static volatile sig_atomic_t running = 1;

/* Ask select()/receive loops to exit after SIGINT or SIGTERM. */
static void handle_signal(int signo)
{
    (void)signo;
    running = 0;
}

/*
 * Decode a 16-bit unsigned integer stored in network byte order (big-endian).
 * The caller must provide at least two bytes at p.
 */
static uint16_t get_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/*
 * Decode a 32-bit unsigned integer stored in network byte order (big-endian).
 * The caller must provide at least four bytes at p.
 */
static uint32_t get_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Send a complete HTTP response segment. Because send() may perform a partial
 * write, continue from the first unsent byte; retry interrupted calls while
 * the service is running and return -1 if the connection fails.
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

/*
 * Create and bind an IPv4 UDP socket on the requested port and all local
 * interfaces. SO_REUSEADDR permits prompt rebinding after a previous run.
 * Return the bound socket descriptor, or -1 if creation or bind fails.
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
    /* Convert the textual port to network byte order for sockaddr_in. */
    addr.sin_port = htons((uint16_t)strtoul(port, NULL, 10));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * Create an IPv4 TCP socket for HTTP clients, bind it on all local interfaces,
 * and start listening with a small pending-connection queue.
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
    /* Configure the address for HTTP clients on the selected listening port. */
    addr.sin_port = htons((uint16_t)strtoul(port, NULL, 10));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * Read the client's HTTP request through the end of its headers (CRLF CRLF).
 * The request is bounded by capacity and null-terminated after each receive.
 * Return the number of bytes read, or -1 for a closed connection, socket
 * error, or request that fills the buffer without completing its headers.
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

/*
 * Send a self-contained HTML page whose img element opens /stream.
 * The browser displays the successive JPEG parts of that MJPEG response as
 * a live image; Content-Length excludes the page array's terminating NUL.
 */
static void send_http_page(int fd)
{
    static const char page[] =
        "<!doctype html><html lang=\"en\"><head>"
        "<meta charset=\"utf-8\"><meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32-CAM UDP Live Stream</title>"
        "<style>body{margin:0;padding:24px;background:#171717;color:#eee;"
        "font:16px Arial,sans-serif;text-align:center}"
        "img{display:block;width:100%;max-width:640px;height:auto;"
        "margin:20px auto;background:#000}</style></head><body>"
        "<h2>ESP32-CAM Live Stream (UDP)</h2>"
        "<img src=\"/stream\" alt=\"Waiting for stream...\">"
        "</body></html>";

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

/* Return a minimal 404 response for unsupported methods or request paths. */
static void send_not_found(int fd)
{
    static const char response[] = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n";

    (void)send_all(fd, response, sizeof(response) - 1U);
}

/*
 * State required to assemble one JPEG frame from possibly reordered UDP
 * datagrams. received[] tracks packet indexes so duplicates are ignored;
 * newest_frame_id prevents delayed packets from rolling the assembler back.
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
 * Begin assembling a new frame and clear the packet-received bitmap.
 * The data buffer itself is reused; packet payloads will overwrite their
 * respective offsets as they arrive.
 */
static void reset_frame(struct frame_assembler *a, uint32_t frame_id,
                        uint32_t frame_size, uint16_t packet_count)
{
    memset(a->received, 0, MAX_PACKETS);
    a->frame_id = frame_id;
    a->frame_size = frame_size;
    a->packet_count = packet_count;
    a->received_count = 0;
    a->active = 1;
}

/*
 * Receive and validate one UDP datagram, then copy its payload into the
 * corresponding offset of the current frame buffer.
 * Return 1 when all packets for a frame have arrived, 0 for an incomplete or
 * invalid datagram, and -1 for a fatal socket error. The frame buffer is reused.
 */
static int receive_udp_packet(int udpfd, struct frame_assembler *a)
{
    /*
     * One extra byte lets validation detect datagrams larger than the maximum
     * permitted packet size instead of silently accepting a truncated packet.
     */
    uint8_t packet[UDP_DATAGRAM_SIZE + 1U];
    ssize_t n;

    do
    {
        n = recv(udpfd, packet, sizeof(packet), 0);
    } while (n < 0 && errno == EINTR && running);

    if (n < 0)
        return running ? -1 : 0;

    /*
     * Validate the fixed header before reading its fields: minimum/maximum
     * packet size, four-byte magic, protocol version, and declared header size.
     */
    if ((size_t)n < UDP_HEADER_SIZE || (size_t)n > UDP_DATAGRAM_SIZE ||
        memcmp(packet, "MJPG", 4) != 0 || packet[4] != PROTOCOL_VERSION ||
        get_u16_be(packet + 6) != UDP_HEADER_SIZE)

        return 0;

    /*
     * Decode packet metadata. Multi-byte integers in this custom wire format
     * are stored in big-endian order; payload_size describes bytes after the
     * fixed UDP header.
     */
    const uint32_t frame_id = get_u32_be(packet + 8);
    const uint16_t packet_index = get_u16_be(packet + 12);
    const uint16_t packet_count = get_u16_be(packet + 14);
    const uint32_t frame_size = get_u32_be(packet + 16);
    const uint16_t payload_size = get_u16_be(packet + 20);

    /* Reject inconsistent or out-of-range metadata before calculating offsets. */
    if (frame_size == 0 || frame_size > MAX_FRAME_SIZE ||
        packet_count == 0 || packet_count > MAX_PACKETS ||
        packet_index >= packet_count || payload_size == 0 ||
        (size_t)n != UDP_HEADER_SIZE + (size_t)payload_size)

        return 0;

    /*
     * A frame is split into fixed-size payload chunks except for its final
     * packet. Derive the expected packet count and payload length from the
     * total frame size to reject gaps, impossible indexes, and malformed tails.
     */
    const uint32_t expected_count = (frame_size + (uint32_t)UDP_PAYLOAD_SIZE - 1U) /
                                    (uint32_t)UDP_PAYLOAD_SIZE;

    /* Calculate where this packet belongs in the assembled JPEG buffer. */
    const size_t offset = (size_t)packet_index * UDP_PAYLOAD_SIZE;

    size_t expected_payload = (size_t)frame_size - offset;

    if (expected_count != packet_count || offset >= frame_size)
        return 0;

    if (expected_payload > UDP_PAYLOAD_SIZE)
        expected_payload = UDP_PAYLOAD_SIZE;

    if (payload_size != expected_payload)
        return 0;

    if (!a->active || a->frame_id != frame_id)
    {
        /*
         * Ignore delayed packets from an older frame. Signed subtraction
         * handles uint32_t frame_id wraparound when frame IDs are reasonably
         * close, as they are for consecutive live frames.
         */
        if (a->have_newest_frame_id && (int32_t)(frame_id - a->newest_frame_id) <= 0)
            return 0;

        /* Start collecting this newer frame and reset its received-packet map. */
        a->newest_frame_id = frame_id;
        a->have_newest_frame_id = 1;

        reset_frame(a, frame_id, frame_size, packet_count);
    }
    else if (a->frame_size != frame_size || a->packet_count != packet_count)
    {
        /* Packets for one frame ID must agree on the frame's declared shape. */
        return 0;
    }

    /* Copy each packet only once so retransmissions do not inflate the count. */
    if (a->received[packet_index] == 0)
    {
        memcpy(a->data + offset, packet + UDP_HEADER_SIZE, payload_size);

        a->received[packet_index] = 1;
        ++a->received_count;
    }

    /* The caller can forward the frame only after every declared packet arrived. */
    return a->received_count == a->packet_count ? 1 : 0;
}

/*
 * Send the HTTP response headers for an indefinite multipart MJPEG response.
 * Each subsequent JPEG image is framed with boundary=frame and its own
 * Content-Length header.
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

/*
 * Check whether the browser has closed its connection without consuming any
 * pending request data. MSG_PEEK leaves received bytes in the socket; the
 * nonblocking flag prevents this liveness check from stalling the stream loop.
 */
static int browser_disconnected(int fd)
{
    uint8_t byte;

    ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);

    if (n == 0)
        return 1;

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        return 1;

    return 0;
}

/*
 * Serve one browser's MJPEG response while continuing to receive UDP packets.
 * select() watches both sockets so browser disconnects are noticed promptly
 * and packet arrival can complete frames for immediate forwarding.
 * Return 0 when the client disconnects or shutdown is requested, and -1 on
 * socket or forwarding failure.
 */
static int stream_to_browser(int udpfd, int browserfd,
                             struct frame_assembler *assembler)
{
    unsigned long forwarded = 0;

    /* Start the HTTP multipart response before forwarding any frame payloads. */
    if (send_mjpeg_headers(browserfd) != 0)
        return -1;

    printf("Browser connected; forwarding complete UDP frames\n");

    while (running)
    {
        /*
         * Monitor both the camera's UDP socket and browser socket. A timeout
         * periodically rechecks the running flag even when neither is readable.
         */
        fd_set readfds;

        FD_ZERO(&readfds);
        FD_SET(udpfd, &readfds);
        FD_SET(browserfd, &readfds);

        int maxfd = udpfd > browserfd ? udpfd : browserfd;

        struct timeval timeout = {
            .tv_sec = 1,
            .tv_usec = 0};

        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }

        /* Detect a browser that closed its stream while no frame was arriving. */
        if (FD_ISSET(browserfd, &readfds) && browser_disconnected(browserfd))
        {
            printf("Browser disconnected\n");
            return 0;
        }

        /* Add each received datagram to the current frame assembly. */
        if (FD_ISSET(udpfd, &readfds))
        {
            int result = receive_udp_packet(udpfd, assembler);

            if (result < 0)
                return -1;

            if (result == 1)
            {
                /* Emit one complete JPEG as a single multipart HTTP section. */
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

                    printf("Could not forward frame; browser likely disconnected\n");
                    return -1;
                }

                /* Mark this frame inactive; the next valid packet starts a new one. */
                ++forwarded;

                assembler->active = 0;

                if (forwarded % 100UL == 0UL)
                    printf("Forwarded %lu complete frames\n", forwarded);
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    /* Accept optional UDP and HTTP ports; use defaults when omitted. */
    if (argc > 3)
    {
        fprintf(stderr,
                "Usage: %s [udp_port] [web_port]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    /* Select port arguments without copying their strings. */
    const char *udp_port = argc >= 2 ? argv[1] : DEFAULT_UDP_PORT;
    const char *web_port = argc >= 3 ? argv[2] : DEFAULT_WEB_PORT;

    /* Convert termination signals into orderly loop exits and ignore SIGPIPE. */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    /*
     * Allocate one maximum-sized JPEG buffer and a bitmap with one byte per
     * possible packet. Keeping these allocations for the server lifetime
     * avoids repeated allocation while receiving frames.
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

    /* Bind the UDP socket before starting the HTTP service. */
    int udpfd = create_udp_receiver(udp_port);

    if (udpfd < 0)
    {
        perror("UDP bind");

        free(assembler.data);
        free(assembler.received);

        return EXIT_FAILURE;
    }

    /* Start the TCP listener used by browsers to request the page or stream. */
    int listenfd = create_http_server(web_port);

    if (listenfd < 0)
    {
        perror("HTTP listen");

        close(udpfd);

        free(assembler.data);
        free(assembler.received);

        return EXIT_FAILURE;
    }

    printf("UDP receiver listening on port %s\n", udp_port);
    printf("HTTP server listening on port %s\n", web_port);
    printf("Open Chrome: http://<RASPBERRY_PI_IP>:%s/\n", web_port);
    printf("Press Ctrl+C to stop\n");

    /*
     * The main loop serves ordinary page requests and keeps draining UDP when
     * no browser is connected. This prevents the receive queue from growing
     * without bound during idle periods; incomplete idle frames are discarded.
     */
    while (running)
    {
        /* Wait for camera datagrams or a new browser connection, with shutdown polling. */
        fd_set readfds;

        FD_ZERO(&readfds);
        FD_SET(udpfd, &readfds);
        FD_SET(listenfd, &readfds);

        int maxfd = udpfd > listenfd ? udpfd : listenfd;

        struct timeval timeout = {
            .tv_sec = 1,
            .tv_usec = 0};

        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            perror("select");
            break;
        }

        /*
         * Drain camera traffic while no client is being handled. Completed
         * frames are marked inactive because there is no browser to receive
         * them; the newest frame ID is retained to reject delayed old packets.
         */
        if (FD_ISSET(udpfd, &readfds))
        {
            int result = receive_udp_packet(udpfd, &assembler);

            if (result < 0)
                perror("recv UDP");

            if (result == 1)
                assembler.active = 0;
        }

        /* Accept and handle one browser request when the listening socket is ready. */
        if (FD_ISSET(listenfd, &readfds))
        {
            int clientfd = accept(listenfd, NULL, NULL);

            if (clientfd < 0)
            {
                if (errno != EINTR && running)
                    perror("accept");

                continue;
            }

            /* Read and parse the request line after collecting the HTTP headers. */
            char request[REQUEST_SIZE];

            int n = read_http_request(clientfd, request, sizeof(request));

            char method[16] = {0};
            char path[256] = {0};

            /* Route supported GET requests to the page or continuous MJPEG stream. */
            if (n > 0 &&
                sscanf(request, "%15s %255s", method, path) == 2 &&
                strcmp(method, "GET") == 0 &&
                (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0))
            {

                send_http_page(clientfd);
            }
            else if (n > 0 &&
                     sscanf(request, "%15s %255s", method, path) == 2 &&
                     strcmp(method, "GET") == 0 &&
                     strcmp(path, "/stream") == 0)
            {
                /* Keep this client open until streaming ends or it disconnects. */
                (void)stream_to_browser(udpfd, clientfd, &assembler);
            }
            else
            {
                send_not_found(clientfd);
            }

            /* Each request is handled synchronously, so close its client socket here. */
            close(clientfd);
        }
    }

    /* Release listening sockets and frame-assembly buffers during shutdown. */
    close(listenfd);
    close(udpfd);

    free(assembler.data);
    free(assembler.received);

    printf("\nUDP MJPEG gateway stopped\n");

    return EXIT_SUCCESS;
}