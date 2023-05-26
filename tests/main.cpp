#include <algorithm>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include "../src/ringbuffer.h"

typedef ringbuffer_mpmc test_ringbuffer;

static const std::string_view test_str = []() {
    const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static std::string str;
    for (int i = 0; i < 1000; ++i) {
        str += base64_chars[i % 64];
    }

    return std::string_view(str);
}();

void test_write(test_ringbuffer* rb) {
    while (true) {
        rb->write((const uint8_t*)test_str.data(), test_str.length());
    }
}

void test_read(test_ringbuffer* rb) {
    while (true) {
        uint8_t buf[test_str.length()];
        rb->read(buf, test_str.length());
        assert(std::string_view((const char*)buf, test_str.length()) == test_str);
    }
}

std::string convertBytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double converted = static_cast<double>(bytes);
    while (converted >= 1024 && unit < 4) {
        converted /= 1024;
        ++unit;
    }
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << converted << " " << units[unit];
    return ss.str();
}

void on_time_print(test_ringbuffer* rb) {
    uint64_t last_total_write = 0;
    uint64_t last_total_read = 0;
    for (int i = 1;; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t total_read = rb->get_total_out();
        uint64_t total_write = rb->get_total_in();

        uint64_t avg_write = total_write / i;
        uint64_t avg_read = total_write / i;

        spdlog::info("cur write:{}/S, read:{}/S, avg write:{}/S, read:{}/S, total write:{}, total read:{}", convertBytes(total_write - last_total_write),
                     convertBytes(total_read - last_total_read), convertBytes(avg_write), convertBytes(avg_read), convertBytes(total_write),
                     convertBytes(total_read));

        last_total_write = total_write;
        last_total_read = total_read;
    }
}

int main(int argc, char* argv[]) {
    int n = std::thread::hardware_concurrency();
    int w = 1;
    int r = n - w > 0 ? n - w : 1;

    if (argc > 1) {
        w = std::atoi(argv[1]);
    }

    if (argc > 2) {
        r = std::atoi(argv[2]);
    }

    spdlog::default_logger()->set_pattern("%Y-%m-%d %H:%M:%S.%f [%P](%t) [%s:%#] [%l] %v");

    std::cout << "set w = " << w << ", r = " << r << std::endl;

    //8k size
    test_ringbuffer rb(1ul << 13, false);

    std::vector<std::thread> all_threads;
    all_threads.reserve(w + r + 1);
    all_threads.emplace_back(on_time_print, &rb);

    for (auto _ : std::views::iota(0, r)) {
        all_threads.emplace_back(test_read, &rb);
    }

    for (auto _ : std::views::iota(0, w)) {
        all_threads.emplace_back(test_write, &rb);
    }

    for (auto& e : all_threads) {
        e.join();
    }

    return 0;
}