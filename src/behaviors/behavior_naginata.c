/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_naginata

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/behavior.h>

#include <zmk_naginata/nglist.h>
#include <zmk_naginata/nglistarray.h>
#include <zmk_naginata/naginata_func.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define NONE 0

// 薙刀式

// 31キーを32bitの各ビットに割り当てる
#define B_Q (1UL << 0)
#define B_W (1UL << 1)
#define B_E (1UL << 2)
#define B_R (1UL << 3)
#define B_T (1UL << 4)

#define B_Y (1UL << 5)
#define B_U (1UL << 6)
#define B_I (1UL << 7)
#define B_O (1UL << 8)
#define B_P (1UL << 9)

#define B_A (1UL << 10)
#define B_S (1UL << 11)
#define B_D (1UL << 12)
#define B_F (1UL << 13)
#define B_G (1UL << 14)

#define B_H (1UL << 15)
#define B_J (1UL << 16)
#define B_K (1UL << 17)
#define B_L (1UL << 18)
#define B_SEMI (1UL << 19)

#define B_Z (1UL << 20)
#define B_X (1UL << 21)
#define B_C (1UL << 22)
#define B_V (1UL << 23)
#define B_B (1UL << 24)

#define B_N (1UL << 25)
#define B_M (1UL << 26)
#define B_COMMA (1UL << 27)
#define B_DOT (1UL << 28)
#define B_SLASH (1UL << 29)

#define B_SPACE (1UL << 30)

static NGListArray nginput;
static uint32_t pressed_keys = 0UL; // 押しているキーのビットをたてる
static int8_t n_pressed_keys = 0;   // 押しているキーの数
static int64_t last_press_timestamp;
static uint32_t last_pressed_keycode;
static bool defer_flush;
static struct k_work_delayable naginata_flush_work;

extern user_config_t naginata_config;

#if IS_ENABLED(CONFIG_NAGINATA_PERSISTENT_STATE)
static void naginata_save_work_handler(struct k_work *work) {
    settings_save_one("naginata/state", &naginata_config, sizeof(naginata_config));
    LOG_DBG("Saved Naginata state: %d, %d", naginata_config.os, naginata_config.tategaki);
}

static struct k_work_delayable naginata_save_work;

static int naginata_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(naginata_config)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &naginata_config, sizeof(naginata_config));
        if (rc >= 0) {
            LOG_INF("Loaded Naginata state: %d, %d", naginata_config.os, naginata_config.tategaki);
        }
        return MIN(rc, 0);
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(naginata, "naginata", NULL, naginata_settings_load_cb, NULL, NULL);
#endif // IS_ENABLED(CONFIG_NAGINATA_PERSISTENT_STATE)

static const uint32_t ng_key[] = {
    [A - A] = B_A,     [B - A] = B_B,         [C - A] = B_C,         [D - A] = B_D,
    [E - A] = B_E,     [F - A] = B_F,         [G - A] = B_G,         [H - A] = B_H,
    [I - A] = B_I,     [J - A] = B_J,         [K - A] = B_K,         [L - A] = B_L,
    [M - A] = B_M,     [N - A] = B_N,         [O - A] = B_O,         [P - A] = B_P,
    [Q - A] = B_Q,     [R - A] = B_R,         [S - A] = B_S,         [T - A] = B_T,
    [U - A] = B_U,     [V - A] = B_V,         [W - A] = B_W,         [X - A] = B_X,
    [Y - A] = B_Y,     [Z - A] = B_Z,         [SEMI - A] = B_SEMI,   [COMMA - A] = B_COMMA,
    [DOT - A] = B_DOT, [SLASH - A] = B_SLASH, [SPACE - A] = B_SPACE, [ENTER - A] = B_SPACE,
};

#define KANA_MAX_LEN 6

// カナ変換テーブル
typedef struct {
    uint32_t shift;
    uint32_t douji;
    uint32_t kana[KANA_MAX_LEN];
    void (*func)(void);
} naginata_kanamap;

// なかぼね配列：修飾キー（Qの裏）が直前のかなを変換するために使う記録
static void ng_modifier(void);
static uint32_t last_romaji[KANA_MAX_LEN];
static int last_romaji_len = 0;

static const naginata_kanamap ngdickana[] = {
    {.shift = NONE      , .douji = B_Q               , .kana = {H, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ほ
    {.shift = NONE      , .douji = B_W               , .kana = {K, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // き
    {.shift = NONE      , .douji = B_E               , .kana = {T, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // て
    {.shift = NONE      , .douji = B_R               , .kana = {S, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // し
    {.shift = NONE      , .douji = B_T               , .kana = {X, T, U, NONE, NONE, NONE}, .func = nofunc        }, // っ
    {.shift = NONE      , .douji = B_Y               , .kana = {K, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // く
    {.shift = NONE      , .douji = B_I               , .kana = {R, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // る
    {.shift = NONE      , .douji = B_O               , .kana = {S, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // す
    {.shift = NONE      , .douji = B_P               , .kana = {MINUS, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ー
    {.shift = NONE      , .douji = B_SEMI            , .kana = {H, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // へ
    {.shift = NONE      , .douji = B_A               , .kana = {R, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ろ
    {.shift = NONE      , .douji = B_S               , .kana = {K, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // け
    {.shift = NONE      , .douji = B_D               , .kana = {N, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // の
    {.shift = NONE      , .douji = B_F               , .kana = {K, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // か
    {.shift = NONE      , .douji = B_G               , .kana = {S, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // そ
    {.shift = NONE      , .douji = B_H               , .kana = {T, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // た
    {.shift = NONE      , .douji = B_J               , .kana = {A, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // あ
    {.shift = NONE      , .douji = B_K               , .kana = {I, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // い
    {.shift = NONE      , .douji = B_L               , .kana = {U, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // う
    {.shift = NONE      , .douji = B_Z               , .kana = {H, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ふ
    {.shift = NONE      , .douji = B_X               , .kana = {H, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ひ
    {.shift = NONE      , .douji = B_C               , .kana = {H, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // は
    {.shift = NONE      , .douji = B_V               , .kana = {D, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // で
    {.shift = NONE      , .douji = B_M               , .kana = {N, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // な
    {.shift = NONE      , .douji = B_COMMA           , .kana = {N, N, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ん
    {.shift = NONE      , .douji = B_DOT             , .kana = {R, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ら
    {.shift = NONE      , .douji = B_SLASH           , .kana = {R, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // れ
    {.shift = NONE      , .douji = B_U               , .kana = {BSPC, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // {BS}
    {.shift = NONE      , .douji = B_K|B_W           , .kana = {N, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぬ（中指シフト）
    {.shift = NONE      , .douji = B_K|B_E           , .kana = {R, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // り（中指シフト）
    {.shift = NONE      , .douji = B_K|B_R           , .kana = {M, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // め（中指シフト）
    {.shift = NONE      , .douji = B_K|B_T           , .kana = {T, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ち（中指シフト）
    {.shift = NONE      , .douji = B_K|B_A           , .kana = {S, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // せ（中指シフト）
    {.shift = NONE      , .douji = B_K|B_S           , .kana = {N, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // に（中指シフト）
    {.shift = NONE      , .douji = B_K|B_D           , .kana = {T, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // と（中指シフト）
    {.shift = NONE      , .douji = B_K|B_F           , .kana = {M, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ま（中指シフト）
    {.shift = NONE      , .douji = B_K|B_G           , .kana = {M, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // み（中指シフト）
    {.shift = NONE      , .douji = B_K|B_C           , .kana = {W, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // を（中指シフト）
    {.shift = NONE      , .douji = B_K|B_V           , .kana = {K, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // こ（中指シフト）
    {.shift = NONE      , .douji = B_D|B_Y           , .kana = {Y, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // や（中指シフト）
    {.shift = NONE      , .douji = B_D|B_U           , .kana = {S, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // さ（中指シフト）
    {.shift = NONE      , .douji = B_D|B_I           , .kana = {Y, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // よ（中指シフト）
    {.shift = NONE      , .douji = B_D|B_O           , .kana = {E, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // え（中指シフト）
    {.shift = NONE      , .douji = B_D|B_H           , .kana = {W, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // わ（中指シフト）
    {.shift = NONE      , .douji = B_D|B_L           , .kana = {T, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // つ（中指シフト）
    {.shift = NONE      , .douji = B_D|B_P           , .kana = {Y, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ゆ（中指シフト）
    {.shift = NONE      , .douji = B_D|B_M           , .kana = {O, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // お（中指シフト）
    {.shift = NONE      , .douji = B_D|B_COMMA       , .kana = {N, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ね（中指シフト）
    {.shift = NONE      , .douji = B_D|B_DOT         , .kana = {M, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // む（中指シフト）
    {.shift = NONE      , .douji = B_Q|B_K           , .kana = {NONE, NONE, NONE, NONE, NONE, NONE}, .func = ng_modifier }, // 修飾キー（Qの裏）
    {.shift = NONE      , .douji = B_D|B_F           , .kana = {M, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // も
    {.shift = NONE      , .douji = B_J|B_Q           , .kana = {B, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぼ（濁点）
    {.shift = NONE      , .douji = B_J|B_W           , .kana = {G, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぎ（濁点）
    {.shift = NONE      , .douji = B_J|B_E           , .kana = {D, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // で（濁点）
    {.shift = NONE      , .douji = B_J|B_R           , .kana = {Z, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // じ（濁点）
    {.shift = NONE      , .douji = B_J|B_T           , .kana = {D, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぢ（濁点）
    {.shift = NONE      , .douji = B_J|B_A           , .kana = {Z, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぜ（濁点）
    {.shift = NONE      , .douji = B_J|B_S           , .kana = {G, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // げ（濁点）
    {.shift = NONE      , .douji = B_J|B_D           , .kana = {D, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ど（濁点）
    {.shift = NONE      , .douji = B_J|B_F           , .kana = {G, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // が（濁点）
    {.shift = NONE      , .douji = B_J|B_G           , .kana = {Z, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぞ（濁点）
    {.shift = NONE      , .douji = B_J|B_Z           , .kana = {B, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぶ（濁点）
    {.shift = NONE      , .douji = B_J|B_X           , .kana = {B, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // び（濁点）
    {.shift = NONE      , .douji = B_J|B_C           , .kana = {B, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ば（濁点）
    {.shift = NONE      , .douji = B_J|B_V           , .kana = {G, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ご（濁点）
    {.shift = NONE      , .douji = B_F|B_Y           , .kana = {G, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぐ（濁点）
    {.shift = NONE      , .douji = B_F|B_U           , .kana = {Z, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ざ（濁点）
    {.shift = NONE      , .douji = B_F|B_O           , .kana = {Z, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ず（濁点）
    {.shift = NONE      , .douji = B_F|B_SEMI        , .kana = {B, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // べ（濁点）
    {.shift = NONE      , .douji = B_F|B_H           , .kana = {D, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // だ（濁点）
    {.shift = NONE      , .douji = B_F|B_L           , .kana = {D, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // づ（濁点）
    {.shift = NONE      , .douji = B_M|B_C           , .kana = {P, A, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぱ（半濁点）
    {.shift = NONE      , .douji = B_M|B_X           , .kana = {P, I, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぴ（半濁点）
    {.shift = NONE      , .douji = B_M|B_Z           , .kana = {P, U, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぷ（半濁点）
    {.shift = NONE      , .douji = B_M|B_Q           , .kana = {P, O, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぽ（半濁点）
    {.shift = NONE      , .douji = B_V|B_SEMI        , .kana = {P, E, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ぺ（半濁点）
    {.shift = NONE      , .douji = B_W|B_Y           , .kana = {K, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // きゃ
    {.shift = NONE      , .douji = B_W|B_P           , .kana = {K, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // きゅ
    {.shift = NONE      , .douji = B_W|B_I           , .kana = {K, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // きょ
    {.shift = NONE      , .douji = B_R|B_Y           , .kana = {S, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // しゃ
    {.shift = NONE      , .douji = B_R|B_P           , .kana = {S, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // しゅ
    {.shift = NONE      , .douji = B_R|B_I           , .kana = {S, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // しょ
    {.shift = NONE      , .douji = B_T|B_Y           , .kana = {T, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // ちゃ（2キー）
    {.shift = NONE      , .douji = B_T|B_P           , .kana = {T, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // ちゅ（2キー）
    {.shift = NONE      , .douji = B_T|B_I           , .kana = {T, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // ちょ（2キー）
    {.shift = NONE      , .douji = B_S|B_Y           , .kana = {N, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // にゃ（2キー）
    {.shift = NONE      , .douji = B_S|B_P           , .kana = {N, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // にゅ（2キー）
    {.shift = NONE      , .douji = B_S|B_I           , .kana = {N, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // にょ（2キー）
    {.shift = NONE      , .douji = B_X|B_Y           , .kana = {H, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // ひゃ
    {.shift = NONE      , .douji = B_X|B_P           , .kana = {H, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // ひゅ
    {.shift = NONE      , .douji = B_X|B_I           , .kana = {H, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // ひょ
    {.shift = NONE      , .douji = B_G|B_Y           , .kana = {M, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // みゃ（2キー）
    {.shift = NONE      , .douji = B_G|B_P           , .kana = {M, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // みゅ（2キー）
    {.shift = NONE      , .douji = B_G|B_I           , .kana = {M, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // みょ（2キー）
    {.shift = NONE      , .douji = B_E|B_Y           , .kana = {R, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // りゃ（2キー）
    {.shift = NONE      , .douji = B_E|B_P           , .kana = {R, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // りゅ（2キー）
    {.shift = NONE      , .douji = B_E|B_I           , .kana = {R, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // りょ（2キー）
    {.shift = NONE      , .douji = B_W|B_E|B_Y       , .kana = {G, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // ぎゃ
    {.shift = NONE      , .douji = B_W|B_E|B_P       , .kana = {G, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // ぎゅ
    {.shift = NONE      , .douji = B_W|B_E|B_I       , .kana = {G, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // ぎょ
    {.shift = NONE      , .douji = B_E|B_R|B_Y       , .kana = {Z, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // じゃ
    {.shift = NONE      , .douji = B_E|B_R|B_P       , .kana = {Z, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // じゅ
    {.shift = NONE      , .douji = B_E|B_R|B_I       , .kana = {Z, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // じょ
    {.shift = NONE      , .douji = B_X|B_C|B_Y       , .kana = {B, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // びゃ
    {.shift = NONE      , .douji = B_X|B_C|B_P       , .kana = {B, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // びゅ
    {.shift = NONE      , .douji = B_X|B_C|B_I       , .kana = {B, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // びょ
    {.shift = NONE      , .douji = B_Z|B_X|B_Y       , .kana = {P, Y, A, NONE, NONE, NONE}, .func = nofunc        }, // ぴゃ
    {.shift = NONE      , .douji = B_Z|B_X|B_P       , .kana = {P, Y, U, NONE, NONE, NONE}, .func = nofunc        }, // ぴゅ
    {.shift = NONE      , .douji = B_Z|B_X|B_I       , .kana = {P, Y, O, NONE, NONE, NONE}, .func = nofunc        }, // ぴょ
    {.shift = NONE      , .douji = B_E|B_R|B_O       , .kana = {Z, Y, E, NONE, NONE, NONE}, .func = nofunc        }, // じぇ
    {.shift = NONE      , .douji = B_A|B_Q           , .kana = {LS(N1), NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ！
    {.shift = NONE      , .douji = B_A|B_W           , .kana = {LS(SLASH), NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ？
    {.shift = NONE      , .douji = B_S|B_D           , .kana = {COMMA, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // 、
    {.shift = NONE      , .douji = B_K|B_U           , .kana = {LBKT, RBKT, LEFT, NONE, NONE, NONE}, .func = nofunc        }, // 「」セット
    {.shift = NONE      , .douji = B_K|B_O           , .kana = {LS(N9), LS(N0), LEFT, NONE, NONE, NONE}, .func = nofunc        }, // （）セット
    {.shift = NONE      , .douji = B_K|B_J           , .kana = {DOT, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // 。
    {.shift = NONE      , .douji = B_K|B_Y           , .kana = {SLASH, NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ・
    {.shift = NONE      , .douji = B_K|B_H           , .kana = {LS(SEMI), NONE, NONE, NONE, NONE, NONE}, .func = nofunc        }, // ：
};

// Helper function for counting matches/candidates
static int count_kana_entries(NGList *keys, bool exact_match) {
  if (keys->size == 0) return 0;
  // 変換テーブルの最大同時押しは3キー。4キー以上に一致するエントリは存在しない。
  // 連続シフト処理でリストが4キー以上に伸びる場合があるため、ここで弾く。
  if (keys->size > 3) return 0;

  int count = 0;
  uint32_t keyset0 = 0UL, keyset1 = 0UL, keyset2 = 0UL;

  // keysetを配列にしたらバイナリサイズが増えた
  switch (keys->size) {
    case 1:
      keyset0 = ng_key[keys->elements[0] - A];
      break;
    case 2:
      keyset0 = ng_key[keys->elements[0] - A];
      keyset1 = ng_key[keys->elements[1] - A];
      break;
    default:
      keyset0 = ng_key[keys->elements[0] - A];
      keyset1 = ng_key[keys->elements[1] - A];
      keyset2 = ng_key[keys->elements[2] - A];
      break;
  }

  for (int i = 0; i < sizeof ngdickana / sizeof ngdickana[0]; i++) {
    bool matches = false;

    switch (keys->size) {
      case 1:
        if (exact_match) {
          matches = (ngdickana[i].shift == keyset0) ||
                   (ngdickana[i].shift == 0UL && ngdickana[i].douji == keyset0);
        } else {
          matches = ((ngdickana[i].shift & keyset0) == keyset0) ||
                   (ngdickana[i].shift == 0UL && (ngdickana[i].douji & keyset0) == keyset0);
        }
        break;
      case 2:
        if (exact_match) {
          matches = (ngdickana[i].shift == (keyset0 | keyset1)) ||
                   (ngdickana[i].shift == keyset0 && ngdickana[i].douji == keyset1) ||
                   (ngdickana[i].shift == 0UL && ngdickana[i].douji == (keyset0 | keyset1));
        } else {
          matches = (ngdickana[i].shift == (keyset0 | keyset1)) ||
                   (ngdickana[i].shift == keyset0 && (ngdickana[i].douji & keyset1) == keyset1) ||
                   (ngdickana[i].shift == 0UL && (ngdickana[i].douji & (keyset0 | keyset1)) == (keyset0 | keyset1));
          // しぇ、ちぇ、など2キーで確定してはいけない
          if (matches && (ngdickana[i].shift | ngdickana[i].douji) != (keyset0 | keyset1)) {
            count = 2;
          }
        }
        break;
      default:
        if (exact_match) {
          matches = (ngdickana[i].shift == (keyset0 | keyset1) && ngdickana[i].douji == keyset2) ||
                   (ngdickana[i].shift == keyset0 && ngdickana[i].douji == (keyset1 | keyset2)) ||
                   (ngdickana[i].shift == 0UL && ngdickana[i].douji == (keyset0 | keyset1 | keyset2));
        } else {
          matches = (ngdickana[i].shift == (keyset0 | keyset1) && (ngdickana[i].douji & keyset2) == keyset2) ||
                   (ngdickana[i].shift == keyset0 && (ngdickana[i].douji & (keyset1 | keyset2)) == (keyset1 | keyset2)) ||
                   (ngdickana[i].shift == 0UL && (ngdickana[i].douji & (keyset0 | keyset1 | keyset2)) == (keyset0 | keyset1 | keyset2));
        }
        break;
    }

    if (matches) {
      count++;
      if (count > 1) break;
    }
  }

  return count;
}

int number_of_matches(NGList *keys) {
  return count_kana_entries(keys, true);
}

int number_of_candidates(NGList *keys) {
  return count_kana_entries(keys, false);
}

// キー入力を文字に変換して出力する
void ng_type(NGList *keys) {
    LOG_DBG(">NAGINATA NG_TYPE");

    if (keys->size == 0)
        return;

    int64_t ts = naginata_get_timestamp();

    if (keys->size == 1 && keys->elements[0] == ENTER) {
        LOG_DBG(" NAGINATA type keycode 0x%02X", ENTER);
        raise_zmk_keycode_state_changed_from_encoded(ENTER, true, ts);
        raise_zmk_keycode_state_changed_from_encoded(ENTER, false, ts);
        return;
    }

    uint32_t keyset = 0UL;
    for (int i = 0; i < keys->size; i++) {
        keyset |= ng_key[keys->elements[i] - A];
    }

    for (int i = 0; i < sizeof ngdickana / sizeof ngdickana[0]; i++) {
        if ((ngdickana[i].shift | ngdickana[i].douji) == keyset) {
            if (ngdickana[i].kana[0] != NONE) {
                bool is_romaji = true;
                int rlen = 0;
                for (int k = 0; k < KANA_MAX_LEN; k++) {
                    if (ngdickana[i].kana[k] == NONE)
                        break;
                    if (ngdickana[i].kana[k] < A || ngdickana[i].kana[k] > Z)
                        is_romaji = false;
                    rlen++;
                }
                for (int k = 0; k < KANA_MAX_LEN; k++) {
                    if (ngdickana[i].kana[k] == NONE)
                        break;
                    LOG_DBG(" NAGINATA type keycode 0x%02X", ngdickana[i].kana[k]);
                    raise_zmk_keycode_state_changed_from_encoded(ngdickana[i].kana[k], true,
                                                                 ts);
                    raise_zmk_keycode_state_changed_from_encoded(ngdickana[i].kana[k], false,
                                                                 ts);
                }
                // 修飾キー用に、直前に打ったかなのローマ字を覚えておく
                if (is_romaji && rlen > 0) {
                    for (int k = 0; k < KANA_MAX_LEN; k++) {
                        last_romaji[k] = ngdickana[i].kana[k];
                    }
                    last_romaji_len = rlen;
                }
            } else {
                ngdickana[i].func();
            }
            LOG_DBG("<NAGINATA NG_TYPE");
            return;
        }
    }

    // 1キーで一致しなかった場合、これ以上分割できない。
    // ここで分割再帰すると同じ1キーを無限に再帰してスタックを溢れさせるため打ち切る。
    if (keys->size <= 1) {
        LOG_DBG("<NAGINATA NG_TYPE (no match, single key)");
        return;
    }

    // JIみたいにJIを含む同時押しはたくさんあるが、JIのみの同時押しがないとき
    // 最後の１キーを別に分けて変換する
    NGList a, b;
    initializeList(&a);
    initializeList(&b);
    for (int i = 0; i < keys->size - 1; i++) {
        addToList(&a, keys->elements[i]);
    }
    addToList(&b, keys->elements[keys->size - 1]);
    ng_type(&a);
    ng_type(&b);

    LOG_DBG("<NAGINATA NG_TYPE");
}

// 保留中の候補1件を確定する。短いロールオーバーを分割した場合や、
// 重なり時間の閾値を過ぎた場合に呼び出す。
// 修飾キー（紅皿式）：直前のかなを、未確定のうちにBSで消して変換形を打ち直す。
// 変換は循環する（か→が→か、は→ば→ぱ→は、つ→づ→っ→つ、あ→ぁ→あ）。
typedef struct {
    const char *from;
    const char *to;
} romaji_step;

static const romaji_step romaji_steps[] = {
    {"KA", "GA"}, {"GA", "KA"}, {"KI", "GI"}, {"GI", "KI"}, {"KU", "GU"}, {"GU", "KU"},
    {"KE", "GE"}, {"GE", "KE"}, {"KO", "GO"}, {"GO", "KO"},
    {"SA", "ZA"}, {"ZA", "SA"}, {"SI", "ZI"}, {"ZI", "SI"}, {"SU", "ZU"}, {"ZU", "SU"},
    {"SE", "ZE"}, {"ZE", "SE"}, {"SO", "ZO"}, {"ZO", "SO"},
    {"TA", "DA"}, {"DA", "TA"}, {"TI", "DI"}, {"DI", "TI"},
    {"TU", "DU"}, {"DU", "XTU"}, {"XTU", "TU"},
    {"TE", "DE"}, {"DE", "TE"}, {"TO", "DO"}, {"DO", "TO"},
    {"HA", "BA"}, {"BA", "PA"}, {"PA", "HA"},
    {"HI", "BI"}, {"BI", "PI"}, {"PI", "HI"},
    {"HU", "BU"}, {"BU", "PU"}, {"PU", "HU"},
    {"HE", "BE"}, {"BE", "PE"}, {"PE", "HE"},
    {"HO", "BO"}, {"BO", "PO"}, {"PO", "HO"},
    {"A", "XA"}, {"XA", "A"}, {"I", "XI"}, {"XI", "I"},
    {"U", "XU"}, {"XU", "U"}, {"E", "XE"}, {"XE", "E"},
    {"O", "XO"}, {"XO", "O"},
    {"YA", "XYA"}, {"XYA", "YA"}, {"YU", "XYU"}, {"XYU", "YU"},
    {"YO", "XYO"}, {"XYO", "YO"}, {"WA", "XWA"}, {"XWA", "WA"},
    {"KYA", "GYA"}, {"GYA", "KYA"}, {"KYU", "GYU"}, {"GYU", "KYU"},
    {"KYO", "GYO"}, {"GYO", "KYO"},
    {"SYA", "ZYA"}, {"ZYA", "SYA"}, {"SYU", "ZYU"}, {"ZYU", "SYU"},
    {"SYO", "ZYO"}, {"ZYO", "SYO"},
    {"TYA", "DYA"}, {"DYA", "TYA"}, {"TYU", "DYU"}, {"DYU", "TYU"},
    {"TYO", "DYO"}, {"DYO", "TYO"},
    {"HYA", "BYA"}, {"BYA", "PYA"}, {"PYA", "HYA"},
    {"HYU", "BYU"}, {"BYU", "PYU"}, {"PYU", "HYU"},
    {"HYO", "BYO"}, {"BYO", "PYO"}, {"PYO", "HYO"},
};

static void ng_modifier(void) {
    if (last_romaji_len == 0) {
        return;
    }
    // 直前のローマ字を文字列として取り出す
    char cur[KANA_MAX_LEN + 1];
    for (int i = 0; i < last_romaji_len; i++) {
        cur[i] = (char)('A' + (last_romaji[i] - A));
    }
    cur[last_romaji_len] = '\0';

    for (int i = 0; i < (int)(sizeof romaji_steps / sizeof romaji_steps[0]); i++) {
        if (strcmp(cur, romaji_steps[i].from) == 0) {
            const char *to = romaji_steps[i].to;
            int to_len = strlen(to);
            int64_t ts = naginata_get_timestamp();
            // 未確定のローマ字を消して、変換形を打ち直す
            int kana_count = (last_romaji_len == 3 && cur[1] == 'Y') ? 2 : 1;
            for (int k = 0; k < kana_count; k++) {
                raise_zmk_keycode_state_changed_from_encoded(BSPC, true, ts);
                raise_zmk_keycode_state_changed_from_encoded(BSPC, false, ts);
            }
            for (int k = 0; k < to_len; k++) {
                uint32_t kc = (uint32_t)(A + (to[k] - 'A'));
                raise_zmk_keycode_state_changed_from_encoded(kc, true, ts);
                raise_zmk_keycode_state_changed_from_encoded(kc, false, ts);
                last_romaji[k] = kc;
            }
            last_romaji_len = to_len;
            return;
        }
    }
}

static void flush_deferred_input(void) {
    if (!defer_flush) {
        return;
    }

    defer_flush = false;
    if (nginput.size > 0 && number_of_candidates(&(nginput.elements[0])) == 1) {
        ng_type(&(nginput.elements[0]));
        removeFromListArrayAt(&nginput, 0);
    }
}

static void naginata_flush_work_handler(struct k_work *work) {
    (void)work;
    // Work items do not carry a key event timestamp. k_uptime_get() uses the
    // same millisecond clock as ZMK behavior events.
    naginata_set_timestamp(k_uptime_get());
    flush_deferred_input();
}

// nginputに入力を積む。満杯なら最古の入力を確定して空きを作ってから追加する。
// (戻り値を無視して黙ってキーを捨てるのを防ぐ)
static void push_input(NGList *e) {
    if (!addToListArray(&nginput, e)) {
        ng_type(&(nginput.elements[0]));
        removeFromListArrayAt(&nginput, 0);
        addToListArray(&nginput, e);
    }
}

// 薙刀式の入力処理
bool naginata_press(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    LOG_DBG(">NAGINATA PRESS");

    uint32_t keycode = binding->param1;

    switch (keycode) {
    case A ... Z:
    case SPACE:
    case ENTER:
    case DOT:
    case COMMA:
    case SLASH:
    case SEMI:
        n_pressed_keys++;
        pressed_keys |= ng_key[keycode - A]; // キーの重ね合わせ

        if (keycode == SPACE || keycode == ENTER) {
            NGList a;
            initializeList(&a);
            addToList(&a, keycode);
            push_input(&a);
        } else {
            last_pressed_keycode = keycode;
            last_press_timestamp = event.timestamp;
            NGList a;
            NGList b;
            if (nginput.size > 0) {
                copyList(&(nginput.elements[nginput.size - 1]), &a);
                copyList(&a, &b);
                addToList(&b, keycode);
            }

            // 前のキーとの同時押しの可能性があるなら前に足す
            // 同じキー連打を除外
            if (nginput.size > 0 && a.elements[a.size - 1] != keycode &&
                number_of_candidates(&b) > 0) {
                removeFromListArrayAt(&nginput, nginput.size - 1);
                addToListArray(&nginput, &b);
                // 前のキーと同時押しはない
            } else {
                // 連続シフトではない
                NGList e;
                initializeList(&e);
                addToList(&e, keycode);
                push_input(&e);
            }
        }

        // 連続シフト
        static const uint32_t rs[12][2] = {{D, F},     {C, V}, {J, K}, {M, COMMA}, {SPACE, 0},
                                     {ENTER, 0}, {F, 0}, {V, 0}, {J, 0},     {M, 0},
                                     {K, 0},     {D, 0}};

        uint32_t keyset = 0UL;
        for (int i = 0; i < nginput.elements[nginput.size - 1].size; i++) {
            keyset |= ng_key[nginput.elements[nginput.size - 1].elements[i] - A];
        }
        for (int i = 0; i < 12; i++) {
            NGList rskc;
            initializeList(&rskc);
            addToList(&rskc, rs[i][0]);
            if (rs[i][1] > 0) {
                addToList(&rskc, rs[i][1]);
            }

            int c = includeList(&rskc, keycode);
            uint32_t brs = 0UL;
            for (int j = 0; j < rskc.size; j++) {
                brs |= ng_key[rskc.elements[j] - A];
            }

            NGList l = nginput.elements[nginput.size - 1];
            bool overflow = false;
            for (int j = 0; j < l.size; j++) {
                if (!addToList(&rskc, l.elements[j])) {
                    overflow = true;
                    break;
                }
            }
            if (overflow) {
                continue;
            }

            if (c < 0 && ((brs & pressed_keys) == brs) && (keyset & brs) != brs && number_of_matches(&rskc) > 0) {
                nginput.elements[nginput.size - 1] = rskc;
                break;
            }
        }

        if (nginput.size > 1) {
            ng_type(&(nginput.elements[0]));
            removeFromListArrayAt(&nginput, 0);
            defer_flush = false;
            k_work_cancel_delayable(&naginata_flush_work);
        } else if (number_of_candidates(&(nginput.elements[0])) == 1) {
            // 文字キー同士の結合は、重なり時間が短いロールオーバーなら
            // 個別打鍵に分割される可能性があるため、いったん保留する。
            int n_moji = 0;
            for (int i = 0; i < nginput.elements[0].size; i++) {
                if (nginput.elements[0].elements[i] != SPACE &&
                    nginput.elements[0].elements[i] != ENTER) {
                    n_moji++;
                }
            }
            if (CONFIG_NAGINATA_MIN_OVERLAP_MS > 0 && n_moji >= 2) {
                defer_flush = true;
                k_work_reschedule(&naginata_flush_work,
                                  K_MSEC(CONFIG_NAGINATA_MIN_OVERLAP_MS));
            } else {
                ng_type(&(nginput.elements[0]));
                removeFromListArrayAt(&nginput, 0);
            }
        }
        break;
    }

    LOG_DBG("<NAGINATA PRESS");

    return true;
}

bool naginata_release(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    LOG_DBG(">NAGINATA RELEASE");

    uint32_t keycode = binding->param1;

    switch (keycode) {
    case A ... Z:
    case SPACE:
    case ENTER:
    case DOT:
    case COMMA:
    case SLASH:
    case SEMI:
        // 先に押していた文字キーが、後から押した文字キーとの重なり時間
        // 未満で離された場合、同時押しではなく個別打鍵として分割する。
        if (CONFIG_NAGINATA_MIN_OVERLAP_MS > 0 && nginput.size > 0 &&
            keycode != SPACE && keycode != ENTER) {
            NGList *last = &(nginput.elements[nginput.size - 1]);
            if (last->size >= 2 && keycode != last_pressed_keycode &&
                last->elements[last->size - 1] == last_pressed_keycode &&
                includeList(last, keycode) >= 0 &&
                event.timestamp - last_press_timestamp < CONFIG_NAGINATA_MIN_OVERLAP_MS) {
                last->size--;
                NGList e;
                initializeList(&e);
                addToList(&e, last_pressed_keycode);
                push_input(&e);
                defer_flush = false;
                k_work_cancel_delayable(&naginata_flush_work);

                // 分割で手前の要素はこれ以上結合されないので確定する。
                while (nginput.size > 1) {
                    ng_type(&(nginput.elements[0]));
                    removeFromListArrayAt(&nginput, 0);
                }
            }
        }

        if (n_pressed_keys > 0)
            n_pressed_keys--;
        if (n_pressed_keys == 0)
            pressed_keys = 0UL;

        pressed_keys &= ~ng_key[keycode - A]; // キーの重ね合わせ

        if (pressed_keys == 0UL) {
            defer_flush = false;
            k_work_cancel_delayable(&naginata_flush_work);
            while (nginput.size > 0) {
                ng_type(&(nginput.elements[0]));
                removeFromListArrayAt(&nginput, 0);
            }
        } else {
            if (nginput.size > 0 && number_of_candidates(&(nginput.elements[0])) == 1) {
                defer_flush = false;
                k_work_cancel_delayable(&naginata_flush_work);
                ng_type(&(nginput.elements[0]));
                removeFromListArrayAt(&nginput, 0);
            }
        }
        break;
    }

    LOG_DBG("<NAGINATA RELEASE");

    return true;
}

// 薙刀式

static int behavior_naginata_init(const struct device *dev) {
    LOG_DBG("NAGINATA INIT");

    initializeListArray(&nginput);
    pressed_keys = 0UL;
    n_pressed_keys = 0;
    last_press_timestamp = 0;
    last_pressed_keycode = 0;
    defer_flush = false;
    k_work_init_delayable(&naginata_flush_work, naginata_flush_work_handler);
#if defined(CONFIG_NAGINATA_DEFAULT_OS_WINDOWS)
    naginata_config.os = NG_WINDOWS;
#elif defined(CONFIG_NAGINATA_DEFAULT_OS_LINUX)
    naginata_config.os = NG_LINUX;
#else
    naginata_config.os = NG_MACOS;
#endif

#if IS_ENABLED(CONFIG_NAGINATA_PERSISTENT_STATE)
    k_work_init_delayable(&naginata_save_work, naginata_save_work_handler);
#endif
    return 0;
};

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    // F15が押されたらnaginata_config.os=NG_WINDOWS
    {
#if IS_ENABLED(CONFIG_NAGINATA_PERSISTENT_STATE)
        uint8_t saved_os = naginata_config.os;
        bool saved_tategaki = naginata_config.tategaki;
#endif
        switch (binding->param1) {
            case F15:
                naginata_config.os = NG_WINDOWS;
                break;
            case F16:
                naginata_config.os = NG_MACOS;
                break;
            case F17:
                naginata_config.os = NG_LINUX;
                break;
            case F18:
                naginata_config.tategaki = true;
                break;
            case F19:
                naginata_config.tategaki = false;
                break;
        }
        switch (binding->param1) {
            case F15 ... F19:
#if IS_ENABLED(CONFIG_NAGINATA_PERSISTENT_STATE)
                if (naginata_config.os != saved_os || naginata_config.tategaki != saved_tategaki) {
                    // Debounce saving to flash to reduce wear
                    k_work_reschedule(&naginata_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
                }
#endif
                return ZMK_BEHAVIOR_OPAQUE;
        }
    }

    naginata_set_timestamp(event.timestamp);
    naginata_press(binding, event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    naginata_set_timestamp(event.timestamp);
    naginata_release(binding, event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_naginata_driver_api = {
    .binding_pressed = on_keymap_binding_pressed, .binding_released = on_keymap_binding_released};

#define KP_INST(n)                                                                                 \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_naginata_init, NULL, NULL, NULL, POST_KERNEL,              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_naginata_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)
