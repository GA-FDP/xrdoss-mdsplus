#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "ResultCache.hh"

#include <string>
#include <thread>
#include <vector>

TEST_CASE("stores and retrieves by key") {
    fdp::ResultCache cache(1024);
    cache.Put("/a", "hello");

    std::string out;
    REQUIRE(cache.Get("/a", out));
    CHECK(out == "hello");
}

TEST_CASE("misses on an unknown key") {
    fdp::ResultCache cache(1024);
    std::string out;
    CHECK_FALSE(cache.Get("/nope", out));
}

TEST_CASE("evicts least-recently-used entries past the byte budget") {
    fdp::ResultCache cache(10);
    cache.Put("/a", "12345");
    cache.Put("/b", "12345");

    std::string out;
    REQUIRE(cache.Get("/a", out));      // touch /a so /b becomes the LRU victim

    cache.Put("/c", "12345");
    CHECK(cache.Get("/a", out));
    CHECK_FALSE(cache.Get("/b", out));
    CHECK(cache.Get("/c", out));
}

TEST_CASE("refuses entries larger than the whole budget") {
    fdp::ResultCache cache(4);
    cache.Put("/big", "12345");
    std::string out;
    CHECK_FALSE(cache.Get("/big", out));
    CHECK(cache.BytesUsed() == 0);
}

TEST_CASE("overwrites an existing key without double-counting bytes") {
    fdp::ResultCache cache(10);
    cache.Put("/a", "12345");
    cache.Put("/a", "abcde");
    cache.Put("/b", "12345");

    std::string out;
    REQUIRE(cache.Get("/a", out));
    CHECK(out == "abcde");
    CHECK(cache.Get("/b", out));
    CHECK(cache.BytesUsed() == 10);
}

TEST_CASE("never exceeds the byte budget") {
    fdp::ResultCache cache(100);
    for (int i = 0; i < 200; ++i) {
        cache.Put("/k" + std::to_string(i), std::string(17, 'x'));
        CHECK(cache.BytesUsed() <= 100);
    }
}

TEST_CASE("a zero budget caches nothing but still works") {
    fdp::ResultCache cache(0);
    cache.Put("/a", "x");
    std::string out;
    CHECK_FALSE(cache.Get("/a", out));
    CHECK(cache.BytesUsed() == 0);
}

TEST_CASE("stores payloads containing NUL bytes") {
    fdp::ResultCache cache(1024);
    const std::string binary("\x00\xff\x00\x01", 4);
    cache.Put("/bin", binary);

    std::string out;
    REQUIRE(cache.Get("/bin", out));
    CHECK(out == binary);
    CHECK(out.size() == 4);
}

TEST_CASE("is safe under concurrent access") {
    fdp::ResultCache cache(1 << 20);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.push_back(std::thread([&cache, t] {
            for (int i = 0; i < 500; ++i) {
                const std::string key = "/k" + std::to_string((t * 500 + i) % 100);
                cache.Put(key, std::string(64, 'x'));
                std::string out;
                cache.Get(key, out);
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
    CHECK(cache.BytesUsed() <= (1u << 20));
}
