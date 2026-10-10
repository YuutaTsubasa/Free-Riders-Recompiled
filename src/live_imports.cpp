#include "live_imports.h"

#include "guest_heap.h"
#include "guest_memory.h"
#include "live_client.h"
#include "local_profile.h"

#include <array>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// Layouts and message numbers follow Xenia Canary's netplay build
// (AdrianCassar/xenia-canary, netplay_canary_experimental: xnet.h,
// xsession.h, xgi_app.cc, xam_net.cc; BSD licence), checked against what
// this title sends (docs/xbox-live.md).
namespace sfr {
namespace {
constexpr uint32_t ok = 0, failed = 0x65B /* ERROR_FUNCTION_FAILED */, insufficient = 0x7A, pending = 997;
constexpr uint32_t heap_tag = 0x4C495645;  // "LIVE": blocks this file allocates
constexpr uint32_t handle_base = 0x4C760000;
constexpr uint16_t game_port = 1000;
constexpr uint32_t session_host = 0x01;

uint64_t parse_hex(const std::string& text) { return std::strtoull(text.c_str(), nullptr, 16); }
std::vector<uint8_t> parse_bytes(const std::string& text) {
    std::vector<uint8_t> bytes;
    if (text == "-") return bytes;
    for (size_t i = 0; i + 1 < text.size(); i += 2) bytes.push_back(uint8_t(parse_hex(text.substr(i, 2))));
    return bytes;
}
std::string hex(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        text += digits[data[i] >> 4];
        text += digits[data[i] & 15];
    }
    return text.empty() ? "-" : text;
}
std::vector<std::string> words(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> out;
    for (std::string word; stream >> word;) out.push_back(word);
    return out;
}

struct Session {
    uint32_t handle = 0, object = 0;
    bool created = false, host = false;
    uint32_t flags = 0, public_slots = 0, private_slots = 0;
    std::array<uint8_t, 8> id{};
    std::array<uint8_t, 16> key{};
    uint64_t nonce = 0;
    uint32_t host_address = 0;
    std::array<uint8_t, 6> host_mac{};
    uint64_t host_machine = 0;
    std::string id_text() const { return hex(id.data(), id.size()); }
};

struct Socket {
    uint16_t port = 0;
    bool nonblocking = false;
};

struct Peer {
    std::array<uint8_t, 6> mac{};
    uint64_t machine = 0;
};

struct State {
    std::mutex mutex;
    std::map<uint32_t, Session> sessions;  // by handle
    std::map<uint32_t, Socket> sockets;
    std::map<uint32_t, Peer> peers;  // by address
    std::map<uint32_t, uint32_t> contexts;
    std::map<uint32_t, std::vector<uint8_t>> properties;  // XUSER_PROPERTY, 0x18 bytes
    uint32_t next_handle = handle_base + 1, next_socket = 0x100;
    uint16_t next_port = 49152;
    uint64_t random() {
        static std::mt19937_64 generator{std::random_device{}()};
        return generator();
    }
};
State& state() {
    static State s;
    return s;
}

uint32_t allocate(uint32_t size) { return active_heap ? active_heap->allocate(heap_tag, size, true) : 0; }

void write_bytes(GuestMemory& memory, uint32_t address, const uint8_t* data, size_t size) {
    memory.write_bytes(address, std::span<const uint8_t>(data, size));
}

// XNADDR (0x24): ina, inaOnline, wPortOnline, abEnet[6], then the security
// gateway address: ina, SPI, xbox id (the machine id), platform.
void write_xnaddr(GuestMemory& memory, uint32_t at, uint32_t address, const std::array<uint8_t, 6>& mac, uint64_t machine) {
    memory.check_write(at, 0x24);
    for (uint32_t i = 0; i < 0x24; i += 4) memory.store<uint32_t>(at + i, 0);
    memory.store<uint32_t>(at, address);
    memory.store<uint32_t>(at + 4, address);
    memory.store<uint16_t>(at + 8, game_port);
    write_bytes(memory, at + 10, mac.data(), 6);
    memory.store<uint32_t>(at + 0x10, address);
    memory.store<uint64_t>(at + 0x18, machine);
}
void write_own_xnaddr(GuestMemory& memory, uint32_t at) {
    const auto* client = LiveClient::instance();
    write_xnaddr(memory, at, client->address(), client->mac(), client->machine());
}

Session* session_for_object(uint32_t object, GuestMemory& memory) {
    if (!object || !memory.readable(object, 4)) return nullptr;
    const auto found = state().sessions.find(memory.load<uint32_t>(object));
    return found == state().sessions.end() ? nullptr : &found->second;
}

std::vector<std::string> ask(const std::string& line) {
    auto answer = LiveClient::instance()->request(line);
    if (answer.empty() || answer[0].rfind("OK", 0) != 0)
        std::cerr << "LIVE_REQUEST " << line.substr(0, line.find(' ')) << " answer="
                  << (answer.empty() ? std::string("none") : answer[0]) << '\n';
    return answer;
}

// Properties travel as XUSER_PROPERTY (0x18) entries as the guest holds them,
// a string's or binary's bytes (their size at +16) right after its entry,
// padded to 4. Contexts travel as XUSER_CONTEXT (8) entries.
constexpr uint32_t property_hostname = 0x40008109;  // X_PROPERTY_GAMER_HOSTNAME
bool has_payload(uint32_t id) { return (id >> 28) == 4 || (id >> 28) == 6; }

std::string properties_text() {
    std::vector<uint8_t> bytes;
    auto properties = state().properties;
    if (!properties.count(property_hostname)) {
        // The host's gamertag, which LIVE adds to every search result.
        const std::string gamertag = live_gamertag();
        std::vector<uint8_t> entry(0x18, 0);
        for (int i = 0; i < 4; ++i) entry[i] = uint8_t(property_hostname >> (24 - 8 * i));
        entry[8] = 4;
        const uint32_t size = uint32_t(gamertag.size() + 1) * 2;
        for (int i = 0; i < 4; ++i) entry[16 + i] = uint8_t(size >> (24 - 8 * i));
        for (char c : gamertag) entry.insert(entry.end(), {0, uint8_t(c)});
        entry.insert(entry.end(), {0, 0});
        properties[property_hostname] = entry;
    }
    for (auto [id, property] : properties) {
        while (property.size() % 4) property.push_back(0);
        bytes.insert(bytes.end(), property.begin(), property.end());
    }
    return hex(bytes.data(), bytes.size());
}

struct Property {
    std::vector<uint8_t> entry, payload;
};
std::vector<Property> split_properties(const std::vector<uint8_t>& bytes) {
    std::vector<Property> out;
    for (size_t at = 0; at + 0x18 <= bytes.size();) {
        Property p;
        p.entry.assign(bytes.begin() + at, bytes.begin() + at + 0x18);
        at += 0x18;
        const uint32_t id = uint32_t(p.entry[0]) << 24 | p.entry[1] << 16 | p.entry[2] << 8 | p.entry[3];
        if (has_payload(id)) {
            const uint32_t size = uint32_t(p.entry[16]) << 24 | p.entry[17] << 16 | p.entry[18] << 8 | p.entry[19];
            if (at + size > bytes.size()) break;
            p.payload.assign(bytes.begin() + at, bytes.begin() + at + size);
            at += (size + 3) & ~size_t(3);
        }
        out.push_back(std::move(p));
    }
    return out;
}
std::string contexts_text() {
    std::vector<uint8_t> bytes;
    for (const auto& [id, value] : state().contexts)
        for (uint32_t word : {id, value})
            for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(word >> (24 - 8 * i)));
    return hex(bytes.data(), bytes.size());
}

// A search result line (scripts/live_server.py): session key host-ip host-mac
// host-machine flags public private filled-public filled-private props contexts.
struct Found {
    std::array<uint8_t, 8> id{};
    std::array<uint8_t, 16> key{};
    uint32_t address = 0;
    std::array<uint8_t, 6> mac{};
    uint64_t machine = 0;
    uint32_t flags = 0, public_slots = 0, private_slots = 0, filled_public = 0, filled_private = 0;
    std::vector<uint8_t> properties, contexts;
};
std::optional<Found> parse_found(const std::string& line) {
    const auto w = words(line);
    if (w.size() < 12) return std::nullopt;
    Found found;
    const auto id = parse_bytes(w[0]), key = parse_bytes(w[1]), mac = parse_bytes(w[3]);
    if (id.size() != 8 || key.size() != 16 || mac.size() != 6) return std::nullopt;
    std::copy(id.begin(), id.end(), found.id.begin());
    std::copy(key.begin(), key.end(), found.key.begin());
    std::copy(mac.begin(), mac.end(), found.mac.begin());
    found.address = uint32_t(parse_hex(w[2]));
    found.machine = parse_hex(w[4]);
    found.flags = uint32_t(parse_hex(w[5]));
    found.public_slots = uint32_t(std::stoul(w[6]));
    found.private_slots = uint32_t(std::stoul(w[7]));
    found.filled_public = uint32_t(std::stoul(w[8]));
    found.filled_private = uint32_t(std::stoul(w[9]));
    found.properties = parse_bytes(w[10]);
    found.contexts = parse_bytes(w[11]);
    state().peers[found.address] = Peer{found.mac, found.machine};
    return found;
}

// XSESSION_SEARCHRESULT_HEADER, the results (0x5C each), then each result's
// properties and contexts.
uint32_t write_results(GuestMemory& memory, uint32_t at, uint32_t size, const std::vector<Found>& found) {
    uint32_t needed = 8 + uint32_t(found.size()) * 0x5C;
    for (const auto& f : found) needed += uint32_t(f.properties.size() + f.contexts.size()) + 8;
    if (!at || size < needed) return insufficient;
    memory.check_write(at, needed);
    memory.store<uint32_t>(at, uint32_t(found.size()));
    memory.store<uint32_t>(at + 4, found.empty() ? 0 : at + 8);
    uint32_t data = at + 8 + uint32_t(found.size()) * 0x5C;
    for (size_t i = 0; i < found.size(); ++i) {
        const auto& f = found[i];
        const uint32_t result = at + 8 + uint32_t(i) * 0x5C;
        write_bytes(memory, result, f.id.data(), 8);
        write_xnaddr(memory, result + 8, f.address, f.mac, f.machine);
        write_bytes(memory, result + 0x2C, f.key.data(), 16);
        memory.store<uint32_t>(result + 0x3C, f.public_slots - (std::min)(f.filled_public, f.public_slots));
        memory.store<uint32_t>(result + 0x40, f.private_slots - (std::min)(f.filled_private, f.private_slots));
        memory.store<uint32_t>(result + 0x44, f.filled_public);
        memory.store<uint32_t>(result + 0x48, f.filled_private);
        const auto properties = split_properties(f.properties);
        memory.store<uint32_t>(result + 0x4C, uint32_t(properties.size()));
        memory.store<uint32_t>(result + 0x50, uint32_t(f.contexts.size() / 8));
        data = (data + 7) & ~7u;  // XUSER_PROPERTY holds 8-byte values
        memory.store<uint32_t>(result + 0x54, properties.empty() ? 0 : data);
        uint32_t payload = data + uint32_t(properties.size()) * 0x18;
        for (const auto& p : properties) {
            write_bytes(memory, data, p.entry.data(), 0x18);
            if (!p.payload.empty()) {
                write_bytes(memory, payload, p.payload.data(), p.payload.size());
                memory.store<uint32_t>(data + 20, payload);
                payload += (uint32_t(p.payload.size()) + 3) & ~3u;
            }
            data += 0x18;
        }
        data = payload;
        memory.store<uint32_t>(result + 0x58, f.contexts.empty() ? 0 : data);
        if (!f.contexts.empty()) write_bytes(memory, data, f.contexts.data(), f.contexts.size());
        data += uint32_t(f.contexts.size());
    }
    return ok;
}

uint32_t search(GuestMemory& memory, uint32_t buffer, bool extended) {
    const uint32_t wanted = memory.load<uint32_t>(buffer + 8);
    const uint32_t size = memory.load<uint32_t>(buffer + 0x18), results = memory.load<uint32_t>(buffer + 0x1C);
    const auto answer = ask("SEARCH " + std::to_string(wanted ? wanted : 1));
    std::vector<Found> found;
    for (size_t i = 1; i < answer.size(); ++i)
        if (auto f = parse_found(answer[i])) found.push_back(std::move(*f));
    const uint32_t result = write_results(memory, results, size, found);
    std::cerr << "LIVE_SESSION search" << (extended ? "_ex" : "") << " found=" << found.size()
              << " result=0x" << std::hex << result << std::dec << '\n';
    return result;
}

uint32_t create(GuestMemory& memory, uint32_t buffer) {
    Session* session = session_for_object(memory.load<uint32_t>(buffer), memory);
    if (!session) return 0xC0000008;
    session->flags = memory.load<uint32_t>(buffer + 4);
    session->public_slots = memory.load<uint32_t>(buffer + 8);
    session->private_slots = memory.load<uint32_t>(buffer + 0xC);
    const uint32_t info = memory.load<uint32_t>(buffer + 0x14), nonce = memory.load<uint32_t>(buffer + 0x18);
    session->host = session->flags & session_host;
    session->created = true;
    auto* client = LiveClient::instance();
    if (session->host) {
        char line[96];
        std::snprintf(line, sizeof(line), "CREATE %x %u %u ", session->flags, session->public_slots, session->private_slots);
        const auto answer = ask(line + properties_text() + ' ' + contexts_text());
        const auto w = answer.empty() ? std::vector<std::string>{} : words(answer[0]);
        if (w.size() < 4 || w[0] != "OK") return failed;
        const auto id = parse_bytes(w[1]), key = parse_bytes(w[2]), number = parse_bytes(w[3]);
        std::copy(id.begin(), id.end(), session->id.begin());
        std::copy(key.begin(), key.end(), session->key.begin());
        session->nonce = 0;
        for (uint8_t b : number) session->nonce = session->nonce << 8 | b;
        session->host_address = client->address();
        session->host_mac = client->mac();
        session->host_machine = client->machine();
        if (info) {
            memory.check_write(info, 0x3C);
            write_bytes(memory, info, session->id.data(), 8);
            write_own_xnaddr(memory, info + 8);
            write_bytes(memory, info + 0x2C, session->key.data(), 16);
        }
    } else if (info && memory.readable(info, 0x3C)) {
        // Joining: the title copied the search result's XSESSION_INFO here.
        for (uint32_t i = 0; i < 8; ++i) session->id[i] = memory.load<uint8_t>(info + i);
        for (uint32_t i = 0; i < 16; ++i) session->key[i] = memory.load<uint8_t>(info + 0x2C + i);
        session->host_address = memory.load<uint32_t>(info + 8 + 4);
        for (uint32_t i = 0; i < 6; ++i) session->host_mac[i] = memory.load<uint8_t>(info + 8 + 10 + i);
        session->host_machine = memory.load<uint64_t>(info + 8 + 0x18);
        session->nonce = state().random();
    }
    if (nonce) {
        memory.check_write(nonce, 8);
        memory.store<uint64_t>(nonce, session->nonce);
    }
    std::cerr << "LIVE_SESSION create handle=0x" << std::hex << session->handle << " flags=0x" << session->flags
              << std::dec << " host=" << session->host << " public=" << session->public_slots
              << " private=" << session->private_slots << " id=" << session->id_text() << '\n';
    return ok;
}

// XSessionJoin / XSessionLeave (XGI_SESSION_MANAGE): obj, count, xuids or
// user indices, private-slot flags.
uint32_t manage(GuestMemory& memory, uint32_t buffer, bool joining) {
    Session* session = session_for_object(memory.load<uint32_t>(buffer), memory);
    if (!session) return 0xC0000008;
    const uint32_t count = memory.load<uint32_t>(buffer + 4), xuids = memory.load<uint32_t>(buffer + 8);
    const uint32_t privates = memory.load<uint32_t>(buffer + 0x10);
    std::string line = std::string(joining ? "JOIN " : "LEAVE ") + session->id_text();
    for (uint32_t i = 0; i < count && i < 16; ++i) {
        const uint64_t xuid = xuids ? memory.load<uint64_t>(xuids + 8 * i) : LiveClient::instance()->xuid();
        char word[40];
        const bool is_private = privates && memory.load<uint32_t>(privates + 4 * i);
        std::snprintf(word, sizeof(word), joining ? " %016llx:%d" : " %016llx", static_cast<unsigned long long>(xuid),
                      int(is_private));
        line += word;
    }
    ask(line);
    std::cerr << "LIVE_SESSION " << (joining ? "join" : "leave") << " id=" << session->id_text() << " count=" << count
              << " remote=" << (xuids != 0) << '\n';
    return ok;
}

// XSESSION_LOCAL_DETAILS (0x80) and the members after it.
uint32_t details(GuestMemory& memory, uint32_t buffer) {
    Session* session = session_for_object(memory.load<uint32_t>(buffer), memory);
    if (!session) return 0xC0000008;
    const uint32_t size = memory.load<uint32_t>(buffer + 4), at = memory.load<uint32_t>(buffer + 8);
    const auto answer = ask("SESSION " + session->id_text());
    std::vector<std::pair<uint64_t, bool>> members;
    for (size_t i = 2; i < answer.size(); ++i) {
        const auto w = words(answer[i]);
        if (w.size() >= 2) members.emplace_back(parse_hex(w[0]), w[1] == "1");
    }
    const auto found = answer.size() > 1 ? parse_found(answer[1]) : std::nullopt;
    const uint32_t needed = 0x80 + uint32_t(members.size()) * 0x10;
    if (!at || size < 0x80) return insufficient;
    memory.check_write(at, 0x80);
    for (uint32_t i = 0; i < 0x80; i += 4) memory.store<uint32_t>(at + i, 0);
    uint32_t game_type = 0, game_mode = 0;
    if (found)
        for (size_t i = 0; i + 8 <= found->contexts.size(); i += 8) {
            const uint32_t id = uint32_t(found->contexts[i]) << 24 | found->contexts[i + 1] << 16 | found->contexts[i + 2] << 8 | found->contexts[i + 3];
            const uint32_t value = uint32_t(found->contexts[i + 4]) << 24 | found->contexts[i + 5] << 16 | found->contexts[i + 6] << 8 | found->contexts[i + 7];
            if (id == 0x0000800A) game_type = value;
            if (id == 0x0000800B) game_mode = value;
        }
    uint32_t filled_public = 0, filled_private = 0;
    for (const auto& [xuid, is_private] : members) (is_private ? filled_private : filled_public)++;
    memory.store<uint32_t>(at + 0x00, 0);
    memory.store<uint32_t>(at + 0x04, game_type);
    memory.store<uint32_t>(at + 0x08, game_mode);
    memory.store<uint32_t>(at + 0x0C, session->flags);
    memory.store<uint32_t>(at + 0x10, session->public_slots);
    memory.store<uint32_t>(at + 0x14, session->private_slots);
    memory.store<uint32_t>(at + 0x18, session->public_slots - (std::min)(filled_public, session->public_slots));
    memory.store<uint32_t>(at + 0x1C, session->private_slots - (std::min)(filled_private, session->private_slots));
    memory.store<uint32_t>(at + 0x20, uint32_t(members.size()));
    const uint32_t returned = size >= needed ? uint32_t(members.size()) : (size - 0x80) / 0x10;
    memory.store<uint32_t>(at + 0x24, returned);
    memory.store<uint32_t>(at + 0x28, 0);  // lobby
    memory.store<uint64_t>(at + 0x30, session->nonce);
    write_bytes(memory, at + 0x38, session->id.data(), 8);
    write_xnaddr(memory, at + 0x40, session->host_address, session->host_mac, session->host_machine);
    write_bytes(memory, at + 0x64, session->key.data(), 16);
    memory.store<uint32_t>(at + 0x7C, returned ? at + 0x80 : 0);
    for (uint32_t i = 0; i < returned; ++i) {
        const uint32_t member = at + 0x80 + i * 0x10;
        memory.check_write(member, 0x10);
        memory.store<uint64_t>(member, members[i].first);
        memory.store<uint32_t>(member + 8, members[i].first == LiveClient::instance()->xuid() ? 0 : 0xFE);
        memory.store<uint32_t>(member + 12, members[i].second ? 1 : 0);
    }
    return size >= needed ? ok : insufficient;
}

uint32_t set_state(GuestMemory& memory, uint32_t buffer, const char* name) {
    Session* session = session_for_object(memory.load<uint32_t>(buffer), memory);
    if (!session) return 0xC0000008;
    if (session->host) ask("STATE " + session->id_text() + ' ' + name);
    std::cerr << "LIVE_SESSION " << name << " id=" << session->id_text() << '\n';
    return ok;
}

uint32_t stack_word(GuestMemory& memory, const LiveCall& call, int index) {
    return memory.load<uint32_t>(call.sp + 0x54 + 8 * uint32_t(index - 8));
}

// A guest sockaddr_in: family, port, address (big-endian).
void write_sockaddr(GuestMemory& memory, uint32_t at, uint32_t address, uint16_t port) {
    memory.check_write(at, 16);
    memory.store<uint16_t>(at, 2);
    memory.store<uint16_t>(at + 2, port);
    memory.store<uint32_t>(at + 4, address);
    memory.store<uint64_t>(at + 8, 0);
}
}

bool live_enabled() { return LiveClient::instance() != nullptr; }

bool live_owns_handle(uint32_t handle) {
    std::lock_guard lock(state().mutex);
    return state().sessions.count(handle) != 0;
}

bool live_owns_object(uint32_t object) {
    std::lock_guard lock(state().mutex);
    for (const auto& [handle, session] : state().sessions)
        if (session.object == object) return true;
    return false;
}

bool live_import(std::string_view name, LiveCall& call, GuestMemory& memory, const std::function<void(uint32_t)>& set_event) {
    if (!live_enabled()) return false;
    auto* client = LiveClient::instance();
    std::unique_lock lock(state().mutex);
    auto& s = state();
    const auto* r = call.r;
    const std::string_view n = name.substr(0, 7) == "__imp__" ? name.substr(7) : name;

    if (n == "XamUserCheckPrivilege") {
        if (memory.readable(r[5], 4)) memory.store<uint32_t>(r[5], 1);
        call.result = profile_for(r[3]) ? ok : 0x525;
        return true;
    }
    if (n == "XamUserGetXUID") {
        if (!profile_for(r[3])) {
            call.result = 0x525;
            return true;
        }
        memory.check_write(r[5], 8);
        memory.store<uint64_t>(r[5], client->xuid());
        call.result = ok;
        return true;
    }
    if (n == "XamUserAreUsersFriends") {
        // (user, xuids, count, BOOL* result, overlapped): no friends list.
        if (r[6] && memory.readable(r[6], 4)) memory.store<uint32_t>(r[6], 0);
        call.result = ok;
        return true;
    }
    if (n == "XeCryptRandom") {
        // (buffer, length): session keys and nonces the network library makes.
        for (uint32_t i = 0; i < r[4]; ++i) memory.store<uint8_t>(r[3] + i, uint8_t(s.random()));
        call.result = 0;
        return true;
    }
    if (n == "XeKeysGetConsoleID") {
        // (5 raw bytes*, 12-character hex text*): from the machine id.
        const uint64_t id = client->machine() & 0xFFFFFFFFFFull;
        if (r[3]) for (int i = 0; i < 5; ++i) memory.store<uint8_t>(r[3] + i, uint8_t(id >> (32 - 8 * i)));
        if (r[4]) {
            char text[13];
            std::snprintf(text, sizeof(text), "%010llX", static_cast<unsigned long long>(id));
            for (int i = 0; i < 12; ++i) memory.store<uint8_t>(r[4] + i, uint8_t(i < 10 ? text[i] : 0));
        }
        call.result = 0;
        return true;
    }
    // Voice chat: no headset and no voice (as Xenia reports it).
    if (n == "XamVoiceCreate") {
        if (r[5] && memory.readable(r[5], 4)) memory.store<uint32_t>(r[5], 0);
        call.result = 5;  // ERROR_ACCESS_DENIED
        return true;
    }
    if (n == "XamVoiceHeadsetPresent" || n == "XamVoiceClose" || n == "XamVoiceSubmitPacket") {
        call.result = 0;
        return true;
    }
    if (n == "XamUserGetCachedUserFlags") {
        call.result = 0;
        return true;
    }
    if (n == "XamUserGetIndexFromXUID") {
        // (xuid, flags, index*)
        const uint64_t xuid = call.r64[3];
        const bool ours = profile_for(0) && (xuid == client->xuid() || xuid == profile_for(0)->xuid);
        if (r[5] && memory.readable(r[5], 4)) memory.store<uint32_t>(r[5], ours ? 0 : 0xFF);
        call.result = ours ? ok : 0x525;
        return true;
    }
    if (n == "XamShowNuiGamerCardUIForXUID" || n == "XamShowNuiPartyUI" || n == "XamShowNuiFriendsUI") {
        call.result = failed;  // no system UI for these
        return true;
    }
    if (n == "XamUserGetDeviceContext") {
        // (user, -, context*): none, as Xenia reports it for a signed-in user.
        if (r[5] && memory.readable(r[5], 4)) memory.store<uint32_t>(r[5], 0);
        call.result = profile_for(r[3] & 3) || (r[3] & 0xFF) == 0xFF ? ok : 0x80070057;
        return true;
    }
    if (n == "XamUserCreateStatsEnumerator") {
        call.result = failed;  // leaderboards: not yet
        return true;
    }
    if (n == "XamUserReadProfileSettings") {
        // (title, user, xuid count, xuids, setting count, ids, size*, buffer,
        // overlapped). The buffer: {count, settings*} and then the settings,
        // X_USER_PROFILE_SETTING (0x28): source at +0 (1: default), user or
        // XUID at +8, id at +16, X_USER_DATA at +24. Every setting asked for
        // is the default (zero) for every user asked about: the host reads
        // a joining player's before it accepts them.
        const uint32_t user = r[4], xuid_count = r[5], xuids = r[6], count = r[7], ids = r[8];
        const uint32_t size_at = r[9], buffer = r[10];
        const uint32_t overlapped = stack_word(memory, call, 8);
        const uint32_t users = xuid_count ? xuid_count : 1;
        // A string or binary setting (types 4 and 6) carries its bytes after
        // the settings: as many as its id's size field says, zero.
        uint32_t payload = 0;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t id = memory.load<uint32_t>(ids + 4 * k);
            if ((id >> 28) == 4 || (id >> 28) == 6) payload += ((id >> 16) & 0xFFF) * users;
        }
        const uint32_t needed = 8 + count * 0x28 * users + payload;
        if (size_at && (!buffer || memory.load<uint32_t>(size_at) < needed)) {
            memory.store<uint32_t>(size_at, needed);
            call.result = insufficient;
            return true;
        }
        uint32_t result = ok;
        if (!xuid_count && !profile_for(user & 3)) {
            result = failed;
        } else {
            memory.check_write(buffer, needed);
            memory.store<uint32_t>(buffer, count * users);
            memory.store<uint32_t>(buffer + 4, buffer + 8);
            uint32_t bytes = buffer + 8 + count * 0x28 * users;
            for (uint32_t u = 0; u < users; ++u)
                for (uint32_t k = 0; k < count; ++k) {
                    const uint32_t at = buffer + 8 + (u * count + k) * 0x28;
                    const uint32_t id = memory.load<uint32_t>(ids + 4 * k);
                    for (uint32_t w = 0; w < 0x28; w += 4) memory.store<uint32_t>(at + w, 0);
                    memory.store<uint32_t>(at, 1);
                    if (xuid_count) memory.store<uint64_t>(at + 8, memory.load<uint64_t>(xuids + 8 * u));
                    else memory.store<uint32_t>(at + 8, user);
                    memory.store<uint32_t>(at + 16, id);
                    memory.store<uint8_t>(at + 24, uint8_t(id >> 28));
                    if ((id >> 28) == 4 || (id >> 28) == 6) {
                        const uint32_t size = (id >> 16) & 0xFFF;
                        for (uint32_t b = 0; b < size; ++b) memory.store<uint8_t>(bytes + b, 0);
                        memory.store<uint32_t>(at + 32, size);
                        memory.store<uint32_t>(at + 36, bytes);
                        bytes += size;
                    }
                }
        }
        std::cerr << "LIVE_PROFILE_SETTINGS title=0x" << std::hex << r[3] << " user=0x" << user << " first=0x"
                  << (count ? memory.load<uint32_t>(ids) : 0) << std::dec << " xuids=" << xuid_count
                  << " settings=" << count << " result=0x" << std::hex << result << std::dec << '\n';
        if (overlapped) {
            memory.store<uint32_t>(overlapped, result);
            memory.store<uint32_t>(overlapped + 4, 0);
            if (const uint32_t event = memory.load<uint32_t>(overlapped + 0xC)) set_event(event);
            call.result = pending;
        } else {
            call.result = result;
        }
        return true;
    }
    if (n == "XMsgCancelIORequest") {
        call.result = ok;
        return true;
    }
    if (n == "XamSessionCreateHandle") {
        Session session;
        session.handle = s.next_handle++;
        s.sessions[session.handle] = session;
        memory.check_write(r[3], 4);
        memory.store<uint32_t>(r[3], session.handle);
        call.result = ok;
        return true;
    }
    if (n == "XamSessionRefObjByHandle") {
        const auto found = s.sessions.find(r[3]);
        if (found == s.sessions.end()) {
            call.result = 0xC0000008;  // STATUS_INVALID_HANDLE
            return true;
        }
        if (!found->second.object) {
            found->second.object = allocate(16);
            if (!found->second.object) {
                call.result = 0xC0000017;
                return true;
            }
            memory.store<uint32_t>(found->second.object, found->second.handle);
        }
        memory.check_write(r[4], 4);
        memory.store<uint32_t>(r[4], found->second.object);
        call.result = ok;
        return true;
    }

    // XNet (every NetDll call has the caller type first).
    if (n == "NetDll_WSAStartup") {
        if (r[5] && memory.readable(r[5], 4)) {
            memory.store<uint16_t>(r[5], 0x0202);
            memory.store<uint16_t>(r[5] + 2, 0x0202);
        }
        call.result = ok;
        return true;
    }
    if (n == "NetDll_XNetGetTitleXnAddr") {
        write_own_xnaddr(memory, r[4]);
        call.result = 0x02 | 0x04 | 0x20 | 0x40 | 0x80;  // ethernet, static, gateway, DNS, online
        return true;
    }
    if (n == "NetDll_XNetXnAddrToInAddr") {
        const uint32_t address = memory.load<uint32_t>(r[4] + 4);
        Peer peer;
        for (uint32_t i = 0; i < 6; ++i) peer.mac[i] = memory.load<uint8_t>(r[4] + 10 + i);
        peer.machine = memory.load<uint64_t>(r[4] + 0x18);
        if (address) s.peers[address] = peer;
        memory.check_write(r[6], 4);
        memory.store<uint32_t>(r[6], address);
        call.result = address ? ok : 0x2726;  // WSAEINVAL
        return true;
    }
    if (n == "NetDll_XNetInAddrToXnAddr") {
        uint32_t address = r[4];
        if (address == 0x7F000001 || address == 0) address = client->address();
        Peer peer{client->mac(), client->machine()};
        if (address != client->address()) {
            auto found = s.peers.find(address);
            if (found == s.peers.end()) {
                char line[32];
                std::snprintf(line, sizeof(line), "PEER %08x", address);
                const auto answer = ask(line);
                const auto w = answer.empty() ? std::vector<std::string>{} : words(answer[0]);
                if (w.size() >= 3 && w[0] == "OK") {
                    Peer made;
                    const auto mac = parse_bytes(w[1]);
                    if (mac.size() == 6) std::copy(mac.begin(), mac.end(), made.mac.begin());
                    made.machine = parse_hex(w[2]);
                    found = s.peers.emplace(address, made).first;
                }
            }
            if (found != s.peers.end()) peer = found->second;
        }
        if (r[5]) write_xnaddr(memory, r[5], address, peer.mac, peer.machine);
        if (r[6] && memory.readable(r[6], 8)) {
            memory.store<uint64_t>(r[6], 0);
            for (const auto& [handle, session] : s.sessions)
                if (session.created && session.host_address == address) write_bytes(memory, r[6], session.id.data(), 8);
        }
        call.result = ok;
        return true;
    }
    if (n == "NetDll_XNetXnAddrToMachineId") {
        memory.check_write(r[5], 8);
        memory.store<uint64_t>(r[5], memory.load<uint64_t>(r[4] + 0x18));
        call.result = ok;
        return true;
    }
    if (n == "NetDll_XNetQosListen") {
        // (caller, XNKID*, data, size, bits per second, flags): the data a
        // QoS probe of this session gets back.
        const uint32_t data = r[5], size = r[6], flags = r[8];
        if ((flags & 0x04) && r[4]) {
            std::array<uint8_t, 8> id{};
            for (uint32_t i = 0; i < 8; ++i) id[i] = memory.load<uint8_t>(r[4] + i);
            std::vector<uint8_t> bytes(size);
            for (uint32_t i = 0; i < size; ++i) bytes[i] = memory.load<uint8_t>(data + i);
            ask("QOSSET " + hex(id.data(), 8) + ' ' + hex(bytes.data(), bytes.size()));
        }
        std::cerr << "LIVE_QOS listen flags=0x" << std::hex << flags << std::dec << " bytes=" << size << '\n';
        call.result = ok;
        return true;
    }
    if (n == "NetDll_XNetQosLookup") {
        // (caller, count, XNADDR*[], XNKID*[], XNKEY*[], gateways, ..., then
        // on the stack probes, bits per second, flags, event, XNQOS**).
        const uint32_t count = r[4], ids = r[6];
        const uint32_t event = stack_word(memory, call, 11), output = stack_word(memory, call, 12);
        // Every probe the title asked for comes back: it rates the connection
        // from them (1 red bar when they do not), as well as from the times.
        const uint32_t probes = (std::max)(stack_word(memory, call, 8), 1u);
        const uint32_t block = allocate(8 + 0x18 * (count ? count : 1));
        if (!block || !output) {
            call.result = failed;
            return true;
        }
        memory.store<uint32_t>(block, count);
        memory.store<uint32_t>(block + 4, 0);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t info = block + 8 + 0x18 * i;
            std::vector<uint8_t> data;
            if (ids) {
                const uint32_t id_at = memory.load<uint32_t>(ids + 4 * i);
                std::array<uint8_t, 8> id{};
                for (uint32_t b = 0; b < 8; ++b) id[b] = memory.load<uint8_t>(id_at + b);
                const auto answer = ask("QOSGET " + hex(id.data(), 8));
                const auto w = answer.empty() ? std::vector<std::string>{} : words(answer[0]);
                if (w.size() >= 2 && w[0] == "OK") data = parse_bytes(w[1]);
            }
            uint32_t data_at = 0;
            if (!data.empty() && (data_at = allocate(uint32_t(data.size()))))
                write_bytes(memory, data_at, data.data(), data.size());
            memory.store<uint8_t>(info, uint8_t(0x01 | 0x02 | (data_at ? 0x08 : 0)));  // complete, contacted, data
            memory.store<uint8_t>(info + 1, 0);
            memory.store<uint16_t>(info + 2, uint16_t(probes));
            memory.store<uint16_t>(info + 4, uint16_t(probes));
            memory.store<uint16_t>(info + 6, uint16_t(data_at ? data.size() : 0));
            memory.store<uint32_t>(info + 8, data_at);
            memory.store<uint16_t>(info + 12, 10);
            memory.store<uint16_t>(info + 14, 12);
            memory.store<uint32_t>(info + 16, 100000000);
            memory.store<uint32_t>(info + 20, 100000000);
        }
        memory.store<uint32_t>(output, block);
        if (event) set_event(event);
        std::cerr << "LIVE_QOS lookup count=" << count << '\n';
        call.result = ok;
        return true;
    }
    if (n == "NetDll_XNetQosRelease") {
        const uint32_t block = r[4];
        if (block && active_heap && memory.readable(block, 8)) {
            const uint32_t count = memory.load<uint32_t>(block);
            for (uint32_t i = 0; i < count && i < 64; ++i)
                if (const uint32_t data = memory.load<uint32_t>(block + 8 + 0x18 * i + 8)) active_heap->free(data);
            active_heap->free(block);
        }
        call.result = ok;
        return true;
    }

    // Sockets: virtual, carried by the service (LiveClient).
    if (n == "NetDll_socket") {
        const uint32_t handle = s.next_socket++;
        s.sockets[handle] = Socket{};
        std::cerr << "LIVE_SOCKET open=0x" << std::hex << handle << std::dec << " type=" << r[5] << " protocol=" << r[6] << '\n';
        call.result = handle;
        return true;
    }
    if (n == "NetDll_bind") {
        const auto found = s.sockets.find(r[4]);
        if (found == s.sockets.end()) {
            call.result = uint32_t(-1);
            return true;
        }
        uint16_t port = memory.load<uint16_t>(r[5] + 2);
        if (!port) port = s.next_port++;
        found->second.port = port;
        client->open_port(port);
        std::cerr << "LIVE_SOCKET bind=0x" << std::hex << r[4] << std::dec << " port=" << port << '\n';
        call.result = ok;
        return true;
    }
    if (n == "NetDll_ioctlsocket") {
        const auto found = s.sockets.find(r[4]);
        if (found == s.sockets.end()) {
            call.result = uint32_t(-1);
            return true;
        }
        if (r[5] == 0x8004667E) found->second.nonblocking = memory.load<uint32_t>(r[6]) != 0;  // FIONBIO
        if (r[5] == 0x4004667F) memory.store<uint32_t>(r[6], client->pending(found->second.port) ? 1500 : 0);  // FIONREAD
        call.result = ok;
        return true;
    }
    if (n == "NetDll_closesocket") {
        const auto found = s.sockets.find(r[4]);
        if (found != s.sockets.end()) {
            if (found->second.port) client->close_port(found->second.port);
            s.sockets.erase(found);
        }
        call.result = ok;
        return true;
    }
    if (n == "NetDll_sendto") {
        // (caller, socket, buffer, length, flags, to, to length)
        const auto found = s.sockets.find(r[4]);
        if (found == s.sockets.end() || !r[8]) {
            call.result = uint32_t(-1);
            return true;
        }
        if (!found->second.port) {
            found->second.port = s.next_port++;
            client->open_port(found->second.port);
        }
        std::vector<uint8_t> payload(r[6]);
        for (uint32_t i = 0; i < r[6]; ++i) payload[i] = memory.load<uint8_t>(r[5] + i);
        const uint16_t to_port = memory.load<uint16_t>(r[8] + 2);
        const uint32_t to = memory.load<uint32_t>(r[8] + 4);
        client->send(to, to_port, found->second.port, payload.data(), payload.size());
        static uint32_t sent = 0;
        if (++sent <= 16 || (sent & (sent - 1)) == 0)
            std::cerr << "LIVE_SOCKET sendto=0x" << std::hex << to << std::dec << ':' << to_port << " bytes=" << r[6]
                      << " count=" << sent << '\n';
        call.result = r[6];
        return true;
    }
    if (n == "NetDll_recvfrom") {
        // (caller, socket, buffer, length, flags, from, from length*)
        const auto found = s.sockets.find(r[4]);
        if (found == s.sockets.end() || !found->second.port) {
            call.result = uint32_t(-1);
            return true;
        }
        const uint16_t port = found->second.port;
        const bool blocking = !found->second.nonblocking;
        // A blocking socket waits without the lock, so the title's other
        // threads keep sending meanwhile.
        lock.unlock();
        auto datagram = client->receive(port, blocking ? 50 : 0);
        lock.lock();
        if (!datagram) {
            call.result = uint32_t(-1);  // WSAEWOULDBLOCK
            return true;
        }
        const uint32_t copied = (std::min)(uint32_t(datagram->payload.size()), r[6]);
        if (copied) write_bytes(memory, r[5], datagram->payload.data(), copied);
        if (r[8]) write_sockaddr(memory, r[8], datagram->from, datagram->from_port);
        if (r[9] && memory.readable(r[9], 4)) memory.store<uint32_t>(r[9], 16);
        static uint32_t received = 0;
        if (++received <= 16 || (received & (received - 1)) == 0)
            std::cerr << "LIVE_SOCKET recvfrom=0x" << std::hex << datagram->from << std::dec << ':' << datagram->from_port
                      << " bytes=" << copied << " count=" << received << '\n';
        call.result = copied;
        return true;
    }
    return false;
}

std::optional<uint32_t> live_in_process(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length,
                                        GuestMemory& memory) {
    if (!live_enabled() || app != 0xFC) return std::nullopt;
    switch (message) {
    case 0x00058003:  // XLiveBaseLogonGetHR: logged on
        return ok;
    case 0x00058004:  // XOnlineGetLogonID
        if (buffer) memory.store<uint32_t>(buffer, 1);
        return ok;
    case 0x00058006:  // XOnlineGetNatType: open (the service relays everything)
        if (buffer) memory.store<uint32_t>(buffer, 1);
        return ok;
    case 0x0005800E:  // XUserMuteListQuery({user, -, remote xuid}, BOOL* muted): nobody is muted
        if (length && memory.readable(length, 4)) memory.store<uint32_t>(length, 0);
        return ok;
    default:
        std::cerr << "LIVE_IN_PROCESS message=0x" << std::hex << message << std::dec << " result=refused\n";
        return failed;
    }
}

std::optional<uint32_t> live_message(uint32_t message, uint32_t buffer, uint32_t length, GuestMemory& memory) {
    if (!live_enabled()) return std::nullopt;
    std::lock_guard lock(state().mutex);
    auto& s = state();
    switch (message) {
    case 0x000B0006:  // XUserSetContext: user, -, xuid, {id, value}
        s.contexts[memory.load<uint32_t>(buffer + 0x10)] = memory.load<uint32_t>(buffer + 0x14);
        return ok;
    case 0x000B0007: {  // XUserSetProperty: user, -, xuid, id, size, data
        const uint32_t id = memory.load<uint32_t>(buffer + 0x10), size = memory.load<uint32_t>(buffer + 0x14);
        const uint32_t data = memory.load<uint32_t>(buffer + 0x18);
        const uint32_t type = id >> 28;
        // XUSER_PROPERTY: id, then XUSER_DATA: type at +8, value at +16
        // (a string's or binary's size there, its bytes kept after the entry).
        if (data && type >= 1 && type <= 7 && (has_payload(id) ? size <= 0x1000 : size <= 8)) {
            std::vector<uint8_t> property(0x18, 0);
            for (int i = 0; i < 4; ++i) property[i] = uint8_t(id >> (24 - 8 * i));
            property[8] = uint8_t(type);
            if (has_payload(id)) {
                for (int i = 0; i < 4; ++i) property[16 + i] = uint8_t(size >> (24 - 8 * i));
                for (uint32_t i = 0; i < size; ++i) property.push_back(memory.load<uint8_t>(data + i));
            } else {
                for (uint32_t i = 0; i < size; ++i) property[16 + i] = memory.load<uint8_t>(data + i);
            }
            s.properties[id] = property;
        }
        return ok;
    }
    case 0x000B0010: return create(memory, buffer);
    case 0x000B0011: {  // XSessionDelete
        Session* session = session_for_object(memory.load<uint32_t>(buffer), memory);
        if (session && session->host) ask("DELETE " + session->id_text());
        if (session) std::cerr << "LIVE_SESSION delete id=" << session->id_text() << '\n';
        return ok;
    }
    case 0x000B0012: return manage(memory, buffer, true);
    case 0x000B0013: return manage(memory, buffer, false);
    case 0x000B0014: return set_state(memory, buffer, "ingame");
    case 0x000B0015: return set_state(memory, buffer, "lobby");
    case 0x000B0016: return search(memory, buffer, false);
    case 0x000B001C: return search(memory, buffer, true);
    case 0x000B0018: {  // XSessionModify: obj, flags, public, private
        Session* session = session_for_object(memory.load<uint32_t>(buffer), memory);
        if (!session) return 0xC0000008;
        session->flags = memory.load<uint32_t>(buffer + 4);
        session->public_slots = memory.load<uint32_t>(buffer + 8);
        session->private_slots = memory.load<uint32_t>(buffer + 12);
        if (session->host) {
            char line[96];
            std::snprintf(line, sizeof(line), " %x %u %u", session->flags, session->public_slots, session->private_slots);
            ask("MODIFY " + session->id_text() + line);
        }
        return ok;
    }
    case 0x000B001D: return details(memory, buffer);
    case 0x000B0019:  // XSessionGetInvitationData: no invitations
    case 0x000B001F:  // XSessionModifySkill
    case 0x000B0020:  // XUserResetStatsView
    case 0x000B0025:  // XSessionWriteStats
    case 0x000B0026:  // XSessionFlushStats
        return ok;
    case 0x000B001A:  // XSessionArbitrationRegister: ranked matches, not yet
    case 0x000B0021:  // XUserReadStats: leaderboards, not yet
        return failed;
    default:
        (void)length;
        return std::nullopt;
    }
}
}
