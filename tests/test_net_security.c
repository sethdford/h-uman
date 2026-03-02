/* Net security tests */
#include "test_framework.h"
#include "seaclaw/net_security.h"
#include <string.h>

static void test_validate_url_https_ok(void) {
    sc_error_t err = sc_validate_url("https://example.com/path");
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_validate_url_http_localhost_ok(void) {
    sc_error_t err = sc_validate_url("http://localhost:8080/api");
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_validate_url_http_remote_fail(void) {
    sc_error_t err = sc_validate_url("http://example.com/");
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_validate_url_ftp_fail(void) {
    sc_error_t err = sc_validate_url("ftp://example.com");
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_is_private_ip_loopback(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("127.0.0.1"));
    SC_ASSERT_TRUE(sc_is_private_ip("localhost"));
    SC_ASSERT_TRUE(sc_is_private_ip("::1"));
}

static void test_is_private_ip_private_ranges(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("10.0.0.1"));
    SC_ASSERT_TRUE(sc_is_private_ip("192.168.1.1"));
    SC_ASSERT_TRUE(sc_is_private_ip("172.16.0.1"));
}

static void test_is_private_ip_public(void) {
    SC_ASSERT_FALSE(sc_is_private_ip("8.8.8.8"));
    SC_ASSERT_FALSE(sc_is_private_ip("1.1.1.1"));
}

static void test_validate_domain_exact(void) {
    const char *allowed[] = {"example.com"};
    SC_ASSERT_TRUE(sc_validate_domain("example.com", allowed, 1));
}

static void test_validate_domain_wildcard(void) {
    const char *allowed[] = {"*.example.com"};
    SC_ASSERT_TRUE(sc_validate_domain("api.example.com", allowed, 1));
    SC_ASSERT_FALSE(sc_validate_domain("evil.com", allowed, 1));
}

static void test_validate_domain_empty_allowlist(void) {
    const char *allowed[] = {NULL};
    SC_ASSERT_TRUE(sc_validate_domain("anything.com", allowed, 0));
}

static void test_validate_url_https_with_path(void) {
    sc_error_t err = sc_validate_url("https://api.example.com/v1/endpoint?key=val");
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_validate_url_https_with_port(void) {
    sc_error_t err = sc_validate_url("https://example.com:443/path");
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_validate_url_http_127_rejected(void) {
    sc_error_t err = sc_validate_url("http://127.0.0.1:3000/");
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_is_private_ip_172_31(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("172.31.0.1"));
}

static void test_is_private_ip_172_15(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("172.16.0.0"));
}

static void test_is_private_ip_192_168(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("192.168.0.1"));
    SC_ASSERT_TRUE(sc_is_private_ip("192.168.255.255"));
}

static void test_is_private_ip_10_full_range(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("10.0.0.0"));
    SC_ASSERT_TRUE(sc_is_private_ip("10.255.255.255"));
}

static void test_is_private_ip_public_domain(void) {
    SC_ASSERT_FALSE(sc_is_private_ip("google.com"));
}

static void test_validate_domain_exact_match(void) {
    const char *allowed[] = {"api.service.com"};
    SC_ASSERT_TRUE(sc_validate_domain("api.service.com", allowed, 1));
    SC_ASSERT_FALSE(sc_validate_domain("other.service.com", allowed, 1));
}

static void test_validate_domain_multiple_allowed(void) {
    const char *allowed[] = {"a.com", "b.com", "c.com"};
    SC_ASSERT_TRUE(sc_validate_domain("a.com", allowed, 3));
    SC_ASSERT_TRUE(sc_validate_domain("b.com", allowed, 3));
    SC_ASSERT_FALSE(sc_validate_domain("d.com", allowed, 3));
}

static void test_validate_url_null_empty(void) {
    sc_error_t err = sc_validate_url("");
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_validate_url_file_rejected(void) {
    sc_error_t err = sc_validate_url("file:///etc/passwd");
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_validate_url_javascript_rejected(void) {
    sc_error_t err = sc_validate_url("javascript:alert(1)");
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_validate_url_data_rejected(void) {
    sc_error_t err = sc_validate_url("data:text/plain,hello");
    SC_ASSERT_NEQ(err, SC_OK);
}

/* ─── WP-21B parity: URL scheme, IP, domain edge cases ───────────────────── */
static void test_validate_url_null_rejected(void) {
    sc_error_t err = sc_validate_url(NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_validate_url_https_query_fragment(void) {
    sc_error_t err = sc_validate_url("https://example.com/path?foo=bar#anchor");
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_validate_url_ws_rejected(void) {
    sc_error_t err = sc_validate_url("ws://example.com/socket");
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_validate_url_wss_rejected(void) {
    sc_error_t err = sc_validate_url("wss://example.com/socket");
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_validate_url_http_localhost_no_port(void) {
    sc_error_t err = sc_validate_url("http://localhost/");
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_is_private_ip_link_local(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("169.254.1.1"));
}

static void test_is_private_ip_ipv6_loopback_bracketed(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("[::1]"));
}

static void test_is_private_ip_ipv6_unique_local(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("fd00::1"));
}

static void test_is_private_ip_empty_fail_closed(void) {
    SC_ASSERT_TRUE(sc_is_private_ip(""));
}

static void test_is_private_ip_null_fail_closed(void) {
    SC_ASSERT_TRUE(sc_is_private_ip(NULL));
}

static void test_validate_domain_null_host_fail(void) {
    const char *allowed[] = {"example.com"};
    SC_ASSERT_FALSE(sc_validate_domain(NULL, allowed, 1));
}

static void test_validate_domain_empty_host_fail(void) {
    const char *allowed[] = {"example.com"};
    SC_ASSERT_FALSE(sc_validate_domain("", allowed, 1));
}

static void test_validate_domain_wildcard_subdomain_deep(void) {
    const char *allowed[] = {"*.example.com"};
    SC_ASSERT_TRUE(sc_validate_domain("api.v1.example.com", allowed, 1));
}

static void test_validate_domain_implicit_subdomain(void) {
    const char *allowed[] = {"example.com"};
    SC_ASSERT_TRUE(sc_validate_domain("sub.example.com", allowed, 1));
}

static void test_validate_domain_wildcard_wrong_tld(void) {
    const char *allowed[] = {"*.example.com"};
    SC_ASSERT_FALSE(sc_validate_domain("example.org", allowed, 1));
}

static void test_validate_domain_null_pattern_skipped(void) {
    const char *allowed[] = {NULL, "b.com"};
    SC_ASSERT_TRUE(sc_validate_domain("b.com", allowed, 2));
}

static void test_is_private_ip_documentation_range(void) {
    SC_ASSERT_TRUE(sc_is_private_ip("192.0.2.1"));
}

static void test_validate_url_mailto_rejected(void) {
    sc_error_t err = sc_validate_url("mailto:test@example.com");
    SC_ASSERT_NEQ(err, SC_OK);
}

void run_net_security_tests(void) {
    SC_TEST_SUITE("Net security");
    SC_RUN_TEST(test_validate_url_https_ok);
    SC_RUN_TEST(test_validate_url_http_localhost_ok);
    SC_RUN_TEST(test_validate_url_http_remote_fail);
    SC_RUN_TEST(test_validate_url_ftp_fail);
    SC_RUN_TEST(test_is_private_ip_loopback);
    SC_RUN_TEST(test_is_private_ip_private_ranges);
    SC_RUN_TEST(test_is_private_ip_public);
    SC_RUN_TEST(test_validate_domain_exact);
    SC_RUN_TEST(test_validate_domain_wildcard);
    SC_RUN_TEST(test_validate_domain_empty_allowlist);
    SC_RUN_TEST(test_validate_url_https_with_path);
    SC_RUN_TEST(test_validate_url_https_with_port);
    SC_RUN_TEST(test_validate_url_http_127_rejected);
    SC_RUN_TEST(test_is_private_ip_172_31);
    SC_RUN_TEST(test_is_private_ip_172_15);
    SC_RUN_TEST(test_is_private_ip_192_168);
    SC_RUN_TEST(test_is_private_ip_10_full_range);
    SC_RUN_TEST(test_is_private_ip_public_domain);
    SC_RUN_TEST(test_validate_domain_exact_match);
    SC_RUN_TEST(test_validate_domain_multiple_allowed);
    SC_RUN_TEST(test_validate_url_null_empty);
    SC_RUN_TEST(test_validate_url_file_rejected);
    SC_RUN_TEST(test_validate_url_javascript_rejected);
    SC_RUN_TEST(test_validate_url_data_rejected);
    SC_RUN_TEST(test_validate_url_null_rejected);
    SC_RUN_TEST(test_validate_url_https_query_fragment);
    SC_RUN_TEST(test_validate_url_ws_rejected);
    SC_RUN_TEST(test_validate_url_wss_rejected);
    SC_RUN_TEST(test_validate_url_http_localhost_no_port);
    SC_RUN_TEST(test_is_private_ip_link_local);
    SC_RUN_TEST(test_is_private_ip_ipv6_loopback_bracketed);
    SC_RUN_TEST(test_is_private_ip_ipv6_unique_local);
    SC_RUN_TEST(test_is_private_ip_empty_fail_closed);
    SC_RUN_TEST(test_is_private_ip_null_fail_closed);
    SC_RUN_TEST(test_validate_domain_null_host_fail);
    SC_RUN_TEST(test_validate_domain_empty_host_fail);
    SC_RUN_TEST(test_validate_domain_wildcard_subdomain_deep);
    SC_RUN_TEST(test_validate_domain_implicit_subdomain);
    SC_RUN_TEST(test_validate_domain_wildcard_wrong_tld);
    SC_RUN_TEST(test_validate_domain_null_pattern_skipped);
    SC_RUN_TEST(test_is_private_ip_documentation_range);
    SC_RUN_TEST(test_validate_url_mailto_rejected);
}
