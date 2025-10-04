#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

namespace ox {
class Scene;
}

namespace PhysicsLayers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}; // namespace PhysicsLayers

// Class that determines if two object layers can collide
class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
  bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override;
};

// Each broadphase layer results in a separate bounding volume tree in the broad phase. You at least want to have
// a layer for non-moving and moving objects to avoid having to update a tree full of static objects every frame.
// You can have a 1-on-1 mapping between object layers and broadphase layers (like in this case) but if you have
// many object layers you'll be creating many broad phase trees, which is not efficient. If you want to fine tune
// your broadphase layers define JPH_TRACK_BROADPHASE_STATS and look at the stats reported on the TTY.
namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint NUM_LAYERS(2);
}; // namespace BroadPhaseLayers

namespace ox {
// BroadPhaseLayerInterface implementation
// This defines a mapping between object and broadphase layers.
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
  BPLayerInterfaceImpl();

  JPH::uint GetNumBroadPhaseLayers() const override;

  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override;

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override;
#endif

private:
  JPH::BroadPhaseLayer mObjectToBroadPhase[PhysicsLayers::NUM_LAYERS];
};

// Class that determines if an object layer can collide with a broadphase layer
class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
  bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override;
};

class Physics3DBodyActivationListener : public JPH::BodyActivationListener {
public:
  void OnBodyActivated([[maybe_unused]] const JPH::BodyID& inBodyID,
                       [[maybe_unused]] JPH::uint64 inBodyUserData) override;

  void OnBodyDeactivated([[maybe_unused]] const JPH::BodyID& inBodyID,
                         [[maybe_unused]] JPH::uint64 inBodyUserData) override;
};

class Physics3DContactListener : public JPH::ContactListener {
public:
  Physics3DContactListener(ox::Scene* scene) : _scene(scene) {}
  JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1,
                                        const JPH::Body& inBody2,
                                        JPH::RVec3Arg inBaseOffset,
                                        const JPH::CollideShapeResult& inCollisionResult) override;

  void OnContactAdded(const JPH::Body& inBody1,
                      const JPH::Body& inBody2,
                      const JPH::ContactManifold& inManifold,
                      JPH::ContactSettings& ioSettings) override;

  void OnContactPersisted(const JPH::Body& inBody1,
                          const JPH::Body& inBody2,
                          const JPH::ContactManifold& inManifold,
                          JPH::ContactSettings& ioSettings) override;

  void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override;

private:
  ox::Scene* _scene = nullptr;
  static void GetFrictionAndRestitution(const JPH::Body& inBody,
                                        const JPH::SubShapeID& inSubShapeID,
                                        float& outFriction,
                                        float& outRestitution);

  static void OverrideContactSettings(const JPH::Body& inBody1,
                                      const JPH::Body& inBody2,
                                      const JPH::ContactManifold& inManifold,
                                      JPH::ContactSettings& ioSettings);
};
} // namespace ox
