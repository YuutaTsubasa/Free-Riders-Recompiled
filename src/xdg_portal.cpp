#include "xdg_portal.h"
#include <cstdint>
#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <type_traits>
#include <chrono>
#include <cstring>
#endif

namespace sfr::xdg_portal {

std::optional<std::string> path_from_uri(std::string_view uri) {
    constexpr std::string_view scheme = "file://";
    if (uri.substr(0, scheme.size()) != scheme) return std::nullopt;
    uri.remove_prefix(scheme.size());
    // file://host/path: only no host or localhost is this machine.
    const size_t slash = uri.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const std::string_view host = uri.substr(0, slash);
    if (!host.empty() && host != "localhost") return std::nullopt;
    uri.remove_prefix(slash);
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string path;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() &&
            hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0) {
            path += char(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
            i += 2;
        } else {
            path += uri[i];
        }
    }
    return path;
}

#if defined(__linux__) && !defined(__ANDROID__)
namespace {
// The parts of libdbus-1's stable C ABI used here, declared rather than
// included so that building needs no D-Bus headers. The library fills
// these structs; they are given more room than its own definitions need.
struct DBusConnection;
struct DBusMessage;
struct alignas(8) DBusError { const char* name; const char* message; unsigned char rest[48]; };
struct alignas(8) DBusMessageIter { unsigned char bytes[128]; };
using dbool = uint32_t;
constexpr int bus_session = 0;
constexpr int type_string = 's', type_boolean = 'b', type_uint32 = 'u', type_array = 'a',
              type_variant = 'v', type_dict_entry = 'e', type_object_path = 'o';

struct Library {
    void* handle = nullptr;
    void (*error_init)(DBusError*);
    void (*error_free)(DBusError*);
    dbool (*error_is_set)(const DBusError*);
    DBusConnection* (*bus_get)(int, DBusError*);
    const char* (*bus_get_unique_name)(DBusConnection*);
    void (*bus_add_match)(DBusConnection*, const char*, DBusError*);
    void (*bus_remove_match)(DBusConnection*, const char*, DBusError*);
    DBusMessage* (*message_new_method_call)(const char*, const char*, const char*, const char*);
    void (*message_unref)(DBusMessage*);
    dbool (*message_is_signal)(DBusMessage*, const char*, const char*);
    const char* (*message_get_path)(DBusMessage*);
    void (*iter_init_append)(DBusMessage*, DBusMessageIter*);
    dbool (*iter_append_basic)(DBusMessageIter*, int, const void*);
    dbool (*iter_open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*);
    dbool (*iter_close_container)(DBusMessageIter*, DBusMessageIter*);
    dbool (*iter_init)(DBusMessage*, DBusMessageIter*);
    int (*iter_get_arg_type)(DBusMessageIter*);
    void (*iter_get_basic)(DBusMessageIter*, void*);
    dbool (*iter_next)(DBusMessageIter*);
    void (*iter_recurse)(DBusMessageIter*, DBusMessageIter*);
    DBusMessage* (*send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*);
    dbool (*connection_read_write)(DBusConnection*, int);
    DBusMessage* (*connection_pop_message)(DBusConnection*);

    bool load() {
        if (handle) return true;
        void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        bool ok = true;
        const auto get = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(lib, name));
            ok = ok && function;
        };
        get(error_init, "dbus_error_init");
        get(error_free, "dbus_error_free");
        get(error_is_set, "dbus_error_is_set");
        get(bus_get, "dbus_bus_get");
        get(bus_get_unique_name, "dbus_bus_get_unique_name");
        get(bus_add_match, "dbus_bus_add_match");
        get(bus_remove_match, "dbus_bus_remove_match");
        get(message_new_method_call, "dbus_message_new_method_call");
        get(message_unref, "dbus_message_unref");
        get(message_is_signal, "dbus_message_is_signal");
        get(message_get_path, "dbus_message_get_path");
        get(iter_init_append, "dbus_message_iter_init_append");
        get(iter_append_basic, "dbus_message_iter_append_basic");
        get(iter_open_container, "dbus_message_iter_open_container");
        get(iter_close_container, "dbus_message_iter_close_container");
        get(iter_init, "dbus_message_iter_init");
        get(iter_get_arg_type, "dbus_message_iter_get_arg_type");
        get(iter_get_basic, "dbus_message_iter_get_basic");
        get(iter_next, "dbus_message_iter_next");
        get(iter_recurse, "dbus_message_iter_recurse");
        get(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
        get(connection_read_write, "dbus_connection_read_write");
        get(connection_pop_message, "dbus_connection_pop_message");
        if (!ok) { dlclose(lib); return false; }
        handle = lib;
        return true;
    }
};
Library dbus;

// One a{sv} entry whose value is a string or a boolean.
void append_option(DBusMessageIter& dict, const char* key, const char* text, const dbool* flag) {
    DBusMessageIter entry, value;
    dbus.iter_open_container(&dict, type_dict_entry, nullptr, &entry);
    dbus.iter_append_basic(&entry, type_string, &key);
    dbus.iter_open_container(&entry, type_variant, text ? "s" : "b", &value);
    if (text) dbus.iter_append_basic(&value, type_string, &text);
    else dbus.iter_append_basic(&value, type_boolean, flag);
    dbus.iter_close_container(&entry, &value);
    dbus.iter_close_container(&dict, &entry);
}

// Response(u response, a{sv} results): 0 is chosen; results["uris"] is as.
Result read_response(DBusMessage* message) {
    DBusMessageIter args;
    if (!dbus.iter_init(message, &args) || dbus.iter_get_arg_type(&args) != type_uint32) return {Outcome::cancelled, {}};
    uint32_t response = 1;
    dbus.iter_get_basic(&args, &response);
    if (response != 0) return {Outcome::cancelled, {}};
    if (!dbus.iter_next(&args) || dbus.iter_get_arg_type(&args) != type_array) return {Outcome::cancelled, {}};
    DBusMessageIter dict;
    dbus.iter_recurse(&args, &dict);
    while (dbus.iter_get_arg_type(&dict) == type_dict_entry) {
        DBusMessageIter entry;
        dbus.iter_recurse(&dict, &entry);
        const char* key = nullptr;
        if (dbus.iter_get_arg_type(&entry) == type_string) dbus.iter_get_basic(&entry, &key);
        if (key && std::strcmp(key, "uris") == 0 && dbus.iter_next(&entry) && dbus.iter_get_arg_type(&entry) == type_variant) {
            DBusMessageIter variant, uris;
            dbus.iter_recurse(&entry, &variant);
            if (dbus.iter_get_arg_type(&variant) == type_array) {
                dbus.iter_recurse(&variant, &uris);
                if (dbus.iter_get_arg_type(&uris) == type_string) {
                    const char* uri = nullptr;
                    dbus.iter_get_basic(&uris, &uri);
                    if (auto path = uri ? path_from_uri(uri) : std::nullopt) return {Outcome::chosen, *path};
                }
            }
        }
        dbus.iter_next(&dict);
    }
    return {Outcome::cancelled, {}};
}
}

Result open_file(const std::string& title, bool folders) {
    if (!dbus.load()) return {};
    DBusError error;
    dbus.error_init(&error);
    DBusConnection* bus = dbus.bus_get(bus_session, &error);
    if (!bus || dbus.error_is_set(&error)) { dbus.error_free(&error); return {}; }
    // The request's object path is known before the call (from the
    // connection's unique name and a token), so its Response cannot be missed.
    const char* unique = dbus.bus_get_unique_name(bus);
    if (!unique || unique[0] != ':') return {};
    std::string sender(unique + 1);
    for (char& c : sender) if (c == '.') c = '_';
    const std::string token = "sfr" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::string handle = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    const auto match_for = [](const std::string& path) {
        return "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + path + "'";
    };
    std::string match = match_for(handle);
    dbus.bus_add_match(bus, match.c_str(), &error);
    if (dbus.error_is_set(&error)) { dbus.error_free(&error); return {}; }

    DBusMessage* call = dbus.message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                     "org.freedesktop.portal.FileChooser", "OpenFile");
    if (!call) { dbus.bus_remove_match(bus, match.c_str(), nullptr); return {}; }
    DBusMessageIter args, dict;
    dbus.iter_init_append(call, &args);
    const char* parent = "";
    const char* title_text = title.c_str();
    dbus.iter_append_basic(&args, type_string, &parent);
    dbus.iter_append_basic(&args, type_string, &title_text);
    dbus.iter_open_container(&args, type_array, "{sv}", &dict);
    append_option(dict, "handle_token", token.c_str(), nullptr);
    const dbool yes = 1;
    append_option(dict, "modal", nullptr, &yes);
    if (folders) append_option(dict, "directory", nullptr, &yes);
    dbus.iter_close_container(&args, &dict);
    DBusMessage* reply = dbus.send_with_reply_and_block(bus, call, 10000, &error);
    dbus.message_unref(call);
    if (!reply || dbus.error_is_set(&error)) {
        // No portal on this desktop (or it refused): another way is tried.
        dbus.error_free(&error);
        dbus.bus_remove_match(bus, match.c_str(), nullptr);
        return {};
    }
    // An older portal names its own request path.
    DBusMessageIter returned;
    if (dbus.iter_init(reply, &returned) && dbus.iter_get_arg_type(&returned) == type_object_path) {
        const char* path = nullptr;
        dbus.iter_get_basic(&returned, &path);
        if (path && handle != path) {
            dbus.bus_remove_match(bus, match.c_str(), nullptr);
            handle = path;
            match = match_for(handle);
            dbus.bus_add_match(bus, match.c_str(), nullptr);
        }
    }
    dbus.message_unref(reply);

    // The dialog stays open as long as the player likes.
    Result result{Outcome::unavailable, {}};
    bool waiting = true;
    while (waiting && dbus.connection_read_write(bus, 200)) {
        while (DBusMessage* message = dbus.connection_pop_message(bus)) {
            const char* path = dbus.message_get_path(message);
            if (waiting && dbus.message_is_signal(message, "org.freedesktop.portal.Request", "Response") &&
                path && handle == path) {
                result = read_response(message);
                waiting = false;
            }
            dbus.message_unref(message);
        }
    }
    dbus.bus_remove_match(bus, match.c_str(), nullptr);
    return result;
}
#else
Result open_file(const std::string&, bool) { return {}; }
#endif
}
