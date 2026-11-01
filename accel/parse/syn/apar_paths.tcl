# Worst setup paths of the synthesis-only project (run by the CI workflow)
project_open apar
create_timing_netlist
read_sdc
update_timing_netlist
report_timing -setup -npaths 3000 -nworst 1 -detail summary -file apar_paths.txt
report_clock_fmax_summary -file apar_fmax.txt
