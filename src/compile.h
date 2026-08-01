/*
 * compile.h - HyperTxPG (htg) .htgp -> .htgb compile / .htgb load
 *             (spec section 1 and section 7 step 7).
 *
 * The .htgb format keeps exactly the same data structure as .htgp: the project
 * is serialized to its JSON byte sequence and then lightly obfuscated with a
 * repeating-key XOR (the key is a build-time constant). This is *not* strong
 * encryption; its only goal is to stop a player from trivially editing the
 * project in a text editor. `htg run` auto-detects .htgb by extension, decodes
 * it back to JSON, and feeds the same in-memory game engine.
 *
 * On-disk layout:
 *   offset 0 : 4 bytes  magic  "HTGB"
 *   offset 4 : 1 byte   format version (currently 1)
 *   offset 5 : N bytes  XOR-obfuscated UTF-8 JSON payload
 */
#ifndef HTG_COMPILE_H
#define HTG_COMPILE_H

#include "model.h"

/*
 * Compile a .htgp project file into a .htgb file.
 *   in_path  : path to an existing .htgp (JSON) file.
 *   out_path : destination .htgb path. If NULL, it is derived from in_path by
 *              replacing a trailing ".htgp" with ".htgb" (or appending ".htgb").
 * Returns 0 on success. On error, non-zero is returned and (if err is
 * non-NULL) *err points to a short static message.
 */
int htg_compile_file(const char *in_path, const char *out_path, const char **err);

/*
 * Load (decode) a .htgb file back into a live HtgProject.
 * Returns a heap-allocated project (free with htg_project_free) or NULL on
 * error; *err (if non-NULL) receives a short static message.
 */
HtgProject *htg_project_load_htgb(const char *path, const char **err);

#endif /* HTG_COMPILE_H */
