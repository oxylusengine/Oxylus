#include "Scripting/LuaAudioBindings.hpp"

#include <sol/state.hpp>

#include "Asset/AudioSource.hpp"
#include "Audio/AudioEngine.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
auto AudioBinding::bind(sol::state* state) -> void {
  auto audio_source = state->new_usertype<AudioSource>("AudioSource");

  const std::initializer_list<std::pair<sol::string_view, AudioEngine::AttenuationModelType>> attenuation_model_type = {
      ENUM_FIELD(AudioEngine::AttenuationModelType, Inverse),
      ENUM_FIELD(AudioEngine::AttenuationModelType, Linear),
      ENUM_FIELD(AudioEngine::AttenuationModelType, Exponential),
  };
  state->new_enum<AudioEngine::AttenuationModelType, true>("AttenuationModelType", attenuation_model_type);
}
} // namespace ox
