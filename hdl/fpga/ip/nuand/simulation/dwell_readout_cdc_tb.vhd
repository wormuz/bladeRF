-- Exercise the real bundled-data crossing used by the sweep dwell readout.
-- The source analyser and NIOS/system domain intentionally use unrelated
-- clock periods so a test cannot pass by assuming a common sampling edge.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity dwell_readout_cdc_tb is
end entity;

architecture sim of dwell_readout_cdc_tb is
    signal rx_clock          : std_logic := '0';
    signal sys_clock         : std_logic := '0';
    signal rx_reset          : std_logic := '1';
    signal sys_reset         : std_logic := '1';
    signal summary_valid     : std_logic := '0';
    signal rd_index          : unsigned(3 downto 0) := (others => '0');
    signal rd_data_rx        : std_logic_vector(31 downto 0);
    signal rd_data_hold      : std_logic_vector(31 downto 0);
    signal rd_data_sys       : std_logic_vector(31 downto 0) := (others => '0');
    signal req_sys           : std_logic := '0';
    signal ack_sys           : std_logic;
    signal generation        : unsigned(15 downto 0);
    signal energy_sum        : unsigned(63 downto 0) := (others => '0');
    signal peak              : unsigned(31 downto 0) := (others => '0');
    signal clip_count        : unsigned(31 downto 0) := (others => '0');
    signal sample_count      : unsigned(31 downto 0) := (others => '0');
    signal first_timestamp   : unsigned(63 downto 0) := (others => '0');
    signal first_window      : unsigned(15 downto 0) := (others => '0');
    signal mean_power        : unsigned(31 downto 0) := (others => '0');
    signal noise_floor       : unsigned(47 downto 0) := (others => '0');
    signal peak_window       : unsigned(47 downto 0) := (others => '0');
    signal triggered         : std_logic := '0';
    signal measure_valid     : std_logic := '0';
    signal gain_too_high     : std_logic := '0';
    signal settle_elapsed    : unsigned(15 downto 0) := (others => '0');
    signal done              : boolean := false;
begin
    rx_clock <= not rx_clock after 3.5 ns when not done else '0';
    sys_clock <= not sys_clock after 5.5 ns when not done else '0';

    U_readout : entity work.dwell_readout
        port map (
            clock => rx_clock,
            reset => rx_reset,
            summary_valid => summary_valid,
            energy_sum => energy_sum,
            peak => peak,
            clip_count => clip_count,
            sample_count => sample_count,
            first_timestamp => first_timestamp,
            first_window => first_window,
            mean_power => mean_power,
            noise_floor => noise_floor,
            peak_window => peak_window,
            triggered => triggered,
            measure_valid => measure_valid,
            gain_too_high => gain_too_high,
            settle_elapsed => settle_elapsed,
            rd_index => rd_index,
            rd_data => rd_data_rx,
            generation => generation
        );

    U_crossing : entity work.handshake
        generic map ( DATA_WIDTH => 32 )
        port map (
            source_reset => rx_reset,
            source_clock => rx_clock,
            source_data => rd_data_rx,
            dest_reset => sys_reset,
            dest_clock => sys_clock,
            dest_data => rd_data_hold,
            dest_req => req_sys,
            dest_ack => ack_sys
        );

    drive_req : process(sys_clock, sys_reset)
    begin
        if sys_reset = '1' then
            req_sys <= '0';
            rd_data_sys <= (others => '0');
        elsif rising_edge(sys_clock) then
            if ack_sys = '0' then
                req_sys <= '1';
            else
                req_sys <= '0';
                rd_data_sys <= rd_data_hold;
            end if;
        end if;
    end process;

    stim : process
        procedure settle_transfer is
        begin
            for i in 1 to 20 loop
                wait until rising_edge(sys_clock);
            end loop;
        end procedure;
    begin
        for i in 1 to 5 loop
            wait until rising_edge(rx_clock);
        end loop;
        rx_reset <= '0';
        sys_reset <= '0';
        settle_transfer;

        rd_index <= to_unsigned(0, 4);
        energy_sum <= x"AAAABBBB_CCCCDDDD";
        summary_valid <= '1';
        wait until rising_edge(rx_clock);
        summary_valid <= '0';
        settle_transfer;
        assert rd_data_sys = x"CCCCDDDD"
            report "CDC failed to deliver the selected dwell word"
            severity error;
        assert generation = 1
            report "source generation did not advance with the dwell"
            severity error;

        -- Change the selected source word while transfers continue. The
        -- destination must converge to the new complete word, never expose
        -- the asynchronous source bus directly to the system clock.
        rd_index <= to_unsigned(2, 4);
        peak <= x"DEADBEEF";
        summary_valid <= '1';
        wait until rising_edge(rx_clock);
        summary_valid <= '0';
        settle_transfer;
        assert rd_data_sys = x"DEADBEEF"
            report "CDC failed to deliver the next selected dwell word"
            severity error;
        assert generation = 2
            report "source generation lost a completed dwell"
            severity error;

        report "dwell_readout_cdc_tb: PASS";
        done <= true;
        wait;
    end process;
end architecture;
