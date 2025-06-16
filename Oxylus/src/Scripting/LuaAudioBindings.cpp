#include "LuaAudioBindings.hpp"

#include <sol/state.hpp>

#include "Asset/AudioSource.hpp"
#include "LuaHelpers.hpp"
#include "Scene/ECSModule/Core.hpp"

namespace ox::LuaBindings {
void bind_audio(const std::shared_ptr<sol::state>& state) {
  auto audio_source = state->new_usertype<AudioSource>("AudioSource");

  // #define ASC AudioSourceComponent
  // REGISTER_COMPONENT(state, ASC, FIELD(ASC, config), FIELD(ASC, source));

  const std::initializer_list<std::pair<sol::string_view, AttenuationModelType>> attenuation_model_type = {
      ENUM_FIELD(AttenuationModelType, Inverse),
      ENUM_FIELD(AttenuationModelType, Linear),
      ENUM_FIELD(AttenuationModelType, Exponential),
  };
  state->new_enum<AttenuationModelType, true>("AttenuationModelType", attenuation_model_type);

  // #define ALC AudioListenerComponent
  // REGISTER_COMPONENT(state, ALC, FIELD(ALC, active), FIELD(ALC, config), FIELD(ALC, listener));
}
} // namespace ox::LuaBindings
