# Standalone XSim does not modify the open project; all paths target current sources.
set root [file normalize [file join [file dirname [info script]] ../..]]
if {[catch {exec python -I [file join $root scripts m2_real run_sim.py] >@stdout 2>@stderr} msg opts]} {
    return -options $opts $msg
}
