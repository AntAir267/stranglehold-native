/* Enhanced options (options.c): <user>/options.txt, overridable with --option key=value. */
#pragma once

void options_load(void);
int option_parse(const char *line);                     /* "key = value" */
const char *option_str(const char *key, const char *def);
int option_bool(const char *key, int def);
double option_draw_distance(void);      /* draw_distance: original 1, far 2, max 4 (or 1-4) */
int option_sun_true(void);              /* sun: true 1, original 0 */
double option_shadow_distance(void);    /* shadow_distance: original 1, far 2, max 3 */
void option_set(const char *key, const char *value);   /* for this run; options_save() keeps it */
int options_save(void);         /* rewrite options.txt: changed keys in place, new ones appended */
void options_changed(void);     /* the in-game Enhanced page changed something: apply it now */
