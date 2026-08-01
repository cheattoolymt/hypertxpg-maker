/*
 * ui.h - Shared CLI UI helpers for HyperTxPG (htg) (spec section 4).
 *
 * Every interactive menu in `htg edit` / `htg run` funnels through these so
 * the presentation stays uniform with the spec's UI patterns:
 *
 *   === <title> ===
 *   1. <option>
 *   2. <option>
 *   >>
 *
 * Standard C only; reads from stdin, writes to stdout.
 */
#ifndef HTG_UI_H
#define HTG_UI_H

#include <stddef.h>

/*
 * Read one line from stdin into buf (size cap, NUL-terminated, trailing
 * newline stripped). Returns 0 on success, non-zero on EOF/error (buf set to
 * empty string in that case). A prompt (may be NULL) is printed first.
 */
int ui_read_line(const char *prompt, char *buf, size_t cap);

/*
 * Print a numbered menu (title + options[0..n-1]) and read a valid choice.
 * Re-prompts on non-numeric / out-of-range input. Returns the 1-based index
 * chosen (1..count). Returns 0 only on EOF (caller should treat as cancel).
 */
int ui_menu(const char *title, const char *const *options, int count);

/*
 * Prompt for an integer. Re-prompts on non-numeric input. If allow_empty is
 * non-zero, an empty line returns `def` via *out and yields 1 (used). Returns
 * 1 if a value was produced, 0 on EOF.
 */
int ui_read_int(const char *prompt, int *out, int allow_empty, int def);

/*
 * Prompt for a yes/no answer ("(y/n)"). Returns 1 for yes, 0 for no.
 * Re-prompts until a valid answer; on EOF returns `def`.
 */
int ui_read_yesno(const char *prompt, int def);

/*
 * Prompt for a non-empty string, copied into buf. If allow_empty is non-zero,
 * an empty line is accepted (buf becomes ""). Returns 1 on success, 0 on EOF.
 */
int ui_read_string(const char *prompt, char *buf, size_t cap, int allow_empty);

/* Print a section header line: "=== <title> ===". */
void ui_header(const char *title);

#endif /* HTG_UI_H */
