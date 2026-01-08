#include "Core/App.hpp"
#include "Networking/NetworkManager.hpp"
#include "Server.hpp"

int main(int argc, char** argv) {
  auto app = ox::App(argc, argv);
  app
    .with_name("server") //
    .with_frame_limit(66)
    .with<ox::NetworkManager>()
    .with<Server>()
    .run();

  return 0;
}
