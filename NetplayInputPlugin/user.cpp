#include "stdafx.h"

#include "user.h"
#include "common.h"
#include "util.h"

using namespace std;
using namespace asio;

uint32_t user::next_id = 0;

user::user(shared_ptr<connection> conn, shared_ptr<io_service> io_service, shared_ptr<server> server)
    : conn(conn), my_server(server), id(++next_id) { }

void user::set_room(shared_ptr<room> room) {
    this->my_room = room;

    conn->send(pout.reset() << PATH << ("/" + room->get_id()), error_handler());
}

bool user::joined() {
    return !my_room.expired();
}

function<void(const error_code&)> user::error_handler() {
    auto self(shared_from_this());
    return [self](const error_code& error) {
        if (!error) return;
        if (self->joined()) {
            auto address = self->conn->get_address();
            self->my_room.lock()->on_user_quit(self);
            log(self->name + " (" + address + ") disconnected");
        }
        self->conn->close();
    };
}

uint32_t user::get_id() const {
    return id;
}

bool user::is_player() const {
    return !is_spectator();
}

bool user::is_spectator() const {
    return my_controller_map.empty();
}

const array<controller, 4>& user::get_controllers() const {
    return controllers;
}

const string& user::get_name() const {
    return name;
}

double user::get_latency() const {
    return latency_history.empty() ? NAN : latency_history.front();
}

double user::get_median_latency() const {
    if (latency_history.empty()) return NAN;
    vector<double> lat(latency_history.begin(), latency_history.end());
    sort(lat.begin(), lat.end());
    return lat[lat.size() / 2];
}

double user::get_fps() {
    if (frame_history.empty() || frame_history.front() == frame_history.back()) {
        return NAN;
    } else {
        return (frame_history.size() - 1) / (frame_history.back() - frame_history.front());
    }
}

void user::process_packet() {
    auto self(shared_from_this());
    conn->receive([=](packet& pin, const error_code& error) {
        if (error) return self->error_handler()(error);
        if (pin.empty()) return self->process_packet();

        try {
            switch (pin.read<PACKET_TYPE>()) {
                case JOIN: {
                    if (joined()) break;
                    auto protocol_version = pin.read<uint32_t>();
                    if (protocol_version != PROTOCOL_VERSION) {
                        return conn->close();
                    }
                    auto room = pin.read<string>();
                    if (!room.empty() && room[0] == '/') {
                        room = room.substr(1);
                    }
                    pin.read(self->name);
                    log(self->name + " (" + conn->get_address() + ") connected");
                    for (auto& c : controllers) {
                        pin >> c.plugin >> c.present >> c.raw_data;
                    }
                    if (pin.available()) {
                        pin >> rom.crc1 >> rom.crc2 >> rom.name >> rom.country_code >> rom.version;
                    }
                    if (!my_server.expired()) {
                        my_server.lock()->on_user_join(self, room);
                    }
                    break;
                }

                case PING: {
                    pout.reset() << PONG;
                    while (pin.available()) {
                        pout << pin.read<uint8_t>();
                    }
                    conn->send(pout, error_handler());
                    if (!joined()) {
                        log(conn->get_address() + " pinged the server");
                    }
                    break;
                }

                case PONG: {
                    latency_history.push_back(timestamp() - pin.read<double>());
                    while (latency_history.size() > 7) latency_history.pop_front();
                    break;
                }

                case CONTROLLERS: {
                    if (!joined()) break;
                    for (auto& c : controllers) {
                        pin >> c.plugin >> c.present >> c.raw_data;
                    }
                    auto r = my_room.lock();
                    if (!r->started) {
                        r->update_controller_map();
                    }
                    r->send_controllers();
                    break;
                }

                case NAME: {
                    if (!joined()) break;
                    string old_name = self->name;
                    pin.read(self->name);
                    auto r = my_room.lock();
                    log("[" + r->get_id() + "] " + old_name + " is now " + self->name);
                    for (auto& u : r->users) {
                        u->send_name(id, name);
                    }
                    break;
                }

                case MESSAGE: {
                    if (!joined()) break;
                    auto message = pin.read<string>();
                    for (auto& u : my_room.lock()->users) {
                        if (u == self) continue;
                        u->send_message(get_id(), message);
                    }
                    break;
                }

                case LAG: {
                    if (!joined()) break;
                    auto lag = pin.read<uint8_t>();
                    my_room.lock()->send_lag(id, lag);
                    break;
                }

                case AUTOLAG: {
                    if (!joined()) break;
                    auto value = pin.read<int8_t>();
                    auto r = my_room.lock();
                    if (value == (int8_t)r->autolag) break;

                    if (value == 0) {
                        r->autolag = false;
                    } else if (value == 1) {
                        r->autolag = true;
                    } else {
                        r->autolag = !r->autolag;
                    }
                    if (r->autolag) {
                        r->send_info("Automatic Lag is enabled");
                    } else {
                        r->send_info("Automatic Lag is disabled");
                    }
                    break;
                }

                case START: {
                    if (!joined()) break;
                    auto r = my_room.lock();
                    log("[" + r->get_id() + "] " + self->name + " started the game");
                    r->on_game_start();
                    break;
                }

                case INPUT_DATA: {
                    if (!joined()) break;
                    input_received++;
                    current_input.resize(pin.available());
                    pin.read(current_input);
                    auto r = my_room.lock();
                    if (!r->hia) {
                        pout.reset() << INPUT_DATA << id << current_input;
                        for (auto& u : r->users) {
                            if (u == self) continue;
                            u->send_input(*this, pout);
                        }
                    }
                    break;
                }

                case INPUT_FILL: {
                    if (!joined()) break;
                    pin >> input_received;
                    pout.reset() << INPUT_FILL << id << input_received;
                    for (auto& u : my_room.lock()->users) {
                        if (u->id == id) continue;
                        u->conn->send(pout, error_handler());
                    }
                    break;
                }

                case FRAME: {
                    auto ts = timestamp();
                    frame_history.push_back(ts);
                    while (frame_history.front() <= ts - 2.0) {
                        frame_history.pop_front();
                    }
                    break;
                }

                case CONTROLLER_MAP: {
                    if (!joined()) break;
                    controller_map map(pin.read<uint16_t>());
                    pout.reset() << CONTROLLER_MAP << id << map.bits;
                    auto r = my_room.lock();
                    for (auto& u : r->users) {
                        if (u->id == id) continue;
                        u->conn->send(pout, error_handler());
                    }
                    my_controller_map = map;
                    manual_map = true;
                    break;
                }

                case GOLF: {
                    if (!joined()) break;
                    auto r = my_room.lock();
                    r->golf = pin.read<bool>();
                    for (auto& u : r->users) {
                        if (u->id == id) continue;
                        u->conn->send(pin, error_handler());
                    }
                    break;
                }

                case SYNC_REQ: {
                    if (!joined()) break;
                    auto sync_id = pin.read<uint32_t>();
                    pout.reset() << SYNC_REQ << id << sync_id;
                    auto r = my_room.lock();
                    for (auto& u : r->users) {
                        if (u->id == id) continue;
                        u->conn->send(pout, error_handler());
                    }
                    break;
                }

                case SYNC_RES: {
                    if (!joined()) break;
                    auto user_id = pin.read<uint32_t>();
                    auto r = my_room.lock();
                    auto user = r->get_user(user_id);
                    if (!user) break;
                    auto sync_id = pin.read<uint32_t>();
                    auto frame = pin.read<uint32_t>();
                    user->conn->send(pout.reset() << SYNC_RES << id << sync_id << frame, error_handler());
                    break;
                }

                case HIA: {
                    if (!joined()) break;
                    auto hia = std::min(240u, pin.read_var<uint32_t>());
                    auto r = my_room.lock();
                    if (!r->started || r->hia && hia) {
                        r->set_hia(hia);
                        log("[" + r->get_id() + "] " + get_name() + " " + (hia ? "enabled HIA at " + to_string(hia) + " Hz" : "disabled HIA"));
                        for (auto& u : r->users) {
                            u->send_hia(hia);
                        }
                    }
                    break;
                }
            }

            self->process_packet();
        } catch (...) {
            self->conn->close();
        }
    });
}

void user::send_protocol_version() {
    conn->send(pout.reset() << VERSION << PROTOCOL_VERSION, error_handler());
}

void user::send_accept() {
    conn->send(pout.reset() << ACCEPT << id, error_handler());
}

void user::send_join(uint32_t user_id, const string& name) {
    conn->send(pout.reset() << JOIN << user_id << name, error_handler());
}

void user::send_start_game() {
    conn->send(pout.reset() << START, error_handler());
}

void user::send_name(uint32_t user_id, const string& name) {
    conn->send(pout.reset() << NAME << user_id << name, error_handler());
}

void user::send_ping() {
    conn->send(pout.reset() << PING << timestamp(), error_handler());
}

void user::send_quit(uint32_t id) {
    conn->send(pout.reset() << QUIT << id, error_handler());
}

void user::send_message(int32_t id, const string& message) {
    conn->send(pout.reset() << MESSAGE << id << message, error_handler());
}

void user::send_info(const string& message) {
    send_message(INFO_MESSAGE, message);
}

void user::send_error(const string& message) {
    send_message(ERROR_MESSAGE, message);
}

void user::send_lag(uint8_t lag) {
    conn->send(pout.reset() << LAG << lag, error_handler());
}

void user::send_input(const user& user, const packet& p) {
    conn->send(p, error_handler(), false);
    auto r = my_room.lock();
    if (r->hia) return;
    for (auto& u : r->users) {
        if (u->id == id) continue;
        if (u->is_spectator()) continue;
        if (u->input_received < user.input_received) return;
    }
    conn->flush(error_handler());
}

void user::send_hia(uint32_t hia) {
    pout.reset() << HIA;
    pout.write_var(hia);
    conn->send(pout, error_handler());
}