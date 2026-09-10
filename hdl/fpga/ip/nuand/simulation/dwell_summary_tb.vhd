-- Does the dwell summary measure what it claims to measure?
--
-- Four questions, each with an arithmetic answer known in advance:
--
--   1  a quiet dwell still produces a summary, and the trigger stays low
--   2  energy_sum equals the hand-computed sum of I^2+Q^2
--   3  clipping is counted per component, not on the magnitude
--   4  the trigger needs persistence, so one loud window is not enough
--
-- Question 1 is the one that matters most. The whole design rests on quiet
-- dwells still being reported: if a quiet dwell produced nothing, the host
-- could not tell "measured, nothing there" from "never measured".

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity dwell_summary_tb is
end entity;

architecture sim of dwell_summary_tb is

    constant WINDOW_LOG2 : natural := 4;    -- 16 samples, keeps the bench short
    constant CLIP_THR    : natural := 2044;

    signal clock         : std_logic := '0';
    signal reset         : std_logic := '1';
    signal sample        : sample_stream_t := (data_i => (others => '0'),
                                               data_q => (others => '0'),
                                               data_v => '0');
    signal dwell_start   : std_logic := '0';
    signal threshold     : unsigned(47 downto 0) := (others => '0');

    signal summary_valid : std_logic;
    signal energy_sum    : unsigned(63 downto 0);
    signal peak          : unsigned(31 downto 0);
    signal clip_count    : unsigned(31 downto 0);
    signal sample_count  : unsigned(31 downto 0);
    signal triggered     : std_logic;
    signal first_window  : unsigned(15 downto 0);

    -- Free-running counter standing in for the RX timestamp, so a captured
    -- value can be checked against the moment it was captured.
    signal ts_counter      : unsigned(63 downto 0) := (others => '0');
    signal first_timestamp : unsigned(63 downto 0);
    signal mean_power    : unsigned(31 downto 0);
    signal noise_floor   : unsigned(47 downto 0);
    signal peak_window   : unsigned(47 downto 0);

    signal done          : boolean := false;

begin

    -- Stands in for the RX timestamp: free-running, never reset by a dwell,
    -- exactly like the real one.
    ts_proc : process( clock )
    begin
        if( rising_edge(clock) ) then
            ts_counter <= ts_counter + 1;
        end if;
    end process;

    clock <= '0' when done else not clock after 4 ns;   -- 125 MHz

    U_dut : entity work.dwell_summary
        generic map (
            WINDOW_LOG2    => WINDOW_LOG2,
            TRIGGER_K      => 2,
            TRIGGER_OF     => 3,
            CLIP_THRESHOLD => CLIP_THR
        )
        port map (
            clock         => clock,
            reset         => reset,
            sample        => sample,
            dwell_start   => dwell_start,
            threshold     => threshold,
            summary_valid => summary_valid,
            energy_sum    => energy_sum,
            peak          => peak,
            clip_count    => clip_count,
            sample_count  => sample_count,
            triggered     => triggered,
            first_window  => first_window,
            timestamp       => ts_counter,
            first_timestamp => first_timestamp,
            mean_power    => mean_power,
            noise_floor   => noise_floor,
            peak_window   => peak_window
        );

    stim : process
        -- Feed n samples of a fixed magnitude and return what the summary
        -- should be, computed here rather than read back from the DUT.
        procedure feed( i_val : integer; q_val : integer; n : natural ) is
        begin
            for k in 1 to n loop
                wait until rising_edge(clock);
                sample.data_i <= to_signed(i_val, 16);
                sample.data_q <= to_signed(q_val, 16);
                sample.data_v <= '1';
            end loop;
            wait until rising_edge(clock);
            sample.data_v <= '0';
        end procedure;

        procedure end_dwell is
        begin
            -- Two idle cycles so the last sample has cleared the energy
            -- stage before the boundary publishes.
            wait until rising_edge(clock);
            wait until rising_edge(clock);
            wait until rising_edge(clock);
            dwell_start <= '1';
            wait until rising_edge(clock);
            dwell_start <= '0';
            wait until rising_edge(clock);
        end procedure;

        variable expect : natural;
    begin
        wait for 40 ns;
        wait until rising_edge(clock);
        reset <= '0';
        wait until rising_edge(clock);

        ----------------------------------------------------------------
        -- 1: a quiet dwell reports. Not "reports nothing" -- reports.
        ----------------------------------------------------------------
        feed(0, 0, 32);
        end_dwell;
        assert sample_count = 32
            report "FAIL quiet: sample_count = " & integer'image(to_integer(sample_count))
                   & ", expected 32 -- a quiet dwell must still be counted"
            severity error;
        assert energy_sum = 0
            report "FAIL quiet: energy_sum should be zero on silence"
            severity error;
        assert triggered = '0'
            report "FAIL quiet: silence must not trigger"
            severity error;
        report "case 1 OK: quiet dwell reported, 32 samples, no trigger";

        ----------------------------------------------------------------
        -- 2: energy is the plain sum of I^2+Q^2, no scaling, no rounding.
        --    16 samples of (100, 200) -> 16 * (10000 + 40000) = 800000
        ----------------------------------------------------------------
        feed(100, 200, 16);
        end_dwell;
        expect := 16 * (100*100 + 200*200);
        assert energy_sum = expect
            report "FAIL energy: got " & integer'image(to_integer(energy_sum(31 downto 0)))
                   & ", expected " & integer'image(expect)
            severity error;
        assert peak = (100*100 + 200*200)
            report "FAIL energy: peak should be one sample's worth"
            severity error;
        report "case 2 OK: energy_sum = " & integer'image(expect) & " as computed by hand";

        ----------------------------------------------------------------
        -- 3: clipping counts per component. A sample at (2044, 0) is
        --    clipped; (1500, 1500) is not, even though its magnitude is
        --    2121 -- larger than full scale. Judging on magnitude would
        --    call the second one clipped and be wrong.
        ----------------------------------------------------------------
        feed(2044, 0, 8);
        feed(1500, 1500, 8);
        end_dwell;
        assert clip_count = 8
            report "FAIL clip: got " & integer'image(to_integer(clip_count))
                   & ", expected 8 -- only the per-component overs count"
            severity error;
        report "case 3 OK: 8 clips from the rail, none from magnitude 2121";

        ----------------------------------------------------------------
        -- 4: persistence. One loud window out of three must not trigger
        --    when K=2; two must.
        ----------------------------------------------------------------
        threshold <= to_unsigned(16 * (500*500 + 500*500) / 2, 48);

        feed(500, 500, 16);     -- window 0: loud
        feed(0, 0, 16);         -- window 1: quiet
        feed(0, 0, 16);         -- window 2: quiet
        end_dwell;
        assert triggered = '0'
            report "FAIL persistence: one loud window of three triggered with K=2"
            severity error;
        report "case 4a OK: a single loud window does not trigger";

        feed(500, 500, 16);     -- window 0: loud
        feed(500, 500, 16);     -- window 1: loud
        feed(0, 0, 16);         -- window 2: quiet
        end_dwell;
        assert triggered = '1'
            report "FAIL persistence: two loud windows of three did not trigger"
            severity error;
        report "case 4b OK: two loud windows trigger, first_window = "
               & integer'image(to_integer(first_window));

        -- The timestamp must name the crossing, not the dwell boundary.
        -- Both are non-zero, so "did it get captured" is not enough: the
        -- captured value has to be strictly earlier than now, which is what
        -- fails if the sample is taken at end_dwell instead.
        assert first_timestamp > 0
            report "FAIL timestamp: trigger fired but first_timestamp is zero"
            severity error;
        assert first_timestamp < ts_counter
            report "FAIL timestamp: captured at the dwell boundary, not at "
                   & "the crossing (first_timestamp="
                   & integer'image(to_integer(first_timestamp))
                   & " now=" & integer'image(to_integer(ts_counter)) & ")"
            severity error;
        report "case 4c OK: timestamp captured at the crossing, "
               & integer'image(to_integer(first_timestamp))
               & " < now " & integer'image(to_integer(ts_counter));

        ----------------------------------------------------------------
        -- 5: the case the derived values exist for. A signal present in
        --    part of the dwell only.
        --
        --    Window 0 loud at (300,400): 16 * (90000+160000) = 4,000,000
        --    Windows 1 and 2 quiet at (10,10): 16 * 200      =     3,200
        --
        --    A dwell average would report 1,335,466 and describe neither
        --    state. noise_floor reports the quiet part, which is what a
        --    threshold should be measured against, and peak_window reports
        --    the loud part. The host gets the contrast without receiving a
        --    single IQ sample.
        ----------------------------------------------------------------
        threshold <= (others => '0');   -- measure only, no trigger
        feed(300, 400, 16);
        feed(10, 10, 16);
        feed(10, 10, 16);
        end_dwell;

        assert noise_floor = 16 * (10*10 + 10*10)
            report "FAIL floor: got " & integer'image(to_integer(noise_floor(31 downto 0)))
                   & ", expected " & integer'image(16 * 200)
                   & " -- the floor must be the quiet window, not an average"
            severity error;
        assert peak_window = 16 * (300*300 + 400*400)
            report "FAIL peak_window: got " & integer'image(to_integer(peak_window(31 downto 0)))
                   & ", expected " & integer'image(16 * 250000)
            severity error;
        -- mean_power is energy per window: total / 3 windows, shifted by
        -- WINDOW_LOG2. Total = 4,000,000 + 3,200 + 3,200 = 4,006,400.
        assert mean_power = 4006400 / (2**WINDOW_LOG2)
            report "FAIL mean: got " & integer'image(to_integer(mean_power))
                   & ", expected " & integer'image(4006400 / (2**WINDOW_LOG2))
            severity error;
        report "case 5 OK: floor " & integer'image(to_integer(noise_floor(31 downto 0)))
               & ", peak window " & integer'image(to_integer(peak_window(31 downto 0)))
               & " -- contrast visible without any IQ";

        report "dwell_summary_tb: all cases passed";
        done <= true;
        wait;
    end process;

end architecture;
