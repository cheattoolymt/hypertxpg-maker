/*
 * main.c - HyperTxPG (htg) CLI entry point and command dispatch (spec sec. 1).
 *
 * Implemented in this stage (spec sections 1-2):
 *   htg version        - print version
 *   htg help           - print usage
 *   htg new <name>     - create a new <name>.htgp skeleton project
 *
 * The remaining commands are dispatched here but their full behaviour is
 * intentionally deferred to later spec sections:
 *   htg edit <f>       - interactive editor        (spec section 3+/4)
 *   htg run  <f>       - game engine               (spec section 4)
 *   htg compile <f>    - .htgp -> .htgb            (spec section 7 step 7)
 *
 * These stubs still perform the section 1 responsibility of *dispatch*:
 * argument checking, extension-based file-type detection for `run`, and
 * loading/validating the project via the section 2 data model where it makes
 * sense, then reporting that the feature is not yet available.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "model.h"

#define HTG_VERSION "0.1.0"

/* ---- helpers ---- */

static const char *file_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

static int has_ext(const char *path, const char *ext) {
    const char *e = file_ext(path);
    return strcmp(e, ext) == 0;
}

/* Return 1 if a file exists (readable). */
static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static void print_version(void) {
    printf("htg (HyperTxPG) version %s\n", HTG_VERSION);
}

static void print_help(void) {
    printf(
        "HyperTxPG (htg) - ノーコード テキストRPG 作成/実行ツール\n"
        "\n"
        "使い方:\n"
        "  htg new <project_name>          新規 .htgp を生成\n"
        "  htg edit <project.htgp>         対話式エディタを開く\n"
        "  htg run <project.htgp|.htgb>    ゲームを実行(プレイ)\n"
        "  htg compile <project.htgp>      .htgp -> .htgb にコンパイル(難読化)\n"
        "  htg version                     バージョンを表示\n"
        "  htg help                        このヘルプを表示\n"
    );
}

/* ---- commands ---- */

/*
 * `htg new <project_name>`
 * Creates <project_name>.htgp containing a minimal valid skeleton.
 * (Spec section 1 lists this as an interactive generator; the interactive
 *  prompts belong to the editor work of a later section. Here we implement the
 *  concrete, testable core: produce a valid skeleton file for the given name.)
 */
static int cmd_new(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "エラー: プロジェクト名を指定してください。\n");
        fprintf(stderr, "使い方: htg new <project_name>\n");
        return 2;
    }
    const char *name = argv[2];

    /* Build output filename: append .htgp unless already provided. */
    char outpath[1024];
    if (has_ext(name, ".htgp")) {
        snprintf(outpath, sizeof(outpath), "%s", name);
    } else {
        snprintf(outpath, sizeof(outpath), "%s.htgp", name);
    }

    if (file_exists(outpath)) {
        fprintf(stderr, "エラー: '%s' は既に存在します。\n", outpath);
        return 1;
    }

    /* Derive a title/start_room from the base name. */
    char title[512];
    snprintf(title, sizeof(title), "%s", name);
    /* strip trailing .htgp from title if present */
    char *dot = strstr(title, ".htgp");
    if (dot) *dot = '\0';

    JsonValue *root = htg_new_skeleton(title, "room_start");
    if (!root) {
        fprintf(stderr, "エラー: プロジェクトの生成に失敗しました(メモリ不足)。\n");
        return 1;
    }

    if (json_serialize_file(root, outpath, 1) != 0) {
        fprintf(stderr, "エラー: '%s' への書き込みに失敗しました。\n", outpath);
        json_free(root);
        return 1;
    }
    json_free(root);

    printf("→ '%s' を作成しました\n", outpath);
    return 0;
}

/*
 * `htg edit <project.htgp>` - dispatch stub.
 * Loads the project through the data model to prove it is valid, then reports
 * that the interactive editor arrives in a later section.
 */
static int cmd_edit(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "エラー: プロジェクトファイルを指定してください。\n");
        fprintf(stderr, "使い方: htg edit <project.htgp>\n");
        return 2;
    }
    const char *path = argv[2];
    if (!has_ext(path, ".htgp")) {
        fprintf(stderr, "エラー: edit は .htgp ファイルのみ対応しています。\n");
        return 1;
    }
    const char *err = NULL;
    HtgProject *p = htg_project_load(path, &err);
    if (!p) {
        fprintf(stderr, "エラー: '%s' の読み込みに失敗しました: %s\n",
                path, err ? err : "不明なエラー");
        return 1;
    }
    printf("読み込み成功: \"%s\" (rooms:%zu items:%zu actors:%zu skills:%zu events:%zu)\n",
           p->meta.title, p->room_count, p->item_count,
           p->actor_count, p->skill_count, p->event_count);
    printf("(エディタ機能は次の実装段階で提供されます)\n");
    htg_project_free(p);
    return 0;
}

/*
 * `htg run <project.htgp|.htgb>` - dispatch stub.
 * Performs the section 1 responsibility: detect file type by extension. For
 * .htgp it loads via the model. .htgb decoding + the game engine are later.
 */
static int cmd_run(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "エラー: プロジェクトファイルを指定してください。\n");
        fprintf(stderr, "使い方: htg run <project.htgp|.htgb>\n");
        return 2;
    }
    const char *path = argv[2];

    if (has_ext(path, ".htgp")) {
        const char *err = NULL;
        HtgProject *p = htg_project_load(path, &err);
        if (!p) {
            fprintf(stderr, "エラー: '%s' の読み込みに失敗しました: %s\n",
                    path, err ? err : "不明なエラー");
            return 1;
        }
        printf("読み込み成功: \"%s\" / 開始部屋: %s\n",
               p->meta.title, p->meta.start_room);
        printf("(実行エンジンは次の実装段階で提供されます)\n");
        htg_project_free(p);
        return 0;
    } else if (has_ext(path, ".htgb")) {
        printf("(.htgb の読み込み/実行は次の実装段階で提供されます)\n");
        return 0;
    }

    fprintf(stderr, "エラー: 拡張子が不明です(.htgp か .htgb を指定してください)。\n");
    return 1;
}

/*
 * `htg compile <project.htgp>` - dispatch stub.
 * Validates the input via the model; actual .htgb serialization is a later
 * section (spec section 7, step 7).
 */
static int cmd_compile(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "エラー: プロジェクトファイルを指定してください。\n");
        fprintf(stderr, "使い方: htg compile <project.htgp>\n");
        return 2;
    }
    const char *path = argv[2];
    if (!has_ext(path, ".htgp")) {
        fprintf(stderr, "エラー: compile は .htgp ファイルのみ対応しています。\n");
        return 1;
    }
    const char *err = NULL;
    HtgProject *p = htg_project_load(path, &err);
    if (!p) {
        fprintf(stderr, "エラー: '%s' の読み込みに失敗しました: %s\n",
                path, err ? err : "不明なエラー");
        return 1;
    }
    printf("入力の検証に成功しました: \"%s\"\n", p->meta.title);
    printf("(.htgb へのコンパイルは次の実装段階で提供されます)\n");
    htg_project_free(p);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 ||
        strcmp(cmd, "-v") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        print_help();
        return 0;
    }
    if (strcmp(cmd, "new") == 0)     return cmd_new(argc, argv);
    if (strcmp(cmd, "edit") == 0)    return cmd_edit(argc, argv);
    if (strcmp(cmd, "run") == 0)     return cmd_run(argc, argv);
    if (strcmp(cmd, "compile") == 0) return cmd_compile(argc, argv);

    fprintf(stderr, "エラー: 不明なコマンド '%s'\n\n", cmd);
    print_help();
    return 1;
}
