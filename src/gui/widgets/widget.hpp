/*
	Copyright (C) 2007 - 2025
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

#pragma once

#include "color.hpp"
#include "gui/core/event/dispatcher.hpp"
#include "gui/widgets/event_executor.hpp"
#include "gui/widgets/helper.hpp"
#include "scripting/lua_ptr.hpp"
#include "sdl/point.hpp"
#include "sdl/rect.hpp"
#include "wml_exception.hpp"

#include <string>


namespace gui2
{
/* Data format used by styled_widget::set_members to set settings for a single widget. */
using widget_item = std::map<std::string, t_string>;

/* Indexes multiple @ref widget_item maps by widget ID. */
using widget_data = std::map<std::string, widget_item>;

struct builder_widget;
class window;
class grid;

namespace iteration
{
using walker_ptr = std::unique_ptr<class walker_base>;
} // namespace iteration

/**
 * Base class for all widgets.
 *
 * From this abstract all other widgets should derive. It contains the minimal
 * info needed for a real widget and some pure abstract functions which need to
 * be implemented by classes deriving from this class.
 */
class widget : public event_executor, public event::dispatcher, public enable_lua_ptr<widget>
{
	friend class debug_layout_graph;
	friend class window; // needed for modifying the layout_size.


	/***** ***** ***** ***** ***** Types. ***** ***** ***** ***** *****/

public:
	/** Visibility settings done by the user. */
	enum class visibility
	{
		/**
		 * The user sets the widget visible, that means:
		 * * The widget is visible.
		 * * @ref find_at always 'sees' the widget (the active flag is
		 *   tested later).
		 * * The widget (if active) handles events (and sends events to
		 *   its children).
		 * * The widget is drawn.
		 */
		visible,

		/**
		 * The user sets the widget hidden, that means:
		 * * The widget is invisible but keeps its size.
		 * * @ref find_at 'sees' the widget if active is @c false.
		 * * The widget doesn't handle events (and doesn't send events to
		 *   its children).
		 */
		hidden,

		/**
		 * The user set the widget invisible, that means:
		 * * The widget is invisible and its grid cell has size 0,0.
		 * * @ref find_at never 'sees' the widget.
		 * * The widget doesn't handle events (and doesn't send events to
		 *   its children).
		 */
		invisible
	};

	/**
	 * Visibility set by the engine.
	 *
	 * This state only will be used if @ref visible_ is @ref visibility::visible
	 * depending on this state the widget might not be visible after all.
	 */
	enum class redraw_action
	{
		// TODO: draw_manager/hwaccel - are these still useful?
		/**
		 * The widget is fully visible.
		 *
		 * The widget should be drawn. The entire widget's rectangle
		 * should be redrawn.
		 */
		full,

		/**
		 * The widget is partly visible.
		 *
		 * The should be drawn. The rectangle to redraw in determined
		 * by @ref clipping_rectangle_
		 */
		partly,

		/**
		 * The widget is not visible.
		 *
		 * The widget should not be drawn.
		 */
		none
	};

	enum class debug_border {
		/** No border. */
		none,
		/** Single-pixel outline. */
		outline,
		/** Flood-filled rectangle. */
		fill,
	};

	/***** ***** ***** Constructor and destructor. ***** ***** *****/

public:
	widget(const widget&) = delete;
	widget& operator=(const widget&) = delete;

	/** @deprecated use the second overload. */
	widget();

	/**
	 * Constructor.
	 *
	 * @param builder             The builder object with the settings for the
	 *                            object.
	 */
	explicit widget(const builder_widget& builder);

	virtual ~widget() override;


	/***** ***** ***** ***** ID functions. ***** ***** ***** *****/

public:
	/*** *** *** *** *** *** Setters and getters. *** *** *** *** *** ***/

	void set_id(const std::string& id);
	const std::string& id() const;

	/*** *** *** *** *** *** *** *** Members. *** *** *** *** *** *** *** ***/

private:
	/**
	 * The id is the unique name of the widget in a certain context.
	 *
	 * This is needed for certain widgets so the engine knows which widget is
	 * which. E.g. it knows which button is pressed and thus which engine action
	 * is connected to the button. This doesn't mean that the id is unique in a
	 * window, e.g. a listbox can have the same id for every row.
	 */
	std::string id_;


	/***** ***** ***** ***** Parent functions ***** ***** ***** *****/

public:
	/**
	 * Get the parent window.
	 *
	 * @returns                   Pointer to parent window.
	 * @retval nullptr            No parent window found.
	 */
	window* get_window();

	/** The constant version of @ref get_window. */
	const window* get_window() const;

	/**
	 * Get the parent grid.
	 *
	 * @returns                 Pointer to parent grid.
	 * @retval nullptr          No parent grid found.
	 */
	grid* get_parent_grid();
	const grid* get_parent_grid() const;

	/*** *** *** *** *** *** Setters and getters. *** *** *** *** *** ***/

	void set_parent(widget* parent);
	widget* parent();

	/*** *** *** *** *** *** *** *** Members. *** *** *** *** *** *** *** ***/

private:
	/**
	 * The parent widget.
	 *
	 * If the widget has a parent it contains a pointer to the parent, else it
	 * is set to @c nullptr.
	 */
	widget* parent_;


	/***** ***** ***** ***** Size and layout functions. ***** ***** ***** *****/

public:
	/**
	 * How the layout engine works.
	 *
	 * Every widget has a member @ref layout_size_ which holds the best size in
	 * the current layout phase. When the windows starts the layout phase it
	 * calls @ref layout_initialize which resets this value.
	 *
	 * Every widget has two function to get the best size. @ref get_best_size
	 * tests whether layout_size_ is set and if so returns that value otherwise
	 * it calls @ref calculate_best_size so the size can be updated.
	 *
	 * During the layout phase some functions can modify layout_size_ so the
	 * next call to @ref get_best_size returns the currently best size. This
	 * means that after the layout phase @ref get_best_size still returns this
	 * value.
	 */

	/**
	 * Initialises the layout phase.
	 *
	 * Clears the initial best size for the widgets.
	 *
	 * See @ref layout_algorithm for more information.
	 *
	 * @param full_initialization For widgets with scrollbars it hides them
	 *                            unless the mode is
	 *                            scrollbar_mode::ALWAYS_VISIBLE. For
	 *                            other widgets this flag is a @em NOP.
	 */
	virtual void layout_initialize(const bool full_initialization);

	/**
	 * Tries to reduce the width of a widget.
	 *
	 * This function tries to do it 'friendly' and only use scrollbars or
	 * tries to wrap the widget.
	 *
	 * See @ref layout_algorithm for more information.
	 *
	 * @param maximum_width       The wanted maximum width.
	 */
	virtual void request_reduce_width(const unsigned maximum_width) = 0;

	/**
	 * Tries to reduce the width of a widget.
	 *
	 * This function does it more aggressively and should only be used when
	 * using scrollbars and wrapping failed.
	 *
	 * @todo Make pure virtual.
	 *
	 * See @ref layout_algorithm for more information.
	 *
	 * @param maximum_width       The wanted maximum width.
	 */
	virtual void demand_reduce_width(const unsigned maximum_width);

	/**
	 * Tries to reduce the height of a widget.
	 *
	 * This function tries to do it 'friendly' and only use scrollbars.
	 *
	 * @todo Make pure virtual.
	 *
	 * See @ref layout_algorithm for more information.
	 *
	 * @param maximum_height      The wanted maximum height.
	 */
	virtual void request_reduce_height(const unsigned maximum_height);

	/**
	 * Tries to reduce the height of a widget.
	 *
	 * This function does it more aggressively and should only be used when
	 * using scrollbars failed.
	 *
	 * @todo Make pure virtual.
	 *
	 * See @ref layout_algorithm for more information.
	 *
	 * @param maximum_height      The wanted maximum height.
	 */
	virtual void demand_reduce_height(const unsigned maximum_height);

	/**
	 * Gets the best size for the widget.
	 *
	 * During the layout phase a best size will be determined, several stages
	 * might change the best size. This function will return the currently best
	 * size as determined during the layout phase.
	 *
	 * @returns                      The best size for the widget.
	 */
	point get_best_size() const;

private:
	/**
	 * Calculates the best size.
	 *
	 * This function calculates the best size and ignores the current values in
	 * the layout phase. Note containers can call the @ref get_best_size() of
	 * their children since it is meant to update itself.
	 *
	 * @returns                      The best size for the widget.
	 */
	virtual point calculate_best_size() const = 0;

public:
	/**
	 * Whether the mouse move/click event go 'through' this widget.
	 */
	virtual bool can_mouse_focus() const { return true; }
	/**
	 * Can the widget wrap.
	 *
	 * When a widget can wrap it can reduce its width by increasing its
	 * height. When a layout is too wide it should first try to wrap and if
	 * that fails it should check the vertical scrollbar status. After wrapping
	 * the height might (probably will) change so the layout engine needs to
	 * recalculate the height after wrapping.
	 */
	virtual bool can_wrap() const;

	/**
	 * Sets the origin of the widget.
	 *
	 * This function can be used to move the widget without triggering a
	 * redraw. The location is an absolute position, if a relative move is
	 * required use @ref move.
	 *
	 * @param origin              The new origin.
	 */
	virtual void set_origin(const point& origin);

	/**
	 * Sets the size of the widget.
	 *
	 * This version is meant to resize a widget, since the origin isn't
	 * modified. This can be used if a widget needs to change its size and the
	 * layout will be fixed later.
	 *
	 * @param size                The size of the widget.
	 */
	virtual void set_size(const point& size);

	/**
	 * Places the widget.
	 *
	 * This function is normally called by a layout function to do the
	 * placement of a widget.
	 *
	 * @param origin              The position of top left of the widget.
	 * @param size                The size of the widget.
	 */
	virtual void place(const point& origin, const point& size);

	/**
	 * Moves a widget.
	 *
	 * This function can be used to move the widget without queueing a redraw.
	 *
	 * @todo Implement the function to all derived classes.
	 *
	 * @param x_offset            The amount of pixels to move the widget in
	 *                            the x-direction.
	 * @param y_offset            The amount of pixels to move the widget in
	 *                            the y-direction.
	 */
	virtual void move(const int x_offset, const int y_offset);

	/**
	 * Sets the horizontal alignment of the widget within its parent grid.
	 *
	 * @param alignment           The new alignment.
	 */
	virtual void set_horizontal_alignment(const std::string& alignment);

	/**
	 * Sets the horizontal alignment of the widget within its parent grid.
	 *
	 * @param alignment           The new alignment.
	 */
	virtual void set_vertical_alignment(const std::string& alignment);

	/**
	 * Allows a widget to update its children.
	 *
	 * Before the window is populating the dirty list the widgets can update
	 * their content, which allows delayed initialization. This delayed
	 * initialization is only allowed if the widget resizes itself, not when
	 * being placed.
	 */
	virtual void layout_children();

	/**
	 * Returns the screen origin of the widget.
	 *
	 * @returns                   The origin of the widget.
	 */
	point get_origin() const;

	/**
	 * Returns the size of the widget.
	 *
	 * @returns                   The size of the widget.
	 */
	point get_size() const;

	/**
	 * Gets the bounding rectangle of the widget on the screen.
	 *
	 * @returns                   The bounding rectangle of the widget.
	 */
	rect get_rectangle() const;

	/*** *** *** *** *** *** Setters and getters. *** *** *** *** *** ***/

	int get_x() const;

	int get_y() const;

	unsigned get_width() const;

	unsigned get_height() const;

protected:
	void set_layout_size(const point& size);
	const point& layout_size() const;

	/**
	* Throws away @ref layout_size_.
	*
	* Use with care: this function does not recurse to child widgets.
	*
	* See @ref layout_algorithm for more information.
	*/
	void clear_layout_size() { set_layout_size(point()); }

public:
	void set_linked_group(const std::string& linked_group);

	std::string get_linked_group() const
	{
		return linked_group_;
	}

	/*** *** *** *** *** *** *** *** Members. *** *** *** *** *** *** *** ***/

private:
	/** The x-coordinate of the widget on the screen. */
	int x_;

	/** The y-coordinate of the widget on the screen. */
	int y_;

	/** The width of the widget. */
	unsigned width_;

	/** The height of the widget. */
	unsigned height_;

	/**
	 * The best size for the widget.
	 *
	 * When 0,0 the real best size is returned, but in the layout phase a
	 * wrapping or a scrollbar might change the best size for that widget.
	 * This variable holds that best value.
	 *
	 * If the widget size hasn't been changed from the default that
	 * calculate_best_size() returns, layout_size_ is (0,0).
	 */
	point layout_size_;

#ifdef DEBUG_WINDOW_LAYOUT_GRAPHS

	/**
	 * Debug helper to store last value of get_best_size().
	 *
	 * We're mutable so calls can stay const and this is disabled in
	 * production code.
	 */
	mutable point last_best_size_;

#endif

	/**
	 * The linked group the widget belongs to.
	 *
	 * @todo For now the linked group is initialized when the layout of the
	 * widget is initialized. The best time to set it would be upon adding the
	 * widget in the window. Need to look whether it is possible in a clean way.
	 * Maybe a signal just prior to showing a window where the widget can do
	 * some of it's on things, would also be nice for widgets that need a
	 * finalizer function.
	 */
	std::string linked_group_;


	/***** ***** ***** ***** Drawing functions. ***** ***** ***** *****/

public:
	/**
	 * Calculates the blitting rectangle of the widget.
	 *
	 * The blitting rectangle is the entire widget rectangle.
	 *
	 * @returns                   The drawing rectangle.
	 */
	rect calculate_blitting_rectangle() const;

	/**
	 * Calculates the clipping rectangle of the widget.
	 *
	 * The clipping rectangle is used then the @ref redraw_action_ is
	 * @ref redraw_action::partly.
	 *
	 * @returns                   The clipping rectangle.
	 */
	rect calculate_clipping_rectangle() const;

	/**
	 * Draws the background of a widget.
	 *
	 * Derived should override @ref impl_draw_background instead of changing
	 * this function.
	 *
	 * @returns                   False if drawing should be deferred.
	 */
	bool draw_background();

	/**
	 * Draws the children of a widget.
	 *
	 * Containers should draw their children when they get this request.
	 *
	 * Derived should override @ref impl_draw_children instead of changing
	 * this function.
	 */
	void draw_children();

	/**
	 * Draws the foreground of the widget.
	 *
	 * Some widgets e.g. panel and window have a back and foreground layer this
	 * function requests the drawing of the foreground.
	 *
	 * Derived should override @ref impl_draw_foreground instead of changing
	 * this function.
	 *
	 * @returns                   False if drawing should be deferred.
	 */
	bool draw_foreground();

private:
	/** See @ref draw_background. */
	virtual bool impl_draw_background()
	{
		return true;
	}

	/** See @ref draw_children. */
	virtual void impl_draw_children()
	{
	}

	/** See @ref draw_foreground. */
	virtual bool impl_draw_foreground()
	{
		return true;
	}

public:
	/**
	 * Gets the dirty rectangle of the widget.
	 *
	 * Depending on the @ref redraw_action_ it returns the rectangle this
	 * widget dirties while redrawing.
	 *
	 * @returns                   The dirty rectangle.
	 */
	rect get_dirty_rectangle() const;

	/**
	 * Sets the visible rectangle for a widget.
	 *
	 * This function sets the @ref redraw_action_ and the
	 * @ref clipping_rectangle_.
	 *
	 * @param rectangle           The visible rectangle in screen coordinates.
	 */
	virtual void set_visible_rectangle(const rect& rectangle);

	/**
	 * Indicates that this widget should be redrawn.
	 *
	 * This function should be called by widgets whenever their visible
	 * state changes.
	 */
	void queue_redraw();

	/**
	 * Indicate that specific region of the screen should be redrawn.
	 *
	 * This is in absolute drawing-space coordinates, and not constrained
	 * to the current widget.
	 */
	void queue_redraw(const rect& region);

	/*** *** *** *** *** *** Setters and getters. *** *** *** *** *** ***/

	void set_visible(const visibility visible);
	visibility get_visible() const;

	/** Sets widget to visible if @a visible is true, else invisible. */
	void set_visible(bool visible)
	{
		set_visible(visible ? visibility::visible : visibility::invisible);
	}

	redraw_action get_drawing_action() const;

	void set_debug_border_mode(const debug_border debug_border_mode);

	void set_debug_border_color(const color_t debug_border_color);

	/*** *** *** *** *** *** *** *** Members. *** *** *** *** *** *** *** ***/

private:
	/** Field for the status of the visibility. */
	visibility visible_;

	/** Field for the action to do on a drawing request. */
	redraw_action redraw_action_;

	/** The clipping rectangle if a widget is partly visible. */
	rect clipping_rectangle_;

	/**
	 * Mode for drawing the debug border.
	 *
	 * The debug border is a helper border to determine where a widget is
	 * placed. It is only intended for debugging purposes.
	 */
	debug_border debug_border_mode_;

	/** The color for the debug border. */
	color_t debug_border_color_;

	void draw_debug_border();

	/***** ***** ***** ***** Query functions ***** ***** ***** *****/

public:
	/**
	 * Returns the widget at the wanted coordinates.
	 *
	 * @param coordinate          The coordinate which should be inside the
	 *                            widget.
	 * @param must_be_active      The widget should be active, not all widgets
	 *                            have an active flag, those who don't ignore
	 *                            flag.
	 *
	 * @returns                   The widget with the id.
	 * @retval nullptr               No widget at the wanted coordinate found (or
	 *                            not active if must_be_active was set).
	 */
	virtual widget* find_at(const point& coordinate,
							 const bool must_be_active);

	/** The constant version of @ref find_at. */
	virtual const widget* find_at(const point& coordinate,
								   const bool must_be_active) const;

	/**
	 * Returns @em a widget with the wanted id.
	 *
	 * @note Since the id might not be unique inside a container there is no
	 * guarantee which widget is returned.
	 *
	 * @param id                  The id of the widget to find.
	 * @param must_be_active      The widget should be active, not all widgets
	 *                            have an active flag, those who don't ignore
	 *                            flag.
	 *
	 * @returns                   The widget with the id.
	 * @retval nullptr               No widget with the id found (or not active if
	 *                            must_be_active was set).
	 */
	virtual widget* find(const std::string_view id, const bool must_be_active);

	/** The constant version of @ref find. */
	virtual const widget* find(const std::string_view id, const bool must_be_active) const;

	/**
	 * Does the widget contain the widget.
	 *
	 * Widgets can be containers which have more widgets inside them, this
	 * function will traverse in those child widgets and tries to find the
	 * wanted widget.
	 *
	 * @param widget              Pointer to the widget to find.
	 *
	 * @returns                   Whether or not the @p widget was found.
	 */
	virtual bool has_widget(const widget& widget) const;

	/**
	 * Gets a widget with the wanted id.
	 *
	 * This template function doesn't return a pointer to a generic widget but
	 * returns the wanted type and tests for its existence if required.
	 *
	 * @param id                  The id of the widget to find.
	 * @param must_be_active      The widget should be active, not all widgets
	 *                            have an active flag, those who don't ignore
	 *                            flag.
	 * @param must_exist          The widget should be exist, the function will
	 *                            fail if the widget doesn't exist or is
	 *                            inactive and must be active. Upon failure a
	 *                            wml_error is thrown.
	 *
	 * @returns                   The widget with the id.
	 */
	template <class T>
	T* find_widget(
		const std::string_view id,
		const bool must_be_active,
		const bool must_exist)
	{
		T* result = dynamic_cast<T*>(this->find(id, must_be_active));
		VALIDATE(!must_exist || result, missing_widget(std::string(id)));

		return result;
	}

	template <class T>
	const T* find_widget(
		const std::string_view id,
		const bool must_be_active,
		const bool must_exist) const
	{
		T* result = dynamic_cast<T*>(this->find(id, must_be_active));
		VALIDATE(!must_exist || result, missing_widget(std::string(id)));

		return result;
	}

	/**
	 * Gets a widget with the wanted id.
	 *
	 * This template function doesn't return a reference to a generic widget but
	 * returns a reference to the wanted type
	 *
	 * @param id                  The id of the widget to find.
	 * @param must_be_active      The widget should be active, not all widgets
	 *                            have an active flag, those who don't ignore
	 *                            flag.
	 *
	 * @returns                   The widget with the id.
	 */
	template <class T>
	T& find_widget(
		const std::string_view id,
		const bool must_be_active = false)
	{
		return *(this->find_widget<T>(id, must_be_active, true));
	}

	template <class T>
	const T& find_widget(
		const std::string_view id,
		const bool must_be_active = false) const
	{
		return *(this->find_widget<T>(id, must_be_active, true));
	}

private:
	/** See @ref event::dispatcher::is_at. */
	virtual bool is_at(const point& coordinate) const override;

	/**
	 * Is the coordinate inside our area.
	 *
	 * Helper for find_at so also looks at our visibility.
	 *
	 * @param coordinate          The coordinate which should be inside the
	 *                            widget.
	 * @param must_be_active      The widget should be active, not all widgets
	 *                            have an active flag, those who don't ignore
	 *                            flag.
	 *
	 * @returns                   Status.
	 */
	bool is_at(const point& coordinate, const bool must_be_active) const;

	/**
	 * Is the widget and every single one of its parents visible?
	 *
	 * @param widget              Widget where to start the check.
	 * @param must_be_active      The widget should be active, not all widgets
	 *                            have an active flag, those who don't ignore
	 *                            flag.
	 *
	 * @returns                   Status.
	 */
	bool recursive_is_visible(const widget* widget, const bool must_be_active) const;

	/***** ***** ***** ***** Miscellaneous ***** ***** ****** *****/

public:
	/** Does the widget disable easy close? */
	virtual bool disable_click_dismiss() const = 0;

	/** Creates a new walker object on the heap. */
	virtual iteration::walker_ptr create_walker() = 0;
};

} // namespace gui2
