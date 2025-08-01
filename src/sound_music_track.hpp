/*
	Copyright (C) 2003 - 2025
	by David White <dave@whitevine.net>, Iris Morelle <shadowm2006@gmail.com>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#pragma once

#include <chrono>
#include <string>
#include <memory>

class config;

namespace sound {

/**
 * Internal representation of music tracks.
 */
class music_track
{
public:
	music_track(const std::string& file_path, const config& node);
	music_track(const std::string& file_path, const std::string& file);

	static std::shared_ptr<music_track> create(const config& cfg);
	static std::shared_ptr<music_track> create(const std::string& file);

	void write(config& parent_node, bool append) const;

	bool append() const { return append_; }
	bool immediate() const { return immediate_; }
	bool shuffle() const { return shuffle_; }
	bool play_once() const { return once_; }
	auto ms_before() const { return ms_before_; }
	auto ms_after()  const { return ms_after_;  }

	const std::string& file_path() const { return file_path_; }
	const std::string& id() const { return id_; }
	const std::string& title() const { return title_; }

	void set_play_once(bool v) { once_ = v; }
	void set_shuffle(bool v) { shuffle_ = v; }
	void set_ms_before(const std::chrono::milliseconds& v) { ms_before_ = v; }
	void set_ms_after(const std::chrono::milliseconds& v) { ms_after_ = v; }
	void set_title(const std::string& v) { title_ = v; }

private:
	std::string id_;
	std::string file_path_;
	std::string title_;

	std::chrono::milliseconds ms_before_{0};
	std::chrono::milliseconds ms_after_{0};

	bool once_ = false;
	bool append_ = false;
	bool immediate_ = false;
	bool shuffle_ = true;
};

std::shared_ptr<music_track> get_track(unsigned int i);
void set_track(unsigned int i, const std::shared_ptr<music_track>& to);

} /* end namespace sound */

inline bool operator==(const sound::music_track& a, const sound::music_track& b) {
	return a.file_path() == b.file_path();
}
inline bool operator!=(const sound::music_track& a, const sound::music_track& b) {
	return !operator==(a, b);
}
