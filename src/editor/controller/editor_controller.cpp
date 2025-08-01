/*
	Copyright (C) 2008 - 2025
	by Tomasz Sniatowski <kailoran@gmail.com>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#define GETTEXT_DOMAIN "wesnoth-editor"

#include "editor/map/context_manager.hpp"

#include "desktop/open.hpp"

#include "editor/action/action.hpp"
#include "editor/action/action_unit.hpp"
#include "editor/action/action_select.hpp"
#include "editor/action/mouse/mouse_action.hpp"
#include "editor/controller/editor_controller.hpp"
#include "editor/palette/terrain_palettes.hpp"
#include "editor/palette/location_palette.hpp"

#include "help/help.hpp"

#include "gui/dialogs/edit_text.hpp"
#include "gui/dialogs/editor/edit_unit.hpp"
#include "gui/dialogs/editor/custom_tod.hpp"
#include "gui/dialogs/editor/tod_new_schedule.hpp"
#include "gui/dialogs/file_dialog.hpp"
#include "gui/dialogs/message.hpp"
#include "gui/dialogs/preferences_dialog.hpp"
#include "gui/dialogs/units_dialog.hpp"
#include "gui/dialogs/transient_message.hpp"

#include "preferences/preferences.hpp"
#include "resources.hpp"
#include "reports.hpp"
#include "wml_exception.hpp"

#include "cursor.hpp"
#include "desktop/clipboard.hpp"
#include "floating_label.hpp"
#include "gettext.hpp"
#include "picture.hpp"
#include "quit_confirmation.hpp"
#include "sdl/input.hpp" // get_mouse_button_mask
#include "serialization/chrono.hpp"
#include "sound.hpp"
#include "units/animation_component.hpp"
#include "units/unit.hpp"
#include "utils/scope_exit.hpp"

#include <functional>

namespace editor
{
namespace
{
auto parse_editor_music(const config_array_view& editor_music_range)
{
	std::vector<std::shared_ptr<sound::music_track>> tracks;

	for(const config& editor_music : editor_music_range) {
		for(const config& music : editor_music.child_range("music")) {
			if(auto track = sound::music_track::create(music)) {
				tracks.push_back(std::move(track));
			}
		}
	}

	if(tracks.empty()) {
		ERR_ED << "No editor music defined";
	}

	return tracks;
}

} // namespace

std::string editor_controller::current_addon_id_ = "";

editor_controller::editor_controller(bool clear_id)
	: controller_base()
	, mouse_handler_base()
	, quit_confirmation(std::bind(&editor_controller::quit_confirm, this))
	, active_menu_(menu_type::none)
	, reports_(new reports())
	, gui_(new editor_display(*this, *reports_))
	, tods_()
	, context_manager_(new context_manager(*gui_.get(), game_config_, clear_id ? "" : editor_controller::current_addon_id_))
	, toolkit_(nullptr)
	, tooltip_manager_()
	, floating_label_manager_(nullptr)
	, help_manager_(nullptr)
	, do_quit_(false)
	, quit_mode_(EXIT_ERROR)
	, music_tracks_(parse_editor_music(game_config_.child_range("editor_music")))
{
	if(clear_id) {
		editor_controller::current_addon_id_ = "";
	}

	init_gui();
	toolkit_.reset(new editor_toolkit(*gui_.get(), game_config_, *context_manager_.get()));
	help_manager_.reset(new help::help_manager(&game_config_));
	context_manager_->locs_ = toolkit_->get_palette_manager()->location_palette_.get();
	init_tods(game_config_);
	get_current_map_context().set_starting_position_labels(gui());
	cursor::set(cursor::NORMAL);

	gui().queue_rerender();
}

void editor_controller::init_gui()
{
	gui().change_display_context(&get_current_map_context());
	gui().add_redraw_observer(std::bind(&editor_controller::display_redraw_callback, this, std::placeholders::_1));
	floating_label_manager_.reset(new font::floating_label_context());
	gui().set_debug_flag(display::DEBUG_COORDINATES, prefs::get().editor_draw_hex_coordinates());
	gui().set_debug_flag(display::DEBUG_TERRAIN_CODES, prefs::get().editor_draw_terrain_codes());
	gui().set_debug_flag(display::DEBUG_NUM_BITMAPS, prefs::get().editor_draw_num_of_bitmaps());
	gui().set_help_string_enabled(prefs::get().editor_help_text_shown());
//	halo_manager_.reset(new halo::manager(*gui_));
//	resources::halo = halo_manager_.get();
//	^ These lines no longer necessary, the gui owns its halo manager.
//	TODO: Should the editor map contexts actually own the halo manager and swap them in and out from the gui?
//	Note that if that is what happens it might not actually be a good idea for the gui to own the halo manager, so that it can be swapped out
//	without deleting it.
}

void editor_controller::init_tods(const game_config_view& game_config)
{
	for (const config &schedule : game_config.child_range("editor_times")) {

		const std::string& schedule_id = schedule["id"];
		/* Use schedule id as the name if schedule name is empty */
		const std::string& schedule_name = schedule["name"].empty() ? schedule["id"] : schedule["name"];
		if (schedule_id.empty()) {
			ERR_ED << "Missing ID attribute in a TOD Schedule.";
			continue;
		}

		tods_map::iterator times = tods_.find(schedule_id);
		if (times == tods_.end()) {
			std::pair<tods_map::iterator, bool> new_times =
				tods_.emplace(schedule_id, std::pair(schedule_name, std::vector<time_of_day>()));
			times = new_times.first;
		} else {
			ERR_ED << "Duplicate TOD Schedule identifiers.";
			continue;
		}

		for (const config &time : schedule.child_range("time")) {
			times->second.second.emplace_back(time);
		}

	}

	if (tods_.empty()) {
		ERR_ED << "No editor time-of-day defined";
	}
}

editor_controller::~editor_controller()
{
	resources::tod_manager = nullptr;
	resources::filter_con = nullptr;

	resources::classification = nullptr;
}

EXIT_STATUS editor_controller::main_loop()
{
	try {
		while (!do_quit_) {
			play_slice();
		}
	} catch (const editor_exception& e) {
		gui2::show_transient_message(_("Fatal error"), e.what());
		return EXIT_ERROR;
	} catch (const wml_exception& e) {
		e.show();
	}
	return quit_mode_;
}

void editor_controller::status_table() {
}

void editor_controller::do_screenshot(const std::string& screenshot_filename /* = "map_screenshot.png" */)
{
	try {
		surface screenshot = gui().screenshot(true);
		if(!screenshot || image::save_image(screenshot, screenshot_filename) != image::save_result::success) {
			ERR_ED << "Screenshot creation failed!";
		}
	} catch (const wml_exception& e) {
		e.show();
	}
}

bool editor_controller::quit_confirm()
{
	std::string modified;
	std::size_t amount = context_manager_->modified_maps(modified);

	std::string message;
	if (amount == 0) {
		message = _("Do you really want to quit?");
	} else if (amount == 1 && get_current_map_context().modified()) {
		message = _("Do you really want to quit? Changes to this map since the last save will be lost.");
	} else {
		message = _("Do you really want to quit? The following maps were modified and all changes since the last save will be lost:");
		message += "\n" + modified;
	}
	return quit_confirmation::show_prompt(message);
}

void editor_controller::unit_editor_dialog()
{
	gui2::dialogs::editor_edit_unit unit_dlg(game_config_, current_addon_id_);
	if (unit_dlg.show()) {
		unit_dlg.write();
	}
}

void editor_controller::custom_tods_dialog()
{
	if (tods_.empty()) {
		gui2::show_error_message(_("No editor time-of-day found."));
		return;
	}

	tod_manager& manager = *get_current_map_context().get_time_manager();
	std::vector<time_of_day> prev_schedule = manager.times();

	gui2::dialogs::custom_tod tod_dlg(manager.times(), manager.get_current_time(), current_addon_id_);

	/* Register callback to the dialog so that the map changes can be
	 * previewed in real time.
	 */
	std::function<void(std::vector<time_of_day>)> update_func(
				std::bind(
						&editor::editor_controller::update_map_schedule,
						this,
						std::placeholders::_1));
	tod_dlg.register_callback(update_func);

	/* Autogenerate schedule id */
	static constexpr std::string_view ts_format = "%Y-%m-%d_%H-%M-%S";
	std::string timestamp = chrono::format_local_timestamp(std::chrono::system_clock::now(), ts_format);
	std::string sch_id = current_addon_id_+"-schedule";
	/* Set correct textdomain */
	t_string sch_name("", "wesnoth-"+current_addon_id_);

	// TODO : Needs better error handling messages
	/* Show dialog and update current schedule */
	if(tod_dlg.show()) {
		/* Save the new schedule */
		std::vector<time_of_day> schedule = tod_dlg.get_schedule();
		if(!gui2::dialogs::tod_new_schedule::execute(sch_id, sch_name)) {
			/* User pressed Cancel. Restore old schedule */
			update_map_schedule(prev_schedule);
			return;
		}

		/* In case the ID or Name field is blank and user presses OK */
		if (sch_id.empty()) {
			sch_id = current_addon_id_ + "-schedule-" + timestamp;
		} else {
			/* Check if the id entered is same as any of the existing ids
			 * If so, replace */
			// TODO : Notify the user if they enter an already existing schedule ID
			for (auto map_elem : tods_) {
				if (sch_id == map_elem.first) {
					sch_id = current_addon_id_ + "-schedule-" + timestamp;
				}
			}
		}

		tods_.emplace(sch_id, std::pair(sch_name, schedule));
		get_current_map_context().replace_schedule(schedule);
		get_current_map_context().save_schedule(sch_id, sch_name);
		gui_->update_tod();
		context_manager_->refresh_all();
	} else {
		/* Restore old schedule */
		update_map_schedule(prev_schedule);
	}
}

void editor_controller::update_map_schedule(const std::vector<time_of_day>& schedule)
{
	get_current_map_context().replace_schedule(schedule);
	gui_->update_tod();
	context_manager_->refresh_all();
}

bool editor_controller::can_execute_command(const hotkey::ui_command& cmd) const
{
	using namespace hotkey; //reduce hotkey:: clutter
	int index = cmd.index;
	switch(cmd.hotkey_command) {
	case HOTKEY_NULL:
		if (index >= 0) {
			unsigned i = static_cast<unsigned>(index);

			switch (active_menu_) {
			case menu_type::map:
				if (i < context_manager_->open_maps()) {
					return true;
				}
				return false;
			case menu_type::load_mru:
			case menu_type::palette:
			case menu_type::area:
			case menu_type::addon:
			case menu_type::side:
			case menu_type::time:
			case menu_type::schedule:
			case menu_type::local_schedule:
			case menu_type::music:
			case menu_type::local_time:
			case menu_type::unit_facing:
			case menu_type::none:
				return true;
			}
		}
		return false;
	case HOTKEY_EDITOR_PALETTE_GROUPS:
		return true;
	case HOTKEY_EDITOR_PALETTE_UPSCROLL:
		return toolkit_->get_palette_manager()->can_scroll_up();
	case HOTKEY_EDITOR_PALETTE_DOWNSCROLL:
		return toolkit_->get_palette_manager()->can_scroll_down();
	case HOTKEY_ZOOM_IN:
		return !gui_->zoom_at_max();
	case HOTKEY_ZOOM_OUT:
		return !gui_->zoom_at_min();
	case HOTKEY_ZOOM_DEFAULT:
	case HOTKEY_FULLSCREEN:
	case HOTKEY_SCREENSHOT:
	case HOTKEY_MAP_SCREENSHOT:
	case HOTKEY_TOGGLE_GRID:
	case HOTKEY_MOUSE_SCROLL:
	case HOTKEY_ANIMATE_MAP:
	case HOTKEY_MUTE:
	case HOTKEY_PREFERENCES:
	case HOTKEY_HELP:
	case HOTKEY_QUIT_GAME:
	case HOTKEY_SCROLL_UP:
	case HOTKEY_SCROLL_DOWN:
	case HOTKEY_SCROLL_LEFT:
	case HOTKEY_SCROLL_RIGHT:
		return true; //general hotkeys we can always do

	case HOTKEY_UNIT_LIST:
		return !get_current_map_context().units().empty();

	// TODO Disabling this for now until the functionality can be implemnted.
	// See the status_table() method
	case HOTKEY_STATUS_TABLE:
		//return !get_current_map_context().teams().empty();
		return false;
	/////////////////////////////

	case HOTKEY_TERRAIN_DESCRIPTION:
		return gui().mouseover_hex().valid();

		// unit tool related
	case HOTKEY_DELETE_UNIT:
	case HOTKEY_RENAME_UNIT:
	case HOTKEY_EDITOR_UNIT_CHANGE_ID:
	case HOTKEY_EDITOR_UNIT_TOGGLE_CANRECRUIT:
	case HOTKEY_EDITOR_UNIT_TOGGLE_RENAMEABLE:
	case HOTKEY_EDITOR_UNIT_TOGGLE_LOYAL:
	case HOTKEY_EDITOR_UNIT_FACING:
	case HOTKEY_UNIT_DESCRIPTION:
	{
		map_location loc = gui_->mouseover_hex();
		const unit_map& units = get_current_map_context().units();
		return (toolkit_->is_mouse_action_set(HOTKEY_EDITOR_TOOL_UNIT) &&
				units.find(loc) != units.end());
	}

	case HOTKEY_UNDO:
	case HOTKEY_EDITOR_PARTIAL_UNDO:
		return get_current_map_context().can_undo();
	case HOTKEY_REDO:
		return get_current_map_context().can_redo();

	case TITLE_SCREEN__RELOAD_WML:
	case HOTKEY_QUIT_TO_DESKTOP:
	case HOTKEY_EDITOR_MAP_NEW:
	case HOTKEY_EDITOR_SCENARIO_NEW:
	case HOTKEY_EDITOR_MAP_LOAD:
	case HOTKEY_EDITOR_MAP_SAVE_AS:
		return true;

	// Can be enabled as long as a valid addon_id is set
	case HOTKEY_EDITOR_EDIT_UNIT:
		return !current_addon_id_.empty();

	// Only enable when editing a scenario
	case HOTKEY_EDITOR_MAP_TO_SCENARIO:
		return get_current_map_context().is_pure_map();

	// Only enable when editing a scenario
	case HOTKEY_EDITOR_CUSTOM_TODS:
	case HOTKEY_EDITOR_SCENARIO_SAVE_AS:
		return !get_current_map_context().is_pure_map();

	case HOTKEY_EDITOR_PBL:
	case HOTKEY_EDITOR_CHANGE_ADDON_ID:
	case HOTKEY_EDITOR_SELECT_ADDON:
	case HOTKEY_EDITOR_OPEN_ADDON:
		return true;

	case HOTKEY_EDITOR_AREA_ADD:
	case HOTKEY_EDITOR_SIDE_NEW:
		return !get_current_map_context().is_pure_map();

	case HOTKEY_EDITOR_SIDE_EDIT:
	case HOTKEY_EDITOR_SIDE_REMOVE:
		return !get_current_map_context().teams().empty();

	// brushes
	case HOTKEY_EDITOR_BRUSH_NEXT:
	case HOTKEY_EDITOR_BRUSH_1:
	case HOTKEY_EDITOR_BRUSH_2:
	case HOTKEY_EDITOR_BRUSH_3:
	case HOTKEY_EDITOR_BRUSH_NW_SE:
	case HOTKEY_EDITOR_BRUSH_SW_NE:
		return get_mouse_action().supports_brushes();

	case HOTKEY_EDITOR_TOOL_NEXT:
		return true;
	case HOTKEY_EDITOR_PALETTE_ITEM_SWAP:
		return toolkit_->get_palette_manager()->active_palette().supports_swap();
	case HOTKEY_EDITOR_MAP_SAVE:
		return get_current_map_context().modified();
	case HOTKEY_EDITOR_MAP_SAVE_ALL:
		{
			std::string dummy;
			return context_manager_->modified_maps(dummy) > 1;
		}
	case HOTKEY_EDITOR_PLAYLIST:
	case HOTKEY_EDITOR_SCHEDULE:
		return !get_current_map_context().is_pure_map();
	case HOTKEY_EDITOR_MAP_SWITCH:
	case HOTKEY_EDITOR_MAP_CLOSE:
		return true;
	case HOTKEY_EDITOR_MAP_REVERT:
		return !get_current_map_context().get_filename().empty()
				&& get_current_map_context().modified();

	// Tools
	// Pure map editing tools this can be used all the time.
	case HOTKEY_EDITOR_TOOL_PAINT:
	case HOTKEY_EDITOR_TOOL_FILL:
	case HOTKEY_EDITOR_TOOL_SELECT:
	case HOTKEY_EDITOR_TOOL_STARTING_POSITION:
		return true;
	// WWL dependent tools which don't rely on defined sides.
	case HOTKEY_EDITOR_SCENARIO_EDIT:
	case HOTKEY_EDITOR_TOOL_LABEL:
	case HOTKEY_EDITOR_TOOL_ITEM:
		return !get_current_map_context().is_pure_map();
	case HOTKEY_EDITOR_TOOL_UNIT:
	case HOTKEY_EDITOR_TOOL_VILLAGE:
		return !get_current_map_context().teams().empty();

	case HOTKEY_EDITOR_AREA_REMOVE:
	case HOTKEY_EDITOR_AREA_RENAME:
	case HOTKEY_EDITOR_LOCAL_TIME:
		return !get_current_map_context().is_pure_map() &&
				!get_current_map_context().get_time_manager()->get_area_ids().empty();

	case HOTKEY_EDITOR_AREA_SAVE:
		return 	!get_current_map_context().is_pure_map() &&
				!get_current_map_context().get_time_manager()->get_area_ids().empty()
				&& !get_current_map_context().map().selection().empty();

	case HOTKEY_EDITOR_SELECTION_EXPORT:
	case HOTKEY_EDITOR_SELECTION_CUT:
	case HOTKEY_EDITOR_SELECTION_COPY:
	case HOTKEY_EDITOR_SELECTION_FILL:
		return !get_current_map_context().map().selection().empty()
				&& !toolkit_->is_mouse_action_set(HOTKEY_EDITOR_CLIPBOARD_PASTE);
	case HOTKEY_EDITOR_SELECTION_RANDOMIZE:
		return (get_current_map_context().map().selection().size() > 1
				&& !toolkit_->is_mouse_action_set(HOTKEY_EDITOR_CLIPBOARD_PASTE));
	case HOTKEY_EDITOR_SELECTION_ROTATE:
	case HOTKEY_EDITOR_SELECTION_FLIP:
	case HOTKEY_EDITOR_CLIPBOARD_PASTE:
		return !context_manager_->clipboard_empty();
	case HOTKEY_EDITOR_CLIPBOARD_ROTATE_CW:
	case HOTKEY_EDITOR_CLIPBOARD_ROTATE_CCW:
	case HOTKEY_EDITOR_CLIPBOARD_FLIP_HORIZONTAL:
	case HOTKEY_EDITOR_CLIPBOARD_FLIP_VERTICAL:
		return !context_manager_->clipboard_empty()
				&& toolkit_->is_mouse_action_set(HOTKEY_EDITOR_CLIPBOARD_PASTE);
	case HOTKEY_EDITOR_SELECT_ALL:
	case HOTKEY_EDITOR_SELECT_NONE:
		return !toolkit_->is_mouse_action_set(HOTKEY_EDITOR_CLIPBOARD_PASTE);
	case HOTKEY_EDITOR_SELECT_INVERSE:
		return !get_current_map_context().map().selection().empty()
				&& !get_current_map_context().map().everything_selected()
				&& !toolkit_->is_mouse_action_set(HOTKEY_EDITOR_CLIPBOARD_PASTE);
	case HOTKEY_EDITOR_MAP_RESIZE:
	case HOTKEY_EDITOR_MAP_GENERATE:
	case HOTKEY_EDITOR_MAP_APPLY_MASK:
	case HOTKEY_EDITOR_MAP_CREATE_MASK_TO:
	case HOTKEY_EDITOR_REFRESH:
	case HOTKEY_EDITOR_UPDATE_TRANSITIONS:
	case HOTKEY_EDITOR_AUTO_UPDATE_TRANSITIONS:
	case HOTKEY_EDITOR_PARTIAL_UPDATE_TRANSITIONS:
	case HOTKEY_EDITOR_NO_UPDATE_TRANSITIONS:
	case HOTKEY_EDITOR_REFRESH_IMAGE_CACHE:
	case HOTKEY_EDITOR_HELP_TEXT_SHOWN:
	case HOTKEY_MINIMAP_CODING_TERRAIN:
	case HOTKEY_MINIMAP_CODING_UNIT:
	case HOTKEY_MINIMAP_DRAW_UNITS:
	case HOTKEY_MINIMAP_DRAW_TERRAIN:
	case HOTKEY_MINIMAP_DRAW_VILLAGES:
	case HOTKEY_EDITOR_REMOVE_LOCATION:
		return true;
	case HOTKEY_EDITOR_DRAW_COORDINATES:
	case HOTKEY_EDITOR_DRAW_TERRAIN_CODES:
	case HOTKEY_EDITOR_DRAW_NUM_OF_BITMAPS:
		return true;
	default:
		return false;
	}
}

hotkey::action_state editor_controller::get_action_state(const hotkey::ui_command& cmd) const
{
	using namespace hotkey;
	int index = cmd.index;
	switch (cmd.hotkey_command) {

	case HOTKEY_EDITOR_UNIT_TOGGLE_LOYAL:
	{
		unit_map::const_unit_iterator un =
				get_current_map_context().units().find(gui_->mouseover_hex());
		return hotkey::on_if(un->loyal());
	}
	case HOTKEY_EDITOR_UNIT_TOGGLE_CANRECRUIT:
	{
		unit_map::const_unit_iterator un =
				get_current_map_context().units().find(gui_->mouseover_hex());
		return hotkey::on_if(un->can_recruit());
	}
	case HOTKEY_EDITOR_UNIT_TOGGLE_RENAMEABLE:
	{
		unit_map::const_unit_iterator un =
				get_current_map_context().units().find(gui_->mouseover_hex());
		return hotkey::on_if(!un->unrenamable());
	}
	//TODO remove hardcoded hotkey names
	case HOTKEY_EDITOR_AUTO_UPDATE_TRANSITIONS:
		return hotkey::selected_if(context_manager_->is_active_transitions_hotkey("editor-auto-update-transitions"));
	case HOTKEY_EDITOR_PARTIAL_UPDATE_TRANSITIONS:
		return hotkey::selected_if(context_manager_->is_active_transitions_hotkey("editor-partial-update-transitions"));
	case HOTKEY_EDITOR_NO_UPDATE_TRANSITIONS:
		return hotkey::selected_if(context_manager_->is_active_transitions_hotkey("editor-no-update-transitions"));
	case HOTKEY_EDITOR_BRUSH_1:
		return hotkey::on_if(toolkit_->is_active_brush("brush-1"));
	case HOTKEY_EDITOR_BRUSH_2:
		return hotkey::on_if(toolkit_->is_active_brush("brush-2"));
	case HOTKEY_EDITOR_BRUSH_3:
		return hotkey::on_if(toolkit_->is_active_brush("brush-3"));
	case HOTKEY_EDITOR_BRUSH_NW_SE:
		return hotkey::on_if(toolkit_->is_active_brush("brush-nw-se"));
	case HOTKEY_EDITOR_BRUSH_SW_NE:
		return hotkey::on_if(toolkit_->is_active_brush("brush-sw-ne"));

	case HOTKEY_TOGGLE_GRID:
		return hotkey::on_if(prefs::get().grid());
	case HOTKEY_EDITOR_SELECT_ALL:
		return hotkey::selected_if(get_current_map_context().map().everything_selected());
	case HOTKEY_EDITOR_SELECT_NONE:
		return hotkey::selected_if(get_current_map_context().map().selection().empty());
	case HOTKEY_EDITOR_TOOL_FILL:
	case HOTKEY_EDITOR_TOOL_LABEL:
	case HOTKEY_EDITOR_TOOL_PAINT:
	case HOTKEY_EDITOR_TOOL_SELECT:
	case HOTKEY_EDITOR_CLIPBOARD_PASTE:
	case HOTKEY_EDITOR_TOOL_STARTING_POSITION:
	case HOTKEY_EDITOR_TOOL_UNIT:
	case HOTKEY_EDITOR_TOOL_VILLAGE:
	case HOTKEY_EDITOR_TOOL_ITEM:
		return hotkey::on_if(toolkit_->is_mouse_action_set(cmd.hotkey_command));
	case HOTKEY_EDITOR_DRAW_COORDINATES:
		return hotkey::on_if(gui_->debug_flag_set(display::DEBUG_COORDINATES));
	case HOTKEY_EDITOR_DRAW_TERRAIN_CODES:
		return hotkey::on_if(gui_->debug_flag_set(display::DEBUG_TERRAIN_CODES));
	case HOTKEY_EDITOR_DRAW_NUM_OF_BITMAPS:
		return hotkey::on_if(gui_->debug_flag_set(display::DEBUG_NUM_BITMAPS));
	case HOTKEY_EDITOR_HELP_TEXT_SHOWN:
		return hotkey::on_if(gui_->help_string_enabled());
	case HOTKEY_MINIMAP_DRAW_VILLAGES:
		return hotkey::on_if(prefs::get().minimap_draw_villages());
	case HOTKEY_MINIMAP_CODING_UNIT:
		return hotkey::on_if(prefs::get().minimap_movement_coding());
	case HOTKEY_MINIMAP_CODING_TERRAIN:
		return hotkey::on_if(prefs::get().minimap_terrain_coding());
	case HOTKEY_MINIMAP_DRAW_UNITS:
		return hotkey::on_if(prefs::get().minimap_draw_units());
	case HOTKEY_MINIMAP_DRAW_TERRAIN:
		return hotkey::on_if(prefs::get().minimap_draw_terrain());
	case HOTKEY_ZOOM_DEFAULT:
		return hotkey::on_if(gui_->get_zoom_factor() == 1.0);

	case HOTKEY_NULL:
		switch (active_menu_) {
		case menu_type::map:
			return hotkey::selected_if(index == context_manager_->current_context_index());
		case menu_type::load_mru:
			return hotkey::action_state::stateless;
		case menu_type::palette:
			return hotkey::action_state::stateless;
		case menu_type::area:
			return hotkey::selected_if(index == get_current_map_context().get_active_area());
		case menu_type::addon:
			return hotkey::action_state::stateless;
		case menu_type::side:
			return hotkey::selected_if(static_cast<std::size_t>(index) == gui_->playing_team_index());
		case menu_type::time:
			return hotkey::selected_if(index ==	get_current_map_context().get_time_manager()->get_current_time());
		case menu_type::local_time:
			return hotkey::selected_if(index ==	get_current_map_context().get_time_manager()->get_current_area_time(
				get_current_map_context().get_active_area()));
		case menu_type::music:
			return hotkey::on_if(get_current_map_context().playlist_contains(music_tracks_[index]));
		case menu_type::schedule:
			{
				tods_map::const_iterator it = tods_.begin();
				std::advance(it, index);
				const std::vector<time_of_day>& times1 = it->second.second;
				const std::vector<time_of_day>& times2 = get_current_map_context().get_time_manager()->times();
				return hotkey::selected_if(times1 == times2);
			}
		case menu_type::local_schedule:
			{
				tods_map::const_iterator it = tods_.begin();
				std::advance(it, index);
				const std::vector<time_of_day>& times1 = it->second.second;
				int active_area = get_current_map_context().get_active_area();
				const std::vector<time_of_day>& times2 = get_current_map_context().get_time_manager()->times(active_area);
				return hotkey::selected_if(times1 == times2);
			}
		case menu_type::unit_facing:
			{
				unit_map::const_unit_iterator un = get_current_map_context().units().find(gui_->mouseover_hex());
				assert(un != get_current_map_context().units().end());
				return hotkey::selected_if(un->facing() == map_location::direction{index});
			}
		case menu_type::none:
			return hotkey::action_state::stateless;
		}
		return hotkey::action_state::on;
	default:
		return command_executor::get_action_state(cmd);
	}
}

bool editor_controller::do_execute_command(const hotkey::ui_command& cmd, bool press, bool release)
{
	using namespace hotkey;
	HOTKEY_COMMAND command = cmd.hotkey_command;
	SCOPE_ED;
	int index = cmd.index;

	// nothing here handles release; fall through to base implementation
	if (!press) {
		return command_executor::do_execute_command(cmd, press, release);
	}

	switch (command) {
	case HOTKEY_NULL:
		switch (active_menu_) {
		case menu_type::map:
			if (index >= 0) {
				unsigned i = static_cast<unsigned>(index);
				if (i < context_manager_->size()) {
					context_manager_->switch_context(index);
					toolkit_->hotkey_set_mouse_action(HOTKEY_EDITOR_TOOL_PAINT);
					return true;
				}
			}
			return false;
		case menu_type::load_mru:
			if (index >= 0) {
				context_manager_->load_mru_item(static_cast<unsigned>(index));
			}
			return true;
		case menu_type::palette:
			toolkit_->get_palette_manager()->set_group(index);
			return true;
		case menu_type::side:
			gui_->set_viewing_team_index(index, true);
			gui_->set_playing_team_index(index);
			toolkit_->get_palette_manager()->draw_contents();
			return true;
		case menu_type::area:
			{
				get_current_map_context().set_active_area(index);
				const std::set<map_location>& area =
						get_current_map_context().get_time_manager()->get_area_by_index(index);
				get_current_map_context().select_area(index);
				gui_->scroll_to_tiles({ area.begin(), area.end() });
				return true;
			}
		case menu_type::addon:
			return true;
		case menu_type::time:
			{
				get_current_map_context().set_starting_time(index);
				gui_->update_tod();
				return true;
			}
		case menu_type::local_time:
			{
				get_current_map_context().set_local_starting_time(index);
				return true;
			}
		case menu_type::music:
			{
				//TODO mark the map as changed
				sound::play_music_once(music_tracks_[index]->id());
				get_current_map_context().toggle_track(music_tracks_[index]);
				return true;
			}
		case menu_type::schedule:
			{
				tods_map::iterator iter = tods_.begin();
				std::advance(iter, index);
				get_current_map_context().replace_schedule(iter->second.second);
				// TODO: test again after the assign-schedule menu is fixed. Should work, though.
				gui_->update_tod();
				return true;
			}
		case menu_type::local_schedule:
			{
				tods_map::iterator iter = tods_.begin();
				std::advance(iter, index);
				get_current_map_context().replace_local_schedule(iter->second.second);
				return true;
			}
		case menu_type::unit_facing:
			{
				unit_map::unit_iterator un = get_current_map_context().units().find(gui_->mouseover_hex());
				assert(un != get_current_map_context().units().end());
				un->set_facing(map_location::direction(index));
				un->anim_comp().set_standing();
				return true;
			}
		case menu_type::none:
			return true;
		}
		return true;

		//Zoom
	case HOTKEY_ZOOM_IN:
		gui_->set_zoom(true);
		get_current_map_context().get_labels().recalculate_labels();
		toolkit_->set_mouseover_overlay(*gui_);
		return true;
	case HOTKEY_ZOOM_OUT:
		gui_->set_zoom(false);
		get_current_map_context().get_labels().recalculate_labels();
		toolkit_->set_mouseover_overlay(*gui_);
		return true;
	case HOTKEY_ZOOM_DEFAULT:
		gui_->toggle_default_zoom();
		get_current_map_context().get_labels().recalculate_labels();
		toolkit_->set_mouseover_overlay(*gui_);
		return true;

		//Palette
	case HOTKEY_EDITOR_PALETTE_GROUPS:
		//TODO this code waits for the gui2 dialog to get ready
//			std::vector< std::pair< std::string, std::string >> blah_items;
//			toolkit_->get_palette_manager()->active_palette().expand_palette_groups_menu(blah_items);
//			int selected = 1; //toolkit_->get_palette_manager()->active_palette().get_selected;
//			gui2::teditor_select_palette_group::execute(selected, blah_items);
		return true;
	case HOTKEY_EDITOR_PALETTE_UPSCROLL:
		toolkit_->get_palette_manager()->scroll_up();
		return true;
	case HOTKEY_EDITOR_PALETTE_DOWNSCROLL:
		toolkit_->get_palette_manager()->scroll_down();
		return true;

	case HOTKEY_QUIT_GAME:
		if(quit_confirmation::quit()) {
			do_quit_ = true;
			quit_mode_ = EXIT_NORMAL;
		}
		return true;
	case HOTKEY_QUIT_TO_DESKTOP:
		quit_confirmation::quit_to_desktop();
		return true;
	case TITLE_SCREEN__RELOAD_WML:
		context_manager_->save_contexts();
		do_quit_ = true;
		quit_mode_ = EXIT_RELOAD_DATA;
		return true;
	case HOTKEY_EDITOR_EDIT_UNIT:
		unit_editor_dialog();
		return true;
	case HOTKEY_EDITOR_CUSTOM_TODS:
		custom_tods_dialog();
		return true;
	case HOTKEY_EDITOR_PALETTE_ITEM_SWAP:
		toolkit_->get_palette_manager()->active_palette().swap();
		return true;
	case HOTKEY_EDITOR_PARTIAL_UNDO:
		if (dynamic_cast<const editor_action_chain*>(get_current_map_context().last_undo_action()) != nullptr) {
			get_current_map_context().partial_undo();
			context_manager_->refresh_after_action();
		} else {
			undo();
		}
		return true;

		//Tool Selection
	case HOTKEY_EDITOR_TOOL_PAINT:
	case HOTKEY_EDITOR_TOOL_FILL:
	case HOTKEY_EDITOR_TOOL_SELECT:
	case HOTKEY_EDITOR_TOOL_STARTING_POSITION:
	case HOTKEY_EDITOR_TOOL_LABEL:
	case HOTKEY_EDITOR_TOOL_UNIT:
	case HOTKEY_EDITOR_TOOL_VILLAGE:
	case HOTKEY_EDITOR_TOOL_ITEM:
		toolkit_->hotkey_set_mouse_action(command);
		return true;

	case HOTKEY_EDITOR_PBL:
		if(initialize_addon()) {
			context_manager_->edit_pbl();
		}
		return true;

	case HOTKEY_EDITOR_CHANGE_ADDON_ID:
		if(initialize_addon()) {
			context_manager_->change_addon_id();
		}
		return true;

	case HOTKEY_EDITOR_SELECT_ADDON:
		initialize_addon();
		return true;

	case HOTKEY_EDITOR_OPEN_ADDON:
	{
		if (!initialize_addon()) {
			gui2::show_error_message("Could not initialize add-on!");
			return true;
		}

		gui2::dialogs::file_dialog dlg;

		dlg.set_title(_("Add-on Files"))
			.set_path(filesystem::get_current_editor_dir(current_addon_id_));

		if (dlg.show()) {
			std::string filepath = dlg.path();
			if (filesystem::is_map(filepath) || filesystem::is_cfg(filepath)) {
				// Open map or scenario
				context_manager_->load_map(filepath, true);
			} else {
				// Open file using OS application for that format
				if (desktop::open_object_is_supported()) {
					desktop::open_object(filepath);
				} else {
					gui2::show_message("", _("Opening files is not supported, contact your packager"), gui2::dialogs::message::auto_close);
				}
			}
		}

		return true;
	}

	case HOTKEY_EDITOR_AREA_ADD:
		add_area();
		return true;

	case HOTKEY_EDITOR_UNIT_CHANGE_ID:
		change_unit_id();
		return true;

	case HOTKEY_EDITOR_UNIT_TOGGLE_RENAMEABLE:
	{
		map_location loc = gui_->mouseover_hex();
		const unit_map::unit_iterator un = get_current_map_context().units().find(loc);
		bool unrenamable = un->unrenamable();
		un->set_unrenamable(!unrenamable);
		return true;
	}
	case HOTKEY_EDITOR_UNIT_TOGGLE_CANRECRUIT:
	{
		map_location loc = gui_->mouseover_hex();
		const unit_map::unit_iterator un = get_current_map_context().units().find(loc);
		bool canrecruit = un->can_recruit();
		un->set_can_recruit(!canrecruit);
		un->anim_comp().set_standing();
		return true;
	}
	case HOTKEY_EDITOR_UNIT_TOGGLE_LOYAL:
	{
		map_location loc = gui_->mouseover_hex();
		const unit_map::unit_iterator un = get_current_map_context().units().find(loc);
		bool loyal = un->loyal();
		un->set_loyal(!loyal);
		return true;
	}
	case HOTKEY_DELETE_UNIT:
	{
		map_location loc = gui_->mouseover_hex();
		perform_delete(std::make_unique<editor_action_unit_delete>(loc));
		return true;
	}
	case HOTKEY_EDITOR_CLIPBOARD_PASTE: //paste is somewhat different as it might be "one action then revert to previous mode"
		toolkit_->hotkey_set_mouse_action(command);
		return true;

		//Clipboard
	case HOTKEY_EDITOR_CLIPBOARD_ROTATE_CW:
		context_manager_->get_clipboard().rotate_60_cw();
		toolkit_->update_mouse_action_highlights();
		return true;
	case HOTKEY_EDITOR_CLIPBOARD_ROTATE_CCW:
		context_manager_->get_clipboard().rotate_60_ccw();
		toolkit_->update_mouse_action_highlights();
		return true;
	case HOTKEY_EDITOR_CLIPBOARD_FLIP_HORIZONTAL:
		context_manager_->get_clipboard().flip_horizontal();
		toolkit_->update_mouse_action_highlights();
		return true;
	case HOTKEY_EDITOR_CLIPBOARD_FLIP_VERTICAL:
		context_manager_->get_clipboard().flip_vertical();
		toolkit_->update_mouse_action_highlights();
		return true;

		//Brushes
	case HOTKEY_EDITOR_BRUSH_NEXT:
		toolkit_->cycle_brush();
		return true;
	case HOTKEY_EDITOR_BRUSH_1:
		toolkit_->set_brush("brush-1");
		return true;
	case HOTKEY_EDITOR_BRUSH_2:
		toolkit_->set_brush("brush-2");
		return true;
	case HOTKEY_EDITOR_BRUSH_3:
		toolkit_->set_brush("brush-3");
		return true;
	case HOTKEY_EDITOR_BRUSH_NW_SE:
		toolkit_->set_brush("brush-nw-se");
		return true;
	case HOTKEY_EDITOR_BRUSH_SW_NE:
		toolkit_->set_brush("brush-sw-ne");
		return true;

	case HOTKEY_EDITOR_SELECTION_COPY:
		copy_selection();
		return true;
	case HOTKEY_EDITOR_SELECTION_CUT:
		cut_selection();
		return true;
	case HOTKEY_EDITOR_AREA_RENAME:
		context_manager_->rename_area_dialog();
		return true;
	case HOTKEY_EDITOR_AREA_SAVE:
		save_area();
		return true;
	case HOTKEY_EDITOR_SELECTION_EXPORT:
		export_selection_coords();
		return true;
	case HOTKEY_EDITOR_SELECT_ALL:
		if(!get_current_map_context().map().everything_selected()) {
			context_manager_->perform_refresh(editor_action_select_all());
			return true;
		}
		[[fallthrough]];
	case HOTKEY_EDITOR_SELECT_INVERSE:
		context_manager_->perform_refresh(editor_action_select_inverse());
		return true;
	case HOTKEY_EDITOR_SELECT_NONE:
		context_manager_->perform_refresh(editor_action_select_none());
		return true;
	case HOTKEY_EDITOR_SELECTION_FILL:
		context_manager_->fill_selection();
		return true;
	case HOTKEY_EDITOR_SELECTION_RANDOMIZE:
		context_manager_->perform_refresh(editor_action_shuffle_area(
				get_current_map_context().map().selection()));
		return true;

	case HOTKEY_EDITOR_SCENARIO_EDIT:
		context_manager_->edit_scenario_dialog();
		return true;

	case HOTKEY_EDITOR_AREA_REMOVE:
		get_current_map_context().remove_area(
				get_current_map_context().get_active_area());
		return true;

	// map specific
	case HOTKEY_EDITOR_MAP_CLOSE:
		context_manager_->close_current_context();
		// Copy behaviour from when switching windows to always reset the active tool to the Paint Tool
		// This avoids the situation of having a scenario-specific tool active in a map context which can cause a crash if used
		// Not elegant but at least avoids a potential crash and is consistent with existing behaviour
		toolkit_->hotkey_set_mouse_action(HOTKEY_EDITOR_TOOL_PAINT);
		return true;
	case HOTKEY_EDITOR_MAP_LOAD:
		context_manager_->load_map_dialog();
		return true;
	case HOTKEY_EDITOR_MAP_REVERT:
		context_manager_->revert_map();
		return true;
	case HOTKEY_EDITOR_MAP_NEW:
		context_manager_->new_map_dialog();
		return true;
	case HOTKEY_EDITOR_SCENARIO_NEW:
		if(initialize_addon()) {
			context_manager_->new_scenario_dialog();
		}
		return true;
	case HOTKEY_EDITOR_MAP_SAVE:
		save_map();
		return true;
	case HOTKEY_EDITOR_MAP_SAVE_ALL:
		context_manager_->save_all_maps();
		return true;
	case HOTKEY_EDITOR_MAP_SAVE_AS:
		context_manager_->save_map_as_dialog();
		return true;
	case HOTKEY_EDITOR_MAP_TO_SCENARIO:
		if(initialize_addon()) {
			context_manager_->map_to_scenario();
		}
		return true;
	case HOTKEY_EDITOR_SCENARIO_SAVE_AS:
		if(initialize_addon()) {
			context_manager_->save_scenario_as_dialog();
		}
		return true;
	case HOTKEY_EDITOR_MAP_GENERATE:
		context_manager_->generate_map_dialog();
		return true;
	case HOTKEY_EDITOR_MAP_APPLY_MASK:
		context_manager_->apply_mask_dialog();
		return true;
	case HOTKEY_EDITOR_MAP_CREATE_MASK_TO:
		context_manager_->create_mask_to_dialog();
		return true;
	case HOTKEY_EDITOR_MAP_RESIZE:
		context_manager_->resize_map_dialog();
		return true;

	// Side specific ones
	case HOTKEY_EDITOR_SIDE_NEW:
		if(get_current_map_context().teams().size() >= 9) {
			std::size_t new_side_num = get_current_map_context().teams().size() + 1;
			toolkit_->get_palette_manager()->location_palette_->add_item(std::to_string(new_side_num));
		}
		get_current_map_context().new_side();
		gui_->init_flags();
		return true;
	case HOTKEY_EDITOR_SIDE_REMOVE:
		gui_->set_viewing_team_index(0, true);
		gui_->set_playing_team_index(0);
		get_current_map_context().remove_side();
		return true;
	case HOTKEY_EDITOR_SIDE_EDIT:
		context_manager_->edit_side_dialog(gui_->viewing_team());
		return true;

	// Transitions
	case HOTKEY_EDITOR_PARTIAL_UPDATE_TRANSITIONS:
		context_manager_->set_update_transitions_mode(2);
		return true;
	case HOTKEY_EDITOR_AUTO_UPDATE_TRANSITIONS:
		context_manager_->set_update_transitions_mode(1);
		return true;
	case HOTKEY_EDITOR_NO_UPDATE_TRANSITIONS:
		context_manager_->set_update_transitions_mode(0);
		return true;
	case HOTKEY_EDITOR_TOGGLE_TRANSITIONS:
		if(context_manager_->toggle_update_transitions()) {
			return true;
		}
		[[fallthrough]];
	case HOTKEY_EDITOR_UPDATE_TRANSITIONS:
		context_manager_->refresh_all();
		return true;
	// Refresh
	case HOTKEY_EDITOR_REFRESH:
		context_manager_->reload_map();
		return true;
	case HOTKEY_EDITOR_REFRESH_IMAGE_CACHE:
		refresh_image_cache();
		return true;

	case HOTKEY_EDITOR_DRAW_COORDINATES:
		gui().toggle_debug_flag(display::DEBUG_COORDINATES);
		prefs::get().set_editor_draw_hex_coordinates(gui().debug_flag_set(display::DEBUG_COORDINATES));
		gui().invalidate_all();
		return true;
	case HOTKEY_EDITOR_DRAW_TERRAIN_CODES:
		gui().toggle_debug_flag(display::DEBUG_TERRAIN_CODES);
		prefs::get().set_editor_draw_terrain_codes(gui().debug_flag_set(display::DEBUG_TERRAIN_CODES));
		gui().invalidate_all();
		return true;
	case HOTKEY_EDITOR_DRAW_NUM_OF_BITMAPS:
		gui().toggle_debug_flag(display::DEBUG_NUM_BITMAPS);
		prefs::get().set_editor_draw_num_of_bitmaps(gui().debug_flag_set(display::DEBUG_NUM_BITMAPS));
		gui().invalidate_all();
		return true;
	case HOTKEY_EDITOR_HELP_TEXT_SHOWN:
		gui().set_help_string_enabled(!gui().help_string_enabled());
		prefs::get().set_editor_help_text_shown(gui().help_string_enabled());
		return true;
	case HOTKEY_EDITOR_REMOVE_LOCATION: {
		location_palette* lp = dynamic_cast<location_palette*>(&toolkit_->get_palette_manager()->active_palette());
		if (lp) {
			perform_delete(std::make_unique<editor_action_starting_position>(map_location(), lp->selected_item()));
			// No idea if this is the right thing to call, but it ensures starting
			// position labels get removed on delete.
			context_manager_->refresh_after_action();
		}
		return true;
	}
	default:
		return hotkey::command_executor::do_execute_command(cmd, press, release);
	}
}

bool editor_controller::initialize_addon() {
	if(current_addon_id_.empty()) {
		// editor::initialize_addon can return empty id in case of failure
		current_addon_id_ = editor::initialize_addon();
	}
	context_manager_->set_addon_id(current_addon_id_);
	return !current_addon_id_.empty();
}

void editor_controller::show_help()
{
	help::show_help("..editor");
}

bool editor_controller::keep_menu_open() const
{
	// Keep the music menu open to allow multiple selections easily
	return active_menu_ == menu_type::music;
}

void editor_controller::show_menu(const std::vector<config>& items_arg, const point& menu_loc, bool context_menu)
{
	// Ensure active_menu_ is only valid within the scope of this function.
	ON_SCOPE_EXIT(this) { active_menu_ = menu_type::none; };

	if(context_menu
		&& !get_current_map_context().map().on_board_with_border(gui().hex_clicked_on(menu_loc.x, menu_loc.y)))
	{
		return;
	}

	std::vector<config> items;
	for(const auto& c : items_arg) {
		const std::string& id = c["id"];
		const auto cmd = hotkey::ui_command(id);

		if((can_execute_command(cmd) && (!context_menu || in_context_menu(cmd)))
			|| cmd.hotkey_command == hotkey::HOTKEY_NULL)
		{
			items.emplace_back("id", id);
		}
	}

	// No point in showing an empty menu.
	if(items.empty()) {
		return;
	}

	// Based on the ID of the first entry, we fill the menu contextually.
	const std::string& first_id = items.front()["id"];

	// All generated items (might be empty).
	std::vector<config> generated;

	if(first_id == "EDITOR-LOAD-MRU-PLACEHOLDER") {
		active_menu_ = menu_type::load_mru;
		context_manager_->expand_load_mru_menu(generated);
	}

	else if(first_id == "editor-switch-map") {
		active_menu_ = menu_type::map;
		context_manager_->expand_open_maps_menu(generated);
	}

	else if(first_id == "editor-palette-groups") {
		active_menu_ = menu_type::palette;
		toolkit_->get_palette_manager()->active_palette().expand_palette_groups_menu(generated);
	}

	else if(first_id == "editor-switch-side") {
		active_menu_ = menu_type::side;
		context_manager_->expand_sides_menu(generated);
	}

	else if(first_id == "editor-switch-area") {
		active_menu_ = menu_type::area;
		context_manager_->expand_areas_menu(generated);
	}

	else if(first_id == "editor-pbl") {
		active_menu_ = menu_type::addon;
	}

	else if(first_id == "editor-switch-time") {
		active_menu_ = menu_type::time;
		context_manager_->expand_time_menu(generated);
	}

	else if(first_id == "editor-assign-local-time") {
		active_menu_ = menu_type::local_time;
		context_manager_->expand_local_time_menu(generated);
	}

	else if(first_id == "menu-unit-facings") {
		active_menu_ = menu_type::unit_facing;
		auto count = static_cast<int>(map_location::direction::indeterminate);
		std::generate_n(std::back_inserter(generated), count, [dir = 0]() mutable {
			return config{"label", map_location::write_translated_direction(map_location::direction(dir++))};
		});
	}

	else if(first_id == "editor-playlist") {
		active_menu_ = menu_type::music;
		std::transform(music_tracks_.begin(), music_tracks_.end(), std::back_inserter(generated),
			[](const std::shared_ptr<sound::music_track>& track) {
				return config{"label", track->title().empty() ? track->id() : track->title()};
			});
	}

	else if(first_id == "editor-assign-schedule") {
		active_menu_ = menu_type::schedule;
		std::transform(tods_.begin(), tods_.end(), std::back_inserter(generated),
			[](const tods_map::value_type& tod) { return config{"label", tod.second.first}; });
	}

	else if(first_id == "editor-assign-local-schedule") {
		active_menu_ = menu_type::local_schedule;
		std::transform(tods_.begin(), tods_.end(), std::back_inserter(generated),
			[](const tods_map::value_type& tod) { return config{"label", tod.second.first}; });
	}

	else {
		// No placeholders, show everything
		command_executor::show_menu(items, menu_loc, context_menu);
		return;
	}

	// Splice the lists, excluding placeholder entry
	if(items.size() > 1) {
		std::move(items.begin() + 1, items.end(), std::back_inserter(generated));
	}

	command_executor::show_menu(generated, menu_loc, context_menu);
}

void editor_controller::preferences()
{
	gui_->clear_help_string();
	gui2::dialogs::preferences_dialog::display();

	gui_->queue_rerender();
}

void editor_controller::toggle_grid()
{
	prefs::get().set_grid(!prefs::get().grid());
	gui_->invalidate_all();
}

void editor_controller::unit_description()
{
	map_location loc = gui_->mouseover_hex();
	const unit_map& units = get_current_map_context().units();
	const unit_map::const_unit_iterator un = units.find(loc);
	if(un != units.end()) {
		help::show_unit_description(un->type());
	} else {
		help::show_help("..units");
	}
}


void editor_controller::copy_selection()
{
	if (!get_current_map_context().map().selection().empty()) {
		context_manager_->get_clipboard() = map_fragment(get_current_map_context().map(), get_current_map_context().map().selection());
		context_manager_->get_clipboard().center_by_mass();
	}
}

void editor_controller::change_unit_id()
{
	map_location loc = gui_->mouseover_hex();
	unit_map& units = get_current_map_context().units();
	const unit_map::unit_iterator& un = units.find(loc);

	const std::string title(N_("Change Unit ID"));
	const std::string label(N_("ID:"));

	if(un != units.end()) {
		std::string id = un->id();
		if (gui2::dialogs::edit_text::execute(title, label, id)) {
			un->set_id(id);
		}
	}
}

void editor_controller::rename_unit()
{
	map_location loc = gui_->mouseover_hex();
	unit_map& units = get_current_map_context().units();
	const unit_map::unit_iterator& un = units.find(loc);

	const std::string title(N_("Rename Unit"));
	const std::string label(N_("Name:"));

	if(un != units.end()) {
		std::string name = un->name();
		if(gui2::dialogs::edit_text::execute(title, label, name)) {
			//TODO we may not want a translated name here.
			un->set_name(name);
		}
	}
}

void editor_controller::unit_list()
{
	std::vector<unit_const_ptr> unit_list;

	const unit_map& units = gui().context().units();
	for(unit_map::const_iterator i = units.begin(); i != units.end(); ++i) {
		if(i->side() != gui().viewing_team().side()) {
			continue;
		}
		unit_list.push_back(i.get_shared_ptr());
	}

	const auto& unit_dlg = gui2::dialogs::units_dialog::build_unit_list_dialog(unit_list);

	if (unit_dlg->show() && unit_dlg->is_selected()) {
		const map_location& loc = unit_list[unit_dlg->get_selected_index()]->get_location();
		gui().scroll_to_tile(loc, display::WARP);
		gui().select_hex(loc);
	}
}

void editor_controller::cut_selection()
{
	copy_selection();
	context_manager_->perform_refresh(editor_action_paint_area(get_current_map_context().map().selection(), get_selected_bg_terrain()));
}

void editor_controller::save_area()
{
	const std::set<map_location>& area = get_current_map_context().map().selection();
	get_current_map_context().save_area(area);
}

void editor_controller::add_area()
{
	const std::set<map_location>& area = get_current_map_context().map().selection();
	get_current_map_context().new_area(area);
}

void editor_controller::export_selection_coords()
{
	std::stringstream ssx, ssy;
	std::set<map_location>::const_iterator i = get_current_map_context().map().selection().begin();
	if (i != get_current_map_context().map().selection().end()) {
		ssx << "x = " << i->wml_x();
		ssy << "y = " << i->wml_y();
		++i;
		while (i != get_current_map_context().map().selection().end()) {
			ssx << ", " << i->wml_x();
			ssy << ", " << i->wml_y();
			++i;
		}
		ssx << "\n" << ssy.str() << "\n";
		desktop::clipboard::copy_to_clipboard(ssx.str());
	}
}

void editor_controller::perform_delete(std::unique_ptr<editor_action> action)
{
	if (action) {
		get_current_map_context().perform_action(*action);
	}
}

void editor_controller::perform_refresh_delete(std::unique_ptr<editor_action> action, bool drag_part /* =false */)
{
	if (action) {
		context_manager_->perform_refresh(*action, drag_part);
	}
}

void editor_controller::refresh_image_cache()
{
	image::flush_cache();
	context_manager_->refresh_all();
}

void editor_controller::display_redraw_callback(display&)
{
	set_button_state();
	toolkit_->adjust_size();
	get_current_map_context().get_labels().recalculate_labels();
}

void editor_controller::undo()
{
	get_current_map_context().undo();
	context_manager_->refresh_after_action();
}

void editor_controller::redo()
{
	get_current_map_context().redo();
	context_manager_->refresh_after_action();
}

void editor_controller::mouse_motion(int x, int y, const bool /*browse*/,
		bool update, map_location /*new_loc*/)
{
	if (mouse_handler_base::mouse_motion_default(x, y, update)) return;
	map_location hex_clicked = gui().hex_clicked_on(x, y);
	if (get_current_map_context().map().on_board_with_border(drag_from_hex_) && is_dragging()) {
		std::unique_ptr<editor_action> a;
		bool partial = false;
		// last_undo is a non-owning pointer. Although it could have other uses, it seems to be
		// mainly (only?) used for printing debugging information.
		auto last_undo = get_current_map_context().last_undo_action();
		if (dragging_left_ && (sdl::get_mouse_button_mask() & SDL_BUTTON(1)) != 0) {
			if (!get_current_map_context().map().on_board_with_border(hex_clicked)) return;
			a = get_mouse_action().drag_left(*gui_, x, y, partial, last_undo);
		} else if (dragging_right_ && (sdl::get_mouse_button_mask() & SDL_BUTTON(3)) != 0) {
			if (!get_current_map_context().map().on_board_with_border(hex_clicked)) return;
			a = get_mouse_action().drag_right(*gui_, x, y, partial, last_undo);
		}
		//Partial means that the mouse action has modified the
		//last undo action and the controller shouldn't add
		//anything to the undo stack (hence a different perform_ call)
		if (a != nullptr) {
			if (partial) {
				get_current_map_context().perform_partial_action(*a);
			} else {
				get_current_map_context().perform_action(*a);
			}
			context_manager_->refresh_after_action(true);
		}
	} else {
		get_mouse_action().move(*gui_, hex_clicked);
	}
	gui().highlight_hex(hex_clicked);
}

void editor_controller::touch_motion(int /* x */, int /* y */, const bool /* browse */, bool /* update */, map_location /* new_loc */)
{
	// Not implemented at all. Sorry, it's a very low priority for iOS port.
}

bool editor_controller::allow_mouse_wheel_scroll(int x, int y)
{
	return get_current_map_context().map().on_board_with_border(gui().hex_clicked_on(x,y));
}

bool editor_controller::right_click_show_menu(int /*x*/, int /*y*/, const bool /*browse*/)
{
	return get_mouse_action().has_context_menu();
}

bool editor_controller::left_click(int x, int y, const bool browse)
{
	toolkit_->clear_mouseover_overlay();
	if (mouse_handler_base::left_click(x, y, browse))
		return true;

	LOG_ED << "Left click, after generic handling";
	map_location hex_clicked = gui().hex_clicked_on(x, y);
	if (!get_current_map_context().map().on_board_with_border(hex_clicked))
		return true;

	LOG_ED << "Left click action " << hex_clicked;
	auto a = get_mouse_action().click_left(*gui_, x, y);
	if(a) {
		perform_refresh_delete(std::move(a), true);
		set_button_state();
	}

	return false;
}

void editor_controller::left_drag_end(int x, int y, const bool /*browse*/)
{
	auto a = get_mouse_action().drag_end_left(*gui_, x, y);
	perform_delete(std::move(a));
}

void editor_controller::left_mouse_up(int x, int y, const bool /*browse*/)
{
	auto a = get_mouse_action().up_left(*gui_, x, y);
	if(a) {
		perform_delete(std::move(a));
		set_button_state();
	}
	toolkit_->set_mouseover_overlay();
	context_manager_->refresh_after_action();
}

bool editor_controller::right_click(int x, int y, const bool browse)
{
	toolkit_->clear_mouseover_overlay();
	if (mouse_handler_base::right_click(x, y, browse)) return true;
	LOG_ED << "Right click, after generic handling";
	map_location hex_clicked = gui().hex_clicked_on(x, y);
	if (!get_current_map_context().map().on_board_with_border(hex_clicked)) return true;
	LOG_ED << "Right click action " << hex_clicked;
	auto a = get_mouse_action().click_right(*gui_, x, y);
	if(a) {
		perform_refresh_delete(std::move(a), true);
		set_button_state();
	}
	return false;
}

void editor_controller::right_drag_end(int x, int y, const bool /*browse*/)
{
	auto a = get_mouse_action().drag_end_right(*gui_, x, y);
	perform_delete(std::move(a));
}

void editor_controller::right_mouse_up(int x, int y, const bool browse)
{
	// Call base method to handle context menus.
	mouse_handler_base::right_mouse_up(x, y, browse);

	auto a = get_mouse_action().up_right(*gui_, x, y);
	if(a) {
		perform_delete(std::move(a));
		set_button_state();
	}
	toolkit_->set_mouseover_overlay();
	context_manager_->refresh_after_action();
}

void editor_controller::terrain_description()
{
	const map_location& loc = gui().mouseover_hex();
	if (get_current_map_context().map().on_board(loc) == false)
		return;

	const terrain_type& type = get_current_map_context().map().get_terrain_info(loc);
	help::show_terrain_description(type);
}

void editor_controller::process_keyup_event(const SDL_Event& event)
{
	auto a = get_mouse_action().key_event(gui(), event);
	perform_refresh_delete(std::move(a));
	toolkit_->set_mouseover_overlay();
}

hotkey::command_executor * editor_controller::get_hotkey_command_executor() {
	return this;
}

void editor_controller::scroll_up(bool on)
{
	controller_base::set_scroll_up(on);
}

void editor_controller::scroll_down(bool on)
{
	controller_base::set_scroll_down(on);
}

void editor_controller::scroll_left(bool on)
{
	controller_base::set_scroll_left(on);
}

void editor_controller::scroll_right(bool on)
{
	controller_base::set_scroll_right(on);
}

std::vector<std::string> editor_controller::additional_actions_pressed()
{
	return toolkit_->get_palette_manager()->active_palette().action_pressed();
}

} // end namespace editor
