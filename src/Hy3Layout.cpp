#include "Hy3Layout.hpp"
#include "src/Window.hpp"
#include "src/debug/Log.hpp"
#include "src/helpers/Vector2D.hpp"
#include "src/helpers/Workspace.hpp"
#include "src/managers/XWaylandManager.hpp"
#include "src/managers/input/InputManager.hpp"
#include "src/render/Renderer.hpp"
#include <cairo/cairo.h>
#include <ctime>
#include <memory>
#include <src/Compositor.hpp>

Hy3GroupData::Hy3GroupData(Hy3GroupLayout layout): layout(layout) {}

Hy3NodeData::Hy3NodeData(): Hy3NodeData((CWindow*)nullptr) {}

Hy3NodeData::Hy3NodeData(CWindow *window): type(Hy3NodeData::Window) {
	this->as_window = window;
}

Hy3NodeData::Hy3NodeData(Hy3GroupData group): type(Hy3NodeData::Group) {
	new(&this->as_group) Hy3GroupData(std::move(group));
}

Hy3NodeData::~Hy3NodeData() {
	switch (this->type) {
	case Hy3NodeData::Window:
		break;
	case Hy3NodeData::Group:
		this->as_group.~Hy3GroupData();

		// who ever thought calling the dtor after a move was a good idea?
		this->type = Hy3NodeData::Window;
		break;
	}
}

Hy3NodeData::Hy3NodeData(const Hy3NodeData& from): type(from.type) {
	Debug::log(LOG, "Copy CTor type matches? %d is group? %d", this->type == from.type, this->type == Hy3NodeData::Group);
	switch (from.type) {
	case Hy3NodeData::Window:
		this->as_window = from.as_window;
		break;
	case Hy3NodeData::Group:
		new(&this->as_group) Hy3GroupData(from.as_group);
		break;
	}
}

Hy3NodeData::Hy3NodeData(Hy3NodeData&& from): type(from.type) {
	Debug::log(LOG, "Move CTor type matches? %d is group? %d", this->type == from.type, this->type == Hy3NodeData::Group);
	switch (from.type) {
	case Hy3NodeData::Window:
		this->as_window = from.as_window;
		break;
	case Hy3NodeData::Group:
		new(&this->as_group) Hy3GroupData(std::move(from.as_group));
		break;
	}
}

Hy3NodeData& Hy3NodeData::operator=(const Hy3NodeData& from) {
	Debug::log(LOG, "operator= type matches? %d is group? %d", this->type == from.type, this->type == Hy3NodeData::Group);
	if (this->type == Hy3NodeData::Group) {
		this->as_group.~Hy3GroupData();
	}

	this->type = from.type;

	switch (this->type) {
	case Hy3NodeData::Window:
		this->as_window = from.as_window;
		break;
	case Hy3NodeData::Group:
		new(&this->as_group) Hy3GroupData(from.as_group);
		break;
	}

	return *this;
}

bool Hy3NodeData::operator==(const Hy3NodeData& rhs) const {
	if (this->type != rhs.type) return false;
	switch (this->type) {
	case Hy3NodeData::Window:
		return this->as_window == rhs.as_window;
	case Hy3NodeData::Group:
		return this->as_group.children == rhs.as_group.children;
	}

	return false;
}

bool Hy3Node::operator==(const Hy3Node& rhs) const {
	return this->data == rhs.data;
}

void Hy3Node::recalcSizePosRecursive(bool force) {
	if (this->data.type != Hy3NodeData::Group) {
		this->layout->applyNodeDataToWindow(this, force);
		return;
	}

	auto* group = &this->data.as_group;
	int constraint;
	switch (group->layout) {
	case Hy3GroupLayout::SplitH:
		constraint = this->size.x;
		break;
	case Hy3GroupLayout::SplitV:
		constraint = this->size.y;
		break;
	case Hy3GroupLayout::Tabbed:
		break;
	}

	double offset = 0;

	for(auto child: group->children) {
		switch (group->layout) {
		case Hy3GroupLayout::SplitH:
			child->position.x = this->position.x + offset;
			child->size.x = child->size_ratio * ((double) constraint / group->children.size());
			offset += child->size.x;
			child->position.y = this->position.y;
			child->size.y = this->size.y;
			break;
		case Hy3GroupLayout::SplitV:
			child->position.y = this->position.y + offset;
			child->size.y = child->size_ratio * ((double) constraint / group->children.size());
			offset += child->size.y;
			child->position.x = this->position.x;
			child->size.x = this->size.x;
			break;
		case Hy3GroupLayout::Tabbed:
			// TODO: tab bars
			child->position = this->position;
			child->size = this->size;
			break;
		}

		child->recalcSizePosRecursive(force);
	}
}

int Hy3Layout::getWorkspaceNodeCount(const int& id) {
	int count = 0;

	for (auto& node: this->nodes) {
		if (node.workspace_id == id && node.valid) count++;
	}

	return count;
}

Hy3Node* Hy3Layout::getNodeFromWindow(CWindow* window) {
	for (auto& node: this->nodes) {
		if (node.data.type == Hy3NodeData::Window && node.data.as_window == window) {
			return &node;
		}
	}

	return nullptr;
}

Hy3Node* Hy3Layout::getWorkspaceRootGroup(const int& id) {
	for (auto& node: this->nodes) {
		if (node.parent == nullptr && node.data.type == Hy3NodeData::Group) {
			return &node;
		}
	}

	return nullptr;
}

void Hy3Layout::applyNodeDataToWindow(Hy3Node* node, bool force) {
	if (node->data.type != Hy3NodeData::Window) return;
	CWindow* window = node->data.as_window;

	CMonitor* monitor = nullptr;

	if (g_pCompositor->isWorkspaceSpecial(node->workspace_id)) {
		for (auto& m: g_pCompositor->m_vMonitors) {
			if (m->specialWorkspaceID == node->workspace_id) {
				monitor = m.get();
				break;
			}
		}
	} else {
		monitor = g_pCompositor->getMonitorFromID(g_pCompositor->getWorkspaceByID(node->workspace_id)->m_iMonitorID);
	}

	if (monitor == nullptr) {
		Debug::log(ERR, "Orphaned Node %x (workspace ID: %i)!!", node, node->workspace_id);
		return;
	}

	// for gaps outer
	const bool display_left   = STICKS(node->position.x, monitor->vecPosition.x + monitor->vecReservedTopLeft.x);
	const bool display_right  = STICKS(node->position.x + node->size.x, monitor->vecPosition.x + monitor->vecSize.x - monitor->vecReservedBottomRight.x);
	const bool display_top    = STICKS(node->position.y, monitor->vecPosition.y + monitor->vecReservedTopLeft.y);
	const bool display_bottom = STICKS(node->position.y + node->size.y, monitor->vecPosition.y + monitor->vecSize.y - monitor->vecReservedBottomRight.y);

	const auto* border_size = &g_pConfigManager->getConfigValuePtr("general:border_size")->intValue;
	const auto* gaps_in     = &g_pConfigManager->getConfigValuePtr("general:gaps_in")->intValue;
	const auto* gaps_out    = &g_pConfigManager->getConfigValuePtr("general:gaps_out")->intValue;
	static auto* const single_window_no_gaps = &g_pConfigManager->getConfigValuePtr("plugin:hy3:no_gaps_when_only")->intValue;

	if (!g_pCompositor->windowExists(window) || !window->m_bIsMapped) {
		Debug::log(ERR, "Node %p holding invalid window %p!!", node, window);
		this->onWindowRemovedTiling(window);
		return;
	}

	window->m_vSize = node->size;
	window->m_vPosition = node->position;

	auto calcPos = window->m_vPosition + Vector2D(*border_size, *border_size);
	auto calcSize = window->m_vSize - Vector2D(2 * *border_size, 2 * *border_size);

	const auto workspace_node_count = this->getWorkspaceNodeCount(window->m_iWorkspaceID);

	if (*single_window_no_gaps
			&& !g_pCompositor->isWorkspaceSpecial(window->m_iWorkspaceID)
			&& (workspace_node_count == 1
					|| (window->m_bIsFullscreen
							&& g_pCompositor->getWorkspaceByID(window->m_iWorkspaceID)->m_efFullscreenMode == FULLSCREEN_MAXIMIZED)))
	{
		window->m_vRealPosition = window->m_vPosition;
		window->m_vRealSize = window->m_vSize;

		window->updateWindowDecos();

		window->m_sSpecialRenderData.rounding = false;
		window->m_sSpecialRenderData.border = false;
		window->m_sSpecialRenderData.decorate = false;
	} else {
		window->m_sSpecialRenderData.rounding = true;
		window->m_sSpecialRenderData.border = true;
		window->m_sSpecialRenderData.decorate = true;

		Vector2D offset_topleft(
			display_left ? *gaps_out : *gaps_in,
			display_top ? *gaps_out : *gaps_in
		);

		Vector2D offset_bottomright(
			display_right ? *gaps_out : *gaps_in,
			display_bottom ? *gaps_out : *gaps_in
		);

		calcPos = calcPos + offset_topleft;
		calcSize = calcSize - offset_topleft - offset_bottomright;

		const auto reserved_area = window->getFullWindowReservedArea();
		calcPos = calcPos + reserved_area.topLeft;
		calcSize = calcSize - (reserved_area.topLeft - reserved_area.bottomRight);

		window->m_vRealPosition = calcPos;
		window->m_vRealSize = calcSize;
		Debug::log(LOG, "Set size (%f %f)", calcSize.x, calcSize.y);

		g_pXWaylandManager->setWindowSize(window, calcSize);

		if (force) {
			g_pHyprRenderer->damageWindow(window);

			window->m_vRealPosition.warp();
			window->m_vRealPosition.warp();

			g_pHyprRenderer->damageWindow(window);
		}

		window->updateWindowDecos();
	}
}

void Hy3Layout::onWindowCreatedTiling(CWindow* window) {
	if (window->m_bIsFloating) return;

	auto* monitor = g_pCompositor->getMonitorFromID(window->m_iMonitorID);

	Hy3Node* opening_into;
	Hy3Node* opening_after;

	if (g_pCompositor->m_pLastWindow != nullptr
			&& !g_pCompositor->m_pLastWindow->m_bIsFloating
			&& g_pCompositor->m_pLastWindow != window
			&& g_pCompositor->m_pLastWindow->m_iWorkspaceID == window->m_iWorkspaceID
			&& g_pCompositor->m_pLastWindow->m_bIsMapped)
	{
		opening_after = this->getNodeFromWindow(g_pCompositor->m_pLastWindow);
	} else {
		opening_after = this->getNodeFromWindow(g_pCompositor->vectorToWindowTiled(g_pInputManager->getMouseCoordsInternal()));
	}

	if (opening_after != nullptr) {
		opening_into = opening_after->parent;
	} else {
		if ((opening_into = this->getWorkspaceRootGroup(window->m_iWorkspaceID)) == nullptr) {
			this->nodes.push_back({
				.data = Hy3NodeData(Hy3GroupData(Hy3GroupLayout::SplitH)),
				.position = monitor->vecPosition,
				.size = monitor->vecSize,
				.workspace_id = window->m_iWorkspaceID,
				.layout = this,
			});

			opening_into = &this->nodes.back();
		}
	}

	if (opening_into->data.type != Hy3NodeData::Group) {
		Debug::log(ERR, "opening_into node %p was not of type Group", opening_into);
		return;
	}

	this->nodes.push_back({
		.parent = opening_into,
		.data = Hy3NodeData(window),
		.workspace_id = window->m_iWorkspaceID,
		.layout = this,
	});

	auto& node = this->nodes.back();

	if (opening_after == nullptr) {
		opening_into->data.as_group.children.push_back(&node);
	} else {
		auto& children = opening_into->data.as_group.children;
		auto iter = std::find(children.begin(), children.end(), opening_after);
		auto iter2 = std::next(iter);
		children.insert(iter2, &node);
	}
	Debug::log(LOG, "open new window %p(node: %p:%p) on winodow %p in %p", window, &node, node.data.as_window, opening_after, opening_into);

	opening_into->recalcSizePosRecursive();
}

void Hy3Layout::onWindowRemovedTiling(CWindow* window) {
	auto* node = this->getNodeFromWindow(window);
	Debug::log(LOG, "remove tiling %p (window %p)", node, window);

	if (node == nullptr) {
		Debug::log(ERR, "onWindowRemovedTiling node null?");
		return;
	}

	window->m_sSpecialRenderData.rounding = true;
	window->m_sSpecialRenderData.border = true;
	window->m_sSpecialRenderData.decorate = true;

	if (window->m_bIsFullscreen) {
		g_pCompositor->setWindowFullscreen(window, false, FULLSCREEN_FULL);
	}

	auto* parent = node->parent;

	parent->data.as_group.children.remove(node);
	this->nodes.remove(*node);

	while (parent != nullptr && parent->data.as_group.children.empty()) {
		auto* child = parent;
		parent = parent->parent;

		if (parent != nullptr) parent->data.as_group.children.remove(child);
		this->nodes.remove(*child);
	}

	if (parent != nullptr) parent->recalcSizePosRecursive();
}

bool Hy3Layout::isWindowTiled(CWindow* window) {
	return this->getNodeFromWindow(window) != nullptr;
}

void Hy3Layout::recalculateMonitor(const int& eIdleInhibitMode) {
    ; // empty
}

void Hy3Layout::recalculateWindow(CWindow* pWindow) {
    ; // empty
}

void Hy3Layout::resizeActiveWindow(const Vector2D& delta, CWindow* pWindow) {
    ; // empty
}

void Hy3Layout::fullscreenRequestForWindow(CWindow* pWindow, eFullscreenMode mode, bool on) {
    ; // empty
}

std::any Hy3Layout::layoutMessage(SLayoutMessageHeader header, std::string content) {
	if (header.pWindow == nullptr) return "";
	auto* node = this->getNodeFromWindow(header.pWindow);
	if (node == nullptr) return "";

	if (content == "splith" || content == "splitv") {
		Hy3GroupLayout layout = Hy3GroupLayout::SplitH;
		if (content == "splitv") {
			layout = Hy3GroupLayout::SplitV;
		}

		Hy3NodeData node_data = Hy3NodeData(Hy3GroupData(layout));
		std::swap(node->data, node_data);

		this->nodes.push_back({
			.parent = node,
			.data = node_data,
			.workspace_id = node->workspace_id,
			.layout = this,
		});

		node->data.as_group.children.push_back(&this->nodes.back());
		node->recalcSizePosRecursive();
	}

	return "";
}

SWindowRenderLayoutHints Hy3Layout::requestRenderHints(CWindow* pWindow) {
    return {};
}

void Hy3Layout::switchWindows(CWindow* pWindowA, CWindow* pWindowB) {
    ; // empty
}

void Hy3Layout::alterSplitRatio(CWindow* pWindow, float delta, bool exact) {
    ; // empty
}

std::string Hy3Layout::getLayoutName() {
    return "custom";
}

void Hy3Layout::replaceWindowDataWith(CWindow* from, CWindow* to) {
    ; // empty
}

void Hy3Layout::onEnable() {
    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->isHidden() || !w->m_bIsMapped || w->m_bFadingOut || w->m_bIsFloating)
            continue;

        this->onWindowCreatedTiling(w.get());
    }
}

void Hy3Layout::onDisable() {
}
