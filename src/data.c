/* Deterministic content generation and the original name tables.
 *
 * This module owns the Super Kilix Level Format (SKLF): an original, compact
 * representation of a vault as a 4-field header, a column-sorted OBJECT stream
 * layered over a default terrace floor, and a parallel camera-relative SPAWN
 * stream.  Authors write the human-readable macro-DSL face (arrays of SkObj /
 * SkSpawn with absolute columns); level_build() encodes them to the packed byte
 * layout (§13.3/§13.4-style records, our own opcodes and encoding, sharing no
 * bytes with any prior game) and then decodes that stream with a single forward
 * cursor into the runtime tile grid — so the byte format is genuinely exercised
 * on every build.  level_build is a PURE function of the level index: two calls
 * are memcmp-identical because every step is deterministic integer arithmetic
 * over const inputs.
 *
 * The teaching order of the one authored district-1 level (FIRST TERRACE) is the
 * studied introduce -> develop -> twist -> conclude heuristic; every column
 * position, count, and object is original Kilix geometry, designed around the
 * auto-mountable-wall behaviour of the M2 collision resolver. */
#include "super_kilix.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- name tables */

static const char *const district_names[DISTRICTS] = {
    "RUST FLATS", "COIL WARRENS", "NULL TIDE", "RAIL SPIRES",
    "THE CINDERWORKS", "DROWNED CELLS", "EMBER CONDUITS", "THE WARDEN VAULT"
};

static const char *const tile_names[TILE_KIND_COUNT] = {
    "void", "terrace", "subsurface", "bedrock", "scrap block", "cache node",
    "spent block", "conduit", "grate ledge", "riser rail", "iris door", "thorns"
};

/* Machine roster ids (level-grammar.md §13.4); M3 stores spawns as data only. */
static const char *const machine_names[] = {
    "Slag-Treader", "Carapod", "Carapod-Kite", "Dreadpod", "Vent-Maw",
    "Siphon-Squid", "Fin-Drifter", "Riveter", "Hovercaster", "Burr",
    "Cinder-Leaper", "Vault Guardian", "group", "The Overseer"
};

const char *tile_name(int tile)
{
    return tile >= 0 && tile < TILE_KIND_COUNT ? tile_names[tile] : "cell";
}

const char *machine_name(int kind)
{
    int count = (int)(sizeof machine_names / sizeof machine_names[0]);
    return kind >= 0 && kind < count ? machine_names[kind] : "machine";
}

const char *district_name(int district)
{
    return district >= 1 && district <= DISTRICTS
         ? district_names[district - 1] : "district";
}

/* --------------------------------------------------------- SKLF authoring face */

/* Opcodes: our own numbering (level-grammar.md §13.3 op table order).  M3
 * implements the RUST-FLATS subset; the rest are reserved so the enum stays a
 * stable contract for later districts. */
enum {
    OP_GROUND, OP_GAP, OP_SLAB, OP_BRICKS, OP_LEDGE, OP_STAIR, OP_CONDUIT,
    OP_RAIL, OP_CACHE, OP_HIDDEN, OP_SPRING, OP_LIFT, OP_SWITCH, OP_GATE,
    OP_THORN, OP_EMBER, OP_PLASMA, OP_MINE, OP_TENDRIL, OP_RISER, OP_SEAL,
    OP_LINK, OP_SKIP, OP_DECOR
};

/* Cache / hidden content codes (state-dependent contents, cast.md §4). */
enum { CN_MOTE, CN_MULTI, CN_POWER, CN_SHELL };

/* Conduit flag bits (param). */
enum { CF_ENTERABLE = 1, CF_VENT = 2 };

/* Machine ids used by the M3 spawn streams. */
enum { M_TREADER = 0, M_CARAPOD = 1, M_GROUP = 12 };

typedef struct { uint8_t op, col, row, param; } SkObj;   /* macro-DSL, abs column */
typedef struct { uint8_t col, row, kind, param; } SkSpawn;

typedef struct {
    uint8_t     district;      /* 1..8 */
    uint8_t     floor_pattern; /* terrace fill selector */
    uint8_t     charge;        /* 1=300 2=400 3=500 units */
    uint8_t     entry_mode;    /* 0 ground .. */
    uint8_t     scenery;       /* backdrop/parallax id */
    uint16_t    length;        /* columns */
    const SkObj  *objs;   int obj_count;
    const SkSpawn *spawns; int spawn_count;
} AuthoredLevel;

/* -------------------------------------------- authored district-1, level 1 */

/* FIRST TERRACE — the "thesis statement".  Reward-only cache first (col 8), the
 * first forced machine (col 14), an unmissable power cache (col 21) placed to
 * bounce toward the player off conduit #1, four conduits escalating 3->4->4->5
 * that train jump height monotonically (the fourth is the first enterable one —
 * "conduits can be entered" taught only after three "conduits are walls"), the
 * first hidden spare-shell (col 74) and first void gap (col 80) after ~1200 px
 * of consequence-free practice, a Carapod that turns at ledges patrolling a
 * bedrock terrace, a scrap-block row, a one-way grate, a twist pairing stairs
 * with a gap, and the riser at col 176.  All columns original Kilix geometry. */
static const SkObj Z1L1_OBJ[] = {
    {OP_GROUND,   0, 12, 184},            /* explicit opening/continuous floor  */
    {OP_CACHE,    8,  8, CN_MOTE},        /* beat 1: pure reward                 */
    {OP_SLAB,    20,  8, 4},              /* beat 3: 4-wide bedrock terrace      */
    {OP_CACHE,   21,  8, CN_POWER},       /*   power cache, first & near-central */
    {OP_CACHE,   22,  6, CN_MOTE},        /*   high mote                         */
    {OP_CACHE,   24,  8, CN_MOTE},        /*   terrace mote                      */
    {OP_CONDUIT, 28,  9, 0},              /* beat 4: wall #1, height 3           */
    {OP_CONDUIT, 40,  8, 0},              /*   wall #2, height 4                 */
    {OP_CONDUIT, 52,  8, 0},              /*   wall #3, height 4                 */
    {OP_CONDUIT, 66,  7, CF_ENTERABLE},   /*   wall #4, height 5, first enterable */
    {OP_HIDDEN,  74,  4, CN_SHELL},       /* beat 5: hidden spare shell          */
    {OP_GAP,     80,  0, 4},              /*   first void gap (after ~1264 px)   */
    {OP_SLAB,    88,  8, 8},              /* beat 6: bedrock terrace for Carapod */
    {OP_BRICKS, 104,  8, 7},              /*   scrap-block row                   */
    {OP_CACHE,  107,  8, CN_MULTI},       /*   multi-mote cache in the bricks    */
    {OP_LEDGE,  118,  6, 7},              /*   one-way grate above a shallow drop */
    {OP_STAIR,  130, 12, (3u << 1) | 0u}, /* beat 7: ascending stair (3 up)      */
    {OP_GAP,    140,  0, 3},              /*   twist: gap flanked by the stairs  */
    {OP_STAIR,  148, 12, (3u << 1) | 0u}, /*   ascending stair (3 up)            */
    {OP_STAIR,  166, 12, (2u << 1) | 0u}, /* beat 8: short rise to the goal      */
    {OP_RISER,  176, 12, 3},              /*   conclude: riser rail (3 tall)     */
};

static const SkSpawn Z1L1_ENEMY[] = {
    {14, 11, M_TREADER, 0},   /* beat 2: the first forced jump               */
    {46, 11, M_GROUP,   1},   /* a Slag-Treader pair between conduits #1/#2  */
    {88,  7, M_CARAPOD, 0},   /* turns at ledges, patrols the bedrock terrace */
    {132, 11, M_TREADER, 0},  /* pressures the twist staircase               */
};

/* ------------------------------------------------- procedural districts 1-8 */

/* A stateless integer hash — used only to vary generated geometry per index;
 * never touches G.rng (cosmetic/authoring randomness stays out of the sim). */
static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Fill an AuthoredLevel for a generated (non-authored) vault: a continuous
 * terrace with a few clearly-jumpable conduits and narrow gaps, a couple of
 * caches, one or two spawns, and a riser at the end.  Every obstacle stays
 * inside the core-verb reach so level_validate passes on walk/run/jump alone,
 * and the per-index variation keeps every topology signature distinct. */
static void generate_level(int index, AuthoredLevel *out,
                           SkObj *objs, int obj_cap,
                           SkSpawn *spawns, int spawn_cap)
{
    int district = index / VAULTS_PER_DISTRICT + 1;
    int slot     = index % VAULTS_PER_DISTRICT;
    uint32_t h   = mix32((uint32_t)index * 2654435761u + 0x51ed2701u);

    int length = 104 + (int)(h % 72u) + slot * 6;   /* 104..~184 */
    if (length > 200) length = 200;

    out->district      = (uint8_t)district;
    out->floor_pattern = (uint8_t)((district - 1) & 31);
    out->charge        = (uint8_t)(slot >= 2 ? 1 : 2);   /* ascent/gate tighter */
    out->entry_mode    = 0;
    out->scenery       = (uint8_t)((district - 1) & 7);
    out->length        = (uint16_t)length;

    int n = 0;
    objs[n++] = (SkObj){OP_GROUND, 0, 12, (uint8_t)(length & 0xFF)};

    /* Obstacles spaced across the middle third; heights 2..3, gaps 2..3. */
    int cursor = 20;
    int step   = 14 + (int)((h >> 3) % 6u);
    while (cursor < length - 20 && n < obj_cap - 2) {
        uint32_t r = mix32(h + (uint32_t)cursor * 131u);
        int kind = (int)(r % 3u);
        if (kind == 0) {                        /* conduit (jump-height gate) */
            int mouth = 12 - (2 + (int)((r >> 4) % 2u));   /* height 2..3 */
            objs[n++] = (SkObj){OP_CONDUIT, (uint8_t)cursor,
                                (uint8_t)mouth, 0};
        } else if (kind == 1) {                 /* narrow void gap */
            int w = 2 + (int)((r >> 4) % 2u);              /* width 2..3 */
            objs[n++] = (SkObj){OP_GAP, (uint8_t)cursor, 0, (uint8_t)w};
        } else {                                /* reward cache above the floor */
            objs[n++] = (SkObj){OP_CACHE, (uint8_t)cursor, 8,
                                (uint8_t)((r >> 4) & 1u ? CN_MOTE : CN_MULTI)};
        }
        cursor += step + (int)(r % 5u);
    }

    objs[n++] = (SkObj){OP_RISER, (uint8_t)(length - 8), 12, 3};
    out->objs = objs;
    out->obj_count = n;

    int m = 0;
    int spawns_wanted = 1 + (district + slot) % 3;   /* 1..3 */
    for (int i = 0; i < spawns_wanted && m < spawn_cap; i++) {
        uint32_t r = mix32(h + (uint32_t)i * 97u + 0x1234u);
        int col = 24 + (int)(r % (uint32_t)(length > 60 ? length - 44 : 16));
        int mk  = district <= 1 ? M_TREADER : (int)(r % 2u);
        spawns[m++] = (SkSpawn){(uint8_t)col, 11, (uint8_t)mk, 0};
    }
    out->spawns = spawns;
    out->spawn_count = m;
}

/* Select the authored macro-DSL for a level, or synthesize one into the caller's
 * scratch buffers.  Pure: identical index -> identical AuthoredLevel. */
static void author_level(int index, AuthoredLevel *out,
                         SkObj *obj_buf, int obj_cap,
                         SkSpawn *spawn_buf, int spawn_cap)
{
    if (index == 0) {
        out->district      = 1;
        out->floor_pattern = 0;
        out->charge        = 2;              /* 400 units */
        out->entry_mode    = 0;
        out->scenery       = 0;
        out->length        = 184;
        out->objs          = Z1L1_OBJ;
        out->obj_count     = (int)(sizeof Z1L1_OBJ / sizeof Z1L1_OBJ[0]);
        out->spawns        = Z1L1_ENEMY;
        out->spawn_count   = (int)(sizeof Z1L1_ENEMY / sizeof Z1L1_ENEMY[0]);
        (void)obj_buf; (void)obj_cap; (void)spawn_buf; (void)spawn_cap;
        return;
    }
    generate_level(index, out, obj_buf, obj_cap, spawn_buf, spawn_cap);
}

/* --------------------------------------------------- SKLF byte-stream codec */

#define SKLF_OBJ_CAP    768
#define SKLF_SPAWN_CAP  160

/* Stable insertion-sort of the object list by ascending column (the single
 * forward cursor the packed stream is parsed with requires monotone columns). */
static void sort_objs(SkObj *a, int n)
{
    for (int i = 1; i < n; i++) {
        SkObj key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].col > key.col) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
}

static void sort_spawns(SkSpawn *a, int n)
{
    for (int i = 1; i < n; i++) {
        SkSpawn key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].col > key.col) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
}

/* Encode sorted objects to the packed 3-byte-record stream:
 *   byte0 = column delta from the previous object (0xFF terminates)
 *   byte1 = (op << 3) | p_hi        byte2 = (row << 4) | p_lo
 *   param = (p_hi << 3) | p_lo, in 0..62; the sentinel 63 means "a full 8-bit
 *   value follows in an extra byte" (so GROUND can span > 62 columns). */
static int encode_objects(const SkObj *objs, int n, uint8_t *buf, int cap)
{
    int len = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        int delta = (int)objs[i].col - prev;
        if (delta < 0 || delta > 254) return -1;
        int param = objs[i].param, p_field = param, follow = -1;
        if (param > 62) { p_field = 63; follow = param; }
        if (len + 3 + (follow >= 0 ? 1 : 0) + 1 > cap) return -1;
        buf[len++] = (uint8_t)delta;
        buf[len++] = (uint8_t)((objs[i].op << 3) | ((p_field >> 3) & 7));
        buf[len++] = (uint8_t)((objs[i].row << 4) | (p_field & 7));
        if (follow >= 0) buf[len++] = (uint8_t)follow;
        prev = objs[i].col;
    }
    buf[len++] = 0xFF;
    return len;
}

/* Spawn stream: 2-byte records (delta, machine id), 0xFE terminates. */
static int encode_spawns(const SkSpawn *sp, int n, uint8_t *buf, int cap)
{
    int len = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        int delta = (int)sp[i].col - prev;
        if (delta < 0 || delta > 253) return -1;
        if (len + 2 + 1 > cap) return -1;
        buf[len++] = (uint8_t)delta;
        buf[len++] = (uint8_t)(((sp[i].kind & 0x0F) << 4) |
                               ((sp[i].row & 0x0F)));
        prev = sp[i].col;
    }
    buf[len++] = 0xFE;
    return len;
}

/* --------------------------------------------------------- stream -> grid */

static void set_cell(VaultData *v, int col, int row, uint8_t tile)
{
    if (col < 0 || col >= v->cols || row < 0 || row >= v->rows) return;
    v->tiles[row][col] = tile;
}

static void fill_run(VaultData *v, int col, int row, int len, uint8_t tile)
{
    for (int c = col; c < col + len; c++) set_cell(v, c, row, tile);
}

/* Apply one decoded object at (col,row) with param, stamping tiles / recording
 * goal anchors into the vault.  baseline = v->rows - 1 (the ground row). */
static void stamp_object(VaultData *v, int op, int col, int row, int param)
{
    int baseline = v->rows - 1;
    switch (op) {
    case OP_GROUND:
        fill_run(v, col, baseline, param, T_HULL);
        break;
    case OP_GAP:
        for (int c = col; c < col + param; c++) set_cell(v, c, baseline, T_EMPTY);
        break;
    case OP_SLAB:
        fill_run(v, col, row, param, T_BEDROCK);
        break;
    case OP_BRICKS:
        fill_run(v, col, row, param, T_BRICK);
        break;
    case OP_LEDGE:
        fill_run(v, col, row, param, T_LEDGE);
        break;
    case OP_STAIR: {
        int steps = (param >> 1) & 0x1F;
        int down  = param & 1;
        for (int i = 0; i < steps; i++) {
            int height = down ? steps - i : i + 1;   /* rising / falling run */
            int sc = col + i;
            for (int r = baseline; r > baseline - height; r--)
                set_cell(v, sc, r, T_BEDROCK);
        }
        break;
    }
    case OP_CONDUIT:
        /* A capped vent pipe standing on the floor: mouth at `row`, solid down
         * to just above the baseline (height = baseline - row). */
        for (int r = row; r < baseline; r++) set_cell(v, col, r, T_CONDUIT);
        break;
    case OP_CACHE:
        set_cell(v, col, row, T_CACHE);
        break;
    case OP_HIDDEN:
        /* Invisible until bumped (M4); no solid cell is placed at M3 so it can
         * never block the route, but the op is a first-class part of the format. */
        break;
    case OP_THORN:
        fill_run(v, col, row, param, T_THORN);
        break;
    case OP_RISER: {
        int top = baseline - param;            /* rail height in rows */
        if (top < 0) top = 0;
        v->riser_col = col;
        v->riser_row = top;
        for (int r = top; r < baseline; r++) set_cell(v, col, r, T_RISER);
        /* The passive iris door sits a few columns past the riser. */
        int ic = col + 6;
        if (ic > v->length - 2) ic = v->length - 2;
        if (ic <= col) ic = col + 1;
        v->exit_col = ic;
        v->exit_row = baseline - 1;
        set_cell(v, ic, baseline - 1, T_IRIS);
        break;
    }
    case OP_SEAL:
        v->seal_col = col;
        v->seal_row = row;
        break;
    default:
        /* Reserved / not-yet-implemented op (LINK, SKIP, DECOR, LIFT, ...):
         * a no-op in the M3 lowering, but a valid record in the stream. */
        break;
    }
}

/* Decode the packed object stream with a single forward cursor. */
static void decode_objects(const uint8_t *buf, int len, VaultData *v)
{
    int i = 0, col = 0;
    while (i < len) {
        uint8_t delta = buf[i++];
        if (delta == 0xFF) return;
        col += delta;
        if (i + 1 >= len) return;
        uint8_t b1 = buf[i++], b2 = buf[i++];
        int op    = b1 >> 3;
        int p_hi  = b1 & 7;
        int row   = b2 >> 4;
        int p_lo  = b2 & 7;
        int param = (p_hi << 3) | p_lo;
        if (param == 63 && i < len) param = buf[i++];
        stamp_object(v, op, col, row, param);
    }
}

static void decode_spawns(const uint8_t *buf, int len, VaultData *v)
{
    int i = 0, col = 0;
    v->enemy_count = 0;
    while (i < len) {
        uint8_t delta = buf[i++];
        if (delta == 0xFE) return;
        col += delta;
        if (i >= len) return;
        uint8_t b1 = buf[i++];
        if (v->enemy_count < MAX_ENEMIES && col >= 0 && col < v->cols) {
            v->enemies[v->enemy_count++] = (EnemySpawn){
                (uint8_t)(b1 >> 4), (uint8_t)col, (uint8_t)(b1 & 0x0F), 0
            };
        }
    }
}

/* --------------------------------------------------------- biome / charge */

static int biome_of_district(int district)
{
    switch (district) {
    case 1: case 4: return 0;   /* A hull    */
    case 2:         return 1;   /* B underduct */
    case 3: case 6: return 2;   /* C coolant */
    default:        return 3;   /* D forge   */
    }
}

static uint16_t charge_units(int selector)
{
    switch (selector) { case 1: return 300; case 3: return 500; default: return 400; }
}

/* ------------------------------------------------------------- level_build */

void level_build(int level_index, VaultData *out)
{
    if (level_index < 0) level_index = 0;
    if (level_index >= CAMPAIGN_VAULTS) level_index = CAMPAIGN_VAULTS - 1;

    SkObj   gen_objs[128];
    SkSpawn gen_spawns[MAX_ENEMIES];
    AuthoredLevel a;
    author_level(level_index, &a, gen_objs,
                 (int)(sizeof gen_objs / sizeof gen_objs[0]),
                 gen_spawns, (int)(sizeof gen_spawns / sizeof gen_spawns[0]));

    memset(out, 0, sizeof *out);
    out->rows          = PLAY_ROWS;                 /* standard 13-row vault */
    out->length        = a.length;
    out->cols          = a.length;
    out->district      = a.district;
    out->vault         = level_index % VAULTS_PER_DISTRICT + 1;
    out->biome         = biome_of_district(a.district);
    out->entry_mode    = a.entry_mode;
    out->floor_pattern = a.floor_pattern;
    out->timer_start   = charge_units(a.charge);
    out->spawn_col     = 3;
    out->spawn_row     = out->rows - 2;             /* body row on the floor */
    out->riser_col = out->riser_row = -1;
    out->seal_col  = out->seal_row  = -1;
    out->exit_col  = out->exit_row  = -1;

    int baseline = out->rows - 1;

    /* Default terrain: a continuous terrace floor along the whole length (the
     * "diff against a floor" base the object stream then modifies). */
    for (int c = 0; c < out->cols; c++) out->tiles[baseline][c] = T_HULL;

    /* Encode the authored objects to the packed stream, then rebuild the grid
     * by decoding it — the byte format round-trips on every build. */
    SkObj sorted[128];
    int nobj = a.obj_count;
    if (nobj > (int)(sizeof sorted / sizeof sorted[0]))
        nobj = (int)(sizeof sorted / sizeof sorted[0]);
    memcpy(sorted, a.objs, (size_t)nobj * sizeof(SkObj));
    sort_objs(sorted, nobj);

    uint8_t obuf[SKLF_OBJ_CAP];
    int olen = encode_objects(sorted, nobj, obuf, sizeof obuf);
    if (olen > 0) decode_objects(obuf, olen, out);

    SkSpawn ssorted[MAX_ENEMIES];
    int nsp = a.spawn_count;
    if (nsp > MAX_ENEMIES) nsp = MAX_ENEMIES;
    memcpy(ssorted, a.spawns, (size_t)nsp * sizeof(SkSpawn));
    sort_spawns(ssorted, nsp);
    uint8_t sbuf[SKLF_SPAWN_CAP];
    int slen = encode_spawns(ssorted, nsp, sbuf, sizeof sbuf);
    if (slen > 0) decode_spawns(sbuf, slen, out);

    /* A vault always ends at a goal device; if the authored stream somehow
     * carried neither a riser nor a seal, anchor a fallback riser at the end so
     * the campaign invariant (every vault has an exit) holds by construction. */
    if (out->riser_col < 0 && out->seal_col < 0) {
        int rc = out->cols - 8;
        if (rc < out->spawn_col + 2) rc = out->spawn_col + 2;
        stamp_object(out, OP_RISER, rc, 12, 3);
    }
    if (out->exit_col < 0) {
        out->exit_col = out->cols - 2;
        out->exit_row = baseline - 1;
        set_cell(out, out->exit_col, out->exit_row, T_IRIS);
    }

    snprintf(out->title, sizeof out->title, "%d-%d %s",
             out->district, out->vault, district_name(out->district));
}

/* ---------------------------------------------------------- level_signature */

uint32_t level_signature(const VaultData *v)
{
    uint32_t hash = 2166136261u;
#define FNV(byte) do { hash ^= (uint32_t)(uint8_t)(byte); hash *= 16777619u; } while (0)
    for (int r = 0; r < v->rows; r++)
        for (int c = 0; c < v->cols; c++) FNV(v->tiles[r][c]);
    FNV(v->cols); FNV(v->cols >> 8);
    FNV(v->rows);
    FNV(v->spawn_col); FNV(v->spawn_row);
    FNV(v->riser_col); FNV(v->riser_row);
    FNV(v->exit_col);  FNV(v->exit_row);
    FNV(v->seal_col);
    FNV(v->district);  FNV(v->vault);
    FNV(v->enemy_count);
    for (int i = 0; i < v->enemy_count; i++) {
        FNV(v->enemies[i].kind);
        FNV(v->enemies[i].col);
        FNV(v->enemies[i].row);
    }
#undef FNV
    return hash;
}

int level_enemy_budget(int level_index)
{
    int district = (level_index < 0 ? 0 : level_index) / VAULTS_PER_DISTRICT + 1;
    int budget = 8 + district;                 /* generous concurrent budget */
    return budget > MAX_ENEMIES ? MAX_ENEMIES : budget;
}

/* --------------------------------------------------------------- validation */

static bool cell_solid(const VaultData *v, int col, int row)
{
    if (col < 0 || col >= v->cols || row < 0 || row >= v->rows) return false;
    int t = v->tiles[row][col];
    return t >= T_HULL && t <= T_CONDUIT;       /* the contiguous solid range */
}

/* Conservative core-verb reachability (level-grammar.md §14.5).  The player
 * walks the baseline; an on-floor obstacle is a solid stack rising from the row
 * just above the floor, clearable only if <= MAX_CLEAR tall; a gap is a run of
 * floorless columns, crossable only if <= MAX_GAP wide.  Overhead platforms and
 * one-way ledges leave the body row clear and never block the ground route.
 * MAX_CLEAR/MAX_GAP are sized to the running-jump envelope plus the M2
 * auto-mount affordance; a level failing this cannot ship. */
static bool route_reachable(const VaultData *v, char *err, size_t len)
{
    const int MAX_CLEAR = 5;      /* tallest on-floor wall a run-up clears */
    const int MAX_GAP   = 5;      /* widest void a running jump clears */
    int baseline = v->rows - 1;
    int from = v->spawn_col, to = v->exit_col;
    if (to < from) { int t = from; from = to; to = t; }
    int gap_run = 0;
    for (int c = from; c <= to && c < v->cols; c++) {
        if (!cell_solid(v, c, baseline)) {      /* floorless column: a gap */
            if (++gap_run > MAX_GAP) {
                snprintf(err, len, "gap wider than %d at col %d", MAX_GAP, c);
                return false;
            }
            continue;
        }
        gap_run = 0;
        int height = 0;
        for (int r = baseline - 1; r >= 0 && cell_solid(v, c, r); r--) height++;
        if (height > MAX_CLEAR) {
            snprintf(err, len, "wall %d tall (> %d) at col %d",
                     height, MAX_CLEAR, c);
            return false;
        }
    }
    return true;
}

static bool validate_vault(const VaultData *v, char *err, size_t len)
{
    if (v->rows < 1 || v->rows > VAULT_ROWS ||
        v->cols < 8 || v->cols > VAULT_COLS) {
        snprintf(err, len, "vault extent out of range (%dx%d)", v->cols, v->rows);
        return false;
    }
    if (v->district < 1 || v->district > DISTRICTS ||
        v->vault < 1 || v->vault > VAULTS_PER_DISTRICT) {
        snprintf(err, len, "district/vault label out of range");
        return false;
    }
    if (!memchr(v->title, '\0', sizeof v->title)) {
        snprintf(err, len, "title is not terminated");
        return false;
    }
    for (int r = 0; r < v->rows; r++)
        for (int c = 0; c < v->cols; c++)
            if (v->tiles[r][c] >= TILE_KIND_COUNT) {
                snprintf(err, len, "tile %d,%d has invalid kind %d", c, r,
                         v->tiles[r][c]);
                return false;
            }

    int baseline = v->rows - 1;
    if (v->spawn_col < 1 || v->spawn_col >= v->cols ||
        v->spawn_row < 0 || v->spawn_row >= v->rows) {
        snprintf(err, len, "spawn out of bounds");
        return false;
    }
    if (cell_solid(v, v->spawn_col, v->spawn_row) ||
        !cell_solid(v, v->spawn_col, v->spawn_row + 1)) {
        snprintf(err, len, "entrance is embedded or has no footing");
        return false;
    }
    /* Exactly one goal device, standing clear of solid geometry. */
    if (v->exit_col < 0 || v->exit_col >= v->cols ||
        v->tiles[v->exit_row][v->exit_col] != T_IRIS) {
        snprintf(err, len, "iris exit missing or embedded");
        return false;
    }
    bool has_goal = false;
    if (v->riser_col >= 0) {
        if (v->tiles[v->riser_row][v->riser_col] != T_RISER) {
            snprintf(err, len, "riser objective embedded in solid geometry");
            return false;
        }
        has_goal = true;
    }
    if (v->seal_col >= 0) has_goal = true;
    if (!has_goal) {
        snprintf(err, len, "vault has no riser or seal goal");
        return false;
    }
    /* Floor continuity: the baseline must be intact at the entrance and the
     * exit approach (gaps are permitted only in the interior). */
    if (!cell_solid(v, v->spawn_col, baseline) ||
        !cell_solid(v, v->exit_col, baseline)) {
        snprintf(err, len, "floor discontinuous under entrance or exit");
        return false;
    }
    int budget = 8 + v->district;
    if (budget > MAX_ENEMIES) budget = MAX_ENEMIES;
    if (v->enemy_count < 0 || v->enemy_count > budget) {
        snprintf(err, len, "machine budget %d out of range", v->enemy_count);
        return false;
    }
    return route_reachable(v, err, len);
}

bool level_validate(int level_index, char *err, size_t len)
{
    VaultData v;
    level_build(level_index, &v);
    return validate_vault(&v, err, len);
}

bool level_validate_campaign(char *err, size_t len)
{
    uint32_t sigs[CAMPAIGN_VAULTS];
    for (int i = 0; i < CAMPAIGN_VAULTS; i++) {
        VaultData v;
        level_build(i, &v);
        if (!validate_vault(&v, err, len)) {
            char detail[200];
            snprintf(detail, sizeof detail, "vault %d: %s", i + 1, err);
            snprintf(err, len, "%s", detail);
            return false;
        }
        sigs[i] = level_signature(&v);
        for (int j = 0; j < i; j++)
            if (sigs[j] == sigs[i]) {
                snprintf(err, len, "vault %d duplicates vault %d topology",
                         i + 1, j + 1);
                return false;
            }
    }
    return true;
}
