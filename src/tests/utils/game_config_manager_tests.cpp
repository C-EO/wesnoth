/*
	Copyright (C) 2008 - 2025
	by Pauli Nieminen <paniemin@cc.hut.fi>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#define GETTEXT_DOMAIN "wesnoth-test"

#include "tests/utils/game_config_manager_tests.hpp"

#include "config.hpp"
#include "game_config_view.hpp"
#include "config_cache.hpp"
#include "filesystem.hpp"
#include "font/font_config.hpp"
#include "game_config.hpp"
#include "gettext.hpp"
#include "hotkey/hotkey_manager.hpp"
#include "hotkey/hotkey_command.hpp"
#include "hotkey/hotkey_item.hpp"
#include "language.hpp"
#include "units/types.hpp"
#include "utils/general.hpp"

#include "gui/gui.hpp"

#include <clocale>

namespace test_utils {

	class game_config_manager {
		config cfg_;
		game_config_view game_config_view_;
		filesystem::binary_paths_manager paths_manager_;
		const hotkey::manager hotkey_manager_;
		font::manager font_manager_;

		static game_config_manager* manager;
		static void check_manager()
		{
			if(manager)
				return;
			manager = new game_config_manager();
		}
		public:
		game_config_manager()
			: cfg_()
			, game_config_view_(game_config_view::wrap(cfg_))
			, paths_manager_()
			, hotkey_manager_()
			, font_manager_()
		{
#ifdef _WIN32
			setlocale(LC_ALL, "English");
#else
			std::setlocale(LC_ALL, "C");
#endif
			const std::string& intl_dir = filesystem::get_intl_dir();
			translation::bind_textdomain("wesnoth", intl_dir.c_str(), "UTF-8");
			translation::bind_textdomain("wesnoth-lib", intl_dir.c_str(), "UTF-8");
			translation::set_default_textdomain("wesnoth");


			font::load_font_config();
			gui2::init();
			gui2::switch_theme("default");
			load_language_list();
			game_config::config_cache::instance().add_define("TEST");
			game_config::config_cache::instance().get_config(game_config::path + "/data/test/", cfg_);
			::init_textdomains(game_config_view_);
			const std::vector<language_def>& languages = get_languages();
			auto English = utils::ranges::find(languages, "en_US", &language_def::localename);
			::set_language(*English);

			unit_types.set_config(game_config_view_.merged_children_view("units"));

			game_config::load_config(cfg_.mandatory_child("game_config"));
			const hotkey::scope_changer hk_scope{hotkey::scope_game, false};

			hotkey::load_default_hotkeys(game_config_view_);
			paths_manager_.set_paths(game_config_view_);
			font::load_font_config();

		}

		static config& get_config_static()
		{
			check_manager();
			return manager->get_config();
		}

		config& get_config()
		{
			return cfg_;
		}
	};


	game_config_manager* game_config_manager::manager;

	const config& get_test_config_ref()
	{
		return game_config_manager::get_config_static();
	}

	config get_test_config()
	{
		return game_config_manager::get_config_static();
	}

}
