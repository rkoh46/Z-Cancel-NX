// SPDX-License-Identifier: GPL-3.0-or-later
/* See online.h. The screens follow the start screen (menu.c): a list on the left, a detail panel on
 * the right, the Switch's button letters at the bottom, one yellow accent for the selection marker and
 * the connect code. Copy says what happens and what to do next, in the player's words. */

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "online.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "draw.h"
#include "lobby_entry.h"
#include "menu.h"
#include "menu_ui.h"
#include "lobby/znx_lobby.h"
#include "p2p/znx_p2p.h"

enum screen { S_MENU, S_HOST, S_JOINING, S_LOBBY, S_LIST, S_ROOMS /* the lobby's screens (lobby_entry.c) */ };

enum menu_row { ROW_HOST, ROW_CODE, ROW_IP, ROW_LIST, ROW_LOBBY, MENU_ROWS };

#define MAX_LISTINGS 32

static struct {
    struct znx_online_request request;
    enum screen screen;
    int row;
    int list_row;
    char join_target[96];     /* what was typed, as typed */
    char last_code[32], last_ip[64];
    int debug;
    struct znx_p2p_status fake;
    /* the public list, fetched on a thread */
    pthread_t list_thread;
    volatile int list_busy, list_done;
    int list_count, list_total;
    struct znx_p2p_listing list[MAX_LISTINGS], fetched[MAX_LISTINGS];
    int fetched_count;
    char list_error[160];
    char list_host[128];
    int list_port;
    /* the connect code from the traversal server, handed from the lobby thread to this one */
    pthread_mutex_t identity_lock;
    int identity_pending;
    char identity_code[32], identity_token[96];
    char local_ip[48];
} l;

static const char* const l_goodnames[] = {
    /* ref/RMG-K mupen64plus.ini, the GoodName of each game's MD5 (catalog.c): F7C52568..., 2B2D6B29..., 823DFA34... */
    "Super Smash Bros. (U) [!]",
    "SmashRemix2.0.1",
    "SmashRemix2.0.1 +EXTRA (0.6.0)",
};

const char* znx_online_goodname(int game)
{
    return game >= 0 && game < 3 ? l_goodnames[game] : "";
}

/* The start screen's name for a game name n02 announced, or the name itself if it is none of the three. */
static const char* readable_game(const char* goodname)
{
    static const char* const titles[] = { "Super Smash Bros.", "Smash Remix 2.0.1", "Smash Remix 2.0.1 +EXTRA 0.6.0" };
    for (int i = 0; i < 3; ++i)
        if (strcmp(goodname, l_goodnames[i]) == 0)
            return titles[i];
    return goodname;
}

/* ---- small helpers ---- */

static void get_status(struct znx_p2p_status* s)
{
    if (l.debug)
        *s = l.fake;
    else
        znx_p2p_status(s);
}

static void split_server(const char* text, const char* default_host, int default_port, char* host, size_t size, int* port)
{
    const char* colon = text != NULL ? strrchr(text, ':') : NULL;
    if (text == NULL || text[0] == '\0') {
        snprintf(host, size, "%s", default_host);
        *port = default_port;
        return;
    }
    if (colon == NULL) {
        snprintf(host, size, "%s", text);
        *port = default_port;
        return;
    }
    snprintf(host, size, "%.*s", (int)(colon - text), text);
    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535)
        *port = default_port;
}

/* This console's address on its own network, for the host screen: the source address a UDP socket
 * would use towards the internet. connect() on UDP sends nothing. */
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

static void on_identity(const char* code, const char* token, void* user)
{
    (void)user;
    pthread_mutex_lock(&l.identity_lock);
    snprintf(l.identity_code, sizeof(l.identity_code), "%s", code);
    snprintf(l.identity_token, sizeof(l.identity_token), "%s", token);
    l.identity_pending = 1;
    pthread_mutex_unlock(&l.identity_lock);
}

/* On this (the menu's) thread: keeps the code the server gave in the settings file, as RMG-K does. */
static void save_identity_if_new(void)
{
    struct znx_menu_settings* s = l.request.settings;
    int pending;
    if (s == NULL)
        return;
    pthread_mutex_lock(&l.identity_lock);
    pending = l.identity_pending;
    if (pending) {
        snprintf(s->p2p_code, sizeof(s->p2p_code), "%s", l.identity_code);
        snprintf(s->p2p_token, sizeof(s->p2p_token), "%s", l.identity_token);
        l.identity_pending = 0;
    }
    pthread_mutex_unlock(&l.identity_lock);
    if (pending)
        znx_settings_save(l.request.base_dir, s);
}

static void p2p_log(const char* line, void* user)
{
    (void)user;
    znx_log_line(line);
}

static int open_session(int host, const char* target, char* error, size_t error_size)
{
    const struct znx_menu_settings* s = l.request.settings;
    struct znx_p2p_config config;
    char nat_host[128], list_host[128];
    int nat_port, list_port;
    split_server(s->nat_server, ZNX_P2P_NAT_HOST, ZNX_P2P_NAT_PORT, nat_host, sizeof(nat_host), &nat_port);
    split_server(s->list_server, ZNX_P2P_LIST_HOST, ZNX_P2P_LIST_PORT, list_host, sizeof(list_host), &list_port);
    memset(&config, 0, sizeof(config));
    config.host = host;
    config.port = s->p2p_port;
    config.game = l.request.goodname;
    config.username = s->name;
    config.app = ZNX_P2P_APP;
    config.join_target = target;
    config.delay = s->input_delay;
    config.prediction = 0;
    config.use_codes = 1;
    config.enlist = s->public_list;
    config.nat_host = nat_host;
    config.nat_port = nat_port;
    config.list_host = list_host;
    config.list_port = list_port;
    config.code = s->p2p_code;
    config.token = s->p2p_token;
    config.save_identity = on_identity;
    config.log = p2p_log;
    return znx_p2p_open(&config, error, error_size);
}

static void message(char* title, size_t title_size, const char* t, char* text, size_t text_size, const char* x)
{
    snprintf(title, title_size, "%s", t);
    snprintf(text, text_size, "%s", x);
}

/* ---- the public list ---- */

static void* list_main(void* arg)
{
    (void)arg;
    l.fetched_count = znx_p2p_fetch_list(l.list_host, l.list_port, l.fetched, MAX_LISTINGS, l.list_error,
                                         sizeof(l.list_error));
    l.list_done = 1;
    return NULL;
}

static void filter_list(void)
{
    l.list_count = 0;
    l.list_total = l.fetched_count < 0 ? 0 : l.fetched_count;
    /* only games with this ROM's name: RMG-K finds the ROM by the host's game name too */
    for (int i = 0; i < l.list_total; ++i)
        if (strcmp(l.fetched[i].game, l.request.goodname) == 0)
            l.list[l.list_count++] = l.fetched[i];
    if (l.list_row >= l.list_count)
        l.list_row = l.list_count > 0 ? l.list_count - 1 : 0;
}

static void start_list_fetch(void)
{
    pthread_attr_t attr;
    if (l.list_busy || l.debug)
        return;
    split_server(l.request.settings->list_server, ZNX_P2P_LIST_HOST, ZNX_P2P_LIST_PORT, l.list_host,
                 sizeof(l.list_host), &l.list_port);
    l.list_done = 0;
    l.list_error[0] = '\0';
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u * 1024u);
    if (pthread_create(&l.list_thread, &attr, list_main, NULL) == 0)
        l.list_busy = 1;
    else
        snprintf(l.list_error, sizeof(l.list_error), "No thread for the list.");
    pthread_attr_destroy(&attr);
}

static void finish_list_fetch(void)
{
    if (!l.list_busy || !l.list_done)
        return;
    pthread_join(l.list_thread, NULL);
    l.list_busy = 0;
    filter_list();
}

/* ---- entering and leaving ---- */

void znx_online_enter(const struct znx_online_request* request)
{
    static int lock_made;
    if (!lock_made) {
        pthread_mutex_init(&l.identity_lock, NULL);
        lock_made = 1;
    }
    l.request = *request;
    l.screen = S_MENU;
    l.debug = 0;
    if (l.row < 0 || l.row >= MENU_ROWS)
        l.row = 0;
}

void znx_online_close(void)
{
    if (!l.debug && znx_p2p_is_open())
        znx_p2p_close();
    znx_lobby_close();
    if (l.list_busy) {
        pthread_join(l.list_thread, NULL);
        l.list_busy = 0;
    }
}

int znx_online_connected(void)
{
    return l.debug ? 0 : znx_p2p_is_open() || znx_lobby_active();
}

int znx_online_lobby_match(void)
{
    return l.screen == S_ROOMS;
}

int znx_online_run_session(void* core, const struct znx_p2p_session_hooks* hooks)
{
    return l.screen == S_ROOMS ? znx_lobby_run_session(core, hooks) : znx_p2p_run_session(core, hooks);
}

int znx_online_prepare_core(void* core)
{
    return l.screen == S_ROOMS ? znx_lobby_prepare_core(core) : 0;
}

int znx_online_input_players(void)
{
    return l.screen == S_ROOMS ? znx_lobby_input_players() : 2;
}

int znx_online_match_ready(void)
{
    return l.screen == S_ROOMS && znx_lobby_match_ready();
}

static enum znx_online_result fail_open(const char* error, char* title, size_t title_size, char* text, size_t text_size)
{
    message(title, title_size, "Online play could not start", text, text_size, error);
    return ZNX_ONLINE_MESSAGE;
}

static enum znx_online_result start_host(char* title, size_t title_size, char* text, size_t text_size)
{
    char error[256];
    find_local_ip();
    if (open_session(1, NULL, error, sizeof(error)) != 0)
        return fail_open(error, title, title_size, text, text_size);
    l.screen = S_HOST;
    return ZNX_ONLINE_STAY;
}

static enum znx_online_result start_join(const char* target, char* title, size_t title_size, char* text, size_t text_size)
{
    char error[256];
    snprintf(l.join_target, sizeof(l.join_target), "%s", target);
    if (open_session(0, target, error, sizeof(error)) != 0)
        return fail_open(error, title, title_size, text, text_size);
    l.screen = S_JOINING;
    return ZNX_ONLINE_STAY;
}

static enum znx_online_result ask_and_join(int by_code, char* title, size_t title_size, char* text, size_t text_size)
{
    char typed[96], code[32];
    if (l.request.keyboard == NULL)
        return ZNX_ONLINE_STAY;
    if (by_code) {
        if (!l.request.keyboard(1, "Connect code", "The host's code, like ABC@12", l.last_code, typed, sizeof(typed)))
            return ZNX_ONLINE_STAY;
        if (!znx_p2p_normalize_code(typed, code, sizeof(code))) {
            message(title, title_size, "That is not a connect code", text, text_size,
                    "A connect code is three or four letters and a number, like ABC@12. The host sees it on their "
                    "screen, or in RMG-K under Peer to Peer.");
            return ZNX_ONLINE_MESSAGE;
        }
        snprintf(l.last_code, sizeof(l.last_code), "%s", code);
        return start_join(code, title, title_size, text, text_size);
    }
    if (!l.request.keyboard(2, "Host address", "IP address and port, like 192.168.1.20:27886", l.last_ip, typed,
                            sizeof(typed)))
        return ZNX_ONLINE_STAY;
    if (typed[strspn(typed, " ")] == '\0')
        return ZNX_ONLINE_STAY;
    snprintf(l.last_ip, sizeof(l.last_ip), "%s", typed);
    return start_join(typed, title, title_size, text, text_size);
}

/* ---- update ---- */

enum znx_online_result znx_online_update(uint32_t pressed, uint32_t held, char* title, size_t title_size,
                                         char* text, size_t text_size)
{
    struct znx_p2p_status s;
    (void)held;
    if (!l.debug)
        save_identity_if_new();
    finish_list_fetch();

    switch (l.screen) {
    case S_MENU:
        if (pressed & ZNX_BTN_UP) l.row = l.row > 0 ? l.row - 1 : 0;
        if (pressed & ZNX_BTN_DOWN) l.row = l.row < MENU_ROWS - 1 ? l.row + 1 : MENU_ROWS - 1;
        if (pressed & ZNX_BTN_B)
            return ZNX_ONLINE_BACK;
        if (pressed & ZNX_BTN_A) {
            switch (l.row) {
            case ROW_HOST: return start_host(title, title_size, text, text_size);
            case ROW_CODE: return ask_and_join(1, title, title_size, text, text_size);
            case ROW_IP: return ask_and_join(0, title, title_size, text, text_size);
            case ROW_LIST:
                l.screen = S_LIST;
                l.list_count = 0;
                l.list_total = 0;
                l.list_row = 0;
                start_list_fetch();
                return ZNX_ONLINE_STAY;
            default: {
                const enum znx_online_result r = znx_lobby_enter(&l.request, title, title_size, text, text_size);
                if (r == ZNX_ONLINE_STAY)
                    l.screen = S_ROOMS;
                return r;
            }
            }
        }
        return ZNX_ONLINE_STAY;

    case S_ROOMS: {
        const enum znx_online_result r = znx_lobby_update(pressed, held, title, title_size, text, text_size);
        if (r == ZNX_ONLINE_BACK || (r == ZNX_ONLINE_MESSAGE && !znx_lobby_active())) {
            l.screen = S_MENU;
            return r == ZNX_ONLINE_BACK ? ZNX_ONLINE_STAY : r;
        }
        return r;
    }

    case S_LIST:
        if (pressed & ZNX_BTN_UP) l.list_row = l.list_row > 0 ? l.list_row - 1 : 0;
        if (pressed & ZNX_BTN_DOWN)
            l.list_row = l.list_row < l.list_count - 1 ? l.list_row + 1 : (l.list_count > 0 ? l.list_count - 1 : 0);
        if (pressed & ZNX_BTN_B) {
            l.screen = S_MENU;
            return ZNX_ONLINE_STAY;
        }
        if (pressed & ZNX_BTN_Y)
            start_list_fetch();
        if ((pressed & ZNX_BTN_A) && !l.list_busy && l.list_count > 0 && !l.debug) {
            snprintf(l.last_code, sizeof(l.last_code), "%s", l.list[l.list_row].code);
            return start_join(l.list[l.list_row].code, title, title_size, text, text_size);
        }
        return ZNX_ONLINE_STAY;

    case S_HOST:
    case S_JOINING:
    case S_LOBBY:
        get_status(&s);
        if (l.debug) {
            if (pressed & ZNX_BTN_B)
                l.screen = S_MENU;
            return ZNX_ONLINE_STAY;
        }
        if (s.phase == ZNX_P2P_FAILED) {
            const int was_lobby = l.screen == S_LOBBY;
            znx_p2p_close();
            l.screen = S_MENU;
            message(title, title_size, s.host ? "Hosting stopped" : was_lobby ? "The host left" : "Could not join",
                    text, text_size, s.message[0] ? s.message : "The connection ended.");
            return ZNX_ONLINE_MESSAGE;
        }
        if (s.phase == ZNX_P2P_STARTING || s.phase == ZNX_P2P_IN_GAME)
            return ZNX_ONLINE_PLAY;
        if (s.phase == ZNX_P2P_LOBBY)
            l.screen = S_LOBBY;
        else if (s.phase == ZNX_P2P_WAITING)
            l.screen = S_HOST;
        if (pressed & ZNX_BTN_B) {
            znx_p2p_close();
            l.screen = S_MENU;
            return ZNX_ONLINE_STAY;
        }
        if (l.screen == S_LOBBY && (pressed & ZNX_BTN_A) && (s.can_ready || s.local_ready))
            znx_p2p_set_ready(!s.local_ready);
        return ZNX_ONLINE_STAY;
    }
    return ZNX_ONLINE_STAY;
}

enum znx_online_result znx_online_after_game(char* title, size_t title_size, char* text, size_t text_size)
{
    struct znx_p2p_status s;
    char why[256];
    if (l.screen == S_ROOMS) {
        /* a lobby match: back in the room, or how it ended */
        const enum znx_online_result r = znx_lobby_after_game(title, title_size, text, text_size);
        if (!znx_lobby_active())
            l.screen = S_MENU;
        return r;
    }
    const int end = znx_p2p_last_end(why, sizeof(why));
    get_status(&s);
    save_identity_if_new();
    if (znx_p2p_is_open() && s.phase == ZNX_P2P_LOBBY) {
        l.screen = S_LOBBY;
        return ZNX_ONLINE_STAY;
    }
    znx_online_close();
    l.screen = S_MENU;
    switch (end) {
    case ZNX_P2P_END_PEER_LEFT:
        message(title, title_size, "The other player left", text, text_size,
                why[0] ? why : "They closed the connection. Host or join again to play another match.");
        break;
    case ZNX_P2P_END_TIMEOUT:
        message(title, title_size, "The connection was lost", text, text_size,
                why[0] ? why : "Nothing arrived from the other player for five seconds.");
        break;
    case ZNX_P2P_END_ERROR:
        message(title, title_size, "The match could not start", text, text_size,
                why[0] ? why : "The reason is in latest.log.");
        break;
    default:
        message(title, title_size, "The match ended", text, text_size, why[0] ? why : "The connection is closed.");
        break;
    }
    return ZNX_ONLINE_MESSAGE;
}

/* ---- drawing ---- */

static void delay_text(const struct znx_p2p_status* s, char* out, size_t size)
{
    if (s->delay_auto)
        snprintf(out, size, "%d frame%s, from the ping", s->delay, s->delay == 1 ? "" : "s");
    else
        snprintf(out, size, "%d frame%s, set by the host", s->delay, s->delay == 1 ? "" : "s");
}

static void setting_delay_text(const struct znx_menu_settings* set, char* out, size_t size)
{
    if (set->input_delay == 0)
        snprintf(out, size, "Auto, from the ping");
    else
        snprintf(out, size, "%d frame%s", set->input_delay, set->input_delay == 1 ? "" : "s");
}

static void draw_menu(void)
{
    const struct hint hints[] = { { "A", "Choose" }, { "B", "Back" } };
    static const char* const titles[MENU_ROWS] = { "Host a game", "Join with a code", "Join by IP address",
                                                   "Public games", "Lobby" };
    const char* status[MENU_ROWS] = {
        "Others join you with your connect code",
        "The host's code, like ABC@12",
        "For a player on your network, or a forwarded port",
        "",
        "RMG-K's rooms: matches of up to four players",
    };
    const struct znx_menu_settings* set = l.request.settings;
    char title[160], delay[32], port[16], list_status[160];
    float y = LIST_Y;
    snprintf(list_status, sizeof(list_status), "Hosts waiting with %s", l.request.title);
    status[ROW_LIST] = list_status;
    snprintf(title, sizeof(title), "Online \xC2\xB7 %s", l.request.title);
    znx_ui_title(title);
    for (int i = 0; i < MENU_ROWS; ++i) {
        znx_ui_row(y, ROW_H, l.row == i, titles[i], C_TEXT1, status[i], C_TEXT2, 0, NULL);
        y += ROW_H + ROW_GAP;
    }
    setting_delay_text(set, delay, sizeof(delay));
    snprintf(port, sizeof(port), "UDP %d", set->p2p_port);
    {
        const char* paragraph = "";
        const char* note = NULL;
        struct field fields[4];
        int count = 0;
        switch (l.row) {
        case ROW_HOST:
            fields[count++] = (struct field){ "Name", set->name, 0 };
            fields[count++] = (struct field){ "Delay", delay, 0 };
            fields[count++] = (struct field){ "Port", port, 1 };
            fields[count++] = (struct field){ "Listed", set->public_list ? "On the public list" : "Not listed", 0 };
            paragraph = "You are player 1. The other player joins with your code from RMG-K's Peer to Peer window or "
                        "from another Switch.";
            note = "Name, delay and the public list are in Settings.";
            break;
        case ROW_CODE:
            fields[count++] = (struct field){ "Name", set->name, 0 };
            paragraph = "You are player 2. The host's code finds them through RMG-K's connect-code server, which also "
                        "opens the way through both routers.";
            note = "The host's delay is used for the match.";
            break;
        case ROW_IP:
            fields[count++] = (struct field){ "Name", set->name, 0 };
            paragraph = "You are player 2. Type the address the host sees on their screen. Over the internet the host "
                        "must forward that UDP port on their router.";
            note = "Without a port, 27886 is used, RMG-K's default.";
            break;
        case ROW_LIST:
            fields[count++] = (struct field){ "Game", l.request.goodname, 0 };
            paragraph = "Players who listed a game on RMG-K's public list and are waiting for someone to join. Only "
                        "games with this ROM are shown.";
            break;
        default: {
            static char server[160];
            snprintf(server, sizeof(server), "%s", set->lobby_server[0] ? set->lobby_server : ZNX_LOBBY_URL_RELEASE);
            fields[count++] = (struct field){ "Name", set->name, 0 };
            fields[count++] = (struct field){ "Server", server, 1 };
            paragraph = "RMG-K's Join Rollback Netplay lobby: open a room or join one with this game, then play "
                        "against RMG-K players and other Switches.";
            note = "Your controller plays whatever seat the room gives you.";
            break;
        }
        }
        znx_ui_panel(LIST_Y, fields, count, paragraph, note);
    }
    znx_ui_footer(hints, 2, NULL);
}

/* The connect code, the screen's one bright moment: big, tabular, in the accent. */
static void draw_code_card(float y, const struct znx_p2p_status* s)
{
    const float h = 168;
    char line[128];
    znx_draw_rect(LIST_X, y, LIST_W, h, 16, C_CARD);
    znx_draw_slant(LIST_X + 24, y + 76, 6, 40, SLANT, C_ACCENT);
    if (s->code[0]) {
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + 48, y + 44, "Your connect code", C_TEXT3, 0);
        znx_draw_text(ZNX_FONT_TITLE, LIST_X + 48, y + 108, s->code, C_ACCENT, ZNX_TEXT_TABULAR);
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + 48, y + 144, "The same code every time this console hosts", C_TEXT3, 0);
    } else if (s->code_state == 2) {
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + 48, y + 44, "No connect code yet: join by address", C_TEXT3, 0);
        if (s->public_address[0])
            snprintf(line, sizeof(line), "%s", s->public_address);
        else if (l.local_ip[0])
            snprintf(line, sizeof(line), "%s:%d", l.local_ip, s->port);
        else
            snprintf(line, sizeof(line), "Port %d", s->port);
        znx_draw_text(ZNX_FONT_TITLE, LIST_X + 48, y + 108, line, C_TEXT1, ZNX_TEXT_TABULAR);
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + 48, y + 144, "The connect-code server does not answer; still asking",
                      C_TEXT3, 0);
    } else {
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + 48, y + 44, "Your connect code", C_TEXT3, 0);
        znx_draw_text(ZNX_FONT_ROW, LIST_X + 48, y + 100, "Asking the connect-code server", C_TEXT2, 0);
    }
}

static void draw_host(const struct znx_p2p_status* s)
{
    const struct hint hints[] = { { "B", "Stop hosting" } };
    const struct znx_menu_settings* set = l.request.settings;
    char lan[96], delay[48], port[16], listed[64];
    float y = LIST_Y;
    znx_ui_title("Waiting for a player");
    draw_code_card(y, s);
    y += 168 + 16;
    if (l.local_ip[0])
        snprintf(lan, sizeof(lan), "%s:%d", l.local_ip, s->port);
    else
        snprintf(lan, sizeof(lan), "Port %d", s->port);
    znx_ui_row(y, 56, 0, "On your network", C_TEXT2, NULL, 0, 0, lan);
    y += 56 + 4;
    snprintf(listed, sizeof(listed), "%s", set->public_list ? (s->enlisted ? "Listed" : "Listing") : "Off");
    znx_ui_row(y, 56, 0, "Public list", C_TEXT2, NULL, 0, 0, listed);
    y += 56 + 4;
    if (s->message[0]) {
        char fitted[200];
        znx_text_fit(ZNX_FONT_SMALL, s->message, LIST_W - 64, fitted, sizeof(fitted));
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + 32, y + 28, fitted, C_TEXT2, 0);
    }
    setting_delay_text(set, delay, sizeof(delay));
    snprintf(port, sizeof(port), "UDP %d", s->port);
    {
        const struct field fields[] = {
            { "Game", l.request.goodname, 0 },
            { "Name", set->name, 0 },
            { "Delay", delay, 0 },
            { "Port", port, 1 },
        };
        znx_ui_panel(LIST_Y, fields, 4,
                     s->code[0] ? "Give the code to the other player. In RMG-K they open Peer to Peer, choose Join and "
                                  "type it; on a Switch, Join with a code."
                                : "Give the other player your address. In RMG-K they open Peer to Peer, choose Join and "
                                  "type it; on a Switch, Join by IP address.",
                     "If they cannot get through, forward this UDP port on your router and give them your address.");
    }
    znx_ui_footer(hints, 1, NULL);
}

static void draw_joining(void)
{
    const struct hint hints[] = { { "B", "Cancel" } };
    char code[32], title[160];
    const int by_code = znx_p2p_normalize_code(l.join_target, code, sizeof(code));
    const char* steps[2] = { by_code ? "Find the host" : "Call the host", "Log in and get the host's settings" };
    float y = LIST_Y;
    snprintf(title, sizeof(title), "Joining %s", by_code ? code : l.join_target);
    znx_ui_title(title);
    for (int i = 0; i < 2; ++i) {
        const int current = i == 0;
        const float h = current ? 104 : 64;
        if (current) {
            znx_draw_rect(LIST_X, y, LIST_W, h, 12, C_SELECTED);
            znx_draw_slant(LIST_X + 12, y + 16, 6, 40, SLANT, C_ACCENT);
        }
        znx_draw_text(ZNX_FONT_ROW, LIST_X + 32, y + (current ? 36 : 42), steps[i], current ? C_TEXT1 : C_TEXT3, 0);
        if (current)
            znx_draw_text(ZNX_FONT_SMALL, LIST_X + 32, y + 76,
                          by_code ? "Asking RMG-K's connect-code server where the host is" : "Waiting for the host to answer",
                          C_TEXT2, 0);
        y += h + ROW_GAP;
    }
    {
        const struct field fields[] = {
            { by_code ? "Code" : "Address", by_code ? code : l.join_target, 1 },
            { "Game", l.request.goodname, 0 },
            { "Name", l.request.settings->name, 0 },
        };
        znx_ui_panel(LIST_Y, fields, 3,
                     by_code ? "The server tells both consoles where the other is, and both send a few packets to open "
                               "the way through their routers. This takes a few seconds."
                             : "The host must be waiting with a game open. Over the internet their UDP port must be "
                               "forwarded.",
                     by_code ? "Gives up after 15 seconds." : "Gives up after 7 seconds.");
    }
    znx_ui_footer(hints, 1, NULL);
}

/* One player before the match: port number, name, ready state, and who it is. */
static void draw_player(float y, int port, const char* name, int ready, const char* role)
{
    char label[8], fitted[64];
    znx_draw_rect(LIST_X, y, LIST_W, ROW_H, 12, C_CARD);
    snprintf(label, sizeof(label), "P%d", port);
    znx_draw_text(ZNX_FONT_BODY, LIST_X + 24, y + 36, label, C_TEXT3, ZNX_TEXT_TABULAR);
    znx_text_fit(ZNX_FONT_ROW, name, LIST_W - 220, fitted, sizeof(fitted));
    znx_draw_text(ZNX_FONT_ROW, LIST_X + 72, y + 36, fitted, C_TEXT1, 0);
    znx_draw_rect(LIST_X + 72, y + 64 - znx_cap_height(ZNX_FONT_SMALL) / 2 - 4, 8, 8, 4, ready ? C_READY : C_TEXT3);
    znx_draw_text(ZNX_FONT_SMALL, LIST_X + 88, y + 64, ready ? "Ready" : "Not ready", ready ? C_TEXT1 : C_TEXT2, 0);
    if (role[0])
        znx_draw_text(ZNX_FONT_BODY, LIST_X + LIST_W - 24, y + 36, role, C_TEXT2, ZNX_TEXT_RIGHT);
}

static void draw_lobby(const struct znx_p2p_status* s)
{
    const struct hint hints_ready[] = { { "A", "Ready" }, { "B", "Leave" } };
    const struct hint hints_unready[] = { { "A", "Not ready" }, { "B", "Leave" } };
    const struct hint hints_leave[] = { { "B", "Leave" } };
    const char* me = l.request.settings->name;
    const char* them = s->peer_name[0] ? s->peer_name : "the other player";
    char title[96], ping[32], delay[64], prediction[32];
    float y = LIST_Y;
    const char* p1 = s->host ? me : them;
    const char* p2 = s->host ? them : me;
    const int r1 = s->host ? s->local_ready : s->peer_ready;
    const int r2 = s->host ? s->peer_ready : s->local_ready;
    const int blocked = s->game_mismatch || s->not_rollback || (!s->can_ready && !s->local_ready);
    snprintf(title, sizeof(title), "Match with %s", them);
    znx_ui_title(title);
    draw_player(y, 1, p1, r1, s->host ? "You, host" : "Host");
    y += ROW_H + ROW_GAP;
    draw_player(y, 2, p2, r2, s->host ? "" : "You");
    y += ROW_H + ROW_GAP + 16;
    if (s->game_mismatch)
        znx_draw_text(ZNX_FONT_BODY, LIST_X + 24, y + 24, "Not the same game: this match cannot start", C_MISSING, 0);
    else if (s->not_rollback)
        znx_draw_text(ZNX_FONT_BODY, LIST_X + 24, y + 24, "The host uses delay-based netplay", C_MISSING, 0);
    if (!blocked) {
        if (s->local_ready && !s->peer_ready)
            znx_draw_text(ZNX_FONT_BODY, LIST_X + 24, y + 24, "Waiting for the other player to be ready", C_TEXT2, 0);
        else if (!s->local_ready && s->peer_ready)
            znx_draw_text(ZNX_FONT_BODY, LIST_X + 24, y + 24, "They are ready: A starts the match", C_TEXT1, 0);
    }

    if (s->ping >= 0) snprintf(ping, sizeof(ping), "%d ms", s->ping);
    else snprintf(ping, sizeof(ping), "Measuring");
    delay_text(s, delay, sizeof(delay));
    snprintf(prediction, sizeof(prediction), "Up to %d frames", s->prediction);
    {
        struct field fields[4];
        int count = 0;
        const char* paragraph;
        const char* note;
        char mismatch[400], app_note[200];
        fields[count++] = (struct field){ "Game", s->host ? l.request.goodname : (s->host_game[0] ? s->host_game : "Asking the host"), 0 };
        fields[count++] = (struct field){ "Ping", ping, 1 };
        fields[count++] = (struct field){ "Delay", delay, 0 };
        fields[count++] = (struct field){ "Rollback", prediction, 0 };
        if (s->game_mismatch) {
            snprintf(mismatch, sizeof(mismatch), "The host plays %s, and you chose %s. Leave, choose the host's game "
                     "on the first screen and join again.", readable_game(s->host_game), l.request.title);
            paragraph = mismatch;
        } else if (s->not_rollback) {
            paragraph = "The host's RMG-K is set to delay-based netplay. Ask them to switch the Peer to Peer window to "
                        "rollback, then join again.";
        } else if (!s->can_ready && !s->local_ready) {
            paragraph = "Getting the host's settings.";
        } else {
            paragraph = "The match starts when both players are ready. To leave the match, hold \xE2\x88\x92\xC2\xA0(Minus), "
                        "or L\xC2\xA0+\xC2\xA0R\xC2\xA0+\xC2\xA0Z\xC2\xA0+\xC2\xA0Start on a GameCube controller.";
        }
        if (s->app_mismatch) {
            snprintf(app_note, sizeof(app_note), "The other player runs %s, not %s: RMG-K warns that the game "
                     "may desync.", s->peer_app[0] ? s->peer_app : "another version", ZNX_P2P_APP);
            note = app_note;
        } else {
            note = s->host ? "You host: your delay is used." : "The host's delay is used.";
        }
        znx_ui_panel(LIST_Y, fields, count, paragraph, note);
    }
    if (blocked)
        znx_ui_footer(hints_leave, 1, NULL);
    else
        znx_ui_footer(s->local_ready ? hints_unready : hints_ready, 2, NULL);
}

static void draw_list(void)
{
    const struct hint hints[] = { { "A", "Join" }, { "Y", "Refresh" }, { "B", "Back" } };
    const struct hint hints_empty[] = { { "Y", "Refresh" }, { "B", "Back" } };
    char count_text[64], status[200];
    znx_ui_title("Public games");
    if (l.list_busy) {
        znx_draw_text(ZNX_FONT_ROW, LIST_X + 32, LIST_Y + 44, "Looking for games", C_TEXT2, 0);
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + 32, LIST_Y + 76, l.list_host, C_TEXT3, 0);
    } else if (l.list_error[0]) {
        znx_draw_text(ZNX_FONT_ROW, LIST_X + 32, LIST_Y + 44, "The list could not be loaded", C_TEXT1, 0);
        znx_draw_wrapped(ZNX_FONT_BODY, LIST_X + 32, LIST_Y + 84, LIST_W - 64, l.list_error, C_TEXT2);
    } else if (l.list_count == 0) {
        char line[200];
        snprintf(line, sizeof(line), "Nobody is waiting with %s", l.request.title);
        znx_draw_text(ZNX_FONT_ROW, LIST_X + 32, LIST_Y + 44, line, C_TEXT1, 0);
        znx_draw_wrapped(ZNX_FONT_BODY, LIST_X + 32, LIST_Y + 84, LIST_W - 64,
                         "Host a game and turn on Public list in Settings so others find you, or look again in a "
                         "minute.", C_TEXT2);
    } else {
        const int visible = 5;
        int first = l.list_row - visible + 1;
        float y = LIST_Y;
        if (first < 0) first = 0;
        for (int i = first; i < l.list_count && i < first + visible; ++i) {
            const struct znx_p2p_listing* g = &l.list[i];
            const int same = strcmp(g->app, ZNX_P2P_APP) == 0;
            snprintf(status, sizeof(status), "%s \xC2\xB7 %s", g->code, g->app);
            znx_ui_row(y, ROW_H, l.list_row == i, g->user[0] ? g->user : "Unnamed", C_TEXT1, status, C_TEXT2,
                       same ? 0 : C_MISSING, NULL);
            y += ROW_H + ROW_GAP;
        }
    }
    if (l.list_total > 0)
        snprintf(count_text, sizeof(count_text), "%d of %d listed game%s %s this ROM", l.list_count, l.list_total,
                 l.list_total == 1 ? "" : "s", l.list_total == 1 ? "has" : "have");
    else
        count_text[0] = '\0';
    if (l.list_count > 0 && !l.list_busy) {
        const struct znx_p2p_listing* g = &l.list[l.list_row];
        char note[200];
        const struct field fields[] = {
            { "Host", g->user, 0 },
            { "Code", g->code, 1 },
            { "Address", g->address, 1 },
            { "Runs", g->app, 0 },
        };
        if (strcmp(g->app, ZNX_P2P_APP) != 0)
            snprintf(note, sizeof(note), "Not %s: RMG-K warns that a match between versions may desync.", ZNX_P2P_APP);
        else
            snprintf(note, sizeof(note), "%s", count_text);
        znx_ui_panel(LIST_Y, fields, 4, "A joins with the host's code, the same as typing it.", note);
        znx_ui_footer(hints, 3, NULL);
    } else {
        const struct field fields[] = { { "Game", l.request.goodname, 0 } };
        znx_ui_panel(LIST_Y, fields, 1,
                     "Players who listed a game on RMG-K's public list and are waiting for someone to join. Only hosts "
                     "with this ROM are shown.",
                     count_text[0] ? count_text : NULL);
        znx_ui_footer(hints_empty, 2, NULL);
    }
}

void znx_online_draw(void)
{
    struct znx_p2p_status s;
    switch (l.screen) {
    case S_MENU: draw_menu(); break;
    case S_LIST: draw_list(); break;
    case S_HOST: get_status(&s); draw_host(&s); break;
    case S_JOINING: draw_joining(); break;
    case S_LOBBY: get_status(&s); draw_lobby(&s); break;
    case S_ROOMS: znx_lobby_draw(); break;
    }
}

/* ---- preview ---- */

void znx_online_debug(const struct znx_online_request* request, const char* which)
{
    znx_online_enter(request);
    if (!strncmp(which, "room:", 5)) {
        l.debug = 1;
        l.screen = S_ROOMS;
        znx_lobby_debug(request, which + 5);
        return;
    }
    l.debug = 1;
    memset(&l.fake, 0, sizeof(l.fake));
    l.fake.port = request->settings->p2p_port;
    l.fake.ping = 41;
    l.fake.delay = 1;
    l.fake.delay_auto = 1;
    l.fake.prediction = 7;
    snprintf(l.local_ip, sizeof(l.local_ip), "192.168.1.42");
    if (!strcmp(which, "host") || !strcmp(which, "host-ip")) {
        l.screen = S_HOST;
        l.fake.host = 1;
        l.fake.phase = ZNX_P2P_WAITING;
        if (!strcmp(which, "host")) {
            snprintf(l.fake.code, sizeof(l.fake.code), "ZNX@7");
            l.fake.code_state = 1;
        } else {
            l.fake.code_state = 2;
            snprintf(l.fake.public_address, sizeof(l.fake.public_address), "203.0.113.24:27886");
        }
    } else if (!strcmp(which, "joining")) {
        l.screen = S_JOINING;
        snprintf(l.join_target, sizeof(l.join_target), "ZNX@7");
        l.fake.phase = ZNX_P2P_CONNECTING;
    } else if (!strncmp(which, "lobby", 5)) {
        const int mismatch = !strcmp(which, "lobby-mismatch");
        l.screen = S_LOBBY;
        l.fake.phase = ZNX_P2P_LOBBY;
        snprintf(l.fake.peer_name, sizeof(l.fake.peer_name), "kirbyfan64");
        snprintf(l.fake.host_game, sizeof(l.fake.host_game), "%s", mismatch ? "SmashRemix2.0.1" : request->goodname);
        l.fake.game_mismatch = mismatch;
        l.fake.can_ready = !mismatch;
        l.fake.peer_ready = !mismatch;
        l.fake.delay = 2;
        l.fake.delay_auto = 1;
    } else if (!strncmp(which, "list", 4)) {
        static const struct znx_p2p_listing fixture[] = {
            { "Super Smash Bros. (U) [!]", "RMG-K v0.9.16", "Isai Fan", "203.0.113.5:27886", "KPF@3" },
            { "SmashRemix2.0.1", "RMG-K v0.9.16", "remixmain", "198.51.100.7:27886", "RMX@41" },
            { "Super Smash Bros. (U) [!]", "RMG-K v0.9.15", "dreamland_dan", "198.51.100.23:27900", "DLD@12" },
            { "Super Smash Bros. (U) [!]", "RMG-K v0.9.16", "Pika", "203.0.113.77:27886", "PKA@108" },
        };
        const int empty = !strcmp(which, "list-empty");
        l.screen = S_LIST;
        l.list_busy = 0;
        l.list_error[0] = '\0';
        l.fetched_count = empty ? 1 : (int)(sizeof(fixture) / sizeof(fixture[0]));
        memcpy(l.fetched, empty ? &fixture[1] : fixture, sizeof(fixture[0]) * (size_t)l.fetched_count);
        l.list_row = 1;
        filter_list();
    }
}
