#include "common.h"
#include "hashtable.h"
#include "heap.h"
#include "list.h"
#include "thread_pool.h"
#include "zset.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
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
constexpr size_t kMaxArguments = 200u * 1000;
constexpr uint64_t kIdleTimeoutMs = 5u * 1000;
constexpr size_t kMaxTimerWork = 2000;
constexpr size_t kLargeContainerSize = 1000;

using Buffer = std::vector<uint8_t>;

void message(const char *text) {
    std::fprintf(stderr, "%s\n", text);
}

void message_errno(const char *text) {
    std::fprintf(stderr, "[errno:%d] %s\n", errno, text);
}

[[noreturn]] void die(const char *text) {
    std::fprintf(stderr, "[errno:%d] %s\n", errno, text);
    std::abort();
}

uint64_t monotonic_milliseconds() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        die("clock_gettime()");
    }
    return static_cast<uint64_t>(now.tv_sec) * 1000 +
           static_cast<uint64_t>(now.tv_nsec) / 1000 / 1000;
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        die("fcntl()");
    }
}

void buf_append(Buffer &buffer, const uint8_t *data, size_t len) {
    buffer.insert(buffer.end(), data, data + len);
}

void buf_consume(Buffer &buffer, size_t len) {
    assert(len <= buffer.size());
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(len));
}

struct Connection {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    Buffer incoming;
    Buffer outgoing;
    uint64_t last_active_ms = 0;
    DList idle_node;
};

struct ServerState {
    HMap db;
    std::vector<Connection *> fd_to_connection;
    DList idle_list;
    std::vector<HeapItem> ttl_heap;
    ThreadPool thread_pool;
};

ServerState state;

void destroy_connection(Connection *connection) {
    close(connection->fd);
    state.fd_to_connection[connection->fd] = nullptr;
    dlist_detach(&connection->idle_node);
    delete connection;
}

void accept_connections(int listening_fd) {
    while (true) {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);
        const int connection_fd =
            accept(listening_fd, reinterpret_cast<sockaddr *>(&client_address),
                   &address_length);
        if (connection_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            message_errno("accept()");
            return;
        }

        const uint32_t ip = client_address.sin_addr.s_addr;
        std::fprintf(stderr, "new client from %u.%u.%u.%u:%u\n", ip & 255,
                     (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
                     ntohs(client_address.sin_port));

        set_nonblocking(connection_fd);
        Connection *connection = new Connection();
        connection->fd = connection_fd;
        connection->want_read = true;
        connection->last_active_ms = monotonic_milliseconds();
        dlist_init(&connection->idle_node);
        dlist_insert_before(&state.idle_list, &connection->idle_node);

        if (state.fd_to_connection.size() <=
            static_cast<size_t>(connection_fd)) {
            state.fd_to_connection.resize(connection_fd + 1);
        }
        assert(!state.fd_to_connection[connection_fd]);
        state.fd_to_connection[connection_fd] = connection;
    }
}

bool read_u32(const uint8_t *&cursor, const uint8_t *end, uint32_t &out) {
    if (static_cast<size_t>(end - cursor) < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, cursor, sizeof(out));
    cursor += sizeof(out);
    return true;
}

bool read_string(const uint8_t *&cursor, const uint8_t *end, size_t len,
                 std::string &out) {
    if (len > static_cast<size_t>(end - cursor)) {
        return false;
    }
    out.assign(reinterpret_cast<const char *>(cursor), len);
    cursor += len;
    return true;
}

int32_t parse_request(const uint8_t *data, size_t size,
                      std::vector<std::string> &out) {
    const uint8_t *end = data + size;
    uint32_t string_count = 0;
    if (!read_u32(data, end, string_count) || string_count > kMaxArguments) {
        return -1;
    }

    out.reserve(string_count);
    while (out.size() < string_count) {
        uint32_t len = 0;
        if (!read_u32(data, end, len)) {
            return -1;
        }
        out.emplace_back();
        if (!read_string(data, end, len, out.back())) {
            return -1;
        }
    }
    return data == end ? 0 : -1;
}

enum ErrorCode {
    ERR_UNKNOWN = 1,
    ERR_TOO_BIG = 2,
    ERR_BAD_TYPE = 3,
    ERR_BAD_ARGUMENT = 4,
};

enum ValueTag : uint8_t {
    TAG_NIL = 0,
    TAG_ERROR = 1,
    TAG_STRING = 2,
    TAG_INTEGER = 3,
    TAG_DOUBLE = 4,
    TAG_ARRAY = 5,
};

void append_u8(Buffer &out, uint8_t value) {
    out.push_back(value);
}

void append_u32(Buffer &out, uint32_t value) {
    buf_append(out, reinterpret_cast<const uint8_t *>(&value), sizeof(value));
}

void append_i64(Buffer &out, int64_t value) {
    buf_append(out, reinterpret_cast<const uint8_t *>(&value), sizeof(value));
}

void append_double(Buffer &out, double value) {
    buf_append(out, reinterpret_cast<const uint8_t *>(&value), sizeof(value));
}

void out_nil(Buffer &out) {
    append_u8(out, TAG_NIL);
}

void out_string(Buffer &out, const char *data, size_t size) {
    append_u8(out, TAG_STRING);
    append_u32(out, static_cast<uint32_t>(size));
    buf_append(out, reinterpret_cast<const uint8_t *>(data), size);
}

void out_integer(Buffer &out, int64_t value) {
    append_u8(out, TAG_INTEGER);
    append_i64(out, value);
}

void out_double(Buffer &out, double value) {
    append_u8(out, TAG_DOUBLE);
    append_double(out, value);
}

void out_error(Buffer &out, uint32_t code, const std::string &text) {
    append_u8(out, TAG_ERROR);
    append_u32(out, code);
    append_u32(out, static_cast<uint32_t>(text.size()));
    buf_append(out, reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

void out_array(Buffer &out, uint32_t element_count) {
    append_u8(out, TAG_ARRAY);
    append_u32(out, element_count);
}

size_t begin_array(Buffer &out) {
    append_u8(out, TAG_ARRAY);
    append_u32(out, 0);
    return out.size() - sizeof(uint32_t);
}

void end_array(Buffer &out, size_t count_position, uint32_t element_count) {
    assert(count_position > 0 && out[count_position - 1] == TAG_ARRAY);
    std::memcpy(&out[count_position], &element_count, sizeof(element_count));
}

enum EntryType : uint32_t {
    TYPE_UNINITIALIZED = 0,
    TYPE_STRING = 1,
    TYPE_SORTED_SET = 2,
};

struct Entry {
    HNode node;
    std::string key;
    size_t heap_index = static_cast<size_t>(-1);
    EntryType type = TYPE_UNINITIALIZED;
    std::string string_value;
    ZSet sorted_set;
};

struct LookupKey {
    HNode node;
    const std::string *key = nullptr;
};

bool entry_equal(HNode *node, HNode *key_node) {
    Entry *entry = container_of(node, Entry, node);
    LookupKey *lookup = container_of(key_node, LookupKey, node);
    return entry->key == *lookup->key;
}

LookupKey make_lookup_key(const std::string &key) {
    LookupKey lookup;
    lookup.key = &key;
    lookup.node.hcode =
        str_hash(reinterpret_cast<const uint8_t *>(key.data()), key.size());
    return lookup;
}

Entry *lookup_entry(const std::string &key) {
    LookupKey lookup = make_lookup_key(key);
    HNode *node = hm_lookup(&state.db, &lookup.node, &entry_equal);
    return node ? container_of(node, Entry, node) : nullptr;
}

void heap_delete(std::vector<HeapItem> &heap, size_t position) {
    assert(position < heap.size());
    heap[position] = heap.back();
    heap.pop_back();
    if (position < heap.size()) {
        heap_update(heap.data(), position, heap.size());
    }
}

void heap_upsert(std::vector<HeapItem> &heap, size_t position, HeapItem item) {
    if (position < heap.size()) {
        heap[position] = item;
    } else {
        position = heap.size();
        heap.push_back(item);
    }
    heap_update(heap.data(), position, heap.size());
}

void set_entry_ttl(Entry *entry, int64_t ttl_ms) {
    const size_t no_index = static_cast<size_t>(-1);
    if (ttl_ms < 0) {
        if (entry->heap_index != no_index) {
            heap_delete(state.ttl_heap, entry->heap_index);
            entry->heap_index = no_index;
        }
        return;
    }

    const uint64_t expires_at =
        monotonic_milliseconds() + static_cast<uint64_t>(ttl_ms);
    HeapItem timer{expires_at, &entry->heap_index};
    heap_upsert(state.ttl_heap, entry->heap_index, timer);
}

void delete_entry_sync(Entry *entry) {
    if (entry->type == TYPE_SORTED_SET) {
        zset_clear(&entry->sorted_set);
    }
    delete entry;
}

void delete_entry_work(void *argument) {
    delete_entry_sync(static_cast<Entry *>(argument));
}

void delete_entry(Entry *entry) {
    set_entry_ttl(entry, -1);
    const size_t container_size =
        entry->type == TYPE_SORTED_SET ? hm_size(&entry->sorted_set.hmap) : 0;
    if (container_size > kLargeContainerSize) {
        thread_pool_queue(&state.thread_pool, &delete_entry_work, entry);
    } else {
        delete_entry_sync(entry);
    }
}

bool parse_integer(const std::string &text, int64_t &out) {
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    out = std::strtoll(text.c_str(), &end, 10);
    return errno != ERANGE && end == text.c_str() + text.size();
}

bool parse_double(const std::string &text, double &out) {
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    out = std::strtod(text.c_str(), &end);
    return errno != ERANGE && end == text.c_str() + text.size() &&
           !std::isnan(out);
}

void command_get(const std::vector<std::string> &command, Buffer &out) {
    Entry *entry = lookup_entry(command[1]);
    if (!entry) {
        return out_nil(out);
    }
    if (entry->type != TYPE_STRING) {
        return out_error(out, ERR_BAD_TYPE, "not a string value");
    }
    out_string(out, entry->string_value.data(), entry->string_value.size());
}

void command_set(std::vector<std::string> &command, Buffer &out) {
    Entry *entry = lookup_entry(command[1]);
    if (entry) {
        if (entry->type != TYPE_STRING) {
            return out_error(out, ERR_BAD_TYPE, "a non-string value exists");
        }
        entry->string_value.swap(command[2]);
    } else {
        entry = new Entry();
        entry->type = TYPE_STRING;
        entry->key.swap(command[1]);
        entry->string_value.swap(command[2]);
        entry->node.hcode = str_hash(
            reinterpret_cast<const uint8_t *>(entry->key.data()),
            entry->key.size());
        hm_insert(&state.db, &entry->node);
    }
    out_nil(out);
}

void command_delete(const std::vector<std::string> &command, Buffer &out) {
    LookupKey lookup = make_lookup_key(command[1]);
    HNode *node = hm_delete(&state.db, &lookup.node, &entry_equal);
    if (node) {
        delete_entry(container_of(node, Entry, node));
    }
    out_integer(out, node ? 1 : 0);
}

void command_expire(const std::vector<std::string> &command, Buffer &out) {
    int64_t ttl_ms = 0;
    if (!parse_integer(command[2], ttl_ms)) {
        return out_error(out, ERR_BAD_ARGUMENT, "expected int64 TTL");
    }
    Entry *entry = lookup_entry(command[1]);
    if (entry) {
        set_entry_ttl(entry, ttl_ms);
    }
    out_integer(out, entry ? 1 : 0);
}

void command_ttl(const std::vector<std::string> &command, Buffer &out) {
    Entry *entry = lookup_entry(command[1]);
    if (!entry) {
        return out_integer(out, -2);
    }
    if (entry->heap_index == static_cast<size_t>(-1)) {
        return out_integer(out, -1);
    }

    const uint64_t expires_at = state.ttl_heap[entry->heap_index].val;
    const uint64_t now = monotonic_milliseconds();
    out_integer(out, expires_at > now ? static_cast<int64_t>(expires_at - now)
                                      : 0);
}

void command_persist(const std::vector<std::string> &command, Buffer &out) {
    Entry *entry = lookup_entry(command[1]);
    if (!entry || entry->heap_index == static_cast<size_t>(-1)) {
        return out_integer(out, 0);
    }
    set_entry_ttl(entry, -1);
    out_integer(out, 1);
}

bool append_key(HNode *node, void *argument) {
    Buffer &out = *static_cast<Buffer *>(argument);
    const std::string &key = container_of(node, Entry, node)->key;
    out_string(out, key.data(), key.size());
    return true;
}

void command_keys(Buffer &out) {
    out_array(out, static_cast<uint32_t>(hm_size(&state.db)));
    hm_foreach(&state.db, &append_key, &out);
}

void command_zadd(const std::vector<std::string> &command, Buffer &out) {
    double score = 0;
    if (!parse_double(command[2], score)) {
        return out_error(out, ERR_BAD_ARGUMENT, "expected floating-point score");
    }

    Entry *entry = lookup_entry(command[1]);
    if (!entry) {
        entry = new Entry();
        entry->type = TYPE_SORTED_SET;
        entry->key = command[1];
        entry->node.hcode = str_hash(
            reinterpret_cast<const uint8_t *>(entry->key.data()),
            entry->key.size());
        hm_insert(&state.db, &entry->node);
    } else if (entry->type != TYPE_SORTED_SET) {
        return out_error(out, ERR_BAD_TYPE, "expected sorted set");
    }

    const std::string &name = command[3];
    const bool inserted =
        zset_insert(&entry->sorted_set, name.data(), name.size(), score);
    out_integer(out, inserted ? 1 : 0);
}

ZSet *expect_sorted_set(const std::string &key) {
    static ZSet empty;
    Entry *entry = lookup_entry(key);
    if (!entry) {
        return &empty;
    }
    return entry->type == TYPE_SORTED_SET ? &entry->sorted_set : nullptr;
}

void command_zrem(const std::vector<std::string> &command, Buffer &out) {
    ZSet *set = expect_sorted_set(command[1]);
    if (!set) {
        return out_error(out, ERR_BAD_TYPE, "expected sorted set");
    }
    const std::string &name = command[2];
    ZNode *node = zset_lookup(set, name.data(), name.size());
    if (node) {
        zset_delete(set, node);
    }
    out_integer(out, node ? 1 : 0);
}

void command_zscore(const std::vector<std::string> &command, Buffer &out) {
    ZSet *set = expect_sorted_set(command[1]);
    if (!set) {
        return out_error(out, ERR_BAD_TYPE, "expected sorted set");
    }
    const std::string &name = command[2];
    ZNode *node = zset_lookup(set, name.data(), name.size());
    if (node) {
        out_double(out, node->score);
    } else {
        out_nil(out);
    }
}

void command_zquery(const std::vector<std::string> &command, Buffer &out) {
    double score = 0;
    int64_t offset = 0;
    int64_t limit = 0;
    if (!parse_double(command[2], score)) {
        return out_error(out, ERR_BAD_ARGUMENT, "expected floating-point score");
    }
    if (!parse_integer(command[4], offset) ||
        !parse_integer(command[5], limit)) {
        return out_error(out, ERR_BAD_ARGUMENT, "expected integer offset and limit");
    }

    ZSet *set = expect_sorted_set(command[1]);
    if (!set) {
        return out_error(out, ERR_BAD_TYPE, "expected sorted set");
    }
    if (limit <= 0) {
        return out_array(out, 0);
    }

    const std::string &name = command[3];
    ZNode *node = zset_seekge(set, score, name.data(), name.size());
    node = znode_offset(node, offset);

    const size_t count_position = begin_array(out);
    uint32_t element_count = 0;
    while (node && static_cast<int64_t>(element_count / 2) < limit) {
        out_string(out, node->name, node->len);
        out_double(out, node->score);
        element_count += 2;
        node = znode_offset(node, 1);
    }
    end_array(out, count_position, element_count);
}

void execute_command(std::vector<std::string> &command, Buffer &out) {
    if (command.size() == 2 && command[0] == "get") {
        command_get(command, out);
    } else if (command.size() == 3 && command[0] == "set") {
        command_set(command, out);
    } else if (command.size() == 2 && command[0] == "del") {
        command_delete(command, out);
    } else if (command.size() == 1 && command[0] == "dbsize") {
        out_integer(out, static_cast<int64_t>(hm_size(&state.db)));
    } else if (command.size() == 1 && command[0] == "keys") {
        command_keys(out);
    } else if (command.size() == 3 && command[0] == "pexpire") {
        command_expire(command, out);
    } else if (command.size() == 2 && command[0] == "pttl") {
        command_ttl(command, out);
    } else if (command.size() == 2 && command[0] == "persist") {
        command_persist(command, out);
    } else if (command.size() == 4 && command[0] == "zadd") {
        command_zadd(command, out);
    } else if (command.size() == 3 && command[0] == "zrem") {
        command_zrem(command, out);
    } else if (command.size() == 3 && command[0] == "zscore") {
        command_zscore(command, out);
    } else if (command.size() == 6 && command[0] == "zquery") {
        command_zquery(command, out);
    } else {
        out_error(out, ERR_UNKNOWN, "unknown command");
    }
}

size_t response_begin(Buffer &out) {
    const size_t header = out.size();
    append_u32(out, 0);
    return header;
}

void response_end(Buffer &out, size_t header) {
    size_t message_size = out.size() - header - sizeof(uint32_t);
    if (message_size > kMaxMessage) {
        out.resize(header + sizeof(uint32_t));
        out_error(out, ERR_TOO_BIG, "response is too big");
        message_size = out.size() - header - sizeof(uint32_t);
    }
    const uint32_t length = static_cast<uint32_t>(message_size);
    std::memcpy(&out[header], &length, sizeof(length));
}

bool try_one_request(Connection *connection) {
    if (connection->incoming.size() < sizeof(uint32_t)) {
        return false;
    }

    uint32_t length = 0;
    std::memcpy(&length, connection->incoming.data(), sizeof(length));
    if (length > kMaxMessage) {
        message("request is too long");
        connection->want_close = true;
        return false;
    }
    if (connection->incoming.size() < sizeof(uint32_t) + length) {
        return false;
    }

    std::vector<std::string> command;
    if (parse_request(connection->incoming.data() + sizeof(uint32_t), length,
                      command) != 0) {
        message("bad request");
        connection->want_close = true;
        return false;
    }

    const size_t header = response_begin(connection->outgoing);
    execute_command(command, connection->outgoing);
    response_end(connection->outgoing, header);
    buf_consume(connection->incoming, sizeof(uint32_t) + length);
    return true;
}

void handle_write(Connection *connection) {
    assert(!connection->outgoing.empty());
    const ssize_t written =
        write(connection->fd, connection->outgoing.data(),
              connection->outgoing.size());
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    if (written < 0) {
        message_errno("write()");
        connection->want_close = true;
        return;
    }

    buf_consume(connection->outgoing, static_cast<size_t>(written));
    if (connection->outgoing.empty()) {
        connection->want_read = true;
        connection->want_write = false;
    }
}

void handle_read(Connection *connection) {
    uint8_t buffer[64 * 1024];
    const ssize_t read_count = read(connection->fd, buffer, sizeof(buffer));
    if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    if (read_count < 0) {
        message_errno("read()");
        connection->want_close = true;
        return;
    }
    if (read_count == 0) {
        message(connection->incoming.empty() ? "client closed" : "unexpected EOF");
        connection->want_close = true;
        return;
    }

    buf_append(connection->incoming, buffer, static_cast<size_t>(read_count));
    while (try_one_request(connection)) {
    }

    if (!connection->outgoing.empty()) {
        connection->want_read = false;
        connection->want_write = true;
        handle_write(connection);
    }
}

int32_t next_timer_milliseconds() {
    const uint64_t now = monotonic_milliseconds();
    uint64_t next = std::numeric_limits<uint64_t>::max();

    if (!dlist_empty(&state.idle_list)) {
        Connection *connection =
            container_of(state.idle_list.next, Connection, idle_node);
        next = connection->last_active_ms + kIdleTimeoutMs;
    }
    if (!state.ttl_heap.empty()) {
        next = std::min(next, state.ttl_heap.front().val);
    }
    if (next == std::numeric_limits<uint64_t>::max()) {
        return -1;
    }
    if (next <= now) {
        return 0;
    }
    const uint64_t delay = next - now;
    return delay > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
               ? std::numeric_limits<int32_t>::max()
               : static_cast<int32_t>(delay);
}

bool same_node(HNode *node, HNode *key) {
    return node == key;
}

void process_timers() {
    const uint64_t now = monotonic_milliseconds();

    while (!dlist_empty(&state.idle_list)) {
        Connection *connection =
            container_of(state.idle_list.next, Connection, idle_node);
        if (connection->last_active_ms + kIdleTimeoutMs > now) {
            break;
        }
        std::fprintf(stderr, "removing idle connection: %d\n", connection->fd);
        destroy_connection(connection);
    }

    size_t work_count = 0;
    while (!state.ttl_heap.empty() && state.ttl_heap.front().val <= now &&
           work_count < kMaxTimerWork) {
        Entry *entry =
            container_of(state.ttl_heap.front().ref, Entry, heap_index);
        HNode *node = hm_delete(&state.db, &entry->node, &same_node);
        assert(node == &entry->node);
        delete_entry(entry);
        ++work_count;
    }
}

uint16_t parse_port(int argc, char **argv) {
    if (argc == 1) {
        return kDefaultPort;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s [port]\n", argv[0]);
        std::exit(2);
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(argv[1], &end, 10);
    if (errno || end == argv[1] || *end != '\0' || value == 0 ||
        value > std::numeric_limits<uint16_t>::max()) {
        std::fprintf(stderr, "invalid port: %s\n", argv[1]);
        std::exit(2);
    }
    return static_cast<uint16_t>(value);
}

}  // namespace

int main(int argc, char **argv) {
    const uint16_t port = parse_port(argc, argv);
    std::signal(SIGPIPE, SIG_IGN);
    dlist_init(&state.idle_list);
    thread_pool_init(&state.thread_pool, 4);

    const int listening_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listening_fd < 0) {
        die("socket()");
    }
    int reuse_address = 1;
    if (setsockopt(listening_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) != 0) {
        die("setsockopt()");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listening_fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
        die("bind()");
    }
    set_nonblocking(listening_fd);
    if (listen(listening_fd, SOMAXCONN) != 0) {
        die("listen()");
    }
    std::fprintf(stderr, "listening on 0.0.0.0:%u\n", port);

    std::vector<pollfd> poll_arguments;
    while (true) {
        poll_arguments.clear();
        poll_arguments.push_back(pollfd{listening_fd, POLLIN, 0});
        for (Connection *connection : state.fd_to_connection) {
            if (!connection) {
                continue;
            }
            short events = POLLERR | POLLHUP | POLLNVAL;
            if (connection->want_read) {
                events |= POLLIN;
            }
            if (connection->want_write) {
                events |= POLLOUT;
            }
            poll_arguments.push_back(pollfd{connection->fd, events, 0});
        }

        const int result =
            poll(poll_arguments.data(), poll_arguments.size(),
                 next_timer_milliseconds());
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            die("poll()");
        }

        if (poll_arguments[0].revents & POLLIN) {
            accept_connections(listening_fd);
        }
        if (poll_arguments[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            die("listening socket error");
        }

        for (size_t i = 1; i < poll_arguments.size(); ++i) {
            const short ready = poll_arguments[i].revents;
            if (!ready) {
                continue;
            }
            Connection *connection =
                state.fd_to_connection[poll_arguments[i].fd];
            if (!connection) {
                continue;
            }

            connection->last_active_ms = monotonic_milliseconds();
            dlist_detach(&connection->idle_node);
            dlist_insert_before(&state.idle_list, &connection->idle_node);

            if (ready & POLLIN) {
                assert(connection->want_read);
                handle_read(connection);
            }
            if ((ready & POLLOUT) && !connection->want_close) {
                assert(connection->want_write);
                handle_write(connection);
            }
            if ((ready & (POLLERR | POLLHUP | POLLNVAL)) ||
                connection->want_close) {
                destroy_connection(connection);
            }
        }
        process_timers();
    }
}
