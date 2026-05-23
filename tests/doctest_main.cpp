#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <curl/curl.h>

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    doctest::Context ctx(argc, argv);
    const int rc = ctx.run();
    curl_global_cleanup();
    return rc;
}
