/*
	Copyright (C) 2008 - 2025
	by Mark de Wever <koraq@xs4all.nl>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#define GETTEXT_DOMAIN "wesnoth-lib"

#include "gui/dialogs/title_screen.hpp"

#include "addon/manager_ui.hpp"
#include "filesystem.hpp"
#include "font/font_config.hpp"
#include "formula/string_utils.hpp"
#include "game_config.hpp"
#include "game_config_manager.hpp"
#include "game_launcher.hpp"
#include "gui/auxiliary/tips.hpp"
#include "gui/dialogs/achievements_dialog.hpp"
#include "gui/dialogs/core_selection.hpp"
#include "gui/dialogs/debug_clock.hpp"
#include "gui/dialogs/help_browser.hpp"
#include "gui/dialogs/lua_interpreter.hpp"
#include "gui/dialogs/message.hpp"
#include "gui/dialogs/multiplayer/mp_host_game_prompt.hpp"
#include "gui/dialogs/multiplayer/mp_method_selection.hpp"
#include "gui/dialogs/preferences_dialog.hpp"
#include "gui/dialogs/screenshot_notification.hpp"
#include "gui/dialogs/simple_item_selector.hpp"
#include "gui/dialogs/game_version_dialog.hpp"
#include "gui/dialogs/gui_test_dialog.hpp"
#include "language.hpp"
#include "log.hpp"
#include "preferences/preferences.hpp"
//#define DEBUG_TOOLTIP
#ifdef DEBUG_TOOLTIP
#include "gui/dialogs/tooltip.hpp"
#endif
#include "gui/widgets/button.hpp"
#include "gui/widgets/image.hpp"
#include "gui/widgets/label.hpp"
#include "gui/widgets/multi_page.hpp"
#include "gui/widgets/settings.hpp"
#include "gui/widgets/window.hpp"
#include "help/help.hpp"
#include "sdl/surface.hpp"
#include "serialization/unicode.hpp"
#include "video.hpp"
#include "wml_exception.hpp"

#include <algorithm>
#include <functional>

#include <boost/algorithm/string/erase.hpp>

static lg::log_domain log_config("config");
#define ERR_CF LOG_STREAM(err, log_config)
#define WRN_CF LOG_STREAM(warn, log_config)

namespace gui2::dialogs
{

REGISTER_DIALOG(title_screen)

bool show_debug_clock_button = false;

title_screen::title_screen(game_launcher& game)
	: modal_dialog(window_id())
	, debug_clock_()
	, game_(game)
{
	set_allow_plugin_skip(false);
	init_callbacks();
}

title_screen::~title_screen()
{
}

void title_screen::register_button(const std::string& id, hotkey::HOTKEY_COMMAND hk, const std::function<void()>& callback)
{
	if(hk != hotkey::HOTKEY_NULL) {
		register_hotkey(hk, [callback](auto&&...) { callback(); return true; });
	}

	try {
		button& btn = find_widget<button>(id);
		connect_signal_mouse_left_click(btn, [callback](auto&&...) { std::invoke(callback); });
	} catch(const wml_exception& e) {
		ERR_GUI_P << e.user_message;
		prefs::get().set_gui2_theme("default");
		set_retval(RELOAD_UI);
	}
}

namespace
{
void show_lua_console()
{
	gui2::dialogs::lua_interpreter::display(gui2::dialogs::lua_interpreter::APP);
}

void make_screenshot()
{
	surface screenshot = video::read_pixels();
	if(screenshot) {
		std::string filename = filesystem::get_screenshot_dir() + "/" + _("Screenshot") + "_";
		filename = filesystem::get_next_filename(filename, ".jpg");
		gui2::dialogs::screenshot_notification::display(filename, screenshot);
	}
}

} // anon namespace

#ifdef DEBUG_TOOLTIP
/*
 * This function is used to test the tooltip placement algorithms as
 * described in the »Tooltip placement« section in the GUI2 design
 * document.
 *
 * Use a 1024 x 768 screen size, set the maximum loop iteration to:
 * - 0   to test with a normal tooltip placement.
 * - 30  to test with a larger normal tooltip placement.
 * - 60  to test with a huge tooltip placement.
 * - 150 to test with a borderline to insanely huge tooltip placement.
 * - 180 to test with an insanely huge tooltip placement.
 */
static void debug_tooltip(window& /*window*/, bool& handled, const point& coordinate)
{
	std::string message = "Hello world.";

	for(int i = 0; i < 0; ++i) {
		message += " More greetings.";
	}

	gui2::tip::remove();
	gui2::tip::show("tooltip", message, coordinate);

	handled = true;
}
#endif

void title_screen::init_callbacks()
{
	set_click_dismiss(false);
	set_enter_disabled(true);
	set_escape_disabled(true);

#ifdef DEBUG_TOOLTIP
	connect_signal<event::SDL_MOUSE_MOTION>(
			std::bind(debug_tooltip, std::ref(*this), std::placeholders::_3, std::placeholders::_5),
			event::dispatcher::front_child);
#endif

	connect_signal<event::SDL_VIDEO_RESIZE>(std::bind(&title_screen::on_resize, this));

	//
	// General hotkeys
	//
	register_hotkey(hotkey::TITLE_SCREEN__RELOAD_WML,
		[this](auto&&...) { set_retval(RELOAD_GAME_DATA); return true; });

	register_hotkey(hotkey::TITLE_SCREEN__TEST,
		[this](auto&&...) { hotkey_callback_select_tests(); return true; });

	register_hotkey(hotkey::TITLE_SCREEN__CORES,
		[this](auto&&...) { button_callback_cores(); return true; });

	register_hotkey(hotkey::LUA_CONSOLE,
		[](auto&&...) { show_lua_console(); return true; });

	/** @todo: should eventually become part of global hotkey handling. */
	register_hotkey(hotkey::HOTKEY_SCREENSHOT,
		[](auto&&...) { make_screenshot(); return true; });

	//
	// Background and logo images
	//
	if(game_config::images::game_title.empty() && game_config::images::game_title_background.empty()) {
		// game works just fine if just one of the background images are missing
		ERR_CF << "No titlescreen background defined in game config";
	}

	get_canvas(0).set_variable("title_image", wfl::variant(game_config::images::game_title));
	get_canvas(0).set_variable("background_image", wfl::variant(game_config::images::game_title_background));

	find_widget<image>("logo-bg").set_image(game_config::images::game_logo_background);
	find_widget<image>("logo").set_image(game_config::images::game_logo);

	//
	// Tip-of-the-day browser
	//
	if(auto tip_pages = find_widget<multi_page>("tips", false, false)) {
		for(const game_tip& tip : tip_of_the_day::shuffle(settings::tips))	{
			tip_pages->add_page({
				{ "tip", {
					{ "use_markup", "true" },
					{ "label", tip.text }
				}},
				{ "source", {
					{ "use_markup", "true" },
					{ "label", tip.source }
				}}
			});
		}

		update_tip(true);
	}

	register_button("next_tip", hotkey::TITLE_SCREEN__NEXT_TIP,
		std::bind(&title_screen::update_tip, this, true));

	register_button("previous_tip", hotkey::TITLE_SCREEN__PREVIOUS_TIP,
		std::bind(&title_screen::update_tip, this, false));

	// Tip panel visiblity and close button
	panel& tip_panel = find_widget<panel>("tip_panel");

	tip_panel.set_visible(prefs::get().show_tips()
		? widget::visibility::visible
		: widget::visibility::hidden);

	if(auto toggle_tips = find_widget<button>("toggle_tip_panel", false, false)) {
		connect_signal_mouse_left_click(*toggle_tips, [&tip_panel](auto&&...) {
			const bool currently_hidden = tip_panel.get_visible() == widget::visibility::hidden;

			tip_panel.set_visible(currently_hidden
				? widget::visibility::visible
				: widget::visibility::hidden);

			// If previously hidden, will now be visible, so we can reuse the same value
			prefs::get().set_show_tips(currently_hidden);
		});
	}

	//
	// Help
	//
	register_button("help", hotkey::HOTKEY_HELP, []() {
		help::help_manager help_manager(&game_config_manager::get()->game_config());
		help::show_help();
	});

	//
	// About
	//
	register_button("about", hotkey::HOTKEY_NULL, [] { game_version::display(); });

	//
	// Campaign
	//
	register_button("campaign", hotkey::TITLE_SCREEN__CAMPAIGN, [this]() {
		try{
			if(game_.new_campaign()) {
				// Suspend drawing of the title screen,
				// so it doesn't flicker in between loading screens.
				hide();
				set_retval(LAUNCH_GAME);
			}
		} catch (const config::error& e) {
			gui2::show_error_message(e.what());
		}
	});

	//
	// Multiplayer
	//
	register_button("multiplayer", hotkey::TITLE_SCREEN__MULTIPLAYER,
		std::bind(&title_screen::button_callback_multiplayer, this));

	//
	// Load game
	//
	register_button("load", hotkey::HOTKEY_LOAD_GAME, [this]() {
		if(game_.load_game()) {
			// Suspend drawing of the title screen,
			// so it doesn't flicker in between loading screens.
			hide();
			set_retval(LAUNCH_GAME);
		}
	});

	//
	// Addons
	//
	register_button("addons", hotkey::TITLE_SCREEN__ADDONS, [this]() {
		if(manage_addons()) {
			set_retval(RELOAD_GAME_DATA);
		}
	});

	//
	// Editor
	//
	register_button("editor", hotkey::TITLE_SCREEN__EDITOR, [this]() { set_retval(MAP_EDITOR); });

	//
	// Language
	//
	register_button("language", hotkey::HOTKEY_LANGUAGE, [this]() {
		try {
			if(game_.change_language()) {
				on_resize();
				update_static_labels();
			}
		} catch(const std::runtime_error& e) {
			gui2::show_error_message(e.what());
		}
	});

	//
	// Preferences
	//
	register_button("preferences", hotkey::HOTKEY_PREFERENCES,
		std::bind(&title_screen::show_preferences, this));

	//
	// Achievements
	//
	register_button("achievements", hotkey::HOTKEY_ACHIEVEMENTS,
		[] { dialogs::achievements_dialog::display(); });

	//
	// Community
	//
	register_button("community", hotkey::HOTKEY_NULL,
		[] { dialogs::game_version::display(4); }); // shows the 5th tab, community

	//
	// Quit
	//
	register_button("quit", hotkey::HOTKEY_QUIT_TO_DESKTOP, [this]() { set_retval(QUIT_GAME); });
	// A sanity check, exit immediately if the .cfg file didn't have a "quit" button.
	find_widget<button>("quit", false, true);

	//
	// Debug clock
	//
	register_button("clock", hotkey::HOTKEY_NULL,
		std::bind(&title_screen::show_debug_clock_window, this));

	auto clock = find_widget<button>("clock", false, false);
	if(clock) {
		clock->set_visible(show_debug_clock_button);
	}

	//
	// GUI Test and Debug Window
	//
	register_button("test_dialog", hotkey::HOTKEY_NULL,
		[] { dialogs::gui_test_dialog::display(); });

	auto test_dialog = find_widget<button>("test_dialog", false, false);
	if(test_dialog) {
		test_dialog->set_visible(show_debug_clock_button);
	}

	//
	// Static labels (version and language)
	//
	update_static_labels();
}

void title_screen::update_static_labels()
{
	//
	// Version menu label
	//
	const std::string& version_string = VGETTEXT("Version $version", {{ "version", game_config::revision }});

	if(label* version_label = find_widget<label>("revision_number", false, false)) {
		version_label->set_label(version_string);
	}

	get_canvas(1).set_variable("revision_number", wfl::variant(version_string));

	//
	// Language menu label
	//
	if(auto* lang_button = find_widget<button>("language", false, false); lang_button) {
		const auto& locale = translation::get_effective_locale_info();
		// Just assume everything is UTF-8 (it should be as long as we're called Wesnoth)
		// and strip the charset from the Boost locale identifier.
		const auto& boost_name = boost::algorithm::erase_first_copy(locale.name(), ".UTF-8");
		const auto& langs = get_languages(true);

		auto lang_def = utils::ranges::find(langs, boost_name, &language_def::localename);
		if(lang_def != langs.end()) {
			lang_button->set_label(lang_def->language.str());
		} else if(boost_name == "c" || boost_name == "C") {
			// HACK: sometimes System Default doesn't match anything on the list. If you fork
			// Wesnoth and change the neutral language to something other than US English, you
			// want to change this too.
			lang_button->set_label("English (US)");
		} else {
			// If somehow the locale doesn't match a known translation, use the
			// locale identifier as a last resort
			lang_button->set_label(boost_name);
		}
	}
}

void title_screen::on_resize()
{
	set_retval(REDRAW_BACKGROUND);
}

void title_screen::update_tip(const bool previous)
{
	multi_page* tip_pages = find_widget<multi_page>("tips", false, false);
	if(tip_pages == nullptr) {
		return;
	}
	if(tip_pages->get_page_count() == 0) {
		return;
	}

	int page = tip_pages->get_selected_page();
	if(previous) {
		if(page <= 0) {
			page = tip_pages->get_page_count();
		}
		--page;
	} else {
		++page;
		if(static_cast<unsigned>(page) >= tip_pages->get_page_count()) {
			page = 0;
		}
	}

	tip_pages->select_page(page);
}

void title_screen::show_debug_clock_window()
{
	assert(show_debug_clock_button);

	if(debug_clock_) {
		debug_clock_.reset(nullptr);
	} else {
		debug_clock_.reset(new debug_clock());
		debug_clock_->show(true);
	}
}

void title_screen::hotkey_callback_select_tests()
{
	game_config_manager::get()->load_game_config_for_create(false, true);

	std::vector<std::string> options;
	for(const config &sc : game_config_manager::get()->game_config().child_range("test")) {
		if(!sc["is_unit_test"].to_bool(false)) {
			options.emplace_back(sc["id"]);
		}
	}

	std::sort(options.begin(), options.end());

	gui2::dialogs::simple_item_selector dlg(_("Choose Test Scenario"), "", options);
	dlg.show();

	int choice = dlg.selected_index();
	if(choice >= 0) {
		game_.set_test(options[choice]);
		set_retval(LAUNCH_GAME);
	}
}

void title_screen::show_preferences()
{
	gui2::dialogs::preferences_dialog pref_dlg;
	pref_dlg.show();
	if (pref_dlg.get_retval() == RELOAD_UI) {
		set_retval(RELOAD_UI);
	}

	// Currently blurred windows don't capture well if there is something
	// on top of them at the time of blur. Resizing the game window in
	// preferences will cause the title screen tip and menu panels to
	// capture the prefs dialog in their blur. This workaround simply
	// forces them to re-capture the blur after the dialog closes.
	panel* tip_panel = find_widget<panel>("tip_panel", false, false);
	if(tip_panel != nullptr) {
		tip_panel->get_canvas(tip_panel->get_state()).queue_reblur();
		tip_panel->queue_redraw();
	}
	panel* menu_panel = find_widget<panel>("menu_panel", false, false);
	if(menu_panel != nullptr) {
		menu_panel->get_canvas(menu_panel->get_state()).queue_reblur();
		menu_panel->queue_redraw();
	}
}

void title_screen::button_callback_multiplayer()
{
	while(true) {
		gui2::dialogs::mp_method_selection dlg;
		dlg.show();

		if(dlg.get_retval() != gui2::retval::OK) {
			return;
		}

		const auto res = dlg.get_choice();

		if(res == decltype(dlg)::choice::HOST && prefs::get().mp_server_warning_disabled() < 2) {
			if(!gui2::dialogs::mp_host_game_prompt::execute()) {
				continue;
			}
		}

		switch(res) {
		case decltype(dlg)::choice::JOIN:
			game_.select_mp_server(prefs::get().builtin_servers_list().front().address);
			set_retval(MP_CONNECT);
			break;
		case decltype(dlg)::choice::CONNECT:
			game_.select_mp_server("");
			set_retval(MP_CONNECT);
			break;
		case decltype(dlg)::choice::HOST:
			game_.select_mp_server("localhost");
			set_retval(MP_HOST);
			break;
		case decltype(dlg)::choice::LOCAL:
			set_retval(MP_LOCAL);
			break;
		}

		return;
	}
}

void title_screen::button_callback_cores()
{
	int current = 0;

	std::vector<config> cores;
	for(const config& core : game_config_manager::get()->game_config().child_range("core")) {
		cores.push_back(core);

		if(core["id"] == prefs::get().core()) {
			current = cores.size() - 1;
		}
	}

	gui2::dialogs::core_selection core_dlg(cores, current);
	if(core_dlg.show()) {
		const std::string& core_id = cores[core_dlg.get_choice()]["id"];

		prefs::get().set_core(core_id);
		set_retval(RELOAD_GAME_DATA);
	}
}

} // namespace dialogs
