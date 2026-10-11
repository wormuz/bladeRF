library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;
    use std.env.all;

library work;
    use work.fifo_readwrite_p.all;

entity tx_retune_gate_tb is
end entity;

architecture test of tx_retune_gate_tb is
    signal clock : std_logic := '0';
    signal reset : std_logic := '1';
    signal timestamp : unsigned(63 downto 0) := (others => '0');
    signal arm, immediate, complete, abort, underflow_event : std_logic := '0';
    signal target_timestamp : unsigned(63 downto 0) := (others => '0');
    signal samples_in : sample_streams_t(0 to 1) := (others => (
        data_i => to_signed(1234, 16),
        data_q => to_signed(-567, 16),
        data_v => '1'));
    signal samples_out : sample_streams_t(0 to 1);
    signal state : unsigned(1 downto 0);
    signal muted, mute_start_event, resume_event : std_logic;
    signal mute_start_timestamp : unsigned(63 downto 0);
    signal muted_sample_count : unsigned(63 downto 0);
begin
    clock <= not clock after 5 ns;

    dut : entity work.tx_retune_gate
        generic map (NUM_STREAMS => 2)
        port map (
            clock => clock,
            reset => reset,
            timestamp => timestamp,
            arm => arm,
            arm_immediate => immediate,
            target_timestamp => target_timestamp,
            complete => complete,
            abort => abort,
            underflow => underflow_event,
            in_samples => samples_in,
            out_samples => samples_out,
            state => state,
            muted => muted,
            mute_start_event => mute_start_event,
            resume_event => resume_event,
            mute_start_timestamp => mute_start_timestamp,
            muted_sample_count => muted_sample_count);

    stimulus : process
    begin
        wait for 20 ns;
        wait until falling_edge(clock);
        reset <= '0';
        wait for 1 ns;
        assert muted = '0' and samples_out(0).data_i = to_signed(1234, 16)
            report "active TX gate changed IQ" severity failure;

        target_timestamp <= to_unsigned(10, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        assert state = "01" and muted = '0'
            report "future TX retune did not arm without muting early"
            severity failure;

        timestamp <= to_unsigned(9, 64);
        wait for 1 ns;
        assert muted = '0'
            report "TX gate muted before the scheduled boundary"
            severity failure;
        timestamp <= to_unsigned(10, 64);
        wait for 1 ns;
        assert muted = '1' and samples_out(0).data_i = to_signed(0, 16) and
               samples_out(1).data_q = to_signed(0, 16) and
               samples_out(0).data_v = '1'
            report "TX gate did not replace boundary IQ with valid zero-IQ"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" and mute_start_event = '1' and
               mute_start_timestamp = to_unsigned(10, 64)
            report "TX mute event did not capture the scheduled boundary"
            severity failure;
        assert muted_sample_count /= 0
            report "TX fence did not count consumed suppressed samples"
            severity failure;

        timestamp <= to_unsigned(12, 64);
        complete <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        complete <= '0';
        assert state = "00" and muted = '0' and resume_event = '1' and
               samples_out(0).data_i = to_signed(1234, 16)
            report "TX gate failed to resume after RFIC completion"
            severity failure;

        -- A queued replacement must never move an already armed mute
        -- boundary later. This exercises the compare that selects which
        -- timestamp is retained when ARM arrives in STATE_ARMED.
        timestamp <= to_unsigned(20, 64);
        target_timestamp <= to_unsigned(40, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        target_timestamp <= to_unsigned(50, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        timestamp <= to_unsigned(39, 64);
        wait for 1 ns;
        assert state = "01" and muted = '0'
            report "later replacement moved the queued boundary"
            severity failure;
        timestamp <= to_unsigned(40, 64);
        wait for 1 ns;
        assert muted = '1'
            report "gate failed to retain the earlier queued boundary"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" and mute_start_timestamp = to_unsigned(40, 64)
            report "retained earlier boundary was not recorded exactly"
            severity failure;
        complete <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        complete <= '0';
        assert state = "00" and muted = '0'
            report "TX gate failed to resume after later-target test"
            severity failure;

        -- Conversely, an earlier replacement must supersede the queued
        -- boundary and mute at the replacement timestamp.
        timestamp <= to_unsigned(50, 64);
        target_timestamp <= to_unsigned(80, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        target_timestamp <= to_unsigned(65, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        -- A later queued request must not replace the already selected 65
        -- tick boundary when it is written into the other target bank.
        target_timestamp <= to_unsigned(70, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        timestamp <= to_unsigned(64, 64);
        wait for 1 ns;
        assert state = "01" and muted = '0'
            report "earlier replacement muted before its boundary"
            severity failure;
        timestamp <= to_unsigned(65, 64);
        wait for 1 ns;
        assert muted = '1'
            report "earlier replacement did not move the boundary earlier"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" and mute_start_timestamp = to_unsigned(65, 64)
            report "earlier replacement boundary was not recorded exactly"
            severity failure;
        complete <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        complete <= '0';
        assert state = "00" and muted = '0'
            report "TX gate failed to resume after earlier-target test"
            severity failure;

        -- After selecting one replacement bank, a still earlier request in
        -- the now-inactive bank must become active and retain its boundary.
        timestamp <= to_unsigned(50, 64);
        target_timestamp <= to_unsigned(80, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        target_timestamp <= to_unsigned(65, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        target_timestamp <= to_unsigned(60, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        timestamp <= to_unsigned(59, 64);
        wait for 1 ns;
        assert state = "01" and muted = '0'
            report "second earlier replacement muted before its boundary"
            severity failure;
        timestamp <= to_unsigned(60, 64);
        wait for 1 ns;
        assert muted = '1'
            report "second earlier replacement did not select its target bank"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" and mute_start_timestamp = to_unsigned(60, 64)
            report "second earlier replacement boundary was not exact"
            severity failure;
        complete <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        complete <= '0';
        assert state = "00" and muted = '0'
            report "TX gate failed to resume after repeated replacement test"
            severity failure;

        -- Modulo ordering must also choose the earlier boundary across the
        -- 64-bit wrap: FFFF...FFF0 precedes 0000...0010.
        timestamp <= unsigned'(x"ffffffffffffff00");
        target_timestamp <= to_unsigned(16, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        target_timestamp <= unsigned'(x"fffffffffffffff0");
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        timestamp <= unsigned'(x"ffffffffffffffef");
        wait for 1 ns;
        assert state = "01" and muted = '0'
            report "wraparound replacement muted before the earlier target"
            severity failure;
        timestamp <= unsigned'(x"fffffffffffffff0");
        wait for 1 ns;
        assert muted = '1'
            report "wraparound replacement failed to select earlier target"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" and
               mute_start_timestamp = unsigned'(x"fffffffffffffff0")
            report "wraparound replacement boundary was not exact"
            severity failure;
        complete <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        complete <= '0';
        assert state = "00" and muted = '0'
            report "TX gate failed to resume after wraparound test"
            severity failure;

        -- The TX integration ORs FIFO underflow into ABORT so DAC muting
        -- does not wait for the host's periodic status monitor.
        underflow_event <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        underflow_event <= '0';
        assert state = "11" and muted = '1' and
               samples_out(0).data_i = to_signed(0, 16)
            report "TX abort did not fail closed" severity failure;

        timestamp <= unsigned'(x"fffffffffffffffe");
        target_timestamp <= to_unsigned(1, 64);
        immediate <= '0';
        arm <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        arm <= '0';
        -- An ERROR-state recovery must remain muted immediately, even when
        -- the fresh ARM names a future timestamp. Otherwise stale queued IQ
        -- leaks before RFIC recovery completes.
        assert state = "10" and muted = '1' and
               samples_out(0).data_i = to_signed(0, 16)
            report "TX gate recovery ARM reopened output before completion"
            severity failure;
        timestamp <= unsigned'(x"ffffffffffffffff");
        wait for 1 ns;
        assert muted = '1' report "TX gate unmuted during recovery"
            severity failure;
        timestamp <= to_unsigned(1, 64);
        wait for 1 ns;
        complete <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        complete <= '0';
        assert state = "00" and muted = '0' and resume_event = '1' and
               samples_out(0).data_i = to_signed(1234, 16)
            report "TX gate did not resume after recovery completion"
            severity failure;

        -- The split comparator must preserve the exact modulo boundary
        -- across a low-half carry, without muting one tick early.
        timestamp <= unsigned'(x"00000000ffffffff");
        target_timestamp <= unsigned'(x"0000000100000000");
        immediate <= '0';
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        assert state = "01" and muted = '0'
            report "split comparator released one tick before low-half carry"
            severity failure;
        timestamp <= unsigned'(x"0000000100000000");
        wait for 1 ns;
        assert muted = '1'
            report "split comparator missed exact low-half carry boundary"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" and mute_start_event = '1' and
               mute_start_timestamp = unsigned'(x"0000000100000000")
            report "split comparator changed the scheduled mute timestamp"
            severity failure;

        complete <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        complete <= '0';
        assert state = "00" and muted = '0'
            report "TX gate failed to re-arm after carry-boundary test"
            severity failure;
        timestamp <= unsigned'(x"0000ffffffffffff");
        target_timestamp <= unsigned'(x"0001000000000000");
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        assert state = "01" and muted = '0'
            report "split comparator muted early across 48-bit carry"
            severity failure;
        timestamp <= unsigned'(x"0001000000000000");
        wait for 1 ns;
        assert muted = '1'
            report "split comparator missed 48-bit carry boundary"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" and
               mute_start_timestamp = unsigned'(x"0001000000000000")
            report "48-bit carry changed the TX mute timestamp"
            severity failure;
        complete <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        complete <= '0';
        assert state = "00" and muted = '0'
            report "TX gate failed to re-arm after 48-bit carry test"
            severity failure;
        timestamp <= unsigned'(x"fffffffffffffffe");
        target_timestamp <= to_unsigned(1, 64);
        arm <= '1';
        wait until rising_edge(clock);
        wait until falling_edge(clock);
        arm <= '0';
        assert state = "01" and muted = '0'
            report "split comparator muted early across 64-bit wrap"
            severity failure;
        timestamp <= to_unsigned(1, 64);
        wait for 1 ns;
        assert muted = '1'
            report "split comparator missed target across 64-bit wrap"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" and mute_start_timestamp = to_unsigned(1, 64)
            report "64-bit wrap changed the TX mute timestamp"
            severity failure;

        report "tx_retune_gate_tb: PASS" severity note;
        stop;
        wait;
    end process;
end architecture;
