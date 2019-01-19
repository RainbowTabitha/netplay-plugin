#include "stdafx.h"

#include "server.h"
#include "room.h"
#include "user.h"
#include "tcp_connection.h"
#include "version.h"

using namespace std;
using namespace asio;

server::server(shared_ptr<io_service> io_s, bool multiroom) : io_s(io_s), multiroom(multiroom), acceptor(*io_s), timer(*io_s) { }

uint16_t server::open(uint16_t port) {
    error_code error;

    auto ipv = ip::tcp::v6();
    acceptor.open(ipv, error);
    if (error) { // IPv6 not available
        ipv = ip::tcp::v4();
        acceptor.open(ipv, error);
        if (error) throw error;
    }

    acceptor.bind(ip::tcp::endpoint(ipv, port));
    acceptor.listen();
    accept();

    on_tick();

    log("Listening on port " + to_string(acceptor.local_endpoint().port()) + "...");

    return acceptor.local_endpoint().port();
}

void server::close() {
    error_code error;

    if (acceptor.is_open()) {
        acceptor.close(error);
    }

    timer.cancel(error);

    unordered_map<string, shared_ptr<room>> r;
    r.swap(rooms);
    for (auto& e : r) {
        e.second->close();
    }
}

void server::accept() {
    auto conn = make_shared<tcp_connection>(io_s);
    acceptor.async_accept(conn->get_socket(), [=](error_code error) {
        if (error) return;

        conn->get_socket().set_option(ip::tcp::no_delay(true), error);
        if (error) return;

        auto u = make_shared<user>(conn, io_s, shared_from_this());
        u->send_protocol_version();
        u->process_packet();

        accept();
    });
}

void server::on_user_join(std::shared_ptr<user> user, string room_id) {
    if (multiroom) {
        if (room_id == "") room_id = get_random_room_id();
    } else {
        room_id = "";
    }

    if (rooms.find(room_id) == rooms.end()) {
        rooms[room_id] = make_shared<room>(room_id, shared_from_this());
        log("[" + room_id + "] " + user->get_name() + " created room. Room count: " + to_string(rooms.size()));
    }

    rooms[room_id]->on_user_join(user);
}

void server::on_room_close(std::shared_ptr<room> room) {
    auto id = room->get_id();
    auto age = (int)(timestamp() - room->creation_timestamp);
    if (rooms.erase(id)) {
        log("[" + id + "] Room destroyed after " + to_string(age / 60) + "m. Room count: " + to_string(rooms.size()));
    }
}

void server::on_tick() {
    for (auto& e : rooms) {
        e.second->on_tick();
    }

    timer.expires_from_now(std::chrono::milliseconds(500));
    timer.async_wait([=](const error_code& error) { if (!error) on_tick(); });
}

string server::get_random_room_id() {
    static const string ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static uniform_int_distribution<size_t> dist(0, ALPHABET.length() - 1);
    static random_device rd;

    string result;
    result.resize(5);
    do {
        for (char& c : result) {
            c = ALPHABET[dist(rd)];
        }
    } while (rooms.find(result) != rooms.end());

    return result;
}

#ifdef __GNUC__
void handle(int sig) {
    print_stack_trace();
    exit(1);
}
#endif

int main(int argc, char* argv[]) {
#ifdef __GNUC__
    signal(SIGSEGV, handle);
#endif

    log(APP_NAME_AND_VERSION);

    try {
        uint16_t port = argc >= 2 ? stoi(argv[1]) : 6400;
        auto io_s = make_shared<io_service>();
        auto my_server = make_shared<server>(io_s, true);
        port = my_server->open(port);
        io_s->run();
    } catch (const exception& e) {
        log(cerr, e.what());
        return 1;
    } catch (const error_code& e) {
        log(cerr, e.message());
        return 1;
    }

    return 0;
}
