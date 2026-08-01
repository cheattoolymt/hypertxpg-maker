/*
 * compile.c - HyperTxPG (htg) .htgp -> .htgb compile / .htgb load
 *             (spec section 1 and section 7 step 7).
 *
 * Implementation: serialize the project's JSON source tree to a byte string,
 * apply a repeating-key XOR (key is a build-time constant), and write it out
 * behind a small header ("HTGB" + version byte). Decoding reverses the XOR and
 * hands the recovered JSON text to the normal parser/model, so .htgb and .htgp
 * share one and the same game engine.
 *
 * Standard C only (C11), no external libraries, no OS-specific APIs.
 */
#include "compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/* Build-time obfuscation key. Deliberately not a secret; per the spec this is
 * only meant to stop casual text-editor tampering, not to be real crypto. */
static const unsigned char HTGB_KEY[] = {
    0x48, 0x79, 0x70, 0x65, 0x72, 0x54, 0x78, 0x50, 0x47, 0x2D, 0x68, 0x74, 0x67
}; /* "HyperTxPG-htg" */

static const char  HTGB_MAGIC[4] = { 'H', 'T', 'G', 'B' };
#define HTGB_FORMAT_VERSION 1

/* XOR the buffer in place with the repeating key. XOR is its own inverse, so
 * the same routine both obfuscates and de-obfuscates. */
static void htgb_xor(unsigned char *buf, size_t len) {
    size_t klen = sizeof(HTGB_KEY);
    for (size_t i = 0; i < len; i++) {
        buf[i] = (unsigned char)(buf[i] ^ HTGB_KEY[i % klen]);
    }
}

/* Derive "<base>.htgb" from an input path ending in ".htgp" (or otherwise by
 * appending ".htgb"). Writes into out (size cap). Returns 0 on success. */
static int derive_htgb_path(const char *in_path, char *out, size_t cap) {
    size_t n = strlen(in_path);
    const char *dot = strrchr(in_path, '.');
    if (dot && strcmp(dot, ".htgp") == 0) {
        size_t base = (size_t)(dot - in_path);
        if (base + 6 >= cap) return -1; /* ".htgb" + NUL */
        memcpy(out, in_path, base);
        memcpy(out + base, ".htgb", 6); /* includes NUL */
        return 0;
    }
    if (n + 6 >= cap) return -1;
    memcpy(out, in_path, n);
    memcpy(out + n, ".htgb", 6);
    return 0;
}

int htg_compile_file(const char *in_path, const char *out_path, const char **err) {
    /* Load & validate the source project first (also confirms it is a
     * well-formed .htgp per the model). */
    const char *lerr = NULL;
    HtgProject *p = htg_project_load(in_path, &lerr);
    if (!p) {
        if (err) *err = lerr ? lerr : "プロジェクトの読み込みに失敗しました";
        return 1;
    }

    /* Serialize the source tree to compact JSON (no need to pretty-print an
     * obfuscated blob). */
    char *json = htg_project_to_string(p);
    htg_project_free(p);
    if (!json) {
        if (err) *err = "JSON シリアライズに失敗しました(メモリ不足)";
        return 1;
    }

    size_t json_len = strlen(json);

    /* Obfuscate. Cast is safe: JSON text is a byte buffer. */
    htgb_xor((unsigned char *)json, json_len);

    /* Resolve output path. */
    char derived[1024];
    if (!out_path) {
        if (derive_htgb_path(in_path, derived, sizeof(derived)) != 0) {
            free(json);
            if (err) *err = "出力パスの導出に失敗しました(パスが長すぎます)";
            return 1;
        }
        out_path = derived;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        free(json);
        if (err) *err = "出力ファイルを開けませんでした";
        return 1;
    }

    unsigned char header[5];
    memcpy(header, HTGB_MAGIC, 4);
    header[4] = (unsigned char)HTGB_FORMAT_VERSION;

    int ok = 1;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) ok = 0;
    if (ok && json_len > 0 && fwrite(json, 1, json_len, f) != json_len) ok = 0;

    fclose(f);
    free(json);

    if (!ok) {
        if (err) *err = "出力ファイルへの書き込みに失敗しました";
        return 1;
    }
    return 0;
}

HtgProject *htg_project_load_htgb(const char *path, const char **err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err) *err = "ファイルを開けませんでした";
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err) *err = "ファイルのシークに失敗しました";
        return NULL;
    }
    long size = ftell(f);
    if (size < 5) {
        fclose(f);
        if (err) *err = "不正な .htgb ファイルです(サイズが小さすぎます)";
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        if (err) *err = "ファイルのシークに失敗しました";
        return NULL;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        if (err) *err = "メモリの確保に失敗しました";
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        if (err) *err = "ファイルの読み込みに失敗しました";
        return NULL;
    }
    fclose(f);

    if (memcmp(buf, HTGB_MAGIC, 4) != 0) {
        free(buf);
        if (err) *err = "不正な .htgb ファイルです(マジックが一致しません)";
        return NULL;
    }
    if (buf[4] != HTGB_FORMAT_VERSION) {
        free(buf);
        if (err) *err = "未対応の .htgb フォーマットバージョンです";
        return NULL;
    }

    size_t payload_len = (size_t)size - 5;

    /* De-obfuscate the payload and NUL-terminate it as JSON text. */
    char *json = (char *)malloc(payload_len + 1);
    if (!json) {
        free(buf);
        if (err) *err = "メモリの確保に失敗しました";
        return NULL;
    }
    memcpy(json, buf + 5, payload_len);
    free(buf);
    htgb_xor((unsigned char *)json, payload_len);
    json[payload_len] = '\0';

    const char *perr = NULL;
    JsonValue *root = json_parse(json, &perr);
    free(json);
    if (!root) {
        if (err) *err = perr ? perr : "JSON の解析に失敗しました";
        return NULL;
    }

    const char *merr = NULL;
    HtgProject *p = htg_project_from_json(root, &merr);
    if (!p) {
        json_free(root);
        if (err) *err = merr ? merr : "データモデルの構築に失敗しました";
        return NULL;
    }
    return p;
}
