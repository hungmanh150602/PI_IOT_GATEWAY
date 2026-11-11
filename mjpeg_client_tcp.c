/*
 * Connect to an ESP32-CAM MJPEG endpoint, parse its multipart response, and
 * save each JPEG frame as a separate file in the specified output directory.
 * Existing regular files named with the "frame_" prefix are removed before
 * capture starts.
 */

/* Enable GNU extensions used by functions such as strcasestr(). */
#define _GNU_SOURCE

/* Standard C and POSIX headers for I/O, memory, signals, and error handling. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

#include <dirent.h>
#include <sys/stat.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

/* Network buffer sizes and the maximum accepted JPEG frame size. */
#define BUFFER_SIZE 4096
#define LINE_SIZE 1024
#define MAX_FRAME_SIZE (10U * 1024U * 1024U)

/* Shared shutdown flag updated by the signal handler. */
static volatile sig_atomic_t running = 1;

/* Stop the receive loop cleanly when the user interrupts the program. */
static void handle_sigint(int signo)
{
    (void)signo;
    running = 0;
}

/*
 * Send every byte of an HTTP request, retrying interrupted calls and
 * continuing after partial writes. Return -1 if sending fails or the peer
 * closes the connection before the complete request is transmitted.
 */
static int send_all(int sockfd, const char *data, size_t length)
{
    size_t total = 0;

    while (total < length)
    {
        ssize_t n = send(sockfd, data + total, length - total, 0);

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

    return 0;
}

/*
 * Read one CRLF-terminated protocol line from the TCP socket.
 * The returned, null-terminated string excludes CRLF. Return the line length,
 * or -1 if the connection closes, an error occurs, or the line exceeds the
 * supplied buffer capacity.
 */
static int read_line(int sockfd, char *buffer, size_t capacity)
{
    size_t pos = 0;
    char ch;

    if (capacity < 2)
        return -1;

    while (pos < capacity - 1)
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
    return -1; /* The line did not fit in the provided buffer. */
}

/*
 * Receive exactly length bytes from the socket and write them to the open
 * output file. Use a fixed-size temporary buffer so large frames do not need
 * to be allocated all at once; return -1 on an incomplete read or file error.
 */
static int receive_frame(int sockfd, FILE *fp, size_t length)
{
    unsigned char buffer[BUFFER_SIZE];
    size_t remaining = length;

    while (remaining > 0 && running)
    {
        size_t wanted = remaining < sizeof(buffer)
                            ? remaining
                            : sizeof(buffer);

        ssize_t received = recv(sockfd, buffer, wanted, 0);

        if (received == 0)
            /* The peer closed the connection before the frame was complete. */
            return -1;

        if (received < 0)
        {
            if (errno == EINTR && running)
                continue;
            return -1;
        }

        size_t written = fwrite(buffer, 1, (size_t)received, fp);

        if (written != (size_t)received)
        {
            perror("fwrite");
            return -1;
        }

        remaining -= (size_t)received;
    }

    return remaining == 0 ? 0 : -1;
}

/*
 * Parse the decimal value after the colon in a frame's Content-Length header.
 * Reject empty, malformed, zero, or oversized values before using the length
 * to determine how many bytes to receive and save.
 */
static int parse_content_length(const char *line, size_t *length)
{
    const char *colon = strchr(line, ':');

    if (colon == NULL)
        return -1;

    char *end = NULL;
    errno = 0;

    unsigned long value = strtoul(colon + 1, &end, 10);

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
 * Resolve the server host and port, then try each returned address until a
 * TCP connection succeeds. Return the connected socket descriptor, or -1
 * if name resolution fails or no candidate address can be connected to.
 */
static int connect_to_server(const char *host, const char *port)
{
    /*Structures used to resolve the host and iterate through candidate addresses.*/
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    int sockfd = -1;

    /* Clear optional address-resolution settings before configuring them. */
    memset(&hints, 0, sizeof(hints));
    /* Allow either IPv4 or IPv6 addresses. */
    hints.ai_family = AF_UNSPEC;
    /* Request a stream socket, which uses TCP. */
    hints.ai_socktype = SOCK_STREAM;

    /* Resolve the host name and service port into candidate socket addresses. */
    int ret = getaddrinfo(host, port, &hints, &result);

    if (ret != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    /* Try each resolved address until a socket connects successfully. */
    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sockfd < 0)
            continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            /* Keep this connected socket and stop searching the address list. */
            break;

        /* Discard the failed socket before trying the next candidate address. */
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result);
    return sockfd;
}

/*
 * Remove stale regular files whose names start with "frame_" from the output
 * directory. lstat() is used so symbolic links are not followed, and only
 * regular files are unlinked; directories and symlinks are left untouched.
 * Return -1 if the directory cannot be opened, otherwise return 0 after the scan.
 */
static int cleanup_frame_files(const char *output_dir)
{
    DIR *dir = opendir(output_dir);

    if (dir == NULL)
    {
        perror("opendir");
        return -1;
    }

    struct dirent *entry;
    char filepath[PATH_MAX];
    unsigned int deleted_count = 0;

    while ((entry = readdir(dir)) != NULL)
    {

        /* Ignore directory entries that do not use the frame-file prefix. */
        if (strncmp(entry->d_name, "frame_", 6) != 0)
            continue;

        /* Construct and bounds-check the full path before using filesystem calls. */
        int len = snprintf(filepath, sizeof(filepath), "%s/%s",
                           output_dir, entry->d_name);

        if (len < 0 || (size_t)len >= sizeof(filepath))
        {
            fprintf(stderr, "Path too long: %s\n", entry->d_name);
            continue;
        }

        struct stat st;

        if (lstat(filepath, &st) != 0)
        {
            perror("lstat");
            continue;
        }

        /* Do not remove directories, symbolic links, or other non-regular entries. */
        if (!S_ISREG(st.st_mode))
            continue;

        if (unlink(filepath) == 0)
        {
            deleted_count++;
            /* printf("Deleted: %s\n", filepath); */
        }
        else
        {
            perror("unlink");
        }
    }

    closedir(dir);

    printf("Cleanup completed: %u old frame file(s) deleted\n", deleted_count);

    return 0;
}

/*
 * Command-line arguments:
 * argv[0] - program name
 * argv[1] - ESP32-CAM host name or IP address
 * argv[2] - camera TCP port
 * argv[3] - directory in which JPEG frames are saved
 */
int main(int argc, char *argv[])
{
    /* Require the camera host, port, and destination directory. */
    if (argc != 4)
    {
        fprintf(stderr,
                "Usage: %s <host> <port> <output_directory>\n"
                "Example: ./build/mjpeg_client_tcp 192.168.1.100 81 images\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    /* Keep the caller-provided connection and output settings. */
    const char *host = argv[1];
    const char *port = argv[2];
    const char *output_dir = argv[3];

    /*
     * Remove previous frame captures first so the output directory contains
     * only files produced by this run. Other files are left untouched.
     */
    printf("Cleaning old frame files...\n");

    if (cleanup_frame_files(output_dir) != 0)
    {
        fprintf(stderr, "Failed to clean output directory\n");
        return EXIT_FAILURE;
    }

    /* Let Ctrl+C request a clean exit from the capture loop. */
    signal(SIGINT, handle_sigint);

    /* Establish the TCP connection to the camera before sending the request. */
    int sockfd = connect_to_server(host, port);

    if (sockfd < 0)
    {
        fprintf(stderr, "TCP connection failed\n");
        return EXIT_FAILURE;
    }

    printf("TCP connection established\n");

    /*
     * Build the HTTP/1.1 request for the camera's /stream endpoint.
     * Accept advertises that this client expects a multipart MJPEG response;
     * keep-alive keeps the TCP connection open while frames are streamed.
     */
    char request[1024];

    int request_len = snprintf(request, sizeof(request),
                               "GET /stream HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Accept: multipart/x-mixed-replace\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n",
                               host);

    /* Verify that the complete request fits in the fixed-size request buffer. */
    if (request_len < 0 || (size_t)request_len >= sizeof(request))
    {
        fprintf(stderr, "HTTP request is too long\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Send the complete request, including handling any partial socket writes. */
    if (send_all(sockfd, request, (size_t)request_len) != 0)
    {
        perror("send_all");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("HTTP GET request sent\n");

    /*
     * Read the response headers line by line until the blank line separating
     * the HTTP headers from the multipart body. Track the status and confirm
     * that the server uses the boundary expected by the frame parser.
     */
    char line[LINE_SIZE];
    int status_code = 0;
    int have_content_type = 0;

    while (running)
    {
        /* Read the next response-header line from the camera. */
        int len = read_line(sockfd, line, sizeof(line));

        if (len < 0)
        {
            fprintf(stderr, "Failed to read HTTP response header\n");
            close(sockfd);
            return EXIT_FAILURE;
        }

        if (len == 0)
            break;

        /*
         * Parse the status line and Content-Type. Other response headers are
         * not needed by this client and are ignored.
         */
        if (strncmp(line, "HTTP/", 5) == 0)
        {
            if (sscanf(line, "%*s %d", &status_code) != 1)
            {
                fprintf(stderr, "Invalid HTTP status line\n");
                close(sockfd);
                return EXIT_FAILURE;
            }
        }
        else if (strncasecmp(line, "Content-Type:", 13) == 0)
        {
            if (strcasestr(line, "multipart/x-mixed-replace") != NULL &&
                strcasestr(line, "boundary=frame") != NULL)
            {
                have_content_type = 1;
            }
        }
    }

    printf("HTTP Status: %d\n", status_code);

    /* Do not attempt to parse frames unless the response matches expectations. */
    if (status_code != 200 || !have_content_type)
    {
        fprintf(stderr, "Unexpected HTTP response or multipart boundary\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("MJPEG multipart response accepted\n");

    /*
     * Parse each multipart section and save its JPEG payload as a separate
     * sequentially numbered file in the output directory.
     */
    unsigned int frame_count = 0;

    while (running)
    {
        /* Each JPEG part must begin with the expected multipart boundary. */
        int len = read_line(sockfd, line, sizeof(line));

        if (len < 0)
            break;

        if (strcmp(line, "--frame--") == 0)
            break;

        if (strcmp(line, "--frame") != 0)
        {
            fprintf(stderr, "Unexpected multipart boundary: %s\n", line);
            break;
        }

        /*
         * Read this part's headers through the blank line before the payload.
         * Both a valid Content-Length and an image/jpeg Content-Type are
         * required to safely identify and receive the frame body.
         */
        size_t content_length = 0;
        int have_length = 0;
        int valid_frame_type = 0;

        while (running)
        {
            len = read_line(sockfd, line, sizeof(line));

            if (len < 0)
                break;

            if (len == 0)
                break;

            if (strncasecmp(line, "Content-Length:", 15) == 0)
            {
                if (parse_content_length(line, &content_length) != 0)
                {
                    fprintf(stderr, "Invalid frame Content-Length\n");
                    running = 0;
                    break;
                }
                have_length = 1;
            }
            else if (strncasecmp(line, "Content-Type:", 13) == 0)
            {
                if (strcasestr(line, "image/jpeg") != NULL)
                    valid_frame_type = 1;
            }
        }

        if (!running || !have_length || !valid_frame_type)
        {
            fprintf(stderr, "Invalid JPEG frame headers\n");
            break;
        }

        /* Build the next output name and reject paths that exceed the buffer. */
        char filename[PATH_MAX];

        int name_len = snprintf(filename, sizeof(filename),
                                "%s/frame_%06u.jpg",
                                output_dir, frame_count + 1);

        if (name_len < 0 || (size_t)name_len >= sizeof(filename))
        {
            fprintf(stderr, "Output filename too long\n");
            break;
        }

        /* Open each frame in binary mode so JPEG bytes are preserved exactly. */
        FILE *fp = fopen(filename, "wb");

        if (fp == NULL)
        {
            perror("fopen");
            break;
        }

        /* Receive exactly the declared payload length, writing it to this file. */
        int receive_result = receive_frame(sockfd, fp, content_length);

        if (fclose(fp) != 0)
        {
            perror("fclose");
            receive_result = -1;
        }

        /* Do not keep a partially received image if the stream or file write fails. */
        if (receive_result != 0)
        {
            fprintf(stderr, "Incomplete frame; removing %s\n", filename);
            unlink(filename);
            break;
        }

        frame_count++;

        printf("Frame %06u received: %zu bytes -> %s\n",
               frame_count, content_length, filename);

        /* Consume the CRLF separator following the binary JPEG payload. */
        len = read_line(sockfd, line, sizeof(line));

        if (len != 0)
        {
            fprintf(stderr, "Missing CRLF after JPEG frame\n");
            break;
        }
    }

    /* Close the camera socket after normal completion, protocol error, or interrupt. */
    close(sockfd);

    printf("\nMJPEG client stopped\n");
    printf("Total frames saved: %u\n", frame_count);

    return EXIT_SUCCESS;
}