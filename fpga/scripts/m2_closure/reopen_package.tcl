# Explicit disposable-copy native verification. Never launch against mainline.
# Vivado 2025.1 -mode batch -source this-file -log OUT/native.log
#   -journal OUT/native.jou -tclargs ABS_EXTRACTED_ROOT ABS_NEW_REPORT_DIR
# The extracted root contains project/, evidence/, README.md and manifest.
# The reused native verifier opens normally (writable), force-validates the BD,
# checks protected IP parameters/topology/reference paths, then closes unsaved.
# No synthesis, generation, bit/XSA export, downloads or cleanup occurs.
if {[llength $argv] != 2} {error {Expected absolute extracted root and outside report directory}}
set extracted [file normalize [lindex $argv 0]]
set reports [file normalize [lindex $argv 1]]
foreach arg $argv {if {[file pathtype $arg] ne "absolute"} {error {Use explicit absolute paths}}}
if {[llength [current_project -quiet]]} {error {Fresh Vivado process required}}
if {[version -short] ne "2025.1"} {error {Vivado 2025.1 required}}
proc closure_inside {path root} {
    set path [string tolower [string map {\\ /} [file normalize $path]]]
    set root [string trimright [string tolower [string map {\\ /} [file normalize $root]]] /]
    return [expr {$path eq $root || [string first "$root/" $path]==0}]
}
set source_root [file normalize [file join [file dirname [info script]] ../..]]
if {[closure_inside $extracted $source_root] || [closure_inside $reports $source_root] || [closure_inside $reports $extracted]} {
    error {Refusing mainline or reports inside project/extraction trees}
}
set helper [file join [file dirname [info script]] package_delivery.py]
puts [exec python -I -B $helper verify-tree --tree $extracted --reports $reports]
set copy [file join $extracted project]
set config [file join $extracted evidence diff before config.tcl]
set library [file join $copy scripts m2_real reopen_package.tcl]
foreach required [list $config [file join [file dirname $config] networks.tsv] $library] {
    if {![file isfile $required]} {error "Missing native reopen dependency: $required"}
}
# Reuse established 2025.1 object/CONFIG/network checks, with explicit packaged
# baseline rather than its historical default delivery path. No mainline helper
# open/project entry is sourced; this verifier refuses original-mainline refs.
set argv [list $copy $reports $config]
source $library
