/*
 * editor.h - Interactive `htg edit` editor (spec sections 3, 4 & 5).
 *
 * The editor mutates the project's underlying JSON source tree (p->source),
 * which is exactly what htg_project_save() serializes. Working on the raw JSON
 * (rather than the typed structs) keeps unmodelled fields intact and makes the
 * "save" step a straight round-trip of the edited tree.
 */
#ifndef HTG_EDITOR_H
#define HTG_EDITOR_H

/*
 * Run the interactive editor for the .htgp file at `path`.
 * Returns 0 on a clean exit (whether the user saved or discarded),
 * non-zero on a fatal error (file could not be loaded, etc.).
 */
int htg_editor_run(const char *path);

#endif /* HTG_EDITOR_H */
