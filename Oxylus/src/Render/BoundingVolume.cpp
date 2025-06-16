#include "BoundingVolume.hpp"

#include "Frustum.hpp"
#include "Physics/RayCast.hpp"

namespace ox {
void AABB::translate(const glm::vec3& translation) {
  ZoneScoped;
  min += translation;
  max += translation;
}

void AABB::scale(const glm::vec3& scale) {
  ZoneScoped;
  min *= scale;
  max *= scale;
}

void AABB::rotate(const glm::mat3& rotation) {
  ZoneScoped;
  const auto center = get_center();
  const auto extents = get_extents();

  const auto rotated_extents = glm::vec3(rotation * glm::vec4(extents, 1.0f));

  min = center - rotated_extents;
  max = center + rotated_extents;
}

void AABB::transform(const glm::mat4& transform) {
  ZoneScoped;
  const glm::vec3 new_center = transform * glm::vec4(get_center(), 1.0f);
  const glm::vec3 old_edge = get_size() * 0.5f;
  const glm::vec3 new_edge = glm::vec3(glm::abs(transform[0][0]) * old_edge.x + glm::abs(transform[1][0]) * old_edge.y +
                                           glm::abs(transform[2][0]) * old_edge.z,
                                       glm::abs(transform[0][1]) * old_edge.x + glm::abs(transform[1][1]) * old_edge.y +
                                           glm::abs(transform[2][1]) * old_edge.z,
                                       glm::abs(transform[0][2]) * old_edge.x + glm::abs(transform[1][2]) * old_edge.y +
                                           glm::abs(transform[2][2]) * old_edge.z);

  min = new_center - new_edge;
  max = new_center + new_edge;
}

AABB AABB::get_transformed(const glm::mat4& transform) const {
  AABB aabb(*this);
  aabb.transform(transform);
  return aabb;
}

void AABB::merge(const AABB& other) {
  ZoneScoped;
  if (other.min.x < min.x)
    min.x = other.min.x;
  if (other.min.y < min.y)
    min.y = other.min.y;
  if (other.min.z < min.z)
    min.z = other.min.z;
  if (other.max.x > max.x)
    max.x = other.max.x;
  if (other.max.y > max.y)
    max.y = other.max.y;
  if (other.max.z > max.z)
    max.z = other.max.z;
}

// https://gdbooks.gitbooks.io/3dcollisions/content/Chapter2/static_aabb_plane.html
bool AABB::is_on_or_forward_plane(const Plane& plane) const {
  ZoneScoped;
  // projection interval radius of b onto L(t) = b.c + t * p.n
  const auto extent = get_extents();
  const auto center = get_center();
  const float r = extent.x * std::abs(plane.normal.x) + extent.y * std::abs(plane.normal.y) +
                  extent.z * std::abs(plane.normal.z);

  return -r <= plane.get_distance(center);
}

bool AABB::is_on_frustum(const Frustum& frustum) const {
  ZoneScoped;
  return is_on_or_forward_plane(frustum.left_face) && is_on_or_forward_plane(frustum.right_face) &&
         is_on_or_forward_plane(frustum.top_face) && is_on_or_forward_plane(frustum.bottom_face) &&
         is_on_or_forward_plane(frustum.near_face) && is_on_or_forward_plane(frustum.far_face);
}

bool AABB::intersects(const glm::vec3& point) const {
  if (point.x < min.x || point.x > max.x || point.y < min.y || point.y > max.y || point.z < min.z || point.z > max.z)
    return false;
  return true;
}

Intersection AABB::intersects(const AABB& box) const {
  if (box.max.x < min.x || box.min.x > max.x || box.max.y < min.y || box.min.y > max.y || box.max.z < min.z ||
      box.min.z > max.z)
    return Outside;
  if (box.min.x < min.x || box.max.x > max.x || box.min.y < min.y || box.max.y > max.y || box.min.z < min.z ||
      box.max.z > max.z)
    return Intersects;
  return Inside;
}

bool AABB::intersects_fast(const AABB& box) const {
  if (box.max.x < min.x || box.min.x > max.x || box.max.y < min.y || box.min.y > max.y || box.max.z < min.z ||
      box.min.z > max.z)
    return false;
  return true;
}

bool AABB::intersects(const RayCast& ray) const {
  if (intersects(ray.get_origin()))
    return true;

  const float tx1 = (min.x - ray.get_origin().x) * ray.get_direction_inverse().x;
  const float tx2 = (max.x - ray.get_origin().x) * ray.get_direction_inverse().x;

  float tmin = std::min(tx1, tx2);
  float tmax = std::max(tx1, tx2);
  if (ray.t_max < tmin || ray.t_min > tmax)
    return false;

  const float ty1 = (min.y - ray.get_origin().y) * ray.get_direction_inverse().y;
  const float ty2 = (max.y - ray.get_origin().y) * ray.get_direction_inverse().y;

  tmin = std::max(tmin, std::min(ty1, ty2));
  tmax = std::min(tmax, std::max(ty1, ty2));
  if (ray.t_max < tmin || ray.t_min > tmax)
    return false;

  const float tz1 = (min.z - ray.get_origin().z) * ray.get_direction_inverse().z;
  const float tz2 = (max.z - ray.get_origin().z) * ray.get_direction_inverse().z;

  tmin = std::max(tmin, std::min(tz1, tz2));
  tmax = std::min(tmax, std::max(tz1, tz2));
  if (ray.t_max < tmin || ray.t_min > tmax)
    return false;

  return tmax >= tmin;
}

bool Sphere::intersects(const AABB& b) const {
  const glm::vec3 closestPointInAabb = glm::min(glm::max(center, b.min), b.max);
  const float distanceSquared = glm::distance2(closestPointInAabb, center);
  return distanceSquared < radius * radius;
}

bool Sphere::intersects(const Sphere& b) const {
  float dist = 0;
  return intersects(b, dist);
}

bool Sphere::intersects(const Sphere& b, float& dist) const {
  dist = glm::distance(center, b.center);
  dist = dist - radius - b.radius;
  return dist < 0;
}

bool Sphere::intersects(const Sphere& b, float& dist, glm::vec3& direction) const {
  auto dir = center - b.center;
  const auto dist1 = glm::length(dir);
  dir = dir / dist1;
  direction = dir;
  dist = dist1;
  dist = dist - radius - b.radius;
  return dist < 0;
}

bool Sphere::intersects(const RayCast& b) const {
  float dist;
  glm::vec3 direction;
  return intersects(b, dist, direction);
}

bool Sphere::intersects(const RayCast& b, float& dist) const {
  glm::vec3 direction;
  return intersects(b, dist, direction);
}

bool Sphere::intersects(const RayCast& b, float& dist, glm::vec3& direction) const {
  auto C = center;
  auto O = b.get_origin();
  auto D = b.get_direction();
  auto OC = O - C;
  float B = glm::dot(OC, D);
  float c = glm::dot(OC, OC) - radius * radius;
  float discr = B * B - c;
  if (discr > 0) {
    float discrSq = std::sqrt(discr);

    float t = (-B - discrSq);
    if (t < b.t_max && t > b.t_min) {
      auto P = O + D * t;
      auto N = glm::normalize(P - C);
      dist = t;
      direction = N;
      return true;
    }

    t = (-B + discrSq);
    if (t < b.t_max && t > b.t_min) {
      auto P = O + D * t;
      auto N = glm::normalize(P - C);
      dist = t;
      direction = N;
    }
  }
  return false;
}
} // namespace ox
