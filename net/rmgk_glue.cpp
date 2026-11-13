// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-net: what RMG-K's session driver (RMG-Core/rmgk_gekko.cpp, compiled unchanged from
 * ref/RMG-K) needs from the rest of RMG-Core, which znx-net does not build: settings, the log
 * directory, the frame counter, the netplay-save tracker and the n02 .krec recorder. Each is the
 * smallest stand-in that keeps the P2P path's behaviour; none of them touches the wire protocol.
 *
 * Three of RMG-K's own core wrappers (RMG-Core/RollbackNetcode.cpp, also compiled unchanged) are
 * renamed with -D when that file is compiled (rmgk_CoreRollback...) and wrapped here, so znx-net
 * sees every rollback save, load and re-simulated frame: that is where the per-frame checksums
 * for the "check" lines and the rollback statistics come from. The wrappers call RMG-K's code
 * first and change nothing it returns. */

#include <filesystem>
#include <string>

#include "Directories.hpp"
#include "Emulation.hpp"
#include "NetplaySave.hpp"
#include "RollbackNetcode.hpp"
#include "Settings.hpp"
#include "m64p/Api.hpp"
#include "n02_client.h"

#include <gekkonet.h>

#include "znx_net.h"

/* ---- RollbackNetcode.cpp, renamed when compiled ---- */

bool rmgk_CoreRollbackSaveGameStateInto(CoreRollbackState& state, unsigned char* buffer, int capacity, int frame);
bool rmgk_CoreRollbackLoadGameState(const CoreRollbackState& state);
bool rmgk_CoreRollbackRunFrame(int flags);

bool CoreRollbackSaveGameStateInto(CoreRollbackState& state, unsigned char* buffer, int capacity, int frame)
{
    const bool ok = rmgk_CoreRollbackSaveGameStateInto(state, buffer, capacity, frame);
    if (ok)
        znx_net_state_saved(frame);
    return ok;
}

bool CoreRollbackLoadGameState(const CoreRollbackState& state)
{
    const bool ok = rmgk_CoreRollbackLoadGameState(state);
    if (ok)
        znx_net_state_loaded(state.frame);
    return ok;
}

bool CoreRollbackRunFrame(int flags)
{
    const bool ok = rmgk_CoreRollbackRunFrame(flags);
    if (ok)
        znx_net_frame_resimulated();
    return ok;
}

/* ---- Emulation.cpp ---- */

bool CoreSetFrameOutput(int flags)
{
    return m64p::Core.DoCommand(M64CMD_FRAME_OUTPUT_SET, flags, nullptr) == M64ERR_SUCCESS;
}

bool CoreRunFrames(int frames, int flags)
{
    /* Only CoreRollbackAdvanceFrame calls this, and nothing on the P2P path calls that. */
    (void)frames;
    (void)flags;
    return false;
}

int CoreGetCurrentFrameCount(void)
{
    return znx_net_visible_frames();
}

/* ---- Directories.cpp ---- */

std::filesystem::path CoreGetLibraryDirectory(void)
{
    return std::filesystem::path(znx_net_log_dir());
}

/* ---- Settings.cpp: only the rollback logging switches are read on the P2P path ---- */

bool CoreSettingsGetBoolValue(SettingsID settingId)
{
    switch (settingId) {
    case SettingsID::Rollback_VerboseStats:
        return znx_net_verbose_log();
    default:
        return false;
    }
}

/* ---- NetplaySave.cpp: the tracker that publishes in-match save changes (lobby feature) ---- */

void CoreNetplaySaveTrackStateSaved(int frame)
{
    (void)frame;
}

void CoreNetplaySaveTrackEndFrame(int confirmedFrame)
{
    /* rmgk_gekko calls this once per visible frame, after that frame's deferred saves, with
     * gekko_confirmed_frame(): the hook znx-net prints final "check" lines from. */
    znx_net_end_frame(confirmedFrame);
}

void CoreNetplaySaveTrackSessionEnd(int confirmedFrame)
{
    (void)confirmedFrame;
}

/* ---- n02_client.cpp: the .krec recorder (P2P matches are recorded by RMG-K; not here) ---- */

namespace n02
{
void recordingWriteInputs(const void* values, int size)
{
    (void)values;
    (void)size;
}

int recordingFrameCount()
{
    return 0;
}

void recordingRepeatPortChanges()
{
}
} // namespace n02

/* ---- GekkoNet's asio adapter: built out (GEKKONET_NO_ASIO), only the lobby path names it ---- */

extern "C" GekkoNetAdapter* gekko_default_adapter(unsigned short port)
{
    (void)port;
    return nullptr;
}

extern "C" bool gekko_default_adapter_destroy()
{
    return true;
}
