/* Link check: static libcurl (+OpenSSL, zlib) for the PS5 payload SDK. */
#include <stdio.h>
#include <curl/curl.h>

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *c = curl_easy_init();
    printf("%s\n", curl_version());
    if (c) curl_easy_cleanup(c);
    curl_global_cleanup();
    return c ? 0 : 1;
}
