#pragma once
#include "net/connection.h"
#include <memory>
#include <string>

namespace chat { class ChatServer; }

namespace chat::handlers {

// Top-level dispatcher invoked by ChatServer::on_frame on a worker thread.
// Routes a parsed-but-not-decoded JSON payload to the appropriate handler.
void dispatch(ChatServer* srv,
              std::shared_ptr<net::Connection> conn,
              const std::string& payload);

}  // namespace chat::handlers
