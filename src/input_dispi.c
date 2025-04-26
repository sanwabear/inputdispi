#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "raylib.h"
#include "rlgl.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <wchar.h>
#include <locale.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <signal.h>
#include <termios.h>

#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1080
#define BG1_WIDTH 240
#define BG2_WIDTH 30
#define MAX_LOG 22
#define TRAJECTORY_LENGTH 15
#define LINE_HEIGHT 36
#define STATUS_Y (LINE_HEIGHT * 2)
#define LOG_X 80
#define LOG_Y (LINE_HEIGHT * 3)
#define MAX_FRAME_COUNT 1000

#define COUNT_CACHE_SIZE 1001
#define DIR_STATE_COUNT 16
#define BTN_STATE_COUNT 16

#define LEFT 0
#define RIGHT 1
#define CENTER 2

// ・↖↗↙↘ が収録されているフォント
#define FONT_PATH "fonts/InputDispi.otf"
#define FONT_SIZE 32

// ロックファイル用ファイルディスクリプタ（多重起動防止機能で使用）
static int lock_fd = -1;

/**
 * @brief プログラム起動時にロックファイルを作成し排他ロックを取得する。
 *        すでにロック取得済みなら多重起動と判断してエラー終了する。
 */
static void acquire_lock_or_exit(void)
{
    lock_fd = open("/var/lock/input_dispi.lock", O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0)
    {
        perror("open lock file failed");
        exit(EXIT_FAILURE);
    }

    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0)
    {
        fprintf(stderr, "Another instance of input_dispi is already running.\n");
        close(lock_fd);
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief プログラム終了時にロックファイルを解放する。
 *        正常終了時に呼び出しリソースリークを防止する。
 */
static void release_lock(void)
{
    if (lock_fd >= 0)
    {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

static volatile sig_atomic_t exit_requested = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    exit_requested = 1;
}

/**
 * @brief SIGINTシグナルハンドラを登録する。
 *        安全なプログラム中断を可能にする。
 */
static void setup_signal_handlers(void)
{
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

/**
 * @brief ターミナル状態の復旧をする。
 */
static void reset_terminal_mode(void)
{
    struct termios term;

    if (tcgetattr(STDIN_FILENO, &term) == -1)
    {
        perror("tcgetattr");
        return;
    }

    term.c_lflag |= (ICANON | ECHO); // カノニカルモード（行編集）とエコーON
    if (tcsetattr(STDIN_FILENO, TCSANOW, &term) == -1)
    {
        perror("tcsetattr");
    }
}

static char *text = "•・↖↗↙↘↑↓←→ABCD0123456789LOTあいうえお";
static bool show_debug = false;
static bool rt_debug_state = false;
static bool delete_requested = false;
static bool debug_triggered = false;
static bool prev_debug_state = false;

typedef struct
{
    unsigned char up : 1;
    unsigned char down : 1;
    unsigned char left : 1;
    unsigned char right : 1;
    unsigned char a : 1;
    unsigned char b : 1;
    unsigned char c : 1;
    unsigned char d : 1;
    unsigned char dir_index : 4; // 上下左右状態のビット値を合成した計算省略用フィールド
    unsigned char btn_index : 4;  // ABCD状態のビット値を合成した計算省略用フィールド
    unsigned short int count : 10; // フレームカウント0-1000まで
} InputState;

/**
 * @brief 各ビット値の合計フィールドを比較して同値なら真値を返す。
 */
static inline bool is_equal_state(const InputState *a, const InputState *b)
{
    return a->dir_index == b->dir_index && a->btn_index == b->btn_index;
}

typedef struct
{
    char text[8]; // 元文字列
    int *codepoints; // 文字列と一致するコードポイントバッファ
    int codepoint_count; // コードポイント数=文字数
} CachedText;

static void init_cached_text(CachedText *c, const char *s)
{
    memset(c->text, 0, sizeof(c->text));
    strncpy(c->text, s, sizeof(c->text) - 1);
    c->codepoints = LoadCodepoints(c->text, &c->codepoint_count);
}

static CachedText count_cache[COUNT_CACHE_SIZE];
static CachedText dir_cache[DIR_STATE_COUNT];
static CachedText button_cache[BTN_STATE_COUNT];

static void init_codepoint_cache()
{
    char buf[8];
    for (int i = 0; i < 1000; i++)
    {
        snprintf(buf, sizeof(buf), "%03d", i);
        init_cached_text(&count_cache[i], buf);
    }
    init_cached_text(&count_cache[1000], "LOT");

    const char *nt = "•", *up = "↑", *down = "↓", *left = "←", *right = "→",
               *ul = "↖", *ur = "↗", *dl = "↙", *dr = "↘";
    const char *directions[DIR_STATE_COUNT] = {
        nt,    // 0x00 neutral
        up,    // 0x01
        down,  // 0x02
        nt,    // 0x03 (↑↓ cancel)
        left,  // 0x04
        ul,    // 0x05
        dl,    // 0x06
        left,  // 0x07 (↑↓+←)
        right, // 0x08
        ur,    // 0x09
        dr,    // 0x0A
        right, // 0x0B (↑↓+→)
        nt,    // 0x0C (←→ cancel)
        up,    // 0x0D (←→+↑)
        down,  // 0x0E (←→+↓)
        nt     // 0x0F (↑↓←→ cancel)
    };
    for (int i = 0; i < DIR_STATE_COUNT; i++)
    {
        init_cached_text(&dir_cache[i], directions[i]);
    }

    for (int i = 0; i < BTN_STATE_COUNT; i++)
    {
        int len = 0;
        char b[8] = "";
        if (i & 0x1)
            b[len++] = 'A';
        if (i & 0x2)
            b[len++] = 'B';
        if (i & 0x4)
            b[len++] = 'C';
        if (i & 0x8)
            b[len++] = 'D';
        b[len] = '\0';
        init_cached_text(&button_cache[i], b);
    }
}

static void cleanup_codepoint_cache()
{
    for (int i = 0; i < COUNT_CACHE_SIZE; i++)
        if (count_cache[i].codepoints)
            UnloadCodepoints(count_cache[i].codepoints);
    for (int i = 0; i < DIR_STATE_COUNT; i++)
        if (dir_cache[i].codepoints)
            UnloadCodepoints(dir_cache[i].codepoints);
    for (int i = 0; i < BTN_STATE_COUNT; i++)
        if (button_cache[i].codepoints)
            UnloadCodepoints(button_cache[i].codepoints);
}

static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
// ティアリング検知用バッファ
InputState lastDrawnState1 = {0}, lastDrawnState2 = {0};

// 状態更新スレッドと入力検知スレッド用の中間バッファ
static InputState realtime_state1 = {0}, realtime_state2 = {0};
static bool cur_debug_state = false;

// 描画スレッドと状態更新スレッド用の中間バッファ
static InputState drawable_state1 = {0}, drawable_state2 = {0};
static int drawable_count1 = 1, drawable_count2 = 1;
static InputState drawable_traj1[TRAJECTORY_LENGTH] = {0};
static InputState drawable_traj2[TRAJECTORY_LENGTH] = {0};
static InputState drawableLogP1[MAX_LOG] = {0};
static InputState drawableLogP2[MAX_LOG] = {0};
static bool drawable1, drawable2;

static Font font;
static Font font2;

/**
 * @brief 文字表示用のユーティリティです。
 */
static void draw_text(const CachedText *c, int x, int y, int align)
{
    if (!c || !c->codepoints || c->codepoint_count <= 0)
        return;
    Vector2 sizeVec = MeasureTextEx(font, c->text, FONT_SIZE, 1);
    int base_x = x;
    if (align == CENTER)
        base_x = x - sizeVec.x / 2;
    else if (align == RIGHT)
        base_x = x - sizeVec.x;
    DrawTextCodepoints(font, c->codepoints, c->codepoint_count, (Vector2){base_x, y}, FONT_SIZE, 2, WHITE);
}

/**
 * @brief レバーとボタンおよびフレームカウントの入力ログを表示する。
 */
static void draw_logs(const InputState *states, int x, int baseY, int align_right, int len)
{
    for (int i = 0; i < len; ++i)
    {
        if (states[i].count == 0)
            continue;

        CachedText *direction = &dir_cache[states[i].dir_index];
        CachedText *buttons = &button_cache[states[i].btn_index];
        int y = baseY + i * LINE_HEIGHT;

        if (align_right)
        {
            int dx = x - LOG_X;
            Vector2 btnSize = MeasureTextEx(font, buttons->text, FONT_SIZE, 1);
            draw_text(direction, dx - btnSize.x, y, RIGHT);        // 方向
            draw_text(buttons, dx, y, RIGHT);                      // ボタン
            draw_text(&count_cache[states[i].count], x, y, RIGHT); // フレームカウント
        }
        else
        {
            draw_text(&count_cache[states[i].count], x, y, LEFT); // フレームカウント
            draw_text(direction, x + LOG_X, y, LEFT);             // 方向
            Vector2 dirSize = MeasureTextEx(font, direction->text, FONT_SIZE, 1);
            draw_text(buttons, x + LOG_X + dirSize.x, y, LEFT); // ボタン
        }
    }
}

/**
 * @brief draw_stick_and_buttonsで使用するサブ関数でボタンラベルと円を表示する。
 */
static void draw_button_label(int btn_index, bool active, int x, int y)
{
    Color color = active ? (Color){255, 255, 255, 255} : (Color){128, 128, 128, 255};
    if (btn_index == 0x1)
        color = active ? RED : (Color){96, 0, 0, 255};
    else if (btn_index == 0x2)
        color = active ? GOLD : (Color){96, 96, 0, 255};
    else if (btn_index == 0x4)
        color = active ? LIME : (Color){0, 96, 0, 255};
    else if (btn_index == 0x8)
        color = active ? SKYBLUE : (Color){0, 96, 96, 255};
    else
        return;
    DrawCircleV((Vector2){x, y}, 18, color);
    draw_text(&button_cache[btn_index], x, y - FONT_SIZE / 2 + 2, CENTER);
}

/**
 * @brief draw_stick_and_buttonsで使用するレバー位置の事前キャッシュを行う。
 */
static void init_stick_vector_cache(Vector2 *cache, int cx, int cy, int radius)
{
    for (int i = 0; i < 16; i++)
    {
        int dir = -1;
        int up = i & 0x01, down = i & 0x02, left = i & 0x04, right = i & 0x08;

        if (up && left && !down && !right)
            dir = 5;
        else if (up && right && !down && !left)
            dir = 7;
        else if (down && left && !up && !right)
            dir = 3;
        else if (down && right && !up && !left)
            dir = 1;
        else if (up && !down && !left && !right)
            dir = 6;
        else if (down && !up && !left && !right)
            dir = 2;
        else if (left && !right && !up && !down)
            dir = 4;
        else if (right && !left && !up && !down)
            dir = 0;

        if (dir == -1)
        {
            cache[i] = (Vector2){cx, cy};
        }
        else
        {
            float angle = (PI / 4.0f) * dir;
            cache[i] = (Vector2){cx + radius * cosf(angle), cy + radius * sinf(angle)};
        }
    }
}

static Vector2 stick_vector_cache1[16];
static Vector2 stick_vector_cache2[16];

/**
 * @brief レバーとボタンの入力状態をビジュアル表現する。
 */
static void draw_stick_and_buttons(const InputState *state, int base_x, int baseY, InputState *history, Vector2 *stick_vector_cache)
{
    int x = base_x + 80;
    DrawRectangleRounded((Rectangle){base_x - 45, baseY - 45, 90, 90}, 0.3f, 8, WHITE);
    for (int i = TRAJECTORY_LENGTH - 1; i > 0; i--)
    {
        Vector2 p1 = stick_vector_cache[history[i].dir_index];
        Vector2 p2 = stick_vector_cache[history[i - 1].dir_index];
        Color c = (Color){128 + 127 * i / TRAJECTORY_LENGTH, 0, 255 - 127 * i / TRAJECTORY_LENGTH, 255};
        DrawLineEx(p1, p2, 12, c);
    }
    DrawCircleV(stick_vector_cache[state->dir_index], 14, RED);
    draw_button_label(0x1, state->a, x, baseY);
    draw_button_label(0x2, state->b, x + 28, baseY - 25);
    draw_button_label(0x4, state->c, x + 64, baseY - 32);
    draw_button_label(0x8, state->d, x + 100, baseY - 30);
}

/**
 * @brief コードポイントから重複するものを取り除いてユニークなもののみをにして返す。
 */
static int *codepoint_remove_duplicates(int *codepoints, int codepoint_count, int *codepoints_result_count)
{
    if (!codepoints || codepoint_count <= 0 || !codepoints_result_count)
        return NULL;

    int *codepoints_no_dups = (int *)calloc(codepoint_count, sizeof(int));
    if (!codepoints_no_dups)
        return NULL;

    int unique_count = 0;
    for (int i = 0; i < codepoint_count; i++)
    {
        bool duplicate = false;
        for (int j = 0; j < unique_count; j++)
        {
            if (codepoints[i] == codepoints_no_dups[j])
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            codepoints_no_dups[unique_count++] = codepoints[i];
        }
    }

    *codepoints_result_count = unique_count;
    return codepoints_no_dups;
}

/**
 * @brief 4ビットで上下左右状態を表現したデータを返す。
 */
static unsigned char conv_dir_index(bool up, bool down, bool left, bool right)
{
    unsigned char dir_index = 0;
    if (up)
        dir_index |= 0x1;
    if (down)
        dir_index |= 0x2;
    if (left)
        dir_index |= 0x4;
    if (right)
        dir_index |= 0x8;
    return dir_index;
}

/**
 * @brief 4ビットでABCDボタン状態を表現したデータを返す。
 */
static unsigned char conv_button_index(bool a, bool b, bool c, bool d)
{
    unsigned char btn_index = 0;
    if (a)
        btn_index |= 0x1;
    if (b)
        btn_index |= 0x2;
    if (c)
        btn_index |= 0x4;
    if (d)
        btn_index |= 0x8;
    return btn_index;
}

/**
 * @brief 最新の入力データを1000FPSでメモリに記録して状態管理スレッドに移譲する。
 */
void *input_thread(void *arg)
{
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 1000000}; // 1ms
    InputState new_state1 = (InputState){0}, new_state2 = (InputState){0};

    printf("[info] state_thread started\n");

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    while (!exit_requested)
    {
        pthread_testcancel();

        // トグル用：1+5 or 2+6 が同時押し
        bool switch_debug = false;
        if ((IsKeyDown(KEY_ONE) && IsKeyDown(KEY_FIVE)) ||
            (IsKeyDown(KEY_TWO) && IsKeyDown(KEY_SIX)))
        {
            switch_debug = true;
        }

        struct timespec frame_start, frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        // 1P
        bool up1 = IsKeyDown(KEY_W);
        bool down1 = IsKeyDown(KEY_S);
        bool left1 = IsKeyDown(KEY_A);
        bool right1 = IsKeyDown(KEY_D);
        bool a1 = IsKeyDown(KEY_N);
        bool b1 = IsKeyDown(KEY_M);
        bool c1 = IsKeyDown(KEY_COMMA);
        bool d1 = IsKeyDown(KEY_PERIOD);

        // 2P
        bool up2 = IsKeyDown(KEY_UP);
        bool down2 = IsKeyDown(KEY_DOWN);
        bool left2 = IsKeyDown(KEY_LEFT);
        bool right2 = IsKeyDown(KEY_RIGHT);
        bool a2 = IsKeyDown(KEY_KP_1);
        bool b2 = IsKeyDown(KEY_KP_2);
        bool c2 = IsKeyDown(KEY_KP_3);
        bool d2 = IsKeyDown(KEY_KP_4);

        new_state1 = (InputState){
            up1, down1, left1, right1,
            a1, b1, c1, d1,
            conv_dir_index(up1, down1, left1, right1),
            conv_button_index(a1, b1, c1, d1),
            0};
        new_state2 = (InputState){
            up2, down2, left2, right2,
            a2, b2, c2, d2,
            conv_dir_index(up2, down2, left2, right2),
            conv_button_index(a2, b2, c2, d2),
            0};

        // 状態更新スレッドへ値連携
        if (pthread_mutex_lock(&input_lock) == 0)
        {
            realtime_state1 = new_state1;
            realtime_state2 = new_state2;
            rt_debug_state = switch_debug;

            pthread_mutex_unlock(&input_lock);
        }
        else
        {
            fprintf(stderr, "pthread_mutex_lock input_lock failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        nanosleep(&interval, NULL);
    }
    return NULL;
}

/**
 * @brief 入力データを60FPSで状態保存する。
 */
void *state_thread(void *arg)
{
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 16666666}; // 60FPS

    unsigned int temp_count1 = 0, temp_count2 = 0;
    int no_op_count1 = -1, no_op_count2 = -1;
    InputState current_state1 = {0}, current_state2 = {0};
    InputState prev_state1 = {0}, prev_state2 = {0};
    InputState trajectory1[TRAJECTORY_LENGTH] = {0};
    InputState trajectory2[TRAJECTORY_LENGTH] = {0};
    InputState log_1[MAX_LOG] = {0};
    InputState log_2[MAX_LOG] = {0};

    // 入力状態とログの初期化
    current_state1 = prev_state1 = (InputState){0};
    current_state2 = prev_state2 = (InputState){0};
    for (int i = 0; i < TRAJECTORY_LENGTH; i++)
    {
        trajectory1[i] = (InputState){0};
        trajectory2[i] = (InputState){0};
    }
    for (int i = 0; i < MAX_LOG; i++)
    {
        log_1[i] = (InputState){0};
        log_2[i] = (InputState){0};
    }
    current_state1.count = current_state2.count = 1;

    printf("[info] state_thread started\n");

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    while (!exit_requested)
    {
        pthread_testcancel();

        struct timespec frame_start, frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        // 入力検知用の構造体で上書きされるので退避
        temp_count1 = current_state1.count;
        temp_count2 = current_state2.count;

        // 入力検知スレッドから値連携
        if (pthread_mutex_lock(&input_lock) == 0)
        {
            current_state1 = realtime_state1;
            current_state2 = realtime_state2;
            cur_debug_state = rt_debug_state;

            pthread_mutex_unlock(&input_lock);
        }
        else
        {
            fprintf(stderr, "pthread_mutex_lock input_lock failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        // 入力検知用の構造体で上書きされたものを復旧
        no_op_count1 = current_state1.count = temp_count1;
        no_op_count2 = current_state2.count = temp_count2;

        // trajectoryとログ、カウント更新、DELによるリセット処理をここで統合

        // DELキーで初期化
        bool delkey = IsKeyPressed(KEY_DELETE);
        // 30秒間無操作（約1800フレーム）でリセット
        if (delkey || no_op_count1 >= 1800)
        {
            current_state1 = prev_state1 = (InputState){0};
            for (int i = 0; i < TRAJECTORY_LENGTH; i++)
                trajectory1[i] = (InputState){0};
            for (int i = 0; i < MAX_LOG; i++)
                log_1[i] = (InputState){0};
            no_op_count1 = delkey ? 0 : -1;
        }
        if (delkey || no_op_count2 >= 1800)
        {
            current_state2 = prev_state2 = (InputState){0};
            for (int i = 0; i < TRAJECTORY_LENGTH; i++)
                trajectory2[i] = (InputState){0};
            for (int i = 0; i < MAX_LOG; i++)
                log_2[i] = (InputState){0};
            no_op_count2 = delkey ? 0 : -1;
        }

        // フレームカウントとログ
        if (is_equal_state(&current_state1, &prev_state1))
        {
            if (current_state1.count < MAX_FRAME_COUNT)
                current_state1.count++;
            if (no_op_count1 != -1)
                no_op_count1++;
        }
        else
        {
            memmove(&log_1[1], &log_1[0], sizeof(InputState) * (MAX_LOG - 1));
            log_1[0] = prev_state1;
            current_state1.count = no_op_count1 = 1;
            prev_state1 = current_state1;
        }
        if (is_equal_state(&current_state2, &prev_state2))
        {
            if (current_state2.count < MAX_FRAME_COUNT)
                current_state2.count++;
            if (no_op_count2 != -1)
                no_op_count2++;
        }
        else
        {
            memmove(&log_2[1], &log_2[0], sizeof(InputState) * (MAX_LOG - 1));
            log_2[0] = prev_state2;
            current_state2.count = no_op_count2 = 1;
            prev_state2 = current_state2;
        }

        // デバッグフラグ更新
        if (cur_debug_state && !prev_debug_state && !debug_triggered)
        {
            show_debug ^= 1;
            debug_triggered = true;
        }
        if (!cur_debug_state)
        {
            debug_triggered = false;
        }
        prev_debug_state = cur_debug_state;

        // 軌跡更新
        memmove(&trajectory1[1], &trajectory1[0], sizeof(InputState) * (TRAJECTORY_LENGTH - 1));
        trajectory1[0] = current_state1;
        memmove(&trajectory2[1], &trajectory2[0], sizeof(InputState) * (TRAJECTORY_LENGTH - 1));
        trajectory2[0] = current_state2;

        // 描画スレッドへ値連携
        if (pthread_mutex_lock(&state_lock) == 0)
        {
            drawable_state1 = current_state1;
            memcpy(drawable_traj1, trajectory1, sizeof(trajectory1));
            memcpy(drawableLogP1, log_1, sizeof(log_1));
            drawable1 = (no_op_count1 != -1);

            drawable_state2 = current_state2;
            memcpy(drawable_traj2, trajectory2, sizeof(trajectory2));
            memcpy(drawableLogP2, log_2, sizeof(log_2));
            drawable2 = (no_op_count2 != -1);

            pthread_mutex_unlock(&state_lock);
        }
        else
        {
            fprintf(stderr, "pthread_mutex_lock state_lock failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        // インターバルのパディング
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        long elapsed_ns = (frame_end.tv_sec - frame_start.tv_sec) * 1000000000L +
                          (frame_end.tv_nsec - frame_start.tv_nsec);
        long target_ns = 16666666L;
        long remaining_ns = target_ns - elapsed_ns;
        if (remaining_ns > 0)
        {
            struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = remaining_ns};
            nanosleep(&sleep_time, NULL);
        }
    }
    return NULL;
}

/**
 * @brief プログラムのエントリポイント。
 *        起動時にロック取得、初期化、描画ループ開始。
 *        SIGINTやウィンドウクローズ要求で終了シーケンスに移行し、
 *        全スレッドキャンセル・ロック解放を行う。
 */
int main()
{
    acquire_lock_or_exit();
    setup_signal_handlers();

    setlocale(LC_CTYPE, "");
    if (!setlocale(LC_CTYPE, "ja_JP.UTF-8"))
    {
        fprintf(stderr, "Failed to set locale ja_JP.UTF-8.\n");
        return 1;
    }

    SetConfigFlags(FLAG_WINDOW_UNDECORATED | FLAG_FULLSCREEN_MODE);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "input_dispi raylib");

    init_codepoint_cache();
    int codepoint_count = 0;
    int *codepoints = LoadCodepoints(text, &codepoint_count);
    int codepoints_no_dups_count = 0;
    int *codepoints_no_dups = codepoint_remove_duplicates(codepoints, codepoint_count, &codepoints_no_dups_count);
    UnloadCodepoints(codepoints);
    font = LoadFontEx(FONT_PATH, FONT_SIZE, codepoints_no_dups, codepoints_no_dups_count);
    font2 = LoadFontEx(FONT_PATH, FONT_SIZE + 2, codepoints_no_dups, codepoints_no_dups_count);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    free(codepoints_no_dups);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024); // スタックサイズ1MB
    pthread_t tid;
    if (pthread_create(&tid, &attr, input_thread, NULL) != 0)
    {
        perror("[error] input_thread creation failed\n");
        return 1;
    }
    else
    {
        printf("[info] input_thread created\n");
    }
    pthread_t state_tid;
    if (pthread_create(&state_tid, &attr, state_thread, NULL) != 0)
    {
        perror("[error] state_thread creation failed\n");
        return 1;
    }
    else
    {
        printf("[info] state_thread created\n");
    }
    pthread_attr_destroy(&attr);

    struct sched_param param;
    param.sched_priority = 40;
    pthread_setschedparam(state_tid, SCHED_FIFO, &param);

    SetTargetFPS(60);

    InputState draw_state1 = (InputState){0}, draw_state2 = (InputState){0};
    InputState draw_trajectory1[TRAJECTORY_LENGTH] = {0};
    InputState draw_trajectory2[TRAJECTORY_LENGTH] = {0};
    InputState draw_log1[MAX_LOG] = {0};
    InputState draw_log2[MAX_LOG] = {0};
    bool draw1, draw2;

    // 入力状態とログの初期化
    for (int i = 0; i < TRAJECTORY_LENGTH; i++)
    {
        draw_trajectory1[i] = (InputState){0};
        draw_trajectory2[i] = (InputState){0};
    }
    for (int i = 0; i < MAX_LOG; i++)
    {
        draw_log1[i] = (InputState){0};
        draw_log2[i] = (InputState){0};
    }
    draw_state1.count = draw_state2.count = 1;

    printf("[info] main_thread started\n");

    bool first = true;

    Color bg1 = (Color){200, 200, 200, 48};
    Color bg2 = (Color){200, 200, 200, 24};
    Color bg3 = (Color){200, 200, 200, 0};

    while (!WindowShouldClose() && !exit_requested)
    {
        struct timespec frame_start, frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        // 状態更新スレッドから値連携
        if (pthread_mutex_lock(&state_lock) == 0)
        {
            draw_state1 = drawable_state1;
            memcpy(draw_trajectory1, drawable_traj1, sizeof(drawable_traj1));
            memcpy(draw_log1, drawableLogP1, sizeof(drawableLogP1));
            draw1 = drawable1;

            draw_state2 = drawable_state2;
            memcpy(draw_trajectory2, drawable_traj2, sizeof(drawable_traj2));
            memcpy(draw_log2, drawableLogP2, sizeof(drawableLogP2));
            draw2 = drawable2;

            pthread_mutex_unlock(&state_lock);
        }
        else
        {
            fprintf(stderr, "pthread_mutex_lock state_lock failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        BeginDrawing();

        ClearBackground(BLACK); // #000000 キーカラー
        DrawRectangleGradientH(0, 0, BG1_WIDTH, SCREEN_HEIGHT, bg1, bg2);
        DrawRectangleGradientH(BG1_WIDTH, 0, BG2_WIDTH, SCREEN_HEIGHT, bg2, bg3);
        DrawRectangleGradientH(SCREEN_WIDTH - BG1_WIDTH, 0, BG1_WIDTH, SCREEN_HEIGHT, bg2, bg1);
        DrawRectangleGradientH(SCREEN_WIDTH - BG1_WIDTH - BG2_WIDTH, 0, BG2_WIDTH, SCREEN_HEIGHT, bg3, bg2);

        if (first)
        {
            first = false;
            init_stick_vector_cache(stick_vector_cache1, 80, 980, LINE_HEIGHT);   // 1P
            init_stick_vector_cache(stick_vector_cache2, 1680, 980, LINE_HEIGHT); // 2P
        }
        draw_stick_and_buttons(&draw_state1, 80, 980, draw_trajectory1, stick_vector_cache1);
        draw_stick_and_buttons(&draw_state2, 1680, 980, draw_trajectory2, stick_vector_cache2);

        if (draw1)
        {
            draw_logs(&draw_state1, 40, STATUS_Y, LEFT, 1);
            draw_logs(draw_log1, 40, LOG_Y, LEFT, MAX_LOG);
        }
        if (draw2)
        {
            draw_logs(&draw_state2, 1860, STATUS_Y, RIGHT, 1);
            draw_logs(draw_log2, 1860, LOG_Y, RIGHT, MAX_LOG);
        }

        if (show_debug)
            DrawFPS(10, 10);

        EndDrawing();
    }

    pthread_cancel(tid);
    pthread_cancel(state_tid);
    pthread_join(tid, NULL);
    pthread_join(state_tid, NULL);

    UnloadFont(font);
    UnloadFont(font2);
    cleanup_codepoint_cache();
    CloseWindow();

    reset_terminal_mode();

    release_lock();
    return 0;
}
