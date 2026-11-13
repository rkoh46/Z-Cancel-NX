// SPDX-License-Identifier: GPL-3.0-or-later
/* See lobby_entry.h. The screens follow the start screen and the online screens (online.c): a list on
 * the left, a detail panel on the right, the Switch's button letters at the bottom, one yellow accent for
 * the selection marker. The lobby itself (connection, rooms, ICE, the match) is net/lobby_play.cpp; these
 * functions only show it and pass the player's choices on. */

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "lobby_entry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "draw.h"
#include "lobby_play.h"
#include "menu.h"
#include "menu_ui.h"

#define MAX_ROOMS 32
#define CHAT_LINES 10
#define ROOM_ROWS_VISIBLE 5

/* What the screens show: from net/lobby_play.h, or made up (preview). */
struct view {
    struct znx_lobby_play_status status;
    struct znx_lobby_play_room rooms[MAX_ROOMS];
    int room_count, all_rooms;
    struct znx_lobby_play_room_view room;
    struct znx_lobby_play_chat_line chat[CHAT_LINES];
    int chat_count;
};

static struct {
    struct znx_online_request request;
    int open;
    int debug;
    struct view v;
    int row;                    /* rooms screen: 0 opens a room, 1.. the rooms */
    int seats;                  /* the seats of a room we open (2-4, RMG-K's default 2) */
    char entering[96];          /* the room being joined or opened */
    char local_ip[48];
    char username[32];
    uint64_t delay_room;        /* the room our delay setting was last applied in */
} l;

/* ---- small helpers ---- */

static void message(char* title, size_t title_size, const char* t, char* text, size_t text_size, const char* x)
{
    snprintf(title, title_size, "%s", t);
    snprintf(text, text_size, "%s", x);
}

static void lobby_log(const char* line)
{
    znx_log_line(line);
}

/* This console's address on its own network: the source address of a UDP socket towards the internet
 * (connect() on UDP sends nothing). libnx has no getifaddrs, so the ICE agents are bound to it: that
 * address is then this console's host candidate. */
static void find_local_ip(void)
{
    struct sockaddr_in to, me;
    socklen_t length = sizeof(me);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    l.local_ip[0] = '\0';
    if (fd < 0)
        return;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(0xC0000201u);   /* 192.0.2.1, the documentation network: never reached */
    if (connect(fd, (struct sockaddr*)&to, sizeof(to)) == 0 && getsockname(fd, (struct sockaddr*)&me, &length) == 0 &&
        me.sin_addr.s_addr != 0)
        snprintf(l.local_ip, sizeof(l.local_ip), "%s", inet_ntoa(me.sin_addr));
    close(fd);
}

static void refresh(void)
{
    if (l.debug)
        return;
    znx_lobby_play_poll();
    znx_lobby_play_status(&l.v.status);
    l.v.room_count = znx_lobby_play_rooms(l.v.rooms, MAX_ROOMS, &l.v.all_rooms);
    if (znx_lobby_play_room_view(&l.v.room) != 0)
        memset(&l.v.room, 0, sizeof(l.v.room));
    l.v.chat_count = znx_lobby_play_chat(l.v.chat, CHAT_LINES);
}

static const char* delay_label(int delay_auto, int delay, char* out, size_t size)
{
    if (delay_auto)
        snprintf(out, size, "Auto, %d frame%s from the ping", delay, delay == 1 ? "" : "s");
    else
        snprintf(out, size, "%d frame%s", delay, delay == 1 ? "" : "s");
    return out;
}

/* ---- entering and leaving ---- */

enum znx_online_result znx_lobby_enter(const struct znx_online_request* request, char* title, size_t title_size,
                                       char* text, size_t text_size)
{
    const struct znx_menu_settings* s = request->settings;
    struct znx_lobby_play_config config;
    char error[300];
    l.request = *request;
    l.debug = 0;
    l.row = 0;
    if (l.seats < 2 || l.seats > 4)
        l.seats = 2;
    memset(&l.v, 0, sizeof(l.v));
    find_local_ip();
    znx_lobby_play_username(s->name, l.username, sizeof(l.username));
    memset(&config, 0, sizeof(config));
    config.server_url = s->lobby_server[0] ? s->lobby_server : NULL;
    config.username = l.username;
    config.rom_md5 = request->rom_md5;
    config.rom_name = request->goodname;
    config.stun = s->lobby_stun[0] ? s->lobby_stun : NULL;   /* empty: the server's, as RMG-K */
    config.no_turn = 0;
#ifdef __SWITCH__
    config.bind_address = s->lobby_bind[0] ? s->lobby_bind : (l.local_ip[0] ? l.local_ip : NULL);
#else
    config.bind_address = s->lobby_bind[0] ? s->lobby_bind : NULL;
#endif
    config.local_ip = l.local_ip[0] ? l.local_ip : NULL;
    config.log = lobby_log;
    if (znx_lobby_play_open(&config, error, sizeof(error)) != 0) {
        message(title, title_size, "The lobby could not open", text, text_size, error);
        return ZNX_ONLINE_MESSAGE;
    }
    l.open = 1;
    l.delay_room = 0;
    refresh();
    return ZNX_ONLINE_STAY;
}

void znx_lobby_close(void)
{
    if (!l.debug && znx_lobby_play_is_open())
        znx_lobby_play_close();
    l.open = 0;
    l.debug = 0;
}

int znx_lobby_active(void)
{
    if (l.debug)
        return l.open;
    return l.open && znx_lobby_play_is_open();
}

int znx_lobby_match_ready(void)
{
    struct znx_lobby_play_status s;
    if (l.debug || !znx_lobby_active())
        return 0;
    znx_lobby_play_status(&s);
    return s.phase == ZNX_LP_READY;
}

int znx_lobby_run_session(void* core, const struct znx_p2p_session_hooks* hooks)
{
    return znx_lobby_play_run_session(core, hooks);
}

int znx_lobby_prepare_core(void* core)
{
    return znx_lobby_play_prepare_core(core);
}

int znx_lobby_input_players(void)
{
    return znx_lobby_play_input_players();
}

enum znx_online_result znx_lobby_after_game(char* title, size_t title_size, char* text, size_t text_size)
{
    if (znx_lobby_play_after_game(title, title_size, text, text_size)) {
        refresh();
        return ZNX_ONLINE_STAY;
    }
    refresh();
    if (l.v.status.phase == ZNX_LP_FAILED || l.v.status.phase == ZNX_LP_CLOSED)
        znx_lobby_close();
    return ZNX_ONLINE_MESSAGE;
}

/* ---- update ---- */

static void open_room(void)
{
    char name[64], typed[64];
    snprintf(name, sizeof(name), "%s's Room", l.username);   /* CreateRoomDialog's default */
    if (l.request.keyboard != NULL) {
        if (!l.request.keyboard(4, "Room name", "Players see it in the room list", name, typed, sizeof(typed)))
            return;
        if (typed[strspn(typed, " ")] != '\0')
            snprintf(name, sizeof(name), "%s", typed);
    }
    snprintf(l.entering, sizeof(l.entering), "%s", name);
    znx_lobby_play_create_room(name, l.seats);
}

static void change_delay(int step)
{
    struct znx_menu_settings* s = l.request.settings;
    /* Auto, then 1..9 frames: the online delay setting's values (0 = Auto) */
    int d = s->input_delay + step;
    if (d < 0) d = 0;
    if (d > 9) d = 9;
    if (d == s->input_delay)
        return;
    s->input_delay = d;
    znx_lobby_play_set_delay(d == 0 ? -1 : d);
    znx_settings_save(l.request.base_dir, s);
}

enum znx_online_result znx_lobby_update(uint32_t pressed, uint32_t held, char* title, size_t title_size, char* text,
                                        size_t text_size)
{
    (void)held;
    refresh();
    if (l.debug) {
        if (pressed & ZNX_BTN_B) {
            l.open = 0;
            return ZNX_ONLINE_BACK;
        }
        return ZNX_ONLINE_STAY;
    }
    {
        const int problem = znx_lobby_play_take_problem(title, title_size, text, text_size);
        if (problem != ZNX_LPP_NONE) {
            if (l.v.status.phase == ZNX_LP_FAILED || l.v.status.phase == ZNX_LP_CLOSED)
                znx_lobby_close();
            return ZNX_ONLINE_MESSAGE;
        }
    }
    switch (l.v.status.phase) {
    case ZNX_LP_CLOSED:
    case ZNX_LP_FAILED:
        znx_lobby_close();
        return ZNX_ONLINE_BACK;
    case ZNX_LP_READY:
    case ZNX_LP_IN_GAME:
        return ZNX_ONLINE_PLAY;
    case ZNX_LP_CONNECTING:
    case ZNX_LP_ENTERING:
        if (pressed & ZNX_BTN_B) {
            znx_lobby_close();
            return ZNX_ONLINE_BACK;
        }
        return ZNX_ONLINE_STAY;
    case ZNX_LP_ROOMS: {
        const int rows = 1 + l.v.room_count;
        if (l.row >= rows) l.row = rows - 1;
        if (pressed & ZNX_BTN_UP) l.row = l.row > 0 ? l.row - 1 : 0;
        if (pressed & ZNX_BTN_DOWN) l.row = l.row < rows - 1 ? l.row + 1 : rows - 1;
        if (l.row == 0) {
            if (pressed & ZNX_BTN_LEFT) l.seats = l.seats > 2 ? l.seats - 1 : 2;
            if (pressed & ZNX_BTN_RIGHT) l.seats = l.seats < 4 ? l.seats + 1 : 4;
        }
        if (pressed & ZNX_BTN_B) {
            znx_lobby_close();
            return ZNX_ONLINE_BACK;
        }
        if (pressed & ZNX_BTN_A) {
            if (l.row == 0) {
                open_room();
            } else {
                const struct znx_lobby_play_room* r = &l.v.rooms[l.row - 1];
                if (r->in_game || r->players >= r->max_players) {
                    message(title, title_size, r->in_game ? "That room is playing a match" : "That room is full", text,
                            text_size, r->in_game ? "Its players are in a match. Join when it is over, or pick another room."
                                                  : "Every seat is taken. Pick another room, or open your own.");
                    return ZNX_ONLINE_MESSAGE;
                }
                snprintf(l.entering, sizeof(l.entering), "%s", r->name);
                znx_lobby_play_join_room(r->id);
            }
        }
        return ZNX_ONLINE_STAY;
    }
    case ZNX_LP_ROOM:
        if (l.v.room.id != 0 && l.v.room.id != l.delay_room) {
            /* our delay in this room: the online delay setting (0 = Auto, RMG-K's default) */
            const int d = l.request.settings->input_delay;
            znx_lobby_play_set_delay(d == 0 ? -1 : d);
            l.delay_room = l.v.room.id;
        }
        if (pressed & ZNX_BTN_LEFT) change_delay(-1);
        if (pressed & ZNX_BTN_RIGHT) change_delay(1);
        if ((pressed & ZNX_BTN_A) && l.v.room.is_host && l.v.room.can_start)
            znx_lobby_play_start();
        if ((pressed & ZNX_BTN_Y) && l.request.keyboard != NULL) {
            char line[128];
            if (l.request.keyboard(3, "Room chat", "Everyone in the room reads it", "", line, sizeof(line)) &&
                line[strspn(line, " ")] != '\0')
                znx_lobby_play_send_chat(line);
        }
        if (pressed & ZNX_BTN_B) {
            znx_lobby_play_leave_room();
            l.row = 0;
        }
        return ZNX_ONLINE_STAY;
    case ZNX_LP_STARTING:
        if (pressed & ZNX_BTN_B) {
            znx_lobby_play_leave_room();
            l.row = 0;
        }
        return ZNX_ONLINE_STAY;
    }
    return ZNX_ONLINE_STAY;
}

/* ---- drawing ---- */

/* Steps of something that takes a few seconds: the current one raised, with a line under it. */
static void draw_steps(const char* const* steps, int count, int current, const char* detail)
{
    float y = LIST_Y;
    for (int i = 0; i < count; ++i) {
        const int now = i == current;
        const float h = now ? 104 : 64;
        if (now) {
            znx_draw_rect(LIST_X, y, LIST_W, h, 12, C_SELECTED);
            znx_draw_slant(LIST_X + 12, y + 16, 6, 40, SLANT, C_ACCENT);
        }
        znx_draw_text(ZNX_FONT_ROW, LIST_X + 32, y + (now ? 36 : 42), steps[i], i < current ? C_TEXT2 : now ? C_TEXT1 : C_TEXT3, 0);
        if (now && detail != NULL) {
            char fitted[200];
            znx_text_fit(ZNX_FONT_SMALL, detail, LIST_W - 64, fitted, sizeof(fitted));
            znx_draw_text(ZNX_FONT_SMALL, LIST_X + 32, y + 76, fitted, C_TEXT2, 0);
        }
        y += h + ROW_GAP;
    }
}

static void draw_connecting(void)
{
    const struct hint hints[] = { { "B", "Cancel" } };
    static const char* const steps[2] = { "Reach the lobby server", "Log in and get the rooms" };
    char title[160], detail[200];
    snprintf(title, sizeof(title), "Lobby \xC2\xB7 %s", l.request.title);
    znx_ui_title(title);
    if (l.v.status.seconds >= 2)
        snprintf(detail, sizeof(detail), "%s, %d s", l.v.status.server, l.v.status.seconds);
    else
        snprintf(detail, sizeof(detail), "%s", l.v.status.server);
    draw_steps(steps, 2, 0, detail);
    {
        const struct field fields[] = {
            { "Server", l.v.status.server, 1 },
            { "Name", l.v.status.username, 0 },
            { "Game", l.request.goodname, 0 },
            { "Speaks as", "RMG-K " ZNX_LOBBY_CLIENT_VERSION, 0 },
        };
        znx_ui_panel(LIST_Y, fields, 4,
                     "RMG-K's rollback lobby: rooms of players with the same game, up to four in a match. The "
                     "server introduces the consoles; the match itself runs directly between them.",
                     "Gives up after 15 seconds.");
    }
    znx_ui_footer(hints, 1, NULL);
}

static void draw_entering(void)
{
    const struct hint hints[] = { { "B", "Cancel" } };
    static const char* const steps[2] = { "Ask the server for a seat", "Connect to the other players" };
    char title[160];
    snprintf(title, sizeof(title), "%s", l.entering[0] ? l.entering : "Joining");
    znx_ui_title(title);
    draw_steps(steps, 2, 0, "Waiting for the lobby server");
    {
        const struct field fields[] = { { "Name", l.v.status.username, 0 }, { "Game", l.request.goodname, 0 } };
        znx_ui_panel(LIST_Y, fields, 2, "The room opens as soon as the server answers.", NULL);
    }
    znx_ui_footer(hints, 1, NULL);
}

static void footer_left(char* out, size_t size)
{
    if (l.v.status.users_online > 0)
        snprintf(out, size, "%s \xC2\xB7 %d online%s%s", l.v.status.username, l.v.status.users_online,
                 l.v.status.region[0] ? " \xC2\xB7 " : "", l.v.status.region);
    else
        snprintf(out, size, "%s", l.v.status.username);
}

static void draw_rooms(void)
{
    const struct hint hints_open[] = { { "A", "Open" }, { "+-", "Seats" }, { "B", "Leave lobby" } };
    const struct hint hints_join[] = { { "A", "Join" }, { "B", "Leave lobby" } };
    char title[160], left[160], status[200], ping[32], seats[32], name[64];
    int first;
    float y = LIST_Y;
    snprintf(title, sizeof(title), "Rooms \xC2\xB7 %s", l.request.title);
    znx_ui_title(title);
    footer_left(left, sizeof(left));

    first = l.row - ROOM_ROWS_VISIBLE + 1;
    if (first < 0) first = 0;
    for (int i = first; i <= l.v.room_count && i < first + ROOM_ROWS_VISIBLE; ++i) {
        if (i == 0) {
            snprintf(status, sizeof(status), "%d seats, you start the match", l.seats);
            znx_ui_row(y, ROW_H, l.row == 0, "Open a room", C_TEXT1, status, C_TEXT2, 0, NULL);
        } else {
            const struct znx_lobby_play_room* r = &l.v.rooms[i - 1];
            const int joinable = !r->in_game && r->players < r->max_players;
            snprintf(status, sizeof(status), "%s \xC2\xB7 %d of %d players%s", r->host, r->players, r->max_players,
                     r->in_game ? " \xC2\xB7 in a match" : r->players >= r->max_players ? " \xC2\xB7 full" : "");
            if (r->ping_estimate > 0) snprintf(ping, sizeof(ping), "~%d ms", r->ping_estimate);
            else ping[0] = '\0';
            znx_ui_row(y, ROW_H, l.row == i, r->name, joinable ? C_TEXT1 : C_TEXT2, status, C_TEXT2,
                       joinable ? C_READY : C_TEXT3, NULL);
            if (ping[0])   /* an estimate: quieter than a measured figure */
                znx_draw_text(ZNX_FONT_BODY, LIST_X + LIST_W - 32, y + 36, ping, C_TEXT2, ZNX_TEXT_RIGHT | ZNX_TEXT_TABULAR);
        }
        y += ROW_H + ROW_GAP;
    }
    if (l.v.room_count == 0) {
        char line[200];
        snprintf(line, sizeof(line), "No room is open with %s right now. Open one and wait, or stay here: the "
                 "list updates by itself.", l.request.title);
        znx_draw_wrapped(ZNX_FONT_BODY, LIST_X + 32, y + 32, LIST_W - 64, line, C_TEXT2);
    }

    if (l.row == 0) {
        snprintf(name, sizeof(name), "%s's Room", l.username[0] ? l.username : l.v.status.username);
        snprintf(seats, sizeof(seats), "%d players", l.seats);
        {
            const struct field fields[] = {
                { "Name", name, 0 },
                { "Seats", seats, 0 },
                { "Game", l.request.goodname, 0 },
            };
            znx_ui_panel(LIST_Y, fields, 3,
                         "You become the host and start the match once everyone is connected. RMG-K players with the "
                         "same ROM see the room in their lobby.",
                         "A asks for the room's name; left and right change the seats.");
        }
        znx_ui_footer(hints_open, 3, left);
    } else {
        const struct znx_lobby_play_room* r = &l.v.rooms[l.row - 1];
        char players[32], ping_text[64], note[160];
        snprintf(players, sizeof(players), "%d of %d", r->players, r->max_players);
        if (r->ping_estimate > 0)
            snprintf(ping_text, sizeof(ping_text), "About %d ms, from the regions", r->ping_estimate);
        else
            snprintf(ping_text, sizeof(ping_text), "Measured once you are in the room");
        if (l.v.all_rooms > l.v.room_count)
            snprintf(note, sizeof(note), "%d room%s with other games %s not shown.", l.v.all_rooms - l.v.room_count,
                     l.v.all_rooms - l.v.room_count == 1 ? "" : "s", l.v.all_rooms - l.v.room_count == 1 ? "is" : "are");
        else
            note[0] = '\0';
        {
            const struct field fields[] = {
                { "Host", r->host, 0 },
                { "Players", players, 0 },
                { "Ping", ping_text, 0 },
                { "State", r->in_game ? "In a match" : r->players >= r->max_players ? "Full" : "Waiting for players", 0 },
            };
            znx_ui_panel(LIST_Y, fields, 4,
                         "A joins: you take the first free seat. The host starts the match once every player has a "
                         "direct connection to every other.",
                         note[0] ? note : NULL);
        }
        znx_ui_footer(hints_join, 2, left);
    }
}

/* One seat of the room: port, name, connection, delay, who it is. */
static void draw_seat(float y, float h, int port, const struct znx_lobby_play_seat* s)
{
    char label[8], fitted[64], line[96], right[32];
    uint32_t dot = C_TEXT3;
    znx_draw_rect(LIST_X, y, LIST_W, h, 12, C_CARD);
    snprintf(label, sizeof(label), "P%d", port);
    znx_draw_text(ZNX_FONT_BODY, LIST_X + 24, y + 34, label, C_TEXT3, ZNX_TEXT_TABULAR);
    if (s->user_id == 0) {
        znx_draw_text(ZNX_FONT_ROW, LIST_X + 72, y + 34, "Open seat", C_TEXT3, 0);
        return;
    }
    znx_text_fit(ZNX_FONT_ROW, s->name, LIST_W - 260, fitted, sizeof(fitted));
    znx_draw_text(ZNX_FONT_ROW, LIST_X + 72, y + 34, fitted, C_TEXT1, 0);
    if (s->self) {
        snprintf(line, sizeof(line), "This console");
        dot = C_READY;
    } else if (s->ice == 1) {
        if (s->ping >= 0) snprintf(line, sizeof(line), "Connected \xC2\xB7 %d ms", s->ping);
        else snprintf(line, sizeof(line), "Connected \xC2\xB7 measuring the ping");
        dot = C_READY;
    } else if (s->ice < 0) {
        snprintf(line, sizeof(line), "No direct connection");
        dot = C_MISSING;
    } else if (s->attempt > 1) {
        snprintf(line, sizeof(line), "Connecting, attempt %d of 20", s->attempt);
    } else {
        snprintf(line, sizeof(line), "Connecting");
    }
    znx_draw_rect(LIST_X + 72, y + 58 - znx_cap_height(ZNX_FONT_SMALL) / 2 - 4, 8, 8, 4, dot);
    znx_draw_text(ZNX_FONT_SMALL, LIST_X + 88, y + 58, line, s->ice < 0 && !s->self ? C_TEXT1 : C_TEXT2, 0);
    snprintf(right, sizeof(right), "%s", s->self ? (s->host ? "You, host" : "You") : s->host ? "Host" : "");
    if (right[0])
        znx_draw_text(ZNX_FONT_BODY, LIST_X + LIST_W - 24, y + 34, right, C_TEXT2, ZNX_TEXT_RIGHT);
    if (s->delay >= 0) {
        snprintf(line, sizeof(line), "Delay %d", s->delay);
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + LIST_W - 24, y + 58, line, C_TEXT3, ZNX_TEXT_RIGHT | ZNX_TEXT_TABULAR);
    }
}

static void draw_chat(float top, float bottom)
{
    const float step = znx_line_height(ZNX_FONT_SMALL);
    int fit = (int)((bottom - top - 16) / step);
    int first;
    float y;
    znx_draw_rect(LIST_X, top, LIST_W, 1, 0, C_HAIRLINE);
    if (fit < 1)
        return;
    if (l.v.chat_count == 0) {
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + 24, top + 16 + step, "Room chat: Y writes a line", C_TEXT3, 0);
        return;
    }
    first = l.v.chat_count > fit ? l.v.chat_count - fit : 0;
    y = top + 16 + step;
    for (int i = first; i < l.v.chat_count; ++i, y += step) {
        const struct znx_lobby_play_chat_line* c = &l.v.chat[i];
        char fitted[300];
        float x = LIST_X + 24;
        if (c->name[0]) {
            char name[64];
            snprintf(name, sizeof(name), "%s", c->name);
            x += znx_draw_text(ZNX_FONT_SMALL, x, y, name, c->self ? C_TEXT3 : C_TEXT2, 0) + 12;
            znx_text_fit(ZNX_FONT_SMALL, c->text, LIST_X + LIST_W - 24 - x, fitted, sizeof(fitted));
            znx_draw_text(ZNX_FONT_SMALL, x, y, fitted, C_TEXT1, 0);
        } else {
            znx_text_fit(ZNX_FONT_SMALL, c->text, LIST_W - 48, fitted, sizeof(fitted));
            znx_draw_text(ZNX_FONT_SMALL, x, y, fitted, C_TEXT3, 0);
        }
    }
}

static void draw_room(void)
{
    const struct hint hints_start[] = { { "A", "Start" }, { "Y", "Chat" }, { "+-", "Delay" }, { "B", "Leave" } };
    const struct hint hints[] = { { "Y", "Chat" }, { "+-", "Delay" }, { "B", "Leave" } };
    const struct znx_lobby_play_room_view* r = &l.v.room;
    const int seats = r->max_players >= 2 && r->max_players <= 4 ? r->max_players : 2;
    const float h = 72;
    char left[160], delay[64], prediction[32], players[32], paragraph[300];
    float y = LIST_Y;
    znx_ui_title(r->name[0] ? r->name : "Room");
    footer_left(left, sizeof(left));
    for (int i = 0; i < seats; ++i) {
        draw_seat(y, h, i + 1, &r->seats[i]);
        y += h + ROW_GAP;
    }
    draw_chat(y + 8, FOOTER_LINE - 16);

    delay_label(r->delay_auto, r->local_delay, delay, sizeof(delay));
    snprintf(prediction, sizeof(prediction), "Up to %d frames", r->prediction);
    snprintf(players, sizeof(players), "%d of %d", r->seated, seats);
    if (!r->same_rom)
        snprintf(paragraph, sizeof(paragraph), "This room plays %s, not your game. Leave and choose that game on the "
                 "first screen.", r->rom_name);
    else if (r->in_game)
        snprintf(paragraph, sizeof(paragraph), "%s. You play in the next one.", r->start_note);
    else if (r->is_host && r->can_start)
        snprintf(paragraph, sizeof(paragraph), "Everyone has a direct connection: A starts the match. To leave a "
                 "match, hold \xE2\x88\x92\xC2\xA0(Minus), or L\xC2\xA0+\xC2\xA0R\xC2\xA0+\xC2\xA0Z\xC2\xA0+\xC2\xA0Start on "
                 "a GameCube controller.");
    else if (r->is_host)
        snprintf(paragraph, sizeof(paragraph), "%s. You can start once every player has a direct connection to "
                 "every other and the pings are measured.", r->start_note);
    else
        snprintf(paragraph, sizeof(paragraph), "%s once everyone is connected. To leave a match, hold "
                 "\xE2\x88\x92\xC2\xA0(Minus), or L\xC2\xA0+\xC2\xA0R\xC2\xA0+\xC2\xA0Z\xC2\xA0+\xC2\xA0Start on a "
                 "GameCube controller.", r->start_note);
    {
        const struct field fields[] = {
            { "Game", r->rom_name, 0 },
            { "Your delay", delay, 0 },
            { "Rollback", prediction, 0 },
            { "Players", players, 0 },
        };
        znx_ui_panel(LIST_Y, fields, 4, paragraph,
                     "Every player picks their own delay. Left and right change yours; Auto follows the worst ping.");
    }
    if (r->is_host && r->can_start && !r->in_game)
        znx_ui_footer(hints_start, 4, left);
    else
        znx_ui_footer(hints, 3, left);
}

static void draw_starting(void)
{
    const struct hint hints[] = { { "B", "Leave the room" } };
    static const char* const steps[3] = { "Connect to every player", "Sync the room's settings", "Start the game" };
    const struct znx_lobby_play_status* s = &l.v.status;
    const struct znx_lobby_play_room_view* r = &l.v.room;
    char detail[160], seat[16], delay[64], prediction[32];
    int self_seat = 0;
    for (int i = 0; i < 4; ++i)
        if (r->seats[i].self)
            self_seat = i + 1;
    znx_ui_title("Starting the match");
    if (s->start_step == 0)
        snprintf(detail, sizeof(detail), "%d of %d connected \xC2\xB7 %d s left", s->start_connected, s->start_peers,
                 s->start_seconds_left);
    else
        snprintf(detail, sizeof(detail), "The host's cheats and save data \xC2\xB7 %d s left", s->start_seconds_left);
    draw_steps(steps, 3, s->start_step, detail);
    snprintf(seat, sizeof(seat), "P%d", self_seat);
    delay_label(r->delay_auto, r->local_delay, delay, sizeof(delay));
    snprintf(prediction, sizeof(prediction), "Up to %d frames", r->prediction);
    {
        const struct field fields[] = {
            { "Room", r->name, 0 },
            { "Your port", seat, 0 },
            { "Delay", delay, 0 },
            { "Rollback", prediction, 0 },
        };
        znx_ui_panel(LIST_Y, fields, 4,
                     "Your controller plays port 1 of this console in the seat the room gave you. RMG-K waits 15 "
                     "seconds for the connections and 10 for the sync; if either runs out, the room goes back to "
                     "waiting.",
                     NULL);
    }
    znx_ui_footer(hints, 1, NULL);
}

void znx_lobby_draw(void)
{
    switch (l.v.status.phase) {
    case ZNX_LP_CONNECTING: draw_connecting(); break;
    case ZNX_LP_ENTERING: draw_entering(); break;
    case ZNX_LP_ROOM: draw_room(); break;
    case ZNX_LP_STARTING:
    case ZNX_LP_READY:
    case ZNX_LP_IN_GAME: draw_starting(); break;
    case ZNX_LP_ROOMS:
    default: draw_rooms(); break;
    }
}

/* ---- preview ---- */

static void fake_seat(struct znx_lobby_play_seat* s, uint64_t id, const char* name, int delay, int ice, int ping,
                      int self, int host)
{
    memset(s, 0, sizeof(*s));
    s->user_id = id;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->delay = delay;
    s->ice = ice;
    s->ping = ping;
    s->self = self;
    s->host = host;
    s->attempt = 1;
}

static void fake_chat(const char* name, const char* text, int self)
{
    struct znx_lobby_play_chat_line* c = &l.v.chat[l.v.chat_count++];
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->text, sizeof(c->text), "%s", text);
    c->self = self;
}

void znx_lobby_debug(const struct znx_online_request* request, const char* which)
{
    struct view* v = &l.v;
    static const struct znx_lobby_play_room rooms[] = {
        { 101, "Friendlies, Hyrule only", "Isai_Fan", 1, 2, 0, 0, 30 },
        { 102, "4p FFA", "dreamland_dan", 2, 4, 0, 0, 80 },
        { 103, "Netplay ladder warmup", "Pika", 2, 2, 1, 0, 40 },
        { 104, "kirbyfan64's Room", "kirbyfan64", 1, 2, 0, 0, 90 },
    };
    l.request = *request;
    l.debug = 1;
    l.open = 1;
    l.seats = 2;
    l.row = 0;
    memset(v, 0, sizeof(*v));
    znx_lobby_play_username(request->settings->name, l.username, sizeof(l.username));
    snprintf(v->status.server, sizeof(v->status.server), "%s",
             request->settings->lobby_server[0] ? request->settings->lobby_server : ZNX_LOBBY_URL_RELEASE);
    snprintf(v->status.username, sizeof(v->status.username), "%s", l.username);
    snprintf(v->status.region, sizeof(v->status.region), "us-east");
    v->status.users_online = 23;
    v->status.phase = ZNX_LP_ROOMS;
    if (!strcmp(which, "connecting")) {
        v->status.phase = ZNX_LP_CONNECTING;
        v->status.seconds = 3;
    } else if (!strncmp(which, "rooms", 5) || !strcmp(which, "create")) {
        if (strcmp(which, "rooms-empty") != 0) {
            v->room_count = (int)(sizeof(rooms) / sizeof(rooms[0]));
            memcpy(v->rooms, rooms, sizeof(rooms));
            v->all_rooms = v->room_count + 3;
            l.row = strcmp(which, "create") == 0 ? 0 : 1;
        } else {
            v->all_rooms = 2;
        }
    } else {
        struct znx_lobby_play_room_view* r = &v->room;
        const int host = !strcmp(which, "room-host") || !strcmp(which, "room-4");
        r->id = 104;
        r->same_rom = 1;
        r->max_players = !strcmp(which, "room-4") ? 4 : 2;
        r->prediction = 7;
        r->local_delay = 2;
        r->delay_auto = 1;
        snprintf(r->rom_name, sizeof(r->rom_name), "%s", request->goodname);
        if (host) {
            snprintf(r->name, sizeof(r->name), "%s's Room", l.username);
            snprintf(r->host_name, sizeof(r->host_name), "%s", l.username);
            fake_seat(&r->seats[0], 1, l.username, 2, 1, 0, 1, 1);
            fake_seat(&r->seats[1], 2, "kirbyfan64", 2, 1, 38, 0, 0);
            r->is_host = 1;
            r->can_start = 1;
            r->seated = 2;
            snprintf(r->start_note, sizeof(r->start_note), "Everyone is connected");
            if (r->max_players == 4) {
                fake_seat(&r->seats[2], 3, "dreamland_dan", 3, 0, -1, 0, 0);
                r->seats[2].attempt = 2;
                r->seated = 3;
                r->can_start = 0;
                snprintf(r->start_note, sizeof(r->start_note), "Connecting to dreamland_dan");
            }
            fake_chat("", "You opened the room", 0);
            fake_chat("", "kirbyfan64 joined", 0);
            fake_chat("", "Direct connection to kirbyfan64", 0);
            fake_chat("kirbyfan64", "hi! fox only?", 0);
            fake_chat(l.username, "sure, Hyrule", 1);
            if (r->max_players == 4)
                fake_chat("", "dreamland_dan joined", 0);
        } else {
            const int failed = !strcmp(which, "room-ice-failed");
            snprintf(r->name, sizeof(r->name), "kirbyfan64's Room");
            snprintf(r->host_name, sizeof(r->host_name), "kirbyfan64");
            fake_seat(&r->seats[0], 2, "kirbyfan64", 2, failed ? -1 : 1, failed ? -1 : 41, 0, 1);
            fake_seat(&r->seats[1], 1, l.username, 2, 1, 0, 1, 0);
            r->seated = 2;
            snprintf(r->start_note, sizeof(r->start_note), "kirbyfan64 starts the match");
            fake_chat("", "You joined the room", 0);
            if (failed) {
                fake_chat("", "No direct connection to kirbyfan64 after 20 attempts", 0);
            } else {
                fake_chat("", "Direct connection to kirbyfan64", 0);
                fake_chat("kirbyfan64", "gl hf", 0);
            }
        }
        v->status.phase = ZNX_LP_ROOM;
        if (!strcmp(which, "starting") || !strcmp(which, "sync")) {
            v->status.phase = ZNX_LP_STARTING;
            v->status.start_step = !strcmp(which, "sync");
            v->status.start_peers = 1;
            v->status.start_connected = !strcmp(which, "sync");
            v->status.start_seconds_left = !strcmp(which, "sync") ? 9 : 13;
        }
    }
}
