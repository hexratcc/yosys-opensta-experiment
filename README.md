# warning: experimental llm slop

## opensta_net

Yosys plugin implementing OpenSTA's `sta::Network` interface over a live
RTLIL design. OpenSTA runs inside Yosys on its Tcl interpreter; SDC is
evaluated directly against the design, no netlist export.

Built on `kernel/netview.h` from the Yosys fork
https://github.com/hexratcc/yosys, branch `nella/opensta-llm-experiment`,
the provider-neutral netlist view. This plugin adds the OpenSTA half: name
escaping, liberty binding, bus-bit ports, `keep_hierarchy` recording,
`NetworkEdit`.

Verification spike. Passes OpenSTA's own `network/test` suite (26 cases,
byte-identical) and 18 local checks, including the Verilog + SDC handoff
into OpenSTA's reader.

### Build

Needs that Yosys fork on the branch, built in `<yosys>/build`, and the
OpenROAD fork of OpenSTA built PIC without IPO:

    cmake -B build . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POSITION_INDEPENDENT_CODE=ON

Then, with `nix-shell` for the toolchain:

    make YOSYS_SRC=../yosys OPENSTA=../OpenSTA
    make test

### Use

    read_liberty -lib cells.lib
    read_verilog design.v
    hierarchy -top top
    uniquify
    opensta_net -liberty cells.lib -link
    opensta_net -sdc design.sdc -keep_hierarchy
    opensta_net -eval report_checks

Options: `-liberty`, `-link`, `-sdc`, `-keep_hierarchy`, `-source`, `-eval`,
`-edits <file>` (log of every edit since boot). OpenSTA commands also work
directly in Yosys's Tcl mode. A design changed behind the Monitor's back
relinks on the next call and clears constraints.

Limits: hierarchical designs must be uniquified; leaf cells bind to liberty
by type name; liberty before `-link`; no `deletePin`, `mergeInto`, or
deleting nets across a module boundary.

### License

GPL-3.0-or-later (derivative of OpenSTA), see `LICENSE`. Yosys and
`kernel/netview.h` stay ISC. Tested against OpenSTA `be771a0`.
