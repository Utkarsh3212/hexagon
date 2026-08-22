#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr uint16_t kDefaultPort = 2203;
constexpr size_t kMaxMessage = 32u << 20;

[[noreturn]] void die(const char *message) {
    std::fprintf(stderr, "[errno:%d] %s\n", errno, message);
    std::exit(1);
}

bool read_full(int fd, uint8_t *buffer, size_t size) {
    while (size > 0) {
        const ssize_t count = read(fd, buffer, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        buffer += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

bool write_all(int fd, const uint8_t *buffer, size_t size) {
    while (size > 0) {
        const ssize_t count = write(fd, buffer, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        buffer += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

bool send_request(int fd, const std::vector<std::string> &command) {
    size_t body_size = sizeof(uint32_t);
    for (const std::string &argument : command) {
        body_size += sizeof(uint32_t) + argument.size();
    }
    if (body_size > kMaxMessage || body_size > UINT32_MAX) {
        std::fprintf(stderr, "request is too large\n");
        return false;
    }

    std::vector<uint8_t> request(sizeof(uint32_t) + body_size);
    const uint32_t body_length = static_cast<uint32_t>(body_size);
    const uint32_t argument_count = static_cast<uint32_t>(command.size());
    std::memcpy(request.data(), &body_length, sizeof(body_length));
    std::memcpy(request.data() + sizeof(uint32_t), &argument_count,
                sizeof(argument_count));

    size_t cursor = sizeof(uint32_t) * 2;
    for (const std::string &argument : command) {
        const uint32_t length = static_cast<uint32_t>(argument.size());
        std::memcpy(request.data() + cursor, &length, sizeof(length));
        cursor += sizeof(length);
        std::memcpy(request.data() + cursor, argument.data(), argument.size());
        cursor += argument.size();
    }
    return write_all(fd, request.data(), request.size());
}

enum ValueTag : uint8_t {
    TAG_NIL = 0,
    TAG_ERROR = 1,
    TAG_STRING = 2,
    TAG_INTEGER = 3,
    TAG_DOUBLE = 4,
    TAG_ARRAY = 5,
};

bool available(size_t size, size_t cursor, size_t needed) {
    return cursor <= size && needed <= size - cursor;
}

int64_t print_value(const uint8_t *data, size_t size, unsigned depth = 0) {
    if (!size) {
        return -1;
    }
    const char *indent = depth ? "  " : "";
    switch (data[0]) {
        case TAG_NIL:
            std::printf("%s(nil)\n", indent);
            return 1;
        case TAG_ERROR: {
            if (!available(size, 1, 8)) {
                return -1;
            }
            uint32_t code = 0;
            uint32_t length = 0;
            std::memcpy(&code, data + 1, 4);
            std::memcpy(&length, data + 5, 4);
            if (!available(size, 9, length)) {
                return -1;
            }
            std::printf("%s(error %u) ", indent, code);
            std::fwrite(data + 9, 1, length, stdout);
            std::putchar('\n');
            return 9 + length;
        }
        case TAG_STRING: {
            if (!available(size, 1, 4)) {
                return -1;
            }
            uint32_t length = 0;
            std::memcpy(&length, data + 1, 4);
            if (!available(size, 5, length)) {
                return -1;
            }
            std::printf("%s(string) ", indent);
            std::fwrite(data + 5, 1, length, stdout);
            std::putchar('\n');
            return 5 + length;
        }
        case TAG_INTEGER: {
            if (!available(size, 1, 8)) {
                return -1;
            }
            int64_t value = 0;
            std::memcpy(&value, data + 1, 8);
            std::printf("%s(integer) %lld\n", indent,
                        static_cast<long long>(value));
            return 9;
        }
        case TAG_DOUBLE: {
            if (!available(size, 1, 8)) {
                return -1;
            }
            double value = 0;
            std::memcpy(&value, data + 1, 8);
            std::printf("%s(double) %g\n", indent, value);
            return 9;
        }
        case TAG_ARRAY: {
            if (!available(size, 1, 4)) {
                return -1;
            }
            uint32_t length = 0;
            std::memcpy(&length, data + 1, 4);
            std::printf("%s(array) %u element(s)\n", indent, length);
            size_t cursor = 5;
            for (uint32_t i = 0; i < length; ++i) {
                const int64_t consumed = print_value(data + cursor, size - cursor,
                                                     depth + 1);
                if (consumed < 0) {
                    return -1;
                }
                cursor += static_cast<size_t>(consumed);
            }
            return static_cast<int64_t>(cursor);
        }
        default:
            return -1;
    }
}

bool read_response(int fd) {
    uint32_t length = 0;
    if (!read_full(fd, reinterpret_cast<uint8_t *>(&length), sizeof(length))) {
        std::fprintf(stderr, "failed to read response header\n");
        return false;
    }
    if (length > kMaxMessage) {
        std::fprintf(stderr, "response is too large\n");
        return false;
    }

    std::vector<uint8_t> response(length);
    if (!read_full(fd, response.data(), response.size())) {
        std::fprintf(stderr, "failed to read response body\n");
        return false;
    }
    const int64_t consumed = print_value(response.data(), response.size());
    if (consumed < 0 || static_cast<size_t>(consumed) != response.size()) {
        std::fprintf(stderr, "malformed response\n");
        return false;
    }
    return true;
}

uint16_t parse_port(const char *text) {
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno || end == text || *end != '\0' || value == 0 ||
        value > std::numeric_limits<uint16_t>::max()) {
        std::fprintf(stderr, "invalid port: %s\n", text);
        std::exit(2);
    }
    return static_cast<uint16_t>(value);
}

}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);
    uint16_t port = kDefaultPort;
    int argument_index = 1;
    if (argc >= 3 && std::strcmp(argv[1], "--port") == 0) {
        port = parse_port(argv[2]);
        argument_index = 3;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
        die("connect()");
    }

    std::vector<std::string> command;
    for (int i = argument_index; i < argc; ++i) {
        command.emplace_back(argv[i]);
    }
    const bool ok = send_request(fd, command) && read_response(fd);
    close(fd);
    return ok ? 0 : 1;
}
