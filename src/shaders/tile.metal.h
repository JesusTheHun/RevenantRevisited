// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *           tile.metal.h  - MSL tile G-buffer fill shaders              *
// *************************************************************************
//
// Deferred shading G-buffer fill. Each tile sprite writes:
//   color(0)  albedo  (RGBA8)
//   color(1)  world normal, packed x*0.5+0.5 (RGBA16F); .a = surface
//             class, 1 tile / 0 mesh (selects the Classic lighting model)
//   color(2)  scene_z  (R32F, maps tile-local z to camera-depth [0..1])
//   color(3)  obj_id, non-zero iff this pixel is occupied
//   depth     same scene_z, for hardware depth test
//
// The opaque tile path intentionally fills depth/normal/scene_z/obj_id in the
// padded G-buffer border so screen-space shadows can see offscreen occluders.
// Albedo is masked back to the visible display rect by raycast.z (kGBufPad).
// TODO(renderer): split this into a visible color pass and a depth/id-only
// occluder-border pass once the pass graph is less transitional.
//
// Normals are reconstructed per-fragment from the tile depth map via a
// bilateral central-difference stencil. The iso inverse below matches the
// retail iso projection (cos30 = 867/1000). See docs/DEFERRED_LIGHTING.md
// for the full derivation.
//
// Perspective tiles have three projection modes:
//   0 flat projected quad: fastest fallback, not height/silhouette correct.
//   1 volume reference: slow screen/depth-space search used as ground truth.
//   2 relief/SPOM: default. Intersect the projected view ray against the
//     tile-local heightfield. A renderer-built min/max height mip pyramid skips
//     empty or depth-disjoint cells; a short bracket/refine pass resolves the
//     final pixel-stable UV for the shared G-buffer write code.
//
// Depth textures are sampled as data: point reads only, with zero treated as
// invalid coverage. Do not filter tile depth in this shader.
//
// *************************************************************************

#pragma once

inline constexpr const char* kTileVsMetal = R"MSL(
#include <metal_stdlib>
using namespace metal;
#define ISO_COS30 0.867
#define ISO_WZ_DENOM 2.003378
struct params { float4 rect; float4 zparams; float4 tile_root; float4 tile_sprite;
                float4 filter; float4 obj_id; float4 camera; float4 proj; float4 tile_rect;
                float4 cam_tile; float4 debug; float4 raycast; };
struct vs_in  { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };
struct vs_out { float4 pos [[position]]; float2 uv; };
vertex vs_out _main(vs_in in [[stage_in]], constant params& p [[buffer(0)]]) {
    vs_out o;
    if (p.camera.w > 0.5 && p.raycast.w <= 0.5) {
        float sx = in.uv.x * p.tile_sprite.z - p.tile_sprite.x;
        float sy = in.uv.y * p.tile_sprite.w - p.tile_sprite.y;
        float lz = (-2.0 * sy * ISO_COS30) / ISO_WZ_DENOM;
        float su = 2.0 * (sy + lz * ISO_COS30);
        float wx = p.tile_root.x + 0.5 * (su + sx);
        float wy = p.tile_root.y + 0.5 * (su - sx);
        float wz = p.tile_root.z + lz;
        float relx = wx - p.camera.x;
        float rely = wy - p.camera.y;
        float sum = relx + rely;
        float S = relx - rely;
        float T = 0.5 * sum - wz * ISO_COS30;
        float scene_z_wu = p.camera.z - ISO_COS30 * sum - 0.5 * wz;
        float zoom = max(p.zparams.w, 0.0001);
        float persp_scale = p.camera.z * zoom / max(scene_z_wu, 1.0);
        float spx = p.proj.x + S * persp_scale;
        float spy = p.proj.y + T * persp_scale;
        o.pos = float4(2.0 * spx / max(p.proj.z, 1.0) - 1.0,
                       1.0 - 2.0 * spy / max(p.proj.w, 1.0),
                       0.0, 1.0);
        o.uv = in.uv;
        return o;
    }
    o.pos = float4(p.rect.xy + in.pos * p.rect.zw, 0.0, 1.0);
    o.uv  = in.uv;
    return o;
}
)MSL";

// G-buffer fill:
//   zparams      .x = anchor_z, .y = depth_mul, .z = tile coverage scale, .w = zoom
//   tile_root    .xyz = anchor pixel world xyz, .w = zraw_to_wu scale
//   tile_sprite  .xy = anchor pixel (regx,regy), .zw = sprite size
//   tile_rect    .xy/.zw = conservative valid-heightfield coverage in
//                source pixels. This clips the camera-depth ray interval
//                before any iterative heightfield work.
//   filter       .x = normal_radius (texels), .y = bilateral edge threshold
//                .z/.w = tile local zraw min/max, relative to the anchor
//   cam_tile     .xy = tile anchor in old camera plane, .z = anchor camera depth,
//                .w = zraw_to_camera_depth scale
inline constexpr const char* kTileFsMetal = R"MSL(
#include <metal_stdlib>
using namespace metal;
#define ISO_COS30 0.867
#define ISO_WZ_DENOM 2.003378
#define EDGE_EPS 1e-6
struct params { float4 rect; float4 zparams; float4 tile_root; float4 tile_sprite;
                float4 filter; float4 obj_id; float4 camera; float4 proj; float4 tile_rect;
                float4 cam_tile; float4 debug; float4 raycast; };
struct vs_out { float4 pos [[position]]; float2 uv; };
struct fs_out { float4 albedo   [[color(0)]];
                float4 normal   [[color(1)]];
                float4 scene_z  [[color(2)]];
                float4 obj_id   [[color(3)]];
                float  depth    [[depth(any)]]; };
static float tile_depth_raw(float2 uv, texture2d<float> depth_tex) {
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 0.0;
    uint w = depth_tex.get_width();
    uint h = depth_tex.get_height();
    uint2 px = uint2(uint(clamp(uv.x * float(w), 0.0, float(w - 1))),
                     uint(clamp(uv.y * float(h), 0.0, float(h - 1))));
    return depth_tex.read(px).r;
}
static bool tile_depth_valid(float2 uv, texture2d<float> depth_tex) {
    return fabs(tile_depth_raw(uv, depth_tex)) > 0.0001;
}
static float tile_depth_read(float2 uv, texture2d<float> depth_tex, sampler smp) {
    (void)smp;
    return tile_depth_raw(uv, depth_tex);
}
static float2 tile_uv_for_cam_z(float cam_z, float2 ray_cam_xy, constant params& p) {
    const float tile_scale = max(p.zparams.z, 0.0001);
    float2 cam_xy = ray_cam_xy * cam_z;
    float sx = (cam_xy.x - p.cam_tile.x) / tile_scale;
    float sy = (cam_xy.y - p.cam_tile.y) / tile_scale;
    return float2((sx + p.tile_sprite.x) / max(p.tile_sprite.z, 1.0),
                  (sy + p.tile_sprite.y) / max(p.tile_sprite.w, 1.0));
}
static float4 tile_surface_at_cam_z(float cam_z, float2 ray_cam_xy,
                                    texture2d<float> depth_tex,
                                    constant params& p) {
    float2 uv = tile_uv_for_cam_z(cam_z, ray_cam_xy, p);
    float zraw = tile_depth_raw(uv, depth_tex);
    if (fabs(zraw) <= 0.0001)
        return float4(0.0, 0.0, uv.x, uv.y);
    return float4(1.0, p.cam_tile.z + zraw * p.cam_tile.w, uv.x, uv.y);
}
static bool tile_minmax_valid(float2 mm) {
    return fabs(mm.x) > 0.0001 || fabs(mm.y) > 0.0001;
}
static bool tile_segment_outside_unit(float2 a, float2 b) {
    return (a.x < 0.0 && b.x < 0.0) || (a.x > 1.0 && b.x > 1.0) ||
           (a.y < 0.0 && b.y < 0.0) || (a.y > 1.0 && b.y > 1.0);
}
static uint2 tile_mip_texel(float2 uv, texture2d<float> height_minmax_tex, uint mip) {
    uint w = max(height_minmax_tex.get_width(mip), 1u);
    uint h = max(height_minmax_tex.get_height(mip), 1u);
    return uint2(uint(clamp(uv.x * float(w), 0.0, float(w - 1))),
                 uint(clamp(uv.y * float(h), 0.0, float(h - 1))));
}
static bool tile_clip_z_axis(float ray_axis, float anchor_axis, float local_min,
                             float local_max, float tile_scale,
                             thread float& z0, thread float& z1) {
    float lo = anchor_axis + local_min * tile_scale;
    float hi = anchor_axis + local_max * tile_scale;
    if (lo > hi) {
        float tmp = lo;
        lo = hi;
        hi = tmp;
    }

    if (fabs(ray_axis) <= 1e-7) {
        // The ray is parallel to this tile axis in camera space. Either every
        // depth has the same coordinate and can proceed, or none can.
        return lo <= 0.0 && 0.0 <= hi;
    }

    float a = lo / ray_axis;
    float b = hi / ray_axis;
    if (a > b) {
        float tmp = a;
        a = b;
        b = tmp;
    }
    z0 = max(z0, a);
    z1 = min(z1, b);
    return z0 <= z1;
}
static bool tile_clip_z_to_coverage(float2 ray_cam_xy, constant params& p,
                                    thread float& z0, thread float& z1) {
    const float tile_scale = max(p.zparams.z, 0.0001);
    const float x0 = p.tile_rect.x - p.tile_sprite.x;
    const float y0 = p.tile_rect.y - p.tile_sprite.y;
    const float x1 = p.tile_rect.z - p.tile_sprite.x;
    const float y1 = p.tile_rect.w - p.tile_sprite.y;
    if (!(x0 < x1 && y0 < y1))
        return false;
    if (!tile_clip_z_axis(ray_cam_xy.x, p.cam_tile.x, x0, x1,
                          tile_scale, z0, z1))
        return false;
    return tile_clip_z_axis(ray_cam_xy.y, p.cam_tile.y, y0, y1,
                            tile_scale, z0, z1);
}
static float tile_hierarchy_skip_z(float cam_z, float z1, float base_step,
                                   float2 ray_cam_xy,
                                   texture2d<float> height_minmax_tex,
                                   constant params& p) {
    const int max_mip = min(int(height_minmax_tex.get_num_mip_levels()) - 1, 10);
    if (max_mip <= 0)
        return cam_z;

    // Min/max mips are conservative only while the ray segment stays inside one
    // mip texel. Use the largest eligible cell as a cheap reject. If that cell
    // might contain the surface, stop immediately and let the exact depth path
    // bracket/refine; walking down the pyramid in likely-hit pixels costs more
    // than it saves.
    for (int mip = max_mip; mip >= 1; --mip) {
        const float span = base_step * float(1 << mip);
        const float next_z = min(cam_z + span, z1);
        if (next_z <= cam_z + 0.001)
            continue;

        const float2 uv0 = tile_uv_for_cam_z(cam_z, ray_cam_xy, p);
        const float2 uv1 = tile_uv_for_cam_z(next_z, ray_cam_xy, p);
        if (tile_segment_outside_unit(uv0, uv1))
            return next_z;
        if (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0 ||
            uv1.x < 0.0 || uv1.x > 1.0 || uv1.y < 0.0 || uv1.y > 1.0)
            continue;

        const uint umip = uint(mip);
        const uint2 c0 = tile_mip_texel(uv0, height_minmax_tex, umip);
        const uint2 c1 = tile_mip_texel(uv1, height_minmax_tex, umip);
        if (c0.x != c1.x || c0.y != c1.y)
            continue;

        const float2 mm = height_minmax_tex.read(c0, umip).rg;
        if (!tile_minmax_valid(mm))
            return next_z;

        float surf_min = p.cam_tile.z + min(mm.x, mm.y) * p.cam_tile.w;
        float surf_max = p.cam_tile.z + max(mm.x, mm.y) * p.cam_tile.w;
        if (surf_min > surf_max) {
            float tmp = surf_min;
            surf_min = surf_max;
            surf_max = tmp;
        }
        if (surf_max < cam_z || surf_min > next_z)
            return next_z;

        return cam_z;
    }
    return cam_z;
}
static float4 tile_relief_intersect(float2 frag_px,
                                    texture2d<float> depth_tex,
                                    texture2d<float> height_minmax_tex,
                                    constant params& p) {
    const float zoom = max(p.zparams.w, 0.0001);
    const float2 ray_cam_xy = (frag_px - p.proj.xy) / max(p.camera.z * zoom, 1.0);
    float z0 = p.cam_tile.z + min(p.filter.z, p.filter.w) * p.cam_tile.w;
    float z1 = p.cam_tile.z + max(p.filter.z, p.filter.w) * p.cam_tile.w;
    if (z1 < z0) { float tmp = z0; z0 = z1; z1 = tmp; }
    z0 = max(z0, 1.0);
    z1 = max(z1, z0 + 1.0);
    if (!tile_clip_z_to_coverage(ray_cam_xy, p, z0, z1))
        return float4(0.0);
    z0 = max(z0, 1.0);
    if (z1 <= z0 + 0.001)
        return float4(0.0);

    // This is relief/SPOM-style heightfield intersection, not the expensive
    // reference volume search. The hierarchy skips spans that cannot intersect
    // the heightfield; the local coarse pass brackets the frontmost crossing in
    // tile space, and the binary pass gives pixel-stable UVs.
    int coarse_steps = clamp(int(p.raycast.x), 4, 128);
    int refine_steps = clamp(int(p.raycast.y), 0, 8);
    float base_step = (z1 - z0) / float(coarse_steps);
    bool prev_valid = false;
    float prev_z = z0;
    float prev_diff = 0.0;
    float2 prev_uv = float2(0.0);
    float cam_z = z0;

    for (int i = 0; i < 128; ++i) {
        if (cam_z > z1 + 0.001) break;

        float skip_z = tile_hierarchy_skip_z(cam_z, z1, base_step, ray_cam_xy,
                                             height_minmax_tex, p);
        if (skip_z > cam_z + 0.001) {
            prev_valid = false;
            cam_z = skip_z;
            continue;
        }

        float4 s = tile_surface_at_cam_z(cam_z, ray_cam_xy, depth_tex, p);
        if (s.x < 0.5) {
            prev_valid = false;
            cam_z += base_step;
            continue;
        }

        float2 uv = s.zw;
        float diff = s.y - cam_z;
        if (prev_valid && prev_diff >= 0.0 && diff <= 0.0) {
            float lo_z = prev_z;
            float hi_z = cam_z;
            float2 lo_uv = prev_uv;
            float2 hi_uv = uv;
            bool refine_ok = true;
            for (int r = 0; r < 8; ++r) {
                if (r >= refine_steps) break;
                float mid_z = 0.5 * (lo_z + hi_z);
                float4 ms = tile_surface_at_cam_z(mid_z, ray_cam_xy, depth_tex, p);
                if (ms.x < 0.5) {
                    refine_ok = false;
                    break;
                }
                float mid_diff = ms.y - mid_z;
                if (mid_diff >= 0.0) {
                    lo_z = mid_z;
                    lo_uv = ms.zw;
                } else {
                    hi_z = mid_z;
                    hi_uv = ms.zw;
                }
            }
            if (!refine_ok)
                return float4(0.0);
            float2 hit_uv = 0.5 * (lo_uv + hi_uv);
            bool valid_uv = hit_uv.x >= 0.0 && hit_uv.x <= 1.0 &&
                            hit_uv.y >= 0.0 && hit_uv.y <= 1.0;
            return valid_uv ? float4(1.0, 0.0, hit_uv.x, hit_uv.y) : float4(0.0);
        }

        prev_valid = true;
        prev_z = cam_z;
        prev_diff = diff;
        prev_uv = uv;
        cam_z += base_step;
    }

    return float4(0.0);
}
fragment fs_out _main(vs_out in [[stage_in]],
                      texture2d<float> color_tex [[texture(0)]],
                      texture2d<float> depth_tex [[texture(1)]],
                      texture2d<float> height_minmax_tex [[texture(2)]],
                      sampler smp              [[sampler(0)]],
                      constant params& p       [[buffer(0)]]) {
    fs_out o;
    float2 hit_uv = in.uv;
    const int projection_mode = int(round(p.raycast.w));
    const bool projected_heightfield = p.camera.w > 0.5 && projection_mode > 0;
    const bool relief_projected = p.camera.w > 0.5 && projection_mode >= 2;
    const bool volume_reference = p.camera.w > 0.5 && projection_mode == 1;
    const int debug_mode = int(round(p.debug.x));
    if (projected_heightfield && (debug_mode == 1 || debug_mode == 3)) {
        const float zraw_mid = 0.5 * (p.filter.z + p.filter.w);
        const float d = clamp(p.zparams.x + zraw_mid * p.zparams.y, 0.0, 1.0);
        float3 tint = p.debug.yzw;
        if (debug_mode == 3) {
            // Wire debug submits thin screen-space quads along the clipped
            // proxy boundary. The shader fills those quads directly; using
            // triangle barycentrics here would expose the triangulation fan,
            // not the proxy shape.
            tint = mix(float3(1.0), tint, 0.35);
        } else {
            const float2 cell = floor(in.pos.xy / 12.0);
            const float check = fmod(cell.x + cell.y, 2.0);
            tint *= check > 0.5 ? 1.0 : 0.45;
        }
        o.albedo  = float4(tint, 1.0);
        o.normal  = float4(0.5, 0.5, 1.0, 1.0);
        o.scene_z = float4(d, 0.0, 0.0, 1.0);
        o.obj_id  = p.obj_id;
        o.depth   = d;
        return o;
    }
    bool hit_valid_depth = !projected_heightfield;
    if (relief_projected) {
        float4 relief_hit = tile_relief_intersect(in.pos.xy, depth_tex, height_minmax_tex, p);
        if (relief_hit.x < 0.5)
            discard_fragment();
        hit_uv = relief_hit.zw;
        hit_valid_depth = tile_depth_valid(hit_uv, depth_tex);
        if (!hit_valid_depth) discard_fragment();
    } else if (volume_reference) {
        const float2 frag_px = in.pos.xy;
        const float zoom = max(p.zparams.w, 0.0001);
        const float2 ray_cam_xy = (frag_px - p.proj.xy) / max(p.camera.z * zoom, 1.0);
        float z0 = p.cam_tile.z + min(p.filter.z, p.filter.w) * p.cam_tile.w;
        float z1 = p.cam_tile.z + max(p.filter.z, p.filter.w) * p.cam_tile.w;
        if (z1 < z0) { float tmp = z0; z0 = z1; z1 = tmp; }
        z0 = max(z0, 1.0);
        z1 = max(z1, z0 + 1.0);
        if (!tile_clip_z_to_coverage(ray_cam_xy, p, z0, z1))
            discard_fragment();
        z0 = max(z0, 1.0);
        if (z1 <= z0 + 0.001)
            discard_fragment();

        bool found = false;
        float best_z = z0;
        float2 best_uv = float2(0.0);
        float prev_z = z0;
        float prev_diff = 0.0;
        float2 prev_uv = float2(0.0);
        bool prev_valid = false;
        int valid_run = 0;
        const float tile_scale = max(p.zparams.z, 0.0001);

        int volume_steps = clamp(int(p.raycast.x), 4, 128);
        int refine_steps = clamp(int(p.raycast.y), 0, 8);
        for (int i = 0; i < 128; ++i) {
            if (i >= volume_steps) break;
            float cam_z = mix(z0, z1, float(i) / float(volume_steps - 1));
            float2 cam_xy = ray_cam_xy * cam_z;
            float sx = (cam_xy.x - p.cam_tile.x) / tile_scale;
            float sy = (cam_xy.y - p.cam_tile.y) / tile_scale;
            float2 uv = float2((sx + p.tile_sprite.x) / max(p.tile_sprite.z, 1.0),
                               (sy + p.tile_sprite.y) / max(p.tile_sprite.w, 1.0));
            if (!tile_depth_valid(uv, depth_tex)) {
                prev_valid = false;
                valid_run = 0;
                continue;
            }
            float zraw_surf = tile_depth_read(uv, depth_tex, smp);
            float surf_z = p.cam_tile.z + zraw_surf * p.cam_tile.w;
            float diff = surf_z - cam_z;
            if (prev_valid && valid_run >= 2 && prev_diff >= 0.0 && diff <= 0.0) {
                float lo_z = prev_z;
                float hi_z = cam_z;
                float2 lo_uv = prev_uv;
                float2 hi_uv = uv;
                bool refine_ok = true;
                // Refine only within the already valid front-surface bracket.
                // The alpha boundary is not geometry; invalid samples never
                // create a bracket, so tile sides stay non-renderable.
                for (int r = 0; r < 8; ++r) {
                    if (r >= refine_steps) break;
                    float mid_z = 0.5 * (lo_z + hi_z);
                    float2 mid_xy = ray_cam_xy * mid_z;
                    float mid_sx = (mid_xy.x - p.cam_tile.x) / tile_scale;
                    float mid_sy = (mid_xy.y - p.cam_tile.y) / tile_scale;
                    float2 mid_uv = float2((mid_sx + p.tile_sprite.x) / max(p.tile_sprite.z, 1.0),
                                           (mid_sy + p.tile_sprite.y) / max(p.tile_sprite.w, 1.0));
                    if (!tile_depth_valid(mid_uv, depth_tex)) {
                        // Leaving valid depth means this bracket is crossing
                        // the tile silhouette, not the front surface.
                        refine_ok = false;
                        break;
                    }
                    float mid_raw = tile_depth_read(mid_uv, depth_tex, smp);
                    float mid_surface_z = p.cam_tile.z + mid_raw * p.cam_tile.w;
                    float mid_diff = mid_surface_z - mid_z;
                    if (mid_diff >= 0.0) {
                        lo_z = mid_z;
                        lo_uv = mid_uv;
                    } else {
                        hi_z = mid_z;
                        hi_uv = mid_uv;
                    }
                }
                if (refine_ok) {
                    best_z = 0.5 * (lo_z + hi_z);
                    best_uv = 0.5 * (lo_uv + hi_uv);
                    found = true;
                    break;
                }
            }
            prev_valid = true;
            ++valid_run;
            prev_z = cam_z;
            prev_diff = diff;
            prev_uv = uv;
        }
        (void)prev_uv;
        if (!found || best_uv.x < 0.0 || best_uv.x > 1.0 ||
            best_uv.y < 0.0 || best_uv.y > 1.0)
            discard_fragment();
        hit_uv = best_uv;
        hit_valid_depth = tile_depth_valid(hit_uv, depth_tex);
        if (!hit_valid_depth) discard_fragment();
    }

    float4 c = color_tex.sample(smp, hit_uv);
    const float albedo_pad = max(p.raycast.z, 0.0);
    const float2 gbuf_size = max(p.proj.zw, float2(1.0));
    const bool present_albedo =
        in.pos.x >= albedo_pad && in.pos.x < (gbuf_size.x - albedo_pad) &&
        in.pos.y >= albedo_pad && in.pos.y < (gbuf_size.y - albedo_pad);
    if (p.camera.w <= 0.5 && c.a < 0.01) discard_fragment();
    if (!projected_heightfield && p.camera.w > 0.5 && c.a < 0.01) discard_fragment();
    // Flat sprites have valid zero depth; their decoded alpha above is coverage.
    if (projected_heightfield) c.a = 1.0;
    if (p.debug.x > 1.5) {
        c.rgb = hit_valid_depth ? float3(0.1, 0.2, 1.0) : float3(0.0, 1.0, 0.1);
        c.a = 1.0;
    } else if (p.debug.x > 0.5) {
        c.rgb = p.debug.yzw;
        c.a = 1.0;
    }
    float zraw = tile_depth_read(hit_uv, depth_tex, smp);
    float d    = p.zparams.x + zraw * p.zparams.y;
    if (d < 0.0 || d > 1.0) discard_fragment();
    float  nr  = max(p.filter.x, 0.5);
    float  thr = max(p.filter.y, 0.0);
    float  tw  = float(depth_tex.get_width());
    float  th  = float(depth_tex.get_height());
    float2 tex = float2(nr / max(tw, 1.0), nr / max(th, 1.0));
    float  sx  = hit_uv.x * p.tile_sprite.z - p.tile_sprite.x;
    float  sy  = hit_uv.y * p.tile_sprite.w - p.tile_sprite.y;
    float  zl  = tile_depth_read(hit_uv - float2(tex.x, 0.0), depth_tex, smp);
    float  zrs = tile_depth_read(hit_uv + float2(tex.x, 0.0), depth_tex, smp);
    float  zt  = tile_depth_read(hit_uv - float2(0.0, tex.y), depth_tex, smp);
    float  zb  = tile_depth_read(hit_uv + float2(0.0, tex.y), depth_tex, smp);
    if (fabs(zl  - zraw) > thr + EDGE_EPS) zl  = zraw;
    if (fabs(zrs - zraw) > thr + EDGE_EPS) zrs = zraw;
    if (fabs(zt  - zraw) > thr + EDGE_EPS) zt  = zraw;
    if (fabs(zb  - zraw) > thr + EDGE_EPS) zb  = zraw;
    float  dzl = zl  * p.tile_root.w;
    float  dzr = zrs * p.tile_root.w;
    float  dzt = zt  * p.tile_root.w;
    float  dzb = zb  * p.tile_root.w;
    float  nrx = nr;
    float  nry = nr;
    float  lz_l = (-dzl - 2.0 * sy * ISO_COS30) / ISO_WZ_DENOM;
    float  su_l = 2.0 * (sy + lz_l * ISO_COS30);
    float3 Wl = float3(p.tile_root.x + 0.5 * (su_l + (sx - nrx)),
                      p.tile_root.y + 0.5 * (su_l - (sx - nrx)),
                      p.tile_root.z + lz_l);
    float  lz_r = (-dzr - 2.0 * sy * ISO_COS30) / ISO_WZ_DENOM;
    float  su_r = 2.0 * (sy + lz_r * ISO_COS30);
    float3 Wr = float3(p.tile_root.x + 0.5 * (su_r + (sx + nrx)),
                      p.tile_root.y + 0.5 * (su_r - (sx + nrx)),
                      p.tile_root.z + lz_r);
    float  lz_t = (-dzt - 2.0 * (sy - nry) * ISO_COS30) / ISO_WZ_DENOM;
    float  su_t = 2.0 * ((sy - nry) + lz_t * ISO_COS30);
    float3 Wt = float3(p.tile_root.x + 0.5 * (su_t + sx),
                      p.tile_root.y + 0.5 * (su_t - sx),
                      p.tile_root.z + lz_t);
    float  lz_b = (-dzb - 2.0 * (sy + nry) * ISO_COS30) / ISO_WZ_DENOM;
    float  su_b = 2.0 * ((sy + nry) + lz_b * ISO_COS30);
    float3 Wb = float3(p.tile_root.x + 0.5 * (su_b + sx),
                      p.tile_root.y + 0.5 * (su_b - sx),
                      p.tile_root.z + lz_b);
    float3 t_u = Wr - Wl;
    float3 t_v = Wb - Wt;
    float3 N   = normalize(cross(t_v, t_u));
    float3 Vc  = normalize(float3(ISO_COS30, ISO_COS30, 1.0));
    if (dot(N, Vc) < 0.0) N = -N;
    if (!present_albedo) c = float4(0.0);
    o.albedo  = c;
    o.normal  = float4(N * 0.5 + 0.5, 1.0);
    o.scene_z = float4(d, 0.0, 0.0, 1.0);
    o.obj_id  = p.obj_id;
    o.depth   = d;
    return o;
}
)MSL";
