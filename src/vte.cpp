/*
 * Copyright 2013 Giulio Camuffo <giuliocamuffo@gmail.com>
 *
 * This file is part of Termistor
 *
 * Termistor is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Termistor is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Termistor.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <QSocketNotifier>
#include <QFile>
#include <QDebug>

#include "vte.h"
#include "screen.h"
#include "terminal.h"

static const char *sev2str_table[] = {
    "FATAL",
    "ALERT",
    "CRITICAL",
    "ERROR",
    "WARNING",
    "NOTICE",
    "INFO",
    "DEBUG"
};

static const char *sev2str(unsigned int sev)
{
    if (sev > 7)
        return "DEBUG";

    return sev2str_table[sev];
}

static void log(void *data, const char *file, int line, const char *func, const char *subs, unsigned int sev, const char *format, va_list args)
{
    char msg[256];
    int n = snprintf(msg, 256, "%s: %s: ", sev2str(sev), subs);
    vsnprintf(msg + n, 256 - n, format, args);
//     fprintf(stderr, "%s                               \n",msg);
    Debugger::print(msg);
}


static uint8_t color_palette_solarized_white[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]         = {  63,  63,  63 }, /* black */
	[TSM_COLOR_RED]           = { 112,  80,  80 }, /* red */
	[TSM_COLOR_GREEN]         = { 96,  180, 138 }, /* green */
	[TSM_COLOR_YELLOW]        = { 223, 175, 143 }, /* yellow */
	[TSM_COLOR_BLUE]          = { 154, 184, 215 }, /* blue */
	[TSM_COLOR_MAGENTA]       = { 220, 140, 195 }, /* magenta */
	[TSM_COLOR_CYAN]          = { 140, 208, 211 }, /* cyan */
	[TSM_COLOR_LIGHT_GREY]    = { 238, 232, 213 }, /* light grey */
	[TSM_COLOR_DARK_GREY]     = { 112, 144, 128 }, /* dark grey */
	[TSM_COLOR_LIGHT_RED]     = { 220, 163, 163 }, /* light red */
	[TSM_COLOR_LIGHT_GREEN]   = { 114, 213, 163 }, /* light green */
	[TSM_COLOR_LIGHT_YELLOW]  = { 240, 223, 175 }, /* light yellow */
	[TSM_COLOR_LIGHT_BLUE]    = { 148, 191, 243 }, /* light blue */
	[TSM_COLOR_LIGHT_MAGENTA] = { 236, 147, 211 }, /* light magenta */
	[TSM_COLOR_LIGHT_CYAN]    = { 147, 224, 227 }, /* light cyan */
	[TSM_COLOR_WHITE]         = { 253, 246, 227 }, /* white */

	[TSM_COLOR_FOREGROUND]    = { 220, 220, 204 }, /* black */
	[TSM_COLOR_BACKGROUND]    = {  44,  44,  44 }, /* light grey */
};

VTE::VTE(Screen *screen)
   : QObject(screen)
   , m_termScreen(screen)
{
    if (tsm_screen_new(&m_screen, log, 0) < 0) {
        tsm_screen_unref(m_screen);
        qFatal("Failed to create tsm screen");
    }

    if (tsm_vte_new(&m_vte, m_screen, [](tsm_vte *, const char *u8, size_t len, void *data) {
                                        static_cast<VTE *>(data)->vte_event(u8, len); }, this, log, 0) < 0) {
        tsm_vte_unref(m_vte);
        tsm_screen_unref(m_screen);
        qFatal("Failed to create tsm vte");
    }
    tsm_vte_set_palette_colors(m_vte, color_palette_solarized_white, TSM_COLOR_NUM);
    tsm_screen_set_max_sb(m_screen, 10000);

    pid_t pid = forkpty(&m_master, NULL, NULL, NULL);
    if (pid == 0) {
        char **argv = new char*[3];
        argv[0] = getenv("SHELL") ? : strdup("/bin/sh");
        argv[1] = strdup("-i");
        argv[2] = 0;

        setenv("TERM", "xterm-256color", 1);
        execve(argv[0], argv, environ);
        fprintf(stderr, "exec failed: %m\n");
        exit(EXIT_FAILURE);
    } else if (pid < 0) {
        fprintf(stderr, "failed to fork and create pty (%d): %m\n", errno);
    }

    fcntl(m_master, F_SETFL, O_NONBLOCK);
    m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &VTE::onSocketActivated);
}

VTE::~VTE()
{
    tsm_vte_unref(m_vte);
    tsm_screen_unref(m_screen);
}

void VTE::resize(int rows, int cols)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = cols;
    ws.ws_row = rows;
    ioctl(m_master, TIOCSWINSZ, &ws);
}

void VTE::paste(const QByteArray &data)
{
    QByteArray d = data;
    d.replace('\n', '\r');
    vte_event(d.constData(), data.length());
}

void VTE::vte_event(const char *u8, size_t len) {
    QFile file;
    file.open(m_master, QIODevice::WriteOnly);
    file.write(u8, len);
}

void VTE::onSocketActivated(int socket) {
    QFile file;
    file.open(socket, QIODevice::ReadOnly);
    QByteArray data = file.readAll();
    if (data.length() == 0) {
        Debugger::print("No data read. Exiting.");
        m_termScreen->close();
        return;
    }
    tsm_vte_input(m_vte, data.constData(), data.length());
    m_termScreen->update();

}

struct {
    int qtkey;
    int sym;
} keysyms[] = {
    { Qt::Key_Left,     XKB_KEY_Left      },
    { Qt::Key_Up,       XKB_KEY_Up        },
    { Qt::Key_Right,    XKB_KEY_Right     },
    { Qt::Key_Down,     XKB_KEY_Down      },
    { Qt::Key_Home,     XKB_KEY_Home      },
    { Qt::Key_End,      XKB_KEY_End       },
    { Qt::Key_Delete,   XKB_KEY_Delete    },
    { Qt::Key_PageUp,   XKB_KEY_Page_Up   },
    { Qt::Key_PageDown, XKB_KEY_Page_Down }
};

static int findSym(int key)
{
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return XKB_KEY_F1 + key - Qt::Key_F1;
    }

    for (unsigned int i = 0; i < sizeof(keysyms) / sizeof(keysyms[0]); ++i) {
        if (keysyms[i].qtkey == key) {
            return keysyms[i].sym;
        }
    }

    return 0;
}

void VTE::keyPress(int key, Qt::KeyboardModifiers modifiers, const QString &string)
{
    if (modifiers & Qt::ShiftModifier) {
        if (key == Qt::Key_PageUp) {
            tsm_screen_sb_page_up(m_screen, 1);
            m_termScreen->update();
            return;
        }
        if (key == Qt::Key_PageDown) {
            tsm_screen_sb_page_down(m_screen, 1);
            m_termScreen->update();
            return;
        }
    }

    QChar c = string.data()[0];

    int mods = 0;
    if (modifiers & Qt::ShiftModifier) mods |= TSM_SHIFT_MASK;
    if (modifiers & Qt::ControlModifier) mods |= TSM_CONTROL_MASK;
    if (modifiers & Qt::AltModifier) mods |= TSM_ALT_MASK;
    if (modifiers & Qt::MetaModifier) mods |= TSM_LOGO_MASK;

    uint32_t ucs4 = c.unicode();
    if (!ucs4) {
        ucs4 = TSM_VTE_INVALID;
    }

    if (tsm_vte_handle_keyboard(m_vte, findSym(key), c.toLatin1(), mods, ucs4)) {
        tsm_screen_sb_reset(m_screen);
    }
}


