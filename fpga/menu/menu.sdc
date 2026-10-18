# The menu core's own timing exceptions (sys/sys_top.sdc has the framework's).

# The configuration bits from the Main (hps_io cfg: scanlines, VGA mode...)
# are set once and stay put: no timing between them and the video clock.
set_false_path -from [get_registers {*|hps_io:hps_io|cfg[*]*}]
