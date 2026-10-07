library ieee;
use ieee.std_logic_1164.all;
use std.env.all;

entity rx_trigger_arm_cdc_tb is
end entity;

architecture test of rx_trigger_arm_cdc_tb is
    signal rx_clock       : std_logic := '0';
    signal trigger_arm    : std_logic := '0';
    signal trigger_armed  : std_logic;
begin
    rx_clock <= not rx_clock after 5 ns;

    dut : entity work.reset_synchronizer
        generic map (
            INPUT_LEVEL  => '0',
            OUTPUT_LEVEL => '0'
        )
        port map (
            clock => rx_clock,
            async => trigger_arm,
            sync  => trigger_armed
        );

    check : process
    begin
        wait for 2 ns;
        assert trigger_armed = '0'
            report "RX trigger must start disarmed" severity failure;

        trigger_arm <= '1';
        wait until rising_edge(rx_clock);
        wait for 1 ns;
        assert trigger_armed = '0'
            report "RX trigger arm must not cross on the first sample clock"
            severity failure;
        wait until rising_edge(rx_clock);
        wait for 1 ns;
        assert trigger_armed = '0'
            report "RX trigger arm must not cross on the second sample clock"
            severity failure;
        wait until rising_edge(rx_clock);
        wait for 1 ns;
        assert trigger_armed = '1'
            report "RX trigger arm must release after synchronized clocks"
            severity failure;

        trigger_arm <= '0';
        wait for 1 ns;
        assert trigger_armed = '0'
            report "RX trigger disarm must assert asynchronously"
            severity failure;

        report "rx_trigger_arm_cdc_tb: PASS" severity note;
        finish;
    end process;
end architecture;
