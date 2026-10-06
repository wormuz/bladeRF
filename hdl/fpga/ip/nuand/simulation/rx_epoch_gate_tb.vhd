library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;
    use std.env.all;

library work;
    use work.fifo_readwrite_p.all;

entity rx_epoch_gate_tb is
end entity;

architecture test of rx_epoch_gate_tb is
    signal clock : std_logic := '0';
    signal reset : std_logic := '1';
    signal controls_in : sample_controls_t(0 to 1) := (others => SAMPLE_CONTROL_ENABLE);
    signal samples_in : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal timestamp : unsigned(63 downto 0) := (others => '0');
    signal arm, complete, abort : std_logic := '0';
    signal epoch_id_in : unsigned(7 downto 0) := x"2A";
    signal out_controls : sample_controls_t(0 to 1);
    signal out_samples : sample_streams_t(0 to 1);
    signal epoch_id : unsigned(7 downto 0);
    signal state : unsigned(3 downto 0);
    signal discard_active, start_event : std_logic;
    signal first_valid_timestamp : unsigned(63 downto 0);
begin
    clock <= not clock after 5 ns;
    dut : entity work.rx_epoch_gate
        port map (
            clock => clock, reset => reset,
            in_sample_controls => controls_in, in_samples => samples_in,
            rx_timestamp => timestamp,
            epoch_arm => arm, epoch_complete => complete,
            epoch_abort => abort, epoch_id_in => epoch_id_in,
            settle_samples_in => to_unsigned(1000000, 32),
            out_sample_controls => out_controls, out_samples => out_samples,
            out_epoch_id => epoch_id, out_state => state,
            out_discard_active => discard_active,
            epoch_start_event => start_event,
            first_valid_timestamp => first_valid_timestamp
        );

    test_process : process
    begin
        wait for 20 ns;
        wait until falling_edge(clock);
        reset <= '0';

        -- A failed RFIC operation may have changed the LO partially. ABORT
        -- must therefore invalidate the data path instead of reopening old
        -- samples under the candidate epoch ID.
        wait until falling_edge(clock);
        epoch_id_in <= x"29";
        arm <= '1';
        wait until falling_edge(clock);
        arm <= '0';
        wait until falling_edge(clock);
        abort <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '0'
            report "ABORT admitted samples from the failed epoch" severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0100" and discard_active = '1'
            report "ABORT did not leave the gate fail-closed" severity failure;
        abort <= '0';

        -- Recovery is explicit: a new ARM leaves ERROR only by starting a
        -- fresh fenced epoch.
        wait until falling_edge(clock);
        epoch_id_in <= x"2A";
        arm <= '1';
        wait until falling_edge(clock);
        arm <= '0';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0001" and discard_active = '1'
            report "new ARM did not recover from ERROR into PENDING" severity failure;

        wait until falling_edge(clock);
        samples_in(0).data_v <= '1';
        samples_in(1).data_v <= '1';
        wait until falling_edge(clock);
        timestamp <= to_unsigned(100, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '0' report "ARM did not fence old epoch" severity failure;

        wait until falling_edge(clock);
        complete <= '1';
        timestamp <= to_unsigned(200, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '0' report "completion edge admitted pre-boundary sample" severity failure;

        wait until falling_edge(clock);
        complete <= '0';
        samples_in(0).data_v <= '0';
        samples_in(1).data_v <= '0';
        timestamp <= to_unsigned(201, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '0' report "epoch opened without a valid RX sample" severity failure;
        assert start_event = '0' report "epoch start fired without a valid RX sample" severity failure;
        assert first_valid_timestamp = to_unsigned(0, 64)
            report "timestamp latched before first valid RX sample" severity failure;

        -- Delay the ADC valid edge after RFIC completion. The gate must
        -- stay fenced until that edge, then admit and timestamp exactly it.
        wait until falling_edge(clock);
        timestamp <= to_unsigned(202, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '0' report "epoch opened during empty RX cycles" severity failure;
        assert state /= "0000" report "ACTIVE published before first valid sample" severity failure;

        wait until falling_edge(clock);
        timestamp <= to_unsigned(203, 64);
        samples_in(0).data_v <= '1';
        samples_in(1).data_v <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' report "first valid post-completion sample not admitted" severity failure;
        assert start_event = '1' report "epoch start event missing" severity failure;
        assert first_valid_timestamp = to_unsigned(203, 64)
            report "first-valid timestamp does not match first admitted sample" severity failure;
        assert epoch_id = x"2A" report "epoch ID mismatch" severity failure;

        -- A failure after RFIC completion but before the first ADC sample
        -- must also remain fenced and must not publish a valid epoch.
        wait until falling_edge(clock);
        wait until rising_edge(clock); -- retire ACTIVE_NEW status cycle
        wait until falling_edge(clock);
        samples_in(0).data_v <= '0';
        samples_in(1).data_v <= '0';
        epoch_id_in <= x"2B";
        arm <= '1';
        wait until falling_edge(clock);
        arm <= '0';
        wait until falling_edge(clock);
        complete <= '1';
        wait until falling_edge(clock);
        complete <= '0';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0010" and out_controls(0).enable = '0'
            report "completion did not remain fenced while awaiting ADC sample" severity failure;
        wait until falling_edge(clock);
        abort <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '0' and start_event = '0'
            report "ABORT after COMPLETE admitted invalid data" severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0100" and discard_active = '1' and
               out_controls(0).enable = '0' and start_event = '0'
            report "ABORT after COMPLETE exposed an unvalidated epoch" severity failure;

        report "rx_epoch_gate_tb: PASS" severity note;
        stop;
        wait;
    end process;
end architecture;
