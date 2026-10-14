# Lists the slowest paths of the compiled menu core (CI log).
project_open menu
create_timing_netlist
read_sdc
update_timing_netlist
foreach corner {slow_100c slow_n40c} {
	set_operating_conditions -model slow -temperature [expr {$corner eq "slow_100c" ? 100 : -40}] -voltage 1100
	update_timing_netlist
	report_timing -setup -npaths 8 -detail summary -stdout
}
delete_timing_netlist
project_close
