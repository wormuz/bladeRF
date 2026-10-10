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
    signal arm, immediate, complete, abort : std_logic := '0';
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

        abort <= '1';
        wait until rising_edge(clock);
        wait for 1 ns;
        abort <= '0';
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
        -- Timestamp wraps from FFFF...FFFF to 1; target is due within three
        -- ticks under the same signed modular comparison as NIOS scheduling.
        assert state = "01" and muted = '0'
            report "TX gate recovery arm did not wait for wrapped target"
            severity failure;
        timestamp <= unsigned'(x"ffffffffffffffff");
        wait for 1 ns;
        assert muted = '0' report "TX gate muted before wrapped target"
            severity failure;
        timestamp <= to_unsigned(1, 64);
        wait for 1 ns;
        assert muted = '1' report "TX gate missed wrapped target"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert state = "10" report "TX gate did not latch wrapped boundary"
            severity failure;

        report "tx_retune_gate_tb: PASS" severity note;
        stop;
        wait;
    end process;
end architecture;
