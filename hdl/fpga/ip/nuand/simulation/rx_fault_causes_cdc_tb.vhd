library ieee;
use ieee.std_logic_1164.all;
use std.env.all;

entity rx_fault_causes_cdc_tb is
end entity;

architecture test of rx_fault_causes_cdc_tb is
    signal rx_clock          : std_logic := '0';
    signal sys_clock         : std_logic := '0';
    signal rx_reset          : std_logic := '1';
    signal sys_reset         : std_logic := '1';
    signal fault_source      : std_logic_vector(4 downto 0) := (others => '0');
    signal source_holding    : std_logic_vector(4 downto 0);
    signal request           : std_logic := '0';
    signal acknowledge       : std_logic;
    signal received          : std_logic_vector(4 downto 0) := (others => '0');
begin
    rx_clock  <= not rx_clock after 5 ns;
    sys_clock <= not sys_clock after 7 ns;

    crossing : entity work.handshake
        generic map (DATA_WIDTH => fault_source'length)
        port map (
            source_reset => rx_reset,
            source_clock => rx_clock,
            source_data  => fault_source,
            dest_reset   => sys_reset,
            dest_clock   => sys_clock,
            dest_data    => source_holding,
            dest_req     => request,
            dest_ack     => acknowledge
        );

    receive_snapshot : process(sys_clock, sys_reset)
    begin
        if sys_reset = '1' then
            received <= (others => '0');
        elsif rising_edge(sys_clock) then
            if acknowledge = '1' then
                received <= source_holding;
            end if;
        end if;
    end process;

    check : process
    begin
        wait for 31 ns;
        rx_reset <= '0';
        sys_reset <= '0';

        -- These are the simultaneous sticky causes that can be latched by
        -- one RX-clock edge. NIOS must observe a coherent vector, not a
        -- combination assembled from independently synchronized bits.
        fault_source <= "10101";
        request <= '1';
        wait until acknowledge = '1';
        wait until rising_edge(sys_clock);
        wait for 1 ns;
        assert received = "10101"
            report "first fault snapshot crossed incoherently" severity failure;

        -- Source changes after the handshake captured its holding register.
        -- The in-flight destination snapshot must remain stable until ACK.
        fault_source <= "11111";
        wait for 2 ns;
        assert source_holding = "10101"
            report "source change corrupted an in-flight snapshot"
            severity failure;
        request <= '0';
        wait until acknowledge = '0';

        request <= '1';
        wait until acknowledge = '1';
        wait until rising_edge(sys_clock);
        wait for 1 ns;
        assert received = "11111"
            report "next fault snapshot did not include newly sticky causes"
            severity failure;
        request <= '0';
        wait until acknowledge = '0';

        -- Clearing on a new stream epoch is also a coherent status update.
        fault_source <= "00000";
        request <= '1';
        wait until acknowledge = '1';
        wait until rising_edge(sys_clock);
        wait for 1 ns;
        assert received = "00000"
            report "fault clear snapshot crossed incoherently" severity failure;

        report "rx_fault_causes_cdc_tb: PASS" severity note;
        finish;
    end process;
end architecture;
