#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

/* Model Gemini được dùng để tạo nội dung. */
#define GEMINI_MODEL "gemini-3.6-flash"

/*
 * URL endpoint của Gemini API để gọi generateContent.
 * Dùng macro để nối chuỗi giá trị GEMINI_MODEL vào URL.
 */
#define API_KEY "YOUR_API_KEY_HERE"
#define API_URL \
    "https://generativelanguage.googleapis.com/v1beta/models/" \
    GEMINI_MODEL ":generateContent"

/*
 * Dung lượng tối đa của prompt nhập từ người dùng.
 * Giới hạn này giúp tránh tràn buffer khi đọc dữ liệu từ stdin.
 */
#define MAX_PROMPT_SIZE 4096
#define GEMINI_THINKING_LEVEL "HIGH"
#define GEMINI_MAX_OUTPUT_TOKENS 4096
#define GEMINI_TEMPERATURE 0.7

/* Buffer lưu HTTP response */
typedef struct
{
    char *data;
    size_t size;
} ResponseBuffer;

/*
 * Callback được libcurl gọi mỗi khi nhận được dữ liệu từ server.
 * Nó sẽ nối thêm phần dữ liệu mới vào buffer hiện tại.
 */
static size_t write_callback(void *contents,
                             size_t size,
                             size_t nmemb,
                             void *userp)
{
    size_t real_size = size * nmemb;
    ResponseBuffer *buffer = (ResponseBuffer *)userp;

    char *new_data = realloc(buffer->data, buffer->size + real_size + 1);

    if (new_data == NULL)
    {
        return 0;
    }

    buffer->data = new_data;

    memcpy(buffer->data + buffer->size, contents, real_size);

    buffer->size += real_size;
    buffer->data[buffer->size] = '\0';

    return real_size;
}

/*
 * Parse chuỗi JSON mà Gemini trả về và in ra nội dung text.
 * Nếu API trả lỗi (field "error"), sẽ in ra thông báo lỗi tương ứng.
 */
static void parse_response(const char *json_response)
{
    cJSON *root = cJSON_Parse(json_response);

    if (root == NULL)
    {
        printf("Error: Invalid JSON response\n");
        return;
    }

    /* Kiểm tra API error */
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");

    if (cJSON_IsObject(error))
    {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");

        if (cJSON_IsString(message))
        {
            printf("Gemini API Error: %s\n", message->valuestring);
        }

        cJSON_Delete(root);
        return;
    }

    /* candidates[0].content.parts[] */
    cJSON *candidates = cJSON_GetObjectItemCaseSensitive(root, "candidates");

    cJSON *candidate = cJSON_GetArrayItem(candidates, 0);

    cJSON *content = cJSON_GetObjectItemCaseSensitive(candidate, "content");

    cJSON *parts = cJSON_GetObjectItemCaseSensitive(content, "parts");

    if (!cJSON_IsArray(parts))
    {
        printf("Error: No response content found\n");
        cJSON_Delete(root);
        return;
    }

    printf("\n========== GEMINI RESPONSE ==========\n");

    cJSON *part = NULL;

    cJSON_ArrayForEach(part, parts)
    {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");

        if (cJSON_IsString(text))
        {
            printf("%s", text->valuestring);
        }
    }

    printf("\n=====================================\n");

    cJSON_Delete(root);
}

/*
 * Gửi request POST chứa prompt đến Gemini API.
 * - Tạo JSON theo định dạng chuẩn của Gemini
 * - Thêm header API key và content-type
 * - Gửi bằng libcurl
 * - Nhận và parse response
 */
static int send_gemini_request(const char *prompt,
                               const char *api_key)
{
    CURL *curl;
    CURLcode result;
    struct curl_slist *headers = NULL;

    ResponseBuffer response = {NULL, 0};

    long http_code = 0;
    int ret = -1;

    /* Tạo JSON request */
    cJSON *root = cJSON_CreateObject();
    cJSON *contents = cJSON_CreateArray();
    cJSON *content = cJSON_CreateObject();
    cJSON *parts = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();

    if (!root || !contents || !content || !parts || !part)
    {
        fprintf(stderr, "JSON allocation failed\n");
        goto cleanup;
    }

    cJSON_AddStringToObject(part, "text", prompt);
    cJSON_AddItemToArray(parts, part);

    cJSON_AddStringToObject(content, "role", "user");
    cJSON_AddItemToObject(content, "parts", parts);

    cJSON_AddItemToArray(contents, content);
    cJSON_AddItemToObject(root, "contents", contents);

    /* Generation configuration */
    cJSON *generation_config = cJSON_CreateObject();

    cJSON_AddNumberToObject(generation_config,
                            "temperature", GEMINI_TEMPERATURE);

    cJSON_AddNumberToObject(generation_config,
                            "maxOutputTokens", GEMINI_MAX_OUTPUT_TOKENS);

    /* Thinking configuration */
    cJSON *thinking_config = cJSON_CreateObject();

    cJSON_AddStringToObject(thinking_config,
                            "thinkingLevel", GEMINI_THINKING_LEVEL);

    cJSON_AddItemToObject(generation_config,
                          "thinkingConfig", thinking_config);

    cJSON_AddItemToObject(root, "generationConfig", generation_config);

    char *json_request = cJSON_PrintUnformatted(root);

    if (json_request == NULL)
    {
        fprintf(stderr, "Cannot create JSON request\n");
        goto cleanup;
    }

    /* Initialize CURL */
    curl = curl_easy_init();

    if (curl == NULL)
    {
        fprintf(stderr, "CURL initialization failed\n");
        free(json_request);
        goto cleanup;
    }

    /* HTTP headers */
    headers = curl_slist_append(headers, "Content-Type: application/json");

    char api_header[512];

    snprintf(api_header, sizeof(api_header), "x-goog-api-key: %s", api_key);

    headers = curl_slist_append(headers, api_header);

    /* Configure HTTP POST */
    curl_easy_setopt(curl, CURLOPT_URL, API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_request);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     (long)strlen(json_request));

    /* Receive server response */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    /* Timeout */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    /* TLS certificate verification */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    printf("\nSending request to Gemini...\n");

    result = curl_easy_perform(curl);

    if (result != CURLE_OK)
    {
        fprintf(stderr, "CURL Error: %s\n", curl_easy_strerror(result));
        goto curl_cleanup;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    printf("HTTP Status: %ld\n", http_code);

    if (http_code != 200)
    {
        printf("Server response:\n%s\n",
               response.data ? response.data : "(empty)");
        goto curl_cleanup;
    }

    if (response.data != NULL)
    {
        parse_response(response.data);
        ret = 0;
    }

curl_cleanup:
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(json_request);
    free(response.data);

cleanup:
    cJSON_Delete(root);

    return ret;
}

/*
 * Hàm main:
 * - Đọc API key từ biến môi trường GEMINI_API_KEY
 * - Lặp nhận câu hỏi từ người dùng
 * - Gửi đến Gemini và in phản hồi
 */
int main(void)
{
    char prompt[MAX_PROMPT_SIZE];

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        fprintf(stderr, "CURL global initialization failed\n");
        return EXIT_FAILURE;
    }

    printf("=====================================\n");
    printf(" Raspberry Pi Gemini API Client\n");
    printf("=====================================\n");

    while (1)
    {
        printf("\nEnter your question (exit to quit):\n> ");

        if (fgets(prompt, sizeof(prompt), stdin) == NULL)
        {
            break;
        }

        prompt[strcspn(prompt, "\n")] = '\0';

        if (strcmp(prompt, "exit") == 0)
        {
            break;
        }

        if (strlen(prompt) == 0)
        {
            continue;
        }

        send_gemini_request(prompt, API_KEY);
    }

    curl_global_cleanup();

    printf("Program terminated.\n");

    return EXIT_SUCCESS;
}
