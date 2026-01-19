#include <Core/App.hpp>
#include <Core/DefaultModules.hpp>

#include "Game.hpp"

int main(int argc, char** argv) {
  auto app = ox::App(argc, argv);
  app.with_name("ExampleGame")
    .with_window({
      .title = "ExampleGame",
      .icon = {},
      .width = 1020,
      .height = 900,
      .flags = ox::WindowFlag::Centered | ox::WindowFlag::Resizable | ox::WindowFlag::HighPixelDensity,
    })
    .with_assets_directory("Resources")
    .with(ox::DefaultModules{})
    .with<Game>()
    .run();

  return 0;
}
