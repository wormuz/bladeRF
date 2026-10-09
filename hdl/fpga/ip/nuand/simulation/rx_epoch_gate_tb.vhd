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
    signal out_timestamp : unsigned(63 downto 0);
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
            out_sample_controls => out_controls, out_samples => out_samples,
            out_timestamp => out_timestamp,
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
        samples_in(0).data_v <= '1';
        samples_in(1).data_v <= '1';
        samples_in(0).data_i <= to_signed(1234, 16);
        samples_in(0).data_q <= to_signed(-2345, 16);
        samples_in(1).data_i <= to_signed(3456, 16);
        samples_in(1).data_q <= to_signed(-4567, 16);
        epoch_id_in <= x"29";
        arm <= '1';
        wait until falling_edge(clock);
        arm <= '0';
        wait until falling_edge(clock);
        abort <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' and out_samples(0).data_v = '1' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(0).data_q = to_signed(0, 16)
            report "ABORT cycle forwarded IQ from the failed epoch" severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0100" and discard_active = '1' and
               out_controls(0).enable = '1' and out_samples(0).data_v = '1' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(0).data_q = to_signed(0, 16) and
               out_samples(1).data_i = to_signed(0, 16) and
               out_samples(1).data_q = to_signed(0, 16)
            report "ERROR did not preserve transport with zero-IQ keepalives" severity failure;
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
        assert state = "0001" and discard_active = '1' and
               out_controls(0).enable = '1' and out_samples(0).data_v = '1' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(0).data_q = to_signed(0, 16)
            report "PENDING did not fence IQ while keeping transport alive" severity failure;

        wait until falling_edge(clock);
        samples_in(0).data_v <= '1';
        samples_in(1).data_v <= '1';
        wait until falling_edge(clock);
        timestamp <= to_unsigned(100, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' and out_samples(0).data_v = '1' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(0).data_q = to_signed(0, 16)
            report "ARM did not replace old IQ with a keepalive" severity failure;

        wait until falling_edge(clock);
        complete <= '1';
        timestamp <= to_unsigned(200, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' and out_samples(0).data_v = '1' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(0).data_q = to_signed(0, 16)
            report "completion edge forwarded an old IQ sample" severity failure;

        wait until falling_edge(clock);
        complete <= '0';
        samples_in(0).data_v <= '0';
        samples_in(1).data_v <= '0';
        timestamp <= to_unsigned(201, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' and out_samples(0).data_v = '0' and
               out_samples(0).data_i = to_signed(0, 16)
            report "epoch forwarded IQ without a valid RX sample" severity failure;
        assert start_event = '0' report "epoch start fired without a valid RX sample" severity failure;
        assert first_valid_timestamp = to_unsigned(0, 64)
            report "timestamp latched before first valid RX sample" severity failure;

        -- Delay the ADC valid edge after RFIC completion. The gate must
        -- stay fenced until that edge, then admit and timestamp exactly it.
        wait until falling_edge(clock);
        timestamp <= to_unsigned(202, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' and out_samples(0).data_v = '0' and
               out_samples(0).data_i = to_signed(0, 16)
            report "epoch forwarded IQ during empty RX cycles" severity failure;
        assert state /= "0000" report "ACTIVE published before first valid sample" severity failure;

        -- RX_X2 must not certify the shared epoch from RX1's first sample
        -- while RX2 is still invalid. Both enabled lanes must be valid on
        -- the admitted boundary sample.
        wait until falling_edge(clock);
        timestamp <= to_unsigned(202, 64);
        samples_in(0).data_v <= '1';
        samples_in(1).data_v <= '0';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0010" and start_event = '0' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(1).data_i = to_signed(0, 16) and
               first_valid_timestamp = to_unsigned(0, 64)
            report "RX_X2 epoch opened before both enabled channels were valid"
            severity failure;

        wait until falling_edge(clock);
        timestamp <= to_unsigned(203, 64);
        samples_in(0).data_v <= '1';
        samples_in(1).data_v <= '1';
        samples_in(0).data_i <= to_signed(2303, 16);
        samples_in(1).data_i <= to_signed(3203, 16);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' report "first valid post-completion sample not admitted" severity failure;
        assert start_event = '1' report "epoch start event missing" severity failure;
        assert first_valid_timestamp = to_unsigned(203, 64)
            report "first-valid timestamp does not match first admitted sample" severity failure;
        assert out_timestamp = to_unsigned(203, 64) and
               out_samples(0).data_i = to_signed(2303, 16) and
               out_samples(1).data_i = to_signed(3203, 16)
            report "registered RX_X2 sample and timestamp lost alignment"
            severity failure;
        assert epoch_id = x"2A" report "epoch ID mismatch" severity failure;

        -- The start pulse is intentionally transient. The status path that
        -- NIOS/libbladeRF polls must retain the certificate after that pulse
        -- has gone away: matching epoch ID, ACTIVE state, and the exact
        -- first-valid timestamp remain observable until the next ARM.
        wait until rising_edge(clock);
        -- out_state is itself registered one cycle behind the internal FSM.
        wait until rising_edge(clock);
        wait for 1 ns;
        assert start_event = '0' and state = "0000" and discard_active = '0' and
               epoch_id = x"2A" and
               first_valid_timestamp = to_unsigned(203, 64)
            report "epoch certificate was not stable after start-event pulse"
            severity failure;

        -- Abort immediately after the first valid sample, while the internal
        -- FSM is in ACTIVE_NEW. This edge must keep both enabled lanes alive
        -- as zero-IQ keepalives rather than briefly disabling the stream.
        wait until falling_edge(clock);
        abort <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' and out_controls(1).enable = '1' and
               out_samples(0).data_v = '1' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(1).data_v = '1' and
               out_samples(1).data_i = to_signed(0, 16) and start_event = '0'
            report "ACTIVE_NEW ABORT stopped transport or exposed IQ"
            severity failure;
        wait until falling_edge(clock);
        abort <= '0';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0100" and out_controls(0).enable = '1' and
               out_controls(1).enable = '1' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(1).data_i = to_signed(0, 16)
            report "ACTIVE_NEW ABORT did not enter zero-IQ ERROR keepalives"
            severity failure;

        -- A failure after RFIC completion but before the first ADC sample
        -- must also remain fenced and must not publish a valid epoch.
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
        assert state = "0010" and out_controls(0).enable = '1' and
               out_samples(0).data_v = '0' and
               out_samples(0).data_i = to_signed(0, 16)
            report "completion did not stay IQ-fenced while awaiting ADC sample" severity failure;
        wait until falling_edge(clock);
        abort <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '1' and out_samples(0).data_v = '0' and
               start_event = '0'
            report "ABORT after COMPLETE admitted invalid data" severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0100" and discard_active = '1' and
               out_controls(0).enable = '1' and out_samples(0).data_v = '0' and
               out_samples(0).data_i = to_signed(0, 16) and
               out_samples(0).data_q = to_signed(0, 16) and start_event = '0'
            report "ABORT after COMPLETE exposed IQ or stopped transport" severity failure;

        -- RX_X1 operation must not wait for the disabled lane. The same
        -- enabled-lane rule supports either physical input selected alone.
        wait until falling_edge(clock);
        abort <= '0';
        controls_in(1).enable <= '0';
        samples_in(0).data_v <= '1';
        epoch_id_in <= x"2C";
        arm <= '1';
        wait until falling_edge(clock);
        arm <= '0';
        wait until falling_edge(clock);
        complete <= '1';
        wait until falling_edge(clock);
        complete <= '0';
        timestamp <= to_unsigned(300, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert start_event = '1' and
               first_valid_timestamp = to_unsigned(300, 64) and
               epoch_id = x"2C"
            report "RX_X1 epoch waited for a disabled RX lane" severity failure;

        -- RX2-only is the symmetric single-lane case and must establish its
        -- own boundary without waiting for the disabled RX1 lane.
        wait until rising_edge(clock); -- retire ACTIVE_NEW status cycle
        wait until rising_edge(clock); -- return to ACTIVE
        wait until falling_edge(clock);
        controls_in(0).enable <= '0';
        controls_in(1).enable <= '1';
        samples_in(0).data_v <= '0';
        samples_in(1).data_v <= '1';
        epoch_id_in <= x"2D";
        arm <= '1';
        wait until falling_edge(clock);
        arm <= '0';
        wait until falling_edge(clock);
        complete <= '1';
        wait until falling_edge(clock);
        complete <= '0';
        timestamp <= to_unsigned(400, 64);
        wait until rising_edge(clock);
        wait for 1 ns;
        assert start_event = '1'
            report "RX2-only epoch did not open on RX2's valid sample" severity failure;
        assert first_valid_timestamp = to_unsigned(400, 64)
            report "RX2-only epoch latched the wrong timestamp" severity failure;
        assert epoch_id = x"2D"
            report "RX2-only epoch latched the wrong epoch ID" severity failure;
        assert out_controls(0).enable = '0' and out_controls(1).enable = '1'
            report "RX2-only epoch did not preserve the selected lane controls"
            severity failure;

        -- Runtime invalidation can arrive while the epoch is already active.
        -- The gate must zero IQ immediately without pulsing either lane's
        -- stream enable low, which could terminate continuous USB RX.
        wait until rising_edge(clock); -- retire ACTIVE_NEW status cycle
        wait until rising_edge(clock); -- enter ACTIVE
        wait until falling_edge(clock);
        samples_in(1).data_i <= to_signed(4321, 16);
        samples_in(1).data_q <= to_signed(-1234, 16);
        abort <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert out_controls(0).enable = '0' and out_controls(1).enable = '1' and
               out_samples(1).data_v = '1' and
               out_samples(1).data_i = to_signed(0, 16) and
               out_samples(1).data_q = to_signed(0, 16)
            report "active-epoch ABORT stopped transport or exposed RX2 IQ"
            severity failure;
        wait until falling_edge(clock);
        abort <= '0';
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "0100" and out_controls(1).enable = '1' and
               out_samples(1).data_v = '1' and
               out_samples(1).data_i = to_signed(0, 16) and
               out_samples(1).data_q = to_signed(0, 16)
            report "ERROR state did not retain zero-IQ RX2 keepalives"
            severity failure;

        report "rx_epoch_gate_tb: PASS" severity note;
        stop;
        wait;
    end process;
end architecture;
