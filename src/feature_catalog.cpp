#include "feature_catalog.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace {
const char *kFeatureText = R"FEATURES(
1. User Interface & Experience (200+)
1.1 Window Management (20)
Support for docking/undocking panels (like Visual Studio).

Save and restore window layouts.

Multi‑monitor awareness.

Full‑screen mode (F11).

High DPI scaling (per‑monitor DPI).

Customizable theme (light/dark/other).

Start page with recent projects and news.

Splash screen on startup.

Application icon in taskbar.

Progress indicator for long operations.

Animated transitions for panel resizing.

Minimize to tray.

Always‑on‑top mode.

Window transparency/opacity slider.

Tabbed editor (multiple files open).

Drag‑and‑drop of files onto editor.

Auto‑reload externally changed files.

Session restore (open files, breakpoints, etc.).

Show line numbers in editor.

Code folding for sections and macros.

1.2 Editor Features (30)
Syntax highlighting for 6502 assembly (mnemonics, directives, labels, comments, numbers).

Auto‑indentation and smart indent.

Auto‑completion for mnemonics and labels.

Code snippets (e.g., loop, macro, if).

Bracket/quote matching.

Show invisible characters (spaces, tabs).

Column/block selection (Alt+drag).

Multiple cursors (Ctrl+click).

Find/Replace with regex support.

Find in files (search across project).

Go to line (Ctrl+G).

Bookmarks (toggle with Ctrl+F2, navigate with F2).

Jump to definition (label or macro).

Peek definition (inline popup).

Rename symbol (label) across all files.

Show references of a label.

Code formatting (indent, align operands).

Comment/uncomment block (Ctrl+/).

Toggle case of selected text.

Insert/remove line numbers.

Zoom in/out editor (Ctrl+MouseWheel).

Customizable font family and size.

Word wrap toggle.

Display whitespace mode.

Minimap of source code.

Code lens (inline info for labels).

Autosave on build.

Backup copies with timestamp.

Editor split (vertical/horizontal).

Diff view between editor and last saved version.

1.3 Register & Status Display (25)
Live updating register values (A, X, Y, PC, SP, P).

Status flags shown as individual LEDs (N,V,‑,B,D,I,Z,C).

Register history graph (show changes over time).

Click on register to toggle display format (hex, decimal, binary).

Copy register values to clipboard.

Show stack pointer with stack memory.

Display current instruction and operands.

Show last executed instruction (history).

Show total cycles, instructions executed.

Show instruction per second (IPS) rate.

Show execution time in microseconds.

Display current bank/segment.

Show emulated clock frequency.

Display CPU state (running, halted, wait).

Show interrupt status (IRQ, NMI pending).

Register change highlight (flash when value changes).

Color‑coded registers (e.g., green for changed).

Hover over flag to see meaning.

Tooltip with flag descriptions.

Export register state to file.

Compare register states between snapshots.

Reset registers to default.

Load register state from file.

Animate register changes (slow motion).

Show register values as both signed and unsigned.

1.4 Memory Viewer & Editor (25)
Hex dump with ASCII view (side‑by‑side).

Configurable bytes per row (8, 16, 32).

Scrollable memory window.

Go to address (Ctrl+G).

Search memory (hex string or ASCII).

Search next/previous.

Edit bytes in place (double‑click).

Fill memory range with value.

Copy selected bytes as hex, ASCII, or C array.

Memory difference highlighting (between snapshots).

Memory access coloring (read, write, execute).

Memory map visualisation (used/free blocks).

Show symbols/labels over addresses.

Mark ROM areas as read‑only.

Show memory protection flags (R/W/X).

Show memory region names (e.g., "Stack", "Screen").

Watchpoint display in memory view.

Memory export to file (binary, hex, Intel HEX).

Memory import from file.

Compare two memory ranges.

Checksum calculation (CRC, SHA).

Memory fill with pattern (e.g., incremental).

Show memory usage statistics.

Jump to address from disassembly view.

Display memory in different endianness.

1.5 Disassembly & Code View (25)
Disassembly with symbols (label names).

Mixed source+disassembly view.

Show instruction bytes and opcodes.

Show cycle counts per instruction.

Show branch target addresses.

Show data in disassembly (.byte, .word).

Disassembly navigation (follow jumps).

Back/forward navigation history.

Mark current PC with a colored arrow.

Set breakpoints by clicking on address.

Show code coverage (executed lines highlighted).

Show profiler hot spots (color intensity).

Disassembly export to text file.

Print disassembly.

Disassemble to file with labels.

Show comments from source (if available).

Show macro expansions.

Collapsible blocks (subroutines).

Call graph generation from disassembly.

Cross‑reference listing.

Dead code detection.

Show instruction frequency histogram.

Show jump/call statistics.

Disassembly with pseudo‑ops for data.

Copy disassembly line to clipboard.

1.6 Terminal/Console (20)
Command history (up/down arrows).

Command autocomplete (Tab).

Colored output (error, warning, info).

Clear console.

Log to file.

Search console text.

Export console to file.

Filter console output by level.

Show timestamps in console.

Console bookmarking.

Custom command aliases.

Script execution from console.

Batch mode (run script file).

Console variables (set and use).

Conditional execution in scripts.

Loop constructs in scripts.

Show last command output.

Auto‑scroll to bottom.

Pause console scrolling.

Copy text from console.

1.7 Toolbars & Menus (20)
Customizable toolbar.

Show/hide toolbars.

Menu bar with shortcuts.

Right‑click context menus in all views.

Quick access buttons for common actions.

Keyboard shortcut editor.

Shortcut presets (VS, Eclipse, etc.).

Recent files list.

Recent projects list.

Open folder as project.

Build configurations (Debug, Release).

Build targets (different memory maps).

Quick build and run button.

Quick run without build.

Step buttons (Step Into, Step Over, Step Out).

Run to cursor.

Reset and restart.

Show/hide panels via menu.

Panel auto‑hide.

Status bar with CPU info and messages.

1.8 Keyboard & Accessibility (20)
Full keyboard shortcut support.

Access keys (Alt+underlined letters).

High contrast mode.

Screen reader compatibility.

Font scaling.

Zoom in/out of all UI.

Reduce motion animations.

Keyboard navigation of all controls.

Tooltips with descriptions.

Focus indicators.

Customizable key bindings.

Export/import key bindings.

Reset key bindings to default.

Support for left‑handed layout.

Sticky keys for special actions.

Voice command support (Windows Speech).

Touch support (gestures).

Magnifier integration.

Color‑blind friendly themes.

User‑defined color schemes.

1.9 Notification & Feedback (15)
Build success/failure notifications.

Breakpoint hit notification.

Watchpoint hit notification.

Emulation pause/warning notifications.

Progress bars for long operations (file upload).

Sound effects for events (optional).

Log area with activity feed.

Error list with click‑to‑navigate.

Warning list.

Hover preview of values in memory.

Live preview of expression evaluation.

Inline error highlighting in editor.

Status bar messages.

Debugging tips popup.

First‑run wizard.

2. Assembler & Compilation (100+)
2.1 Directives & Pseudo‑Ops (25)
.org – set program origin.

.byte / .db – define bytes.

.word / .dw – define 16‑bit words.

.asciiz – null‑terminated string.

.ascii – non‑terminated string.

.include – include another file.

.macro – define macro.

.endm – end macro.

.rept – repeat block.

.irp – iterate over parameters.

.if, .else, .endif – conditional assembly.

.ifdef, .ifndef.

.error – force error.

.warning – emit warning.

.list / .nolist – control listing.

.title – program title.

.subtitle – section subtitle.

.page – form feed in listing.

.align – align to boundary.

.res / .fill – reserve space.

.export – export symbol.

.import – import external symbol.

.weak – weak symbol.

.section – named section.

.end – end of source.

2.2 Expression Evaluation (15)
Arithmetic: +, -, *, /, %, <<, >>.

Bitwise: &, |, ^, ~.

Comparison: ==, !=, <, >, <=, >=.

Logical: &&, ||, !.

Parentheses and operator precedence.

Unary: +, -, ~.

Low/high byte operators: <, >.

Bank byte operator: ^ (for 65816).

Current PC reference: $.

String literals with escape sequences.

Character constants ('A').

Label references (forward/backward).

Anonymous labels (-, +).

Local labels (@ prefix).

Symbol scoping (local/global).

2.3 Macro Support (15)
Define macro with parameters.

Default parameter values.

Macro local labels (@).

Macro expansion in place.

Nested macros.

Macro recursion.

String concatenation in macro.

Macro argument stringification.

Macro argument count.

Macro iteration over arguments.

Conditionals inside macros.

Macro error/warning directives.

Macro expansion listing.

Macro call with named arguments.

Macro library include.

2.4 Error Handling & Reporting (15)
Line numbers in error messages.

Multi‑line error context.

Error codes.

Error suppression ($ prefix).

Warning levels.

Error/warning filtering.

Error list view with links.

Build log with timestamps.

Automatic error recovery (skip line).

Syntax highlighting of errors in editor.

Quick fix suggestions (e.g., missing label).

Spell check for labels.

Unused label warning.

Duplicate label detection.

Instruction validation (addressing mode).

2.5 Symbol Management (15)
Global symbol table.

Local symbols per scope.

Symbol export/import.

Symbol renaming.

Symbol search.

Symbol list in a docked panel.

Symbol usage count.

Symbol documentation comments.

Symbol auto‑complete.

Symbol address display.

Symbol value evaluation.

Symbol overlay (override from other file).

Symbol dependency graph.

Symbol alias.

Symbol versioning.

2.6 Listing & Output Formats (15)
Generate listing file with addresses and bytes.

Generate binary image.

Generate Intel HEX file.

Generate Motorola S‑record.

Generate PRG (C64) file.

Generate NES ROM (iNES) header.

Generate VICE monitor label file.

Generate debug symbol file (DWARF).

Generate disassembly listing.

Generate HTML report with highlighted code.

Generate JSON symbol table.

Output to memory directly.

Output with checksum.

Output to multiple banks.

Output with padding.

3. CPU Core & Instruction Set (120+)
3.1 Official Instructions (25)
ADC, AND, ASL, BCC, BCS, BEQ, BIT, BMI, BNE, BPL, BRK, BVC, BVS.

CLC, CLD, CLI, CLV, CMP, CPX, CPY.

DEC, DEX, DEY, EOR, INC, INX, INY.

JMP, JSR, LDA, LDX, LDY, LSR.

NOP, ORA, PHA, PHP, PLA, PLP.

ROL, ROR, RTI, RTS, SBC.

SEC, SED, SEI, STA, STX, STY.

TAX, TAY, TSX, TXA, TXS, TYA.

All addressing modes: Implied, Accumulator, Immediate, Zero Page, Zero Page X/Y, Absolute, Absolute X/Y, Indirect, Indexed Indirect, Indirect Indexed, Relative.

Page crossing cycle penalties.

Wrap‑around for zero‑page indexing.

JMP indirect bug (6502).

BRK with B flag.

Decimal mode for ADC/SBC.

Flag interactions (N,V,Z,C).

Interrupt handling (IRQ, NMI, BRK).

Stack operations.

Reset vector handling.

Read‑modify‑write cycles.

Instruction timing (cycle accurate).

Illegal opcodes (see below).

65C02 extensions (BRA, PHX, PLX, STZ, TRB, TSB, etc.).

65816 extensions (16‑bit registers, long addressing).

65CE02 extensions (additional registers).

User‑definable instruction hooks.

3.2 Illegal / Undocumented Opcodes (30)
ANC – AND with C flag (0x0B, 0x2B).

ANE – AND with immediate (0x8B).

ARR – AND with rotate (0x6B).

ASR – AND then LSR (0x4B).

DCP – DEC then CMP (0xC7, 0xD7, 0xCF, 0xDF, 0xDB, 0xD3).

ISC – INC then SBC (0xE7, 0xF7, 0xEF, 0xFF, 0xFB, 0xF3).

LAS – LDA with SP (0xBB).

LAX – LDA + TAX (0xA7, 0xB7, 0xAF, 0xBF, 0xA3, 0xB3).

LXA – LAX with immediate (0xAB).

RLA – ROL then AND (0x27, 0x37, 0x2F, 0x3F, 0x3B, 0x33).

RRA – ROR then ADC (0x67, 0x77, 0x6F, 0x7F, 0x7B, 0x73).

SAX – STA then TXA (0x87, 0x97, 0x8F, 0x83).

SBC – alternative (0xEB).

SHX – store X indexed (0x9E).

SHY – store Y indexed (0x9C).

SLO – ASL then ORA (0x07, 0x17, 0x0F, 0x1F, 0x1B, 0x13).

SRE – LSR then EOR (0x47, 0x57, 0x4F, 0x5F, 0x5B, 0x53).

TAS – transfer to SP (0x9B).

XAA – XOR with immediate (0x8B).

AHX – AND with index (0x93, 0x9F).

AXS – AND with SBC (0xCB).

DOP – double NOP (0x1C, 0x3C, 0x5C, 0x7C, 0xDC, 0xFC).

TOP – triple NOP (0x0C, 0x14, 0x34, 0x54, 0x74, 0xD4, 0xF4).

JAM – crash (0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2).

NOP variants (0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xEA).

SAX – store A & X.

SHA – store A & X indirect.

SHY – store Y indirect.

SHX – store X indirect.

ANC – AND with carry.

3.3 Timing & Accuracy (20)
Cycle‑accurate emulation.

Page crossing penalties for all indexed modes.

Read‑modify‑write cycles with dummy reads.

Interrupt latency (7 cycles).

Reset sequence (6 cycles).

BRK with 7 cycles.

JSR with 6 cycles.

RTS with 6 cycles.

RTI with 6 cycles.

Branch taken/not taken cycles.

Branch page crossing extra cycle.

Decimal mode timing.

Illegal opcode timing (often same as official).

Clock frequency configuration (1 MHz, 2 MHz, etc.).

Clock divider for peripherals.

Cycle‑stealing for DMA.

Bus arbitration.

Wait states for memory access.

Synchronous vs asynchronous mode.

Profiling of cycles per instruction.

3.4 Interrupts & Reset (15)
IRQ vector at $FFFE.

NMI vector at $FFFA.

Reset vector at $FFFC.

Interrupt enable flag (I).

Interrupt priority (NMI > IRQ).

BRK as software interrupt.

BRK with B flag set in status.

RTI to return from interrupt.

NMI on edge.

IRQ on level.

Interrupt acknowledge.

Interrupt pending state.

Automatic saving of registers.

Interrupt vector relocation (for 65816).

Non‑maskable interrupt (NMI).

3.5 Register & Flag Management (20)
8‑bit A, X, Y.

16‑bit PC.

8‑bit SP.

8‑bit P (flags).

Flag definitions: N (bit7), V (bit6), B (bit4), D (bit3), I (bit2), Z (bit1), C (bit0).

Unused flag (bit5) always 1.

Decimal flag for BCD mode.

Carry flag for arithmetic.

Zero flag for result zero.

Negative flag for sign.

Overflow flag for signed overflow.

Break flag for BRK.

65816: 16‑bit A, X, Y, and direct page.

65816: Program bank register.

65816: Data bank register.

65816: Direct page register.

65C02: STZ instruction.

65C02: PHX/PLX, PHY/PLY.

65C02: BRA (branch always).

65C02: TRB/TSB (test and set/reset).

3.6 CPU Configuration & Extensibility (10)
Select CPU type (6502, 65C02, 65816, 65CE02, 45GS02).

Select clock speed.

Enable/disable illegal opcodes.

Enable/disable decimal mode.

Enable/disable cycle‑accuracy.

Enable/disable NMI/IRQ.

User‑defined instruction hooks (callback).

Instruction logging (to file).

Instruction replacement (overlay).

Custom instruction set (for fantasy consoles).

4. Memory System (100+)
4.1 Memory Mapping (25)
Flexible memory map configuration.

Define memory regions with start/end.

Region attributes (read, write, execute).

Region naming.

Region overlay (multiple banks).

Bank switching support.

Zero page region (0x0000‑0x00FF).

Stack region (0x0100‑0x01FF).

ROM region (read‑only).

RAM region (read/write).

I/O region (with callback).

Mirroring (aliasing).

Memory‑mapped I/O.

Video memory region.

Audio memory region.

Cartridge memory (banks).

RAM expansion (e.g., 16KB, 32KB).

Flash memory (write‑once).

EEPROM (byte‑writable).

Battery‑backed RAM (save states).

Configurable address bus width.

Data bus width (8/16 bit).

Endianness setting.

Memory protection (user/privileged).

Memory segmentation for 65816.

4.2 Memory Access Hooks (20)
Read hook (callback on read).

Write hook (callback on write).

Pre‑read hook.

Post‑write hook.

Read‑breakpoint (trigger on read).

Write‑breakpoint (trigger on write).

Access‑breakpoint (trigger on any access).

Read‑only protection (raise exception).

Write‑protected (raise exception).

Execute‑only (for code).

Access counting.

Access logging.

Access time simulation (wait states).

Read‑modify‑write hook.

DMA access hook.

Refresh cycle simulation.

Bus request/ack.

Memory page fault.

Memory exception.

Memory statistics collection.

4.3 Memory Operations (25)
Copy memory range.

Fill memory with pattern.

Search memory (byte, word, string).

Compare memory ranges.

Checksum calculation (CRC, MD5, SHA).

Memory encryption/decryption.

Memory compression/decompression.

Memory defragmentation (for banked).

Memory allocation (malloc/free).

Memory pool management.

Memory garbage collection (for dynamic).

Memory snapshot (save).

Memory restore (load).

Memory import from file.

Memory export to file.

Memory reset to initial state.

Memory watch (list of addresses).

Memory watch with highlight.

Memory bookmark (quick jump).

Memory annotation (comment per address).

Memory color coding (by region).

Memory heatmap (access frequency).

Memory change tracking.

Memory delta view.

Memory undo/redo (for manual edits).

4.4 Memory Visualization (15)
Hex viewer with ASCII.

Memory map graph.

Heatmap of usage.

Access pattern timeline.

Memory region editor.

Memory usage pie chart.

Memory fragmentation visualization.

Memory diff view (side‑by‑side).

Memory diff as unified format.

Memory search results list.

Memory bookmarks list.

Memory watch list with values.

Memory expression evaluation.

Memory inline editing.

Memory context menu (copy, fill, jump).

4.5 Memory Devices (15)
RAM (static/dynamic).

ROM (mask, EPROM, EEPROM).

Flash memory (NOR/NAND).

SRAM with battery backup.

VRAM (video RAM).

OAM (object attribute memory).

Palette memory.

SID/POKEY registers.

VIA/PIA registers.

ACIA (UART) registers.

Timer/counter registers.

DMA controller registers.

Interrupt controller registers.

Memory management unit (MMU).

Memory mapper (e.g., MMC1, MMC3 for NES).

5. Debugger (200+)
5.1 Breakpoints (30)
Address breakpoints (PC matches).

Data breakpoints (memory read/write).

Conditional breakpoints (expression).

Breakpoint hit count (stop after N hits).

Breakpoint ignore count (skip first N).

Temporary breakpoint (auto‑remove).

Log breakpoint (print, don't stop).

Breakpoint group.

Enable/disable individual breakpoints.

Breakpoint list with status.

Export/import breakpoints.

Breakpoint hit statistics.

Breakpoint color coding.

Breakpoint location in source.

Breakpoint on label.

Breakpoint on register change.

Breakpoint on stack change.

Breakpoint on interrupt.

Breakpoint on branch taken.

Breakpoint on instruction type.

Breakpoint on opcode.

Breakpoint on memory range.

Breakpoint with command script.

Breakpoint with audio/visual notification.

Breakpoint on cycle count.

Breakpoint on time.

Breakpoint on PC range.

Breakpoint on symbol resolution.

Breakpoint on build.

Breakpoint on program start.

5.2 Watchpoints (20)
Read watchpoint (memory read).

Write watchpoint (memory write).

Access watchpoint (any access).

Watchpoint on register (change).

Watchpoint on stack pointer.

Watchpoint on I/O port.

Conditional watchpoint.

Watchpoint on range.

Watchpoint hit count.

Watchpoint logging.

Watchpoint enable/disable.

Watchpoint group.

Watchpoint list.

Watchpoint export/import.

Watchpoint hit statistics.

Watchpoint color.

Watchpoint on DMA.

Watchpoint on IRQ.

Watchpoint on NMI.

Watchpoint on reset.

5.3 Execution Tracing (25)
Instruction trace (PC, opcode, operands).

Memory trace (address, data, R/W).

Register trace (changes).

Stack trace (push/pop).

Interrupt trace.

Branch trace.

Subroutine call/return trace.

I/O trace.

DMA trace.

Cycle‑accurate trace.

Trace with timestamps.

Trace filtering (by PC range, opcode, etc.).

Trace search.

Trace export (CSV, JSON, binary).

Trace import (for replay).

Trace visualization (timeline).

Trace bookmarks.

Trace annotations.

Trace playback (step through trace).

Trace comparison.

Trace compression.

Trace streaming (real‑time to file).

Trace statistics (instruction count, etc.).

Trace with source lines.

Trace with cycle count.

5.4 Time Travel (20)
Reverse execution (step back).

Rewind to previous instruction.

Forward replay.

History buffer size configurable.

History visual slider.

State snapshot at any point.

Jump to arbitrary history point.

Compare current state with history.

Show differences.

History search (find when register changed).

History export/import.

History compression.

History per‑instruction.

History per‑cycle.

History per‑memory access.

History with annotated events.

History timeline.

History bookmarks.

History undo/redo.

History as debugging aid.

5.5 Profiling (25)
Instruction execution count.

Cycle count per instruction.

Hot spot analysis (top addresses).

Call stack profiling.

Function call count.

Function execution time.

Branch prediction statistics.

Cache hit/miss simulation.

Memory access frequency.

I/O access frequency.

Interrupt frequency.

Code coverage analysis.

Coverage heatmap.

Profiler export (report).

Profiler visualization (flame graph).

Profiler sampling mode.

Profiler interval setting.

Profiler reset.

Profiler merge.

Profiler compare (two runs).

Profiler with source mapping.

Profiler with symbol names.

Profiler for specific PC range.

Profiler for specific time slice.

Profiler on‑the‑fly enable/disable.

5.6 Snapshots & Compare (20)
Take state snapshot (registers, memory).

Snapshot with name/description.

Snapshot list with timestamps.

Compare two snapshots (registers).

Compare two snapshots (memory).

Difference display (color‑coded).

Restore from snapshot.

Snapshot export/import.

Auto‑snapshot on build.

Auto‑snapshot on break.

Snapshot on timer.

Snapshot on demand.

Snapshot of only changed regions.

Snapshot compression.

Snapshot diff report.

Snapshot merge.

Snapshot delete.

Snapshot annotation.

Snapshot search.

Snapshot rollback.

5.7 Expression Evaluation (15)
Evaluate arithmetic expressions.

Evaluate bitwise expressions.

Evaluate logical expressions.

Access registers (A, X, Y, PC, SP, P).

Access memory ($addr).

Access flags (C, Z, etc.).

Access cycle count.

Access instruction count.

Access time.

Complex expressions with parentheses.

Conditional expressions.

String operations.

Built‑in functions (e.g., min, max, abs).

User‑defined variables.

Expression history.

5.8 Debugging Scripts (15)
Scripting language (Python, Lua, or custom).

Set breakpoints from script.

Set watchpoints from script.

Read/write memory from script.

Read/write registers from script.

Single‑step from script.

Continue execution from script.

Print/log from script.

Conditional execution.

Loops and branches.

Function definitions.

Script debugging (step through script).

Script library.

Script hotkeys.

Script on event (break, watch, trace).

5.9 Remote Debugging (15)
GDB remote protocol support.

Connect to remote target.

Download symbols to remote.

Remote memory access.

Remote register access.

Remote breakpoints.

Remote single‑step.

Remote continue.

Remote reset.

Remote status.

Remote logging.

Remote profiling.

Remote snapshot.

Remote authentication.

Remote encryption.

6. Peripherals & I/O (150+)
6.1 Display/Video (30)
Framebuffer display (e.g., 64x64, 256x240).

Character‑based display (e.g., 80x25 text).

Tile‑based graphics (like NES).

Bitmap modes.

Multiple color depths (1, 2, 4, 8 bits per pixel).

Palette support (customizable).

Sprite rendering.

Scrollable display.

Scanline/beam simulation.

Interlaced modes.

NTSC/PAL color emulation.

CRT filter (scanlines, bloom, curvature).

LCD grid filter.

Scaling filters (nearest, bilinear, HQx).

Screen rotation.

Screen mirroring.

Multi‑monitor support.

Full‑screen display.

Screenshot capture.

Video capture (AVI, GIF).

Frame rate limiter.

Frame skip.

V‑sync.

Display brightness/contrast.

Color temperature adjustment.

Gamma correction.

Pixel aspect ratio correction.

Show frame rate.

Show scanline position.

Overlay debug info on screen.

6.2 Audio (25)
Square wave generator.

Triangle wave generator.

Sawtooth wave generator.

Noise generator.

Pulse‑width modulation (PWM).

SID (6581/8580) emulation.

POKEY emulation.

AY‑3‑8910 emulation.

VIC‑20 sound.

NES APU (2x pulse, triangle, noise, DPCM).

SN76489 (TMS9919).

DAC (digital‑to‑analogue).

Audio sample rate conversion.

Audio buffer management.

Volume control.

Balance (left/right).

Equalizer.

Reverb/echo effects.

Audio recording (WAV).

Audio playback (from file).

MIDI output.

Audio visualizer.

Mute/unmute.

Audio latency adjustment.

Audio sync with video.

6.3 Keyboard & Input (25)
PS/2 keyboard emulation.

USB keyboard emulation.

Matrix keyboard (like C64).

Keyboard mapping (remap keys).

Keyboard macros.

Keyboard auto‑repeat.

Keyboard LED indicators.

Mouse emulation (via PS/2).

Joystick/gamepad support (USB).

Multiple joysticks.

Gamepad mapping.

Touch input (for tablets).

Gesture recognition.

Key logger (debug only).

Keyboard hook (global).

Input recording (macro).

Input playback.

Input scripts.

Keyboard shortcut override.

On‑screen keyboard.

Joypad emulation (digital/analog).

Light pen emulation.

Trackball emulation.

Mouse wheel support.

Input filtering (debounce).

6.4 Storage (25)
Tape storage (C64 .TAP).

Disk drive (1541, 1571, 1581) .D64, .D71, .D81.

Hard drive (IDE, SCSI) .HDI.

SD/MMC card (via SPI).

NVRAM (battery‑backed).

Flash ROM (save data).

Cartridge (ROM, RAM).

Bank switching mappers (MMC1, MMC3, UxROM, etc.).

Loading from file (drag‑and‑drop).

Saving to file.

Auto‑load from last session.

Disk directory view.

Disk editor (sector).

Disk copy.

Disk formatting.

Disk image conversion (D64 ↔ G64).

Tape controls (play, stop, rewind).

Tape speed adjustment.

Tape loading sound.

Tape turbo loader support.

Fast loading (skip header).

Write‑protect switch.

Disk drive speed (PAL/NTSC).

Disk drive sounds.

Disk drive status.

6.5 Communication (20)
Serial port (UART 6551).

Parallel port (Centronics).

SPI bus.

I2C bus.

Ethernet (via serial).

Wi‑Fi (ESP8266 emulation).

Bluetooth.

USB device.

MIDI in/out.

RS‑232 terminal.

Telnet server.

WebSocket server.

Modem emulation (AT commands).

IRDA.

CAN bus.

1‑Wire bus.

GPIO pins (bit‑banging).

Network packet capture.

Network statistics.

Port forwarding.

6.6 I/O Chips (20)
6522 VIA (Versatile Interface Adapter).

6520 PIA (Peripheral Interface Adapter).

6551 ACIA (Asynchronous Communication Interface).

6561 VIC (Video Interface Chip).

6581/8580 SID (Sound Interface Device).

6526 CIA (Complex Interface Adapter).

6532 TIA (Timer/IO).

6845 CRTC (Cathode Ray Tube Controller).

6850 ACIA.

6840 PTM (Programmable Timer).

6830 PIA.

6821 PIA.

8255 PPI (Programmable Peripheral Interface).

8253 PIT (Programmable Interval Timer).

8259 PIC (Programmable Interrupt Controller).

DMA controllers (e.g., 8237).

Floppy disk controller (8272).

Keyboard controller (8042).

RTC (Real‑Time Clock).

GPIO expander (MCP23017).

6.7 Bus & System (15)
Address bus (16‑bit, 24‑bit for 65816).

Data bus (8‑bit, 16‑bit).

Control bus (R/W, φ0, φ1, φ2, SYNC, RDY, RESET, IRQ, NMI).

Bus arbitration.

Bus multiplexing.

Bus wait states.

Bus timing diagrams.

Bus monitor.

Bus analyzer.

Bus logging.

Bus speed configuration.

Bus voltage levels (TTL, CMOS).

Bus termination.

Bus transceivers.

Bus buffers.

7. Project Management (80+)
7.1 Project Structure (20)
Create new project (name, location).

Open existing project.

Save project (with all settings).

Close project.

Project template (e.g., NES, C64, bare).

Add/remove source files.

Add/remove data files (binaries).

Project dependencies (link order).

Project include directories.

Project define symbols.

Project memory map configuration.

Project build configurations (Debug, Release).

Project custom build steps (pre/post).

Project build directory.

Project output directory.

Project export (zip).

Project import (unzip).

Project compare (diff).

Project merge.

Project statistics (lines, size).

7.2 Build System (20)
Build all.

Build selected.

Clean build.

Rebuild all.

Build with custom options.

Build with different CPU type.

Build with different memory map.

Build with optimization levels.

Build with listing output.

Build with symbol export.

Build with debug info.

Build with error/warning limits.

Build with include path.

Build with macro definitions.

Build with conditional assembly flags.

Build with output format selection.

Build with checksum.

Build with signing.

Build with encryption.

Build with compression.

7.3 Version Control (20)
Git integration (status, diff, commit, push).

SVN integration.

Show uncommitted changes.

Show file history.

Blame/annotation.

Branch management.

Tag management.

Staging area.

Conflict resolution.

Merge tool.

Diff view (side‑by‑side).

Patch creation.

Apply patch.

Clone repository.

Submodule support.

Ignore files.

LFS (Large File Storage).

Hooks (pre‑commit, post‑commit).

Remote management.

Authentication (SSH, HTTPS).

7.4 Documentation (20)
Generate API documentation.

Generate code comments.

Generate readme.

Generate changelog.

Generate user manual.

Generate developer guide.

Generate tutorial.

Generate examples.

Generate build instructions.

Generate release notes.

Generate license file.

Generate contribution guidelines.

Generate issue templates.

Generate pull request template.

Generate code of conduct.

Generate security policy.

Generate funding file.

Generate .gitignore.

Generate .editorconfig.

Generate .clang-format.

8. Tools & Utilities (80+)
8.1 Calculation Tools (20)
Hex/Bin/Dec converter.

Bitwise calculator.

Address calculator (with offset).

Checksum calculator (CRC, sum, etc.).

Hash calculator (MD5, SHA).

Base64 encoder/decoder.

Binary to text converter.

Text to binary converter.

Character set viewer.

Pixel art editor.

Tile editor.

Sprite editor.

Map editor.

Level editor.

Palette editor.

Sound editor.

Music tracker.

Waveform viewer.

Spectrum analyzer.

Logic analyzer.

8.2 Analysis Tools (20)
Code linter (static analysis).

Code complexity analysis.

Data flow analysis.

Control flow graph.

Dependency graph.

Call graph.

Instruction mix analysis.

Memory access pattern analysis.

Timing analysis (cycle per instruction).

Power consumption estimation.

Heat dissipation estimation.

Emulation performance analysis.

Memory fragmentation analysis.

Stack depth analysis.

Register usage analysis.

Constant propagation detection.

Unreachable code detection.

Redundant instruction detection.

Bottleneck detection.

Optimization suggestions.

8.3 Generation Tools (20)
Test case generator.

Random program generator.

Benchmark generator.

Code coverage report generator.

Profile report generator.

HTML report generator.

PDF report generator.

Graphviz graph generator.

SVG generator.

Waveform generator.

ROM header generator (NES, SNES).

Checksum generator.

Signature generator.

Encryption key generator.

Serialization generator.

Documentation generator.

Screenshot generator.

Video generator.

Audio generator.

Data table generator.

9. Performance & Optimization (60+)
9.1 Emulation Performance (20)
JIT compilation (translate to native code).

Instruction caching.

Block translation (basic block).

Optimized memory access (direct pointers).

CPU instruction prefetch.

Branch prediction.

Inline function calls.

Loop unrolling.

SIMD for frame processing.

GPU acceleration (OpenCL, CUDA).

Multi‑threading (separate CPU, audio, video).

Asynchronous I/O.

Reduced logging in release mode.

Memory pooling.

Object reuse.

Cache‑friendly data structures.

Profile‑guided optimization.

Link‑time optimization.

Binary translation (static recompilation).

Dynamic recompilation (Dynarec).

9.2 Profiling & Tuning (20)
Emulator performance profiler.

CPU usage monitor.

Memory usage monitor.

Disk I/O monitor.

Network I/O monitor.

Frame time monitor.

Audio buffer underrun detection.

Input lag measurement.

Response time measurement.

Overhead of debug features.

Toggle debug features for speed.

Adaptive speed (auto‑adjust).

Speed throttle (limit to 100%).

Turbo mode (unlimited speed).

Frame skip during slow down.

Audio resampling quality setting.

Video scaling quality setting.

Compress memory snapshots.

Optimize breakpoint evaluation.

Reduce tracing overhead.

9.3 Resource Management (20)
Memory limit setting.

CPU affinity (set core).

Priority adjustment.

Garbage collection triggers.

Cache size configuration.

Buffer size configuration.

Thread pool size.

Number of concurrent tasks.

Disk cache.

Memory mapping (file → memory).

Resource monitoring dashboard.

Resource usage warning.

Auto‑scale resources.

Resource cleanup on exit.

Resource leak detection.

Resource sharing.

Resource locking.

Resource pooling.

Resource prioritization.

Resource quotas.

10. Network & Collaboration (50+)
10.1 Remote Emulation (20)
Networked debugging (GDB stub).

Remote console access.

Remote file transfer.

Remote snapshot.

Remote control (start/stop/step).

Web‑based UI (HTML5).

REST API for emulator control.

WebSocket for real‑time events.

Multi‑user collaboration (shared session).

Peer‑to‑peer sync.

Cloud‑based storage of projects.

Cloud build service.

Cloud sharing of binaries.

Emulator as a service (EaaS).

Cluster emulation (distributed).

Docker container for emulator.

Virtual machine integration.

Network traffic replay.

Remote profiling.

Remote debugging session sharing.

10.2 Collaboration Features (15)
Pair programming (live share).

Code review comments.

Annotation and notes.

Shared breakpoints.

Shared watchpoints.

Chat window.

Voice chat.

Screen sharing.

Remote pointer.

Co‑debugging (two users control).

Version control integration for collaboration.

Project fork/merge.

Issue tracking.

Wiki integration.

Feedback loop.

10.3 Cloud Integration (15)
Save projects to cloud.

Save snapshots to cloud.

Save traces to cloud.

Cloud compilation.

Cloud testing (CI/CD).

Cloud deployment (web emulator).

Cloud backup.

Cloud restore.

Cloud share.

Cloud collaboration.

Cloud monitoring.

Cloud logging.

Cloud analytics.

Cloud update.

Cloud license management.

11. Documentation & Help (50+)
11.1 User Documentation (15)
User manual (PDF/HTML).

Quick start guide.

Tutorials (step‑by‑step).

Video tutorials.

Interactive tutorials (guided).

FAQ.

Troubleshooting guide.

Keyboard shortcut reference.

Command reference.

Configuration reference.

Project templates documentation.

Examples documentation.

Glossary.

Change log / release notes.

Update notifications.

11.2 Developer Documentation (15)
Architecture overview.

API reference (Doxygen).

Build instructions.

Contribution guidelines.

Coding standards.

Testing guidelines.

Design decisions.

Roadmap.

Technical debt list.

Performance tuning guide.

Security guidelines.

Compatibility guidelines.

Migration guide.

Extension guide.

Plugin development guide.

11.3 Reference Material (10)
6502 instruction set reference.

6502 addressing modes.

6502 register and flag descriptions.

Memory map reference.

I/O map reference.

Peripheral chip data sheets (summaries).

Assembler directive reference.

Debugger command reference.

CPU variant differences.

Known bugs and workarounds.

11.4 Learning Resources (10)
6502 programming tutorials.

Example programs with explanations.

Coding challenges.

Game development guides.

System programming guides.

Optimization tips.

Common pitfalls.

Best practices.

Community forums link.

Recommended reading.

12. Miscellaneous (50+)
12.1 Configuration & Settings (15)
Global settings (ini/registry).

Project‑specific settings.

User‑defined themes.

Custom fonts.

Default file types.

Auto‑save intervals.

Backup settings.

Log level settings.

Network settings.

Privacy settings.

Telemetry opt‑out.

Language selection.

Region/locale settings.

Accessibility settings.

Experimental features toggle.

12.2 Plugin System (15)
Plugin API (C/C++ DLLs).

Plugin manager (install/remove).

Plugin repository.

Plugin discovery.

Plugin sandbox.

Plugin versioning.

Plugin dependencies.

Plugin configuration.

Plugin enable/disable.

Plugin event hooks.

Plugin UI extension.

Plugin data persistence.

Plugin update.

Plugin security.

Plugin examples.

12.3 Logging & Auditing (10)
Comprehensive logging.

Log rotation.

Log compression.

Log encryption.

Log search.

Log analysis.

Audit trail.

Security event logging.

Performance event logging.

User activity logging.

12.4 Localization (10)
Internationalization (i18n) support.

Language packs.

Translators’ guide.

Date/time format.

Number format.

Currency format.

Text direction (RTL).

Unicode support.

Character set conversion.

Keyboard layout for each language.
)FEATURES";

std::vector<FeatureEntry> build() {
    std::vector<FeatureEntry> out;
    std::istringstream in(kFeatureText);
    std::string line, section;
    const std::regex heading(R"(^\s*\d+(?:\.\d+)?\s+.*$)");
    auto statusFor = [](const std::string &name) {
        static const char *implemented[] = {"Full-screen mode",
                                            "High DPI scaling",
                                            "Customizable theme",
                                            "Tabbed editor",
                                            "Show line numbers",
                                            "Syntax highlighting",
                                            "Find/Replace",
                                            "Go to line",
                                            "Live updating register values",
                                            "Status flags shown",
                                            "Copy register values",
                                            "Hex dump with ASCII view",
                                            "Search memory",
                                            "Edit bytes in place",
                                            "Fill memory range",
                                            "Memory snapshot",
                                            "Memory restore",
                                            "Memory import from file",
                                            "Memory export to file",
                                            "Disassembly with symbols",
                                            "Show instruction bytes",
                                            "Back/forward navigation history",
                                            "Set breakpoints",
                                            "Instruction trace",
                                            "Reverse execution",
                                            "Forward replay",
                                            "Instruction execution count",
                                            "Take state snapshot",
                                            "Compare two snapshots",
                                            "Evaluate arithmetic expressions",
                                            "IRQ vector",
                                            "NMI vector",
                                            "Reset vector",
                                            "Cycle-accurate emulation",
                                            "Page crossing penalties",
                                            "Stack operations",
                                            "Reset sequence",
                                            "BRK with 7 cycles",
                                            "JSR with 6 cycles",
                                            "RTS with 6 cycles",
                                            "RTI with 6 cycles",
                                            "Branch taken/not taken cycles",
                                            "Decimal mode timing",
                                            "RAM (static/dynamic)",
                                            "ROM (mask, EPROM, EEPROM)",
                                            "Memory-mapped I/O",
                                            "Framebuffer display",
                                            "Screenshot capture",
                                            "Frame rate limiter",
                                            "Serial port (UART 6551)",
                                            "6502 instruction set reference",
                                            "6502 addressing modes"};
        for (const char *x : implemented)
            if (name.find(x) != std::string::npos) return FeatureStatus::Implemented;
        static const char *partial[] = {"Docking",
                                        "Save and restore window layouts",
                                        "Multi-monitor awareness",
                                        "Auto-completion",
                                        "Code folding",
                                        "Multiple cursors",
                                        "Minimap",
                                        "Memory access coloring",
                                        "Memory map visualisation",
                                        "Illegal opcodes",
                                        "65C02 extensions",
                                        "65816 extensions",
                                        "Bank switching",
                                        "6522 VIA",
                                        "Display",
                                        "Audio",
                                        "Keyboard & Input",
                                        "Storage",
                                        "Communication",
                                        "Remote Debugging",
                                        "Project Management",
                                        "Plugin System"};
        for (const char *x : partial)
            if (name.find(x) != std::string::npos) return FeatureStatus::Partial;
        return FeatureStatus::Planned;
    };
    while (std::getline(in, line)) {
        if (line.size() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (std::regex_match(line, heading)) {
            section = line;
            continue;
        }
        // source file has feature items as plain lines
        out.push_back({section, line, statusFor(line)});
    }
    return out;
}
} // namespace

const std::vector<FeatureEntry> &FeatureCatalog::all() {
    static const auto data = build();
    return data;
}

std::vector<FeatureEntry> FeatureCatalog::search(const std::string &query) {
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::vector<FeatureEntry> out;
    for (const auto &e : all()) {
        std::string hay = e.section + " " + e.name;
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (q.empty() || hay.find(q) != std::string::npos) out.push_back(e);
    }
    return out;
}
std::string FeatureCatalog::statusName(FeatureStatus s) {
    switch (s) {
    case FeatureStatus::Implemented:
        return "implemented";
    case FeatureStatus::Partial:
        return "partial";
    default:
        return "planned";
    }
}

bool FeatureCatalog::exportText(const std::string &path, std::string &error) {
    std::ofstream f(path);
    if (!f) {
        error = "Could not create " + path;
        return false;
    }
    for (const auto &e : all())
        f << "[" << statusName(e.status) << "] " << e.section << " :: " << e.name << "\n";
    if (!f) {
        error = "Write failed";
        return false;
    }
    error = "Exported " + std::to_string(all().size()) + " features";
    return true;
}
bool FeatureCatalog::exportJson(const std::string &path, std::string &error) {
    std::ofstream f(path);
    if (!f) {
        error = "Could not create " + path;
        return false;
    }
    f << "[\n";
    for (size_t i = 0; i < all().size(); ++i) {
        const auto &e = all()[i];
        auto esc = [](std::string s) {
            std::string o;
            for (char c : s) {
                if (c == '\\' || c == '"') o += '\\';
                if (c == '\n' || c == '\r') {
                    o += ' ';
                    continue;
                }
                o += c;
            }
            return o;
        };
        f << "  {\"section\":\"" << esc(e.section) << "\",\"name\":\"" << esc(e.name) << "\",\"status\":\""
          << statusName(e.status) << "\"}" << (i + 1 < all().size() ? "," : "") << "\n";
    }
    f << "]\n";
    if (!f) {
        error = "Write failed";
        return false;
    }
    error = "Exported JSON feature catalog";
    return true;
}
