-- tx_dac_output against the stage it replaced: tx_retune_gate's
-- combinational output (zero while muted, data_v forced) registered and held
-- while no sample is valid, as bladerf_core did. The gate is driven through
-- a scheduled mute exactly at its target, an ABORT while ARMED, an underflow
-- into ERROR and recovery, with random IQ and random valid gaps; the two DAC
-- values must agree on every edge.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;
    use std.env.all;

library work;
    use work.fifo_readwrite_p.all;

entity tx_dac_output_tb is
end entity;

architecture test of tx_dac_output_tb is
    signal clock : std_logic := '0';
    signal reset : std_logic := '1';
    signal timestamp : unsigned(63 downto 0) := (others => '0');
    signal arm, immediate, complete, abort, underflow : std_logic := '0';
    signal target_timestamp : unsigned(63 downto 0) := (others => '0');
    signal samples_in : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal gated : sample_streams_t(0 to 1);
    signal old_dac : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal new_dac : sample_streams_t(0 to 1);
    signal state : unsigned(1 downto 0);
    signal muted : std_logic;
    signal mute_cycles, checks : natural := 0;
    signal done : boolean := false;
begin
    clock <= not clock after 5 ns when not done;

    gate : entity work.tx_retune_gate
        generic map (NUM_STREAMS => 2)
        port map (
            clock => clock, reset => reset, timestamp => timestamp,
            arm => arm, arm_immediate => immediate,
            target_timestamp => target_timestamp, complete => complete,
            abort => abort, underflow => underflow,
            in_samples => samples_in, out_samples => gated,
            state => state, muted => muted,
            mute_start_event => open, resume_event => open,
            mute_start_timestamp => open, muted_sample_count => open);

    -- The replaced stage: bladerf_core's register on the gate output.
    old_stage : process(clock)
    begin
        if rising_edge(clock) then
            for i in 0 to 1 loop
                if gated(i).data_v = '1' then
                    old_dac(i).data_i <= gated(i).data_i;
                    old_dac(i).data_q <= gated(i).data_q;
                end if;
            end loop;
        end if;
    end process;

    dut : entity work.tx_dac_output
        generic map (NUM_STREAMS => 2)
        port map (clock => clock, in_samples => samples_in, muted => muted,
                  out_samples => new_dac);

    check : process(clock)
    begin
        if falling_edge(clock) and reset = '0' then
            for i in 0 to 1 loop
                assert new_dac(i).data_i = old_dac(i).data_i and
                       new_dac(i).data_q = old_dac(i).data_q
                    report "DAC lane " & integer'image(i) & " differs: old "
                         & integer'image(to_integer(old_dac(i).data_i)) & " new "
                         & integer'image(to_integer(new_dac(i).data_i))
                    severity failure;
            end loop;
            checks <= checks + 1;
            if muted = '1' then
                mute_cycles <= mute_cycles + 1;
            end if;
        end if;
    end process;

    -- Random IQ, random valid gaps, timestamp advancing on some cycles.
    source : process(clock)
        variable lfsr : std_logic_vector(31 downto 0) := x"1D872B41";
    begin
        if rising_edge(clock) then
            lfsr := lfsr(30 downto 0) &
                    (lfsr(31) xor lfsr(21) xor lfsr(1) xor lfsr(0));
            for i in 0 to 1 loop
                samples_in(i).data_i <= signed(lfsr(15 downto 0)) + i;
                samples_in(i).data_q <= signed(lfsr(31 downto 16)) - i;
                samples_in(i).data_v <= lfsr(3 + i) or lfsr(9);
            end loop;
            if lfsr(5) = '1' or lfsr(6) = '1' then
                timestamp <= timestamp + 1;
            end if;
        end if;
    end process;

    stimulus : process
        procedure cycles(n : natural) is
        begin
            for k in 1 to n loop
                wait until rising_edge(clock);
            end loop;
            wait until falling_edge(clock);
        end procedure;

        procedure pulse(signal s : out std_logic) is
        begin
            s <= '1';
            cycles(1);
            s <= '0';
        end procedure;

        -- Schedule a mute at "ahead" ticks from now with the controller's
        -- two-cycle target lead before ARM.
        procedure arm_ahead(ahead : natural) is
        begin
            target_timestamp <= timestamp + ahead;
            cycles(2);
            pulse(arm);
        end procedure;
    begin
        cycles(4);
        reset <= '0';
        cycles(20);
        for rep in 0 to 5 loop
            -- Scheduled mute reached exactly at its target, then complete.
            arm_ahead(15 + rep);
            cycles(60);
            pulse(complete);
            cycles(20);
            -- ABORT while ARMED, then recover.
            arm_ahead(200);
            cycles(5 + rep);
            pulse(abort);
            cycles(20);
            pulse(complete);
            cycles(15);
            -- Underflow into ERROR, then recover.
            pulse(underflow);
            cycles(25);
            pulse(complete);
            cycles(15);
            -- Immediate mute.
            immediate <= '1';
            arm_ahead(0);
            immediate <= '0';
            cycles(10 + rep);
            pulse(complete);
            cycles(30);
        end loop;
        assert mute_cycles > 100
            report "the gate barely muted: " & integer'image(mute_cycles)
            severity failure;
        report "tx_dac_output_tb: PASS (" & integer'image(checks)
             & " edges, " & integer'image(mute_cycles) & " muted)";
        done <= true;
        wait;
    end process;
end architecture;
