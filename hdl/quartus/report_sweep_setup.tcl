# Reproduce the worst sweep setup paths in the adverse cold corner.
# Run from hdl/quartus/work/bladerf-micro-A4-sweep:
#   ~/soft/q25 quartus_sta --64bit -t ../../report_sweep_setup.tcl

project_open -revision sweep bladerf
create_timing_netlist -model slow -temperature 0 -voltage 1100
read_sdc
update_timing_netlist
report_timing -setup -npaths 20 -detail full_path -file sweep_setup_paths.rpt
project_close
puts "Sweep setup paths written to sweep_setup_paths.rpt"
