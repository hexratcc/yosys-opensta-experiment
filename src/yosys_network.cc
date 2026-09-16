// SPDX-License-Identifier: GPL-3.0-or-later
#include "yosys_network.hh"

#include "kernel/newcelltypes.h"

#include "sta/Liberty.hh"
#include "sta/PatternMatch.hh"
#include "sta/PortDirection.hh"
#include "sta/Report.hh"

namespace ysta {

using namespace Yosys;
using sta::ObjectId;

namespace detail {

// Simple Iterator<T> over a copied sequence.
template <typename T> class SeqIterator : public sta::Iterator<T> {
public:
	explicit SeqIterator(std::vector<T> seq) : seq_(std::move(seq)) {}
	bool hasNext() override { return index_ < seq_.size(); }
	T next() override { return seq_[index_++]; }

private:
	std::vector<T> seq_;
	size_t index_ = 0;
};

// STA-namespace name: hierarchy dividers, brackets and backslashes embedded
// in a name are escaped (the reference readers store names this way; the
// SdcNetwork wrapper depends on it to resolve names like esc/inst or a[3]).
std::string sta_name(RTLIL::IdString id)
{
	std::string raw = RTLIL::unescape_id(id);
	std::string result;
	result.reserve(raw.size());
	for (char c : raw) {
		if (c == '/' || c == '[' || c == ']' || c == '\\')
			result += '\\';
		result += c;
	}
	return result;
}

// Inverse of sta_name's escaping.
std::string sta_unescape(std::string_view name)
{
	std::string result;
	result.reserve(name.size());
	for (size_t i = 0; i < name.size(); i++) {
		if (name[i] == '\\' && i + 1 < name.size())
			i++;
		result += name[i];
	}
	return result;
}

std::string bit_name(RTLIL::Wire *wire, int bit)
{
	if (wire->width == 1)
		return sta_name(wire->name);
	return sta_name(wire->name) + "[" + std::to_string(wire->to_hdl_index(bit)) + "]";
}

sta::PortDirection *wire_direction(RTLIL::Wire *wire)
{
	if (wire->port_input && wire->port_output)
		return sta::PortDirection::bidirect();
	if (wire->port_input)
		return sta::PortDirection::input();
	if (wire->port_output)
		return sta::PortDirection::output();
	return sta::PortDirection::unknown();
}

// RAII flag around NetworkEdit calls, see YosysNetwork::inNetworkEdit.
struct detail::EditScope {
	explicit EditScope(bool &flag) : flag_(flag), saved_(flag) { flag_ = true; }
	~EditScope() { flag_ = saved_; }
	bool &flag_;
	bool saved_;
};

} // namespace detail

YosysNetwork::YosysNetwork() : top_instance_sentinel_(reinterpret_cast<sta::Instance *>(1))
{
}

const YosysPin *YosysNetwork::asPin(const sta::Pin *pin)
{
	return reinterpret_cast<const YosysPin *>(pin);
}

const YosysNet *YosysNetwork::asNet(const sta::Net *net)
{
	return reinterpret_cast<const YosysNet *>(net);
}

const YosysTerm *YosysNetwork::asTerm(const sta::Term *term)
{
	return reinterpret_cast<const YosysTerm *>(term);
}

bool YosysNetwork::linkNetwork(std::string_view, bool, sta::Report *report)
{
	if (report != nullptr)
		report->warn(2915, "the network is managed by yosys; link_design only cleared "
		                   "constraints and timing, use `opensta_net -link` to rebuild.");
	return true;
}

bool YosysNetwork::designChanged(RTLIL::Design *design) const
{
	return design != view_.design() || view_.changed();
}

bool YosysNetwork::isLinked() const
{
	// Refusing to run beats reporting plausible numbers on a stale view; the
	// full comparison also catches mutations no callback reports. It is O(design)
	// and Sta::ensureLinked runs it per Tcl command, hence the memo in the view.
	return view_.valid();
}

////////////////////////////////////////////////////////////////
// Build

void YosysNetwork::buildFromDesign(RTLIL::Design *design)
{
	log_assert(!view_.built()); // caller must reset() before relinking
	if (design_lib_ != nullptr)
		deleteLibrary(design_lib_); // left by a build that errored out
	RTLIL::Module *top = design->top_module();
	if (top == nullptr)
		log_error("opensta_net: no top module (run hierarchy -top).\n");
	design_ = design;

	auto t0 = std::chrono::steady_clock::now();

	// Named like OpenSTA's own verilog reader library for output parity.
	design_lib_ = makeLibrary("verilog", "");
	ytop_cell_ = makeModuleCell(top);
	view_.build(design, this); // calls ports() for every leaf cell type
	indexPins(nullptr);
	buildTables(top);
	log_assert(pin_ports_.data.size() < 0x40000000); // ObjectId headroom

	auto t1 = std::chrono::steady_clock::now();
	logCounts(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

void YosysNetwork::logCounts(long ms) const
{
	size_t instances = 0, nets = 0, pins = view_.pins(nullptr).size();
	for (const auto &[module, children] : child_map_) {
		instances += children.size();
		nets += view_.nets(module).size();
		for (const auto &[name, cell] : children)
			pins += view_.pins(cell).size();
	}
	log("opensta_net: built network: %zu instances, %zu nets, %zu pins (%ld ms).\n", instances, nets, pins, ms);
}

void YosysNetwork::reset()
{
	view_.reset();
	if (design_lib_ != nullptr)
		deleteLibrary(design_lib_);
	design_lib_ = nullptr;
	ytop_cell_ = nullptr;
	design_ = nullptr;

	module_cell_.clear();
	type_cell_map_.clear(); // stub cells died with design_lib_
	cell_ports_.clear();
	pin_ports_.clear();
	vertex_ids_.clear();
	child_map_.clear();
	net_names_.clear();
	pin_map_.clear();
	recording_ = false;
	recorded_modules_.clear();
	clearNetDrvrPinMap();
}

// Leaf cell ports for the view, in liberty (or stub) declaration order.
bool YosysNetwork::ports(const RTLIL::Cell *cell, std::vector<NetView::PortShape> &out)
{
	sta::ConcreteCell *ccell = staCellForType(cell->type);
	sta::CellPortIterator *port_iter = portIterator(reinterpret_cast<sta::Cell *>(ccell));
	while (port_iter->hasNext()) {
		sta::Port *port = port_iter->next();
		NetView::PortShape shape;
		// Module cell port names are STA-escaped; liberty names are raw.
		shape.name = RTLIL::escape_id(detail::sta_unescape(name(port)));
		shape.width = size(port);
		shape.from = hasMembers(port) ? fromIndex(port) : 0;
		shape.to = hasMembers(port) ? toIndex(port) : 0;
		sta::PortDirection *dir = direction(port);
		shape.dir = RTLIL::PortDir(dir->isAnyInput() + dir->isAnyOutput() * 2);
		out.push_back(shape);
	}
	delete port_iter;
	return true;
}

// Per-module STA tables over the view: child names, pin names and ports,
// net names. Recurses into the unfolded hierarchy.
void YosysNetwork::buildTables(RTLIL::Module *module)
{
	auto &children = child_map_[module];
	for (RTLIL::Cell *cell : module->cells()) {
		children[detail::sta_name(cell->name)] = cell;
		RTLIL::Module *sub = view_.childModule(cell);
		if (sub != nullptr)
			makeModuleCell(sub); // the view built its pins from the module ports
		indexPins(cell);
	}
	indexNetNames(module);
	for (RTLIL::Cell *cell : module->cells()) {
		RTLIL::Module *sub = view_.childModule(cell);
		if (sub != nullptr)
			buildTables(sub);
	}
}

// Bus-bit port for a pin. RTLIL bit 0 is the LSB; OpenSTA's readers pair
// bus members from `from` to `to` with net bits MSB-first, so the LSB sits
// at index `to` whichever way the bus is declared (verified against the
// reference binary for both directions). The view computes hdl_index by
// that rule for liberty and module ports alike.
sta::ConcretePort *YosysNetwork::bitPort(sta::ConcretePort *port, const YosysPin *pin) const
{
	sta::Port *p = reinterpret_cast<sta::Port *>(port);
	if (!hasMembers(p))
		return port;
	sta::Port *member = findBusBit(p, pin->hdl_index);
	if (member == nullptr)
		report_->error(2916, "bus bit {} out of range on port {}.", pin->bit, name(p));
	return reinterpret_cast<sta::ConcretePort *>(member);
}

sta::ConcreteCell *YosysNetwork::staCellOf(const RTLIL::Cell *cell) const
{
	if (cell == nullptr)
		return ytop_cell_;
	RTLIL::Module *sub = view_.childModule(cell);
	if (sub != nullptr)
		return module_cell_.at(sub);
	return lookupCellForType(cell->type);
}

sta::ConcretePort *YosysNetwork::portOf(const YosysPin *pin) const
{
	sta::ConcreteCell *ccell = staCellOf(pin->cell);
	sta::ConcretePort *port = detail::find2(cell_ports_, ccell, pin->port);
	if (port == nullptr)
		report_->error(2903, "port {} missing on {}.", log_id(pin->port), name(reinterpret_cast<sta::Cell *>(ccell)));
	return bitPort(port, pin);
}

// Bind the STA port objects and port-bit names of one instance's pins
// (cell == nullptr: the top ports).
void YosysNetwork::indexPins(const RTLIL::Cell *cell)
{
	auto &names = pin_map_[cell];
	names.clear();
	for (YosysPin *pin : view_.pins(cell)) {
		sta::ConcretePort *cport = portOf(pin);
		pin_ports_[pin->id] = cport;
		names[name(reinterpret_cast<sta::Port *>(cport))] = pin;
	}
}

std::string YosysNetwork::netName(const YosysNet *net) const
{
	if (net->bit.wire == nullptr)
		return net->bit.data == RTLIL::State::S1 ? "$sta_const1" : "$sta_const0";
	return detail::bit_name(net->bit.wire, net->bit.offset);
}

// Canonical names first, then every public wire bit as an alias (the
// reference readers keep alias nets as separate connected Net objects;
// here they resolve to the canonical net).
void YosysNetwork::indexNetNames(RTLIL::Module *module)
{
	auto &names = net_names_[module];
	for (YosysNet *net : view_.nets(module))
		names[netName(net)] = net;
	for (const NetView::Alias &alias : view_.aliases(module))
		names.emplace(detail::bit_name(alias.wire, alias.bit), alias.net);
}

void YosysNetwork::indexNet(YosysNet *net)
{
	net_names_[net->module][netName(net)] = net;
}

// Build/edit path: resolves a cell type, creating a stub when needed.
sta::ConcreteCell *YosysNetwork::staCellForType(RTLIL::IdString type)
{
	auto it = type_cell_map_.find(type);
	if (it != type_cell_map_.end())
		return it->second;

	std::string name = RTLIL::unescape_id(type);
	sta::LibertyCell *lcell = findLibertyCell(name.c_str());
	sta::ConcreteCell *ccell;
	if (lcell != nullptr) {
		ccell = reinterpret_cast<sta::ConcreteCell *>(cell(lcell));
	} else {
		// A yosys internal cell has no RTLIL module, so its stub would have
		// no ports and its logic would silently vanish from timing.
		if (yosys_celltypes.cell_known(type) && type != ID($scopeinfo))
			log_error("opensta_net: internal cell type %s has no liberty cell; map the design "
			          "first (techmap/dfflibmap/abc, or icell_liberty).\n", name.c_str());
		ccell = makeStubCell(type);
		log_warning("opensta_net: no liberty cell for type %s; made a stub cell. "
		            "Its instances contribute no timing arcs and paths through them break "
		            "(fine for physical-only cells, wrong for unmapped macros).\n", name.c_str());
	}
	type_cell_map_[type] = ccell;
	indexPorts(ccell);
	return ccell;
}

// Query path: a pure lookup, safe to call from delay-calc worker threads.
sta::ConcreteCell *YosysNetwork::lookupCellForType(RTLIL::IdString type) const
{
	auto it = type_cell_map_.find(type);
	if (it == type_cell_map_.end())
		report_->error(2917, "cell type {} was not resolved at link time.", RTLIL::unescape_id(type));
	return it->second;
}

// Stub cell for design cell types absent from liberty (e.g. physical-only
// cells). Ports come from the RTLIL blackbox module when one exists.
sta::ConcreteCell *YosysNetwork::makeStubCell(RTLIL::IdString type)
{
	sta::Cell *scell = makeCell(design_lib_, RTLIL::unescape_id(type), true, "");
	RTLIL::Module *mod = design_->module(type);
	if (mod != nullptr) {
		for (RTLIL::IdString port_name : mod->ports)
			makeWirePort(scell, mod->wire(port_name), detail::sta_name(port_name));
	}
	return reinterpret_cast<sta::ConcreteCell *>(scell);
}

sta::Port *YosysNetwork::makeWirePort(sta::Cell *scell, RTLIL::Wire *wire, const std::string &name)
{
	sta::Port *port;
	if (wire->width == 1) {
		port = makePort(scell, name);
	} else {
		// `to` is the HDL index of RTLIL bit 0 (the LSB), as bitPort assumes.
		int from = wire->to_hdl_index(wire->width - 1);
		int to = wire->to_hdl_index(0);
		port = makeBusPort(scell, name, from, to);
	}
	setDirection(port, detail::wire_direction(wire));
	return port;
}

// STA cell for a module (hierarchical instance master or the top module).
// The display name is the pre-uniquification name when uniquify recorded one.
sta::ConcreteCell *YosysNetwork::makeModuleCell(RTLIL::Module *module)
{
	auto it = module_cell_.find(module);
	if (it != module_cell_.end())
		return it->second;

	std::string cell_name;
	if (module->has_attribute(ID::hdlname))
		cell_name = module->get_string_attribute(ID::hdlname);
	else
		cell_name = RTLIL::unescape_id(module->name);

	sta::Cell *scell = makeCell(design_lib_, cell_name, false, "");
	for (RTLIL::IdString port_name : module->ports)
		makeWirePort(scell, module->wire(port_name), detail::sta_name(port_name));

	sta::ConcreteCell *ccell = reinterpret_cast<sta::ConcreteCell *>(scell);
	module_cell_[module] = ccell;
	indexPorts(ccell);
	return ccell;
}

// Port IdString -> port object, so pins resolve their STA port in O(1).
void YosysNetwork::indexPorts(sta::ConcreteCell *ccell)
{
	auto &ports = cell_ports_[ccell];
	sta::CellPortIterator *port_iter = portIterator(reinterpret_cast<sta::Cell *>(ccell));
	while (port_iter->hasNext()) {
		sta::Port *port = port_iter->next();
		ports[RTLIL::escape_id(detail::sta_unescape(name(port)))] = reinterpret_cast<sta::ConcretePort *>(port);
	}
	delete port_iter;
}

// Module whose contents the instance holds; nullptr for leaf instances.
RTLIL::Module *YosysNetwork::moduleOf(const sta::Instance *instance) const
{
	if (instance == top_instance_sentinel_)
		return view_.top();
	return view_.childModule(reinterpret_cast<const RTLIL::Cell *>(instance));
}

////////////////////////////////////////////////////////////////
// keep_hierarchy recording

void YosysNetwork::beginRecording()
{
	recording_ = true;
	recorded_modules_.clear();
}

pool<RTLIL::Module *> YosysNetwork::endRecording()
{
	recording_ = false;
	return std::move(recorded_modules_);
}

// Record the module containing a referenced object, plus all ancestors.
void YosysNetwork::recordModule(RTLIL::Module *module) const
{
	std::lock_guard<std::mutex> lock(record_mutex_);
	while (module != nullptr && module != view_.top()) {
		if (!recorded_modules_.insert(module).second)
			return; // ancestors already recorded
		RTLIL::Cell *parent = view_.parentCell(module);
		module = parent != nullptr ? parent->module : nullptr;
	}
}

void YosysNetwork::recordInstance(const sta::Instance *instance) const
{
	if (!recording_ || instance == nullptr || instance == top_instance_sentinel_)
		return;
	const RTLIL::Cell *rcell = reinterpret_cast<const RTLIL::Cell *>(instance);
	// A constrained hierarchical instance needs its OWN boundary kept, not
	// just its parent's, or flatten dissolves the instance the SDC names.
	RTLIL::Module *sub = view_.childModule(rcell);
	recordModule(sub != nullptr ? sub : rcell->module);
}

void YosysNetwork::recordPin(const sta::Pin *pin) const
{
	if (!recording_ || pin == nullptr)
		return;
	const YosysPin *ypin = asPin(pin);
	if (ypin->cell != nullptr)
		recordModule(ypin->cell->module);
}

////////////////////////////////////////////////////////////////
// Instance

sta::Instance *YosysNetwork::topInstance() const
{
	if (!view_.built())
		return nullptr;
	return top_instance_sentinel_;
}

std::string YosysNetwork::name(const sta::Instance *instance) const
{
	if (instance == top_instance_sentinel_)
		return RTLIL::unescape_id(view_.top()->name);
	return detail::sta_name(reinterpret_cast<const RTLIL::Cell *>(instance)->name);
}

ObjectId YosysNetwork::id(const sta::Instance *instance) const
{
	if (instance == top_instance_sentinel_)
		return 0;
	return view_.cellId(reinterpret_cast<const RTLIL::Cell *>(instance));
}

std::string YosysNetwork::getAttribute(const sta::Instance *, std::string_view) const
{
	return "";
}

const sta::AttributeMap &YosysNetwork::attributeMap(const sta::Instance *) const
{
	static const sta::AttributeMap empty;
	return empty;
}

sta::Cell *YosysNetwork::cell(const sta::Instance *instance) const
{
	if (instance == top_instance_sentinel_)
		return reinterpret_cast<sta::Cell *>(ytop_cell_);
	return reinterpret_cast<sta::Cell *>(staCellOf(reinterpret_cast<const RTLIL::Cell *>(instance)));
}

sta::Instance *YosysNetwork::parent(const sta::Instance *instance) const
{
	if (instance == top_instance_sentinel_)
		return nullptr;
	const RTLIL::Cell *rcell = reinterpret_cast<const RTLIL::Cell *>(instance);
	if (rcell->module == view_.top())
		return top_instance_sentinel_;
	return reinterpret_cast<sta::Instance *>(view_.parentCell(rcell->module));
}

bool YosysNetwork::isLeaf(const sta::Instance *instance) const
{
	if (instance == top_instance_sentinel_)
		return false;
	return view_.childModule(reinterpret_cast<const RTLIL::Cell *>(instance)) == nullptr;
}

sta::Instance *YosysNetwork::findChild(const sta::Instance *parent, std::string_view name) const
{
	if (view_.stale())
		return nullptr;
	sta::Instance *child = reinterpret_cast<sta::Instance *>(detail::find2(child_map_, moduleOf(parent), name));
	recordInstance(child);
	return child;
}

sta::Pin *YosysNetwork::findPin(const sta::Instance *instance, std::string_view port_name) const
{
	if (view_.stale())
		return nullptr;
	const RTLIL::Cell *key = nullptr;
	if (instance != top_instance_sentinel_)
		key = reinterpret_cast<const RTLIL::Cell *>(instance);
	sta::Pin *pin = reinterpret_cast<sta::Pin *>(detail::find2(pin_map_, key, port_name));
	recordPin(pin);
	return pin;
}

sta::Pin *YosysNetwork::findPin(const sta::Instance *instance, const sta::Port *port) const
{
	return findPin(instance, name(port));
}

sta::InstanceChildIterator *YosysNetwork::childIterator(const sta::Instance *instance) const
{
	std::vector<sta::Instance *> children;
	if (!view_.stale()) {
		auto it = child_map_.find(moduleOf(instance));
		if (it != child_map_.end()) {
			for (const auto &[name, cell] : it->second)
				children.push_back(reinterpret_cast<sta::Instance *>(cell));
		}
	}
	return new detail::SeqIterator<sta::Instance *>(std::move(children));
}

sta::InstancePinIterator *YosysNetwork::pinIterator(const sta::Instance *instance) const
{
	std::vector<sta::Pin *> pins;
	if (!view_.stale()) {
		const RTLIL::Cell *key = nullptr;
		if (instance != top_instance_sentinel_)
			key = reinterpret_cast<const RTLIL::Cell *>(instance);
		for (YosysPin *pin : view_.pins(key))
			pins.push_back(reinterpret_cast<sta::Pin *>(pin));
	}
	return new detail::SeqIterator<sta::Pin *>(std::move(pins));
}

sta::InstanceNetIterator *YosysNetwork::netIterator(const sta::Instance *instance) const
{
	std::vector<sta::Net *> nets;
	if (!view_.stale()) {
		auto it = net_names_.find(moduleOf(instance));
		if (it != net_names_.end()) {
			for (const auto &[name, net] : it->second) {
				if (name != netName(net))
					continue; // alias entry; list the canonical net once
				nets.push_back(reinterpret_cast<sta::Net *>(net));
			}
		}
	}
	return new detail::SeqIterator<sta::Net *>(std::move(nets));
}

sta::InstanceSeq YosysNetwork::findInstancesMatching(const sta::Instance *context,
                                                     const sta::PatternMatch *pattern) const
{
	sta::InstanceSeq matches = sta::Network::findInstancesMatching(context, pattern);
	for (const sta::Instance *instance : matches)
		recordInstance(instance);
	return matches;
}

sta::InstanceSeq YosysNetwork::findInstancesHierMatching(const sta::Instance *instance,
                                                         const sta::PatternMatch *pattern) const
{
	sta::InstanceSeq matches = sta::Network::findInstancesHierMatching(instance, pattern);
	for (const sta::Instance *match : matches)
		recordInstance(match);
	return matches;
}

sta::PinSeq YosysNetwork::findPinsMatching(const sta::Instance *instance,
                                           const sta::PatternMatch *pattern) const
{
	sta::PinSeq matches = sta::Network::findPinsMatching(instance, pattern);
	for (const sta::Pin *pin : matches)
		recordPin(pin);
	return matches;
}

sta::PinSeq YosysNetwork::findPinsHierMatching(const sta::Instance *instance,
                                               const sta::PatternMatch *pattern) const
{
	sta::PinSeq matches = sta::Network::findPinsHierMatching(instance, pattern);
	for (const sta::Pin *pin : matches)
		recordPin(pin);
	return matches;
}

////////////////////////////////////////////////////////////////
// Pin

ObjectId YosysNetwork::id(const sta::Pin *pin) const
{
	return asPin(pin)->id;
}

sta::Instance *YosysNetwork::instance(const sta::Pin *pin) const
{
	const YosysPin *ypin = asPin(pin);
	log_assert(!ypin->dead);
	if (ypin->cell == nullptr)
		return top_instance_sentinel_;
	return reinterpret_cast<sta::Instance *>(ypin->cell);
}

sta::Net *YosysNetwork::net(const sta::Pin *pin) const
{
	return reinterpret_cast<sta::Net *>(asPin(pin)->net);
}

sta::Term *YosysNetwork::term(const sta::Pin *pin) const
{
	return reinterpret_cast<sta::Term *>(asPin(pin)->term);
}

sta::Port *YosysNetwork::port(const sta::Pin *pin) const
{
	return reinterpret_cast<sta::Port *>(pin_ports_.get(asPin(pin)->id));
}

sta::PortDirection *YosysNetwork::direction(const sta::Pin *pin) const
{
	return direction(port(pin));
}

sta::VertexId YosysNetwork::vertexId(const sta::Pin *pin) const
{
	return vertex_ids_.get(asPin(pin)->id);
}

void YosysNetwork::setVertexId(sta::Pin *pin, sta::VertexId id)
{
	vertex_ids_[asPin(pin)->id] = id;
}

////////////////////////////////////////////////////////////////
// Term

ObjectId YosysNetwork::id(const sta::Term *term) const
{
	return asTerm(term)->id;
}

sta::Net *YosysNetwork::net(const sta::Term *term) const
{
	return reinterpret_cast<sta::Net *>(asTerm(term)->net);
}

sta::Pin *YosysNetwork::pin(const sta::Term *term) const
{
	return reinterpret_cast<sta::Pin *>(asTerm(term)->pin);
}

////////////////////////////////////////////////////////////////
// Net

std::string YosysNetwork::name(const sta::Net *net) const
{
	return netName(asNet(net));
}

ObjectId YosysNetwork::id(const sta::Net *net) const
{
	return asNet(net)->id;
}

sta::Net *YosysNetwork::findNet(const sta::Instance *instance, std::string_view net_name) const
{
	if (view_.stale())
		return nullptr;
	YosysNet *net = detail::find2(net_names_, moduleOf(instance), net_name);
	if (net != nullptr && recording_)
		recordModule(net->module);
	return reinterpret_cast<sta::Net *>(net);
}

void YosysNetwork::findInstNetsMatching(const sta::Instance *instance, const sta::PatternMatch *pattern,
                                        sta::NetSeq &matches) const
{
	auto map_it = net_names_.find(moduleOf(instance));
	if (map_it == net_names_.end())
		return;
	std::set<const YosysNet *> seen;
	for (const auto &[name, net] : map_it->second) {
		if (pattern->match(name) && seen.insert(net).second) {
			if (recording_)
				recordModule(net->module);
			matches.push_back(reinterpret_cast<sta::Net *>(net));
		}
	}
}

sta::Instance *YosysNetwork::instance(const sta::Net *net) const
{
	const YosysNet *ynet = asNet(net);
	if (ynet->module == view_.top())
		return top_instance_sentinel_;
	return reinterpret_cast<sta::Instance *>(view_.parentCell(ynet->module));
}

bool YosysNetwork::isPower(const sta::Net *) const
{
	return false;
}

bool YosysNetwork::isGround(const sta::Net *) const
{
	return false;
}

sta::NetPinIterator *YosysNetwork::pinIterator(const sta::Net *net) const
{
	std::vector<const sta::Pin *> pins;
	for (YosysPin *pin : asNet(net)->pins)
		pins.push_back(reinterpret_cast<const sta::Pin *>(pin));
	return new detail::SeqIterator<const sta::Pin *>(std::move(pins));
}

sta::NetTermIterator *YosysNetwork::termIterator(const sta::Net *net) const
{
	std::vector<sta::Term *> terms;
	for (YosysTerm *term : asNet(net)->terms)
		terms.push_back(reinterpret_cast<sta::Term *>(term));
	return new detail::SeqIterator<sta::Term *>(std::move(terms));
}

void YosysNetwork::visitConnectedPins(const sta::Net *net, sta::PinVisitor &visitor,
                                      sta::NetSet &visited_nets) const
{
	if (visited_nets.contains(net))
		return;
	visited_nets.insert(net);
	const YosysNet *ynet = asNet(net);
	// Same shape as ConcreteNetwork: a boundary pin is visited by the net on
	// its outer side, or directly when it has none (top ports).
	for (YosysTerm *term : ynet->terms) {
		YosysPin *above = term->pin;
		if (above->net != nullptr)
			visitConnectedPins(reinterpret_cast<sta::Net *>(above->net), visitor, visited_nets);
		else
			visitor(reinterpret_cast<sta::Pin *>(above));
	}
	for (YosysPin *pin : ynet->pins) {
		visitor(reinterpret_cast<sta::Pin *>(pin));
		if (pin->term != nullptr)
			visitConnectedPins(reinterpret_cast<sta::Net *>(pin->term->net), visitor, visited_nets);
	}
}

sta::ConstantPinIterator *YosysNetwork::constantPinIterator()
{
	sta::NetSet zero_nets(this);
	sta::NetSet one_nets(this);
	for (const auto &[module, ccell] : module_cell_) {
		if (YosysNet *net = view_.constNet(module, false); net != nullptr)
			zero_nets.insert(reinterpret_cast<sta::Net *>(net));
		if (YosysNet *net = view_.constNet(module, true); net != nullptr)
			one_nets.insert(reinterpret_cast<sta::Net *>(net));
	}
	return new sta::NetworkConstantPinIterator(this, zero_nets, one_nets);
}

////////////////////////////////////////////////////////////////
// NetworkEdit

sta::Instance *YosysNetwork::makeInstance(sta::LibertyCell *lcell, std::string_view name,
                                          sta::Instance *parent)
{
	RTLIL::Module *module = moduleOf(parent);
	if (module == nullptr)
		report_->error(2901, "makeInstance under a leaf instance.");
	RTLIL::IdString cell_name = RTLIL::escape_id(detail::sta_unescape(name));
	RTLIL::IdString type = RTLIL::escape_id(this->name(cell(lcell)));
	// Bind the given cell, not whatever findLibertyCell returns for the name.
	if (!type_cell_map_.count(type)) {
		type_cell_map_[type] = reinterpret_cast<sta::ConcreteCell *>(cell(lcell));
		indexPorts(type_cell_map_.at(type));
	}

	detail::EditScope scope(in_network_edit_);
	RTLIL::Cell *rcell = view_.addCell(module, cell_name, type);
	if (rcell == nullptr)
		report_->error(2920, "makeInstance: {} exists or {} is hierarchical.", name, log_id(type));
	child_map_[module][detail::sta_name(rcell->name)] = rcell;
	indexPins(rcell);
	return reinterpret_cast<sta::Instance *>(rcell);
}

void YosysNetwork::makePins(sta::Instance *)
{
	// Pins are created by makeInstance.
}

void YosysNetwork::replaceCell(sta::Instance *inst, sta::Cell *to_cell)
{
	if (inst == top_instance_sentinel_)
		report_->error(2902, "replaceCell on the top instance.");
	RTLIL::Cell *rcell = reinterpret_cast<RTLIL::Cell *>(inst);
	sta::ConcreteCell *ccell = reinterpret_cast<sta::ConcreteCell *>(to_cell);
	if (!cell_ports_.count(ccell))
		indexPorts(ccell);
	// Validate before mutating: the replace_cell Tcl command checks port
	// equivalence, C++ callers must do so themselves.
	for (YosysPin *pin : view_.pins(rcell)) {
		if (detail::find2(cell_ports_, ccell, pin->port) == nullptr)
			report_->error(2903, "replaceCell: port {} missing on {}.", log_id(pin->port), name(to_cell));
	}

	detail::EditScope scope(in_network_edit_);
	RTLIL::IdString type = RTLIL::escape_id(name(to_cell));
	if (!view_.retype(rcell, type))
		report_->error(2922, "replaceCell: {} is not a leaf cell type.", name(to_cell));
	type_cell_map_[type] = ccell;
	indexPins(rcell);
}

void YosysNetwork::deleteInstance(sta::Instance *inst)
{
	if (inst == top_instance_sentinel_ || !isLeaf(inst))
		report_->error(2904, "deleteInstance only supports leaf instances.");
	RTLIL::Cell *rcell = reinterpret_cast<RTLIL::Cell *>(inst);
	detail::EditScope scope(in_network_edit_);
	child_map_[rcell->module].erase(detail::sta_name(rcell->name));
	pin_map_.erase(rcell);
	view_.removeCell(rcell); // pin records outlive the instance, flagged dead
	clearNetDrvrPinMap();
}

sta::Pin *YosysNetwork::connect(sta::Instance *inst, sta::Port *port, sta::Net *net)
{
	if (inst == top_instance_sentinel_)
		report_->error(2905, "connect on the top instance is not supported.");
	if (hasMembers(port))
		report_->error(2906, "connect on bus port {} is not supported; connect its bits.", name(port));
	RTLIL::Cell *rcell = reinterpret_cast<RTLIL::Cell *>(inst);
	YosysNet *ynet = reinterpret_cast<YosysNet *>(net);
	if (ynet->module != rcell->module)
		report_->error(2917, "connect: net {} is not in the instance's module.", name(net));
	YosysPin *pin = detail::find2(pin_map_, static_cast<const RTLIL::Cell *>(rcell), name(port));
	if (pin == nullptr)
		report_->error(2918, "connect: no pin for port {} on {}.", name(port), name(inst));
	// A reconnect is disconnect + connect; YosysSta::connectPin fires the
	// disconnect hook first, the network only expects an unconnected pin.
	if (pin->net != nullptr)
		report_->error(2919, "connect: pin {} is already connected.", name(port));

	detail::EditScope scope(in_network_edit_);
	view_.connect(pin, ynet);
	clearNetDrvrPinMap();
	return reinterpret_cast<sta::Pin *>(pin);
}

sta::Pin *YosysNetwork::connect(sta::Instance *inst, sta::LibertyPort *lport, sta::Net *net)
{
	sta::Port *port = findPort(cell(inst), lport->name());
	if (port == nullptr)
		report_->error(2907, "connect: no port {}.", lport->name());
	return connect(inst, port, net);
}

void YosysNetwork::disconnectPin(sta::Pin *pin)
{
	YosysPin *ypin = reinterpret_cast<YosysPin *>(pin);
	if (ypin->cell == nullptr)
		report_->error(2908, "disconnectPin on a top port is not supported.");
	detail::EditScope scope(in_network_edit_);
	view_.disconnect(ypin);
	clearNetDrvrPinMap();
}

void YosysNetwork::deletePin(sta::Pin *)
{
	report_->error(2910, "deletePin is not supported.");
}

sta::Net *YosysNetwork::makeNet(std::string_view name, sta::Instance *parent)
{
	RTLIL::Module *module = moduleOf(parent);
	if (module == nullptr)
		report_->error(2911, "makeNet under a leaf instance.");
	detail::EditScope scope(in_network_edit_);
	YosysNet *net = view_.addNet(module, RTLIL::escape_id(detail::sta_unescape(name)));
	if (net == nullptr)
		report_->error(2921, "makeNet: name {} is taken.", name);
	indexNet(net);
	return reinterpret_cast<sta::Net *>(net);
}

void YosysNetwork::deleteNet(sta::Net *net)
{
	YosysNet *ynet = reinterpret_cast<YosysNet *>(net);
	if (ynet->bit.wire == nullptr)
		report_->error(2912, "deleteNet on a constant net.");
	// A net with boundary terms is a child port: RTLIL could not follow.
	if (!ynet->terms.empty())
		report_->error(2923, "deleteNet: net {} crosses a hierarchy boundary.", name(net));
	auto &names = net_names_[ynet->module];
	for (auto it = names.begin(); it != names.end();) {
		if (it->second == ynet)
			it = names.erase(it);
		else
			++it;
	}
	// Disconnects the net's pins; the RTLIL wire stays behind unconnected
	// (spike simplification).
	detail::EditScope scope(in_network_edit_);
	log_assert(view_.removeNet(ynet));
	clearNetDrvrPinMap();
}

void YosysNetwork::mergeInto(sta::Net *, sta::Net *)
{
	report_->error(2913, "mergeInto is not supported.");
}

sta::Net *YosysNetwork::mergedInto(sta::Net *)
{
	report_->error(2914, "mergedInto is not supported.");
	return nullptr;
}

} // namespace ysta
