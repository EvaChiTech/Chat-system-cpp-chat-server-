// chat_server entry point.
//
// Parses --config, loads the file, overlays env vars, builds a ChatServer,
// and runs it. Installs SIGINT/SIGTERM handlers (POSIX) for graceful shutdown.
#include "chat/chat_server.h"
#include "common/logger.h"
#include "common/config.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#if !defined(_WIN32)
  #include <unistd.h>
#endif

namespace {
std::atomic<chat::ChatServer*> g_server{nullptr};

void on_signal(int sig) {
  auto* s = g_server.load();
  if (s) s->stop();
  (void)sig;
}
}  // namespace

static void usage() {
  std::cout <<
    "chat_server [--config <path>] [--listen <host>] [--port <n>]\n"
    "            [--no-mysql]   build option only — see CMake\n";
}

int main(int argc, char** argv) {
  std::string config_path;
  std::string override_host;
  int         override_port = -1;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") { usage(); return 0; }
    else if (a == "--config" && i + 1 < argc) config_path = argv[++i];
    else if (a == "--listen" && i + 1 < argc) override_host = argv[++i];
    else if (a == "--port"   && i + 1 < argc) override_port = std::atoi(argv[++i]);
  }

  chat::Config cfg;
  if (!config_path.empty()) cfg.load_file(config_path);
  cfg.overlay_env();
  if (!override_host.empty()) cfg.set("listen_host", override_host);
  if (override_port > 0)      cfg.set("listen_port", std::to_string(override_port));

  chat::ChatServer server(cfg);
  if (!server.init()) {
    std::cerr << "server init failed\n";
    return 1;
  }
  g_server.store(&server);

#if defined(_WIN32)
  ::signal(SIGINT,  on_signal);
  ::signal(SIGTERM, on_signal);
#else
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT,  &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  ::signal(SIGPIPE, SIG_IGN);  // never let a write kill us
#endif

  LOG_INFO("chat_server starting");
  server.run();
  LOG_INFO("chat_server exited cleanly");
  return 0;
}
