implementing gpu;

import common;

public enum ImageOperand : u32 {
    None = 0x0,
    Bias = 0x1,
    Lod  = 0x2,
    Grad = 0x4,
    ConstOffset = 0x8,
    Offset = 0x10,
    ConstOffsets = 0x20,
    MinLod = 0x80,

    // SPIR-V 1.5 - SPV_KHR_vulkan_memory_model
    MakeTexelAvailable = 0x100,  // Requires NonPrivateTexel to also be set.
    MakeTexelVisible = 0x200,    // Requires NonPrivateTexel to also be set.
    NonPrivateTexel = 0x400,
    VolatileTexel = 0x800,
};

// Image ────────────────────────────────────────────────────────────
public typealias Image<T : ITexelElement, S : __ITextureShape> = _Texture<T, S, 0, 0, 0, 0, 0, 0, 0>;

public extension<T : ITexelElement, S : __ITextureShape> Image<T, S> {
    public func sample(in Sampler sampler, vector<f32, S.dimensions> tex_coords) -> T {
        return this.Sample(sampler, tex_coords);
    }

    public func sample_mip(in Sampler sampler, vector<f32, S.dimensions> tex_coords, f32 mip) -> T {
        return this.SampleLevel(sampler, tex_coords, mip);
    }

    public func sample_grad(
        in Sampler sampler,
        vector<f32, S.dimensions> tex_coords,
        vector<f32, S.dimensions> ddx,
        vector<f32, S.dimensions> ddy
    ) -> T {
        return this.SampleGrad(sampler, tex_coords, ddx, ddy);
    }
};

// Image1D ──────────────────────────────────────────────────────────
__generic<T : ITexelElement>
public typealias Image1D = Image<T, __Shape1D>;

public extension<T : ITexelElement> Image1D<T> {
};

// Image2D ──────────────────────────────────────────────────────────
public typealias Image2D<T : ITexelElement> = Image<T, __Shape2D>;

public extension<T : ITexelElement> Image2D<T> {
    public func load(u32x2 texel, u32 mip = 0) -> T {
        let coord = __vectorReshape<2>(texel);
        return spirv_asm {
            %sampled: __sampledType(T) = OpImageFetch $this $coord Lod $mip;
            __truncate $$T result __sampledType(T) %sampled;
        };
    }
};

// Image3D ──────────────────────────────────────────────────────────
public typealias Image3D<T : ITexelElement> = Image<T, __Shape3D>;

public extension<T : ITexelElement> Image3D<T> {
};

// StorageImage ────────────────────────────────────────────────────────────
public typealias StorageImage<T : ITexelElement, S : __ITextureShape, let F : ImageFormat> = _Texture<T, S, 0, 0, 0, 1, 0, 0, F>;
public extension<T : ITexelElement, S : __ITextureShape, let F : ImageFormat> StorageImage<T, S, F> {
    public func load(vector<u32, S.dimensions> texel, com::MemoryScope scope = com::MemoryScope::Device) -> T {
        return spirv_asm {
            %sampled:__sampledType(T) = OpImageRead $this $texel NonPrivateTexel|MakeTexelVisible $scope;
            __truncate $$T result __sampledType(T) %sampled;
        };
    }
    
    public func store(vector<u32, S.dimensions> texel, T value, com::MemoryScope scope = com::MemoryScope::Device) -> void {
        spirv_asm {
            OpImageWrite $this $texel __convertTexel(value) NonPrivateTexel|MakeTexelAvailable $scope;
        };
    }
};

// StorageImage1D ───────────────────────────────────────────────────
public typealias StorageImage1D<T : ITexelElement, let F : ImageFormat = ImageFormat::Undefined> = StorageImage<T, __Shape1D, F>;

// StorageImage2D ───────────────────────────────────────────────────
public typealias StorageImage2D<T : ITexelElement, let F : ImageFormat = ImageFormat::Undefined> = StorageImage<T, __Shape2D, F>;

// StorageImage3D ───────────────────────────────────────────────────
public typealias StorageImage3D<T : ITexelElement, let F : ImageFormat = ImageFormat::Undefined> = StorageImage<T, __Shape3D, F>;
