/***************************************************************************
 *   Copyright (C) 2008-2009 by Andrzej Rybczak                            *
 *   electricityispower@gmail.com                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/

#include <cassert>
#include <cstdlib>
#include <algorithm>

#include "charset.h"
#include "error.h"
#include "mpdpp.h"

using namespace MPD;

MPD::Connection Mpd;

const char *MPD::Message::PartOfSongsAdded = "Only part of requested songs' list added to playlist!";
const char *MPD::Message::FullPlaylist = "Playlist is full!";
const char *MPD::Message::FunctionDisabledFilteringEnabled = "Function disabled due to enabled filtering in playlist";

Connection::Connection() : itsConnection(0),
			   isCommandsListEnabled(0),
			   itsErrorCode(0),
			   itsMaxPlaylistLength(-1),
			   isIdle(0),
			   supportsIdle(0),
			   itsHost("localhost"),
			   itsPort(6600),
			   itsTimeout(15),
			   itsCurrentStatus(0),
			   itsOldStatus(0),
			   itsStats(0),
			   itsUpdater(0),
			   itsErrorHandler(0)
{
}

Connection::~Connection()
{
	if (itsConnection)
		mpd_connection_free(itsConnection);
	if (itsStats)
		mpd_stats_free(itsStats);
	if (itsOldStatus)
		mpd_status_free(itsOldStatus);
	if (itsCurrentStatus)
		mpd_status_free(itsCurrentStatus);
}

bool Connection::Connect()
{
	if (itsConnection)
		return true;
	itsConnection = mpd_connection_new(itsHost.c_str(), itsPort, itsTimeout*1000 /* timeout is in ms now */);
	if (CheckForErrors())
		return false;
	if (!itsPassword.empty())
		SendPassword();
	itsPoll.fd = mpd_connection_get_fd(itsConnection);
	itsPoll.events = POLLIN;
	supportsIdle = Version() > 13;
	// in UpdateStatus() we compare it to itsElapsedTimer[0],
	// and for the first time it has always evaluate to true
	// so we need it to be zero at this point
	itsElapsedTimer[1] = 0;
	return !CheckForErrors();
}

bool Connection::Connected() const
{
	return itsConnection;
}

void Connection::Disconnect()
{
	if (itsConnection)
		mpd_connection_free(itsConnection);
	if (itsStats)
		mpd_stats_free(itsStats);
	if (itsOldStatus)
		mpd_status_free(itsOldStatus);
	if (itsCurrentStatus)
		mpd_status_free(itsCurrentStatus);
	itsConnection = 0;
	isIdle = 0;
	itsCurrentStatus = 0;
	itsOldStatus = 0;
	itsStats = 0;
	isCommandsListEnabled = 0;
	itsMaxPlaylistLength = -1;
}

float Connection::Version() const
{
	if (!itsConnection)
		return 0;
	const unsigned *version = mpd_connection_get_server_version(itsConnection);
	return version[1] + version[2]*0.1;
}

void Connection::SetHostname(const std::string &host)
{
	size_t at = host.find("@");
	if (at != std::string::npos)
	{
		itsPassword = host.substr(0, at);
		itsHost = host.substr(at+1);
	}
	else
		itsHost = host;
}

bool Connection::SendPassword()
{
	GoBusy();
	assert(!isCommandsListEnabled);
	mpd_run_password(itsConnection, itsPassword.c_str());
	return !CheckForErrors();
}

void Connection::SetStatusUpdater(StatusUpdater updater, void *data)
{
	itsUpdater = updater;
	itsStatusUpdaterUserdata = data;
}

void Connection::SetErrorHandler(ErrorHandler handler, void *data)
{
	itsErrorHandler = handler;
	itsErrorHandlerUserdata = data;
}

void Connection::GoIdle()
{
	if (supportsIdle && !isIdle && mpd_send_idle(itsConnection))
		isIdle = 1;
}

mpd_idle Connection::GoBusy()
{
	if (isIdle && mpd_send_noidle(itsConnection))
	{
		isIdle = 0;
		return mpd_recv_idle(itsConnection, 1);
	}
	return mpd_idle(0);
}

void Connection::UpdateStatus()
{
	if (!itsConnection)
		return;
	
	if (isIdle)
	{
		poll(&itsPoll, 1, 10);
		if (itsPoll.revents & POLLIN)
			GoBusy();
		else
		{
			// count local elapsed time as we don't receive
			// this from mpd while being in idle mode
			time(&itsElapsedTimer[1]);
			double diff = difftime(itsElapsedTimer[1], itsElapsedTimer[0]);
			if (diff >= 1.0 && Mpd.GetState() == psPlay)
			{
				time(&itsElapsedTimer[0]);
				itsElapsed += diff;
				StatusChanges changes;
				changes.ElapsedTime = 1;
				if (itsUpdater)
					itsUpdater(this, changes, itsErrorHandlerUserdata);
			}
			return;
		}
	}
	
	CheckForErrors();
	
	if (!itsConnection)
		return;
	
	if (itsOldStatus)
		mpd_status_free(itsOldStatus);
	
	itsOldStatus = itsCurrentStatus;
	itsCurrentStatus = 0;
	
	itsCurrentStatus = mpd_run_status(itsConnection);
	
	if (!itsMaxPlaylistLength)
		itsMaxPlaylistLength = GetPlaylistLength();
	
	if (CheckForErrors())
		return;
	
	if (itsCurrentStatus && itsUpdater)
	{
		if (supportsIdle)
		{
			// sync local elapsed time counter with mpd
			itsElapsed = mpd_status_get_elapsed_time(itsCurrentStatus);
			// little hack as it seems mpd doesn't always return elapsed
			// time equal to 0 even if song has changed, it sometimes
			// returns the last second, so we need to bypass it by zeroing
			// it in this case.
			if (itsElapsed == mpd_status_get_total_time(itsCurrentStatus))
				itsElapsed = 0;
			time(&itsElapsedTimer[0]);
		}
		else
		{
			time(&itsElapsedTimer[0]);
			if (itsElapsedTimer[0] > itsElapsedTimer[1])
			{
				unsigned mpd_elapsed = mpd_status_get_elapsed_time(itsCurrentStatus);
				if (itsElapsed < mpd_elapsed-2 || itsElapsed+1 > mpd_elapsed)
					itsElapsed = mpd_elapsed;
				else if (Mpd.GetState() == psPlay)
					++itsElapsed;
				time(&itsElapsedTimer[1]);
			}
		}
		
		if (!itsOldStatus)
		{
			itsChanges.Playlist = 1;
			itsChanges.SongID = 1;
			itsChanges.Database = 1;
			itsChanges.DBUpdating = 1;
			itsChanges.Volume = 1;
			itsChanges.ElapsedTime = 1;
			itsChanges.Crossfade = 1;
			itsChanges.Random = 1;
			itsChanges.Repeat = 1;
			itsChanges.Single = 1;
			itsChanges.Consume = 1;
			itsChanges.PlayerState = 1;
			itsChanges.StatusFlags = 1;
		}
		else
		{
			itsChanges.Playlist = mpd_status_get_queue_version(itsOldStatus) != mpd_status_get_queue_version(itsCurrentStatus);
			itsChanges.SongID = mpd_status_get_song_id(itsOldStatus) != mpd_status_get_song_id(itsCurrentStatus);
			itsChanges.Database = mpd_status_get_update_id(itsOldStatus) && !mpd_status_get_update_id(itsCurrentStatus);
			itsChanges.DBUpdating = mpd_status_get_update_id(itsOldStatus) != mpd_status_get_update_id(itsCurrentStatus);
			itsChanges.Volume = mpd_status_get_volume(itsOldStatus) != mpd_status_get_volume(itsCurrentStatus);
			itsChanges.ElapsedTime = mpd_status_get_elapsed_time(itsOldStatus) != mpd_status_get_elapsed_time(itsCurrentStatus);
			itsChanges.Crossfade = mpd_status_get_crossfade(itsOldStatus) != mpd_status_get_crossfade(itsCurrentStatus);
			itsChanges.Random = mpd_status_get_random(itsOldStatus) != mpd_status_get_random(itsCurrentStatus);
			itsChanges.Repeat = mpd_status_get_repeat(itsOldStatus) != mpd_status_get_repeat(itsCurrentStatus);
			itsChanges.Single = mpd_status_get_single(itsOldStatus) != mpd_status_get_single(itsCurrentStatus);
			itsChanges.Consume = mpd_status_get_consume(itsOldStatus) != mpd_status_get_consume(itsCurrentStatus);
			itsChanges.PlayerState = mpd_status_get_state(itsOldStatus) != mpd_status_get_state(itsCurrentStatus);
			itsChanges.StatusFlags = itsChanges.Repeat || itsChanges.Random || itsChanges.Single || itsChanges.Consume || itsChanges.Crossfade || itsChanges.DBUpdating;
		}
		itsUpdater(this, itsChanges, itsErrorHandlerUserdata);
		GoIdle();
	}
}

void Connection::UpdateStats()
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	if (itsStats)
		mpd_stats_free(itsStats);
	itsStats = mpd_run_stats(itsConnection);
}

bool Connection::UpdateDirectory(const std::string &path)
{
	if (!itsConnection)
		return false;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		bool success = mpd_run_update(itsConnection, path.c_str());
		if (!supportsIdle && success)
			UpdateStatus();
		return success;
	}
	else
	{
		assert(!isIdle);
		return mpd_send_update(itsConnection, path.c_str());
	}
	
}

void Connection::Play()
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_play(itsConnection);
	}
	else
	{
		assert(!isIdle);
		mpd_send_play(itsConnection);
	}
}

void Connection::Play(int pos)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_play_pos(itsConnection, pos);
	}
	else
	{
		assert(!isIdle);
		mpd_send_play_pos(itsConnection, pos);
	}
}

void Connection::PlayID(int id)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_play_id(itsConnection, id);
	}
	else
	{
		assert(!isIdle);
		mpd_send_play_id(itsConnection, id);
	}
}

void Connection::Pause(bool state)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_pause(itsConnection, state);
	}
	else
	{
		assert(!isIdle);
		mpd_send_pause(itsConnection, state);
	}
}

void Connection::Toggle()
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_toggle_pause(itsConnection);;
	}
	else
	{
		assert(!isIdle);
		mpd_send_toggle_pause(itsConnection);
	}
}

void Connection::Stop()
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_stop(itsConnection);
	}
	else
	{
		assert(!isIdle);
		mpd_send_stop(itsConnection);
	}
}

void Connection::Next()
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_next(itsConnection);
	}
	else
	{
		assert(!isIdle);
		mpd_send_next(itsConnection);
	}
}

void Connection::Prev()
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_previous(itsConnection);
	}
	else
	{
		assert(!isIdle);
		mpd_send_previous(itsConnection);
	}
}

void Connection::Move(unsigned from, unsigned to)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_move(itsConnection, from, to);
	}
	else
	{
		assert(!isIdle);
		mpd_send_move(itsConnection, from, to);
	}
}

void Connection::Swap(unsigned from, unsigned to)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_swap(itsConnection, from, to);
	}
	else
	{
		assert(!isIdle);
		mpd_send_swap(itsConnection, from, to);
	}
}

void Connection::Seek(unsigned where)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_seek_pos(itsConnection, Mpd.GetCurrentSongPos(), where);
	}
	else
	{
		assert(!isIdle);
		mpd_send_seek_pos(itsConnection, Mpd.GetCurrentSongPos(), where);
	}
}

void Connection::Shuffle()
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_shuffle(itsConnection);
	}
	else
	{
		assert(!isIdle);
		mpd_send_shuffle(itsConnection);
	}
}

void Connection::ClearPlaylist()
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_clear(itsConnection);
	}
	else
	{
		assert(!isIdle);
		mpd_send_clear(itsConnection);
	}
}

void Connection::ClearPlaylist(const std::string &playlist)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_playlist_clear(itsConnection, playlist.c_str());
	}
	else
	{
		mpd_send_playlist_clear(itsConnection, playlist.c_str());
		assert(!isIdle);
	}
}

void Connection::AddToPlaylist(const std::string &path, const Song &s)
{
	if (!s.Empty())
		AddToPlaylist(path, s.GetFile());
}

void Connection::AddToPlaylist(const std::string &path, const std::string &file)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_playlist_add(itsConnection, path.c_str(), file.c_str());
	}
	else
	{
		assert(!isIdle);
		mpd_send_playlist_add(itsConnection, path.c_str(), file.c_str());
	}
}

void Connection::Move(const std::string &path, int from, int to)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
		GoBusy();
	else
		assert(!isIdle);
	mpd_send_playlist_move(itsConnection, path.c_str(), from, to);
	if (!isCommandsListEnabled)
		mpd_response_finish(itsConnection);
}

bool Connection::Rename(const std::string &from, const std::string &to)
{
	if (!itsConnection)
		return false;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		return mpd_run_rename(itsConnection, from.c_str(), to.c_str());
	}
	else
	{
		assert(!isIdle);
		return mpd_send_rename(itsConnection, from.c_str(), to.c_str());
	}
}

void Connection::GetPlaylistChanges(unsigned version, SongList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	if (!version)
		v.reserve(GetPlaylistLength());
	GoBusy();
	mpd_send_queue_changes_meta(itsConnection, version);
	while (mpd_song *s = mpd_recv_song(itsConnection))
		v.push_back(new Song(s, 1));
	mpd_response_finish(itsConnection);
}

Song Connection::GetSong(const std::string &path)
{
	if (!itsConnection)
		return Song();
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_list_all_meta(itsConnection, path.c_str());
	mpd_song *s = mpd_recv_song(itsConnection);
	mpd_response_finish(itsConnection);
	return Song(s);
}

int Connection::GetCurrentSongPos() const
{
	return itsCurrentStatus && isPlaying() ? mpd_status_get_song_pos(itsCurrentStatus) : -1;
}

Song Connection::GetCurrentSong()
{
	assert(!isCommandsListEnabled);
	GoBusy();
	return Song(itsConnection && isPlaying() ? mpd_run_current_song(itsConnection) : 0);
}

void Connection::GetPlaylistContent(const std::string &path, SongList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_list_playlist_meta(itsConnection, path.c_str());
	while (mpd_song *s = mpd_recv_song(itsConnection))
		v.push_back(new Song(s));
	mpd_response_finish(itsConnection);
}

void Connection::SetRepeat(bool mode)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_repeat(itsConnection, mode);
	}
	else
	{
		assert(!isIdle);
		mpd_send_repeat(itsConnection, mode);
	}
}

void Connection::SetRandom(bool mode)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_random(itsConnection, mode);
	}
	else
	{
		assert(!isIdle);
		mpd_send_random(itsConnection, mode);
	}
}

void Connection::SetSingle(bool mode)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_single(itsConnection, mode);
	}
	else
	{
		assert(!isIdle);
		mpd_send_single(itsConnection, mode);
	}
}

void Connection::SetConsume(bool mode)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_consume(itsConnection, mode);
	}
	else
	{
		assert(!isIdle);
		mpd_send_consume(itsConnection, mode);
	}
}

void Connection::SetVolume(unsigned vol)
{
	if (!itsConnection || vol > 100)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	if (mpd_run_set_volume(itsConnection, vol) && !supportsIdle)
		UpdateStatus();
}

std::string Connection::GetReplayGainMode()
{
	if (!itsConnection)
		return "Unknown";
	assert(!isCommandsListEnabled);
	GoBusy();
	if (!mpd_send_command(itsConnection, "replay_gain_status", NULL))
		return "Unknown";
	std::string result;
	if (mpd_pair *pair = mpd_recv_pair_named(itsConnection, "replay_gain_mode"))
	{
		result = pair->value;
		if (!result.empty())
			result[0] = toupper(result[0]);
		mpd_return_pair(itsConnection, pair);
	}
	mpd_response_finish(itsConnection);
	return result;
}

void Connection::SetReplayGainMode(ReplayGainMode mode)
{
	if (!itsConnection)
		return;
	const char *rg_mode;
	switch (mode)
	{
		case rgmOff:
			rg_mode = "off";
			break;
		case rgmTrack:
			rg_mode = "track";
			break;
		case rgmAlbum:
			rg_mode = "album";
			break;
		default:
			FatalError("undefined value of ReplayGainMode!");
	}
	if (!isCommandsListEnabled)
		GoBusy();
	else
		assert(!isIdle);
	if (!mpd_send_command(itsConnection, "replay_gain_mode", rg_mode, NULL))
		return;
	if (!isCommandsListEnabled)
		mpd_response_finish(itsConnection);
}

void Connection::SetCrossfade(unsigned crossfade)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_crossfade(itsConnection, crossfade);
	}
	else
	{
		assert(!isIdle);
		mpd_send_crossfade(itsConnection, crossfade);
	}
}

int Connection::AddSong(const std::string &path, int pos)
{
	if (!itsConnection)
		return -1;
	int id = -1;
	if (GetPlaylistLength() < itsMaxPlaylistLength)
	{
		if (!isCommandsListEnabled)
			GoBusy();
		else
			assert(!isIdle);
		if (pos < 0)
			mpd_send_add_id(itsConnection, path.c_str());
		else
			mpd_send_add_id_to(itsConnection, path.c_str(), pos);
		if (!isCommandsListEnabled)
		{
			id = mpd_recv_song_id(itsConnection);
			mpd_response_finish(itsConnection);
		}
		else
			id = 0;
	}
	else if (itsErrorHandler)
		itsErrorHandler(this, MPD_SERVER_ERROR_PLAYLIST_MAX, Message::FullPlaylist, itsErrorHandlerUserdata);
	return id;
}

int Connection::AddSong(const Song &s, int pos)
{
	return !s.Empty() ? (AddSong((!s.isFromDB() ? "file://" : "") + (s.Localized() ? locale_to_utf_cpy(s.GetFile()) : s.GetFile()), pos)) : -1;
}

void Connection::Add(const std::string &path)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_send_add(itsConnection, path.c_str());
	}
	else
	{
		assert(!isIdle);
		mpd_run_add(itsConnection, path.c_str());
	}
}

bool Connection::AddRandomSongs(size_t number)
{
	if (!itsConnection && !number)
		return false;
	assert(!isCommandsListEnabled);
	
	TagList files;
	
	GoBusy();
	mpd_send_list_all(itsConnection, "/");
	while (mpd_pair *item = mpd_recv_pair_named(itsConnection, "file"))
	{
		files.push_back(item->value);
		mpd_return_pair(itsConnection, item);
	}
	mpd_response_finish(itsConnection);
	
	if (number > files.size())
	{
		if (itsErrorHandler)
			itsErrorHandler(this, 0, "Requested number of random songs is bigger than size of your library!", itsErrorHandlerUserdata);
		return false;
	}
	else
	{
		srand(time(0));
		std::random_shuffle(files.begin(), files.end());
		StartCommandsList();
		TagList::const_iterator it = files.begin()+rand()%(files.size()-number);
		for (size_t i = 0; i < number && it != files.end(); ++i)
			AddSong(*it++);
		CommitCommandsList();
	}
	return true;
}

void Connection::Delete(unsigned pos)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
		GoBusy();
	else
		assert(!isIdle);
	mpd_send_delete(itsConnection, pos);
	if (!isCommandsListEnabled)
		mpd_response_finish(itsConnection);
}

void Connection::DeleteID(unsigned id)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
		GoBusy();
	else
		assert(!isIdle);
	mpd_send_delete_id(itsConnection, id);
	if (!isCommandsListEnabled)
		mpd_response_finish(itsConnection);
}

void Connection::Delete(const std::string &playlist, unsigned pos)
{
	if (!itsConnection)
		return;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		mpd_run_playlist_delete(itsConnection, playlist.c_str(), pos);
	}
	else
	{
		assert(!isIdle);
		mpd_send_playlist_delete(itsConnection, playlist.c_str(), pos);
	}
}

void Connection::StartCommandsList()
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_command_list_begin(itsConnection, 1);
	isCommandsListEnabled = 1;
}

bool Connection::CommitCommandsList()
{
	if (!itsConnection)
		return false;
	assert(isCommandsListEnabled);
	assert(!isIdle);
	mpd_command_list_end(itsConnection);
	mpd_response_finish(itsConnection);
	if (GetPlaylistLength() == itsMaxPlaylistLength && itsErrorHandler)
		itsErrorHandler(this, MPD_SERVER_ERROR_PLAYLIST_MAX, Message::FullPlaylist, itsErrorHandlerUserdata);
	isCommandsListEnabled = 0;
	UpdateStatus();
	return !CheckForErrors();
}

bool Connection::DeletePlaylist(const std::string &name)
{
	if (!itsConnection)
		return false;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		return mpd_run_rm(itsConnection, name.c_str());
	}
	else
	{
		assert(!isIdle);
		return mpd_send_rm(itsConnection, name.c_str());
	}
}

bool Connection::SavePlaylist(const std::string &name)
{
	if (!itsConnection)
		return false;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_save(itsConnection, name.c_str());
	mpd_response_finish(itsConnection);
	return !(mpd_connection_get_error(itsConnection) == MPD_ERROR_SERVER
	&&	 mpd_connection_get_server_error(itsConnection) == MPD_SERVER_ERROR_EXIST);
}

void Connection::GetPlaylists(TagList &v)
{
	if (!itsConnection)
		return;
	ItemList list;
	GetDirectory("/", list);
	for (ItemList::const_iterator it = list.begin(); it != list.end(); ++it)
		if (it->type == itPlaylist)
			v.push_back(it->name);
	FreeItemList(list);
}

void Connection::GetList(TagList &v, mpd_tag_type type)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_search_db_tags(itsConnection, type);
	mpd_search_commit(itsConnection);
	while (mpd_pair *item = mpd_recv_pair_tag(itsConnection, type))
	{
		if (item->value[0] != 0) // do not push empty item
			v.push_back(item->value);
		mpd_return_pair(itsConnection, item);
	}
	mpd_response_finish(itsConnection);
}

void Connection::GetAlbums(const std::string &artist, TagList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_search_db_tags(itsConnection, MPD_TAG_ALBUM);
	if (!artist.empty())
		mpd_search_add_tag_constraint(itsConnection, MPD_OPERATOR_DEFAULT, MPD_TAG_ARTIST, artist.c_str());
	mpd_search_commit(itsConnection);
	while (mpd_pair *item = mpd_recv_pair_tag(itsConnection, MPD_TAG_ALBUM))
	{
		if (item->value[0] != 0) // do not push empty item
			v.push_back(item->value);
		mpd_return_pair(itsConnection, item);
	}
	mpd_response_finish(itsConnection);
}

void Connection::StartSearch(bool exact_match)
{
	if (itsConnection)
		mpd_search_db_songs(itsConnection, exact_match);
}

void Connection::StartFieldSearch(mpd_tag_type item)
{
	if (itsConnection)
	{
		itsSearchedField = item;
		mpd_search_db_tags(itsConnection, item);
	}
}

void Connection::AddSearch(mpd_tag_type item, const std::string &str)
{
	// mpd version < 0.14.* doesn't support empty search constraints
	if (Version() < 14 && str.empty())
		return;
	if (itsConnection)
		mpd_search_add_tag_constraint(itsConnection, MPD_OPERATOR_DEFAULT, item, str.c_str());
}

void Connection::CommitSearch(SongList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_search_commit(itsConnection);
	while (mpd_song *s = mpd_recv_song(itsConnection))
		v.push_back(new Song(s));
	mpd_response_finish(itsConnection);
}

void Connection::CommitSearch(TagList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_search_commit(itsConnection);
	while (mpd_pair *tag = mpd_recv_pair_tag(itsConnection, itsSearchedField))
	{
		if (tag->value[0] != 0) // do not push empty item
			v.push_back(tag->value);
		mpd_return_pair(itsConnection, tag);
	}
	mpd_response_finish(itsConnection);
}

void Connection::GetDirectory(const std::string &path, ItemList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_list_meta(itsConnection, path.c_str());
	while (mpd_entity *item = mpd_recv_entity(itsConnection))
	{
		Item it;
		switch (mpd_entity_get_type(item))
		{
			case MPD_ENTITY_TYPE_DIRECTORY:
				it.name = mpd_directory_get_path(mpd_entity_get_directory(item));
				it.type = itDirectory;
				goto WRITE;
			case MPD_ENTITY_TYPE_SONG:
				it.song = new Song(mpd_song_dup(mpd_entity_get_song(item)));
				it.type = itSong;
				goto WRITE;
			case MPD_ENTITY_TYPE_PLAYLIST:
				it.name = mpd_playlist_get_path(mpd_entity_get_playlist(item));
				it.type = itPlaylist;
				goto WRITE;
			WRITE:
				v.push_back(it);
				break;
			default:
				break;
		}
		mpd_entity_free(item);
	}
	mpd_response_finish(itsConnection);
}

void Connection::GetDirectoryRecursive(const std::string &path, SongList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_list_all_meta(itsConnection, path.c_str());
	while (mpd_song *s = mpd_recv_song(itsConnection))
		v.push_back(new Song(s));
	mpd_response_finish(itsConnection);
}

void Connection::GetSongs(const std::string &path, SongList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_list_meta(itsConnection, path.c_str());
	while (mpd_song *s = mpd_recv_song(itsConnection))
		v.push_back(new Song(s));
	mpd_response_finish(itsConnection);
}

void Connection::GetDirectories(const std::string &path, TagList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_list_meta(itsConnection, path.c_str());
	while (mpd_directory *dir = mpd_recv_directory(itsConnection))
	{
		v.push_back(mpd_directory_get_path(dir));
		mpd_directory_free(dir);
	}
	mpd_response_finish(itsConnection);
}

void Connection::GetOutputs(OutputList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_outputs(itsConnection);
	while (mpd_output *output = mpd_recv_output(itsConnection))
	{
		v.push_back(std::make_pair(mpd_output_get_name(output), mpd_output_get_enabled(output)));
		mpd_output_free(output);
	}
	mpd_response_finish(itsConnection);
}

bool Connection::EnableOutput(int id)
{
	if (!itsConnection)
		return false;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		return mpd_run_enable_output(itsConnection, id);
	}
	else
	{
		assert(!isIdle);
		return mpd_send_enable_output(itsConnection, id);
	}
}

bool Connection::DisableOutput(int id)
{
	if (!itsConnection)
		return false;
	if (!isCommandsListEnabled)
	{
		GoBusy();
		return mpd_run_disable_output(itsConnection, id);
	}
	else
	{
		assert(!isIdle);
		return mpd_send_disable_output(itsConnection, id);
	}
}

void Connection::GetURLHandlers(TagList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_list_url_schemes(itsConnection);
	while (mpd_pair *handler = mpd_recv_pair_named(itsConnection, "handler"))
	{
		v.push_back(handler->value);
		mpd_return_pair(itsConnection, handler);
	}
	mpd_response_finish(itsConnection);
}

void Connection::GetTagTypes(TagList &v)
{
	if (!itsConnection)
		return;
	assert(!isCommandsListEnabled);
	GoBusy();
	mpd_send_list_tag_types(itsConnection);
	while (mpd_pair *tag_type = mpd_recv_pair_named(itsConnection, "tagtype"))
	{
		v.push_back(tag_type->value);
		mpd_return_pair(itsConnection, tag_type);
	}
	mpd_response_finish(itsConnection);
}

int Connection::CheckForErrors()
{
	if ((itsErrorCode = mpd_connection_get_error(itsConnection)) != MPD_ERROR_SUCCESS)
	{
		itsErrorMessage = mpd_connection_get_error_message(itsConnection);
		if (itsErrorCode == MPD_ERROR_SERVER)
		{
			// this is to avoid setting too small max size as we check it before fetching current status
			// setting real max playlist length is in UpdateStatus()
			itsErrorCode = mpd_connection_get_server_error(itsConnection);
			if (itsErrorCode == MPD_SERVER_ERROR_PLAYLIST_MAX && itsMaxPlaylistLength == size_t(-1))
				itsMaxPlaylistLength = 0;
		}
		if (!mpd_connection_clear_error(itsConnection))
			Disconnect();
		if (itsErrorHandler)
			itsErrorHandler(this, itsErrorCode, itsErrorMessage.c_str(), itsErrorHandlerUserdata);
	}
	return itsErrorCode;
}

void MPD::FreeSongList(SongList &l)
{
	for (SongList::iterator i = l.begin(); i != l.end(); ++i)
		delete *i;
	l.clear();
}

void MPD::FreeItemList(ItemList &l)
{
	for (ItemList::iterator i = l.begin(); i != l.end(); ++i)
		delete i->song;
	l.clear();
}

