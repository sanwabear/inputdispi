#include <csignal>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <deque>
#include <string>
#include <unordered_map>

std::unordered_map<std::string, SDL_Texture*> text_cache;

void cleanup_text_cache() {
    for (auto& pair : text_cache) {
        SDL_DestroyTexture(pair.second);
    }
    text_cache.clear();
}

#include <sstream>
#include <iomanip>
#include <map>
#include <vector>
#include <cmath>

const int MAX_LOG = 26;
const int LINE_HEIGHT = 32;
const int MAX_FRAME_COUNT = 1000;
const int TRAJECTORY_LENGTH = 30;
const double PI = 3.14159265358979323846;
const int LEFT = 0;
const int RIGHT = 1;
const int CENTER = 2;
volatile bool quit = false; // グローバルに quit を制御するための変数

// Ctrl+C 対応のためのシグナルハンドラ
void handle_sigint(int sig) {
    quit = true;
}

struct InputState {
    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, c = false, d = false;

    std::string to_key() const {
        std::string s;
        if (up && left && !down && !right) s = "↖";
        else if (up && right && !down && !left) s = "↗";
        else if (down && left && !up && !right) s = "↙";
        else if (down && right && !up && !left) s = "↘";
        else if (up && !down && !left && !right) s = "↑";
        else if (down && !up && !left && !right) s = "↓";
        else if (left && !right && !up && !down) s = "←";
        else if (right && !left && !up && !down) s = "→";
        else s = u8"\u30FB";
        if (a) s += "A";
        if (b) s += "B";
        if (c) s += "C";
        if (d) s += "D";
        return s;
    }

    bool operator==(const InputState& other) const {
        return up == other.up && down == other.down && left == other.left && right == other.right &&
               a == other.a && b == other.b && c == other.c && d == other.d;
    }
}

std::string format_log(int count, const std::string& state_str, bool rightAlign = false) {
    if (count >= MAX_FRAME_COUNT) return rightAlign ? state_str + " LOT" : "LOT " + state_str;
    std::ostringstream ss;
    if (rightAlign) {
        std::ostringstream state_ss;
        state_ss << std::setw(8) << std::left << state_str;
        ss << state_ss.str() << " " << std::setw(3) << std::setfill('0') << count;
    } else ss << std::setw(3) << std::setfill('0') << count << " " << state_str;
    return ss.str();
}

SDL_Point stick_position(const InputState& state, int cx, int cy, int radius) {
    int direction = -1;
    if (state.up && state.left && !state.down && !state.right) direction = 5;
    else if (state.up && state.right && !state.down && !state.left) direction = 7;
    else if (state.down && state.left && !state.up && !state.right) direction = 3;
    else if (state.down && state.right && !state.up && !state.left) direction = 1;
    else if (state.up && !state.down && !state.left && !state.right) direction = 6;
    else if (state.down && !state.up && !state.left && !state.right) direction = 2;
    else if (state.left && !state.right && !state.up && !state.down) direction = 4;
    else if (state.right && !state.left && !state.up && !state.down) direction = 0;

    if (direction == -1) return {cx, cy};
    double angle = (PI / 4.0) * direction;
    int tx = cx + static_cast<int>(radius * cos(angle));
    int ty = cy + static_cast<int>(radius * sin(angle));
    return {tx, ty};
}

SDL_Color faded_magenta(int index, int total) {
    float t = index / static_cast<float>(total - 1);
    SDL_Color c;
    c.r = static_cast<Uint8>(128 + 127 * t);
    c.g = 0;
    c.b = static_cast<Uint8>(128 + 127 * (1 - t));
    c.a = 255;
    return c;
}

void draw_stick_trajectory(SDL_Renderer* renderer, const std::deque<InputState>& history, int cx, int cy, int radius) {
    for (size_t i = 1; i < history.size(); ++i) {
        SDL_Point p1 = stick_position(history[i - 1], cx, cy, radius);
        SDL_Point p2 = stick_position(history[i], cx, cy, radius);
        SDL_Color col = faded_magenta(i, history.size());
        if (p1.x != p2.x || p1.y != p2.y)
            thickLineRGBA(renderer, p1.x, p1.y, p2.x, p2.y, 12, col.r, col.g, col.b, col.a);
        filledCircleRGBA(renderer, p1.x, p1.y, 6, col.r, col.g, col.b, col.a);
        if (i == history.size() - 1)
            filledCircleRGBA(renderer, p2.x, p2.y, 6, col.r, col.g, col.b, col.a);
    }
}

void draw_stick_circle(SDL_Renderer* renderer, const InputState& state, int cx, int cy, int radius) {
    SDL_Point p1 = stick_position(state, cx, cy, radius);
    SDL_Color shadowColor = {64, 0, 0, 160}; // 非アクティブ：グレー
    filledCircleRGBA(renderer, p1.x, p1.y, 14, shadowColor.r, shadowColor.g, shadowColor.b, shadowColor.a);
    SDL_Color col = {255, 0, 0, 255}; // 赤
    filledCircleRGBA(renderer, p1.x, p1.y, 12, col.r, col.g, col.b, col.a);
}

void draw_text_glow(SDL_Renderer* renderer, TTF_Font* font, int x, int y, const std::string& text, int align, const SDL_Color& white, bool glow) {
    SDL_Color glowColor = {255, 255, 255, 24};  // 影ぼかし
    SDL_Color shadowColor = {0, 0, 0, 160};  // 影

    SDL_Surface* baseSurface = TTF_RenderUTF8_Blended(font, text.c_str(), white);
    int text_width = baseSurface->w;
    SDL_FreeSurface(baseSurface);

    int base_x = x;
    switch (align) {
        case LEFT: base_x = x; break;
        case RIGHT: base_x = x - text_width; break;
        case CENTER: base_x = x - text_width / 2; break;
    }

    if (glow) {
        // 影ぼかしを重ね描き
        for (int dx = -2; dx <= 4; ++dx) {
            for (int dy = -2; dy <= 4; ++dy) {
                if (dx == 0 && dy == 0) continue;
                SDL_Surface* shadowSurface = TTF_RenderUTF8_Blended(font, text.c_str(), glowColor);
                SDL_Texture* shadowTexture = SDL_CreateTextureFromSurface(renderer, shadowSurface);
                SDL_SetTextureBlendMode(shadowTexture, SDL_BLENDMODE_BLEND);
                SDL_Rect shadowRect = { base_x + dx + 1, y + dy + 1, shadowSurface->w, shadowSurface->h };
                SDL_RenderCopy(renderer, shadowTexture, NULL, &shadowRect);
                SDL_FreeSurface(shadowSurface);
                SDL_DestroyTexture(shadowTexture);
            }
        }
    }
    // 影文字
    SDL_Surface* shadowSurface = TTF_RenderUTF8_Blended(font, text.c_str(), shadowColor);
    SDL_Texture* shadowTexture = SDL_CreateTextureFromSurface(renderer, shadowSurface);
    SDL_Rect shadowRect = {base_x + 2, y + 2, shadowSurface->w, shadowSurface->h};
    SDL_RenderCopy(renderer, shadowTexture, NULL, &shadowRect);
    SDL_FreeSurface(shadowSurface);
    SDL_DestroyTexture(shadowTexture);
    // 本体文字
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), white);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect dst = { base_x, y, surface->w, surface->h };
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void draw_button_shadow(SDL_Renderer* renderer, int x, int y) {
    // グロー状のぼかし影
    for (int r = 20; r <= 28; r += 2) {
        filledCircleRGBA(renderer, x + 16, y + 16, r, 255, 255, 255, 16);
    }
}

void draw_button_circle(SDL_Renderer* renderer, TTF_Font* font, const std::string& name, bool active, int x, int y) {
    int baseX = x, baseY = y;
    SDL_Color white = {255, 255, 255, 255};     // 本体文字
    if (!active) {
        baseX -= 2;
        baseY -= 2;
        white = {128, 128, 128, 255};     // 本体文字
    }

    SDL_Color color = {160, 160, 160, 128}; // 非アクティブ：グレー
    filledCircleRGBA(renderer, baseX + 16, baseY + 16, 18, color.r, color.g, color.b, color.a);

    if (!active) {
        color = {160, 160, 160, 255}; // 非アクティブ：グレー
    } else if (name == "A") {
        color = {255, 0, 0, 255}; // 赤
    } else if (name == "B") {
        color = {235, 205, 0, 255}; // 金（黄）
    } else if (name == "C") {
        color = {50, 205, 50, 255}; // ライムグリーン
    } else if (name == "D") {
        color = {10, 195, 250, 255}; // スカイブルー
    } else {
        color = {200, 200, 200, 255}; // デフォルト
    }
    filledCircleRGBA(renderer, baseX + 16, baseY + 16, 16, color.r, color.g, color.b, color.a);

    // ラベル描画（白）
    draw_text_glow(renderer, font, baseX + 16, baseY, name, CENTER, white, false);
}

int main() {
    // シグナルハンドラを設定
    std::signal(SIGINT, handle_sigint);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return 1;
    }

    TTF_Init();
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN);

    SDL_Window* window = SDL_CreateWindow(
        "input_dispi",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        1920, 1080,
        SDL_WINDOW_FULLSCREEN
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_ShowCursor(SDL_DISABLE);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    TTF_Font* font = TTF_OpenFont("fonts/VL-PGothic-Regular.ttf", 28);
    if (!font) font = TTF_OpenFont("../fonts/VL-PGothic-Regular.ttf", 28);
    if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/vlgothic/VL-PGothic-Regular.ttf", 28);
    if (!font) return 1;

    InputState prevState1, currentState1, prevState2, currentState2;
    std::deque<std::string> log1, log2;
    std::deque<InputState> trajectory1, trajectory2;
    int frameCount1 = 0, frameCount2 = 0;
    SDL_Event e;

    static Uint64 last_counter = SDL_GetPerformanceCounter();
    static Uint64 freq = SDL_GetPerformanceFrequency();

    while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;

            if (e.key.keysym.sym == SDLK_ESCAPE) {
                quit = true;
            }
            if (e.key.keysym.sym == SDLK_DELETE) {
                // 入力状態とログの初期化
                currentState1 = {};
                currentState2 = {};
                prevState1 = {};
                prevState2 = {};
                frameCount1 = 1;
                frameCount2 = 1;
                log1.clear();
                log2.clear();
                trajectory1.clear();
                trajectory2.clear();
            }

            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                bool down = (e.type == SDL_KEYDOWN);
                switch (e.key.keysym.sym) {
                    // 1P: WSAD + N M , .
                    case SDLK_w: currentState1.up = down; break;
                    case SDLK_s: currentState1.down = down; break;
                    case SDLK_a: currentState1.left = down; break;
                    case SDLK_d: currentState1.right = down; break;
                    case SDLK_n: currentState1.a = down; break;
                    case SDLK_m: currentState1.b = down; break;
                    case SDLK_COMMA: currentState1.c = down; break;
                    case SDLK_PERIOD: currentState1.d = down; break;

                    // 2P: Arrow keys + 1 2 3 4
                    case SDLK_UP:    currentState2.up = down; break;
                    case SDLK_DOWN:  currentState2.down = down; break;
                    case SDLK_LEFT:  currentState2.left = down; break;
                    case SDLK_RIGHT: currentState2.right = down; break;
                    case SDLK_1: currentState2.a = down; break;
                    case SDLK_KP_1: currentState2.a = down; break;
                    case SDLK_2: currentState2.b = down; break;
                    case SDLK_KP_2: currentState2.b = down; break;
                    case SDLK_3: currentState2.c = down; break;
                    case SDLK_KP_3: currentState2.c = down; break;
                    case SDLK_4: currentState2.d = down; break;
                    case SDLK_KP_4: currentState2.d = down; break;
                }
            }

        }

        if (currentState1 == prevState1) {
            if (frameCount1 < MAX_FRAME_COUNT) frameCount1++;
        } else {
            if (frameCount1 > 0) {
                log1.push_front(format_log(frameCount1, prevState1.to_key()));
                if (log1.size() > MAX_LOG - 1) log1.pop_back();
            }
            frameCount1 = 1;
            prevState1 = currentState1;
        }
        trajectory1.push_back(currentState1);
        if (trajectory1.size() > TRAJECTORY_LENGTH) trajectory1.pop_front();

        if (currentState2 == prevState2) {
            if (frameCount2 < MAX_FRAME_COUNT) frameCount2++;
        } else {
            if (frameCount1 > 0) {
                log2.push_front(format_log(frameCount2, prevState2.to_key()));
                if (log2.size() > MAX_LOG - 1) log2.pop_back();
            }
            frameCount2 = 1;
            prevState2 = currentState2;
        }
        trajectory2.push_back(currentState2);
        if (trajectory2.size() > TRAJECTORY_LENGTH) trajectory2.pop_front();

        SDL_SetRenderDrawColor(renderer, 179, 89, 0, 255); // #B35900 キーカラー
        SDL_RenderClear(renderer);

        int offsetX = 20, offsetX2 = offsetX + 70, offsetX3 = 1820, offsetX4 = offsetX3 - 20;
        int offsetY = 60, offsetY2 = offsetY + 30;

        SDL_Color white = {255, 255, 255, 255};     // 本体文字

        // 1P側 ログ・現在状態表示
        draw_text_glow(renderer, font, offsetX, offsetY, frameCount1 >= MAX_FRAME_COUNT ? "LOT" : (std::ostringstream() << std::setw(3) << std::setfill('0') << frameCount1).str(), LEFT, white, true);
        draw_text_glow(renderer, font, offsetX2, offsetY, currentState1.to_key(), LEFT, white, true);
        for (size_t i = 0; i < log1.size(); ++i) {
            std::string full = log1[i];
            std::string frame = full.substr(0, 3);
            std::string state = full.substr(4);
            draw_text_glow(renderer, font, offsetX, offsetY2 + (int)i * LINE_HEIGHT, frame, LEFT, white, true);
            draw_text_glow(renderer, font, offsetX2, offsetY2 + (int)i * LINE_HEIGHT, state, LEFT, white, true);
        }

        // 2P側 ログ・現在状態表示
        draw_text_glow(renderer, font, offsetX3, offsetY, frameCount2 >= MAX_FRAME_COUNT ? "LOT" : (std::ostringstream() << std::setw(3) << std::setfill('0') << frameCount2).str(), LEFT, white, true);
        draw_text_glow(renderer, font, offsetX4, offsetY, currentState2.to_key(), RIGHT, white, true);
        for (size_t i = 0; i < log2.size(); ++i) {
            std::string full = log2[i];
            std::string frame = full.substr(0, 3);
            std::string state = full.substr(4);
            draw_text_glow(renderer, font, offsetX4, offsetY2 + (int)i * LINE_HEIGHT, state, RIGHT, white, true);
            draw_text_glow(renderer, font, offsetX3, offsetY2 + (int)i * LINE_HEIGHT, frame, LEFT, white, true);
        }

        // 1P側 背景、軌道、状態
        int wide = 36;
        int leverX = 75, baseX = leverX + 70, baseY = 960;
        roundedBoxRGBA(renderer, leverX - 55, baseY - 55, leverX + 55, baseY + 55, 12, 255, 255, 255, 255);
        draw_stick_trajectory(renderer, trajectory1, leverX, baseY, wide);
        draw_stick_circle(renderer, currentState1, leverX, baseY, wide);
        draw_button_shadow(renderer, baseX, baseY);
        draw_button_shadow(renderer, baseX + 28, baseY - 25);
        draw_button_shadow(renderer, baseX + 64, baseY - 32);
        draw_button_shadow(renderer, baseX + 100, baseY - 30);
        draw_button_circle(renderer, font, "A", currentState1.a, baseX, baseY);
        draw_button_circle(renderer, font, "B", currentState1.b, baseX + 28, baseY - 25);
        draw_button_circle(renderer, font, "C", currentState1.c, baseX + 64, baseY - 32);
        draw_button_circle(renderer, font, "D", currentState1.d, baseX + 100, baseY - 30);

        // 2P側 背景、軌道、状態
        int leverX2 = 1670, baseX2 = leverX2 + 70, baseY2 = baseY;
        roundedBoxRGBA(renderer, leverX2 - 48, baseY2 - 48, leverX2 + 48, baseY2 + 48, 12, 255, 255, 255, 255);
        draw_stick_trajectory(renderer, trajectory2, leverX2, baseY2, wide);
        draw_stick_circle(renderer, currentState2, leverX2, baseY2, wide);
        draw_button_shadow(renderer, baseX2, baseY2);
        draw_button_shadow(renderer, baseX2 + 28, baseY2 - 25);
        draw_button_shadow(renderer, baseX2 + 64, baseY2 - 32);
        draw_button_shadow(renderer, baseX2 + 100, baseY2 - 30);
        draw_button_circle(renderer, font, "A", currentState2.a, baseX2, baseY2);
        draw_button_circle(renderer, font, "B", currentState2.b, baseX2 + 28, baseY2 - 25);
        draw_button_circle(renderer, font, "C", currentState2.c, baseX2 + 64, baseY2 - 32);
        draw_button_circle(renderer, font, "D", currentState2.d, baseX2 + 100, baseY2 - 30);

        SDL_RenderPresent(renderer);

        // 精密なフレームキャップ処理
        Uint64 now = SDL_GetPerformanceCounter();
        double elapsed = (now - last_counter) / static_cast<double>(freq);
        double delay = (1.0 / 60.0) - elapsed;
        if (delay > 0) SDL_Delay(static_cast<Uint32>(delay * 1000));
        last_counter = SDL_GetPerformanceCounter();
    }

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    cleanup_text_cache();
    TTF_Quit();
    SDL_Quit();
    return 0;
}
