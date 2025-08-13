#include "Scripting/LuaAudioBindings.hpp"

#include <sol/state.hpp>

#include "Asset/AudioSource.hpp"
#include "Audio/AudioEngine.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox::LuaBindings {
void bind_audio(sol::state* state) {
  auto audio_source = state->new_usertype<AudioSource>("AudioSource");

  // #define ASC AudioSourceComponent
  // REGISTER_COMPONENT(state, ASC, FIELD(ASC, config), FIELD(ASC, source));

  const std::initializer_list<std::pair<sol::string_view, AudioEngine::AttenuationModelType>> attenuation_model_type = {
      ENUM_FIELD(AudioEngine::AttenuationModelType, Inverse),
      ENUM_FIELD(AudioEngine::AttenuationModelType, Linear),
      ENUM_FIELD(AudioEngine::AttenuationModelType, Exponential),
  };
  state->new_enum<AudioEngine::AttenuationModelType, true>("AttenuationModelType", attenuation_model_type);

  // #define ALC AudioListenerComponent
  // REGISTER_COMPONENT(state, ALC, FIELD(ALC, active), FIELD(ALC, config), FIELD(ALC, listener));
}
} // namespace ox::LuaBindings
