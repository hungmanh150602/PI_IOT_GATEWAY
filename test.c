#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netdb.h>

#define BUFFER_SIZE 4096

static int send_all(int sockfd, const char *data, size_t len)
{
    size_t total = 0;

    while (total < len)
    {
        ssize_t sent = send(sockfd, data + total, len - total, 0);

        if (sent < 0)
        {
            perror("send");
            return -1;
        }

        if (sent == 0)
        {
            fprintf(stderr, "Connection closed while sending\n");
            return -1;
        }

        total += (size_t)sent;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        fprintf(stderr,
                "Usage: %s <host> <port> <path> <output>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *path = argv[3];
    const char *output = argv[4];

    int sockfd = -1;
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;

    FILE *file = NULL;
    char request[1024];
    char buffer[BUFFER_SIZE];

    int ret = EXIT_FAILURE;

    /* 1. Configure address resolution */

    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host, port, &hints, &result);

    if (status != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        goto cleanup;
    }

    /* 2. Create socket and connect */

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {

        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sockfd < 0)
            continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
        {
            break;
        }

        close(sockfd);
        sockfd = -1;
    }

    if (sockfd < 0)
    {
        fprintf(stderr, "Cannot connect to server\n");
        goto cleanup;
    }

    printf("TCP connection established\n");

    /* 3. Construct HTTP GET request */

    int request_len = snprintf(request,
                               sizeof(request),
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Connection: close\r\n"
                               "\r\n",
                               path,
                               host);

    if (request_len < 0 ||
        (size_t)request_len >= sizeof(request))
    {
        fprintf(stderr, "HTTP request is too long\n");
        goto cleanup;
    }

    /* 4. Send HTTP request */

    if (send_all(sockfd, request, (size_t)request_len) < 0)
    {
        goto cleanup;
    }

    printf("HTTP GET request sent\n");

    /* 5. Open output file */

    file = fopen(output, "wb");

    if (file == NULL)
    {
        perror("fopen");
        goto cleanup;
    }

    /* 6. Receive HTTP response */

    size_t total_received = 0;

    while (1)
    {

        ssize_t received = recv(sockfd, buffer, sizeof(buffer), 0);

        if (received < 0)
        {
            perror("recv");
            goto cleanup;
        }

        if (received == 0)
            break;

        size_t written = fwrite(buffer, 1, (size_t)received, file);

        if (written != (size_t)received)
        {
            fprintf(stderr, "File write error\n");
            goto cleanup;
        }

        total_received += (size_t)received;
    }

    printf("HTTP response received\n");
    printf("Total bytes: %zu\n", total_received);

    ret = EXIT_SUCCESS;

cleanup:

    if (file != NULL)
        fclose(file);

    if (sockfd >= 0)
        close(sockfd);

    if (result != NULL)
        freeaddrinfo(result);

    return ret;
}