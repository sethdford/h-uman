/* Net security tests */
#include "human/net_security.h"
#include "test_framework.h"
#include <string.h>

static void test_validate_url_https_ok(void) {
    hu_error_t err = hu_validate_url("https://example.com/path");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_validate_url_http_localhost_ok(void) {
    hu_error_t err = hu_validate_url("http://localhost:8080/api");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_validate_url_http_remote_fail(void) {
    hu_error_t err = hu_validate_url("http://example.com/");
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_validate_url_ftp_fail(void) {
    hu_error_t err = hu_validate_url("ftp://example.com");
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_is_private_ip_loopback(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("127.0.0.1"));
    HU_ASSERT_TRUE(hu_is_private_ip("localhost"));
    HU_ASSERT_TRUE(hu_is_private_ip("::1"));
}

static void test_is_private_ip_private_ranges(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("10.0.0.1"));
    HU_ASSERT_TRUE(hu_is_private_ip("192.168.1.1"));
    HU_ASSERT_TRUE(hu_is_private_ip("172.16.0.1"));
}

static void test_is_private_ip_public(void) {
    HU_ASSERT_FALSE(hu_is_private_ip("8.8.8.8"));
    HU_ASSERT_FALSE(hu_is_private_ip("1.1.1.1"));
}

static void test_validate_domain_exact(void) {
    const char *allowed[] = {"example.com"};
    HU_ASSERT_TRUE(hu_validate_domain("example.com", allowed, 1));
}

static void test_validate_domain_wildcard(void) {
    const char *allowed[] = {"*.example.com"};
    HU_ASSERT_TRUE(hu_validate_domain("api.example.com", allowed, 1));
    HU_ASSERT_FALSE(hu_validate_domain("evil.com", allowed, 1));
}

static void test_validate_domain_empty_allowlist(void) {
    const char *allowed[] = {NULL};
    HU_ASSERT_TRUE(hu_validate_domain("anything.com", allowed, 0));
}

static void test_validate_url_https_with_path(void) {
    hu_error_t err = hu_validate_url("https://api.example.com/v1/endpoint?key=val");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_validate_url_https_with_port(void) {
    hu_error_t err = hu_validate_url("https://example.com:443/path");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_validate_url_http_127_rejected(void) {
    hu_error_t err = hu_validate_url("http://127.0.0.1:3000/");
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_is_private_ip_172_31(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("172.31.0.1"));
}

static void test_is_private_ip_172_15(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("172.16.0.0"));
}

static void test_is_private_ip_192_168(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("192.168.0.1"));
    HU_ASSERT_TRUE(hu_is_private_ip("192.168.255.255"));
}

static void test_is_private_ip_10_full_range(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("10.0.0.0"));
    HU_ASSERT_TRUE(hu_is_private_ip("10.255.255.255"));
}

static void test_is_private_ip_public_domain(void) {
    HU_ASSERT_FALSE(hu_is_private_ip("google.com"));
}

static void test_validate_domain_exact_match(void) {
    const char *allowed[] = {"api.service.com"};
    HU_ASSERT_TRUE(hu_validate_domain("api.service.com", allowed, 1));
    HU_ASSERT_FALSE(hu_validate_domain("other.service.com", allowed, 1));
}

static void test_validate_domain_multiple_allowed(void) {
    const char *allowed[] = {"a.com", "b.com", "c.com"};
    HU_ASSERT_TRUE(hu_validate_domain("a.com", allowed, 3));
    HU_ASSERT_TRUE(hu_validate_domain("b.com", allowed, 3));
    HU_ASSERT_FALSE(hu_validate_domain("d.com", allowed, 3));
}

static void test_validate_url_null_empty(void) {
    hu_error_t err = hu_validate_url("");
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_validate_url_file_rejected(void) {
    hu_error_t err = hu_validate_url("file:///etc/passwd");
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_validate_url_javascript_rejected(void) {
    hu_error_t err = hu_validate_url("javascript:alert(1)");
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_validate_url_data_rejected(void) {
    hu_error_t err = hu_validate_url("data:text/plain,hello");
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ─── WP-21B parity: URL scheme, IP, domain edge cases ───────────────────── */
static void test_validate_url_null_rejected(void) {
    hu_error_t err = hu_validate_url(NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_validate_url_https_query_fragment(void) {
    hu_error_t err = hu_validate_url("https://example.com/path?foo=bar#anchor");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_validate_url_ws_rejected(void) {
    hu_error_t err = hu_validate_url("ws://example.com/socket");
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_validate_url_wss_rejected(void) {
    hu_error_t err = hu_validate_url("wss://example.com/socket");
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_validate_url_http_localhost_no_port(void) {
    hu_error_t err = hu_validate_url("http://localhost/");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_is_private_ip_link_local(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("169.254.1.1"));
}

static void test_is_private_ip_ipv6_loopback_bracketed(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("[::1]"));
}

static void test_is_private_ip_ipv6_unique_local(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("fd00::1"));
}

static void test_is_private_ip_empty_fail_closed(void) {
    HU_ASSERT_TRUE(hu_is_private_ip(""));
}

static void test_is_private_ip_null_fail_closed(void) {
    HU_ASSERT_TRUE(hu_is_private_ip(NULL));
}

static void test_validate_domain_null_host_fail(void) {
    const char *allowed[] = {"example.com"};
    HU_ASSERT_FALSE(hu_validate_domain(NULL, allowed, 1));
}

static void test_validate_domain_empty_host_fail(void) {
    const char *allowed[] = {"example.com"};
    HU_ASSERT_FALSE(hu_validate_domain("", allowed, 1));
}

static void test_validate_domain_wildcard_subdomain_deep(void) {
    const char *allowed[] = {"*.example.com"};
    HU_ASSERT_TRUE(hu_validate_domain("api.v1.example.com", allowed, 1));
}

static void test_validate_domain_implicit_subdomain(void) {
    const char *allowed[] = {"example.com"};
    HU_ASSERT_TRUE(hu_validate_domain("sub.example.com", allowed, 1));
}

static void test_validate_domain_wildcard_wrong_tld(void) {
    const char *allowed[] = {"*.example.com"};
    HU_ASSERT_FALSE(hu_validate_domain("example.org", allowed, 1));
}

static void test_validate_domain_null_pattern_skipped(void) {
    const char *allowed[] = {NULL, "b.com"};
    HU_ASSERT_TRUE(hu_validate_domain("b.com", allowed, 2));
}

static void test_is_private_ip_documentation_range(void) {
    HU_ASSERT_TRUE(hu_is_private_ip("192.0.2.1"));
}

static void test_validate_url_mailto_rejected(void) {
    hu_error_t err = hu_validate_url("mailto:test@example.com");
    HU_ASSERT_NEQ(err, HU_OK);
}

void run_net_security_tests(void) {
    HU_TEST_SUITE("Net security");
    HU_RUN_TEST(test_validate_url_https_ok);
    HU_RUN_TEST(test_validate_url_http_localhost_ok);
    HU_RUN_TEST(test_validate_url_http_remote_fail);
    HU_RUN_TEST(test_validate_url_ftp_fail);
    HU_RUN_TEST(test_is_private_ip_loopback);
    HU_RUN_TEST(test_is_private_ip_private_ranges);
    HU_RUN_TEST(test_is_private_ip_public);
    HU_RUN_TEST(test_validate_domain_exact);
    HU_RUN_TEST(test_validate_domain_wildcard);
    HU_RUN_TEST(test_validate_domain_empty_allowlist);
    HU_RUN_TEST(test_validate_url_https_with_path);
    HU_RUN_TEST(test_validate_url_https_with_port);
    HU_RUN_TEST(test_validate_url_http_127_rejected);
    HU_RUN_TEST(test_is_private_ip_172_31);
    HU_RUN_TEST(test_is_private_ip_172_15);
    HU_RUN_TEST(test_is_private_ip_192_168);
    HU_RUN_TEST(test_is_private_ip_10_full_range);
    HU_RUN_TEST(test_is_private_ip_public_domain);
    HU_RUN_TEST(test_validate_domain_exact_match);
    HU_RUN_TEST(test_validate_domain_multiple_allowed);
    HU_RUN_TEST(test_validate_url_null_empty);
    HU_RUN_TEST(test_validate_url_file_rejected);
    HU_RUN_TEST(test_validate_url_javascript_rejected);
    HU_RUN_TEST(test_validate_url_data_rejected);
    HU_RUN_TEST(test_validate_url_null_rejected);
    HU_RUN_TEST(test_validate_url_https_query_fragment);
    HU_RUN_TEST(test_validate_url_ws_rejected);
    HU_RUN_TEST(test_validate_url_wss_rejected);
    HU_RUN_TEST(test_validate_url_http_localhost_no_port);
    HU_RUN_TEST(test_is_private_ip_link_local);
    HU_RUN_TEST(test_is_private_ip_ipv6_loopback_bracketed);
    HU_RUN_TEST(test_is_private_ip_ipv6_unique_local);
    HU_RUN_TEST(test_is_private_ip_empty_fail_closed);
    HU_RUN_TEST(test_is_private_ip_null_fail_closed);
    HU_RUN_TEST(test_validate_domain_null_host_fail);
    HU_RUN_TEST(test_validate_domain_empty_host_fail);
    HU_RUN_TEST(test_validate_domain_wildcard_subdomain_deep);
    HU_RUN_TEST(test_validate_domain_implicit_subdomain);
    HU_RUN_TEST(test_validate_domain_wildcard_wrong_tld);
    HU_RUN_TEST(test_validate_domain_null_pattern_skipped);
    HU_RUN_TEST(test_is_private_ip_documentation_range);
    HU_RUN_TEST(test_validate_url_mailto_rejected);
}
