/* Deterministic content generation and the original name tables.  M0 ships the
 * name-lookup stubs; vault generation and validators arrive at M3. */
#include "super_kilix.h"

const char *tile_name(int tile)
{
    (void)tile;
    return "cell";
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
