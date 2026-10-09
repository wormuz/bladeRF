library ieee;
    use ieee.std_logic_1164.all;
library std;
    use std.env.all;

entity if_clock_watchdog_tb is
end entity;

architecture test of if_clock_watchdog_tb is
    signal system_clock : std_logic := '0';
    signal interface_clock : std_logic := '0';
    signal interface_clock_enable : std_logic := '0';
    signal alive : std_logic;
begin
    system_clock <= not system_clock after 5 ns;

    interface_clock_gen : process
    begin
        wait for 5 ns;
        if interface_clock_enable = '1' then
            interface_clock <= not interface_clock;
        else
            interface_clock <= '0';
        end if;
    end process;

    dut : entity work.if_clock_watchdog
        generic map (TIMEOUT_CYCLES => 16)
        port map (
            system_clock => system_clock,
            interface_clock => interface_clock,
            alive => alive
        );

    test_process : process
    begin
        wait for 500 ns;
        assert alive = '0'
            report "watchdog accepted a stopped interface clock"
            severity failure;

        interface_clock_enable <= '1';
        wait for 3 us;
        assert alive = '1'
            report "watchdog did not detect a running interface clock"
            severity failure;

        interface_clock_enable <= '0';
        wait for 250 ns;
        assert alive = '0'
            report "watchdog did not expire after interface clock stopped"
            severity failure;

        interface_clock_enable <= '1';
        wait for 3 us;
        assert alive = '1'
            report "watchdog did not recover after interface clock resumed"
            severity failure;

        report "if_clock_watchdog_tb: PASS" severity note;
        stop;
        wait;
    end process;
end architecture;
