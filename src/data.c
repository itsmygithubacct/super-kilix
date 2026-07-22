/* Deterministic content generation and the original name tables.  M2 ships one
 * hard-coded physics test arena so the collision/camera/autopilot code has
 * geometry to run against; the real per-index vault generator lands at M3. */
#include "super_kilix.h"

#include <string.h>

/* Arena geometry (tiles).  A single roamable stage carrying every collision
 * feature the M2 physics fixtures exercise: a continuous floor (no bottomless
 * pit, so nothing can fall out of the world before the death system exists), a
 * floating shelf to walk off, a one-way grate to drop through, two walls of
 * different height, and a raised block. */
#define ARENA_COLS 48

void vault_build(int level_index, VaultData *out)
{
    (void)level_index;               /* M2: one arena regardless of index */
    memset(out, 0, sizeof *out);
    out->cols = ARENA_COLS;
    out->rows = PLAY_ROWS;           /* 13 rows: baseline is the last row */
    int floor_row = out->rows - 1;   /* row 12 */

    for (int col = 0; col < out->cols; col++)
        out->tiles[floor_row][col] = T_HULL;

    /* A floating shelf (solid) with open air beneath it — walk off its edge. */
    for (int col = 6; col <= 9; col++)
        out->tiles[8][col] = T_HULL;

    /* A one-way grate: land on it from above, rise through it from below. */
    for (int col = 14; col <= 18; col++)
        out->tiles[8][col] = T_LEDGE;

    /* A short (2-tall) wall standing on the floor. */
    out->tiles[floor_row - 1][24] = T_HULL;
    out->tiles[floor_row - 2][24] = T_HULL;

    /* A taller (3-tall) wall. */
    out->tiles[floor_row - 1][34] = T_HULL;
    out->tiles[floor_row - 2][34] = T_HULL;
    out->tiles[floor_row - 3][34] = T_HULL;

    /* A raised block platform near the exit. */
    for (int col = 40; col <= 43; col++)
        out->tiles[floor_row - 3][col] = T_HULL;

    out->spawn_col = 2;
    out->spawn_row = floor_row - 1;      /* body row resting on the floor */
    out->exit_col  = ARENA_COLS - 2;
    out->exit_row  = floor_row - 1;
}

const char *tile_name(int tile)
{
    switch (tile) {
    case T_EMPTY: return "void";
    case T_HULL:  return "hull";
    case T_LEDGE: return "grate";
    default:      return "cell";
    }
}

const char *machine_name(int kind)
{
    (void)kind;
    return "machine";
}

const char *district_name(int district)
{
    (void)district;
    return "district";
}
