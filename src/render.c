/* All drawing: the three-canvas pipeline and the primitive wrappers.  render.c
 * is the only module that includes soft_raster.h.  M0 is a safe stub; the real
 * pipeline and draw_kilix arrive at M1. */
#include "super_kilix.h"

#include "soft_raster.h"

bool render_init(int w, int h)
{
    (void)w;
    (void)h;
    return true;
}

bool render_resize(int w, int h)
{
    (void)w;
    (void)h;
    return true;
}

void render_shutdown(void)
{
}

void render_frame(void)
{
    /* M0 draws nothing; the renderer never touches simulation state. */
}

uint8_t *render_fb(void)
{
    return NULL;
}

bool render_dump_ppm(const char *path)
{
    (void)path;
    return false;
}
