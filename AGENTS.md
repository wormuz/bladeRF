# Project command environment

This checkout is the bladeRF source used by the adjacent `sdr-scanner`
project. Run builds, tests, Quartus, NIOS, and project utilities through the
scanner environment guard:

```sh
../sdr-scanner/tools/with-project-env <command> [args...]
```

Use the absolute path `/home/bonho/projects/sdr-scanner/tools/with-project-env`
when running from outside this checkout. The wrapper loads both this checkout's
`.envrc` and the scanner `.envrc`, then verifies the Quartus, NIOS GCC/BSP, and
GHDL tools required by this project. Non-interactive tool shells do not
reliably load the direnv hook from `.bashrc`; do not invoke project tools from
an uninitialized shell. If the guard reports missing tools, inspect
`direnv status`, fix `ALTERA_ROOT`/tool paths or approve both project `.envrc`
files with `direnv allow`, then rerun through the guard.
