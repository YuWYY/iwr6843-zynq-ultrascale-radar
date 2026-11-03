# Native proof of one reset release per epoch/destination domain. Read-only.
proc m2c::reset_fanin_cells {endpoint} {
    set endpoint [get_pins -quiet $endpoint]
    m2c::require {[llength $endpoint] == 1} {Missing/ambiguous physical reset boundary}
    set drivers [get_pins -quiet -leaf -of_objects [get_nets -segments -of_objects $endpoint] -filter {DIRECTION == OUT}]
    m2c::require {[llength $drivers] == 1} "Reset boundary has no unique leaf driver: $endpoint"
    set cell [get_cells -of_objects $drivers]
    if {[get_property IS_SEQUENTIAL $cell]} {
        m2c::require {[get_property REF_PIN_NAME $drivers] eq "Q"} "Reset boundary not driven by Q: $endpoint"
        set starts $drivers
    } else {
        set starts [all_fanin -flat -startpoints_only -to $drivers]
    }
    set cells {}
    foreach start $starts {
        set c [get_cells -quiet -of_objects $start]
        m2c::require {[llength $c] == 1 && [get_property IS_SEQUENTIAL $c]} "Reset has nonregistered source: $endpoint / $start"
        lappend cells [get_property NAME $c]
    }
    m2c::require {[llength $cells] > 0} "Reset has no source registers: $endpoint"
    return [lsort -unique $cells]
}
# Rebuilt synthesis hierarchy can rename/remove RTL input ports. Select reset
# consumers independently by module, destination clock and register role, then
# prove every physical reset pin's source. Never select by the desired fanout.
proc m2c::shared_consumer_pins {base relative clock} {
    set module [file dirname $relative]
    set port [file tail $relative]
    set scope $base/$module
    if {$port eq "cal_enable"} {
        set pins [get_pins -quiet [list $scope/cal_enable $scope/cal_enable_ref]]
        m2c::require {[llength $pins] == 1} "Missing/ambiguous REF epoch supervisor enable: $scope"
        return $pins
    }
    if {$port eq "allow_n"} {
        set regs [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${scope}/released_reg* && IS_SEQUENTIAL == 1"]
    } else {
        set regs [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${scope}/* && IS_SEQUENTIAL == 1"]
    }
    set selected {}; set pins {}; set stems {}
    foreach reg $regs {
        set name [get_property NAME $reg]
        set tail [string range $name [expr {[string length $scope]+1}] end]
        # Receive subblocks have their own functional reset terms. The outer
        # command state and abort synchronizer are the shared-reset consumers.
        if {$module eq "u_receive" && [string first / $tail] >= 0 && ![string match u_abort/* $tail]} {continue}
        # Observation generation deliberately survives a local delay epoch.
        if {$module eq "u_phy/u_delay_ctrl" && [string match observation_generation_reg* $tail]} {continue}
        set actual [get_clocks -quiet -of_objects [get_pins -of_objects $reg -filter {REF_PIN_NAME == C}]]
        if {$actual ne $clock} {continue}
        set reset [get_pins -quiet -of_objects $reg -filter {REF_PIN_NAME == CLR || REF_PIN_NAME == PRE}]
        m2c::require {[llength $reset] == 1} "Shared reset consumer lost asynchronous reset: $name"
        lappend selected $reg; lappend pins {*}$reset; lappend stems $tail
    }
    m2c::require {[llength $selected] > 0} "Missing shared reset consumer registers: $relative"
    # Stable, mandatory anchors prevent an optimized or changed endpoint from
    # silently reducing a shared boundary to unrelated surviving registers.
    set anchors {}
    switch -- $port {
        src_resetn_in {set anchors {req_toggle_reg ack_ff_reg}}
        dst_resetn_in {set anchors {ack_toggle_reg req_ff_reg dst_valid_reg}}
        sys_resetn_in {set anchors {req_toggle_reg ack_ff_reg data_reg generation_reg valid_reg}}
        rx_resetn_in {set anchors {ack_toggle_reg req_ff_reg rx_hold_reg}}
        common_resetn {set anchors {complete_reg fault_reg idle_observed_reg}}
        resetn_rx {set anchors {ready_reg executing_reg en_vtc_reg}}
        arstn {set anchors {sync_ff_reg}}
        allow_n {set anchors {released_reg}}
        default {error "Unreviewed shared reset consumer kind: $relative"}
    }
    foreach anchor $anchors {
        set found [lsearch -glob -all $stems ${anchor}*]
        m2c::require {[llength $found] > 0} "Missing reset consumer anchor: $relative / $anchor"
        if {$anchor in {ack_ff_reg req_ff_reg sync_ff_reg}} {
            set stages {}
            foreach index $found {lappend stages [lindex $selected $index]}
            m2c::require {[m2c::bit_set $stages $anchor] eq {0 1 2}} "Shared reset synchronizer incomplete: $relative / $anchor"
        }
    }
    return $pins
}
proc m2c::check_shared_reset {base clocks contract} {
    set expected {}
    foreach owner [dict get $contract reset_xpm_owners] {lappend expected $base/$owner/g_async.u_reset_sync}
    set expected [lsort $expected]
    set actual [lsort [get_property NAME [get_cells -hier -quiet -filter "NAME =~ ${base}/*u_reset_sync"]]]
    m2c::require {$actual eq $expected && [llength $actual] > 0} "Unexpected/missing reset XPM parent (internal mailbox helpers forbidden): expected=$expected actual=$actual"
    set inventory {}
    foreach parent $actual {
        set stages [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${parent}/arststages_ff_reg* && IS_SEQUENTIAL == 1"]
        m2c::require {[m2c::bit_set $stages arststages_ff_reg] eq {0 1 2}} "Wrong XPM reset depth: $parent"
        foreach reg $stages {m2c::require {[get_property ASYNC_REG $reg]} "Missing reset ASYNC_REG: $reg"}
        set clock [get_clocks -quiet -of_objects [get_pins -of_objects $stages -filter {REF_PIN_NAME == C}]]
        m2c::require {[llength $clock] == 1} "Reset stages do not share one clock: $parent"
        set raw [get_pins -quiet $parent/src_arst]
        m2c::require {[llength $raw] == 1} "Missing raw reset boundary: $parent"
        dict set inventory $parent [dict create clock $clock sources [m2c::reset_fanin_cells $raw]]
    }
    set rows {}; set names {}
    foreach group [dict get $contract shared_reset_groups] {
        set name [dict get $group name]; lappend names $name
        set owner $base/[dict get $group owner]
        set parent $owner/g_async.u_reset_sync
        set clock [m2c::clock_role [dict get $group clock] $clocks]
        set source_pattern $base/[dict get $group source]*
        # Detect any second raw chain receiving this source in the same domain,
        # including an accidental old endpoint-local reset synchronizer.
        set chains {}
        dict for {candidate info} $inventory {
            if {[dict get $info clock] ne $clock} {continue}
            foreach source [dict get $info sources] {
                if {[string match $source_pattern $source]} {lappend chains $candidate; break}
            }
        }
        m2c::require {$chains eq [list $parent]} "Shared reset has missing/duplicate raw chain: $name / $chains"
        set raw_sources [m2c::reset_sources [get_pins $parent/src_arst] $source_pattern [dict get $clocks SYS]]
        set regs [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${owner}/released_reg* && IS_SEQUENTIAL == 1"]
        m2c::require {[llength $regs] > 0} "Missing shared release register: $owner"
        foreach reg $regs {
            m2c::require {[get_clocks -of_objects [get_pins -of_objects $reg -filter {REF_PIN_NAME == C}]] eq $clock} "Reset owner has wrong clock: $reg"
        }
        set pins {}
        foreach relative [dict get $group consumers] {
            lappend pins {*}[m2c::shared_consumer_pins $base $relative $clock]
        }
        if {[dict exists $group register_consumers]} {
            foreach item [dict get $group register_consumers] {
                set stem [dict get $item stem]
                set cs [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${base}/${stem}* && IS_SEQUENTIAL == 1"]
                m2c::require {[m2c::bit_set $cs [file tail $stem]] eq [dict get $item bits]} "Missing/extra shared reset register consumers: $name / $stem"
                set cp [get_pins -of_objects $cs -filter "REF_PIN_NAME == [dict get $item pin]"]
                m2c::require {[llength $cp] == [llength $cs]} "Missing register reset pins: $name / $stem"
                lappend pins {*}$cp
            }
        }
        m2c::require {[llength $pins] > 0 && [llength [lsort -unique $pins]] == [llength $pins]} "Empty/duplicate shared reset consumer: $name"
        set consumers {}; set pin_names {}
        foreach pin $pins {
            set pin [get_pins -quiet $pin]
            m2c::require {[llength $pin] == 1} {Missing/ambiguous physical shared-reset consumer}
            set sources [m2c::reset_sources $pin $owner/released_reg* $clock]
            set pn [get_property NAME $pin]; lappend pin_names $pn
            lappend consumers "\{\"pin\":[m2c::json_string $pn],\"source_cells\":[m2c::json_array $sources]\}"
        }
        lappend rows "\{\"name\":[m2c::json_string $name],\"owner\":[m2c::json_string $owner],\"clock\":[m2c::json_string $clock],\"raw_chain_count\":[llength $chains],\"raw_source_cells\":[m2c::json_array $raw_sources],\"expected_consumers\":[m2c::json_array $pin_names],\"consumers\":\[[join $consumers ,]\]\}"
    }
    m2c::require {[llength $names] == 8 && [llength [lsort -unique $names]] == 8} {Shared reset group population changed without review}
    set local_rows {}
    foreach item [dict get $contract prequalified_reset_owners] {
        set owner $base/[dict get $item owner]
        set clock [m2c::clock_role [dict get $item clock] $clocks]
        m2c::require {[llength [get_cells -quiet $owner/g_async.u_reset_sync]] == 0} "Prequalified HOLD must not resynchronize: $owner"
        set regs [get_cells -hier -quiet -include_replicated_objects * -filter "NAME =~ ${owner}/released_reg* && IS_SEQUENTIAL == 1"]
        set inputs [get_pins -quiet -of_objects $regs -filter {REF_PIN_NAME == CLR}]
        m2c::require {[llength $inputs] > 0 && [llength $inputs] == [llength $regs]} "Missing prequalified HOLD reset boundary: $owner"
        set sources {}
        foreach input $inputs {lappend sources {*}[m2c::reset_fanin_cells $input]}
        set sources [lsort -unique $sources]
        set wanted [dict get $item sources]; set found {}
        foreach source $sources {
            set matched 0
            foreach stem $wanted {if {[string match $base/${stem}* $source]} {lappend found $stem; set matched 1}}
            m2c::require {$matched} "Prequalified HOLD has unreviewed source: $owner / $source"
            m2c::require {[get_clocks -of_objects [get_pins -of_objects [get_cells $source] -filter {REF_PIN_NAME == C}]] eq $clock} "Prequalified HOLD crosses domains: $owner / $source"
        }
        m2c::require {[lsort -unique $found] eq [lsort $wanted]} "Missing prequalified source: $owner"
        lappend local_rows "\{\"owner\":[m2c::json_string $owner],\"clock\":[m2c::json_string $clock],\"source_cells\":[m2c::json_array $sources],\"raw_xpm_count\":0\}"
    }
    return "\{\"status\":\"PASS\",\"xpm_parents\":[m2c::json_array $actual],\"expected_xpm_parents\":[m2c::json_array $expected],\"groups\":\[[join $rows ,]\],\"prequalified\":\[[join $local_rows ,]\]\}"
}
