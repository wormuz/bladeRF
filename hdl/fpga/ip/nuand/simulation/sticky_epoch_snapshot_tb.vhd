library ieee;
use ieee.std_logic_1164.all;

entity sticky_epoch_snapshot_tb is
end entity;

architecture tb of sticky_epoch_snapshot_tb is
    signal clock         : std_logic := '0';
    signal reset         : std_logic := '1';
    signal start_toggle  : std_logic := '0';
    signal sticky_flags  : std_logic_vector(2 downto 0) := (others => '0');
    signal previous_fault : std_logic;
begin
    clock <= not clock after 5 ns;

    dut : entity work.sticky_epoch_snapshot
        generic map ( WIDTH => sticky_flags'length )
        port map ( clock => clock, reset => reset,
                   start_toggle => start_toggle,
                   sticky_flags => sticky_flags,
                   previous_fault => previous_fault );

    stimulus : process
    begin
        wait for 20 ns;
        wait until rising_edge(clock);
        reset <= '0';
        wait until rising_edge(clock);
        assert previous_fault = '0'
            report "reset must clear prior-epoch fault" severity failure;

        sticky_flags <= "010";
        wait until rising_edge(clock);
        start_toggle <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert previous_fault = '1'
            report "START must retain the fault present before START"
            severity failure;

        sticky_flags <= (others => '0');
        wait until rising_edge(clock);
        assert previous_fault = '1'
            report "prior-epoch fault must survive live sticky clear"
            severity failure;

        start_toggle <= '0';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert previous_fault = '0'
            report "next clean START must replace prior fault with clear state"
            severity failure;

        report "sticky_epoch_snapshot_tb: PASS" severity note;
        std.env.stop;
        wait;
    end process;
end architecture;
