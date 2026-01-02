implementing gpu;

import common;

// Image1D ──────────────────────────────────────────────────────────
public typealias Image1D<T : ITexelElement> = Texture1D<T>;

// Image2D ──────────────────────────────────────────────────────────
public typealias Image2D<T : ITexelElement> = Texture2D<T>;

// Image3D ──────────────────────────────────────────────────────────
public typealias Image3D<T : ITexelElement> = Texture3D<T>;

// StorageImage1D ───────────────────────────────────────────────────
public typealias StorageImage1D<T : ITexelElement, let F : ImageFormat = ImageFormat::Undefined> = RWTexture1D<T>;

// StorageImage2D ───────────────────────────────────────────────────
public typealias StorageImage2D<T : ITexelElement, let F : ImageFormat = ImageFormat::Undefined> = RWTexture2D<T>;

// StorageImage3D ───────────────────────────────────────────────────
public typealias StorageImage3D<T : ITexelElement, let F : ImageFormat = ImageFormat::Undefined> = RWTexture3D<T>;