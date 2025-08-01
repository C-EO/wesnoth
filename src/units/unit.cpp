/*
	Copyright (C) 2003 - 2025
	by David White <dave@whitevine.net>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

/**
 *  @file
 *  Routines to manage units.
 */

#include "units/unit.hpp"

#include "ai/manager.hpp"
#include "color.hpp"
#include "deprecation.hpp"
#include "display.hpp"
#include "formatter.hpp"
#include "formula/string_utils.hpp" // for VGETTEXT
#include "game_board.hpp"           // for game_board
#include "game_config.hpp"          // for add_color_info, etc
#include "game_data.hpp"
#include "game_events/manager.hpp" // for add_events
#include "game_version.hpp"
#include "lexical_cast.hpp"
#include "log.hpp"                       // for LOG_STREAM, logger, etc
#include "map/map.hpp"                   // for gamemap
#include "preferences/preferences.hpp"          // for encountered_units
#include "random.hpp"                    // for generator, rng
#include "resources.hpp"                 // for units, gameboard, teams, etc
#include "scripting/game_lua_kernel.hpp" // for game_lua_kernel
#include "synced_context.hpp"
#include "team.hpp"                      // for team, get_teams, etc
#include "units/abilities.hpp"           // for effect, filter_base_matches
#include "units/animation_component.hpp" // for unit_animation_component
#include "units/filter.hpp"
#include "units/id.hpp"
#include "units/map.hpp" // for unit_map, etc
#include "units/types.hpp"
#include "utils/config_filters.hpp"
#include "utils/general.hpp"
#include "variable.hpp" // for vconfig, etc

#include <cassert>                     // for assert
#include <cstdlib>                     // for rand
#include <exception>                    // for exception
#include <iterator>                     // for back_insert_iterator, etc
#include <string_view>
#include <utility>

namespace t_translation { struct terrain_code; }

static lg::log_domain log_unit("unit");
#define DBG_UT LOG_STREAM(debug, log_unit)
#define LOG_UT LOG_STREAM(info, log_unit)
#define WRN_UT LOG_STREAM(warn, log_unit)
#define ERR_UT LOG_STREAM(err, log_unit)

namespace
{
	// "advance" only kept around for backwards compatibility; only "advancement" should be used
	const std::set<std::string_view> ModificationTypes { "advancement", "advance", "trait", "object" };

	/**
	 * Pointers to units which have data in their internal caches. The
	 * destructor of an unit removes itself from the cache, so the pointers are
	 * always valid.
	 */
	static std::vector<const unit*> units_with_cache;

	static const std::string leader_crown_path = "misc/leader-crown.png";
	static const std::set<std::string_view> internalized_attrs {
		"type",
		"id",
		"name",
        "male_name",
        "female_name",
		"gender",
		"random_gender",
		"variation",
		"role",
		"ai_special",
		"side",
		"underlying_id",
		"overlays",
		"facing",
		"race",
		"level",
		"recall_cost",
		"undead_variation",
		"max_attacks",
		"attacks_left",
		"alpha",
		"zoc",
		"flying",
		"cost",
		"max_hitpoints",
		"max_moves",
		"vision",
		"jamming",
		"max_experience",
		"advances_to",
		"hitpoints",
		"goto_x",
		"goto_y",
		"moves",
		"experience",
		"resting",
		"unrenamable",
		"dismissable",
		"block_dismiss_message",
		"alignment",
		"canrecruit",
		"extra_recruit",
		"x",
		"y",
		"placement",
		"parent_type",
		"description",
		"usage",
		"halo",
		"ellipse",
		"upkeep",
		"random_traits",
		"generate_name",
		"profile",
		"small_profile",
		"fire_event",
		"passable",
		"overwrite",
		"location_id",
		"hidden",
		// Useless attributes created when saving units to WML:
		"flag_rgb",
		"language_name",
		"image",
		"image_icon",
		"favorite"
	};

	void warn_unknown_attribute(const config::const_attr_itors& cfg)
	{
		config::const_attribute_iterator cur = cfg.begin();
		config::const_attribute_iterator end = cfg.end();

		auto cur_known = internalized_attrs.begin();
		auto end_known = internalized_attrs.end();

		while(cur_known != end_known) {
			if(cur == end) {
				return;
			}
			int comp = cur->first.compare(*cur_known);
			if(comp < 0) {
				WRN_UT << "Unknown attribute '" << cur->first << "' discarded.";
				++cur;
			}
			else if(comp == 0) {
				++cur;
				++cur_known;
			}
			else {
				++cur_known;
			}
		}

		while(cur != end) {
			WRN_UT << "Unknown attribute '" << cur->first << "' discarded.";
			++cur;
		}
	}

	auto stats_storage_resetter(unit& u, bool clamp = false)
	{
		int hitpoints = u.hitpoints();
		int moves = u.movement_left();
		int attacks = u.attacks_left(true);
		int experience= u.experience();
		bool slowed= u.get_state(unit::STATE_SLOWED);
		bool poisoned= u.get_state(unit::STATE_POISONED);
		return [=, &u] () {
			if(clamp) {
				u.set_movement(std::min(u.total_movement(), moves));
				u.set_hitpoints(std::min(u.max_hitpoints(), hitpoints));
				u.set_attacks(std::min(u.max_attacks(), attacks));
			} else {
				u.set_movement(moves);
				u.set_hitpoints(hitpoints);
				u.set_attacks(attacks);
			}
			u.set_experience(experience);
			u.set_state(unit::STATE_SLOWED, slowed && !u.get_state("unslowable"));
			u.set_state(unit::STATE_POISONED, poisoned && !u.get_state("unpoisonable"));
		};
	}

	map_location::direction get_random_direction()
	{
		constexpr int last_facing = static_cast<int>(map_location::direction::indeterminate) - 1;
		return map_location::direction{randomness::rng::default_instance().get_random_int(0, last_facing)};
	}
} // end anon namespace

/**
 * Converts a string ID to a unit_type.
 * Throws a game_error exception if the string does not correspond to a type.
 */
static const unit_type& get_unit_type(const std::string& type_id)
{
	if(type_id.empty()) {
		throw unit_type::error("creating unit with an empty type field");
	}
	std::string new_id = type_id;
	unit_type::check_id(new_id);
	const unit_type* i = unit_types.find(new_id);
	if(!i) throw unit_type::error("unknown unit type: " + type_id);
	return *i;
}

static unit_race::GENDER generate_gender(const unit_type& type, bool random_gender)
{
	const std::vector<unit_race::GENDER>& genders = type.genders();
	assert(genders.size() > 0);

	if(random_gender == false  ||  genders.size() == 1) {
		return genders.front();
	} else {
		return genders[randomness::generator->get_random_int(0,genders.size()-1)];
	}
}

static unit_race::GENDER generate_gender(const unit_type& u_type, const config& cfg)
{
	const std::string& gender = cfg["gender"];
	if(!gender.empty()) {
		return string_gender(gender);
	}

	return generate_gender(u_type, cfg["random_gender"].to_bool());
}

// Copy constructor
unit::unit(const unit& o)
	: std::enable_shared_from_this<unit>()
	, loc_(o.loc_)
	, advances_to_(o.advances_to_)
	, type_(o.type_)
	, type_name_(o.type_name_)
	, race_(o.race_)
	, id_(o.id_)
	, name_(o.name_)
	, underlying_id_(o.underlying_id_)
	, undead_variation_(o.undead_variation_)
	, variation_(o.variation_)
	, hit_points_(o.hit_points_)
	, max_hit_points_(o.max_hit_points_)
	, experience_(o.experience_)
	, max_experience_(o.max_experience_)
	, level_(o.level_)
	, recall_cost_(o.recall_cost_)
	, canrecruit_(o.canrecruit_)
	, recruit_list_(o.recruit_list_)
	, alignment_(o.alignment_)
	, flag_rgb_(o.flag_rgb_)
	, image_mods_(o.image_mods_)
	, unrenamable_(o.unrenamable_)
	, dismissable_(o.dismissable_)
	, dismiss_message_(o.dismiss_message_)
	, side_(o.side_)
	, gender_(o.gender_)
	, movement_(o.movement_)
	, max_movement_(o.max_movement_)
	, vision_(o.vision_)
	, jamming_(o.jamming_)
	, movement_type_(o.movement_type_)
	, hold_position_(o.hold_position_)
	, end_turn_(o.end_turn_)
	, resting_(o.resting_)
	, attacks_left_(o.attacks_left_)
	, max_attacks_(o.max_attacks_)
	, states_(o.states_)
	, known_boolean_states_(o.known_boolean_states_)
	, variables_(o.variables_)
	, events_(o.events_)
	, filter_recall_(o.filter_recall_)
	, emit_zoc_(o.emit_zoc_)
	, overlays_(o.overlays_)
	, role_(o.role_)
	, attacks_(o.attacks_)
	, facing_(o.facing_)
	, trait_names_(o.trait_names_)
	, trait_descriptions_(o.trait_descriptions_)
	, trait_nonhidden_ids_(o.trait_nonhidden_ids_)
	, unit_value_(o.unit_value_)
	, goto_(o.goto_)
	, interrupted_move_(o.interrupted_move_)
	, is_fearless_(o.is_fearless_)
	, is_healthy_(o.is_healthy_)
	, is_favorite_(o.is_favorite_)
	, modification_descriptions_(o.modification_descriptions_)
	, anim_comp_(new unit_animation_component(*this, *o.anim_comp_))
	, hidden_(o.hidden_)
	, modifications_(o.modifications_)
	, abilities_(o.abilities_)
	, advancements_(o.advancements_)
	, description_(o.description_)
	, special_notes_(o.special_notes_)
	, usage_(o.usage_)
	, halo_(o.halo_)
	, ellipse_(o.ellipse_)
	, random_traits_(o.random_traits_)
	, generate_name_(o.generate_name_)
	, upkeep_(o.upkeep_)
	, profile_(o.profile_)
	, small_profile_(o.small_profile_)
	, changed_attributes_(o.changed_attributes_)
	, invisibility_cache_()
	, has_ability_distant_(o.has_ability_distant_)
	, has_ability_distant_image_(o.has_ability_distant_image_)
{
	affect_distant_ = o.affect_distant_;
	// Copy the attacks rather than just copying references
	for(auto& a : attacks_) {
		a.reset(new attack_type(*a));
	}
}

unit::unit(unit_ctor_t)
	: std::enable_shared_from_this<unit>()
	, loc_()
	, advances_to_()
	, type_(nullptr)
	, type_name_()
	, race_(&unit_race::null_race)
	, id_()
	, name_()
	, underlying_id_(0)
	, undead_variation_()
	, variation_()
	, hit_points_(1)
	, max_hit_points_(1)
	, experience_(0)
	, max_experience_(1)
	, level_(0)
	, recall_cost_(-1)
	, canrecruit_(false)
	, recruit_list_()
	, alignment_()
	, flag_rgb_()
	, image_mods_()
	, unrenamable_(false)
	, dismissable_(true)
	, dismiss_message_(_("This unit cannot be dismissed."))
	, side_(0)
	, gender_(unit_race::NUM_GENDERS)
	, movement_(0)
	, max_movement_(0)
	, vision_(-1)
	, jamming_(0)
	, movement_type_()
	, hold_position_(false)
	, end_turn_(false)
	, resting_(false)
	, attacks_left_(0)
	, max_attacks_(0)
	, states_()
	, known_boolean_states_()
	, variables_()
	, events_()
	, filter_recall_()
	, emit_zoc_(0)
	, overlays_()
	, role_()
	, attacks_()
	, facing_(map_location::direction::indeterminate)
	, trait_names_()
	, trait_descriptions_()
	, trait_nonhidden_ids_()
	, unit_value_()
	, goto_()
	, interrupted_move_()
	, is_fearless_(false)
	, is_healthy_(false)
	, is_favorite_(false)
	, modification_descriptions_()
	, anim_comp_(new unit_animation_component(*this))
	, hidden_(false)
	, modifications_()
	, abilities_()
	, advancements_()
	, description_()
	, special_notes_()
	, usage_()
	, halo_()
	, ellipse_()
	, random_traits_(true)
	, generate_name_(true)
	, upkeep_(upkeep_full{})
	, changed_attributes_(0)
	, invisibility_cache_()
	, has_ability_distant_(utils::nullopt)
	, has_ability_distant_image_(utils::nullopt)
{
	affect_distant_.clear();
}

void unit::set_has_ability_distant()
{
	// check if unit own abilities with [affect_adjacent/distant]
	// else variables are false or erased.
	affect_distant_.clear();
	has_ability_distant_ = utils::nullopt;
	has_ability_distant_image_ = utils::nullopt;
	for(const auto [key, ability] : abilities_.all_children_view()) {
		for (const config &i : ability.child_range("affect_adjacent")) {
			// if 'radius' = "all_map" then radius is to maximum.
			unsigned int radius = i["radius"] != "all_map" ? i["radius"].to_int(1) : INT_MAX;
			if(!affect_distant_[key] || *affect_distant_[key] < radius) {
				affect_distant_[key] = radius;
			}
			if(!has_ability_distant_ || *has_ability_distant_ < radius) {
				has_ability_distant_ =  radius;
			}
			if((!has_ability_distant_image_ || *has_ability_distant_image_ < radius) && (ability.has_attribute("halo_image") || ability.has_attribute("overlay_image"))) {
				has_ability_distant_image_ = radius;
			}
		}
	}
}

void unit::init(const config& cfg, bool use_traits, const vconfig* vcfg)
{
	loc_ = map_location(cfg["x"], cfg["y"], wml_loc());
	type_ = &get_unit_type(cfg["parent_type"].blank() ? cfg["type"].str() : cfg["parent_type"].str());
	race_ = &unit_race::null_race;
	id_ = cfg["id"].str();
	variation_ = cfg["variation"].empty() ? type_->default_variation() : cfg["variation"].str();
	canrecruit_ = cfg["canrecruit"].to_bool();
	gender_ = generate_gender(*type_, cfg);
    name_ = gender_value(cfg, gender_, "male_name", "female_name", "name").t_str();
	role_ = cfg["role"].str();
	//, facing_(map_location::direction::indeterminate)
	//, anim_comp_(new unit_animation_component(*this))
	hidden_ = cfg["hidden"].to_bool();
	random_traits_ = true;
	generate_name_ = true;

	side_ = cfg["side"].to_int();
	if(side_ <= 0) {
		side_ = 1;
	}
	validate_side(side_);

	is_favorite_ = cfg["favorite"].to_bool();

	underlying_id_ = n_unit::unit_id(cfg["underlying_id"].to_size_t());
	set_underlying_id(resources::gameboard ? resources::gameboard->unit_id_manager() : n_unit::id_manager::global_instance());

	if(vcfg) {
		const vconfig& filter_recall = vcfg->child("filter_recall");
		if(!filter_recall.null())
			filter_recall_ = filter_recall.get_config();

		for(const vconfig& e : vcfg->get_children("event")) {
			events_.add_child("event", e.get_config());
		}
		for(const vconfig& abilities_tag : vcfg->get_children("abilities")) {
			for(const auto& [key, child] : abilities_tag.all_ordered()) {
				for(const vconfig& ability_event : child.get_children("event")) {
					events_.add_child("event", ability_event.get_config());
				}
			}
		}
		for(const vconfig& attack : vcfg->get_children("attack")) {
			for(const vconfig& specials_tag : attack.get_children("specials")) {
				for(const auto& [key, child] : specials_tag.all_ordered()) {
					for(const vconfig& special_event : child.get_children("event")) {
						events_.add_child("event", special_event.get_config());
					}
				}
			}
		}
	} else {
		filter_recall_ = cfg.child_or_empty("filter_recall");

		for(const config& unit_event : cfg.child_range("event")) {
			events_.add_child("event", unit_event);
		}
		for(const config& abilities : cfg.child_range("abilities")) {
			for(const auto [key, ability] : abilities.all_children_view()) {
				for(const config& ability_event : ability.child_range("event")) {
					events_.add_child("event", ability_event);
				}
			}
		}
		for(const config& attack : cfg.child_range("attack")) {
			for(const config& specials : attack.child_range("specials")) {
				for(const auto [key, special] : specials.all_children_view()) {
					for(const config& special_event : special.child_range("event")) {
						events_.add_child("event", special_event);
					}
				}
			}
		}
	}

	if(resources::game_events && resources::lua_kernel) {
		resources::game_events->add_events(events_.child_range("event"), *resources::lua_kernel);
	}

	random_traits_ = cfg["random_traits"].to_bool(true);
	facing_ = map_location::parse_direction(cfg["facing"]);
	if(facing_ == map_location::direction::indeterminate) facing_ = get_random_direction();

	for(const config& mods : cfg.child_range("modifications")) {
		modifications_.append_children(mods);
	}

	generate_name_ = cfg["generate_name"].to_bool(true);

	// Apply the unit type's data to this unit.
	advance_to(*type_, use_traits);

	if(const config::attribute_value* v = cfg.get("overlays")) {
		auto overlays = utils::parenthetical_split(v->str(), ',');
		if(overlays.size() > 0) {
			deprecated_message("[unit]overlays", DEP_LEVEL::PREEMPTIVE, {1, 17, 0}, "This warning is only triggered by the cases that *do* still work: setting [unit]overlays= works, but the [unit]overlays attribute will always be empty if WML tries to read it.");
			config effect;
			config o;
			effect["apply_to"] = "overlay";
			effect["add"] = v->str();
			o.add_child("effect", effect);
			add_modification("object", o);
		}
	}

	if(auto variables = cfg.optional_child("variables")) {
		variables_ = *variables;
	}

	if(const config::attribute_value* v = cfg.get("race")) {
		if(const unit_race *r = unit_types.find_race(*v)) {
			race_ = r;
		} else {
			race_ = &unit_race::null_race;
		}
	}

	if(const config::attribute_value* v = cfg.get("level")) {
		set_level(v->to_int(level_));
	}

	if(const config::attribute_value* v = cfg.get("undead_variation")) {
		set_undead_variation(v->str());
	}

	if(const config::attribute_value* v = cfg.get("max_attacks")) {
		set_max_attacks(std::max(0, v->to_int(1)));
	}

	if(const config::attribute_value* v = cfg.get("zoc")) {
		set_emit_zoc(v->to_bool(level_ > 0));
	}

	if(const config::attribute_value* v = cfg.get("description")) {
		description_ = *v;
	}

	if(const config::attribute_value* v = cfg.get("cost")) {
		unit_value_ = v->to_int();
	}

	if(const config::attribute_value* v = cfg.get("ellipse")) {
		set_image_ellipse(*v);
	}

	if(const config::attribute_value* v = cfg.get("halo")) {
		set_image_halo(*v);
	}

	if(const config::attribute_value* v = cfg.get("usage")) {
		set_usage(*v);
	}

	if(const config::attribute_value* v = cfg.get("profile")) {
		set_big_profile(v->str());
	}

	if(const config::attribute_value* v = cfg.get("small_profile")) {
		set_small_profile(v->str());
	}

	if(const config::attribute_value* v = cfg.get("max_hitpoints")) {
		set_max_hitpoints(std::max(1, v->to_int(max_hit_points_)));
	}
	if(const config::attribute_value* v = cfg.get("max_moves")) {
		set_total_movement(std::max(0, v->to_int(max_movement_)));
	}
	if(const config::attribute_value* v = cfg.get("max_experience")) {
		set_max_experience(std::max(1, v->to_int(max_experience_)));
	}

	vision_ = cfg["vision"].to_int(vision_);
	jamming_ = cfg["jamming"].to_int(jamming_);

	advances_to_t temp_advances = utils::split(cfg["advances_to"]);
	if(temp_advances.size() == 1 && temp_advances.front() == "null") {
		set_advances_to(advances_to_t());
	} else if(temp_advances.size() >= 1 && !temp_advances.front().empty()) {
		set_advances_to(temp_advances);
	}

	if(auto ai = cfg.optional_child("ai")) {
		config ai_events;
		for(config mai : ai->child_range("micro_ai")) {
			mai.clear_children("filter");
			mai.add_child("filter")["id"] = id();
			mai["side"] = side();
			mai["action"] = "add";
			ai_events.add_child("micro_ai", mai);
		}
		for(config ca : ai->child_range("candidate_action")) {
			ca.clear_children("filter_own");
			ca.add_child("filter_own")["id"] = id();
			// Sticky candidate actions not supported here (they cause a crash because the unit isn't on the map yet)
			ca.remove_attribute("sticky");
			std::string stage = "main_loop";
			if(ca.has_attribute("stage")) {
				stage = ca["stage"].str();
				ca.remove_attribute("stage");
			}
			config mod{
				"action", "add",
				"side", side(),
				"path", "stage[" + stage + "].candidate_action[]",
				"candidate_action", ca,
			};
			ai_events.add_child("modify_ai", mod);
		}
		if(ai_events.all_children_count() > 0) {
			ai::manager::get_singleton().append_active_ai_for_side(side(), ai_events);
		}
	}

	// Don't use the unit_type's attacks if this config has its own defined
	if(config::const_child_itors cfg_range = cfg.child_range("attack")) {
		set_attr_changed(UA_ATTACKS);
		attacks_.clear();
		for(const config& c : cfg_range) {
			attacks_.emplace_back(new attack_type(c));
		}
	}

	// Don't use the unit_type's special notes if this config has its own defined
	if(config::const_child_itors cfg_range = cfg.child_range("special_note")) {
		set_attr_changed(UA_NOTES);
		special_notes_.clear();
		for(const config& c : cfg_range) {
			special_notes_.emplace_back(c["note"].t_str());
		}
	}

	// If cfg specifies [advancement]s, replace this [advancement]s with them.
	if(cfg.has_child("advancement")) {
		set_attr_changed(UA_ADVANCEMENTS);
		advancements_.clear();
		for(const config& adv : cfg.child_range("advancement")) {
			advancements_.push_back(adv);
		}
	}

	// Don't use the unit_type's abilities if this config has its own defined
	// Why do we allow multiple [abilities] tags?
	if(config::const_child_itors cfg_range = cfg.child_range("abilities")) {
		set_attr_changed(UA_ABILITIES);
		abilities_.clear();
		for(const config& abilities : cfg_range) {
			abilities_.append(abilities);
		}
	}

	set_has_ability_distant();

	if(const config::attribute_value* v = cfg.get("alignment")) {
		set_attr_changed(UA_ALIGNMENT);
		auto new_align = unit_alignments::get_enum(v->str());
		if(new_align) {
			alignment_ = *new_align;
		}
	}

	// Adjust the unit's defense, movement, vision, jamming, resistances, and
	// flying status if this config has its own defined.
	if(cfg.has_child("movement_costs")
	|| cfg.has_child("vision_costs")
	|| cfg.has_child("jamming_costs")
	|| cfg.has_child("defense")
	|| cfg.has_child("resistance")
	|| cfg.has_attribute("flying"))
	{
		set_attr_changed(UA_MOVEMENT_TYPE);
	}

	movement_type_.merge(cfg);

	if(auto status_flags = cfg.optional_child("status")) {
		for(const auto& [key, value] : status_flags->attribute_range()) {
			if(value.to_bool()) {
				set_state(key, true);
			}
		}
	}

	if(cfg["ai_special"] == "guardian") {
		set_state(STATE_GUARDIAN, true);
	}

	if(const config::attribute_value* v = cfg.get("invulnerable")) {
		set_state(STATE_INVULNERABLE, v->to_bool());
	}

	goto_.set_wml_x(cfg["goto_x"].to_int());
	goto_.set_wml_y(cfg["goto_y"].to_int());

	attacks_left_ = std::max(0, cfg["attacks_left"].to_int(max_attacks_));

	movement_ = std::max(0, cfg["moves"].to_int(max_movement_));
	// we allow negative hitpoints, one of the reasons is that a unit
	// might be stored+unstored during a attack related event before it
	// dies when it has negative hp and when dont want the event to
	// change the unit hp when it was not intended.
	hit_points_ = cfg["hitpoints"].to_int(max_hit_points_);

	experience_ = cfg["experience"].to_int();
	resting_ = cfg["resting"].to_bool();
	unrenamable_ = cfg["unrenamable"].to_bool();

	// leader units can't be dismissed by default
	dismissable_ = cfg["dismissable"].to_bool(!canrecruit_);
	if(canrecruit_) {
		dismiss_message_ = _ ("This unit is a leader and cannot be dismissed.");
	}
	if(!cfg["block_dismiss_message"].blank()) {
		dismiss_message_ = cfg["block_dismiss_message"].t_str();
	}

	// We need to check to make sure that the cfg is not blank and if it
	// isn't pull that value otherwise it goes with the default of -1.
	if(!cfg["recall_cost"].blank()) {
		recall_cost_ = cfg["recall_cost"].to_int(recall_cost_);
	}

	generate_name();

	parse_upkeep(cfg["upkeep"]);

	set_recruits(utils::split(cfg["extra_recruit"]));

	warn_unknown_attribute(cfg.attribute_range());

#if 0
	// Debug unit animations for units as they appear in game
	for(const auto& anim : anim_comp_->animations_) {
		std::cout << anim.debug() << std::endl;
	}
#endif
}

void unit::clear_status_caches()
{
	for(auto& u : units_with_cache) {
		u->clear_visibility_cache();
	}

	units_with_cache.clear();
}

void unit::init(const unit_type& u_type, int side, bool real_unit, unit_race::GENDER gender, const std::string& variation)
{
	type_ = &u_type;
	race_ = &unit_race::null_race;
	variation_ = variation.empty() ? type_->default_variation() : variation;
	side_ = side;
	gender_ = gender != unit_race::NUM_GENDERS ? gender : generate_gender(u_type, real_unit);
	facing_ = get_random_direction();
	upkeep_ = upkeep_full{};

	// Apply the unit type's data to this unit.
	advance_to(u_type, real_unit);

	if(real_unit) {
		generate_name();
	}

	set_underlying_id(resources::gameboard ? resources::gameboard->unit_id_manager() : n_unit::id_manager::global_instance());

	// Set these after traits and modifications have set the maximums.
	movement_ = max_movement_;
	hit_points_ = max_hit_points_;
	attacks_left_ = max_attacks_;
}

unit::~unit()
{
	try {
		anim_comp_->clear_haloes();

		// Remove us from the status cache
		auto itor = std::find(units_with_cache.begin(), units_with_cache.end(), this);

		if(itor != units_with_cache.end()) {
			units_with_cache.erase(itor);
		}
	} catch(const std::exception & e) {
		ERR_UT << "Caught exception when destroying unit: " << e.what();
	} catch(...) {
		DBG_UT << "Caught general exception when destroying unit: " << utils::get_unknown_exception_type();
	}
}

void unit::generate_name()
{
	if(!name_.empty() || !generate_name_) {
		return;
	}
	name_ = race_->generate_name(gender_);
	generate_name_ = false;
}

void unit::generate_traits(bool must_have_only)
{
	LOG_UT << "Generating a trait for unit type " << type().log_id() << " with must_have_only " << must_have_only;
	const unit_type& u_type = type();

	config::const_child_itors current_traits = modifications_.child_range("trait");

	// Handle must-have only at the beginning
	for(const config& t : u_type.possible_traits()) {
		// Skip the trait if the unit already has it.
		const std::string& tid = t["id"];
		bool already = false;
		for(const config& mod : current_traits) {
			if(mod["id"] == tid) {
				already = true;
				break;
			}
		}
		if(already) {
			continue;
		}
		// Add the trait if it is mandatory.
		const std::string& avl = t["availability"];
		if(avl == "musthave") {
			modifications_.add_child("trait", t);
			current_traits = modifications_.child_range("trait");
			continue;
		}
	}

	if(must_have_only) {
		return;
	}

	std::vector<const config*> candidate_traits;
	std::vector<std::string> temp_require_traits;
	std::vector<std::string> temp_exclude_traits;

	// Now randomly fill out to the number of traits required or until
	// there aren't any more traits.
	int nb_traits = current_traits.size();
	int max_traits = u_type.num_traits();
	for(; nb_traits < max_traits; ++nb_traits)
	{
		current_traits = modifications_.child_range("trait");
		candidate_traits.clear();
		for(const config& t : u_type.possible_traits()) {
			// Skip the trait if the unit already has it.
			const std::string& tid = t["id"];
			bool already = false;
			for(const config& mod : current_traits) {
				if(mod["id"] == tid) {
					already = true;
					break;
				}
			}

			if(already) {
				continue;
			}
			// Skip trait if trait requirements are not met
			// or trait exclusions are present
			temp_require_traits = utils::split(t["require_traits"]);
			temp_exclude_traits = utils::split(t["exclude_traits"]);

			// See if the unit already has a trait that excludes the current one
			for(const config& mod : current_traits) {
				if (mod["exclude_traits"] != "") {
					for (const auto& c: utils::split(mod["exclude_traits"])) {
						temp_exclude_traits.push_back(c);
					}
				}
			}

			// First check for requirements
			bool trait_req_met = true;
			for(const std::string& s : temp_require_traits) {
				bool has_trait = false;
				for(const config& mod : current_traits) {
					if (mod["id"] == s)
						has_trait = true;
				}
				if(!has_trait) {
					trait_req_met = false;
					break;
				}
			}
			if(!trait_req_met) {
				continue;
			}

			// Now check for exclusionary traits
			bool trait_exc_met = true;

			for(const std::string& s : temp_exclude_traits) {
				bool has_exclusionary_trait = false;
				for(const config& mod : current_traits) {
					if (mod["id"] == s)
						has_exclusionary_trait = true;
				}
				if (tid == s) {
					has_exclusionary_trait = true;
				}
				if(has_exclusionary_trait) {
					trait_exc_met = false;
					break;
				}
			}
			if(!trait_exc_met) {
				continue;
			}

			const std::string& avl = t["availability"];
			// The trait is still available, mark it as a candidate for randomizing.
			// For leaders, only traits with availability "any" are considered.
			if(!must_have_only && (!can_recruit() || avl == "any")) {
				candidate_traits.push_back(&t);
			}
		}
		// No traits available anymore? Break
		if(candidate_traits.empty()) {
			break;
		}

		int num = randomness::generator->get_random_int(0,candidate_traits.size()-1);
		modifications_.add_child("trait", *candidate_traits[num]);
		candidate_traits.erase(candidate_traits.begin() + num);
	}
	// Once random traits are added, don't do it again.
	// Such as when restoring a saved character.
	random_traits_ = false;
}

std::vector<std::string> unit::get_modifications_list(const std::string& mod_type) const
{
	std::vector<std::string> res;

	for(const config& mod : modifications_.child_range(mod_type)){
		// Make sure to return empty id trait strings as otherwise
		// names will not match in length (Bug #21967)
		res.push_back(mod["id"]);
	}
	if(mod_type == "advancement"){
		for(const config& mod : modifications_.child_range("advance")){
			res.push_back(mod["id"]);
		}
	}
	return res;
}

std::size_t unit::modification_count(const std::string& type) const
{
	//return numbers of modifications of same type, same without ID.
	std::size_t res = modifications_.child_range(type).size();
	if(type == "advancement"){
		res += modification_count("advance");
	}

	return res;
}


/**
 * Advances this unit to the specified type.
 * Experience is left unchanged.
 * Current hitpoints/movement/attacks_left is left unchanged unless it would violate their maximum.
 * Assumes gender_ and variation_ are set to their correct values.
 */
void unit::advance_to(const unit_type& u_type, bool use_traits)
{
	auto ss = stats_storage_resetter(*this, true);
	appearance_changed_ = true;
	// For reference, the type before this advancement.
	const unit_type& old_type = type();
	// Adjust the new type for gender and variation.
	const unit_type& new_type = u_type.get_gender_unit_type(gender_).get_variation(variation_);
	// In case u_type was already a variation, make sure our variation is set correctly.
	variation_ = new_type.variation_id();

	// Reset the scalar values first
	trait_names_.clear();
	trait_descriptions_.clear();
	trait_nonhidden_ids_.clear();
	is_fearless_ = false;
	is_healthy_ = false;
	image_mods_.clear();
	overlays_.clear();
	ellipse_.reset();

	// Clear modification-related caches
	modification_descriptions_.clear();


	if(!new_type.usage().empty()) {
		 set_usage(new_type.usage());
	}

	set_image_halo(new_type.halo());
	if(!new_type.ellipse().empty()) {
		set_image_ellipse(new_type.ellipse());
	}

	generate_name_ &= new_type.generate_name();
	abilities_ = new_type.abilities_cfg();
	advancements_.clear();

	for(const config& advancement : new_type.advancements()) {
		advancements_.push_back(advancement);
	}

	// If unit has specific profile, remember it and keep it after advancing
	if(small_profile_.empty() || small_profile_ == old_type.small_profile()) {
		small_profile_ = new_type.small_profile();
	}

	if(profile_.empty() || profile_ == old_type.big_profile()) {
		profile_ = new_type.big_profile();
	}
	// NOTE: There should be no need to access old_cfg (or new_cfg) after this
	//       line. Particularly since the swap might have affected old_cfg.

	advances_to_ = new_type.advances_to();

	race_ = new_type.race();
	type_ = &new_type;
	type_name_ = new_type.type_name();
	description_ = new_type.unit_description();
	special_notes_ = new_type.direct_special_notes();
	undead_variation_ = new_type.undead_variation();
	max_experience_ = new_type.experience_needed(true);
	level_ = new_type.level();
	recall_cost_ = new_type.recall_cost();
	alignment_ = new_type.alignment();
	max_hit_points_ = new_type.hitpoints();
	max_movement_ = new_type.movement();
	vision_ = new_type.vision(true);
	jamming_ = new_type.jamming();
	movement_type_ = new_type.movement_type();
	emit_zoc_ = new_type.has_zoc();
	attacks_.clear();
	std::transform(new_type.attacks().begin(), new_type.attacks().end(), std::back_inserter(attacks_), [](const attack_type& atk) {
		return std::make_shared<attack_type>(atk);
	});
	unit_value_ = new_type.cost();

	max_attacks_ = new_type.max_attacks();

	flag_rgb_ = new_type.flag_rgb();

	upkeep_ = upkeep_full{};
	parse_upkeep(new_type.get_cfg()["upkeep"]);

	anim_comp_->reset_after_advance(&new_type);

	if(random_traits_) {
		generate_traits(!use_traits);
	} else {
		// This will add any "musthave" traits to the new unit that it doesn't already have.
		// This covers the Dark Sorcerer advancing to Lich and gaining the "undead" trait,
		// but random and/or optional traits are not added,
		// and neither are inappropriate traits removed.
		generate_traits(true);
	}

	// Apply modifications etc, refresh the unit.
	// This needs to be after type and gender are fixed,
	// since there can be filters on the modifications
	// that may result in different effects after the advancement.
	apply_modifications();

	// Now that modifications are done modifying traits, check if poison should
	// be cleared.
	// Make sure apply_modifications() didn't attempt to heal the unit (for example if the unit has a default amla.).
	ss();
	if(get_state("unpetrifiable")) {
		set_state(STATE_PETRIFIED, false);
	}

	// In case the unit carries EventWML, apply it now
	if(resources::game_events && resources::lua_kernel) {
		config events;
		const config& cfg = new_type.get_cfg();
		for(const config& unit_event : cfg.child_range("event")) {
			events.add_child("event", unit_event);
		}
		for(const config& abilities : cfg.child_range("abilities")) {
			for(const auto [key, ability] : abilities.all_children_view()) {
				for(const config& ability_event : ability.child_range("event")) {
					events.add_child("event", ability_event);
				}
			}
		}
		for(const config& attack : cfg.child_range("attack")) {
			for(const config& specials : attack.child_range("specials")) {
				for(const auto [key, special] : specials.all_children_view()) {
					for(const config& special_event : special.child_range("event")) {
						events.add_child("event", special_event);
					}
				}
			}
		}
		resources::game_events->add_events(events.child_range("event"), *resources::lua_kernel, new_type.id());
	}
	bool bool_small_profile = get_attr_changed(UA_SMALL_PROFILE);
	bool bool_profile = get_attr_changed(UA_PROFILE);
	clear_changed_attributes();
	if(bool_small_profile && small_profile_ != new_type.small_profile()) {
		set_attr_changed(UA_SMALL_PROFILE);
	}

	if(bool_profile && profile_ != new_type.big_profile()) {
		set_attr_changed(UA_PROFILE);
	}
	set_has_ability_distant();
}

std::string unit::big_profile() const
{
	if(!profile_.empty() && profile_ != "unit_image") {
		return profile_;
	}

	return absolute_image();
}

std::string unit::small_profile() const
{
	if(!small_profile_.empty() && small_profile_ != "unit_image") {
		return small_profile_;
	}

	if(!profile_.empty() && small_profile_ != "unit_image" && profile_ != "unit_image") {
		return profile_;
	}

	return absolute_image();
}

const std::string& unit::leader_crown()
{
	return leader_crown_path;
}

const std::string& unit::flag_rgb() const
{
	return flag_rgb_.empty() ? game_config::unit_rgb : flag_rgb_;
}

static color_t hp_color_impl(int hitpoints, int max_hitpoints)
{
	const double unit_energy = max_hitpoints > 0
		? static_cast<double>(hitpoints) / max_hitpoints
		: 0.0;

	if(1.0 == unit_energy) {
		return {33, 225, 0};
	} else if(unit_energy > 1.0) {
		return {100, 255, 100};
	} else if(unit_energy >= 0.75) {
		return {170, 255, 0};
	} else if(unit_energy >= 0.5) {
		return {255, 175, 0};
	} else if(unit_energy >= 0.25) {
		return {255, 155, 0};
	} else {
		return {255, 0, 0};
	}
}

color_t unit::hp_color() const
{
	return hp_color_impl(hitpoints(), max_hitpoints());
}

color_t unit::hp_color(int new_hitpoints) const
{
	return hp_color_impl(new_hitpoints, hitpoints());
}

color_t unit::hp_color_max()
{
	return hp_color_impl(1, 1);
}

color_t unit::xp_color(int xp_to_advance, bool can_advance, bool has_amla)
{
	const bool near_advance = xp_to_advance <= game_config::kill_experience;
	const bool mid_advance  = xp_to_advance <= game_config::kill_experience * 2;
	const bool far_advance  = xp_to_advance <= game_config::kill_experience * 3;

	if(can_advance) {
		if(near_advance) {
			return {255, 255, 255};
		} else if(mid_advance) {
			return {150, 255, 255};
		} else if(far_advance) {
			return {0, 205, 205};
		}
	} else if(has_amla) {
		if(near_advance) {
			return {225, 0, 255};
		} else if(mid_advance) {
			return {169, 30, 255};
		} else if(far_advance) {
			return {139, 0, 237};
		} else {
			return {170, 0, 255};
		}
	}

	return {0, 160, 225};
}

color_t unit::xp_color() const
{
	bool major_amla = false;
	bool has_amla = false;
	for(const config& adv:get_modification_advances()){
		major_amla |= adv["major_amla"].to_bool();
		has_amla = true;
	}
	//TODO: calculating has_amla and major_amla can be a quite slow operation, we should cache these two values somehow.
	return xp_color(experience_to_advance(), !advances_to().empty() || major_amla, has_amla);
}

void unit::set_recruits(const std::vector<std::string>& recruits)
{
	unit_types.check_types(recruits);
	recruit_list_ = recruits;
}

const std::vector<std::string> unit::advances_to_translated() const
{
	std::vector<std::string> result;
	for(const std::string& adv_type_id : advances_to_) {
		if(const unit_type* adv_type = unit_types.find(adv_type_id)) {
			result.push_back(adv_type->type_name());
		} else {
			WRN_UT << "unknown unit in advances_to list of type "
			<< type().log_id() << ": " << adv_type_id;
		}
	}

	return result;
}

void unit::set_advances_to(const std::vector<std::string>& advances_to)
{
	set_attr_changed(UA_ADVANCE_TO);
	unit_types.check_types(advances_to);
	advances_to_ = advances_to;
}

void unit::set_movement(int moves, bool unit_action)
{
	// If this was because the unit acted, clear its "not acting" flags.
	if(unit_action) {
		end_turn_ = hold_position_ = false;
	}

	movement_ = std::max<int>(0, moves);
}

/**
 * Determines if @a mod_dur "matches" @a goal_dur.
 * If goal_dur is not empty, they match if they are equal.
 * If goal_dur is empty, they match if mod_dur is neither empty nor "forever".
 * Helper function for expire_modifications().
 */
inline bool mod_duration_match(const std::string& mod_dur, const std::string& goal_dur)
{
	if(goal_dur.empty()) {
		// Default is all temporary modifications.
		return !mod_dur.empty() && mod_dur != "forever";
	}

	return mod_dur == goal_dur;
}

void unit::expire_modifications(const std::string& duration)
{
	// If any modifications expire, then we will need to rebuild the unit.
	const unit_type* rebuild_from = nullptr;
	// Loop through all types of modifications.
	for(const auto& mod_name : ModificationTypes) {
		// Loop through all modifications of this type.
		// Looping in reverse since we may delete the current modification.
		for(int j = modifications_.child_count(mod_name)-1; j >= 0; --j)
		{
			const config& mod = modifications_.mandatory_child(mod_name, j);

			if(mod_duration_match(mod["duration"], duration)) {
				// If removing this mod means reverting the unit's type:
				if(const config::attribute_value* v = mod.get("prev_type")) {
					rebuild_from = &get_unit_type(v->str());
				}
				// Else, if we have not already specified a type to build from:
				else if(rebuild_from == nullptr) {
					rebuild_from = &type();
				}

				modifications_.remove_child(mod_name, j);
			}
		}
	}

	if(rebuild_from != nullptr) {
		anim_comp_->clear_haloes();
		advance_to(*rebuild_from);
	}
}

void unit::new_turn()
{
	expire_modifications("turn");

	end_turn_ = hold_position_;
	movement_ = total_movement();
	attacks_left_ = max_attacks_;
	set_state(STATE_UNCOVERED, false);
}

void unit::end_turn()
{
	expire_modifications("turn end");

	set_state(STATE_SLOWED,false);
	if((movement_ != total_movement()) && !(get_state(STATE_NOT_MOVED))) {
		resting_ = false;
	}

	set_state(STATE_NOT_MOVED,false);
	// Clear interrupted move
	set_interrupted_move(map_location());
}

void unit::new_scenario()
{
	// Set the goto-command to be going to no-where
	goto_ = map_location();

	// Expire all temporary modifications.
	expire_modifications("");

	heal_fully();
	set_state(STATE_SLOWED, false);
	set_state(STATE_POISONED, false);
	set_state(STATE_PETRIFIED, false);
	set_state(STATE_GUARDIAN, false);
}

void unit::heal(int amount)
{
	int max_hp = max_hitpoints();
	if(hit_points_ < max_hp) {
		hit_points_ += amount;

		if(hit_points_ > max_hp) {
			hit_points_ = max_hp;
		}
	}

	if(hit_points_<1) {
		hit_points_ = 1;
	}
}

const std::set<std::string> unit::get_states() const
{
	std::set<std::string> all_states = states_;
	for(const auto& state : known_boolean_state_names_) {
		if(get_state(state.second)) {
			all_states.insert(state.first);
		}
	}

	// Backwards compatibility for not_living. Don't remove before 1.12
	if(all_states.count("undrainable") && all_states.count("unpoisonable") && all_states.count("unplagueable")) {
		all_states.insert("not_living");
	}

	return all_states;
}

bool unit::get_state(const std::string& state) const
{
	state_t known_boolean_state_id = get_known_boolean_state_id(state);
	if(known_boolean_state_id!=STATE_UNKNOWN){
		return get_state(known_boolean_state_id);
	}

	// Backwards compatibility for not_living. Don't remove before 1.12
	if(state == "not_living") {
		return
			get_state("undrainable")  &&
			get_state("unpoisonable") &&
			get_state("unplagueable");
	}

	return states_.find(state) != states_.end();
}

void unit::set_state(state_t state, bool value)
{
	known_boolean_states_[state] = value;
}

bool unit::get_state(state_t state) const
{
	return known_boolean_states_[state];
}

unit::state_t unit::get_known_boolean_state_id(const std::string& state)
{
	auto i = known_boolean_state_names_.find(state);
	if(i != known_boolean_state_names_.end()) {
		return i->second;
	}

	return STATE_UNKNOWN;
}

std::string unit::get_known_boolean_state_name(state_t state)
{
	for(const auto& p : known_boolean_state_names_) {
		if(p.second == state) {
			return p.first;
		}
	}
	return "";
}

std::map<std::string, unit::state_t> unit::known_boolean_state_names_ {
	{"slowed",       STATE_SLOWED},
	{"poisoned",     STATE_POISONED},
	{"petrified",    STATE_PETRIFIED},
	{"uncovered",    STATE_UNCOVERED},
	{"not_moved",    STATE_NOT_MOVED},
	{"unhealable",   STATE_UNHEALABLE},
	{"guardian",     STATE_GUARDIAN},
	{"invulnerable", STATE_INVULNERABLE},
};

void unit::set_state(const std::string& state, bool value)
{
	appearance_changed_ = true;
	state_t known_boolean_state_id = get_known_boolean_state_id(state);
	if(known_boolean_state_id != STATE_UNKNOWN) {
		set_state(known_boolean_state_id, value);
		return;
	}

	// Backwards compatibility for not_living. Don't remove before 1.12
	if(state == "not_living") {
		set_state("undrainable", value);
		set_state("unpoisonable", value);
		set_state("unplagueable", value);
	}

	if(value) {
		states_.insert(state);
	} else {
		states_.erase(state);
	}
}

bool unit::has_ability_by_id(const std::string& ability) const
{
	for(const auto [key, cfg] : abilities_.all_children_view()) {
		if(cfg["id"] == ability) {
			return true;
		}
	}

	return false;
}

void unit::remove_ability_by_id(const std::string& ability)
{
	set_attr_changed(UA_ABILITIES);
	config::all_children_iterator i = abilities_.ordered_begin();
	while (i != abilities_.ordered_end()) {
		if(i->cfg["id"] == ability) {
			i = abilities_.erase(i);
		} else {
			++i;
		}
	}
}

void unit::remove_ability_by_attribute(const config& filter)
{
	set_attr_changed(UA_ABILITIES);
	config::all_children_iterator i = abilities_.ordered_begin();
	while (i != abilities_.ordered_end()) {
		if(ability_matches_filter(i->cfg, i->key, filter)) {
			i = abilities_.erase(i);
		} else {
			++i;
		}
	}
}

bool unit::get_attacks_changed() const
{
	for(const auto& a_ptr : attacks_) {
		if(a_ptr->get_changed()) {
			return true;
		}

	}
	return false;
}

void unit::write(config& cfg, bool write_all) const
{
	config back;
	auto write_subtag = [&](const std::string& key, const config& child)
	{
		cfg.clear_children(key);

		if(!child.empty()) {
			cfg.add_child(key, child);
		} else {
			back.add_child(key, child);
		}
	};

	if(write_all || get_attr_changed(UA_MOVEMENT_TYPE)) {
		movement_type_.write(cfg, false);
	}
	if(write_all || get_attr_changed(UA_SMALL_PROFILE)) {
		cfg["small_profile"] = small_profile_;
	}
	if(write_all || get_attr_changed(UA_PROFILE)) {
		cfg["profile"] = profile_;
	}
	if(description_ != type().unit_description()) {
		cfg["description"] = description_;
	}
	if(write_all || get_attr_changed(UA_NOTES)) {
		for(const t_string& note : special_notes_) {
			cfg.add_child("special_note")["note"] = note;
		}
	}

	if(halo_) {
		cfg["halo"] = *halo_;
	}

	if(ellipse_) {
		cfg["ellipse"] = *ellipse_;
	}

	if(usage_) {
		cfg["usage"] = *usage_;
	}

	write_upkeep(cfg["upkeep"]);

	cfg["hitpoints"] = hit_points_;
	if(write_all || get_attr_changed(UA_MAX_HP)) {
		cfg["max_hitpoints"] = max_hit_points_;
	}
	cfg["image_icon"] = type().icon();
	cfg["image"] = type().image();
	cfg["random_traits"] = random_traits_;
	cfg["generate_name"] = generate_name_;
	cfg["experience"] = experience_;
	if(write_all || get_attr_changed(UA_MAX_XP)) {
		cfg["max_experience"] = max_experience_;
	}
	cfg["recall_cost"] = recall_cost_;

	cfg["side"] = side_;

	cfg["type"] = type_id();

	if(type_id() != type().parent_id()) {
		cfg["parent_type"] = type().parent_id();
	}

	cfg["gender"] = gender_string(gender_);
	cfg["variation"] = variation_;
	cfg["role"] = role_;

	cfg["favorite"] = is_favorite_;

	config status_flags;
	for(const std::string& state : get_states()) {
		status_flags[state] = true;
	}

	write_subtag("variables", variables_);
	write_subtag("filter_recall", filter_recall_);
	write_subtag("status", status_flags);

	cfg.clear_children("events");
	cfg.append(events_);

	// Overlays are exported as the modifications that add them, not as an overlays= value,
	// however removing the key breaks the Gui Debug Tools.
	// \todo does anything depend on the key's value, other than the re-import code in unit::init?
	cfg["overlays"] = "";

	cfg["name"] = name_;
	cfg["id"] = id_;
	cfg["underlying_id"] = underlying_id_.value;

	if(can_recruit()) {
		cfg["canrecruit"] = true;
	}

	cfg["extra_recruit"] = utils::join(recruit_list_);

	cfg["facing"] = map_location::write_direction(facing_);

	cfg["goto_x"] = goto_.wml_x();
	cfg["goto_y"] = goto_.wml_y();

	cfg["moves"] = movement_;
	if(write_all || get_attr_changed(UA_MAX_MP)) {
		cfg["max_moves"] = max_movement_;
	}
	cfg["vision"] = vision_;
	cfg["jamming"] = jamming_;

	cfg["resting"] = resting_;

	if(write_all || get_attr_changed(UA_ADVANCE_TO)) {
		cfg["advances_to"] = utils::join(advances_to_);
	}

	cfg["race"] = race_->id();
	cfg["language_name"] = type_name_;
	cfg["undead_variation"] = undead_variation_;
	if(write_all || get_attr_changed(UA_LEVEL)) {
		cfg["level"] = level_;
	}
	if(write_all || get_attr_changed(UA_ALIGNMENT)) {
		cfg["alignment"] = unit_alignments::get_string(alignment_);
	}
	cfg["flag_rgb"] = flag_rgb_;
	cfg["unrenamable"] = unrenamable_;
	cfg["dismissable"] = dismissable_;
	cfg["block_dismiss_message"] = dismiss_message_;

	cfg["attacks_left"] = attacks_left_;
	if(write_all || get_attr_changed(UA_MAX_AP)) {
		cfg["max_attacks"] = max_attacks_;
	}
	if(write_all || get_attr_changed(UA_ZOC)) {
		cfg["zoc"] = emit_zoc_;
	}
	cfg["hidden"] = hidden_;

	if(write_all || get_attr_changed(UA_ATTACKS) || get_attacks_changed()) {
		cfg.clear_children("attack");
		for(attack_ptr i : attacks_) {
			i->write(cfg.add_child("attack"));
		}
	}

	cfg["cost"] = unit_value_;

	write_subtag("modifications", modifications_);
	if(write_all || get_attr_changed(UA_ABILITIES)) {
		write_subtag("abilities", abilities_);
	}
	if(write_all || get_attr_changed(UA_ADVANCEMENTS)) {
		cfg.clear_children("advancement");
		for(const config& advancement : advancements_) {
			if(!advancement.empty()) {
				cfg.add_child("advancement", advancement);
			}
		}
	}
	cfg.append(back);
}

void unit::set_facing(map_location::direction dir) const
{
	if(dir != map_location::direction::indeterminate && dir != facing_) {
		appearance_changed_ = true;
		facing_ = dir;
	}
	// Else look at yourself (not available so continue to face the same direction)
}

int unit::upkeep() const
{
	// Leaders do not incur upkeep.
	if(can_recruit()) {
		return 0;
	}

	return utils::visit(upkeep_value_visitor{*this}, upkeep_);
}

bool unit::loyal() const
{
	return utils::holds_alternative<upkeep_loyal>(upkeep_);
}

void unit::set_loyal(bool loyal)
{
	if (loyal) {
		upkeep_ = upkeep_loyal{};
		overlays_.push_back("misc/loyal-icon.png");
	} else {
		upkeep_ = upkeep_full{};
		utils::erase(overlays_, "misc/loyal-icon.png");
	}
}

int unit::defense_modifier(const t_translation::terrain_code & terrain) const
{
	int def = movement_type_.defense_modifier(terrain);
#if 0
	// A [defense] ability is too costly and doesn't take into account target locations.
	// Left as a comment in case someone ever wonders why it isn't a good idea.
	unit_ability_list defense_abilities = get_abilities("defense");
	if(!defense_abilities.empty()) {
		unit_abilities::effect defense_effect(defense_abilities, def);
		def = defense_effect.get_composite_value();
	}
#endif
	return def;
}

bool unit::resistance_filter_matches(const config& cfg, const std::string& damage_name, int res) const
{
	const std::string& apply_to = cfg["apply_to"];
	if(!apply_to.empty()) {
		if(damage_name != apply_to) {
			if(apply_to.find(',') != std::string::npos && apply_to.find(damage_name) != std::string::npos) {
				if(!utils::contains(utils::split(apply_to), damage_name)) {
					return false;
				}
			} else {
				return false;
			}
		}
	}

	if(!unit_abilities::filter_base_matches(cfg, res)) {
		return false;
	}

	return true;
}

int unit::resistance_value(unit_ability_list resistance_list, const std::string& damage_name) const
{
	int res = movement_type_.resistance_against(damage_name);
	utils::erase_if(resistance_list, [&](const unit_ability& i) {
		return !resistance_filter_matches(*i.ability_cfg, damage_name, 100-res);
	});

	if(!resistance_list.empty()) {
		unit_abilities::effect resist_effect(resistance_list, 100-res);

		res = 100 - resist_effect.get_composite_value();
	}

	return res;
}

static bool resistance_filter_matches_base(const config& cfg, bool attacker)
{
	if(!(!cfg.has_attribute("active_on") || (attacker && cfg["active_on"] == "offense") || (!attacker && cfg["active_on"] == "defense"))) {
		return false;
	}

	return true;
}

int unit::resistance_against(const std::string& damage_name, bool attacker, const map_location& loc, const_attack_ptr weapon, const const_attack_ptr& opp_weapon) const
{
	if(opp_weapon) {
		return opp_weapon->effective_damage_type().second;
	}
	unit_ability_list resistance_list = get_abilities_weapons("resistance",loc, std::move(weapon), opp_weapon);
	utils::erase_if(resistance_list, [&](const unit_ability& i) {
		return !resistance_filter_matches_base(*i.ability_cfg, attacker);
	});
	return resistance_value(resistance_list, damage_name);
}

std::map<std::string, std::string> unit::advancement_icons() const
{
	std::map<std::string,std::string> temp;
	if(!can_advance()) {
		return temp;
	}

	if(!advances_to_.empty()) {
		std::ostringstream tooltip;
		const std::string& image = game_config::images::level;

		for(const std::string& s : advances_to()) {
			if(!s.empty()) {
				tooltip << s << std::endl;
			}
		}

		temp[image] = tooltip.str();
	}

	for(const config& adv : get_modification_advances()) {
		const std::string& image = adv["image"];
		if(image.empty()) {
			continue;
		}

		std::ostringstream tooltip;
		tooltip << temp[image];

		const std::string& tt = adv["description"];
		if(!tt.empty()) {
			tooltip << tt << std::endl;
		}

		temp[image] = tooltip.str();
	}

	return(temp);
}

std::vector<std::pair<std::string, std::string>> unit::amla_icons() const
{
	std::vector<std::pair<std::string, std::string>> temp;
	std::pair<std::string, std::string> icon; // <image,tooltip>

	for(const config& adv : get_modification_advances()) {
		icon.first = adv["icon"].str();
		icon.second = adv["description"].str();

		for(unsigned j = 0, j_count = modification_count("advancement", adv["id"]); j < j_count; ++j) {
			temp.push_back(icon);
		}
	}

	return(temp);
}

std::vector<config> unit::get_modification_advances() const
{
	std::vector<config> res;
	for(const config& adv : modification_advancements()) {
		if(adv["strict_amla"].to_bool() && !advances_to_.empty()) {
			continue;
		}
		if(auto filter = adv.optional_child("filter")) {
			if(!unit_filter(vconfig(*filter)).matches(*this, loc_)) {
				continue;
			}
		}

		if(modification_count("advancement", adv["id"]) >= static_cast<unsigned>(adv["max_times"].to_int(1))) {
			continue;
		}

		std::vector<std::string> temp_require = utils::split(adv["require_amla"]);
		std::vector<std::string> temp_exclude = utils::split(adv["exclude_amla"]);

		if(temp_require.empty() && temp_exclude.empty()) {
			res.push_back(adv);
			continue;
		}

		std::sort(temp_require.begin(), temp_require.end());
		std::sort(temp_exclude.begin(), temp_exclude.end());

		std::vector<std::string> uniq_require, uniq_exclude;

		std::unique_copy(temp_require.begin(), temp_require.end(), std::back_inserter(uniq_require));
		std::unique_copy(temp_exclude.begin(), temp_exclude.end(), std::back_inserter(uniq_exclude));

		bool exclusion_found = false;
		for(const std::string& s : uniq_exclude) {
			int max_num = std::count(temp_exclude.begin(), temp_exclude.end(), s);
			int mod_num = modification_count("advancement", s);
			if(mod_num >= max_num) {
				exclusion_found = true;
				break;
			}
		}

		if(exclusion_found) {
			continue;
		}

		bool requirements_done = true;
		for(const std::string& s : uniq_require) {
			int required_num = std::count(temp_require.begin(), temp_require.end(), s);
			int mod_num = modification_count("advancement", s);
			if(required_num > mod_num) {
				requirements_done = false;
				break;
			}
		}

		if(requirements_done) {
			res.push_back(adv);
		}
	}

	return res;
}

void unit::set_advancements(std::vector<config> advancements)
{
	set_attr_changed(UA_ADVANCEMENTS);
	advancements_ = std::move(advancements);
}

const std::string& unit::type_id() const
{
	return type_->id();
}

void unit::set_big_profile(const std::string& value)
{
	set_attr_changed(UA_PROFILE);
	profile_ = value;
	adjust_profile(profile_);
}

std::size_t unit::modification_count(const std::string& mod_type, const std::string& id) const
{
	std::size_t res = 0;
	for(const config& item : modifications_.child_range(mod_type)) {
		if(item["id"] == id) {
			++res;
		}
	}

	// For backwards compatibility, if asked for "advancement", also count "advance"
	if(mod_type == "advancement") {
		res += modification_count("advance", id);
	}

	return res;
}

const std::set<std::string> unit::builtin_effects {
	"alignment", "attack", "defense", "ellipse", "experience", "fearless",
	"halo", "healthy", "hitpoints", "image_mod", "jamming", "jamming_costs", "level",
	"loyal", "max_attacks", "max_experience", "movement", "movement_costs",
	"new_ability", "new_advancement", "new_animation", "new_attack", "overlay", "profile",
	"recall_cost", "remove_ability", "remove_advancement", "remove_attacks", "resistance",
	"status", "type", "variation", "vision", "vision_costs", "zoc"
};

std::string unit::describe_builtin_effect(const std::string& apply_to, const config& effect)
{
	if(apply_to == "attack") {
		std::vector<t_string> attack_names;

		std::string desc;
		for(attack_ptr a : attacks_) {
			bool affected = a->describe_modification(effect, &desc);
			if(affected && !desc.empty()) {
				attack_names.emplace_back(a->name(), "wesnoth-units");
			}
		}
		if(!attack_names.empty()) {
			utils::string_map symbols;
			symbols["attack_list"] = utils::format_conjunct_list("", attack_names);
			symbols["effect_description"] = desc;
			return VGETTEXT("$attack_list|: $effect_description", symbols);
		}
	} else if(apply_to == "hitpoints") {
		const std::string& increase_total = effect["increase_total"];
		if(!increase_total.empty()) {
			return VGETTEXT(
				"<span color=\"$color\">$number_or_percent</span> HP",
				{{"number_or_percent", utils::print_modifier(increase_total)}, {"color", increase_total[0] == '-' ? "#f00" : "#0f0"}});
		}
	} else {
		const std::string& increase = effect["increase"];
		if(increase.empty()) {
			return "";
		}
		if(apply_to == "movement") {
			return VNGETTEXT(
				"<span color=\"$color\">$number_or_percent</span> move",
				"<span color=\"$color\">$number_or_percent</span> moves",
				std::stoi(increase),
				{{"number_or_percent", utils::print_modifier(increase)}, {"color", increase[0] == '-' ? "#f00" : "#0f0"}});
		} else if(apply_to == "vision") {
			return VGETTEXT(
				"<span color=\"$color\">$number_or_percent</span> vision",
				{{"number_or_percent", utils::print_modifier(increase)}, {"color", increase[0] == '-' ? "#f00" : "#0f0"}});
		} else if(apply_to == "jamming") {
			return VGETTEXT(
				"<span color=\"$color\">$number_or_percent</span> jamming",
				{{"number_or_percent", utils::print_modifier(increase)}, {"color", increase[0] == '-' ? "#f00" : "#0f0"}});
		} else if(apply_to == "max_experience") {
			// Unlike others, decreasing experience is a *GOOD* thing
			return VGETTEXT(
				"<span color=\"$color\">$number_or_percent</span> XP to advance",
				{{"number_or_percent", utils::print_modifier(increase)}, {"color", increase[0] == '-' ? "#0f0" : "#f00"}});
		} else if(apply_to == "max_attacks") {
			return VNGETTEXT(
					"<span color=\"$color\">$number_or_percent</span> attack per turn",
					"<span color=\"$color\">$number_or_percent</span> attacks per turn",
					std::stoi(increase),
					{{"number_or_percent", utils::print_modifier(increase)}, {"color", increase[0] == '-' ? "#f00" : "#0f0"}});
		} else if(apply_to == "recall_cost") {
			// Unlike others, decreasing recall cost is a *GOOD* thing
			return VGETTEXT(
				"<span color=\"$color\">$number_or_percent</span> cost to recall",
				{{"number_or_percent", utils::print_modifier(increase)}, {"color", increase[0] == '-' ? "#0f0" : "#f00"}});
		}
	}
	return "";
}

void unit::apply_builtin_effect(const std::string& apply_to, const config& effect)
{
	config events;
	appearance_changed_ = true;
	if(apply_to == "fearless") {
		set_attr_changed(UA_IS_FEARLESS);
		is_fearless_ = effect["set"].to_bool(true);
	} else if(apply_to == "healthy") {
		set_attr_changed(UA_IS_HEALTHY);
		is_healthy_ = effect["set"].to_bool(true);
	} else if(apply_to == "profile") {
		if(const config::attribute_value* v = effect.get("portrait")) {
			set_big_profile((*v).str());
		}

		if(const config::attribute_value* v = effect.get("small_portrait")) {
			set_small_profile((*v).str());
		}

		if(const config::attribute_value* v = effect.get("description")) {
			description_ = *v;
		}

		if(config::const_child_itors cfg_range = effect.child_range("special_note")) {
			for(const config& c : cfg_range) {
				if(!c["remove"].to_bool()) {
					special_notes_.emplace_back(c["note"].t_str());
				} else {
					auto iter = std::find(special_notes_.begin(), special_notes_.end(), c["note"].t_str());
					if(iter != special_notes_.end()) {
						special_notes_.erase(iter);
					}
				}
			}
		}
	} else if(apply_to == "new_attack") {
		set_attr_changed(UA_ATTACKS);
		attacks_.emplace_back(new attack_type(effect));
		for(const config& specials : effect.child_range("specials")) {
			for(const auto [key, special] : specials.all_children_view()) {
				for(const config& special_event : special.child_range("event")) {
					events.add_child("event", special_event);
				}
			}
		}
	} else if(apply_to == "remove_attacks") {
		set_attr_changed(UA_ATTACKS);
		utils::erase_if(attacks_, [&effect](const attack_ptr& a) { return a->matches_filter(effect); });
	} else if(apply_to == "attack") {
		set_attr_changed(UA_ATTACKS);
		for(attack_ptr a : attacks_) {
			a->apply_modification(effect);
			for(const config& specials : effect.child_range("set_specials")) {
				for(const auto [key, special] : specials.all_children_view()) {
					for(const config& special_event : special.child_range("event")) {
						events.add_child("event", special_event);
					}
				}
			}
		}
	} else if(apply_to == "hitpoints") {
		LOG_UT << "applying hitpoint mod..." << hit_points_ << "/" << max_hit_points_;
		const std::string& increase_hp = effect["increase"];
		const std::string& increase_total = effect["increase_total"];
		const std::string& set_hp = effect["set"];
		const std::string& set_total = effect["set_total"];

		// If the hitpoints are allowed to end up greater than max hitpoints
		const bool violate_max = effect["violate_maximum"].to_bool();

		if(!set_hp.empty()) {
			if(set_hp.back() == '%') {
				hit_points_ = lexical_cast_default<int>(set_hp)*max_hit_points_/100;
			} else {
				hit_points_ = lexical_cast_default<int>(set_hp);
			}
		}

		if(!set_total.empty()) {
			if(set_total.back() == '%') {
				set_max_hitpoints(lexical_cast_default<int>(set_total)*max_hit_points_/100);
			} else {
				set_max_hitpoints(lexical_cast_default<int>(set_total));
			}
		}

		if(!increase_total.empty()) {
			// A percentage on the end means increase by that many percent
			set_max_hitpoints(utils::apply_modifier(max_hit_points_, increase_total));
		}

		if(max_hit_points_ < 1)
			set_max_hitpoints(1);

		if(effect["heal_full"].to_bool()) {
			heal_fully();
		}

		if(!increase_hp.empty()) {
			hit_points_ = utils::apply_modifier(hit_points_, increase_hp);
		}

		LOG_UT << "modded to " << hit_points_ << "/" << max_hit_points_;
		if(hit_points_ > max_hit_points_ && !violate_max) {
			LOG_UT << "resetting hp to max";
			hit_points_ = max_hit_points_;
		}

		if(hit_points_ < 1) {
			hit_points_ = 1;
		}
	} else if(apply_to == "movement") {
		const bool apply_to_vision = effect["apply_to_vision"].to_bool(true);

		// Unlink vision from movement, regardless of whether we'll increment both or not
		if(vision_ < 0) {
			vision_ = max_movement_;
		}

		const int old_max = max_movement_;

		const std::string& increase = effect["increase"];
		if(!increase.empty()) {
			set_total_movement(utils::apply_modifier(max_movement_, increase, 1));
		}

		set_total_movement(effect["set"].to_int(max_movement_));

		if(movement_ > max_movement_) {
			movement_ = max_movement_;
		}

		if(apply_to_vision) {
			vision_ = std::max(0, vision_ + max_movement_ - old_max);
		}
	} else if(apply_to == "vision") {
		// Unlink vision from movement, regardless of which one we're about to change.
		if(vision_ < 0) {
			vision_ = max_movement_;
		}

		const std::string& increase = effect["increase"];
		if(!increase.empty()) {
			vision_ = utils::apply_modifier(vision_, increase, 1);
		}

		vision_ = effect["set"].to_int(vision_);
	} else if(apply_to == "jamming") {
		const std::string& increase = effect["increase"];

		if(!increase.empty()) {
			jamming_ = utils::apply_modifier(jamming_, increase, 1);
		}

		jamming_ = effect["set"].to_int(jamming_);
	} else if(apply_to == "experience") {
		const std::string& increase = effect["increase"];
		const std::string& set = effect["set"];

		if(!set.empty()) {
			if(set.back() == '%') {
				experience_ = lexical_cast_default<int>(set)*max_experience_/100;
			} else {
				experience_ = lexical_cast_default<int>(set);
			}
		}

		if(increase.empty() == false) {
			experience_ = utils::apply_modifier(experience_, increase, 0);
		}
	} else if(apply_to == "max_experience") {
		const std::string& increase = effect["increase"];
		const std::string& set = effect["set"];

		if(set.empty() == false) {
			if(set.back() == '%') {
				set_max_experience(lexical_cast_default<int>(set)*max_experience_/100);
			} else {
				set_max_experience(lexical_cast_default<int>(set));
			}
		}

		if(increase.empty() == false) {
			set_max_experience(utils::apply_modifier(max_experience_, increase, 1));
		}
	} else if(apply_to == upkeep_loyal::type()) {
		upkeep_ = upkeep_loyal{};
	} else if(apply_to == "status") {
		const std::string& add = effect["add"];
		const std::string& remove = effect["remove"];

		for(const std::string& to_add : utils::split(add))
		{
			set_state(to_add, true);
		}

		for(const std::string& to_remove : utils::split(remove))
		{
			set_state(to_remove, false);
		}
	} else if(utils::contains(movetype::effects, apply_to)) {
		// "movement_costs", "vision_costs", "jamming_costs", "defense", "resistance"
		if(auto ap = effect.optional_child(apply_to)) {
			set_attr_changed(UA_MOVEMENT_TYPE);
			movement_type_.merge(*ap, apply_to, effect["replace"].to_bool());
		}
	} else if(apply_to == "zoc") {
		if(const config::attribute_value* v = effect.get("value")) {
			set_attr_changed(UA_ZOC);
			emit_zoc_ = v->to_bool();
		}
	} else if(apply_to == "new_ability") {
		if(auto ab_effect = effect.optional_child("abilities")) {
			set_attr_changed(UA_ABILITIES);
			config to_append;
			for(const auto [key, cfg] : ab_effect->all_children_view()) {
				if(!has_ability_by_id(cfg["id"])) {
					to_append.add_child(key, cfg);
					for(const config& event : cfg.child_range("event")) {
						events.add_child("event", event);
					}
				}
			}
			abilities_.append(to_append);
		}
	} else if(apply_to == "remove_ability") {
		if(auto ab_effect = effect.optional_child("abilities")) {
			for(const auto [key, cfg] : ab_effect->all_children_view()) {
				remove_ability_by_id(cfg["id"]);
			}
		}
		if(auto fab_effect = effect.optional_child("filter_ability")) {
			remove_ability_by_attribute(*fab_effect);
		}
		if(auto fab_effect = effect.optional_child("experimental_filter_ability")) {
			deprecated_message("experimental_filter_ability", DEP_LEVEL::INDEFINITE, "", "Use filter_ability instead.");
			remove_ability_by_attribute(*fab_effect);
		}
	} else if(apply_to == "image_mod") {
		LOG_UT << "applying image_mod";
		std::string mod = effect["replace"];
		if(!mod.empty()){
			image_mods_ = mod;
		}
		LOG_UT << "applying image_mod";
		mod = effect["add"].str();
		if(!mod.empty()){
			if(!image_mods_.empty()) {
				image_mods_ += '~';
			}

			image_mods_ += mod;
		}

		game_config::add_color_info(game_config_view::wrap(effect));
		LOG_UT << "applying image_mod";
	} else if(apply_to == "new_animation") {
		anim_comp_->apply_new_animation_effect(effect);
	} else if(apply_to == "ellipse") {
		set_image_ellipse(effect["ellipse"]);
	} else if(apply_to == "halo") {
		set_image_halo(effect["halo"]);
	} else if(apply_to == "overlay") {
		const std::string& add = effect["add"];
		const std::string& replace = effect["replace"];
		const std::string& remove = effect["remove"];

		if(!add.empty()) {
			for(const auto& to_add : utils::parenthetical_split(add, ',')) {
				overlays_.push_back(to_add);
			}
		}
		if(!remove.empty()) {
			for(const auto& to_remove : utils::parenthetical_split(remove, ',')) {
				utils::erase(overlays_, to_remove);
			}
		}
		if(add.empty() && remove.empty() && !replace.empty()) {
			overlays_ = utils::parenthetical_split(replace, ',');
		}
	} else if(apply_to == "new_advancement") {
		const std::string& types = effect["types"];
		const bool replace = effect["replace"].to_bool(false);
		set_attr_changed(UA_ADVANCEMENTS);

		if(!types.empty()) {
			if(replace) {
				advances_to_ = utils::parenthetical_split(types, ',');
			} else {
				std::vector<std::string> temp_advances = utils::parenthetical_split(types, ',');
				std::copy(temp_advances.begin(), temp_advances.end(), std::back_inserter(advances_to_));
			}
		}

		if(effect.has_child("advancement")) {
			if(replace) {
				advancements_.clear();
			}

			for(const config& adv : effect.child_range("advancement")) {
				advancements_.push_back(adv);
			}
		}
	} else if(apply_to == "remove_advancement") {
		const std::string& types = effect["types"];
		const std::string& amlas = effect["amlas"];
		set_attr_changed(UA_ADVANCEMENTS);

		std::vector<std::string> temp_advances = utils::parenthetical_split(types, ',');
		std::vector<std::string>::iterator iter;
		for(const std::string& unit : temp_advances) {
			iter = std::find(advances_to_.begin(), advances_to_.end(), unit);
			if(iter != advances_to_.end()) {
				advances_to_.erase(iter);
			}
		}

		temp_advances = utils::parenthetical_split(amlas, ',');

		for(int i = advancements_.size() - 1; i >= 0; i--) {
			if(std::find(temp_advances.begin(), temp_advances.end(), advancements_[i]["id"]) != temp_advances.end()) {
				advancements_.erase(advancements_.begin() + i);
			}
		}
	} else if(apply_to == "alignment") {
		auto new_align = unit_alignments::get_enum(effect["set"].str());
		if(new_align) {
			set_alignment(*new_align);
		}
	} else if(apply_to == "max_attacks") {
		const std::string& increase = effect["increase"];

		if(!increase.empty()) {
			set_max_attacks(utils::apply_modifier(max_attacks_, increase, 1));
		}
	} else if(apply_to == "recall_cost") {
		const std::string& increase = effect["increase"];
		const std::string& set = effect["set"];
		const int team_recall_cost = resources::gameboard ? resources::gameboard->get_team(side_).recall_cost() : 20;
		const int recall_cost = recall_cost_ < 0 ? team_recall_cost : recall_cost_;

		if(!set.empty()) {
			if(set.back() == '%') {
				recall_cost_ = lexical_cast_default<int>(set)*recall_cost/100;
			} else {
				recall_cost_ = lexical_cast_default<int>(set);
			}
		}

		if(!increase.empty()) {
			recall_cost_ = utils::apply_modifier(recall_cost, increase, 1);
		}
	} else if(effect["apply_to"] == "variation") {
		const unit_type*  base_type = unit_types.find(type().parent_id());
		assert(base_type != nullptr);
		const std::string& variation_id = effect["name"];
		if(variation_id.empty() || base_type->get_gender_unit_type(gender_).has_variation(variation_id)) {
			variation_ = variation_id;
			advance_to(*base_type);
			if(effect["heal_full"].to_bool(false)) {
				heal_fully();
			}
		} else {
			WRN_UT << "unknown variation '" << variation_id << "' (name=) in [effect]apply_to=variation, ignoring";
		}
	} else if(effect["apply_to"] == "type") {
		std::string prev_type = effect["prev_type"];
		if(prev_type.empty()) {
			prev_type = type().parent_id();
		}
		const std::string& new_type_id = effect["name"];
		const unit_type* new_type = unit_types.find(new_type_id);
		if(new_type) {
			advance_to(*new_type);
			prefs::get().encountered_units().insert(new_type_id);
			if(effect["heal_full"].to_bool(false)) {
				heal_fully();
			}
		} else {
			WRN_UT << "unknown type '" << new_type_id << "' (name=) in [effect]apply_to=type, ignoring";
		}
	} else if(effect["apply_to"] == "level") {
		const std::string& increase = effect["increase"];
		const std::string& set = effect["set"];

		set_attr_changed(UA_LEVEL);

		// no support for percentages, since levels are usually small numbers

		if(!set.empty()) {
			level_ = lexical_cast_default<int>(set);
		}

		if(!increase.empty()) {
			level_ += lexical_cast_default<int>(increase);
		}
	}

	// In case the effect carries EventWML, apply it now
	if(resources::game_events && resources::lua_kernel) {
		resources::game_events->add_events(events.child_range("event"), *resources::lua_kernel);
	}

	// verify what unit own ability with [affect_adjacent] before edit has_ability_distant_ and has_ability_distant_image_.
	// It is place here for what variables can't be true if unit don't own abilities with [affect_adjacent](after apply_to=remove_ability by example)
	if(apply_to == "new_ability" || apply_to == "remove_ability") {
		set_has_ability_distant();
	}
}

void unit::add_modification(const std::string& mod_type, const config& mod, bool no_add)
{
	bool generate_description = mod["generate_description"].to_bool(true);

	config* target = nullptr;

	if(no_add == false) {
		target = &modifications_.add_child(mod_type, mod);
		target->remove_children("effect");
	}

	std::vector<t_string> effects_description;
	for(const config& effect : mod.child_range("effect")) {
		if(target) {
			//Store effects only after they are added to avoid double applying effects on advance with apply_to=variation.
			target->add_child("effect", effect);
		}
		// Apply SUF.
		if(auto afilter = effect.optional_child("filter")) {
			assert(resources::filter_con);
			if(!unit_filter(vconfig(*afilter)).matches(*this, loc_)) {
				continue;
			}
		}
		const std::string& apply_to = effect["apply_to"];
		int times = effect["times"].to_int(1);
		t_string description;

		if(no_add && (apply_to == "type" || apply_to == "variation")) {
			continue;
		}

		if(effect["times"] == "per level") {
			if(effect["apply_to"] == "level") {
				WRN_UT << "[effect] times=per level is not allowed with apply_to=level, using default value of 1";
				times = 1;
			}
			else {
				times = level_;
			}
		}

		if(times) {
			while (times > 0) {
				times --;
				std::string description_component;
				if(resources::lua_kernel) {
					description_component = resources::lua_kernel->apply_effect(apply_to, *this, effect, true);
				} else if(builtin_effects.count(apply_to)) {
					// Normally, the built-in effects are dispatched through Lua so that a user
					// can override them if desired. However, since they're built-in, we can still
					// apply them if the lua kernel is unavailable.
					apply_builtin_effect(apply_to, effect);
					description_component = describe_builtin_effect(apply_to, effect);
				}
				if(!times) {
					description += description_component;
				}
			} // end while
		} else { // for times = per level & level = 0 we still need to rebuild the descriptions
			if(resources::lua_kernel) {
				description += resources::lua_kernel->apply_effect(apply_to, *this, effect, false);
			} else if(builtin_effects.count(apply_to)) {
				description += describe_builtin_effect(apply_to, effect);
			}
		}

		if(effect["times"] == "per level" && !times) {
			description = VGETTEXT("$effect_description per level", {{"effect_description", description}});
		}

		if(!description.empty()) {
			effects_description.push_back(description);
		}
	}

	t_string description;

	const t_string& mod_description = mod["description"];
	if(!mod_description.empty()) {
		description = mod_description;
	}

	// Punctuation should be translatable: not all languages use Latin punctuation.
	// (However, there maybe is a better way to do it)
	if(generate_description && !effects_description.empty()) {
		if(!mod_description.empty()) {
			description += "\n";
		}

		for(const auto& desc_line : effects_description) {
			description += desc_line + "\n";
		}
	}

	// store trait info
	if(mod_type == "trait") {
		add_trait_description(mod, description);
	}

	//NOTE: if not a trait, description is currently not used
}

void unit::add_trait_description(const config& trait, const t_string& description)
{
	const std::string& gender_string = gender_ == unit_race::FEMALE ? "female_name" : "male_name";
	const auto& gender_specific_name = trait[gender_string];

	const t_string name = gender_specific_name.empty() ? trait["name"] : gender_specific_name;

	if(!name.empty()) {
		trait_names_.push_back(name);
		trait_descriptions_.push_back(description);
		trait_nonhidden_ids_.push_back(trait["id"]);
	}
}

std::string unit::absolute_image() const
{
	return type().icon().empty() ? type().image() : type().icon();
}

std::string unit::default_anim_image() const
{
	return type().image().empty() ? type().icon() : type().image();
}

void unit::apply_modifications()
{
	log_scope("apply mods");

	variables_.clear_children("mods");
	if(modifications_.has_child("advance")) {
		deprecated_message("[advance]", DEP_LEVEL::PREEMPTIVE, {1, 15, 0}, "Use [advancement] instead.");
	}
	for(const auto [key, cfg] : modifications_.all_children_view()) {
		add_modification(key, cfg, true);
	}
}

bool unit::invisible(const map_location& loc, bool see_all) const
{
	if(loc != get_location()) {
		DBG_UT << "unit::invisible called: id = " << id() << " loc = " << loc << " get_loc = " << get_location();
	}

	// This is a quick condition to check, and it does not depend on the
	// location (so might as well bypass the location-based cache).
	if(get_state(STATE_UNCOVERED)) {
		return false;
	}

	// Fetch from cache
	/**
	 * @todo FIXME: We use the cache only when using the default see_all=true
	 * Maybe add a second cache if the see_all=false become more frequent.
	 */
	if(see_all) {
		const auto itor = invisibility_cache_.find(loc);
		if(itor != invisibility_cache_.end()) {
			return itor->second;
		}
	}

	// Test hidden status
	static const std::string hides("hides");
	bool is_inv = get_ability_bool(hides, loc);
	if(is_inv){
		is_inv = (resources::gameboard ? !resources::gameboard->would_be_discovered(loc, side_,see_all) : true);
	}

	if(see_all) {
		// Add to caches
		if(invisibility_cache_.empty()) {
			units_with_cache.push_back(this);
		}

		invisibility_cache_[loc] = is_inv;
	}

	return is_inv;
}

bool unit::is_visible_to_team(const team& team, bool const see_all) const
{
	const map_location& loc = get_location();
	return is_visible_to_team(loc, team, see_all);
}

bool unit::is_visible_to_team(const map_location& loc, const team& team, bool const see_all) const
{
	if(!display::get_singleton()->context().map().on_board(loc)) {
		return false;
	}

	if(see_all) {
		return true;
	}

	if(team.is_enemy(side()) && invisible(loc)) {
		return false;
	}

	// allied planned moves are also visible under fog. (we assume that fake units on the map are always whiteboard markers)
	if(!team.is_enemy(side()) && underlying_id_.is_fake()) {
		return true;
	}

	// when the whiteboard planned unit map is applied, it uses moved the _real_ unit so
	// underlying_id_.is_fake() will be false and the check above will not apply.
	// TODO: improve this check so that is also works for allied planned units but without
	//       breaking sp campaigns with allies under fog. We probably need an explicit flag
	//       is_planned_ in unit that is set by the whiteboard.
	if(team.side() == side()) {
		return true;
	}

	if(team.fogged(loc)) {
		return false;
	}

	return true;
}

void unit::set_underlying_id(n_unit::id_manager& id_manager)
{
	if(underlying_id_.value == 0) {
		if(synced_context::is_synced() || !resources::gamedata || resources::gamedata->phase() == game_data::INITIAL) {
			underlying_id_ = id_manager.next_id();
		} else {
			underlying_id_ = id_manager.next_fake_id();
		}
	}

	if(id_.empty() /*&& !underlying_id_.is_fake()*/) {
		std::stringstream ss;
		ss << (type_id().empty() ? "Unit" : type_id()) << "-" << underlying_id_.value;
		id_ = ss.str();
	}
}

unit& unit::mark_clone(bool is_temporary)
{
	n_unit::id_manager& ids = resources::gameboard ? resources::gameboard->unit_id_manager() : n_unit::id_manager::global_instance();
	if(is_temporary) {
		underlying_id_ = ids.next_fake_id();
	} else {
		if(synced_context::is_synced() || !resources::gamedata || resources::gamedata->phase() == game_data::INITIAL) {
			underlying_id_ = ids.next_id();
		}
		else {
			underlying_id_ = ids.next_fake_id();
		}
		std::string::size_type pos = id_.find_last_of('-');
		if(pos != std::string::npos && pos+1 < id_.size()
		&& id_.find_first_not_of("0123456789", pos+1) == std::string::npos) {
			// this appears to be a duplicate of a generic unit, so give it a new id
			WRN_UT << "assigning new id to clone of generic unit " << id_;
			id_.clear();
			set_underlying_id(ids);
		}
	}
	return *this;
}


unit_movement_resetter::unit_movement_resetter(const unit &u, bool operate)
	: u_(const_cast<unit&>(u))
	, moves_(u.movement_left(true))
{
	if(operate) {
		u_.set_movement(u_.total_movement());
	}
}

unit_movement_resetter::~unit_movement_resetter()
{
	assert(resources::gameboard);
	try {
		if(!resources::gameboard->units().has_unit(&u_)) {
			/*
			* It might be valid that the unit is not in the unit map.
			* It might also mean a no longer valid unit will be assigned to.
			*/
			DBG_UT << "The unit to be removed is not in the unit map.";
		}

		u_.set_movement(moves_);
	} catch(...) {
		DBG_UT << "Caught exception when destroying unit_movement_resetter: " << utils::get_unknown_exception_type();
	}
}

std::string unit::TC_image_mods() const
{
	return formatter() << "~RC(" << flag_rgb() << ">" << team::get_side_color_id(side()) << ")";
}

std::string unit::image_mods() const
{
	if(!image_mods_.empty()) {
		return formatter() << "~" << image_mods_ << TC_image_mods();
	}

	return TC_image_mods();
}

// Called by the Lua API after resetting an attack pointer.
bool unit::remove_attack(const attack_ptr& atk)
{
	set_attr_changed(UA_ATTACKS);
	auto iter = std::find(attacks_.begin(), attacks_.end(), atk);
	if(iter == attacks_.end()) {
		return false;
	}
	attacks_.erase(iter);
	return true;
}

void unit::remove_attacks_ai()
{
	if(attacks_left_ == max_attacks_) {
		//TODO: add state_not_attacked
	}

	set_attacks(0);
}

void unit::remove_movement_ai()
{
	if(movement_left() == total_movement()) {
		set_state(STATE_NOT_MOVED,true);
	}

	set_movement(0, true);
}

void unit::set_hidden(bool state) const
{
//	appearance_changed_ = true;
	hidden_ = state;
	if(!state) {
		return;
	}

	// TODO: this should really hide the halo, not destroy it
	// We need to get rid of haloes immediately to avoid display glitches
	anim_comp_->clear_haloes();
}

double unit::hp_bar_scaling() const
{
	return type().hp_bar_scaling();
}
double unit::xp_bar_scaling() const
{
	return type().xp_bar_scaling();
}

void unit::set_image_halo(const std::string& halo)
{
	appearance_changed_ = true;
	anim_comp_->clear_haloes();
	halo_ = halo;
}

void unit::parse_upkeep(const config::attribute_value& upkeep)
{
	if(upkeep.empty()) {
		return;
	}

	try {
		upkeep_ = upkeep.apply_visitor(upkeep_parser_visitor());
	} catch(std::invalid_argument& e) {
		WRN_UT << "Found invalid upkeep=\"" << e.what() <<  "\" in a unit";
		upkeep_ = upkeep_full{};
	}
}

void unit::write_upkeep(config::attribute_value& upkeep) const
{
	upkeep = utils::visit(upkeep_type_visitor(), upkeep_);
}

void unit::clear_changed_attributes()
{
	changed_attributes_.reset();
	for(const auto& a_ptr : attacks_) {
		a_ptr->set_changed(false);
	}
}

std::vector<t_string> unit::unit_special_notes() const {
	return combine_special_notes(special_notes_, abilities(), attacks(), movement_type());
}

// Filters unimportant stats from the unit config and returns a checksum of
// the remaining config.
std::string get_checksum(const unit& u, backwards_compatibility::unit_checksum_version version)
{
	config unit_config;
	config wcfg;
	u.write(unit_config);

	static const std::set<std::string_view> main_keys {
		"advances_to",
		"alignment",
		"cost",
		"experience",
		"gender",
		"hitpoints",
		"ignore_race_traits",
		"ignore_global_traits",
		"level",
		"recall_cost",
		"max_attacks",
		"max_experience",
		"max_hitpoints",
		"max_moves",
		"movement",
		"movement_type",
		"race",
		"random_traits",
		"resting",
		"undead_variation",
		"upkeep",
		"zoc"
	};

	for(const std::string_view& main_key : main_keys) {
		wcfg[main_key] = unit_config[main_key];
	}

	static const std::set<std::string_view> attack_keys {
		"name",
		"type",
		"range",
		"damage",
		"number"
	};

	for(const config& att : unit_config.child_range("attack")) {
		config& child = wcfg.add_child("attack");

		for(const std::string_view& attack_key : attack_keys) {
			child[attack_key] = att[attack_key];
		}

		for(const config& spec : att.child_range("specials")) {
			config& child_spec = child.add_child("specials", spec);

			child_spec.recursive_clear_value("description");
			if(version != backwards_compatibility::unit_checksum_version::version_1_16_or_older) {
				child_spec.recursive_clear_value("description_inactive");
				child_spec.recursive_clear_value("name");
				child_spec.recursive_clear_value("name_inactive");
			}
		}
	}

	for(const config& abi : unit_config.child_range("abilities")) {
		config& child = wcfg.add_child("abilities", abi);

		child.recursive_clear_value("description");
		child.recursive_clear_value("description_inactive");
		child.recursive_clear_value("name");
		child.recursive_clear_value("name_inactive");
	}

	for(const config& trait : unit_config.child_range("trait")) {
		config& child = wcfg.add_child("trait", trait);

		child.recursive_clear_value("description");
		child.recursive_clear_value("male_name");
		child.recursive_clear_value("female_name");
		child.recursive_clear_value("name");
	}

	static const std::set<std::string_view> child_keys {
		"advance_from",
		"defense",
		"movement_costs",
		"vision_costs",
		"jamming_costs",
		"resistance"
	};

	for(const std::string_view& child_key : child_keys) {
		for(const config& c : unit_config.child_range(child_key)) {
			wcfg.add_child(child_key, c);
		}
	}

	DBG_UT << wcfg;

	return wcfg.hash();
}
