#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

#include <sys/socket.h>
#include <netdb.h>

#define BUFFER_SIZE 4096
#define HEADER_SIZE 16384

struct http_response
{
    int status_code;
    size_t content_length;
    int has_content_length;
    char content_type[128];
};

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

/* Find CRLF CRLF in HTTP header */
static int find_header_end(const char *buffer, size_t len)
{
    if (len < 4)
        return -1;

    for (size_t i = 0; i <= len - 4; i++)
    {
        /* so sánh 4 byte liên tiếp kể từ vị trí buffer + i xem có khớp
         * hoàn toàn với chuỗi "\r\n\r\n" hay không.
         */
        if (memcmp(buffer + i, "\r\n\r\n", 4) == 0)
            return (int)(i + 4);
    }
    return -1;
}

/* Parse HTTP response headers
 * Hàm parse_headers trong C dùng để phân tích cú pháp (parse)
 * chuỗi HTTP response header vừa nhận được,
 * nhằm trích xuất các thông tin quan trọng như:
 *
 * 1. Mã trạng thái HTTP (Status Code) (ví dụ: 200, 404, 500).
 * 2. Độ dài nội dung (Content-Length).
 * 3. Loại nội dung (Content-Type).
 */
static int parse_headers(char *header, struct http_response *response)
{
    /* Đặt toàn bộ các trường dữ liệu bên trong biến response
     * về giá trị 0 để tránh rác bộ nhớ từ các giá trị cũ.
     */
    memset(response, 0, sizeof(*response));

    /* Xử lý dòng đầu tiên (Status Line)
     * strtok_r: Hàm cắt chuỗi an toàn theo luồng (thread-safe) dùng để tách từng dòng
     * dựa vào ký tự xuống dòng \r\n. Dòng đầu tiên (line) luôn là Status Line
     * (ví dụ: HTTP/1.1 200 OK).
     */
    char *saveptr = NULL;
    char *line = strtok_r(header, "\r\n", &saveptr);

    if (line == NULL || sscanf(line, "HTTP/%*u.%*u %d", &response->status_code) != 1)
    {
        fprintf(stderr, "Invalid HTTP status line\n");
        return -1;
    }

    /* Duyệt qua các dòng header tiếp theo bằng vòng lặp while
     * Vòng lặp tiếp tục cắt từng dòng tiếp theo của header cho đến khi hết chuỗi (NULL)
     */
    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL)
    {
        /* Xử lý header Content-Length
         * strncasecmp: So sánh 15 ký tự đầu tiên của dòng với chuỗi "Content-Length:"
         * mà không phân biệt chữ hoa/chữ thường.
         */
        if (strncasecmp(line, "Content-Length:", 15) == 0)
        {
            char *value = line + 15;
            char *endptr = NULL;

            /* Bỏ qua khoảng trắng hoặc tab thừa
             * Dịch con trỏ value qua phải nếu có dấu cách ( ) hoặc
             * tab (\t) dư thừa sau dấu hai chấm.
             */
            while (*value == ' ' || *value == '\t')
                value++;

            errno = 0;
            /* strtoull: Chuyển đổi chuỗi số thành dạng số nguyên lớn không âm
             * (unsigned long long).
             */
            unsigned long long length = strtoull(value, &endptr, 10);

            /* Kiểm tra lỗi ép kiểu số nguyên an toàn
             * Đảm bảo chuỗi không chứa ký tự lạ, không bị tràn số (length > SIZE_MAX),
             * nếu sai sót sẽ trả về -1.
             * Nếu đúng, lưu giá trị vào response->content_length
             * và đánh dấu response->has_content_length = 1.
             */
            if (errno != 0 || endptr == value || *endptr != '\0' || length > SIZE_MAX)
            {
                fprintf(stderr, "Invalid Content-Length\n");
                return -1;
            }

            response->content_length = (size_t)length;
            response->has_content_length = 1;
        }

        /* Tương tự như Content-Length
         * hàm tìm kiếm dòng bắt đầu bằng "Content-Type:" (không phân biệt hoa thường).
         */
        else if (strncasecmp(line, "Content-Type:", 13) == 0)
        {
            const char *value = line + 13;

            /* Bỏ qua khoảng trắng thừa sau dấu hai chấm. */
            while (*value == ' ' || *value == '\t')
                value++;

            /* snprintf: Sao chép an toàn chuỗi giá trị
             * (ví dụ: application/json hoặc text/html) vào bộ đệm response->content_type
             * nhằm tránh lỗi tràn bộ đệm (buffer overflow).
             */
            snprintf(response->content_type, sizeof(response->content_type), "%s", value);
        }
    }
    return 0;
}

/*
 * Hàm "save_body" trong C dùng để ghi phần thân (body) của phản hồi HTTP
 * (dữ liệu tải về như tệp tin, hình ảnh, HTML,...) vào một tệp (file) đang mở,
 * đồng thời cộng dồn tổng số byte đã ghi vào biến đếm (total).
 */
static int save_body(FILE *file, const char *data, size_t len, size_t *total)
{
    if (len == 0)
        return 0;

    /* fwrite thực hiện ghi len phần tử (mỗi phần tử kích thước 1 byte)
     * từ bộ đệm data vào luồng tệp file.
     * Giá trị trả về (written) là số byte thực tế đã được ghi thành công.
     */
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
    if (argc != 5)
    {
        fprintf(stderr,
                "Usage: %s <host> <port> <path> <output_path>\n"
                "Example: ./build/http_capture_tcp 192.168.1.100 80 /capture images/capture.jpg\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *path = argv[3];
    const char *output = argv[4];

    int sockfd = -1;
    int ret = EXIT_FAILURE;

    /* Các biến này phục vụ DNS resolution và duyệt các địa chỉ IP mà getaddrinfo() trả về. */
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;

    FILE *file = NULL;

    /*
    Ba buffer có ba nhiệm vụ khác nhau:
        Buffer	             Chức năng
    request	        Chứa HTTP GET request
    buffer	        Nhận dữ liệu TCP
    header	        Tích lũy HTTP Header
    */
    char request[1024];
    /* Nhận dữ liệu TCP */
    char buffer[BUFFER_SIZE];
    /* Tích lũy HTTP Header */
    char header[HEADER_SIZE];

    size_t header_len = 0;    /* số byte Header đã tích lũy. */
    size_t body_received = 0; /* số byte Body đã ghi. */

    int header_complete = 0; /* đánh dấu đã tìm được \r\n\r\n. */

    struct http_response response = {0};

    /* 1. Resolve hostname */
    memset(&hints, 0, sizeof(hints)); /* set all to 0 */

    hints.ai_family = AF_UNSPEC;     /* cho phép sử dụng IPv4 hoặc IPv6. */
    hints.ai_socktype = SOCK_STREAM; /* TCP */

    int status = getaddrinfo(host, port, &hints, &result);

    if (status != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        goto cleanup;
    }

    /* 2. Create TCP connection */
    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sockfd < 0)
            continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
        {
            /* nếu kết nối thành công thì thoát */
            break;
        }

        /* nếu kết nối thất bại thì thử địa chỉ tiếp theo */
        close(sockfd);
        sockfd = -1;
    }

    if (sockfd < 0)
    {
        fprintf(stderr, "Cannot connect to server\n");
        goto cleanup;
    }

    printf("TCP connection established\n");

    /* 3. Construct HTTP GET request
    ví dụ:
    GET /stream HTTP/1.1
    Host: 192.168.1.100
    Connection: close
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

    /* 4. Send request
    bảo đảm gửi hết request.
    */
    if (send_all(sockfd, request, (size_t)request_len) < 0)
        goto cleanup;

    printf("HTTP GET request sent\n");

    /* 5. Open output file
    Chế độ "wb" có nghĩa:
    - w: mở để ghi, tạo file nếu chưa có hoặc xóa nội dung cũ nếu đã tồn tại.
    - b: chế độ binary.
    */
    file = fopen(output, "wb");

    if (file == NULL)
    {
        perror("fopen");
        goto cleanup;
    }

    /* 6. Receive and parse HTTP response */
    while (1)
    {
        /* nhận phản hồi */
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

        /* 6.1. Receive HTTP headers */
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

                /* tìm kiếm kết thúc HTTP header */
                int end = find_header_end(header, header_len);

                /* tìm thấy */
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

        /* 6.2. Save HTTP body
         * HTTP Header đã hoàn tất.
         * Trong buffer hiện tại vẫn còn dữ liệu chưa xử lý.
         * Phần dữ liệu còn lại chính là HTTP Body:
         */
        if (header_complete && offset < chunk_len)
        {
            size_t body_len = chunk_len - offset;

            /* Nếu có Content-Length */
            if (response.has_content_length)
            {
                /* Tính số byte còn phải nhận. */
                size_t remaining = response.content_length - body_received;

                /* Không ghi vượt quá Content-Length. */
                if (body_len > remaining)
                    body_len = remaining;
            }

            /* Chỉ ghi phần Body, không ghi Header. */
            if (save_body(file, buffer + offset, body_len, &body_received) < 0)
            {
                goto cleanup;
            }
        }

        /* Nếu đã nhận đủ số byte theo Content-Length thì kết thúc vòng lặp. */
        if (header_complete &&
            response.has_content_length &&
            body_received == response.content_length)
        {
            break;
        }
    }

    /* kiểm tra Header và Body có hoàn chỉnh hay không. */
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

/* Giải phóng tài nguyên */
cleanup:

    if (file != NULL)
        fclose(file);

    if (sockfd >= 0)
        close(sockfd);

    if (result != NULL)
        freeaddrinfo(result);

    return ret;
}