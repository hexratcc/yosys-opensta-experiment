// SPDX-License-Identifier: GPL-3.0-or-later
// YosysNetwork: OpenSTA Network adapter over RTLIL (spike).
//
// The netlist itself comes from Yosys's NetView (kernel/netview.h): pins,
// nets, terms, ids, the unfolded hierarchy, edits and staleness tracking.
// This class adds what is OpenSTA's business: STA-namespace names, liberty
// binding and stub cells, bus-bit port objects, SDC query recording, and
// the NetworkEdit contract.
//
// Handle mapping:
//   Instance* = RTLIL::Cell* (leaf or hierarchical); top = sentinel pointer.
//   Pin*      = NetView::Pin* (materialized, pointer-stable; `user` = VertexId).
//   Net*      = NetView::Net* (one per canonical SigBit of its module).
//   Term*     = NetView::Term* (boundary: top ports and hierarchical pins).
//   Cell*/Port* = ConcreteCell/ConcretePort (LibertyCell for leaf types,
//               adapter-made ConcreteCells for modules) — library half is
//               inherited from ConcreteNetwork.

#pragma once

#include "kernel/yosys.h"
#include "kernel/netview.h"

#include "sta/ConcreteNetwork.hh"

namespace ysta {

using YosysPin = Yosys::NetView::Pin;
using YosysNet = Yosys::NetView::Net;
using YosysTerm = Yosys::NetView::Term;

namespace detail {

struct EditScope;

// STA-namespace name: '/', '[', ']' and '\' embedded in the name are escaped,
// as OpenSTA's own readers store them; SdcNetwork's lookups depend on it.
std::string sta_name(Yosys::RTLIL::IdString id);
std::string sta_unescape(std::string_view name);
// STA name of one wire bit: `w` for scalars, `w[hdl_index]` for buses.
std::string bit_name(Yosys::RTLIL::Wire *wire, int bit);
sta::PortDirection *wire_direction(Yosys::RTLIL::Wire *wire);

// Two-level map lookup; nullptr when either key is absent.
template <typename Outer, typename K1, typename K2>
auto find2(const Outer &outer, const K1 &k1, const K2 &k2)
{
	using Value = std::remove_cvref_t<decltype(outer.begin()->second.begin()->second)>;
	auto it = outer.find(k1);
	if (it == outer.end())
		return Value(nullptr);
	auto inner = it->second.find(k2);
	if (inner == it->second.end())
		return Value(nullptr);
	return inner->second;
}

} // namespace detail

class YosysNetwork : public sta::ConcreteNetwork, private Yosys::NetView::PortModel {
public:
	// Un-hide base overloads shadowed by the netlist-half overrides below.
	using sta::ConcreteNetwork::name;
	using sta::ConcreteNetwork::id;
	using sta::ConcreteNetwork::cell;
	using sta::ConcreteNetwork::direction;
	using sta::ConcreteNetwork::findPin;
	using sta::ConcreteNetwork::findNet;
	using sta::ConcreteNetwork::instance;
	using sta::ConcreteNetwork::net;
	using sta::ConcreteNetwork::pin;
	using sta::ConcreteNetwork::isLeaf;
	using sta::ConcreteNetwork::pinIterator;
	using sta::ConcreteNetwork::getAttribute;
	using sta::ConcreteNetwork::attributeMap;
	using sta::ConcreteNetwork::visitConnectedPins;

	YosysNetwork();

	// Build the network view from the design's (uniquified) top hierarchy.
	// Liberty must already be loaded (leaf types resolve by name).
	void buildFromDesign(Yosys::RTLIL::Design *design);

	// Relink support: drop the whole view (caller must run Sta::networkChanged
	// first so no STA state still references our objects). Liberty survives.
	void reset();
	bool isLinkedToDesign() const { return view_.built(); }
	Yosys::NetView &view() { return view_; }

	// Staleness lives in the view; every host-side mutation route (pass
	// entry, Monitor event, `yosys` Tcl command) must drop its memo.
	bool designChanged(Yosys::RTLIL::Design *design) const;
	void setStale() { view_.setStale(); }
	bool isStale() const { return view_.stale(); }
	void invalidateCheck() const { view_.invalidateCheck(); }
	// Cached driver sets traverse hierarchy; any rewire drops them all.
	void dropDriverCache() { clearNetDrvrPinMap(); }
	// True while a NetworkEdit call is mutating the view: the Sta wrappers fire
	// the timing hooks themselves, so the Monitor bridge must stay quiet.
	bool inNetworkEdit() const { return in_network_edit_; }
	// Name a net the view created after link (a constant first used by a rewire).
	void indexNet(YosysNet *net);

	// Record modules containing objects resolved by SDC getters (for
	// keep_hierarchy marking). Ancestors are added at collection time.
	void beginRecording();
	Yosys::pool<Yosys::RTLIL::Module *> endRecording();

	bool linkNetwork(std::string_view top_cell_name, bool make_black_boxes, sta::Report *report) override;
	// False while stale so OpenSTA commands refuse to run on an outdated view
	// instead of reporting plausible-but-wrong numbers.
	bool isLinked() const override;

	// Instance
	sta::Instance *topInstance() const override;
	std::string name(const sta::Instance *instance) const override;
	sta::ObjectId id(const sta::Instance *instance) const override;
	std::string getAttribute(const sta::Instance *inst, std::string_view key) const override;
	const sta::AttributeMap &attributeMap(const sta::Instance *inst) const override;
	sta::Cell *cell(const sta::Instance *instance) const override;
	sta::Instance *parent(const sta::Instance *instance) const override;
	bool isLeaf(const sta::Instance *instance) const override;
	sta::Instance *findChild(const sta::Instance *parent, std::string_view name) const override;
	sta::Pin *findPin(const sta::Instance *instance, std::string_view port_name) const override;
	sta::Pin *findPin(const sta::Instance *instance, const sta::Port *port) const override;
	sta::InstanceChildIterator *childIterator(const sta::Instance *instance) const override;
	sta::InstancePinIterator *pinIterator(const sta::Instance *instance) const override;
	sta::InstanceNetIterator *netIterator(const sta::Instance *instance) const override;
	sta::InstanceSeq findInstancesMatching(const sta::Instance *context,
	                                       const sta::PatternMatch *pattern) const override;
	sta::InstanceSeq findInstancesHierMatching(const sta::Instance *instance,
	                                           const sta::PatternMatch *pattern) const override;
	sta::PinSeq findPinsMatching(const sta::Instance *instance,
	                             const sta::PatternMatch *pattern) const override;
	sta::PinSeq findPinsHierMatching(const sta::Instance *instance,
	                                 const sta::PatternMatch *pattern) const override;

	// Pin
	sta::ObjectId id(const sta::Pin *pin) const override;
	sta::Instance *instance(const sta::Pin *pin) const override;
	sta::Net *net(const sta::Pin *pin) const override;
	sta::Term *term(const sta::Pin *pin) const override;
	sta::Port *port(const sta::Pin *pin) const override;
	sta::PortDirection *direction(const sta::Pin *pin) const override;
	sta::VertexId vertexId(const sta::Pin *pin) const override;
	void setVertexId(sta::Pin *pin, sta::VertexId id) override;

	// Term
	sta::ObjectId id(const sta::Term *term) const override;
	sta::Net *net(const sta::Term *term) const override;
	sta::Pin *pin(const sta::Term *term) const override;

	// Net
	std::string name(const sta::Net *net) const override;
	sta::ObjectId id(const sta::Net *net) const override;
	sta::Net *findNet(const sta::Instance *instance, std::string_view net_name) const override;
	void findInstNetsMatching(const sta::Instance *instance, const sta::PatternMatch *pattern,
	                          sta::NetSeq &matches) const override;
	sta::Instance *instance(const sta::Net *net) const override;
	bool isPower(const sta::Net *net) const override;
	bool isGround(const sta::Net *net) const override;
	sta::NetPinIterator *pinIterator(const sta::Net *net) const override;
	sta::NetTermIterator *termIterator(const sta::Net *net) const override;

	sta::ConstantPinIterator *constantPinIterator() override;

	// NetworkEdit — OpenSTA-initiated edits; mutate RTLIL through the view.
	// The Sta wrappers fire the before/after timing hooks around these.
	sta::Instance *makeInstance(sta::LibertyCell *cell, std::string_view name,
	                            sta::Instance *parent) override;
	void makePins(sta::Instance *inst) override;
	void replaceCell(sta::Instance *inst, sta::Cell *cell) override;
	void deleteInstance(sta::Instance *inst) override;
	sta::Pin *connect(sta::Instance *inst, sta::Port *port, sta::Net *net) override;
	sta::Pin *connect(sta::Instance *inst, sta::LibertyPort *port, sta::Net *net) override;
	void disconnectPin(sta::Pin *pin) override;
	void deletePin(sta::Pin *pin) override;
	sta::Net *makeNet(std::string_view name, sta::Instance *parent) override;
	void deleteNet(sta::Net *net) override;
	void mergeInto(sta::Net *net, sta::Net *into_net) override;
	sta::Net *mergedInto(sta::Net *net) override;

protected:
	void visitConnectedPins(const sta::Net *net, sta::PinVisitor &visitor,
	                        sta::NetSet &visited_nets) const override;

private:
	// NetView::PortModel: leaf cell ports in liberty declaration order.
	bool ports(const Yosys::RTLIL::Cell *cell, std::vector<Yosys::NetView::PortShape> &out) override;

	Yosys::RTLIL::Module *moduleOf(const sta::Instance *instance) const;
	sta::ConcreteCell *staCellForType(Yosys::RTLIL::IdString type);
	sta::ConcreteCell *lookupCellForType(Yosys::RTLIL::IdString type) const;
	sta::ConcreteCell *makeStubCell(Yosys::RTLIL::IdString type);
	sta::ConcreteCell *makeModuleCell(Yosys::RTLIL::Module *module);
	sta::Port *makeWirePort(sta::Cell *scell, Yosys::RTLIL::Wire *wire, const std::string &name);
	void indexPorts(sta::ConcreteCell *ccell);
	sta::ConcreteCell *staCellOf(const Yosys::RTLIL::Cell *cell) const;
	sta::ConcretePort *bitPort(sta::ConcretePort *port, const YosysPin *pin) const;
	sta::ConcretePort *portOf(const YosysPin *pin) const;
	void buildTables(Yosys::RTLIL::Module *module);
	void indexPins(const Yosys::RTLIL::Cell *cell);
	std::string netName(const YosysNet *net) const;
	void indexNetNames(Yosys::RTLIL::Module *module);
	void logCounts(long ms) const;
	void recordModule(Yosys::RTLIL::Module *module) const;
	void recordInstance(const sta::Instance *instance) const;
	void recordPin(const sta::Pin *pin) const;
	static const YosysPin *asPin(const sta::Pin *pin);
	static const YosysNet *asNet(const sta::Net *net);
	static const YosysTerm *asTerm(const sta::Term *term);

	Yosys::NetView view_;
	Yosys::RTLIL::Design *design_ = nullptr;
	sta::Library *design_lib_ = nullptr;
	sta::Instance *top_instance_sentinel_;
	sta::ConcreteCell *ytop_cell_ = nullptr;

	Yosys::dict<Yosys::RTLIL::Module *, sta::ConcreteCell *> module_cell_;
	Yosys::dict<Yosys::RTLIL::IdString, sta::ConcreteCell *> type_cell_map_;
	// STA cell -> port IdString -> port object (bus ports, not their bits)
	std::unordered_map<const sta::ConcreteCell *, Yosys::dict<Yosys::RTLIL::IdString, sta::ConcretePort *>> cell_ports_;
	Yosys::IdMap<sta::ConcretePort *> pin_ports_; // by pin id: scalar or bus-bit port
	Yosys::IdMap<sta::VertexId> vertex_ids_;      // by pin id
	// STA-escaped child name -> cell, per module
	Yosys::dict<Yosys::RTLIL::Module *, std::map<std::string, Yosys::RTLIL::Cell *, std::less<>>> child_map_;
	// STA net name (canonical and aliases) -> net, per module
	Yosys::dict<Yosys::RTLIL::Module *, std::map<std::string, YosysNet *, std::less<>>> net_names_;
	// STA port-bit name (e.g. "D", "A[3]") -> pin, per instance; key nullptr = top
	std::unordered_map<const Yosys::RTLIL::Cell *, std::map<std::string, YosysPin *, std::less<>>> pin_map_;

	bool in_network_edit_ = false;
	mutable std::mutex record_mutex_;
	mutable bool recording_ = false;
	mutable Yosys::pool<Yosys::RTLIL::Module *> recorded_modules_;
};

} // namespace ysta
