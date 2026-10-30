using System.Numerics;

namespace Oxylus;

// TransformComponent math done in C#, as Scene::get_world_transform does it: a child's local translation, rotation
// and scale under its parent's world transform. Matrices are System.Numerics' row-vector ones, so `local * parent`
// is glm's `parent * local`. Forward is -Z, as the engine's cameras look
public static class Transforms
{
    extension(TransformComponent transform)
    {
        public Matrix4x4 LocalMatrix =>
            Matrix4x4.CreateScale(transform.Scale)
            * Matrix4x4.CreateFromQuaternion(transform.Rotation)
            * Matrix4x4.CreateTranslation(transform.Position);

        public Vector3 Forward => Vector3.Transform(-Vector3.UnitZ, transform.Rotation);
        public Vector3 Right => Vector3.Transform(Vector3.UnitX, transform.Rotation);
        public Vector3 Up => Vector3.Transform(Vector3.UnitY, transform.Rotation);
    }

    extension(Entity entity)
    {
        public Matrix4x4 WorldMatrix
        {
            get
            {
                var world = entity.Get<TransformComponent>().LocalMatrix;
                for (var parent = entity.Parent; parent.Id != 0 && parent.Has<TransformComponent>(); parent = parent.Parent)
                {
                    world *= parent.Get<TransformComponent>().LocalMatrix;
                }

                return world;
            }
        }

        public Vector3 WorldPosition
        {
            get => entity.WorldMatrix.Translation;
            set
            {
                ref var transform = ref entity.Get<TransformComponent>();
                transform.Position = Matrix4x4.Invert(entity.ParentWorldMatrix, out var toLocal)
                    ? Vector3.Transform(value, toLocal)
                    : value;
                entity.Modified<TransformComponent>();
            }
        }

        public Quaternion WorldRotation
        {
            get
            {
                Matrix4x4.Decompose(entity.WorldMatrix, out _, out var rotation, out _);
                return rotation;
            }
            set
            {
                Matrix4x4.Decompose(entity.ParentWorldMatrix, out _, out var parentRotation, out _);
                ref var transform = ref entity.Get<TransformComponent>();
                transform.Rotation = Quaternion.Normalize(Quaternion.Inverse(parentRotation) * value);
                entity.Modified<TransformComponent>();
            }
        }

        // turns the entity's forward toward `target`, both in world space
        public void LookAt(Vector3 target, Vector3 up)
        {
            var direction = target - entity.WorldPosition;
            if (direction.LengthSquared() < 1e-12f)
            {
                return;
            }

            entity.WorldRotation = Quaternion.CreateFromRotationMatrix(
                Matrix4x4.CreateWorld(Vector3.Zero, Vector3.Normalize(direction), up));
        }

        Matrix4x4 ParentWorldMatrix
        {
            get
            {
                var parent = entity.Parent;
                return parent.Id != 0 && parent.Has<TransformComponent>() ? parent.WorldMatrix : Matrix4x4.Identity;
            }
        }
    }
}
