#define _GNU_SOURCE

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
#include <sys/wait.h>
#include <netdb.h>

#define LINE_SIZE       1024
#define MAX_FRAME_SIZE  (10U * 1024U * 1024U)
#define DEFAULT_FPS     "15"
#define DEFAULT_FFPLAY  "ffplay"

static volatile sig_atomic_t running = 1;
static pid_t ffplay_pid = -1;
static int ffplay_stdin = -1;

static void handle_signal(int signo)
{
    (void)signo;
    running = 0;
}

/* Write all bytes to FFplay's stdin pipe. */
static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;
    size_t total = 0;

    while (total < length && running) {
        ssize_t n = write(fd, p + total, length - total);

        if (n < 0) {
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

/* Read one CRLF-terminated line from the ESP32-CAM socket. */
static int read_line(int sockfd, char *buffer, size_t capacity)
{
    size_t pos = 0;
    char ch;

    if (capacity < 2)
        return -1;

    while (pos < capacity - 1 && running) {
        ssize_t n = recv(sockfd, &ch, 1, 0);

        if (n == 0)
            return -1;

        if (n < 0) {
            if (errno == EINTR && running)
                continue;
            return -1;
        }

        if (ch == '\n') {
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

/* Receive exactly length bytes into the supplied buffer. */
static int recv_exact(int sockfd, unsigned char *buffer, size_t length)
{
    size_t total = 0;

    while (total < length && running) {
        ssize_t n = recv(sockfd, buffer + total, length - total, 0);

        if (n == 0)
            return -1;

        if (n < 0) {
            if (errno == EINTR && running)
                continue;
            return -1;
        }

        total += (size_t)n;
    }

    return total == length ? 0 : -1;
}

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
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
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

static int send_http_request(int sockfd, const char *host)
{
    char request[1024];

    int n = snprintf(request, sizeof(request),
                     "GET /stream HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Accept: multipart/x-mixed-replace\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n",
                     host);

    if (n < 0 || (size_t)n >= sizeof(request)) {
        fprintf(stderr, "HTTP request is too long\n");
        return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t written = send(sockfd, request + sent, (size_t)n - sent, 0);
        if (written < 0) {
            if (errno == EINTR && running)
                continue;
            perror("send");
            return -1;
        }
        if (written == 0)
            return -1;
        sent += (size_t)written;
    }

    return 0;
}

/* Validate the ESP32-CAM HTTP response and multipart boundary. */
static int validate_http_response(int sockfd)
{
    char line[LINE_SIZE];
    int status_code = 0;
    int have_mjpeg_type = 0;

    while (running) {
        int len = read_line(sockfd, line, sizeof(line));
        if (len < 0) {
            fprintf(stderr, "Failed to read HTTP response headers\n");
            return -1;
        }

        if (len == 0)
            break;

        if (strncmp(line, "HTTP/", 5) == 0) {
            if (sscanf(line, "%*s %d", &status_code) != 1) {
                fprintf(stderr, "Invalid HTTP status line\n");
                return -1;
            }
        } else if (strncasecmp(line, "Content-Type:", 13) == 0 &&
                   strcasestr(line, "multipart/x-mixed-replace") != NULL &&
                   strcasestr(line, "boundary=frame") != NULL) {
            have_mjpeg_type = 1;
        }
    }

    if (status_code != 200 || !have_mjpeg_type) {
        fprintf(stderr,
                "Unexpected HTTP response (status=%d, MJPEG=%d)\n",
                status_code, have_mjpeg_type);
        return -1;
    }

    printf("ESP32-CAM HTTP MJPEG response accepted\n");
    return 0;
}

/*
 * Start FFplay as a child process.
 * The write end of the pipe remains in the parent; FFplay reads stdin.
 */
static int start_ffplay(const char *fps)
{
    int pipefd[2];

    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    ffplay_pid = fork();

    if (ffplay_pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (ffplay_pid == 0) {
        close(pipefd[1]);

        if (dup2(pipefd[0], STDIN_FILENO) < 0) {
            perror("dup2");
            _exit(127);
        }
        close(pipefd[0]);

        execlp(DEFAULT_FFPLAY, DEFAULT_FFPLAY,
               "-hide_banner",
               "-loglevel", "warning",
               "-fflags", "nobuffer",
               "-flags", "low_delay",
               "-f", "mjpeg",
               "-framerate", fps,
               "-i", "pipe:0",
               "-an",
               "-sn",
               "-dn",
               "-window_title", "ESP32-CAM Live Stream",
               (char *)NULL);

        perror("execlp ffplay");
        _exit(127);
    }

    close(pipefd[0]);
    ffplay_stdin = pipefd[1];

    printf("FFplay started (PID %ld, input FPS %s)\n",
           (long)ffplay_pid, fps);
    return 0;
}

static void stop_ffplay(void)
{
    if (ffplay_stdin >= 0) {
        close(ffplay_stdin);
        ffplay_stdin = -1;
    }

    if (ffplay_pid > 0) {
        int status;
        pid_t ret = waitpid(ffplay_pid, &status, WNOHANG);

        if (ret == 0) {
            /* Give FFplay a short opportunity to exit after EOF. */
            for (int i = 0; i < 10 && ret == 0; i++) {
                usleep(100000);
                ret = waitpid(ffplay_pid, &status, WNOHANG);
            }
        }

        if (ret == 0) {
            kill(ffplay_pid, SIGTERM);
            (void)waitpid(ffplay_pid, &status, 0);
        }

        ffplay_pid = -1;
    }
}

/* Parse each multipart JPEG frame and pass its payload to FFplay. */
static int forward_frames_to_ffplay(int sockfd)
{
    char line[LINE_SIZE];
    unsigned int frame_count = 0;
    unsigned char *jpeg_buffer = NULL;
    size_t buffer_capacity = 0;

    while (running) {
        int len = read_line(sockfd, line, sizeof(line));
        if (len < 0)
            break;

        if (strcmp(line, "--frame--") == 0)
            break;

        if (strcmp(line, "--frame") != 0) {
            fprintf(stderr, "Unexpected multipart boundary: %s\n", line);
            break;
        }

        size_t content_length = 0;
        int have_length = 0;
        int valid_frame_type = 0;

        while (running) {
            len = read_line(sockfd, line, sizeof(line));
            if (len < 0)
                goto out;

            if (len == 0)
                break;

            if (strncasecmp(line, "Content-Length:", 15) == 0) {
                if (parse_content_length(line, &content_length) != 0) {
                    fprintf(stderr, "Invalid JPEG Content-Length\n");
                    goto out;
                }
                have_length = 1;
            } else if (strncasecmp(line, "Content-Type:", 13) == 0 &&
                       strcasestr(line, "image/jpeg") != NULL) {
                valid_frame_type = 1;
            }
        }

        if (!running)
            break;

        if (!have_length || !valid_frame_type) {
            fprintf(stderr, "Invalid JPEG frame headers\n");
            break;
        }

        if (content_length > buffer_capacity) {
            unsigned char *new_buffer = realloc(jpeg_buffer, content_length);
            if (new_buffer == NULL) {
                perror("realloc");
                break;
            }
            jpeg_buffer = new_buffer;
            buffer_capacity = content_length;
        }

        if (recv_exact(sockfd, jpeg_buffer, content_length) != 0) {
            fprintf(stderr, "Incomplete JPEG frame received\n");
            break;
        }

        if (write_all(ffplay_stdin, jpeg_buffer, content_length) != 0) {
            if (running)
                fprintf(stderr, "Cannot write to FFplay; it may have exited\n");
            break;
        }

        frame_count++;
        if (frame_count % 100 == 0)
            printf("Frames sent to FFplay: %u\n", frame_count);

        /* Consume CRLF after the binary JPEG payload. */
        len = read_line(sockfd, line, sizeof(line));
        if (len != 0) {
            fprintf(stderr, "Missing CRLF after JPEG frame\n");
            break;
        }
    }

out:
    free(jpeg_buffer);
    printf("Total frames sent to FFplay: %u\n", frame_count);
    return running ? 0 : 1;
}

int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr,
                "Usage: %s <esp32_host> <esp32_port> [fps]\n"
                "Example: %s 192.168.1.100 81 15\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *fps = (argc == 4) ? argv[3] : DEFAULT_FPS;

    char *end = NULL;
    long fps_value = strtol(fps, &end, 10);
    if (end == fps || *end != '\0' || fps_value < 1 || fps_value > 60) {
        fprintf(stderr, "FPS must be an integer between 1 and 60\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) {
        fprintf(stderr, "TCP connection to ESP32-CAM failed\n");
        return EXIT_FAILURE;
    }

    printf("TCP connection established\n");

    if (send_http_request(sockfd, host) != 0 ||
        validate_http_response(sockfd) != 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (start_ffplay(fps) != 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Receiving MJPEG frames. Press Ctrl+C to stop.\n");

    (void)forward_frames_to_ffplay(sockfd);

    close(sockfd);
    stop_ffplay();

    printf("MJPEG FFplay client stopped\n");
    return EXIT_SUCCESS;
}
