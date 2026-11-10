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

#include <dirent.h>
#include <sys/stat.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 4096
#define LINE_SIZE 1024
#define MAX_FRAME_SIZE (10U * 1024U * 1024U)

static volatile sig_atomic_t running = 1;

static void handle_sigint(int signo)
{
    (void)signo;
    running = 0;
}

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
 * Read exactly one CRLF-terminated line from the TCP socket.
 * The returned string does not contain CRLF.
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
    return -1; /* Line too long */
}

/* Read exactly length bytes from TCP and write them to a file. */
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
            return -1; /* Connection closed before frame completed */

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

/* Parse Content-Length safely. */
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

/* Thiết lập kết nối TCP
 * Nếu thành công, hàm trả về socket file descriptor.
 * Nếu thất bại, trả về -1.
 */
static int connect_to_server(const char *host, const char *port)
{
    /* Các biến này phục vụ DNS resolution và duyệt các địa chỉ IP
     * mà getaddrinfo() trả về.
     */
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    int sockfd = -1;

    /* set all to 0 */
    memset(&hints, 0, sizeof(hints));
    /* cho phép sử dụng IPv4 hoặc IPv6. */
    hints.ai_family = AF_UNSPEC;
    /* TCP */
    hints.ai_socktype = SOCK_STREAM;

    /* Resolve hostname */
    int ret = getaddrinfo(host, port, &hints, &result);

    if (ret != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    /* Create TCP connection */
    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sockfd < 0)
            continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            /* nếu kết nối thành công thì thoát */
            break;

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result);
    return sockfd;
}

/*
 * Delete old frame files from the output directory.
 * Only regular files whose names start with "frame_" are deleted.
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

        /* Only process files starting with "frame_". */
        if (strncmp(entry->d_name, "frame_", 6) != 0)
            continue;

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

        /* Do not delete directories or symbolic links. */
        if (!S_ISREG(st.st_mode))
            continue;

        if (unlink(filepath) == 0)
        {
            deleted_count++;
            // printf("Deleted: %s\n", filepath);
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

/* Tham số	        Ý nghĩa	                            Ví dụ
 * argv[0]	    Tên chương trình	                ./mjpeg_client
 * argv[1]	    IP hoặc hostname của ESP32-CAM	    192.168.1.100
 * argv[2]	    TCP port	                        81
 * argv[3]	    Thư mục lưu ảnh	                    /mnt/usb/frames
 */
int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr,
                "Usage: %s <host> <port> <output_directory>\n"
                "Example: ./build/mjpeg_client_tcp 192.168.1.100 81 images\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *output_dir = argv[3];

    /* Remove old frame files before starting. */
    printf("Cleaning old frame files...\n");

    if (cleanup_frame_files(output_dir) != 0)
    {
        fprintf(stderr, "Failed to clean output directory\n");
        return EXIT_FAILURE;
    }

    /* Đăng ký xử lý tín hiệu SIGINT */
    signal(SIGINT, handle_sigint);

    /* Thiết lập kết nối TCP */
    int sockfd = connect_to_server(host, port);

    if (sockfd < 0)
    {
        fprintf(stderr, "TCP connection failed\n");
        return EXIT_FAILURE;
    }

    printf("TCP connection established\n");

    /* Tạo HTTP GET request:
     * GET /stream HTTP/1.1
     * Host: 192.168.1.100
     * Accept: multipart/x-mixed-replace
     * Connection: keep-alive
     */
    char request[1024];

    int request_len = snprintf(request, sizeof(request),
                               "GET /stream HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Accept: multipart/x-mixed-replace\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n",
                               host);

    if (request_len < 0 || (size_t)request_len >= sizeof(request))
    {
        fprintf(stderr, "HTTP request is too long\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* gửi toàn bộ request qua TCP socket. */
    if (send_all(sockfd, request, (size_t)request_len) != 0)
    {
        perror("send_all");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("HTTP GET request sent\n");

    /* Read HTTP response headers. */
    char line[LINE_SIZE];
    int status_code = 0;
    int have_content_type = 0;

    while (running)
    {
        /* Client bắt đầu đọc HTTP response headers bằng read_line(). */
        int len = read_line(sockfd, line, sizeof(line));

        if (len < 0)
        {
            fprintf(stderr, "Failed to read HTTP response header\n");
            close(sockfd);
            return EXIT_FAILURE;
        }

        if (len == 0)
            break;

        /* Ví dụ server trả về:
         * HTTP/1.1 200 OK
         * Content-Type: multipart/x-mixed-replace; boundary=frame
         * Connection: keep-alive
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

    if (status_code != 200 || !have_content_type)
    {
        fprintf(stderr, "Unexpected HTTP response or multipart boundary\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("MJPEG multipart response accepted\n");

    /* Receive multipart frames. */
    unsigned int frame_count = 0;

    while (running)
    {
        /* Find the next frame boundary. */
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

        /* Read headers belonging to this JPEG frame. */
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

        char filename[PATH_MAX];

        int name_len = snprintf(filename, sizeof(filename),
                                "%s/frame_%06u.jpg",
                                output_dir, frame_count + 1);

        if (name_len < 0 || (size_t)name_len >= sizeof(filename))
        {
            fprintf(stderr, "Output filename too long\n");
            break;
        }

        FILE *fp = fopen(filename, "wb");

        if (fp == NULL)
        {
            perror("fopen");
            break;
        }

        int receive_result = receive_frame(sockfd, fp, content_length);

        if (fclose(fp) != 0)
        {
            perror("fclose");
            receive_result = -1;
        }

        if (receive_result != 0)
        {
            fprintf(stderr, "Incomplete frame; removing %s\n", filename);
            unlink(filename);
            break;
        }

        frame_count++;

        printf("Frame %06u received: %zu bytes -> %s\n",
               frame_count, content_length, filename);

        /* Consume the CRLF after the binary JPEG frame. */
        len = read_line(sockfd, line, sizeof(line));

        if (len != 0)
        {
            fprintf(stderr, "Missing CRLF after JPEG frame\n");
            break;
        }
    }

    close(sockfd);

    printf("\nMJPEG client stopped\n");
    printf("Total frames saved: %u\n", frame_count);

    return EXIT_SUCCESS;
}