#pragma once

#include <sokol_gfx.h>

// Apply to the ordinary mesh G-buffer descriptor. Picking runs after
// lighting: retain scene depth and every visual attachment, and update only
// the visible mesh IDs. The mesh shader still rejects transparent texels.
inline void ConfigureMeshPickPipeline(sg_pipeline_desc& pipeline)
{
    for (int i = 0; i < 3; ++i)
        pipeline.colors[i].write_mask = SG_COLORMASK_NONE;
    pipeline.colors[3].write_mask = SG_COLORMASK_RGBA;
    pipeline.colors[3].blend.enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
}
