#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "raylib.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <locale.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <signal.h>
#include <termios.h>

// 画面のサイズ1920x1920
#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1080

// 左右のログ表示エリアの透過グラデーション領域の幅
// 濃い目のグラデーションで入力を強調して見栄え用に透過を強めて終端させる
#define BG1_WIDTH 240 // メイン領域幅
#define BG2_WIDTH 30  // 見栄え用の終端グラデーション幅

#define MAX_LOG 22        // 最大入力ログ表示数。画面の縦幅に併せた件数にする
#define MAX_TRAJECTORY 15 // 最大レバー軌跡数。描画の負担にならない程度にする
#define LINE_HEIGHT 36    // 行高さ。フォントサイズと調整した高さにする

// レバー軌跡とボタン状態の表示オフセット
#define STATUS_X1 80   // 1Pログ表示の左端
#define STATUS_X2 1680 // 2Pログ表示の左端
#define STATUS_Y 980   // 1P2P共通の上端

// ログ表示オフセット
#define LOG_X1 40                 // 1Pログ表示の左端
#define LOG_X2 1860               // 2Pログ表示の左端
#define LOG_X_FIX 80              // ログ表示の個別の補正幅
#define LOG_Y (LINE_HEIGHT * 3.5) // 1P2P共通の上端
#define MAX_FRAME_COUNT 1000
#define RESET_FRAME_COUNT 1800 // 30秒間無操作（約1800フレーム）でリセット

// 文字のコードポイントキャッシュ数
#define COUNT_CACHE_SIZE 1001 // フレームカウント文字列000～999およびLOTのキャッシュ数
#define DIR_STATE_COUNT 16    // レバー状態ビット構成の0～Fにあわせた上下左右文字のキャッシュ数
#define BTN_STATE_COUNT 16    // ボタン状態ビット構成の0～Fにあわせた上下左右文字のキャッシュ数
// レバー状態もしくはボタン状態のビット列は次の仕様になります
// 0 0 0 0
// | | | |       stick  button
// | | | |       ------ ------
// | | | `-- 0x1 UP     A
// | | `---- 0x2 DOWN   B
// | `------ 0x4 LEFT   C
// `-------- 0x8 RIGHT  D

// 文字表示関数用の寄せ方向
#define LEFT 0
#define RIGHT 1
#define CENTER 2

// ロックファイル
#define LOCK_FILE_PATH "/tmp/input_dispi.lock"

// ・↖↗↙↘ が収録されているフォント
#define FONT_PATH "fonts/InputDispi.otf"
#define FONT_SIZE 32

// 状態表示で使う文字サイズと連動したボタンサイズ
#define BTN_SIZE (FONT_SIZE * 0.5625) // ボタンサイズ
#define BTN_Y_FIX (FONT_SIZE * 0.4)   // 文字表示位置の補正値

static Font font; // 全体共通のフォント変数

// ロックファイル用ファイルディスクリプタ（多重起動防止機能で使用）
static int lock_fd = -1;

/**
 * @brief プログラム起動時にロックファイルを作成し排他ロックを取得する。
 *        すでにロック取得済みなら多重起動と判断してエラー終了する。
 *        ただし、取得失敗したときに古いロックファイルを削除して1回だけリトライし、
 *        それでも取れなければエラー終了する。
 *        これにより、たまたま残っていたロックファイルによる起動失敗を防げ、
 *        ユーザーがいちいちロックファイルを手で消す必要がない振る舞いをします。
 */
void acquire_lock_or_exit(void)
{
    lock_fd = open(LOCK_FILE_PATH, O_RDWR | O_CREAT, 0666);
    if (lock_fd == -1)
    {
        perror("open (lock file)");
        exit(EXIT_FAILURE);
    }

    if (flock(lock_fd, LOCK_EX | LOCK_NB) == -1)
    {
        // まず警告
        perror("flock (lock file) - retrying after cleanup");

        // ロックファイル強制削除→リトライ
        unlink(LOCK_FILE_PATH);
        close(lock_fd);

        lock_fd = open(LOCK_FILE_PATH, O_RDWR | O_CREAT, 0666);
        if (lock_fd == -1)
        {
            perror("re-open (lock file)");
            exit(EXIT_FAILURE);
        }
        if (flock(lock_fd, LOCK_EX | LOCK_NB) == -1)
        {
            perror("flock (lock file) after retry");
            exit(EXIT_FAILURE);
        }
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

// raylibのテキスト描画はコードポイントを事前に登録してそこから文字列を指定する
// 必要になるテキストを事前に用意しておきユニークなコードポイント列として保存する
static char *text = "•・↖↗↙↘↑↓←→ABCD0123456789LOTあいうえお";

// デバッグ表示はスタート+セレクト同時押しのトグル方式になるため
// トグル用に前回状態を保存することと
// 入力検知からの状態変更、表示スレッドへ順に連携させていく必要がある
static bool show_debug = false; // デバッグ状態のフラグ本体
static bool rt_debug_state = false; // 入力検知時のバッファ
static bool debug_triggered = false; // 更新トリガー用のバッファ
static bool prev_debug_state = false; // 前回状態

// レバー状態とボタン状態と継続フレームカウントの構造体
// コードポイントキャッシュから文字列を解決するためのインデックスと有限カウンタでの構成としている。
typedef struct
{
    unsigned char dir_index : 4;   // 上下左右状態のビット値を合成した計算省略用フィールド
    unsigned char btn_index : 4;   // ABCD状態のビット値を合成した計算省略用フィールド
    unsigned short int count : 10; // フレームカウント0-1000まで
} LogState;

// 入力検知用状態フラグ構造体
// 1000Hzで動作する入力検知用スレッドで高速に更新をしていくため単純なON/OFF構成にしている。
// 状態管理スレッドでフラグ判定からのビット加算してLogStateへ変換するために利用する。
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
} InputState;

/**
 * @brief 各ビット値の合計フィールドを比較して同値なら真値を返す。
 */
static inline bool is_neutral(const LogState *a)
{
    return a->dir_index == 0 && a->btn_index == 0;
}

/**
 * @brief 各ビット値の合計フィールドを比較して同値なら真値を返す。
 */
static inline bool is_equal_state(const LogState *a, const LogState *b)
{
    return a->dir_index == b->dir_index && a->btn_index == b->btn_index;
}

// 文字キャッシュ構造体
// 文字列を逐次コードポイントに変換して使用後に破棄すると非効率であるため
// 事前に使用するすべての文字パターンをキャッシュとして保持するようにする。
// 描画に必要なコードポイントとその長さで構成している。
// 元文字列を保持しているのはコードポイントではなく文字列そのものを要求する関数の利用があるため。
// レバー状態とボタンの組み合わせが上限となるため文字列長は短めで設定している。
// 
// [入力ログ表示の仕様]
// 000 →ABCD
// ~~~ ~~~~~~
//  |   |
//  |   `----- 最大5文字: レバー状態とボタンの組み合わせ
//  `--------- 最大3文字: 000～999もしくはLOT
typedef struct
{
    char text[5];        // 元文字列
    int *codepoints;     // 文字列と一致するコードポイントバッファ
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
    for (int i = 0; i < MAX_FRAME_COUNT; i++)
    {
        snprintf(buf, sizeof(buf), "%03d", i);
        init_cached_text(&count_cache[i], buf);
    }
    init_cached_text(&count_cache[MAX_FRAME_COUNT], "LOT");

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

// 状態更新スレッドと入力検知スレッド用の中間バッファ
static InputState realtime_state1 = {0}, realtime_state2 = {0};
static bool cur_debug_state = false;

// 描画スレッドと状態更新スレッド用の中間バッファ
// 軌跡と入力ログは固定長配列。
// 添え字が少ないほど最新のもので入力ログの最新データはカウントアップされていく。
// フラグにより1P、2Pそれぞれの描画有無を制御する。
// 一定時間入力がない場合はデータ初期化のうえ描画を抑制して可視性をよくする。
static unsigned int drawable_traj1[MAX_TRAJECTORY] = {0}; // 1P 軌跡データ
static unsigned int drawable_traj2[MAX_TRAJECTORY] = {0}; // 2P 軌跡データ
static LogState drawable_log1[MAX_LOG] = {0};             // 1P 入力ログデータ
static LogState drawable_log2[MAX_LOG] = {0};             // 2P 入力ログデータ
static bool drawable1, drawable2;                         // 描画するかどうかのbool値

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
static void draw_logs(const LogState *log, int x, int baseY, int align_right, int len)
{
    for (int i = 0; i < len; ++i)
    {
        if (log[i].count == 0)
            continue;

        CachedText *direction = &dir_cache[log[i].dir_index];
        CachedText *buttons = &button_cache[log[i].btn_index];
        int y = baseY + i * LINE_HEIGHT;

        if (align_right)
        {
            int dx = x - LOG_X_FIX;
            Vector2 btnSize = MeasureTextEx(font, buttons->text, FONT_SIZE, 1);
            draw_text(direction, dx - btnSize.x, y, RIGHT);     // 方向
            draw_text(buttons, dx, y, RIGHT);                   // ボタン
            draw_text(&count_cache[log[i].count], x, y, RIGHT); // フレームカウント
        }
        else
        {
            draw_text(&count_cache[log[i].count], x, y, LEFT); // フレームカウント
            draw_text(direction, x + LOG_X_FIX, y, LEFT);      // 方向
            Vector2 dirSize = MeasureTextEx(font, direction->text, FONT_SIZE, 1);
            draw_text(buttons, x + LOG_X_FIX + dirSize.x, y, LEFT); // ボタン
        }
    }
}

// 非アクティブ時のボタンの色
// ネオジオのボタン色と配置にあわせたものにしている。
static Color BTN_COL_INACTIVE = (Color){0x80, 0x80, 0x80, 0xFF}; // #808080FF
static Color BTN_COL_A2 = (Color){0x60, 0, 0, 0xFF};             // #600000FF
static Color BTN_COL_B2 = (Color){0x60, 0x60, 0, 0xFF};          // #606000FF
static Color BTN_COL_C2 = (Color){0, 0x60, 0, 0xFF};             // #006000FF
static Color BTN_COL_D2 = (Color){0, 0x60, 0x60, 0xFF};          // #006060FF

/**
 * @brief draw_stick_and_buttonsで使用するサブ関数でボタンラベルと円を表示する。
 */
static void draw_button_label(int btn_index, int btn, int x, int y)
{
    bool active = btn & btn_index;
    Color color = BLACK;
    if (btn_index == 0x1)
        color = active ? RED : BTN_COL_A2;
    else if (btn_index == 0x2)
        color = active ? GOLD : BTN_COL_B2;
    else if (btn_index == 0x4)
        color = active ? LIME : BTN_COL_C2;
    else if (btn_index == 0x8)
        color = active ? SKYBLUE : BTN_COL_D2;
    else
        return;
    DrawCircleV((Vector2){x, y}, BTN_SIZE, color);
    draw_text(&button_cache[btn_index], x, y - BTN_Y_FIX, CENTER);
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
static void draw_stick_and_buttons(const LogState *log, int base_x, int baseY, unsigned int *trajectory, Vector2 *stick_vector_cache)
{
    int x = base_x + 80;
    DrawRectangleRounded((Rectangle){base_x - 45, baseY - 45, 90, 90}, 0.3f, 8, WHITE);
    for (int i = MAX_TRAJECTORY - 1; i > 0; i--)
    {
        Vector2 p1 = stick_vector_cache[trajectory[i]];
        Vector2 p2 = stick_vector_cache[trajectory[i - 1]];
        // #FF0080FF ~ #8000FFFF
        Color c = (Color){0x80 + 0x7F * i / MAX_TRAJECTORY, 0, 0xFF - 0x7F * i / MAX_TRAJECTORY, 0xFF};
        DrawLineEx(p1, p2, 12, c);
    }
    DrawCircleV(stick_vector_cache[log->dir_index], 14, RED);
    draw_button_label(0x1, log->btn_index, x, baseY);
    draw_button_label(0x2, log->btn_index, x + 28, baseY - 25);
    draw_button_label(0x4, log->btn_index, x + 64, baseY - 32);
    draw_button_label(0x8, log->btn_index, x + 100, baseY - 30);
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
static inline unsigned char conv_dir_index(const InputState *state)
{
    unsigned char dir_index = 0;
    if (state->up)
        dir_index |= 0x1;
    if (state->down)
        dir_index |= 0x2;
    if (state->left)
        dir_index |= 0x4;
    if (state->right)
        dir_index |= 0x8;
    return dir_index;
}

/**
 * @brief 4ビットでABCDボタン状態を表現したデータを返す。
 */
static inline unsigned char conv_button_index(const InputState *state)
{
    unsigned char btn_index = 0;
    if (state->a)
        btn_index |= 0x1;
    if (state->b)
        btn_index |= 0x2;
    if (state->c)
        btn_index |= 0x4;
    if (state->d)
        btn_index |= 0x8;
    return btn_index;
}

/**
 * @brief 最新の入力データを1000FPSでメモリに記録して状態管理スレッドに移譲する。
 */
static void *input_thread(void *arg)
{
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 1000000}; // 1ms

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

        // 状態更新スレッドへ値連携
        if (pthread_mutex_lock(&input_lock) == 0)
        {
            realtime_state1 = (InputState){
                up1, down1, left1, right1,
                a1, b1, c1, d1};
            realtime_state2 = (InputState){
                up2, down2, left2, right2,
                a2, b2, c2, d2};
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
 * @brief 直前の入力データと比較して同値ならフレームカウンタを更新し、異なればログを追加する。
 *        LogState *log : 更新対象のログ（log_1 や log_2）配列
 *        const LogState *new_log : 新しい入力データ
 *        unsigned int *no_op_count : 継続カウント用変数
 *        // 呼び出し例（state_thread内）
 *        update_log_and_count(log_1, &new_log1, &no_op_count1);
 *        update_log_and_count(log_2, &new_log2, &no_op_count2);
 */
static inline void update_log_and_count(LogState *log, const LogState *new_log, unsigned int *no_op_count)
{

    if (is_equal_state(new_log, &log[0]))
    {
        if (is_neutral(new_log))
        {
            // ニュートラル継続によるリセットカウントを加算
            if (*no_op_count != -1 && *no_op_count < RESET_FRAME_COUNT)
                (*no_op_count)++;
        }

        // 継続カウントを加算
        if (log[0].count < MAX_FRAME_COUNT)
            log[0].count++;
    }
    else
    {
        // 入力変更によるログ記録とカウンタリセット
        memmove(&log[1], &log[0], sizeof(LogState) * (MAX_LOG - 1));
        log[0] = *new_log;
        *no_op_count = 1;
    }
}

/**
 * @brief 描画スレッドへの値引き渡し関数です。
 */
static inline void copy_drawable_set(
    unsigned int *dest_traj, const unsigned int *src_traj,
    LogState *dest_log, const LogState *src_log,
    bool *active_flag, int no_op_count)
{
    memcpy(dest_traj, src_traj, MAX_TRAJECTORY * sizeof(unsigned int)); // 軌跡
    memcpy(dest_log, src_log, MAX_LOG * sizeof(LogState));              // 入力ログ
    *active_flag = (no_op_count != -1);                                 // 描画可否を渡す
}

/**
 * @brief 入力データを60FPSで状態保存する。
 */
void *state_thread(void *arg)
{
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 16666666}; // 60FPS

    int no_op_count1 = -1, no_op_count2 = -1;
    InputState current_state1 = {0}, current_state2 = {0};
    LogState new_log1 = {0}, new_log2 = {0};
    unsigned int trajectory1[MAX_TRAJECTORY] = {0};
    unsigned int trajectory2[MAX_TRAJECTORY] = {0};
    LogState log_1[MAX_LOG] = {0};
    LogState log_2[MAX_LOG] = {0};

    printf("[info] state_thread started\n");

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    while (!exit_requested)
    {
        pthread_testcancel();

        struct timespec frame_start, frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        // 入力検知スレッドから値連携
        if (pthread_mutex_lock(&input_lock) == 0)
        {
            current_state1 = realtime_state1;
            current_state2 = realtime_state2;
            cur_debug_state = rt_debug_state;

            pthread_mutex_unlock(&input_lock);

            // ロック外でログ状態構造体へ変換
            new_log1 = (LogState){
                conv_dir_index(&current_state1),
                conv_button_index(&current_state1),
                1};
            new_log2 = (LogState){
                conv_dir_index(&current_state2),
                conv_button_index(&current_state2),
                1};
        }
        else
        {
            fprintf(stderr, "pthread_mutex_lock input_lock failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        // trajectoryとログ、カウント更新、DELによるリセット処理をここで統合

        // DELキーで状態初期化
        bool delkey = IsKeyPressed(KEY_DELETE);
        if (delkey || no_op_count1 >= RESET_FRAME_COUNT)
        {
            memset(trajectory1, 0, sizeof(trajectory1));
            memset(log_1, 0, sizeof(log_1));
            no_op_count1 = delkey ? 0 : -1;
        }
        if (delkey || no_op_count2 >= RESET_FRAME_COUNT)
        {
            memset(trajectory2, 0, sizeof(trajectory2));
            memset(log_2, 0, sizeof(log_2));
            no_op_count2 = delkey ? 0 : -1;
        }

        // レバー軌跡更新
        memmove(&trajectory1[1], &trajectory1[0], sizeof(unsigned int) * (MAX_TRAJECTORY - 1));
        trajectory1[0] = new_log1.dir_index;
        memmove(&trajectory2[1], &trajectory2[0], sizeof(unsigned int) * (MAX_TRAJECTORY - 1));
        trajectory2[0] = new_log2.dir_index;

        // 最新ログのフレームカウント加算かログ更新
        update_log_and_count(log_1, &new_log1, &no_op_count1);
        update_log_and_count(log_2, &new_log2, &no_op_count2);

        // デバッグフラグ更新
        if (cur_debug_state && !prev_debug_state && !debug_triggered)
        {
            show_debug ^= 1;
            debug_triggered = true;
        }
        if (!cur_debug_state)
            debug_triggered = false;
        prev_debug_state = cur_debug_state;

        // 描画スレッドへ値連携
        if (pthread_mutex_lock(&state_lock) == 0)
        {
            copy_drawable_set(
                drawable_traj1, trajectory1,
                drawable_log1, log_1,
                &drawable1, no_op_count1);
            copy_drawable_set(
                drawable_traj2, trajectory2,
                drawable_log2, log_2,
                &drawable2, no_op_count2);

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
 * @brief 状態スレッドからの値引き受け取り関数です。
 */
static inline void recv_drawable_set(
    unsigned int *dest_traj, const unsigned int *src_traj,
    LogState *dest_log, const LogState *src_log,
    bool *active_flag, bool *src_active)
{
    memcpy(dest_traj, src_traj, MAX_TRAJECTORY * sizeof(unsigned int)); // 軌跡
    memcpy(dest_log, src_log, MAX_LOG * sizeof(LogState));              // 入力ログ
    *active_flag = *src_active == true;                                 // 描画可否を渡す
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

    SetConfigFlags(FLAG_WINDOW_UNDECORATED | FLAG_FULLSCREEN_MODE);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "input_dispi raylib");

    init_codepoint_cache();
    int codepoint_count = 0;
    int *codepoints = LoadCodepoints(text, &codepoint_count);
    int codepoints_no_dups_count = 0;
    int *codepoints_no_dups = codepoint_remove_duplicates(codepoints, codepoint_count, &codepoints_no_dups_count);
    UnloadCodepoints(codepoints);
    font = LoadFontEx(FONT_PATH, FONT_SIZE, codepoints_no_dups, codepoints_no_dups_count);
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
        printf("[info] input_thread created\n");
    pthread_t state_tid;
    if (pthread_create(&state_tid, &attr, state_thread, NULL) != 0)
    {
        perror("[error] state_thread creation failed\n");
        return 1;
    }
    else
        printf("[info] state_thread created\n");
    pthread_attr_destroy(&attr);

    struct sched_param param;
    param.sched_priority = 40;
    pthread_setschedparam(state_tid, SCHED_FIFO, &param);

    SetTargetFPS(60);

    unsigned int draw_trajectory1[MAX_TRAJECTORY] = {0};
    unsigned int draw_trajectory2[MAX_TRAJECTORY] = {0};
    LogState draw_log1[MAX_LOG] = {0};
    LogState draw_log2[MAX_LOG] = {0};
    bool draw1 = true, draw2 = true;

    printf("[info] main_thread started\n");

    // グラデーション用カラー
    Color bg1 = (Color){0xC8, 0xC8, 0xC8, 0x30}; // #C8C8C830
    Color bg2 = (Color){0xC8, 0xC8, 0xC8, 0x18}; // #C8C8C818
    Color bg3 = (Color){0xC8, 0xC8, 0xC8, 0x00}; // #C8C8C800

    // レバー位置キャッシュ
    init_stick_vector_cache(stick_vector_cache1, STATUS_X1, STATUS_Y, LINE_HEIGHT); // 1P
    init_stick_vector_cache(stick_vector_cache2, STATUS_X2, STATUS_Y, LINE_HEIGHT); // 2P

    while (!WindowShouldClose() && !exit_requested)
    {
        struct timespec frame_start, frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        // 状態更新スレッドから値連携
        if (pthread_mutex_lock(&state_lock) == 0)
        {
            recv_drawable_set(
                draw_trajectory1, drawable_traj1,
                draw_log1, drawable_log1,
                &draw1, &drawable1);
            recv_drawable_set(
                draw_trajectory2, drawable_traj2,
                draw_log2, drawable_log2,
                &draw2, &drawable2);

            pthread_mutex_unlock(&state_lock);
        }
        else
        {
            fprintf(stderr, "pthread_mutex_lock state_lock failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        BeginDrawing();

        // 背景色
        ClearBackground(BLACK); // #000000 キーカラー

        // 背景グラデーション
        // レバー位置とボタン状態の描画
        // キーログの描画
        if (draw1)
        {
            DrawRectangleGradientH(0, 0, BG1_WIDTH, SCREEN_HEIGHT, bg1, bg2);
            DrawRectangleGradientH(BG1_WIDTH, 0, BG2_WIDTH, SCREEN_HEIGHT, bg2, bg3);
            draw_stick_and_buttons(&draw_log1[0], STATUS_X1, STATUS_Y, draw_trajectory1, stick_vector_cache1);
            draw_logs(draw_log1, LOG_X1, LOG_Y, LEFT, MAX_LOG);
        }
        if (draw2)
        {
            DrawRectangleGradientH(SCREEN_WIDTH - BG1_WIDTH, 0, BG1_WIDTH, SCREEN_HEIGHT, bg2, bg1);
            DrawRectangleGradientH(SCREEN_WIDTH - BG1_WIDTH - BG2_WIDTH, 0, BG2_WIDTH, SCREEN_HEIGHT, bg3, bg2);
            draw_stick_and_buttons(&draw_log2[0], STATUS_X2, STATUS_Y, draw_trajectory2, stick_vector_cache2);
            draw_logs(draw_log2, LOG_X2, LOG_Y, RIGHT, MAX_LOG);
        }

        // デバッグ表示
        if (show_debug)
            DrawFPS(10, 10);

        EndDrawing();
    }

    pthread_cancel(tid);
    pthread_cancel(state_tid);
    pthread_join(tid, NULL);
    pthread_join(state_tid, NULL);

    UnloadFont(font);
    cleanup_codepoint_cache();
    CloseWindow();

    reset_terminal_mode();

    release_lock();
    return 0;
}
