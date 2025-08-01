/*
	Copyright (C) 2016 - 2025
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

#include "chat_events.hpp"
#include "chat_log.hpp"
#include "game_initialization/lobby_info.hpp"
#include "gui/widgets/container_base.hpp"

#include <map>
#include <string>

class config;

namespace gui2
{

// ------------ WIDGET -----------{

class listbox;
class multi_page;
class text_box;

namespace implementation
{
	struct builder_chatbox;
}

struct lobby_chat_window
{
	lobby_chat_window(const std::string& name, bool whisper)
		: name(name), whisper(whisper), pending_messages(0)
	{
	}

	std::string name;
	bool whisper;
	int pending_messages;
};

class chatbox : public container_base, public events::chat_handler
{
	friend struct implementation::builder_chatbox;

public:
	explicit chatbox(const implementation::builder_chatbox& builder);

	/** See @ref styled_widget::set_active. */
	virtual void set_active(const bool /*active*/) override
	{
		/* DO NOTHING */
	}

	/** See @ref styled_widget::get_active. */
	virtual bool get_active() const override
	{
		return true;
	}

	/** See @ref styled_widget::get_state. */
	virtual unsigned get_state() const override
	{
		return 0;
	}

	void send_to_server(const ::config& cfg) override;

	/** @todo: remove */
	void set_active_window_changed_callback(const std::function<void(void)>& f)
	{
		active_window_changed_callback_ = f;
	}

	void load_log(std::map<std::string, chatroom_log>& log, bool show_lobby);

protected:
	/**
	 * Initializes the internal sub-widget pointers.
	 * Should be called after initializing the widget's grid.
	 */
	void finalize_setup();

	/** Inherited form @ref chat_handler */
	virtual void user_relation_changed(const std::string& name) override;

	/** Inherited form @ref chat_handler */
	virtual void add_chat_message(const std::chrono::system_clock::time_point& time,
		const std::string& speaker,
		int side,
		const std::string& message,
		events::chat_handler::MESSAGE_TYPE type
		= events::chat_handler::MESSAGE_PRIVATE) override;

	/** Inherited form @ref chat_handler */
	virtual void add_whisper_sent(const std::string& receiver,
		const std::string& message) override;

	/** Inherited form @ref chat_handler */
	virtual void add_whisper_received(const std::string& sender,
		const std::string& message) override;

	/** Inherited form @ref chat_handler */
	virtual void add_chat_room_message_sent(const std::string& room,
		const std::string& message) override;

	/** Inherited form @ref chat_handler */
	virtual void add_chat_room_message_received(const std::string& room,
		const std::string& speaker,
		const std::string& message) override;

private:
	listbox* roomlistbox_;

	multi_page* chat_log_container_;

	text_box* chat_input_;

	std::vector<lobby_chat_window> open_windows_;

	std::size_t active_window_;

	std::function<void(void)> active_window_changed_callback_;

	std::map<std::string, chatroom_log>* log_;

public:
	/** Static type getter that does not rely on the widget being constructed. */
	static const std::string& type();

private:
	/** Inherited from styled_widget, implemented by REGISTER_WIDGET. */
	virtual const std::string& get_control_type() const override;

	/** See @ref container_base::set_self_active. */
	virtual void set_self_active(const bool /*active*/) override
	{
		/* DO NOTHING */
	}

	void chat_input_keypress_callback(const SDL_Keycode key);

	void append_to_chatbox(const std::string& text, const bool force_scroll = false);

	void append_to_chatbox(const std::string& text, std::size_t id, const bool force_scroll = false);

	/** @returns true if the whisper window for "name" is the active window. */
	bool whisper_window_active(const std::string& name);

	/** @returns true if the room window for "room" is the active window. */
	bool room_window_active(const std::string& room);

	/** Mark the whisper window for "name" as having one more pending message. */
	void increment_waiting_whispers(const std::string& name);

	/** Mark the room window for "room" as having one more pending message. */
	void increment_waiting_messages(const std::string& room);

	/** Add a whisper message to the whisper window. */
	void add_whisper_window_whisper(const std::string& sender,
		const std::string& message);

	/**
	 * Add a whisper message to the current window which is not the whisper window for "name".
	 */
	void add_active_window_whisper(const std::string& sender,
		const std::string& message,
		const bool force_scroll = false);

	/** Add a message to the window for room "room". */
	void add_room_window_message(const std::string& room,
		const std::string& sender,
		const std::string& message);

	/** Add a message to the window for room "room". */
	void add_active_window_message(const std::string& sender,
		const std::string& message,
		const bool force_scroll = false);

	void close_window(std::size_t idx);

public:
	/** Inherited form @ref chat_handler */
	virtual void send_chat_message(const std::string& message, bool allies_only) override;

	virtual void clear_messages() override;

	/**
	 * Switch to the window given by a valid pointer (e.g. received from a call
	 * to *_window_open)
	 */
	void switch_to_window(lobby_chat_window* t);

	void switch_to_window(std::size_t id);

	void active_window_changed();

	/**
	 * Check if a room window for "room" is open, if open_new is true
	 * then it will be created if not found. If allow_close is false, the
	 * 'close' button will be disabled.
	 * @return valid ptr if the window was found or added, null otherwise
	 */
	lobby_chat_window* room_window_open(const std::string& room,
		const bool open_new, const bool allow_close = true);

	/**
	 * Check if a whisper window for user "name" is open, if open_new is true
	 * then it will be created if not found.
	 * @return valid ptr if the window was found or added, null otherwise
	 */
	lobby_chat_window* whisper_window_open(const std::string& name, bool open_new);

	/**
	 * Helper function to find and open a new window, used by *_window_open
	 */
	lobby_chat_window* find_or_create_window(const std::string& name, const bool whisper, const bool open_new, const bool allow_close, const std::string& initial_text);

	void close_window_button_callback(const std::string& room_name, bool& handled, bool& halt);

	void process_message(const ::config& data, bool whisper = false);

	void process_network_data(const ::config& data);

private:
	void signal_handler_receive_keyboard_focus(const event::ui_event event);
};

// }---------- DEFINITION ---------{

struct chatbox_definition : public styled_widget_definition
{
	explicit chatbox_definition(const config& cfg);

	struct resolution : public resolution_definition
	{
		explicit resolution(const config& cfg);

		builder_grid_ptr grid;
	};
};

// }---------- BUILDER -----------{

namespace implementation
{

struct builder_chatbox : public builder_styled_widget
{
public:
	explicit builder_chatbox(const config& cfg);

	using builder_styled_widget::build;

	virtual std::unique_ptr<widget> build() const override;

private:
};

} // namespace implementation

// }------------ END --------------

} // namespace gui2
