module fullscreen;

import common;

public struct VertexOutput {
    public f32x4 position : SV_Position;
    public f32x2 tex_coord : TEXCOORD;
};

[[shader("vertex")]]
public VertexOutput vs_main(u32 vertex_id : SV_VertexID) {
    VertexOutput output;
    output.tex_coord = f32x2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = f32x4(2.0 * output.tex_coord - 1.0, 1.0, 1.0);

    return output;
}