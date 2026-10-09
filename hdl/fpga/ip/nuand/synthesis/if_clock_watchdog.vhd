library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

-- Monitors an AD9361 interface clock from the independent FPGA system clock.
-- The divided toggle keeps the crossing rate low and the two-stage
-- synchronizer prevents metastable state from reaching the watchdog logic.
entity if_clock_watchdog is
  generic (
    TIMEOUT_CYCLES : positive := 100000
  );
  port (
    system_clock    : in  std_logic;
    interface_clock : in  std_logic;
    alive           : out std_logic
  );
end entity;

architecture rtl of if_clock_watchdog is
    signal divider       : unsigned(3 downto 0) := (others => '0');
    signal source_toggle : std_logic := '0';
    signal sync_meta     : std_logic := '0';
    signal sync_state    : std_logic := '0';
    signal last_toggle  : std_logic := '0';
    signal seen_toggle  : std_logic := '0';
    signal toggle_count : natural range 0 to 8 := 0;
    signal age           : natural range 0 to TIMEOUT_CYCLES := TIMEOUT_CYCLES;

    attribute ALTERA_ATTRIBUTE : string;
    attribute PRESERVE : boolean;
    attribute ALTERA_ATTRIBUTE of rtl : architecture is
        "-name SDC_STATEMENT ""set_false_path -to [get_registers {*if_clock_watchdog:*|sync_meta}] """;
    attribute ALTERA_ATTRIBUTE of sync_meta : signal is
        "-name SYNCHRONIZER_IDENTIFICATION ""FORCED IF ASYNCHRONOUS"" ; -name DONT_MERGE_REGISTER ON ; -name PRESERVE_REGISTER ON ; -name ADV_NETLIST_OPT_ALLOWED NEVER_ALLOW";
    attribute ALTERA_ATTRIBUTE of sync_state : signal is
        "-name SYNCHRONIZER_IDENTIFICATION ""FORCED IF ASYNCHRONOUS"" ; -name DONT_MERGE_REGISTER ON ; -name PRESERVE_REGISTER ON ; -name ADV_NETLIST_OPT_ALLOWED NEVER_ALLOW";
    attribute PRESERVE of sync_meta : signal is TRUE;
    attribute PRESERVE of sync_state : signal is TRUE;
begin
    -- Toggle once per sixteen source-clock edges so even a fast interface
    -- clock is sampled reliably by the slower system-clock domain.
    process(interface_clock)
    begin
        if rising_edge(interface_clock) then
            if divider = 15 then
                divider <= (others => '0');
                source_toggle <= not source_toggle;
            else
                divider <= divider + 1;
            end if;
        end if;
    end process;

    process(system_clock)
    begin
        if rising_edge(system_clock) then
            sync_meta <= source_toggle;
            sync_state <= sync_meta;
            if sync_state /= last_toggle then
                last_toggle <= sync_state;
                if toggle_count < 8 then
                    toggle_count <= toggle_count + 1;
                    if toggle_count = 7 then
                        seen_toggle <= '1';
                    end if;
                end if;
                age <= 0;
            elsif age < TIMEOUT_CYCLES then
                age <= age + 1;
                if age = TIMEOUT_CYCLES - 1 then
                    seen_toggle <= '0';
                    toggle_count <= 0;
                end if;
            end if;
        end if;
    end process;

    alive <= '1' when seen_toggle = '1' and age < TIMEOUT_CYCLES else '0';
end architecture;
