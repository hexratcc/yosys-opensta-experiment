// SPDX-License-Identifier: GPL-3.0-or-later
// OpenSTA network adapter spike: boot a Sta instance on the Yosys Tcl
// interpreter, link the RTLIL design through YosysNetwork, and evaluate STA
// Tcl commands from a Yosys pass.
//
// Out-of-tree verification plugin, not intended for upstreaming.

#include "kernel/yosys.h"

#include <tcl.h>

#include <fstream>

#include "sta/Liberty.hh"
#include "sta/MinMax.hh"
#include "sta/ReportTcl.hh"
#include "sta/Sta.hh"
#include "sta/StaMain.hh"

#include "yosys_network.hh"

namespace sta {
extern const char *tcl_inits[];
}

// Swig uses C linkage for init functions.
extern "C" {
extern int Sta_Init(Tcl_Interp *interp);
}

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// ReportTcl prints via the interp's stdout channel, which bypasses yosys's
// log files; tee everything into them so `yosys -l` captures STA output.
struct YosysStaReport : public sta::ReportTcl {
	size_t printString(const char *buffer, size_t length) override
	{
		// The console is already served by the Tcl channel below; only feed
		// real log sinks (yosys keeps stdout in log_files by default).
		for (FILE *f : log_files) {
			if (f != stdout && f != stderr)
				fwrite(buffer, 1, length, f);
		}
		for (std::ostream *s : log_streams)
			s->write(buffer, length);
		return sta::ReportTcl::printString(buffer, length);
	}
};

struct YosysSta : public sta::Sta {
	ysta::YosysNetwork *yosys_network = nullptr;
	std::unique_ptr<EditLog> edit_log;
	// Iterative flows re-run the same script; a re-read would pile up
	// duplicate libraries in the scene (Sta never clears them).
	pool<std::string> liberty_files_read;

	void makeReport() override
	{
		report_ = new YosysStaReport;
	}

	void makeNetwork() override
	{
		yosys_network = new ysta::YosysNetwork;
		network_ = yosys_network;
	}

	// Sta::connectPin fires no disconnect hook, so a reconnect would leave
	// the old net's wire edge alive in the timing graph: make it an explicit
	// disconnect + connect.
	void connectPin(sta::Instance *inst, sta::Port *port, sta::Net *net) override
	{
		sta::Pin *pin = network_->findPin(inst, port);
		if (pin != nullptr && network_->net(pin) != nullptr)
			disconnectPin(pin);
		sta::Sta::connectPin(inst, port, net);
	}

	void connectPin(sta::Instance *inst, sta::LibertyPort *lport, sta::Net *net) override
	{
		sta::Port *port = network_->findPort(network_->cell(inst), lport->name());
		if (port == nullptr)
			report_->error(2907, "connect: no port {}.", lport->name());
		connectPin(inst, port, net);
	}

	// Sta::deleteInstance fires deleteInstanceBefore before the network can
	// refuse a hierarchical instance; check first so the graph stays intact.
	void deleteInstance(sta::Instance *inst) override
	{
		if (!network_->isLeaf(inst))
			report_->error(2904, "deleteInstance only supports leaf instances.");
		sta::Sta::deleteInstance(inst);
	}
};

// Never deleted on purpose: yosys calls Tcl_Finalize at exit and Sta teardown
// after that is unsafe (pitfalls M10).
YosysSta *the_sta = nullptr;

// Bridge: yosys-initiated connectivity edits, already applied to the view
// by its RTLIL Monitor, -> Sta timing hooks. Create/delete/rename gaps are
// the documented upstream Monitor-extension ask; the view goes stale on
// anything it cannot follow and the next opensta_net call relinks.
struct StaNetListener : public Netlist::Listener {
	// Adapter-initiated edits run inside the Sta wrappers, which fire the
	// timing hooks themselves; every other change (Monitor, or another user
	// of the view) needs them here.
	void disconnecting(Netlist::Pin *pin, Netlist::Origin) override
	{
		if (the_sta->yosys_network->inNetworkEdit())
			return;
		the_sta->disconnectPinBefore(reinterpret_cast<sta::Pin *>(pin));
		the_sta->yosys_network->dropDriverCache();
	}

	void connected(Netlist::Pin *pin, Netlist::Origin) override
	{
		if (the_sta->yosys_network->inNetworkEdit())
			return;
		the_sta->yosys_network->dropDriverCache();
		the_sta->connectPinAfter(reinterpret_cast<sta::Pin *>(pin));
	}

	void netAdded(Netlist::Net *net, Netlist::Origin) override
	{
		the_sta->yosys_network->indexNet(net);
	}
};

StaNetListener the_listener;

// Leave-trace on the `yosys` Tcl command: any pass run from Tcl may have
// mutated the design behind the Monitor's back, so the next OpenSTA command
// must re-run the full staleness comparison.
int tcl_network_touched(ClientData, Tcl_Interp *, int, Tcl_Obj *const[])
{
	the_sta->yosys_network->invalidateCheck();
	return TCL_OK;
}

void boot_sta()
{
	if (the_sta != nullptr)
		return;

	Tcl_Interp *interp = yosys_get_tcl_interp();

	sta::initSta();
	the_sta = new YosysSta;
	sta::Sta::setSta(the_sta);
	the_sta->makeComponents();
	the_sta->setTclInterp(interp);
	the_sta->setThreadCount(1);

	if (Sta_Init(interp) != TCL_OK)
		log_error("OpenSTA: Sta_Init failed: %s\n", Tcl_GetStringResult(interp));
	sta::evalTclInit(interp, sta::tcl_inits);
	if (Tcl_Eval(interp, "init_sta_cmds") != TCL_OK)
		log_error("OpenSTA: init_sta_cmds failed: %s\n", Tcl_GetStringResult(interp));

	the_sta->yosys_network->view().addListener(&the_listener);
	the_sta->edit_log = std::make_unique<EditLog>(the_sta->yosys_network->view());
	Tcl_CreateObjCommand(interp, "ysta::network_touched", tcl_network_touched, nullptr, nullptr);
	if (Tcl_Eval(interp, "trace add execution yosys leave ysta::network_touched") != TCL_OK)
		log_error("OpenSTA: cannot trace the yosys command: %s\n", Tcl_GetStringResult(interp));

	log("OpenSTA instance created on the Yosys Tcl interpreter.\n");
}

struct OpenstaNetPass : public Pass {
	OpenstaNetPass() : Pass("opensta_net", "OpenSTA network adapter spike") {}

	void help() override
	{
		log("\n");
		log("    opensta_net [-liberty <file>] [-link] [-eval <tcl>]\n");
		log("\n");
		log("Boot an embedded OpenSTA instance on the Yosys Tcl interpreter.\n");
		log("\n");
		log("    -liberty <file>\n");
		log("        read a liberty file into OpenSTA.\n");
		log("\n");
		log("    -link\n");
		log("        (re)build the OpenSTA network view from the uniquified design.\n");
		log("        If the design changed since the last link (e.g. opt passes ran),\n");
		log("        any invocation relinks automatically and clears the constraints.\n");
		log("\n");
		log("    -sdc <file>\n");
		log("        source an SDC file in the shared interpreter.\n");
		log("\n");
		log("    -keep_hierarchy\n");
		log("        with -sdc: mark modules referenced by the constraints (and their\n");
		log("        ancestors) with the keep_hierarchy attribute.\n");
		log("\n");
		log("    -source <file>\n");
		log("        source a Tcl file in the shared interpreter.\n");
		log("\n");
		log("    -eval <tcl>\n");
		log("        evaluate <tcl> in the shared interpreter and log the result.\n");
		log("\n");
		log("    -edits <file>\n");
		log("        write every edit made since boot (OpenSTA-initiated and yosys-side\n");
		log("        rewires seen by the Monitor), one per line, by name.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing OPENSTA_NET pass (spike).\n");

		std::string eval_cmd, liberty_file, source_file, sdc_file, edits_file;
		bool link = false, keep_hierarchy = false;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-eval" && argidx + 1 < args.size()) {
				eval_cmd = args[++argidx];
				continue;
			}
			if (args[argidx] == "-edits" && argidx + 1 < args.size()) {
				edits_file = args[++argidx];
				continue;
			}
			if (args[argidx] == "-liberty" && argidx + 1 < args.size()) {
				liberty_file = args[++argidx];
				continue;
			}
			if (args[argidx] == "-link") {
				link = true;
				continue;
			}
			if (args[argidx] == "-source" && argidx + 1 < args.size()) {
				source_file = args[++argidx];
				continue;
			}
			if (args[argidx] == "-sdc" && argidx + 1 < args.size()) {
				sdc_file = args[++argidx];
				continue;
			}
			if (args[argidx] == "-keep_hierarchy") {
				keep_hierarchy = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		boot_sta();
		if (!liberty_file.empty())
			readLiberty(liberty_file);
		linkDesign(design, link);
		if (!sdc_file.empty())
			sourceFile(sdc_file, "sdc", keep_hierarchy);
		if (!source_file.empty())
			sourceFile(source_file, "tcl", false);
		if (!eval_cmd.empty())
			evalTcl(eval_cmd);
		if (!edits_file.empty())
			dumpEdits(edits_file);
	}

	// `w` or `w[idx]` for wire bits, the constant otherwise.
	static std::string signalName(const RTLIL::SigBit &bit)
	{
		if (bit.wire == nullptr)
			return log_signal(bit);
		if (bit.wire->width == 1)
			return RTLIL::unescape_id(bit.wire->name);
		return RTLIL::unescape_id(bit.wire->name) + "[" + std::to_string(bit.wire->to_hdl_index(bit.offset)) + "]";
	}

	// One line per edit: kind origin module cell port bit type-or-signal.
	static void dumpEdits(const std::string &file)
	{
		std::ofstream out(file);
		if (!out)
			log_error("opensta_net: cannot write %s.\n", file.c_str());
		for (const EditLog::Entry &e : the_sta->edit_log->entries()) {
			out << EditLog::kindName(e.kind) << ' ' << (e.origin == Netlist::Origin::Edit ? "edit" : "monitor");
			if (e.kind == EditLog::Kind::Reset) {
				out << '\n';
				continue;
			}
			out << ' ' << RTLIL::unescape_id(e.module);
			switch (e.kind) {
			case EditLog::Kind::CellAdded:
			case EditLog::Kind::CellRetyped:
			case EditLog::Kind::CellRemoved:
				out << ' ' << RTLIL::unescape_id(e.cell) << ' ' << RTLIL::unescape_id(e.type);
				break;
			case EditLog::Kind::NetAdded:
			case EditLog::Kind::NetRemoved:
				out << ' ' << signalName(e.signal);
				break;
			default:
				out << ' ' << (e.cell.empty() ? "-" : RTLIL::unescape_id(e.cell)) << ' ' << RTLIL::unescape_id(e.port)
				    << ' ' << e.bit << ' ' << signalName(e.signal);
				break;
			}
			out << '\n';
		}
		log("opensta_net: wrote %zu edits to %s.\n", the_sta->edit_log->entries().size(), file.c_str());
	}

	static void readLiberty(const std::string &file)
	{
		if (the_sta->liberty_files_read.count(file)) {
			log("opensta_net: liberty file %s already loaded; skipping.\n", file.c_str());
			return;
		}
		sta::LibertyLibrary *lib = the_sta->readLiberty(file.c_str(), the_sta->cmdScene(),
		                                                sta::MinMaxAll::all(), true);
		if (lib == nullptr)
			log_error("OpenSTA: failed to read liberty file %s.\n", file.c_str());
		the_sta->liberty_files_read.insert(file);
	}

	// Relink when asked, or when the design changed in ways the Monitor
	// cannot report (opt passes deleting/adding/renaming cells or wires).
	static void linkDesign(RTLIL::Design *design, bool link)
	{
		ysta::YosysNetwork *network = the_sta->yosys_network;
		network->invalidateCheck(); // passes may have run since the last command
		bool relink = false;
		if (network->isLinkedToDesign() && (network->isStale() || network->designChanged(design))) {
			log_warning("opensta_net: design changed since link; rebuilding STA network "
			            "(constraints cleared, re-read the SDC).\n");
			relink = true;
			// Present an empty netlist during teardown: Sta::networkChanged
			// traverses the network, whose tables may hold freed objects.
			network->setStale();
		}
		if ((link || relink) && network->isLinkedToDesign()) {
			the_sta->networkChanged();
			network->reset();
		}
		if (link || relink)
			network->buildFromDesign(design); // the view registers its own Monitor
	}

	// sourceTclFile evals command-by-command like OpenSTA's own script
	// handling (matters for warning line numbers). With keep_hierarchy the
	// modules owning resolved objects are marked once the file has run.
	static void sourceFile(const std::string &file, const char *what, bool keep_hierarchy)
	{
		ysta::YosysNetwork *network = the_sta->yosys_network;
		Tcl_Interp *interp = yosys_get_tcl_interp();
		if (keep_hierarchy)
			network->beginRecording();
		int result = sta::sourceTclFile(file.c_str(), false, false, interp);
		if (keep_hierarchy) {
			pool<RTLIL::Module *> modules = network->endRecording();
			for (RTLIL::Module *module : modules) {
				module->set_bool_attribute(ID::keep_hierarchy);
				log("opensta_net: keep_hierarchy on %s\n", log_id(module));
			}
			log("opensta_net: marked %zu modules keep_hierarchy.\n", modules.size());
		}
		if (result != TCL_OK)
			log_error("OpenSTA %s error: %s\n", what, Tcl_GetStringResult(interp));
	}

	static void evalTcl(const std::string &cmd)
	{
		Tcl_Interp *interp = yosys_get_tcl_interp();
		int result = Tcl_Eval(interp, cmd.c_str());
		const char *result_str = Tcl_GetStringResult(interp);
		if (result != TCL_OK)
			log_error("OpenSTA tcl error: %s\n", result_str);
		if (result_str[0] != 0)
			log("%s\n", result_str);
	}
} OpenstaNetPass;

// Rewire one scalar cell port to a wire through the official RTLIL API, so
// the Monitor bridge picks it up (M4 yosys-side incremental edit demo).
struct OpenstaDemoEditPass : public Pass {
	OpenstaDemoEditPass() : Pass("opensta_demo_edit", "rewire a cell port (spike demo)") {}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		std::string cell_name, port_name, wire_name;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-cell" && argidx + 1 < args.size()) {
				cell_name = args[++argidx];
				continue;
			}
			if (args[argidx] == "-port" && argidx + 1 < args.size()) {
				port_name = args[++argidx];
				continue;
			}
			if (args[argidx] == "-wire" && argidx + 1 < args.size()) {
				wire_name = args[++argidx];
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		RTLIL::Module *module = design->top_module();
		RTLIL::Cell *cell = module->cell(RTLIL::escape_id(cell_name));
		RTLIL::Wire *wire = module->wire(RTLIL::escape_id(wire_name));
		if (cell == nullptr || wire == nullptr)
			log_error("opensta_demo_edit: cell or wire not found.\n");
		cell->setPort(RTLIL::escape_id(port_name), RTLIL::SigSpec(RTLIL::SigBit(wire, 0)));
		log("opensta_demo_edit: %s/%s -> %s\n", cell_name.c_str(), port_name.c_str(), wire_name.c_str());
	}
} OpenstaDemoEditPass;

PRIVATE_NAMESPACE_END
