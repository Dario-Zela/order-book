#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "itch/mmap_file.hpp"

namespace {

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

}  // namespace

TEST_CASE("MmapFile maps file contents") {
    const auto path = temp_path("ob_mmap_test.bin");
    {
        std::ofstream f(path, std::ios::binary);
        f << "hello, itch";
    }
    {
        ob::itch::MmapFile m(path);
        REQUIRE(m.size() == 11);
        const auto b = m.bytes();
        CHECK(static_cast<char>(b[0]) == 'h');
        CHECK(static_cast<char>(b[10]) == 'h');
    }
    std::filesystem::remove(path);
}

TEST_CASE("MmapFile on empty file yields empty span") {
    const auto path = temp_path("ob_mmap_empty.bin");
    { std::ofstream f(path, std::ios::binary); }
    {
        ob::itch::MmapFile m(path);
        CHECK(m.size() == 0);
        CHECK(m.bytes().empty());
    }
    std::filesystem::remove(path);
}

TEST_CASE("MmapFile on missing file throws") {
    CHECK_THROWS_AS(ob::itch::MmapFile("/nonexistent/definitely/missing.bin"),
                    std::system_error);
}
