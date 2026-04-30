// Minimal CLI chat client — TCP, length-prefixed JSON.
//
// Usage: cli_client <host> <port>
// Type ':help' at the prompt for a list of commands.
#include "common/platform.h"
#include "protocol/framing.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

socket_t g_sock = CHAT_SOCKET_INVALID;
std::atomic<bool> g_running{true};
std::atomic<std::int64_t> g_seq{1};
std::mutex g_out;

void out(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_out);
  std::cout << s << std::endl;
}

bool send_frame(const json& j) {
  auto framed = chat::protocol::frame(j.dump());
  std::size_t off = 0;
  while (off < framed.size()) {
    auto n = ::send(g_sock, framed.data() + off,
                    static_cast<int>(framed.size() - off), 0);
    if (n <= 0) return false;
    off += static_cast<std::size_t>(n);
  }
  return true;
}

void reader_loop() {
  chat::protocol::FrameDecoder dec;
  char buf[4096];
  while (g_running.load()) {
    auto n = ::recv(g_sock, buf, sizeof(buf), 0);
    if (n <= 0) {
      out("[server closed connection]");
      g_running.store(false);
      return;
    }
    try {
      auto frames = dec.feed(buf, static_cast<std::size_t>(n));
      for (auto& f : frames) {
        try {
          auto j = json::parse(f);
          out(j.dump(2));
        } catch (...) {
          out("[non-json frame]");
        }
      }
    } catch (const std::exception& e) {
      out(std::string("[bad frame] ") + e.what());
      g_running.store(false);
      return;
    }
  }
}

void print_help() {
  out("Commands:");
  out("  :help                          show this help");
  out("  :register <user> <pass>");
  out("  :login    <user> <pass>");
  out("  :logout");
  out("  :dm <user_id> <content...>     send a direct message");
  out("  :create_room <name>");
  out("  :join <room_id|name>");
  out("  :leave <room_id>");
  out("  :rooms                         list rooms you're in");
  out("  :room <room_id> <content...>   send to a room");
  out("  :history_dm <user_id> [limit]");
  out("  :history_room <room_id> [limit]");
  out("  :presence <user_id>");
  out("  :ping");
  out("  :quit");
}

std::vector<std::string> tokens(const std::string& s, std::size_t max) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < s.size() && out.size() + 1 < max) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    std::size_t j = i;
    while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
    if (i < j) out.push_back(s.substr(i, j - i));
    i = j;
  }
  while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
  if (i < s.size()) out.push_back(s.substr(i));
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: cli_client <host> <port>\n";
    return 1;
  }
  if (!chat::platform::init()) {
    std::cerr << "platform init failed\n";
    return 1;
  }

  addrinfo hints{};
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(argv[1], argv[2], &hints, &res) != 0 || !res) {
    std::cerr << "resolve failed\n";
    return 1;
  }
  g_sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (!socket_valid(g_sock)) { std::cerr << "socket failed\n"; return 1; }
  if (::connect(g_sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
    std::cerr << "connect failed\n"; return 1;
  }
  ::freeaddrinfo(res);
  out(std::string("connected to ") + argv[1] + ":" + argv[2]);
  out("type :help for commands");

  std::thread reader(reader_loop);

  std::string line;
  while (g_running.load() && std::getline(std::cin, line)) {
    if (line.empty()) continue;
    auto t = tokens(line, 16);
    if (t.empty()) continue;
    json req;
    req["id"] = g_seq.fetch_add(1);

    const std::string& cmd = t[0];
    if (cmd == ":help") { print_help(); continue; }
    else if (cmd == ":quit") { g_running.store(false); break; }
    else if (cmd == ":ping") { req["type"] = "ping"; }
    else if (cmd == ":register" && t.size() >= 3) {
      req["type"]="register"; req["username"]=t[1]; req["password"]=t[2];
    }
    else if (cmd == ":login" && t.size() >= 3) {
      req["type"]="login"; req["username"]=t[1]; req["password"]=t[2];
    }
    else if (cmd == ":logout") { req["type"]="logout"; }
    else if (cmd == ":dm" && t.size() >= 3) {
      req["type"]="send_dm";
      req["receiver_id"]=std::stoll(t[1]);
      req["content"]=t[2];
    }
    else if (cmd == ":create_room" && t.size() >= 2) {
      req["type"]="create_room"; req["name"]=t[1];
    }
    else if (cmd == ":join" && t.size() >= 2) {
      req["type"]="join_room";
      try { req["room_id"]=std::stoll(t[1]); } catch(...) { req["name"]=t[1]; }
    }
    else if (cmd == ":leave" && t.size() >= 2) {
      req["type"]="leave_room"; req["room_id"]=std::stoll(t[1]);
    }
    else if (cmd == ":rooms") { req["type"]="list_rooms"; }
    else if (cmd == ":room" && t.size() >= 3) {
      req["type"]="send_room";
      req["room_id"]=std::stoll(t[1]);
      req["content"]=t[2];
    }
    else if (cmd == ":history_dm" && t.size() >= 2) {
      req["type"]="history_dm"; req["user_id"]=std::stoll(t[1]);
      if (t.size() >= 3) req["limit"]=std::stoi(t[2]);
    }
    else if (cmd == ":history_room" && t.size() >= 2) {
      req["type"]="history_room"; req["room_id"]=std::stoll(t[1]);
      if (t.size() >= 3) req["limit"]=std::stoi(t[2]);
    }
    else if (cmd == ":presence" && t.size() >= 2) {
      req["type"]="presence"; req["user_id"]=std::stoll(t[1]);
    }
    else {
      out("unknown command; :help"); continue;
    }
    if (!send_frame(req)) {
      out("[send failed]"); g_running.store(false); break;
    }
  }
  g_running.store(false);
  socket_close(g_sock);
  reader.join();
  chat::platform::shutdown();
  return 0;
}
