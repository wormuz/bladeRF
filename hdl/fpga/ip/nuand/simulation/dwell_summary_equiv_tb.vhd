-- Do two dwell_summary configurations produce the same records?
--
-- Pipelining the analyser is a timing fix, not a behaviour change, and the
-- way that goes wrong is subtle: a stage boundary moves WHEN a field is
-- produced, and if window index, first_window, trigger onset, sample_count,
-- peak/floor updates and the end-of-dwell clear do not all move together,
-- the module still produces plausible records that disagree with the
-- unpipelined one by a window or a sample. Case-by-case assertions on hand
-- computed values (dwell_summary_tb) would not catch that: they check a few
-- known answers, not that two implementations agree on every dwell.
--
-- So this drives two instances from one stimulus and compares every output
-- field on every summary_valid pulse. It is the test the architect asked
-- for before any pipelining is attempted: "старий і конвеєрний варіанти
-- дають ІДЕНТИЧНІ записи зупинки".
--
-- Right now both instances are the same entity with the same generics, so
-- this passes trivially -- that is deliberate. It establishes the harness
-- against a known-equal pair first, so that when the pipelined variant
-- arrives, a failure means the variant disagrees, not that the bench is
-- wrong. Point U_pipelined at the new entity when it exists.
--
-- The stimulus deliberately includes the cases where a stage boundary is
-- most likely to slip: a dwell ending exactly on a window boundary, a
-- dwell ending mid-window, a trigger that latches on the last window of a
-- dwell, and back-to-back dwells with no idle gap between them.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity dwell_summary_equiv_tb is
end entity;

architecture sim of dwell_summary_equiv_tb is

    constant WINDOW_LOG2 : natural := 4;    -- 16 samples per window
    constant WINDOW_LEN  : natural := 2 ** WINDOW_LOG2;
    constant CLIP_THR    : natural := 2044;
    constant CLK_PERIOD  : time    := 8 ns;

    signal clock       : std_logic := '0';
    signal reset       : std_logic := '1';
    signal sample      : sample_stream_t := (data_i => (others => '0'),
                                             data_q => (others => '0'),
                                             data_v => '0');
    signal dwell_start : std_logic := '0';
    signal threshold   : unsigned(47 downto 0) := (others => '0');
    signal ts_counter  : unsigned(63 downto 0) := (others => '0');
    signal done        : boolean := false;

    -- Reference instance.
    signal a_valid     : std_logic;
    signal a_energy    : unsigned(63 downto 0);
    signal a_peak      : unsigned(31 downto 0);
    signal a_clips     : unsigned(31 downto 0);
    signal a_samples   : unsigned(31 downto 0);
    signal a_trig      : std_logic;
    signal a_window    : unsigned(15 downto 0);
    signal a_ts        : unsigned(63 downto 0);
    signal a_mean      : unsigned(31 downto 0);
    signal a_floor     : unsigned(47 downto 0);
    signal a_peakwin   : unsigned(47 downto 0);

    -- Candidate instance (the pipelined variant, once it exists).
    signal b_valid     : std_logic;
    signal b_energy    : unsigned(63 downto 0);
    signal b_peak      : unsigned(31 downto 0);
    signal b_clips     : unsigned(31 downto 0);
    signal b_samples   : unsigned(31 downto 0);
    signal b_trig      : std_logic;
    signal b_window    : unsigned(15 downto 0);
    signal b_ts        : unsigned(63 downto 0);
    signal b_mean      : unsigned(31 downto 0);
    signal b_floor     : unsigned(47 downto 0);
    signal b_peakwin   : unsigned(47 downto 0);

    signal records_seen : natural := 0;
    signal mismatches   : natural := 0;

begin

    clock <= not clock after CLK_PERIOD/2 when not done else '0';

    ts_proc : process( clock )
    begin
        if rising_edge(clock) and reset = '0' then
            ts_counter <= ts_counter + 1;
        end if;
    end process;

    U_reference : entity work.dwell_summary
        generic map ( WINDOW_LOG2 => WINDOW_LOG2, CLIP_THRESHOLD => CLIP_THR )
        port map (
            clock => clock, reset => reset, sample => sample,
            dwell_start => dwell_start, timestamp => ts_counter,
            threshold => threshold,
            summary_valid => a_valid, energy_sum => a_energy, peak => a_peak,
            clip_count => a_clips, sample_count => a_samples,
            triggered => a_trig, first_window => a_window,
            first_timestamp => a_ts, mean_power => a_mean,
            noise_floor => a_floor, peak_window => a_peakwin
        );

    -- Swap this entity for the pipelined one when it lands. Everything else
    -- in this bench stays as it is.
    U_pipelined : entity work.dwell_summary
        generic map ( WINDOW_LOG2 => WINDOW_LOG2, CLIP_THRESHOLD => CLIP_THR )
        port map (
            clock => clock, reset => reset, sample => sample,
            dwell_start => dwell_start, timestamp => ts_counter,
            threshold => threshold,
            summary_valid => b_valid, energy_sum => b_energy, peak => b_peak,
            clip_count => b_clips, sample_count => b_samples,
            triggered => b_trig, first_window => b_window,
            first_timestamp => b_ts, mean_power => b_mean,
            noise_floor => b_floor, peak_window => b_peakwin
        );

    -- Compare on every record. A pipelined variant may legitimately publish
    -- its record a fixed number of cycles later than the reference; if that
    -- happens this process reports it as a valid-timing mismatch, which is
    -- the right thing to see -- the record contents must match AND the
    -- publication must stay aligned with dwell_start, because the host
    -- reads the record against the retune sequence that generated it.
    compare : process( clock )
    begin
        if rising_edge(clock) then
            if a_valid /= b_valid then
                mismatches <= mismatches + 1;
                report "MISMATCH summary_valid: reference=" & std_logic'image(a_valid)
                     & " pipelined=" & std_logic'image(b_valid)
                     & " at ts=" & integer'image(to_integer(ts_counter(31 downto 0)))
                    severity error;
            elsif a_valid = '1' then
                records_seen <= records_seen + 1;
                if a_energy /= b_energy then
                    mismatches <= mismatches + 1;
                    report "MISMATCH energy_sum: " & integer'image(to_integer(a_energy(31 downto 0)))
                         & " vs " & integer'image(to_integer(b_energy(31 downto 0))) severity error;
                end if;
                if a_peak /= b_peak then
                    mismatches <= mismatches + 1;
                    report "MISMATCH peak: " & integer'image(to_integer(a_peak))
                         & " vs " & integer'image(to_integer(b_peak)) severity error;
                end if;
                if a_clips /= b_clips then
                    mismatches <= mismatches + 1;
                    report "MISMATCH clip_count: " & integer'image(to_integer(a_clips))
                         & " vs " & integer'image(to_integer(b_clips)) severity error;
                end if;
                if a_samples /= b_samples then
                    mismatches <= mismatches + 1;
                    report "MISMATCH sample_count: " & integer'image(to_integer(a_samples))
                         & " vs " & integer'image(to_integer(b_samples)) severity error;
                end if;
                if a_trig /= b_trig then
                    mismatches <= mismatches + 1;
                    report "MISMATCH triggered: " & std_logic'image(a_trig)
                         & " vs " & std_logic'image(b_trig) severity error;
                end if;
                if a_window /= b_window then
                    mismatches <= mismatches + 1;
                    report "MISMATCH first_window: " & integer'image(to_integer(a_window))
                         & " vs " & integer'image(to_integer(b_window)) severity error;
                end if;
                if a_ts /= b_ts then
                    mismatches <= mismatches + 1;
                    report "MISMATCH first_timestamp: " & integer'image(to_integer(a_ts(31 downto 0)))
                         & " vs " & integer'image(to_integer(b_ts(31 downto 0))) severity error;
                end if;
                if a_mean /= b_mean then
                    mismatches <= mismatches + 1;
                    report "MISMATCH mean_power: " & integer'image(to_integer(a_mean))
                         & " vs " & integer'image(to_integer(b_mean)) severity error;
                end if;
                if a_floor /= b_floor then
                    mismatches <= mismatches + 1;
                    report "MISMATCH noise_floor: " & integer'image(to_integer(a_floor(31 downto 0)))
                         & " vs " & integer'image(to_integer(b_floor(31 downto 0))) severity error;
                end if;
                if a_peakwin /= b_peakwin then
                    mismatches <= mismatches + 1;
                    report "MISMATCH peak_window: " & integer'image(to_integer(a_peakwin(31 downto 0)))
                         & " vs " & integer'image(to_integer(b_peakwin(31 downto 0))) severity error;
                end if;
            end if;
        end if;
    end process;

    stim : process

        procedure feed( n : natural; i_val : integer; q_val : integer ) is
        begin
            for k in 1 to n loop
                wait until rising_edge(clock);
                sample.data_i <= to_signed(i_val, 16);
                sample.data_q <= to_signed(q_val, 16);
                sample.data_v <= '1';
            end loop;
        end procedure;

        procedure idle( n : natural ) is
        begin
            for k in 1 to n loop
                wait until rising_edge(clock);
                sample.data_v <= '0';
            end loop;
        end procedure;

        procedure boundary is
        begin
            wait until rising_edge(clock);
            sample.data_v <= '0';
            dwell_start   <= '1';
            wait until rising_edge(clock);
            dwell_start   <= '0';
        end procedure;

    begin
        reset <= '1';
        wait for 10*CLK_PERIOD;
        wait until rising_edge(clock);
        reset <= '0';
        -- Past the 8-cycle settle the link blocks use; harmless here, kept
        -- so the two benches start from the same shape.
        idle(10);

        threshold <= to_unsigned(20000, 48);

        -- 1. Dwell ending exactly on a window boundary.
        feed(WINDOW_LEN * 3, 100, 100);
        boundary;

        -- 2. Dwell ending mid-window: the partial window must be handled
        --    identically by both, including whether it counts at all.
        feed(WINDOW_LEN * 2 + 7, 100, 100);
        boundary;

        -- 3. Trigger latching on the last window of a dwell -- the case
        --    where a stage boundary is most likely to publish the trigger
        --    into the wrong record.
        feed(WINDOW_LEN, 50, 50);
        feed(WINDOW_LEN, 900, 900);
        feed(WINDOW_LEN, 900, 900);
        boundary;

        -- 4. Back-to-back dwells with no idle samples between them.
        feed(WINDOW_LEN, 300, 300);
        boundary;
        feed(WINDOW_LEN, 400, 400);
        boundary;

        -- 5. A quiet dwell, which must still publish a record.
        idle(WINDOW_LEN * 2);
        boundary;

        -- 6. Clipping at the rail, both components.
        feed(WINDOW_LEN, 2047, -2048);
        boundary;

        -- 7. Amplitude between plausible clip thresholds. Case 6 clips under
        --    any sane threshold and so cannot tell two thresholds apart;
        --    this one is above 1500 and below 2044, so a candidate that
        --    judges clipping differently from the reference shows up here
        --    rather than passing by luck. (Used to self-check this bench:
        --    with CLIP_THRESHOLD deliberately mismatched on the candidate,
        --    this is the case that reports the mismatch.)
        feed(WINDOW_LEN, 1800, 1800);
        boundary;

        idle(20);

        assert records_seen > 0
            report "no records were produced at all -- the bench drove nothing"
            severity failure;
        assert mismatches = 0
            report "the two instances disagreed on " & integer'image(mismatches)
                 & " field(s)"
            severity failure;

        report "dwell_summary_equiv_tb OK: " & integer'image(records_seen)
             & " records compared field by field, no mismatch";
        done <= true;
        wait;
    end process;

end architecture;
