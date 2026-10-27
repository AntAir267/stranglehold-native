/* Enhanced options (design point 1): <user>/options.txt, "key = value" lines, # comments.
   Missing keys keep the faithful defaults. --option key=value on the command line overrides
   the file for one run. The in-game Enhanced page (enhanced.c) edits the same file. */
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"

typedef struct { char key[48], value[208]; int dirty; } Opt;
static Opt opts[64];
static int nopts;

static void set(const char *k, const char *v)
{
    for (int i = 0; i < nopts; i++)
        if (!strcasecmp(opts[i].key, k)) {
            snprintf(opts[i].value, sizeof opts[i].value, "%s", v);
            return;
        }
    if (nopts < 64) {
        snprintf(opts[nopts].key, sizeof opts[nopts].key, "%s", k);
        snprintf(opts[nopts].value, sizeof opts[nopts].value, "%s", v);
        nopts++;
    }
}

/* "key = value" -> set; returns 0 if the line has no '=' */
int option_parse(const char *line)
{
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    char k[64], v[256];
    const char *a = line, *b = eq;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    snprintf(k, sizeof k, "%.*s", (int)(b - a), a);
    a = eq + 1;
    while (*a && isspace((unsigned char)*a)) a++;
    b = a + strlen(a);
    while (b > a && isspace((unsigned char)b[-1])) b--;
    snprintf(v, sizeof v, "%.*s", (int)(b - a), a);
    set(k, v);
    return 1;
}

void options_load(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/options.txt", user_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s && !option_parse(s))
            LOG(LOG_ALWAYS, "options.txt: \"%s\" is not key = value", s);
    }
    fclose(f);
}

/* Where a key was found, by the address of the caller's key string (some are asked many times a
   frame): a hint, checked; each thread its own. A key not there stays not there until one is added. */
static __thread struct { const char *key; int idx, n; } hint[32];

const char *option_str(const char *key, const char *def)
{
    unsigned h = (unsigned)((uintptr_t)key >> 3) % 32;
    if (hint[h].key == key) {
        int i = hint[h].idx;
        if (i < 0 ? hint[h].n == nopts : !strcasecmp(opts[i].key, key))
            return i < 0 ? def : opts[i].value;
    }
    int found = -1;
    for (int i = 0; i < nopts && found < 0; i++)
        if (!strcasecmp(opts[i].key, key))
            found = i;
    hint[h].key = key;
    hint[h].idx = found;
    hint[h].n = nopts;
    return found < 0 ? def : opts[found].value;
}

int option_bool(const char *key, int def)
{
    const char *v = option_str(key, NULL);
    if (!v) return def;
    return !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
}

/* option draw_distance as the factor on the game's distances (d3d9.c's fog end, game.c's car and
   city tables): original 1 (the game's fog end 2000/3000/4000 by its VIEW DISTANCE), far 2,
   max 4, or a number from 1 to 4 */
double option_draw_distance(void)
{
    const char *v = option_str("draw_distance", "far");
    double d = !strcasecmp(v, "original") ? 1 : !strcasecmp(v, "far") ? 2 : !strcasecmp(v, "max") ? 4 : atof(v);
    return d < 1 ? 1 : d > 4 ? 4 : d;
}

/* option sun (decomp/shaders.c sh_light_from, shadowmap.c 0x563610): true (the default: the mode's
   own light direction, at least SUN_MIN_ELEVATION up, for the shading and the shadow map) or
   original (the game's: the shading's light turned to about 81 degrees up, the shadows' to 54) */
int option_sun_true(void)
{
    return strcasecmp(option_str("sun", "true"), "original") != 0;
}

/* option shadow_distance as the factor on the shadow map's ranges (decomp/shadowmap.c 0x562470):
   original 1 (the game's: shadows up to 1500 ahead), far 2, max 3 */
double option_shadow_distance(void)
{
    const char *v = option_str("shadow_distance", "far");
    return !strcasecmp(v, "original") ? 1 : !strcasecmp(v, "max") ? 3 : 2;
}

/* every option in force, for the offline checker's recordings (devrec.c) */
int option_count(void) { return nopts; }
const char *option_key(int i) { return i < nopts ? opts[i].key : ""; }
const char *option_value(int i) { return i < nopts ? opts[i].value : ""; }

void option_set(const char *key, const char *value)
{
    set(key, value);
    for (int i = 0; i < nopts; i++)
        if (!strcasecmp(opts[i].key, key))
            opts[i].dirty = 1;
}

/* comments for keys written to the file the first time */
static const char *const help[][2] = {
    {"display", "display: window or fullscreen (Alt+Enter switches too)"},
    {"vsync", "vsync: on (frames wait for the screen) or off"},
    {"antialiasing", "antialiasing: off, 2x, 4x (the default) or 8x (multisampling: the 3D world's edges smoothed; "
                     "the HUD and menus drawn as they were)"},
    {"rumble", "rumble: on or off (controller vibration)"},
    {"controller_layout", "controller_layout: 1 (the default), 2, 3 or 4 - the Xbox 360 version's controller configurations"},
    {"frame_limit", "frame_limit: off, screen (3 under the screen's refresh, for G-Sync / FreeSync) or a rate"},
    {"frame_rate", "frame_rate: 30 (the game's), 60, 90 or 120 (the game's own simulation runs that many frames a second)"},
    {"resolution", "resolution: original (the game's video modes), desktop or WxH"},
    {"ui", "ui: center (menus, text and HUD keep their 4:3 shape on wide screens) or stretch"},
    {"draw_distance", "draw_distance: original (the game's fog end: 2000, 3000 or 4000 by VIEW DISTANCE), far (twice, the default) "
                      "or max (four times; or a number 1-4): how far the city shows, its detailed models and the cars following"},
    {"fog", "fog: haze (the default: the near field clear, thickening to the end) or original (fogged from the camera on, "
            "in proportion to distance)"},
    {"start", "start: off, roam[:location] or race:event - straight from the title into driving (tests)"},
    {"start_car", "start_car: the car for start (0-0x37)"},
    {"mode", "mode: off or 0-5 - the lighting mode (time of day) for every race and free roam: 0 afternoon, 1 midday, "
             "2 sunset, 3 night, 4 dusk/overcast, 5 morning (tests)"},
    {"render_rate", "render_rate: screen (frames drawn at the screen's refresh, the default), sim (at frame_rate) or a rate; the simulation steps at frame_rate under it, what moves blended between steps"},
    {"catch_up", "catch_up: on (a slow frame is made up with an extra simulation step, keeping real time) or off"},
    {"hud_scale", "hud_scale: auto (the race HUD grows with the screen: its height / 720) or a number (1 = the PC's pixel size)"},
    {"hud_layout", "hud_layout: native (the default: the race HUD's side margins those of a 4:3 screen of the same height, "
                   "speedometer, minimap and district sign placed against the same margins, so the four corners line up on "
                   "any screen shape) or original (the game's: margins 7.5% of the screen's width, the bottom corners at fixed "
                   "offsets)"},
    {"music","music: xbox (the Xbox soundtrack, 44.1 kHz stereo, when converted) or pc (22 kHz mono)"},
    {"skip_intro", "skip_intro: on (straight to the title) or off"},
    {"sun", "sun: true (the default: the shading and the shadows follow the sun the sky shows, at least 30 degrees "
            "up, with the sky's light in the shade: shaders = improved) or original (the game's: the shading lit from "
            "about 81 degrees up, the shadows from 54, whatever the sky shows)"},
    {"shadow_distance", "shadow_distance: far (the default: shadows up to 300 m ahead), max (450 m, coarser up close) "
                        "or original (the game's 150 m)"},
    {"shadow_resolution", "shadow_resolution: original (the game's 1200 x 1200), 2048, 4096 (the default) or 8192: sharper shadows"},
    {"shadow_filter", "shadow_filter: original (the game's hard 2x2 edge), 3, 5 or 7 (the default): shadow edges softened over that many shadow-map texels"},
    {"reflection_resolution", "reflection_resolution: original (the game's 256), 512, 1024 (the default) or 2048: sharper reflections on the cars"},
    {"anisotropic", "anisotropic: 16, 8, 4, 2 or off (texture filtering that keeps the road ahead sharp)"},
    {"split", "split: horizontal (two players one above the other, the default) or vertical (side by side)"},
    {"mods", "mods: on (each folder in the user folder's mods/ holds files at the game's own paths, loose or whole "
             ".dir/.res packs, used in place of the game's; folders in name order, the first wins) or off"},
    {"shaders", "shaders: improved (the default: the replacements built from data/shaders, per-pixel car lighting) or original (the game's own)"},
    {"xbox_data", "xbox_data: on (files the Xbox version has better, prepared in the user folder's xbox_data, are used) or off"},
    {NULL, NULL}};

/* Rewrite options.txt keeping its comments and order: a changed key's line is replaced, keys
   the file does not have yet are appended. */
int options_save(void)
{
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/options.txt", user_dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        LOG(LOG_ALWAYS, "options: cannot write %s", tmp);
        return 0;
    }
    int written[64] = {0};
    char line[512];
    if (!in)
        fputs("# 1NSANE native: Enhanced options (key = value), also set from the game's Options menu.\n"
              "# Delete a line to get the original behaviour.\n", out);
    while (in && fgets(line, sizeof line, in)) {
        char k[64] = "";
        const char *eq = strchr(line, '=');
        const char *hash = strchr(line, '#');
        if (eq && (!hash || hash > eq)) {
            const char *a = line, *b = eq;
            while (a < b && isspace((unsigned char)*a)) a++;
            while (b > a && isspace((unsigned char)b[-1])) b--;
            snprintf(k, sizeof k, "%.*s", (int)(b - a), a);
        }
        int done = 0;
        for (int i = 0; i < nopts && k[0]; i++)
            if (!strcasecmp(opts[i].key, k) && opts[i].dirty) {
                if (!written[i]) fprintf(out, "%s = %s\n", opts[i].key, opts[i].value);
                written[i] = done = 1;
            }
        if (!done)
            fputs(line, out);
    }
    for (int i = 0; i < nopts; i++)
        if (opts[i].dirty && !written[i]) {
            for (int k = 0; help[k][0]; k++)
                if (!strcasecmp(help[k][0], opts[i].key))
                    fprintf(out, "\n# %s\n", help[k][1]);
            fprintf(out, "%s = %s\n", opts[i].key, opts[i].value);
        }
    if (in) fclose(in);
    int ok = fclose(out) == 0 && rename(tmp, path) == 0;
    if (!ok)
        LOG(LOG_ALWAYS, "options: cannot save %s", path);
    return ok;
}
