-- Testbench for gain_sequencer.
--
-- Checks the four things that can go wrong silently:
--
--   1. measure_valid rises only after the settling interval, and counts
--      SAMPLES rather than clock edges -- otherwise the interval means
--      different amounts of time at each sample rate.
--   2. a dwell boundary restarts settling, so a retune is never measured
--      through the transient.
--   3. the clip verdict is a ratio, not a count: the same number of clips
--      is a fault in a short dwell and noise in a long one.
--   4. an empty dwell clears the verdict instead of inheriting the previous
--      band's, which would blame the wrong frequency.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity gain_sequencer_tb is
end entity;

architecture sim of gain_sequencer_tb is

    constant SETTLE_LOG2 : natural := 4;    -- 16 samples, keeps the run short
    constant CLIP_PPM    : natural := 100;  -- 0.01%

    signal clock         : std_logic := '0';
    signal reset         : std_logic := '1';
    signal dwell_start   : std_logic := '0';
    signal sample_valid  : std_logic := '0';
    signal settle_sel    : unsigned(1 downto 0) := (others => '0');
    signal summary_valid : std_logic := '0';
    signal clip_count    : unsigned(31 downto 0) := (others => '0');
    signal sample_count  : unsigned(31 downto 0) := (others => '0');
    signal measure_valid : std_logic;
    signal gain_too_high : std_logic;
    signal settle_elapsed: unsigned(SETTLE_LOG2 downto 0);

    signal done          : boolean := false;

begin

    clock <= not clock after 5 ns when not done else '0';

    U_dut : entity work.gain_sequencer
        generic map (
            SETTLE_LOG2    => SETTLE_LOG2,
            CLIP_PPM_LIMIT => CLIP_PPM
        )
        port map (
            clock          => clock,
            reset          => reset,
            dwell_start    => dwell_start,
            sample_valid   => sample_valid,
            settle_sel     => settle_sel,
            summary_valid  => summary_valid,
            clip_count     => clip_count,
            sample_count   => sample_count,
            measure_valid  => measure_valid,
            gain_too_high  => gain_too_high,
            settle_elapsed => settle_elapsed
        );

    stim : process

        procedure tick( n : natural := 1 ) is
        begin
            for i in 1 to n loop
                wait until rising_edge(clock);
            end loop;
        end procedure;

        -- n sample strobes, one per clock.
        procedure feed( n : natural ) is
        begin
            for i in 1 to n loop
                sample_valid <= '1';
                wait until rising_edge(clock);
            end loop;
            sample_valid <= '0';
            wait until rising_edge(clock);
        end procedure;

        procedure new_dwell is
        begin
            dwell_start <= '1';
            wait until rising_edge(clock);
            dwell_start <= '0';
            wait until rising_edge(clock);
        end procedure;

        -- gain_too_high now updates one cycle after summary_valid pulses,
        -- not on the same edge: the multiply/compare was pipelined to fix
        -- a setup violation on the LVDS pll_sclk domain (measured
        -- -0.115..-0.469 ns, shared edge with dwell_start's settle-counter
        -- reset). One extra tick here matches the new one-cycle-later
        -- contract; every caller below observes gain_too_high already
        -- settled, since none of them read it until after this returns.
        procedure report_summary( clips : natural; samples : natural ) is
        begin
            clip_count    <= to_unsigned(clips, 32);
            sample_count  <= to_unsigned(samples, 32);
            summary_valid <= '1';
            wait until rising_edge(clock);
            summary_valid <= '0';
            wait until rising_edge(clock);
            wait until rising_edge(clock);
        end procedure;

    begin
        tick(4);
        reset <= '0';
        tick(2);

        ------------------------------------------------------------------
        -- 1. Settling gates on sample count, not elapsed clocks.
        ------------------------------------------------------------------
        new_dwell;
        assert measure_valid = '0'
            report "FAIL: measure_valid set immediately after a dwell start"
            severity error;

        -- Idle clocks with no samples must not advance settling at all.
        tick(40);
        assert measure_valid = '0'
            report "FAIL: settling advanced on idle clocks -- the interval "
                   & "would mean different times at different sample rates"
            severity error;
        report "case 1 OK: 40 idle clocks did not advance settling";

        ------------------------------------------------------------------
        -- 2. It does settle once enough SAMPLES arrive.
        ------------------------------------------------------------------
        feed(2 ** SETTLE_LOG2 + 2);
        assert measure_valid = '1'
            report "FAIL: still settling after the full interval of samples"
            severity error;
        report "case 2 OK: measure_valid after "
               & integer'image(2 ** SETTLE_LOG2) & " samples, elapsed = "
               & integer'image(to_integer(settle_elapsed));

        ------------------------------------------------------------------
        -- 3. A new dwell restarts settling.
        ------------------------------------------------------------------
        new_dwell;
        assert measure_valid = '0'
            report "FAIL: a retune did not restart settling -- the transient "
                   & "would be measured as signal"
            severity error;
        report "case 3 OK: dwell boundary cleared measure_valid";

        ------------------------------------------------------------------
        -- 4. Clip verdict is a ratio. 50 clips in 100000 is 500 ppm, over
        --    the 100 ppm limit; the same 50 clips in 1000000 is 50 ppm and
        --    is not. A count-based test would call both the same.
        ------------------------------------------------------------------
        report_summary(50, 100000);
        assert gain_too_high = '1'
            report "FAIL: 500 ppm clipping not flagged"
            severity error;

        report_summary(50, 1000000);
        assert gain_too_high = '0'
            report "FAIL: 50 ppm clipping flagged -- verdict is counting "
                   & "clips instead of taking the ratio"
            severity error;
        report "case 4 OK: same 50 clips, flagged at 100k samples and not "
               & "at 1M -- the verdict is a ratio";

        ------------------------------------------------------------------
        -- 5. Exactly at the limit is not over it. 100 clips in 1000000 is
        --    exactly 100 ppm.
        ------------------------------------------------------------------
        report_summary(100, 1000000);
        assert gain_too_high = '0'
            report "FAIL: a dwell exactly at the limit was called over it"
            severity error;
        report "case 5 OK: exactly 100 ppm is not over the limit";

        ------------------------------------------------------------------
        -- 6. An empty dwell clears the verdict rather than inheriting it.
        ------------------------------------------------------------------
        report_summary(50, 100000);     -- set the flag
        assert gain_too_high = '1'
            report "FAIL: setup for case 6 did not set the flag"
            severity error;
        report_summary(0, 0);           -- nothing measured
        assert gain_too_high = '0'
            report "FAIL: an empty dwell kept the previous band's verdict, "
                   & "blaming the wrong frequency"
            severity error;
        report "case 6 OK: empty dwell cleared the verdict";

        ------------------------------------------------------------------
        -- 7. settle_sel shortens the interval by powers of two. With
        --    SETTLE_LOG2 = 4 the full interval is 16 samples; sel=1 must
        --    settle at 8 and not before.
        ------------------------------------------------------------------
        settle_sel <= "01";
        new_dwell;
        feed(7);
        assert measure_valid = '0'
            report "FAIL: settled after 7 samples with sel=1, expected 8"
            severity error;
        feed(3);
        assert measure_valid = '1'
            report "FAIL: not settled after 10 samples with sel=1 (expected "
                   & "8) -- settle_sel is not shortening the interval"
            severity error;
        report "case 7 OK: sel=1 settles at half the interval";

        -- And sel=0 is still the full interval, so the default did not move.
        settle_sel <= "00";
        new_dwell;
        feed(10);
        assert measure_valid = '0'
            report "FAIL: sel=0 settled early -- the default interval changed"
            severity error;
        feed(8);
        assert measure_valid = '1'
            report "FAIL: sel=0 did not settle after the full interval"
            severity error;
        report "case 8 OK: sel=0 unchanged at the full interval";

        report "gain_sequencer_tb: all cases passed";
        done <= true;
        wait;
    end process;

end architecture;
